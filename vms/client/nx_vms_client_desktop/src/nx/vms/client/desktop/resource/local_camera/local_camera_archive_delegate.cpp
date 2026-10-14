// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_archive_delegate.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

#include <core/resource/audio_layout.h>
#include <core/resource/resource_media_layout.h>
#include <nx/media/audio_data_packet.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/video_data_packet.h>
#include <nx/utils/log/log.h>

#include "local_camera_source.h"

namespace nx::vms::client::desktop {

namespace {

/** Stream URL scheme; selects the protocol-specific FFmpeg options. */
enum class StreamScheme { rtsp, http, rtmp, udp, other };

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

} // namespace

struct LocalCameraArchiveDelegate::Private
{
    /** Capture device name; empty for URL sources. */
    const QString deviceId;

    /** Stream URL; empty for capture devices. */
    const QString url;

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

    QString lastError;
};

LocalCameraArchiveDelegate::LocalCameraArchiveDelegate(const QString& deviceId, const QString& url):
    d(new Private{deviceId, url})
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
    avdevice_register_all();

    if (!d->url.isEmpty())
    {
        if (!openUrl())
            return false;
    }
    else if (!openCaptureDevice())
    {
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

    NX_INFO(typeid(LocalCameraArchiveDelegate), "Opened local source '%1': %2x%3, %4 fps, audio: %5",
        sourceDescription(),
        stream->codecpar->width,
        stream->codecpar->height,
        av_q2d(stream->avg_frame_rate),
        d->audioStreamIndex >= 0);

    return true;
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
                options = {
                    {"rw_timeout", "8000000"},
                    {"reconnect", "1"},
                    {"reconnect_streamed", "1"},
                    {"reconnect_delay_max", "4"},
                    {"user_agent", "NxMetaClient"}};
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

    d->lastError = tr("Failed to open stream '%1': %2.")
        .arg(localCameraSourceUrlForDisplay(d->url), lastError);
    return false;
}

void LocalCameraArchiveDelegate::closeStream()
{
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

    d->pacingStarted = false;
    d->firstPacketUs = 0;
    d->playbackStartUs = 0;
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
}

qint64 LocalCameraArchiveDelegate::startTime() const
{
    // A live source has no archive range.
    return 0;
}

qint64 LocalCameraArchiveDelegate::endTime() const
{
    return 0;
}

qint64 LocalCameraArchiveDelegate::seek(qint64 /*time*/, bool /*findIFrame*/)
{
    // Nothing to seek to in a live stream.
    return -1;
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

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::makeVideoPacket()
{
    // Live data is timestamped with the wall clock, in microseconds.
    QnWritableCompressedVideoDataPtr video(
        new QnWritableCompressedVideoData(d->packet->size, d->codecParameters));
    video->m_data.write(reinterpret_cast<const char*>(d->packet->data), d->packet->size);
    video->compressionType = d->codecParameters->getAvCodecParameters()->codec_id;
    video->timestamp = QDateTime::currentMSecsSinceEpoch() * 1000;
    video->channelNumber = 0;
    video->width = d->codecParameters->getWidth();
    video->height = d->codecParameters->getHeight();

    if (d->packet->flags & AV_PKT_FLAG_KEY)
        video->flags |= QnAbstractMediaData::MediaFlags_AVKey;
    video->flags |= QnAbstractMediaData::MediaFlags_LIVE;

    av_packet_unref(d->packet);
    return video;
}

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::makeAudioPacket()
{
    const AVStream* stream = d->formatContext->streams[d->audioStreamIndex];

    QnWritableCompressedAudioDataPtr audio(
        new QnWritableCompressedAudioData(d->packet->size, d->audioCodecParameters));
    audio->channelNumber = 0;
    audio->timestamp = QDateTime::currentMSecsSinceEpoch() * 1000;
    if (d->packet->duration > 0)
    {
        audio->duration = av_rescale_q(
            d->packet->duration, stream->time_base, AVRational{1, 1000000});
    }
    audio->m_data.write(reinterpret_cast<const char*>(d->packet->data), d->packet->size);
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

        // Pace the stream against the wall clock. A live source already arrives in real time, so
        // this is a no-op for it; a file-like source (an mp4 URL or a VOD HLS playlist) would
        // otherwise be consumed at full speed.
        const AVPacket& packet = *d->packet;
        const qint64 rawTimestamp = packet.dts != AV_NOPTS_VALUE ? packet.dts : packet.pts;
        if (rawTimestamp != AV_NOPTS_VALUE)
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
