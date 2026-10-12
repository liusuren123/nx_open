// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

#include <nx/streaming/abstract_media_stream_data_provider.h>
#include <nx/utils/impl_ptr.h>

namespace nx::vms::client::desktop {

/**
 * Streams a local capture device (USB / built-in camera) into the media pipeline.
 *
 * The device is opened through FFmpeg's avdevice layer (the `dshow` input on Windows). Compressed
 * frames (MJPEG or H.264, which is what web cameras normally output) are forwarded as is; asking the
 * device for MJPEG keeps the CPU cost of a local camera negligible.
 *
 * Packets are marked with MediaFlags_LIVE and timestamped with the wall clock, so the client treats
 * the feed as a live camera rather than an archive.
 */
class LocalCameraDataProvider: public QnAbstractMediaStreamDataProvider
{
    Q_OBJECT
    using base_type = QnAbstractMediaStreamDataProvider;

public:
    LocalCameraDataProvider(
        const QnResourcePtr& resource, const QString& deviceName, QObject* parent = nullptr);
    virtual ~LocalCameraDataProvider() override;

    virtual void start(Priority priority = InheritPriority) override;
    virtual void pleaseStop() override;
    virtual QnConstResourceVideoLayoutPtr getVideoLayout() const override;

    /** Human readable description of the last error, empty if there was none. */
    QString lastError() const;

protected:
    virtual void run() override;

private:
    static int interruptCallback(void* opaque);

    bool openDevice();
    void closeDevice();
    void putVideoPacket(const struct AVPacket* packet);

    struct Private;
    nx::utils::ImplPtr<Private> d;
};

} // namespace nx::vms::client::desktop
