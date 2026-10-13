// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

#include <nx/reflect/instrument.h>
#include <nx/utils/json/qt_containers_reflect.h>
#include <nx/utils/uuid.h>

namespace nx::vms::client::desktop {

/**
 * A client-side video source configured by the user, e.g. an RTSP camera. Such sources are opened
 * directly by the client through FFmpeg, without a media server, and are persisted in the local
 * settings.
 */
struct LocalCameraSource
{
    /** Stable identifier of the entry; also used as the id of the corresponding resource. */
    nx::Uuid id;

    /** Human readable name shown in the resource tree. */
    QString name;

    /** Stream URL, e.g. "rtsp://user:password@192.168.1.10:554/stream". */
    QString url;

    bool operator==(const LocalCameraSource& other) const = default;
};

NX_REFLECTION_INSTRUMENT(LocalCameraSource, (id)(name)(url))

} // namespace nx::vms::client::desktop
