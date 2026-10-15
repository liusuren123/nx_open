// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>
#include <utility>

#include <QtCore/QString>

#include <nx/streaming/abstract_archive_resource.h>
#include <nx/utils/uuid.h>

#include "local_camera_source.h"

namespace nx::vms::client::desktop {

/**
 * A live camera provided by the client itself, without a media server. Video comes either from a
 * local capture device (USB / built-in camera) or from a user-configured stream URL (e.g. an RTSP
 * camera); both are opened by LocalCameraArchiveDelegate through FFmpeg.
 *
 * The resource carries the Qn::local_live_cam flag, so the media pipeline treats it as a live
 * stream, as opposed to QnAviResource, which is an archive (a local file).
 */
class LocalCameraResource: public QnAbstractArchiveResource
{
    Q_OBJECT
    using base_type = QnAbstractArchiveResource;

public:
    /**
     * @param deviceId Stable, backend specific device identifier. The resource id is derived from
     *     it, so the same physical device keeps its id across restarts.
     * @param name Human readable device name shown in the UI.
     */
    LocalCameraResource(const QString& deviceId, const QString& name);

    /** A user-configured source, e.g. an RTSP stream. */
    explicit LocalCameraResource(const LocalCameraSource& source);

    /** Whether this is a user-configured source rather than an auto-detected capture device. */
    bool isUrlSource() const { return !m_url.isEmpty(); }

    /** Capture device name; empty for URL sources. */
    const QString& deviceId() const { return m_deviceId; }

    /** Whether the URL turned out to be a finite, seekable media file rather than a live stream. */
    bool isSeekableFileSource() const { return m_seekableFile; }

    const QString& url() const { return m_url; }

    /** Id of the configuring LocalCameraSource entry; null for capture devices. */
    const nx::Uuid& sourceId() const { return m_sourceId; }

    /**
     * Refreshes a URL resource after its configuring LocalCameraSource was edited, so the new name
     * and URL are picked up. Does nothing for capture devices.
     */
    void updateFromSource(const LocalCameraSource& source);

    /** Deterministic resource id for the given device id. */
    static nx::Uuid resourceIdForDevice(const QString& deviceId);

    virtual bool hasVideo(const QnAbstractStreamDataProvider* dataProvider) const override;
    virtual QnAbstractStreamDataProvider* createDataProvider(Qn::ConnectionRole role) override;
    virtual QnConstResourceVideoLayoutPtr getVideoLayout(
        const QnAbstractStreamDataProvider* dataProvider = nullptr) override;

    /** Range of the file, so the timeline can show its length. Not a file: nullopt. */
    virtual std::optional<std::pair<qint64, qint64>> localFileRangeUs() const override;

private:
    /** Capture device name, empty for URL sources. */
    QString m_deviceId;

    /** Stream URL, empty for capture devices. */
    QString m_url;

    /** Id of the configuring source entry; null for capture devices. */
    nx::Uuid m_sourceId;

    /** Set when the URL is a finite, seekable media file; playback then gets a seekable timeline. */
    bool m_seekableFile = false;

    /** Duration of the file, in microseconds. */
    qint64 m_durationUs = 0;
};

} // namespace nx::vms::client::desktop
