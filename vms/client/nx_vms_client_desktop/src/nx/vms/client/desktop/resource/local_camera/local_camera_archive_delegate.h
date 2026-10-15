// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>

#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>

#include <nx/streaming/abstract_archive_delegate.h>
#include <nx/utils/impl_ptr.h>

namespace nx::vms::client::desktop {

/**
 * Feeds a client-side live source to a QnArchiveStreamReader: either a local capture device
 * (USB / built-in camera) or a user-provided stream URL (e.g. an RTSP camera).
 *
 * The client's media pipeline expects every resource data provider to be an archive stream reader
 * backed by an archive delegate (that is how both local files and live cameras work), so a local
 * source has to be provided through this interface rather than as a plain stream provider.
 *
 * Capture devices are opened through FFmpeg's avdevice layer (the `dshow` input on Windows), stream
 * URLs through the regular avformat layer. Compressed frames (MJPEG, H.264, H.265...) are forwarded
 * as is, marked with MediaFlags_LIVE and timestamped with the wall clock.
 */
class LocalCameraArchiveDelegate: public QnAbstractArchiveDelegate
{
    Q_OBJECT
    using base_type = QnAbstractArchiveDelegate;

public:
    /**
     * Exactly one of the two arguments must be set.
     *
     * @param deviceId Capture device name as expected by FFmpeg's `dshow` input.
     * @param url Stream URL the client opens directly (rtsp, http, udp...).
     * @param filePlaybackExpected Set for sources the user expects to be a media file rather than a
     *     live stream. When set, the source is checked for a finite duration and, if it is a
     *     seekable file, played back like a local recording.
     */
    LocalCameraArchiveDelegate(const QString& deviceId, const QString& url,
        bool filePlaybackExpected = false);
    virtual ~LocalCameraArchiveDelegate() override;

    virtual bool open(const QnResourcePtr& resource,
        AbstractArchiveIntegrityWatcher* archiveIntegrityWatcher = nullptr) override;
    virtual void close() override;

    /**
     * A finite, seekable source (a media file served over a network, or a VOD playlist) plays back
     * like a local recording: it has a known duration and can be seeked. A live source has no range
     * and cannot be seeked.
     */
    virtual qint64 startTime() const override;
    virtual qint64 endTime() const override;
    virtual qint64 seek(qint64 time, bool findIFrame) override;

    /** True when the source turned out to be a seekable file rather than a live stream. */
    bool isFilePlayback() const;

    virtual QnAbstractMediaDataPtr getNextData() override;

    virtual QnConstResourceVideoLayoutPtr getVideoLayout() override;
    virtual AudioLayoutConstPtr getAudioLayout() override;
    virtual bool hasVideo() const override;

    virtual bool isRealTimeSource() const override;

    virtual void pleaseStop() override;

    QString lastErrorMessage() const;

private:
    static int interruptCallback(void* opaque);
    bool openStream();
    /** Classifies the opened source as a live stream or a seekable file. */
    void detectFilePlayback();
    bool openCaptureDevice();
    bool openUrl();
    /** Reads the stream layout of the currently opened input. */
    bool readStreamInfo();
    /** Seeks to the given position of the file, returning it or a negative FFmpeg error. */
    qint64 seekTo(qint64 time, bool findIFrame);
    /**
     * Replaces the input of a finite file with the stream cache, so that seeking and replaying it do
     * not download it again. Returns false when the source must keep being read directly.
     */
    bool switchToCachedInput();
    /** Opens the stream with the given FFmpeg options, reporting the reason on failure. */
    bool openUrlWithOptions(const QList<QPair<QString, QString>>& options, QString* errorOut);
    /**
     * The URL actually handed to FFmpeg: for UDP a missing '@' is added in front of a multicast
     * address, which FFmpeg needs in order to receive it.
     */
    QString effectiveUrl() const;
    void closeStream();
    QnAbstractMediaDataPtr takePacket();
    /** Timestamp of the packet in d->packet, in microseconds; live data is wall-clock based. */
    qint64 packetTimestampUs() const;
    QnAbstractMediaDataPtr makeVideoPacket();
    QnAbstractMediaDataPtr makeAudioPacket();
    QString sourceDescription() const;

    struct Private;
    nx::utils::ImplPtr<Private> d;
    std::atomic<bool> m_needStop{false};
};

} // namespace nx::vms::client::desktop
