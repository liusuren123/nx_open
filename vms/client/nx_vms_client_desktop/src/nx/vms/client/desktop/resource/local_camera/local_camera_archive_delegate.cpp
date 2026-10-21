// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_archive_delegate.h"

#include <memory>
#include <mutex>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QStringList>
#include <QtCore/QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

#include <core/resource/audio_layout.h>
#include <core/resource/resource_media_layout.h>
#include <nx/media/audio_data_packet.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/video_data_packet.h>
#include <nx/utils/log/log.h>
#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/settings/local_settings.h>

#include "hls_vod_source.h"
#include "local_camera_source.h"
#include "stream_cache.h"

namespace nx::vms::client::desktop {

namespace {

/** Stream URL scheme; selects the protocol-specific FFmpeg options. */
enum class StreamScheme { rtsp, http, rtmp, udp, other };

/** How far before the end of a file a seek to the very end is backed off to. */
constexpr qint64 kEndSeekBackoffUs = 1'000'000;

/**
 * How many segments of a finite playlist are downloaded at the same time.
 *
 * Measured against a server that limits every connection on its own: one connection delivers about
 * 2.8 MB/s, four about 5.8 MB/s in total and eight about 6.8 MB/s. Past four the gain is small while
 * the connections multiply, and every source downloads what it needs on its own, so they are kept
 * few: a segment a demuxer waits for is then fetched by connections that do not compete with a dozen
 * others.
 *
 * The value comes from the local settings, so it can be changed while the client runs; it is applied
 * to sources created after the change.
 */
int prefetchConnections()
{
    return qBound(1, appContext()->localSettings()->streamPrefetchConnections(), 32);
}

StreamScheme schemeOf(const QString& url)
{
    const QString scheme = url.section("://", 0, 0).toLower();
    if (scheme == "rtsp" || scheme == "rtsps")
        return StreamScheme::rtsp;
    if (scheme == "http" || scheme == "https")
        return StreamScheme::http;
    if (scheme == "rtmp" || scheme == "rtmps" || scheme == "rtmpt")
        return StreamScheme::rtmp;
    if (scheme == "udp" || scheme == "rtp")
        return StreamScheme::udp;
    return StreamScheme::other;
}

bool isMulticastAddress(const QString& host)
{
    const QStringList octets = host.split('.');
    if (octets.size() != 4)
        return false;

    bool ok = false;
    const int firstOctet = octets.first().toInt(&ok);
    return ok && firstOctet >= 224 && firstOctet <= 239;
}

/**
 * Options a progressive HTTP source is opened with. The same ones are used to probe whether the
 * source is a file, so a live stream keeps the connection opened for the probe.
 */
QList<QPair<QString, QString>> httpOptions(const QString& url)
{
    const HttpSourceHeaders headers = httpSourceHeaders(url);

    QList<QPair<QString, QString>> options{
        {"rw_timeout", "8000000"},
        {"reconnect", "1"},
        {"reconnect_streamed", "1"},
        {"reconnect_delay_max", "4"},
        {"user_agent", headers.userAgent}};

    if (!headers.referer.isEmpty())
        options.append({"referer", headers.referer});

    return options;
}

/**
 * Whether the demuxer gets the media from the resource itself. A manifest-based demuxer (HLS, DASH,
 * ...) reads a playlist and fetches the media on its own, so caching its input would store the
 * playlist instead of the video, and a live playlist would stop being refreshed.
 */
bool readsResourceDirectly(const AVInputFormat* inputFormat)
{
    if (!inputFormat || !inputFormat->name)
        return false;

    static const QStringList manifestDemuxers{
        QStringLiteral("hls"),
        QStringLiteral("applehttp"),
        QStringLiteral("dash"),
        QStringLiteral("smoothstreaming")};

    for (const QString& name: QString::fromLatin1(inputFormat->name).split(','))
    {
        if (manifestDemuxers.contains(name))
            return false;
    }

    return true;
}

/**
 * Whether the demuxer fetches the media itself instead of reading it from the resource it was given
 * (HLS, DASH, ...). Such a demuxer seeks the stream rather than its input, so the input does not
 * have to support byte seeking - a playlist served by a server that ignores Range requests cannot
 * be seeked by bytes at all, and yet the media itself can be seeked and has a known duration.
 */
bool fetchesMediaItself(const AVInputFormat* inputFormat)
{
    if (!inputFormat)
        return false;

    // FFmpeg marks the demuxers that do not seek their input by bytes, and a manifest is one of them
    // by nature, whether or not it is marked as such.
    return (inputFormat->flags & AVFMT_NO_BYTE_SEEK) || !readsResourceDirectly(inputFormat);
}

/** Whether a URL is most likely an HLS playlist, without having to open it. */
bool looksLikeHlsPlaylist(const QString& url)
{
    return url.contains(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

} // namespace

struct LocalCameraArchiveDelegate::Private
{
    /** Capture device name; empty for URL sources. */
    const QString deviceId;

    /** Stream URL; empty for capture devices. */
    const QString url;

    /** Set when the source is expected to be a media file rather than a live stream. */
    const bool filePlaybackExpected = false;

    AVFormatContext* formatContext = nullptr;
    AVPacket* packet = nullptr;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;
    CodecParametersConstPtr codecParameters;
    CodecParametersConstPtr audioCodecParameters;
    AudioLayoutPtr audioLayout;

    /** Real-time pacing state: wall-clock time the stream (re)started and its first timestamp. */
    bool pacingStarted = false;
    qint64 firstPacketUs = 0;
    qint64 playbackStartUs = 0;

    /** Set when the source is a finite, seekable file: it has a duration and can be seeked. */
    bool filePlayback = false;

    /** File duration and the timestamp of its first frame, both in microseconds. */
    qint64 fileDurationUs = 0;
    qint64 fileBaseTimestampUs = 0;

    /**
     * Disk cache the file is read through. Null for a live stream and for a file whose cache is
     * unavailable, both of which read the resource directly.
     */
    std::shared_ptr<StreamCacheReader> cacheReader;

    /**
     * Finite HLS playlist whose segments are downloaded in parallel and served locally. Null for
     * every source that is read directly. The source itself outlives this handle.
     */
    std::shared_ptr<HlsVodSource> hlsSource;

    QString lastError;
};

LocalCameraArchiveDelegate::LocalCameraArchiveDelegate(const QString& deviceId, const QString& url,
    bool filePlaybackExpected):
    d(new Private{deviceId, url, filePlaybackExpected})
{
    setObjectName("LocalCameraArchiveDelegate");
}

LocalCameraArchiveDelegate::~LocalCameraArchiveDelegate()
{
    close();
}

QString LocalCameraArchiveDelegate::lastErrorMessage() const
{
    return d->lastError;
}

QString LocalCameraArchiveDelegate::sourceDescription() const
{
    return d->url.isEmpty() ? d->deviceId : localCameraSourceUrlForDisplay(d->url);
}

int LocalCameraArchiveDelegate::interruptCallback(void* opaque)
{
    const auto delegate = static_cast<LocalCameraArchiveDelegate*>(opaque);
    return delegate->m_needStop ? 1 : 0;
}

bool LocalCameraArchiveDelegate::openStream()
{
    // Registering the FFmpeg devices is not thread safe, and streams are opened by several playback
    // threads at once, so it is done once rather than from every one of them.
    static std::once_flag devicesRegistered;
    std::call_once(devicesRegistered, [] { avdevice_register_all(); });

    if (!d->url.isEmpty())
    {
        // A finite playlist that has already been played is served from its cached copy straight away:
        // probing the remote playlist again would make opening it wait for the network every time.
        const bool interfaceThread =
            QThread::currentThread() == QCoreApplication::instance()->thread();

        if (switchToPrefetchedPlaylist(/*cachedOnly*/ true))
        {
            if (!readStreamInfo())
                return false;
        }
        else if (interfaceThread && d->filePlaybackExpected && looksLikeHlsPlaylist(d->url))
        {
            // The interface must not wait for the network either, so the playlist is prepared in the
            // background and this call fails straight away. The framework retries, and the reader
            // thread - the one that plays - is where the source is really opened.
            if (StreamCache* const cache = appContext()->streamCache())
            {
                HlsVodSource::prepareInBackground(
                    d->url, cache->folder(), cache->limitBytes(), prefetchConnections());
            }

            d->lastError = tr("Preparing '%1' for playback.").arg(sourceDescription());
            return false;
        }
        else
        {
            if (!openUrl())
                return false;

            if (!readStreamInfo())
                return false;

            // A finite playlist is served from a local copy whose segments are downloaded in parallel:
            // a server that limits every connection on its own would otherwise keep the playlist below
            // real time, however fast the network is. A plain file is re-read through the disk cache
            // instead.
            //
            // A replacement that fails keeps the input it was opened with, so a failed attempt simply
            // leaves the source being read directly and the chain may safely try the next one.
            if (!d->hlsSource && switchToPrefetchedPlaylist())
            {
                if (!readStreamInfo())
                    return false;
            }
            else if (!d->cacheReader && switchToCachedInput())
            {
                if (!readStreamInfo())
                    return false;
            }
        }
    }
    else if (!openCaptureDevice())
    {
        return false;
    }

    d->videoStreamIndex = av_find_best_stream(
        d->formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->videoStreamIndex < 0)
    {
        d->lastError = tr("Source '%1' has no video stream.").arg(sourceDescription());
        return false;
    }

    const AVStream* stream = d->formatContext->streams[d->videoStreamIndex];
    const AVCodecID codecId = stream->codecpar->codec_id;

    // Raw pixel formats would have to be converted and re-encoded, which is not supported yet.
    const bool isRaw = codecId == AV_CODEC_ID_NONE || codecId == AV_CODEC_ID_RAWVIDEO;
    if (isRaw || !avcodec_find_decoder(codecId))
    {
        d->lastError = tr("Unsupported video format of source '%1' (codec id %2).")
            .arg(sourceDescription()).arg(static_cast<int>(codecId));
        return false;
    }

    d->codecParameters = CodecParametersConstPtr(new CodecParameters(stream->codecpar));
    d->packet = av_packet_alloc();

    // Audio is optional and opened on a best-effort basis, so a video-only source (e.g. an IP
    // camera without a microphone) keeps working.
    d->audioStreamIndex = av_find_best_stream(
        d->formatContext, AVMEDIA_TYPE_AUDIO, -1, d->videoStreamIndex, nullptr, 0);
    if (d->audioStreamIndex >= 0)
    {
        const AVStream* audioStream = d->formatContext->streams[d->audioStreamIndex];
        if (audioStream->codecpar->ch_layout.nb_channels < 1
            || !avcodec_find_decoder(audioStream->codecpar->codec_id))
        {
            d->audioStreamIndex = -1;
        }
        else
        {
            d->audioCodecParameters = CodecParametersConstPtr(
                new CodecParameters(audioStream->codecpar));
            d->audioLayout = std::make_shared<AudioLayout>();
            d->audioLayout->addTrack(d->audioCodecParameters,
                d->audioCodecParameters->getAudioCodecDescription());
        }
    }

    detectFilePlayback();

    NX_INFO(typeid(LocalCameraArchiveDelegate),
        "Opened local source '%1': %2x%3, %4 fps, audio: %5, %6",
        sourceDescription(),
        stream->codecpar->width,
        stream->codecpar->height,
        av_q2d(stream->avg_frame_rate),
        d->audioStreamIndex >= 0,
        d->filePlayback
            ? QString("file, %1 s%2").arg(d->fileDurationUs / 1'000'000.0, 0, 'f', 3)
                .arg(d->cacheReader ? ", cached" : "")
            : QString("live"));

    return true;
}

bool LocalCameraArchiveDelegate::readStreamInfo()
{
    if (!d->formatContext)
    {
        d->lastError = tr("Source '%1' is not open.").arg(sourceDescription());
        return false;
    }

    // Let av_read_frame() bail out once playback is stopped.
    d->formatContext->interrupt_callback.callback =
        &LocalCameraArchiveDelegate::interruptCallback;
    d->formatContext->interrupt_callback.opaque = this;

    if (avformat_find_stream_info(d->formatContext, nullptr) < 0)
    {
        d->lastError = tr("Failed to read stream information from '%1'.")
            .arg(sourceDescription());
        return false;
    }

    return true;
}

void LocalCameraArchiveDelegate::detectFilePlayback()
{
    d->filePlayback = false;
    d->fileDurationUs = 0;
    d->fileBaseTimestampUs = 0;

    // Only a source the user expects to be a file is checked. A live stream is never treated as a
    // file even if its container happens to report a duration.
    if (!d->filePlaybackExpected)
        return;

    // A file reports a finite duration; anything else keeps being live.
    const qint64 duration = d->formatContext->duration; //< In AV_TIME_BASE units (microseconds).

    // It has to be seekable as well, or the timeline would be useless. A demuxer that fetches the
    // media itself seeks the stream, while every other one seeks through its input, which then has
    // to support it.
    const bool ioSeekable = d->formatContext->pb
        && (d->formatContext->pb->seekable & AVIO_SEEKABLE_NORMAL);
    const AVInputFormat* const inputFormat = d->formatContext->iformat;
    const bool selfSeeking = fetchesMediaItself(inputFormat);
    if (duration <= 0 || (!ioSeekable && !selfSeeking))
    {
        NX_DEBUG(typeid(LocalCameraArchiveDelegate),
            "Source '%1' is played as a live stream: duration %2 us, seekable input %3, "
            "format '%4' fetches the media itself: %5",
            sourceDescription(), duration, ioSeekable,
            inputFormat && inputFormat->name ? inputFormat->name : "unknown", selfSeeking);
        return;
    }

    d->filePlayback = true;
    d->fileDurationUs = duration;

    // Report timestamps relative to the first frame, so the timeline starts at zero.
    const AVStream* videoStream = d->formatContext->streams[d->videoStreamIndex];
    if (videoStream && videoStream->start_time != AV_NOPTS_VALUE)
    {
        d->fileBaseTimestampUs =
            av_rescale_q(videoStream->start_time, videoStream->time_base, AV_TIME_BASE_Q);
    }
}

bool LocalCameraArchiveDelegate::openCaptureDevice()
{
    const AVInputFormat* inputFormat = av_find_input_format("dshow");
    if (!inputFormat)
    {
        d->lastError = tr("The FFmpeg 'dshow' input device is not available.");
        return false;
    }

    const QByteArray deviceName = ("video=" + d->deviceId).toUtf8();

    // Prefer the camera's compressed output: MJPEG / H.264 frames are forwarded without
    // re-encoding. A few devices accept neither and only expose raw formats, so a plain open is
    // tried last.
    const QStringList pixelFormats{"mjpeg", "h264", QString()};
    for (const QString& pixelFormat: pixelFormats)
    {
        AVDictionary* options = nullptr;
        if (!pixelFormat.isEmpty())
            av_dict_set(&options, "pixel_format", pixelFormat.toUtf8().constData(), 0);

        const int result = avformat_open_input(
            &d->formatContext, deviceName.constData(), inputFormat, &options);
        av_dict_free(&options);

        if (result >= 0)
            return true;

        d->formatContext = nullptr;
    }

    d->lastError = tr("Failed to open capture device '%1'.").arg(d->deviceId);
    return false;
}

QString LocalCameraArchiveDelegate::effectiveUrl() const
{
    // FFmpeg needs the '@' marker ("udp://@group:port") to receive a multicast stream; many IPTV
    // playlists omit it, so add it when the target is a multicast address.
    const QString prefix = "udp://";
    if (!d->url.startsWith(prefix, Qt::CaseInsensitive))
        return d->url;

    const QString rest = d->url.mid(prefix.size());
    if (rest.startsWith('@'))
        return d->url;

    const QString host = rest.section('/', 0, 0).section(':', 0, 0);
    if (!isMulticastAddress(host))
        return d->url;

    return prefix + '@' + rest;
}

bool LocalCameraArchiveDelegate::openUrlWithOptions(
    const QList<QPair<QString, QString>>& options, QString* errorOut)
{
    closeStream();

    d->formatContext = avformat_alloc_context();
    if (!d->formatContext)
    {
        *errorOut = tr("Failed to allocate an FFmpeg context.");
        return false;
    }

    d->formatContext->interrupt_callback.callback =
        &LocalCameraArchiveDelegate::interruptCallback;
    d->formatContext->interrupt_callback.opaque = this;

    AVDictionary* dictionary = nullptr;
    for (const auto& option: options)
    {
        av_dict_set(&dictionary, option.first.toUtf8().constData(),
            option.second.toUtf8().constData(), 0);
    }

    const int result = avformat_open_input(
        &d->formatContext, effectiveUrl().toUtf8().constData(), /*inputFormat*/ nullptr,
        &dictionary);
    av_dict_free(&dictionary);

    if (result >= 0 && d->formatContext)
        return true;

    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, buffer, sizeof(buffer));
    *errorOut = QString::fromUtf8(buffer);

    d->formatContext = nullptr;
    return false;
}

bool LocalCameraArchiveDelegate::openUrl()
{
    const StreamScheme scheme = schemeOf(d->url);
    QString lastError;

    if (scheme == StreamScheme::rtsp)
    {
        // Interleaved RTSP over TCP tolerates packet loss and works through NAT much better than
        // plain UDP, so it is tried first. A few servers (e.g. VLC's VOD mode) only support UDP,
        // hence the fallback.
        static const QStringList transports{"tcp", "udp"};
        for (const QString& transport: transports)
        {
            const QList<QPair<QString, QString>> options{
                {"rtsp_transport", transport},
                {"stimeout", "5000000"}, //< Socket timeout, in microseconds.
                {"max_delay", "500000"},
                {"fflags", "nobuffer"},
                {"flags", "low_delay"}};
            if (openUrlWithOptions(options, &lastError))
                return true;
        }
    }
    else
    {
        QList<QPair<QString, QString>> options;
        switch (scheme)
        {
            case StreamScheme::http:
                // Progressive HTTP, HLS (m3u8) and DASH are all auto-detected by FFmpeg.
                options = httpOptions(effectiveUrl());
                break;

            case StreamScheme::rtmp:
                options = {{"rw_timeout", "8000000"}};
                break;

            case StreamScheme::udp:
                // Multicast/unicast UDP: keep running on overruns and enlarge the buffers.
                options = {
                    {"overrun_nonfatal", "1"},
                    {"fifo_size", "65536"},
                    {"buffer_size", "2097152"}};
                break;

            default:
                options = {{"rw_timeout", "8000000"}};
                break;
        }

        if (openUrlWithOptions(options, &lastError))
            return true;
    }

    // A signed address that has run out is not going to work again, and saying so is more useful
    // than the 403 it produces.
    const qint64 deadline = streamUrlDeadlineSec(d->url);
    if (deadline > 0 && deadline < QDateTime::currentSecsSinceEpoch())
    {
        d->lastError = tr("The link to '%1' has expired (it was valid until %2). Open the page "
            "in the browser and scan it again.")
            .arg(localCameraSourceUrlForDisplay(d->url),
                QDateTime::fromSecsSinceEpoch(deadline).toString(
                    QStringLiteral("yyyy-MM-dd HH:mm")));
        return false;
    }

    d->lastError = tr("Failed to open stream '%1': %2.")
        .arg(localCameraSourceUrlForDisplay(d->url), lastError);
    return false;
}

bool LocalCameraArchiveDelegate::switchToPrefetchedPlaylist(bool cachedOnly)
{
    // Only an HLS playlist served over plain HTTP is prefetched; anything else is read directly.
    if (schemeOf(d->url) != StreamScheme::http)
        return false;

    if (cachedOnly && !d->filePlaybackExpected)
        return false;

    if (d->formatContext)
    {
        // The remote input has been opened already, so the demuxer tells what this source is.
        const AVInputFormat* const inputFormat = d->formatContext->iformat;
        if (!inputFormat || readsResourceDirectly(inputFormat))
            return false;

        const QString format = QString::fromLatin1(inputFormat->name ? inputFormat->name : "");
        if (!format.contains(QLatin1String("hls")) && !format.contains(QLatin1String("applehttp")))
            return false;
    }
    else if (!cachedOnly)
    {
        return false; //< Nothing has been opened yet, so nothing tells an HLS playlist here.
    }

    StreamCache* const cache = appContext()->streamCache();
    if (!cache)
        return false;

    if (cachedOnly)
    {
        // Nothing is fetched here, so the cached playlist is all that is needed.
        if (!HlsVodSource::hasCachedPlaylist(d->url, cache->folder()))
            return false;
    }
    else if (!cache->writable())
    {
        return false;
    }

    // The interface is never made to wait for the download: it is served locally only when what a
    // demuxer needs to open the playlist is already cached, and otherwise the media is prepared in
    // the background and the caller is told to come back. Whoever can afford to fetch it - the reader
    // thread, the one that plays - opens the source directly instead.
    if (QThread::currentThread() == QCoreApplication::instance()->thread()
        && !HlsVodSource::isProbeReady(d->url, cache->folder()))
    {
        NX_DEBUG(typeid(LocalCameraArchiveDelegate),
            "Not prefetching '%1' on the GUI thread: the beginning of the media is not cached yet.",
            sourceDescription());
        return false;
    }

    QString error;
    QElapsedTimer timer;
    timer.start();
    std::shared_ptr<HlsVodSource> source = HlsVodSource::acquire(
        d->url, cache->folder(), cache->limitBytes(), prefetchConnections(), &error);
    if (!source)
    {
        NX_DEBUG(typeid(LocalCameraArchiveDelegate), "Not prefetching '%1': %2.",
            sourceDescription(), error);
        return false;
    }

    const qint64 acquireMs = timer.restart();

    // The replacement is built before the input in use is touched: should the local playlist fail to
    // open, the source keeps being read through the input it was opened with instead of being left
    // with none. On failure avformat_open_input() releases the context itself, so nothing is left to
    // free here.
    AVFormatContext* formatContext = avformat_alloc_context();
    if (!formatContext)
    {
        d->lastError = tr("Failed to allocate an FFmpeg context.");
        return false;
    }

    formatContext->interrupt_callback.callback =
        &LocalCameraArchiveDelegate::interruptCallback;
    formatContext->interrupt_callback.opaque = this;

    // No read timeout here: a segment may still be downloading when the demuxer asks for it.
    AVDictionary* options = nullptr;
    av_dict_set(&options, "user_agent", "NxMetaClient", 0);
    const int result = avformat_open_input(&formatContext,
        source->url().toUtf8().constData(), /*inputFormat*/ nullptr, &options);
    av_dict_free(&options);

    if (result < 0)
    {
        d->lastError = tr("Failed to open the prefetched stream '%1'.").arg(sourceDescription());
        return false;
    }

    // Read the media from the local server instead of the original playlist.
    closeStream();
    d->hlsSource = source;
    d->formatContext = formatContext;

    NX_INFO(typeid(LocalCameraArchiveDelegate),
        "Prefetched '%1' in %2 ms (source %3 ms, open %4 ms); gui thread: %5.",
        sourceDescription(), acquireMs + timer.elapsed(), acquireMs, timer.elapsed(),
        QThread::currentThread() == QCoreApplication::instance()->thread());

    return true;
}

bool LocalCameraArchiveDelegate::switchToCachedInput()
{
    // Only a source the user expects to be a finite file, served over plain HTTP and already opened,
    // is worth caching. Anything else is read directly, as it was before the cache existed.
    if (!d->formatContext || !d->filePlaybackExpected || schemeOf(d->url) != StreamScheme::http)
        return false;

    const qint64 duration = d->formatContext->duration; //< In AV_TIME_BASE units (microseconds).
    const bool ioSeekable = d->formatContext->pb
        && (d->formatContext->pb->seekable & AVIO_SEEKABLE_NORMAL);
    const qint64 size = d->formatContext->pb ? avio_size(d->formatContext->pb) : -1;

    NX_DEBUG(typeid(LocalCameraArchiveDelegate),
        "Cache probe of '%1': duration %2 us, seekable %3, size %4 bytes, format '%5'",
        sourceDescription(), duration, ioSeekable, size,
        d->formatContext->iformat && d->formatContext->iformat->name
            ? d->formatContext->iformat->name
            : "unknown");

    // A file reports a finite duration and its I/O can seek. A live stream, and a container whose
    // input is not the media itself (a playlist), keeps being read directly.
    if (duration <= 0 || !ioSeekable || size <= 0
        || !readsResourceDirectly(d->formatContext->iformat))
    {
        return false;
    }

    StreamCache* const cache = appContext()->streamCache();
    const std::shared_ptr<StreamCacheReader> reader = cache ? cache->open(d->url, size) : nullptr;
    if (!reader)
        return false;

    // The replacement is built before the input in use is touched: should the cached copy fail to
    // open, the source keeps being read through the input it was opened with instead of being left
    // with none. The cache owns its input, so the format context must be told not to close it; the
    // cache is also what keeps the file seekable no matter how the connection behaves.
    AVFormatContext* formatContext = avformat_alloc_context();
    if (!formatContext)
    {
        d->lastError = tr("Failed to allocate an FFmpeg context.");
        reader->requestStop();
        return false;
    }

    formatContext->flags |= AVFMT_FLAG_CUSTOM_IO;
    formatContext->pb = reader->avioContext();
    formatContext->interrupt_callback.callback = &LocalCameraArchiveDelegate::interruptCallback;
    formatContext->interrupt_callback.opaque = this;

    // The URL is only a hint for the format probing here: the data itself comes from the cache.
    if (avformat_open_input(&formatContext, d->url.toUtf8().constData(),
            /*inputFormat*/ nullptr, /*options*/ nullptr) < 0)
    {
        d->lastError = tr("Failed to open the cached stream '%1'.").arg(sourceDescription());
        reader->requestStop();
        return false;
    }

    // Read the resource through the cache instead of the connection just used to probe it.
    closeStream();
    d->cacheReader = reader;
    d->formatContext = formatContext;

    return true;
}

void LocalCameraArchiveDelegate::closeStream()
{
    // A download in progress cannot be interrupted from the outside, so the reader is asked to give
    // up before it is released, which is what unblocks a reader thread stuck in a network read.
    if (d->cacheReader)
        d->cacheReader->requestStop();

    if (d->packet)
        av_packet_free(&d->packet);

    if (d->formatContext)
        avformat_close_input(&d->formatContext);

    d->formatContext = nullptr;
    d->videoStreamIndex = -1;
    d->audioStreamIndex = -1;
    d->codecParameters.reset();
    d->audioCodecParameters.reset();
    d->audioLayout.reset();

    if (d->hlsSource)
    {
        // Only the handle is released: the source keeps serving the playlist, so that the reader,
        // which closes and re-opens its delegate when it jumps, does not have to start over.
        d->hlsSource.reset();
    }

    d->pacingStarted = false;
    d->firstPacketUs = 0;
    d->playbackStartUs = 0;
    d->filePlayback = false;
    d->fileDurationUs = 0;
    d->fileBaseTimestampUs = 0;

    // Released last: the format context above is the one using its input.
    d->cacheReader.reset();
}

bool LocalCameraArchiveDelegate::open(
    const QnResourcePtr& resource, AbstractArchiveIntegrityWatcher* archiveIntegrityWatcher)
{
    Q_UNUSED(resource)
    Q_UNUSED(archiveIntegrityWatcher)

    if (d->formatContext)
        return true;

    m_needStop = false;
    if (openStream())
        return true;

    NX_WARNING(typeid(LocalCameraArchiveDelegate), "Failed to open local source '%1': %2",
        sourceDescription(), d->lastError);
    return false;
}

void LocalCameraArchiveDelegate::close()
{
    m_needStop = true;
    closeStream();
}

void LocalCameraArchiveDelegate::pleaseStop()
{
    m_needStop = true;

    if (d->cacheReader)
        d->cacheReader->requestStop();
}

qint64 LocalCameraArchiveDelegate::startTime() const
{
    // Both a live source and a file are reported from zero; a file's endTime() carries its length.
    return 0;
}

qint64 LocalCameraArchiveDelegate::endTime() const
{
    return d->filePlayback ? d->fileDurationUs : 0;
}

qint64 LocalCameraArchiveDelegate::seek(qint64 time, bool findIFrame)
{
    // Nothing to seek to in a live stream.
    if (!d->filePlayback || !d->formatContext)
        return -1;

    // The caller may ask for a position at or past the end of the file. That must not overflow when
    // the base timestamp is added to it, and the very end is also past the last position the demuxer
    // is able to seek to, so a failed seek is retried a little earlier.
    time = qBound<qint64>(0, time, d->fileDurationUs);

    qint64 result = seekTo(time, findIFrame);
    if (result < 0 && time > 0)
    {
        const qint64 backedOff = time - qMin<qint64>(kEndSeekBackoffUs, time);
        result = seekTo(backedOff, findIFrame);
        if (result >= 0)
            time = backedOff;
    }

    if (result < 0)
    {
        NX_WARNING(typeid(LocalCameraArchiveDelegate),
            "Failed to seek local source '%1' to %2 us: %3",
            sourceDescription(), time, result);
        return -1;
    }

    // Drop the live pacing state: playback continues from the new position.
    d->pacingStarted = false;

    return time;
}

qint64 LocalCameraArchiveDelegate::seekTo(qint64 time, bool findIFrame)
{
    const qint64 target = d->fileBaseTimestampUs + time;
    const int result = av_seek_frame(d->formatContext, /*stream_index*/ -1, target,
        findIFrame ? AVSEEK_FLAG_BACKWARD : AVSEEK_FLAG_ANY);

    return result < 0 ? result : time;
}

bool LocalCameraArchiveDelegate::isFilePlayback() const
{
    return d->filePlayback;
}

bool LocalCameraArchiveDelegate::isRealTimeSource() const
{
    return !d->filePlayback;
}

QnConstResourceVideoLayoutPtr LocalCameraArchiveDelegate::getVideoLayout()
{
    // A capture device or a stream URL always provides a single video channel.
    static const QnConstResourceVideoLayoutPtr layout =
        std::make_shared<QnDefaultResourceVideoLayout>();
    return layout;
}

AudioLayoutConstPtr LocalCameraArchiveDelegate::getAudioLayout()
{
    return d->audioLayout;
}

bool LocalCameraArchiveDelegate::hasVideo() const
{
    return true;
}

qint64 LocalCameraArchiveDelegate::packetTimestampUs() const
{
    if (!d->packet)
        return QDateTime::currentMSecsSinceEpoch() * 1000;

    const qint64 rawTimestamp =
        d->packet->dts != AV_NOPTS_VALUE ? d->packet->dts : d->packet->pts;

    // Live data is timestamped with the wall clock, in microseconds.
    if (!d->filePlayback || rawTimestamp == AV_NOPTS_VALUE)
        return QDateTime::currentMSecsSinceEpoch() * 1000;

    // File data keeps its own timeline, relative to the first frame, so seeking stays consistent.
    const AVStream* stream = d->formatContext->streams[d->packet->stream_index];
    const qint64 timestampUs = av_rescale_q(rawTimestamp, stream->time_base, AV_TIME_BASE_Q);
    return qMax(0ll, timestampUs - d->fileBaseTimestampUs);
}

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::makeVideoPacket()
{
    QnWritableCompressedVideoDataPtr video(
        new QnWritableCompressedVideoData(d->packet->size, d->codecParameters));
    video->m_data.write(reinterpret_cast<const char*>(d->packet->data), d->packet->size);
    video->compressionType = d->codecParameters->getAvCodecParameters()->codec_id;
    video->timestamp = packetTimestampUs();
    video->channelNumber = 0;
    video->width = d->codecParameters->getWidth();
    video->height = d->codecParameters->getHeight();

    if (d->packet->flags & AV_PKT_FLAG_KEY)
        video->flags |= QnAbstractMediaData::MediaFlags_AVKey;
    if (!d->filePlayback)
        video->flags |= QnAbstractMediaData::MediaFlags_LIVE;

    av_packet_unref(d->packet);
    return video;
}

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::makeAudioPacket()
{
    const AVStream* stream = d->formatContext->streams[d->audioStreamIndex];

    QnWritableCompressedAudioDataPtr audio(
        new QnWritableCompressedAudioData(d->packet->size, d->audioCodecParameters));
    // The decoder is chosen by the packet's codec id, exactly as for video: without it the packet
    // would carry AV_CODEC_ID_NONE and no audio decoder could ever be created for it.
    audio->compressionType = d->audioCodecParameters->getAvCodecParameters()->codec_id;
    audio->channelNumber = 0;
    audio->timestamp = packetTimestampUs();
    if (d->packet->duration > 0)
    {
        audio->duration = av_rescale_q(
            d->packet->duration, stream->time_base, AVRational{1, 1000000});
    }
    audio->m_data.write(reinterpret_cast<const char*>(d->packet->data), d->packet->size);
    if (!d->filePlayback)
        audio->flags |= QnAbstractMediaData::MediaFlags_LIVE;

    av_packet_unref(d->packet);
    return audio;
}

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::takePacket()
{
    if (!d->formatContext || !d->packet || !d->codecParameters)
        return {};

    while (!m_needStop)
    {
        const int result = av_read_frame(d->formatContext, d->packet);
        if (result < 0)
        {
            if (result == AVERROR(EAGAIN))
                continue;

            if (!m_needStop)
            {
                NX_WARNING(typeid(LocalCameraArchiveDelegate),
                    "Local source '%1' read error: %2", sourceDescription(), result);
            }

            return {};
        }

        const int streamIndex = d->packet->stream_index;
        const bool isVideo = (streamIndex == d->videoStreamIndex);
        const bool isAudio = (streamIndex == d->audioStreamIndex);
        if (!isVideo && !isAudio)
        {
            av_packet_unref(d->packet);
            continue;
        }

        // Pace a live stream against the wall clock, so the reader does not drain the socket at full
        // speed. A file is read as fast as the pipeline requests it and paced by its timestamps.
        const AVPacket& packet = *d->packet;
        const qint64 rawTimestamp = packet.dts != AV_NOPTS_VALUE ? packet.dts : packet.pts;
        if (!d->filePlayback && rawTimestamp != AV_NOPTS_VALUE)
        {
            const qint64 packetUs = av_rescale_q(rawTimestamp,
                d->formatContext->streams[streamIndex]->time_base, AVRational{1, 1000000});
            const qint64 nowUs = av_gettime_relative();
            if (!d->pacingStarted)
            {
                d->pacingStarted = true;
                d->firstPacketUs = packetUs;
                d->playbackStartUs = nowUs;
            }
            else
            {
                const qint64 delayUs = d->playbackStartUs + (packetUs - d->firstPacketUs) - nowUs;
                // Ignore tiny waits and cap large ones, so a timestamp jump cannot stall playback.
                if (delayUs > 2000 && delayUs < 5000000 && !m_needStop)
                    av_usleep(delayUs);
            }
        }

        return isVideo ? makeVideoPacket() : makeAudioPacket();
    }

    return {};
}

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::getNextData()
{
    return takePacket();
}

} // namespace nx::vms::client::desktop
