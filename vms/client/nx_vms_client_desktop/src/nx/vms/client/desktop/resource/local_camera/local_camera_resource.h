// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

#include <nx/streaming/abstract_archive_resource.h>
#include <nx/utils/uuid.h>

namespace nx::vms::client::desktop {

/**
 * A live camera provided by the client itself, without a media server. Video comes from a local
 * capture device (USB / built-in camera) opened by LocalCameraArchiveDelegate.
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

    QString deviceId() const { return m_deviceId; }

    /** Deterministic resource id for the given device id. */
    static nx::Uuid resourceIdForDevice(const QString& deviceId);

    virtual bool hasVideo(const QnAbstractStreamDataProvider* dataProvider) const override;
    virtual QnAbstractStreamDataProvider* createDataProvider(Qn::ConnectionRole role) override;
    virtual QnConstResourceVideoLayoutPtr getVideoLayout(
        const QnAbstractStreamDataProvider* dataProvider = nullptr) override;

private:
    const QString m_deviceId;
};

} // namespace nx::vms::client::desktop
