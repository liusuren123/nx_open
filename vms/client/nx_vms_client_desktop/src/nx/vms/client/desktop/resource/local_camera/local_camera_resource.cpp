// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_resource.h"

#include <core/resource/resource_media_layout.h>
#include <nx/streaming/archive_stream_reader.h>

#include "local_camera_archive_delegate.h"

namespace nx::vms::client::desktop {

LocalCameraResource::LocalCameraResource(const QString& deviceId, const QString& name):
    m_deviceId(deviceId)
{
    // `local_live_cam` marks a live camera provided by the client itself, so the whole media
    // pipeline renders it as a live stream rather than as an archive (a local file).
    addFlags(Qn::local_live_cam);

    setName(name.isEmpty() ? tr("Local camera") : name);
    setUrl(m_deviceId);
    setIdUnsafe(resourceIdForDevice(deviceId));
}

nx::Uuid LocalCameraResource::resourceIdForDevice(const QString& deviceId)
{
    return nx::Uuid::fromArbitraryData("nx_local_camera:" + deviceId);
}

bool LocalCameraResource::hasVideo(const QnAbstractStreamDataProvider*) const
{
    return true;
}

QnAbstractStreamDataProvider* LocalCameraResource::createDataProvider(Qn::ConnectionRole)
{
    // The media pipeline expects an archive stream reader backed by an archive delegate, which is
    // how both local files and live cameras are provided. The captured frames are fed through a
    // delegate, so the reader (and the camera display behind it) get everything they need.
    auto reader = new QnArchiveStreamReader(toSharedPointer(this));
    reader->setArchiveDelegate(
        std::unique_ptr<QnAbstractArchiveDelegate>(new LocalCameraArchiveDelegate(getName())));
    return reader;
}

QnConstResourceVideoLayoutPtr LocalCameraResource::getVideoLayout(const QnAbstractStreamDataProvider*)
{
    // A capture device always provides a single video channel.
    static const QnConstResourceVideoLayoutPtr layout =
        std::make_shared<QnDefaultResourceVideoLayout>();
    return layout;
}

} // namespace nx::vms::client::desktop
