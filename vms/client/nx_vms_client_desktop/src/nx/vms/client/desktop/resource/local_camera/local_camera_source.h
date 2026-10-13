// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

#include <nx/reflect/instrument.h>
#include <nx/utils/json/qt_containers_reflect.h>
#include <nx/utils/uuid.h>

namespace nx::vms::client::desktop {

/**
 * Returns the given stream URL with any embedded credentials removed, so it is safe to show or to
 * write to the log. The URL itself is preserved otherwise.
 */
QString localCameraSourceUrlForDisplay(const QString& url);

/**
 * A client-side video source configured by the user, e.g. an RTSP camera. Such sources are opened
 * directly by the client through FFmpeg, without a media server, and are persisted in the local
 * settings.
 *
 * Credentials may either be given explicitly (username/password) or embedded into the URL
 * ("rtsp://user:password@host/stream"); the explicit fields take precedence.
 */
struct LocalCameraSource
{
    /** Stable identifier of the entry; also used as the id of the corresponding resource. */
    nx::Uuid id;

    /** Human readable name shown in the resource tree. */
    QString name;

    /** Stream URL without credentials, e.g. "rtsp://192.168.1.10:554/stream". */
    QString url;

    /** User name, if the source requires authentication. */
    QString username;

    /** Password, if the source requires authentication. */
    QString password;

    bool operator==(const LocalCameraSource& other) const = default;

    /**
     * URL with credentials embedded, exactly as FFmpeg expects it. Built from url/username/password;
     * falls back to the URL as typed when no explicit credentials are set.
     */
    QString streamUrl() const;

    /**
     * Name to show in the UI. Falls back to a name derived from the URL when no explicit name is
     * set. Credentials are never included, so a password cannot leak into the resource tree or into
     * a screenshot.
     */
    QString displayName() const;
};

NX_REFLECTION_INSTRUMENT(LocalCameraSource, (id)(name)(url)(username)(password))

} // namespace nx::vms::client::desktop
