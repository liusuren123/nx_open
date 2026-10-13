// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_archive_delegate.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <core/resource/resource_media_layout.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/video_data_packet.h>
#include <nx/utils/log/log.h>

#include "local_camera_source.h"

namespace nx::vms::client::desktop {

struct LocalCameraArchiveDelegate::Private
{
    /** Capture device name; empty for URL sources. */
    const QString deviceId;

    /** Stream URL; empty for capture devices. */
    const QString url;

    AVFormatContext* formatContext = nullptr;
    AVPacket* packet = nullptr;
    int videoStreamIndex = -1;
    CodecParametersConstPtr codecParameters;

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

    NX_INFO(typeid(LocalCameraArchiveDelegate), "Opened local source '%1': %2x%3, %4 fps",
        sourceDescription(),
        stream->codecpar->width,
        stream->codecpar->height,
        av_q2d(stream->avg_frame_rate));

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

bool LocalCameraArchiveDelegate::openUrl()
{
    // Interleaved RTSP over TCP tolerates packet loss and works through NAT much better than plain
    // UDP, so it is tried first. A few servers (e.g. VLC's VOD mode) only support UDP, hence the
    // fallback.
    static const QStringList transports{"tcp", "udp"};

    QString lastError;
    for (const QString& transport: transports)
    {
        d->formatContext = avformat_alloc_context();
        if (!d->formatContext)
        {
            d->lastError = tr("Failed to allocate an FFmpeg context.");
            return false;
        }

        d->formatContext->interrupt_callback.callback =
            &LocalCameraArchiveDelegate::interruptCallback;
        d->formatContext->interrupt_callback.opaque = this;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", transport.toUtf8().constData(), 0);
        av_dict_set(&options, "stimeout", "5000000", 0); //< Socket timeout, in microseconds.
        av_dict_set(&options, "max_delay", "500000", 0);
        av_dict_set(&options, "fflags", "nobuffer", 0);
        av_dict_set(&options, "flags", "low_delay", 0);

        const int result = avformat_open_input(
            &d->formatContext, d->url.toUtf8().constData(), /*inputFormat*/ nullptr, &options);
        av_dict_free(&options);

        if (result >= 0 && d->formatContext)
            return true;

        char buffer[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, buffer, sizeof(buffer));
        lastError = QString::fromUtf8(buffer);

        d->formatContext = nullptr;
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
    d->codecParameters.reset();
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
    return {};
}

bool LocalCameraArchiveDelegate::hasVideo() const
{
    return true;
}

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::takeVideoPacket()
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
                NX_WARNING(typeid(LocalCameraArchiveDelegate),
                    "Local source '%1' read error: %2", sourceDescription(), result);

            return {};
        }

        if (d->packet->stream_index != d->videoStreamIndex)
        {
            av_packet_unref(d->packet);
            continue;
        }

        // Live data is timestamped with the wall clock, in microseconds.
        const qint64 timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;

        QnWritableCompressedVideoDataPtr video(
            new QnWritableCompressedVideoData(d->packet->size, d->codecParameters));
        video->m_data.write(reinterpret_cast<const char*>(d->packet->data), d->packet->size);
        video->compressionType = d->codecParameters->getAvCodecParameters()->codec_id;
        video->timestamp = timestampUs;
        video->channelNumber = 0;
        video->width = d->codecParameters->getWidth();
        video->height = d->codecParameters->getHeight();

        if (d->packet->flags & AV_PKT_FLAG_KEY)
            video->flags |= QnAbstractMediaData::MediaFlags_AVKey;
        video->flags |= QnAbstractMediaData::MediaFlags_LIVE;

        av_packet_unref(d->packet);
        return video;
    }

    return {};
}

QnAbstractMediaDataPtr LocalCameraArchiveDelegate::getNextData()
{
    return takeVideoPacket();
}

} // namespace nx::vms::client::desktop
