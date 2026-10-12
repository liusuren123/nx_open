// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_data_provider.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>
#include <QtCore/QThread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

#include <core/resource/resource_media_layout.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/video_data_packet.h>
#include <nx/utils/log/log.h>

namespace nx::vms::client::desktop {

struct LocalCameraDataProvider::Private
{
    const QString deviceName;

    AVFormatContext* formatContext = nullptr;
    int videoStreamIndex = -1;
    CodecParametersConstPtr codecParameters;

    QString lastError;
};

LocalCameraDataProvider::LocalCameraDataProvider(const QString& deviceName, QObject* parent):
    base_type(),
    d(new Private{deviceName})
{
    setObjectName("LocalCameraDataProvider");
    // The provider is owned by the reader (see LocalCameraResource::createDataProvider), not by the
    // parent, so it is created without one.
    Q_UNUSED(parent)
}

LocalCameraDataProvider::~LocalCameraDataProvider()
{
    pleaseStop();
    wait();
}

void LocalCameraDataProvider::start(Priority priority)
{
    if (isRunning())
        return;

    m_needStop = false;
    base_type::start(priority);
}

void LocalCameraDataProvider::pleaseStop()
{
    m_needStop = true;
}

QnConstResourceVideoLayoutPtr LocalCameraDataProvider::getVideoLayout() const
{
    // A capture device always provides a single video channel.
    static const QnConstResourceVideoLayoutPtr layout =
        std::make_shared<QnDefaultResourceVideoLayout>();
    return layout;
}

QString LocalCameraDataProvider::lastError() const
{
    return d->lastError;
}

int LocalCameraDataProvider::interruptCallback(void* opaque)
{
    const auto provider = static_cast<LocalCameraDataProvider*>(opaque);
    return provider->m_needStop ? 1 : 0;
}

bool LocalCameraDataProvider::openDevice()
{
    avdevice_register_all();

    const AVInputFormat* inputFormat = av_find_input_format("dshow");
    if (!inputFormat)
    {
        d->lastError = tr("The FFmpeg 'dshow' input device is not available.");
        return false;
    }

    const QByteArray deviceName = ("video=" + d->deviceName).toUtf8();

    // Prefer the camera's compressed output: MJPEG (or H.264) frames can be forwarded without
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
            break;

        d->formatContext = nullptr;
    }

    if (!d->formatContext)
    {
        d->lastError = tr("Failed to open capture device '%1'.").arg(d->deviceName);
        return false;
    }

    // Let av_read_frame() bail out once the provider is asked to stop.
    d->formatContext->interrupt_callback.callback = &LocalCameraDataProvider::interruptCallback;
    d->formatContext->interrupt_callback.opaque = this;

    if (avformat_find_stream_info(d->formatContext, nullptr) < 0)
    {
        d->lastError = tr("Failed to read stream information from '%1'.").arg(d->deviceName);
        return false;
    }

    d->videoStreamIndex = av_find_best_stream(
        d->formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->videoStreamIndex < 0)
    {
        d->lastError = tr("Capture device '%1' has no video stream.").arg(d->deviceName);
        return false;
    }

    const AVStream* stream = d->formatContext->streams[d->videoStreamIndex];
    const AVCodecID codecId = stream->codecpar->codec_id;
    if (codecId != AV_CODEC_ID_MJPEG && codecId != AV_CODEC_ID_H264)
    {
        // Raw pixel formats would have to be converted and re-encoded; not supported yet.
        d->lastError = tr("Unsupported pixel format of capture device '%1' (codec id %2).")
            .arg(d->deviceName).arg(static_cast<int>(codecId));
        return false;
    }

    d->codecParameters = CodecParametersConstPtr(new CodecParameters(stream->codecpar));

    NX_INFO(this, "Opened local camera '%1': %2x%3, %4 fps",
        d->deviceName,
        stream->codecpar->width,
        stream->codecpar->height,
        av_q2d(stream->avg_frame_rate));

    return true;
}

void LocalCameraDataProvider::closeDevice()
{
    if (d->formatContext)
        avformat_close_input(&d->formatContext);

    d->formatContext = nullptr;
    d->videoStreamIndex = -1;
    d->codecParameters.reset();
}

void LocalCameraDataProvider::putVideoPacket(const AVPacket* packet)
{
    const auto codecParameters = d->codecParameters;
    if (!codecParameters)
        return;

    // Live data is timestamped with the wall clock, in microseconds.
    const qint64 timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;

    QnWritableCompressedVideoDataPtr video(
        new QnWritableCompressedVideoData(packet->size, codecParameters));
    video->m_data.write(reinterpret_cast<const char*>(packet->data), packet->size);
    video->compressionType = codecParameters->getAvCodecParameters()->codec_id;
    video->timestamp = timestampUs;
    video->channelNumber = 0;
    video->width = codecParameters->getWidth();
    video->height = codecParameters->getHeight();

    if (packet->flags & AV_PKT_FLAG_KEY)
        video->flags |= QnAbstractMediaData::MediaFlags_AVKey;
    video->flags |= QnAbstractMediaData::MediaFlags_LIVE;
    video->dataProvider = this;

    putData(video);
}

void LocalCameraDataProvider::run()
{
    QThread::currentThread()->setPriority(QThread::HighPriority);

    if (!openDevice())
    {
        NX_WARNING(this, "Could not open local camera '%1': %2", d->deviceName, d->lastError);
        closeDevice();
        return;
    }

    AVPacket* packet = av_packet_alloc();
    while (!m_needStop)
    {
        const int result = av_read_frame(d->formatContext, packet);
        if (result < 0)
        {
            if (result == AVERROR(EAGAIN))
                continue;

            if (!m_needStop)
                NX_WARNING(this, "Local camera '%1' read error: %2", d->deviceName, result);
            break;
        }

        if (packet->stream_index == d->videoStreamIndex)
            putVideoPacket(packet);

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    closeDevice();
}

} // namespace nx::vms::client::desktop
