// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>

#include <QtCore/QString>

#include <nx/streaming/abstract_archive_delegate.h>
#include <nx/utils/impl_ptr.h>

namespace nx::vms::client::desktop {

/**
 * Feeds a local capture device (USB / built-in camera) to a QnArchiveStreamReader.
 *
 * The client's media pipeline expects every resource data provider to be an archive stream reader
 * backed by an archive delegate (that is how both local files and live cameras work), so a local
 * camera has to be provided through this interface rather than as a plain stream provider.
 *
 * The device is opened through FFmpeg's avdevice layer (the `dshow` input on Windows). Compressed
 * frames (MJPEG or H.264, which is what web cameras normally output) are forwarded as is, marked
 * with MediaFlags_LIVE and timestamped with the wall clock.
 */
class LocalCameraArchiveDelegate: public QnAbstractArchiveDelegate
{
    Q_OBJECT
    using base_type = QnAbstractArchiveDelegate;

public:
    explicit LocalCameraArchiveDelegate(const QString& deviceName);
    virtual ~LocalCameraArchiveDelegate() override;

    virtual bool open(const QnResourcePtr& resource,
        AbstractArchiveIntegrityWatcher* archiveIntegrityWatcher = nullptr) override;
    virtual void close() override;

    /** Live source: there is no archive range and nothing to seek to. */
    virtual qint64 startTime() const override;
    virtual qint64 endTime() const override;
    virtual qint64 seek(qint64 time, bool findIFrame) override;

    virtual QnAbstractMediaDataPtr getNextData() override;

    virtual QnConstResourceVideoLayoutPtr getVideoLayout() override;
    virtual AudioLayoutConstPtr getAudioLayout() override;
    virtual bool hasVideo() const override;

    virtual bool isRealTimeSource() const override { return true; }

    virtual void pleaseStop() override;

    QString lastErrorMessage() const;

private:
    static int interruptCallback(void* opaque);
    bool openDevice();
    void closeDevice();
    QnAbstractMediaDataPtr takeVideoPacket();

    struct Private;
    nx::utils::ImplPtr<Private> d;
    std::atomic<bool> m_needStop{false};
};

} // namespace nx::vms::client::desktop
