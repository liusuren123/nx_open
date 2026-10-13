// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_source.h"

namespace nx::vms::client::desktop {

QString localCameraSourceUrlForDisplay(const QString& url)
{
    // The URL has to keep its credentials to open the stream, but they must never be shown in the
    // UI or written to the log (which ends up in support bundles). Parsing is done by hand rather
    // than with QUrl: passwords may contain characters that make the URL not strictly valid, and
    // QUrl then refuses to expose the host.
    const QString schemeSeparator = "://";
    const int schemeEnd = url.indexOf(schemeSeparator);
    const int authorityStart = schemeEnd < 0 ? 0 : schemeEnd + schemeSeparator.size();

    // The userinfo, if any, ends at the last '@' of the authority (a password may itself contain
    // '@'), and the authority ends at the first '/' after it.
    const int authorityEnd = url.indexOf('/', authorityStart);
    const int credentialsEnd = url.lastIndexOf(
        '@', authorityEnd < 0 ? url.size() - 1 : authorityEnd);

    if (credentialsEnd < authorityStart)
        return url;

    return url.left(authorityStart) + url.mid(credentialsEnd + 1);
}

QString LocalCameraSource::displayName() const
{
    return name.isEmpty() ? localCameraSourceUrlForDisplay(url) : name;
}

} // namespace nx::vms::client::desktop
