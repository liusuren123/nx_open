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
    setUrl(deviceId);
    setIdUnsafe(resourceIdForDevice(deviceId));
}

LocalCameraResource::LocalCameraResource(const LocalCameraSource& source):
    m_url(source.streamUrl()),
    m_sourceId(source.id),
    m_seekableFile(source.seekableFile),
    m_durationUs(source.durationUs)
{
    // A finite, seekable file is played back like a local recording (with a timeline and seeking);
    // anything else is a live stream.
    if (!m_seekableFile)
        addFlags(Qn::local_live_cam);

    // The resource name and URL are used for display and for layout descriptors, so they must never
    // contain the credentials from the stream URL. The full URL is kept in m_url and is only passed
    // to FFmpeg.
    setName(source.displayName());
    setUrl(source.displayName());
    setIdUnsafe(source.id);
}

void LocalCameraResource::updateFromSource(const LocalCameraSource& source)
{
    if (!isUrlSource())
        return; //< Capture devices are not backed by a LocalCameraSource.

    m_url = source.streamUrl();
    setName(source.displayName());
    setUrl(source.displayName());

    m_seekableFile = source.seekableFile;
    m_durationUs = source.durationUs;

    // Keep the live/file classification in sync after an edit.
    if (m_seekableFile)
        removeFlags(Qn::local_live_cam);
    else
        addFlags(Qn::local_live_cam);
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
    reader->setArchiveDelegate(std::unique_ptr<QnAbstractArchiveDelegate>(
        new LocalCameraArchiveDelegate(m_deviceId, m_url, m_seekableFile)));
    return reader;
}

QnConstResourceVideoLayoutPtr LocalCameraResource::getVideoLayout(const QnAbstractStreamDataProvider*)
{
    // A capture device or a stream URL always provides a single video channel.
    static const QnConstResourceVideoLayoutPtr layout =
        std::make_shared<QnDefaultResourceVideoLayout>();
    return layout;
}

std::optional<std::pair<qint64, qint64>> LocalCameraResource::localFileRangeUs() const
{
    if (!m_seekableFile || m_durationUs <= 0)
        return std::nullopt;

    return std::pair<qint64, qint64>(0, m_durationUs);
}

} // namespace nx::vms::client::desktop
