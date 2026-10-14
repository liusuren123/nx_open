// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "local_camera_source.h"

#include <QtCore/QUrl>

#include "local_camera_archive_delegate.h"

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

QString LocalCameraSource::streamUrl() const
{
    const QString trimmedUsername = username.trimmed();
    const QString trimmedPassword = password.trimmed();

    // No explicit credentials: use the URL exactly as typed (it may already embed them).
    if (trimmedUsername.isEmpty() && trimmedPassword.isEmpty())
        return url;

    if (url.isEmpty())
        return url;

    // The credentials are inserted the very same way the user would have typed them into the URL,
    // e.g. "rtsp://user:password@host/stream", and are deliberately NOT percent-encoded: FFmpeg
    // treats URLs verbatim and splits the userinfo at the last '@', which is exactly how a
    // hand-written URL behaves.
    QString userinfo = trimmedUsername;
    if (!trimmedPassword.isEmpty())
        userinfo += ':' + trimmedPassword;

    const QString schemeSeparator = "://";
    const int schemeEnd = url.indexOf(schemeSeparator);
    const int authorityStart = schemeEnd < 0 ? 0 : schemeEnd + schemeSeparator.size();

    // The authority runs up to the first '/' after it (or to the end of the URL).
    const int authorityEnd = url.indexOf('/', authorityStart);
    const int authorityLimit = authorityEnd < 0 ? url.size() : authorityEnd;

    // If the URL already carries credentials, drop them so they are not duplicated.
    const int existingAt = url.lastIndexOf('@', authorityLimit - 1);
    const int hostStart = existingAt >= authorityStart ? existingAt + 1 : authorityStart;
    const QString host = url.mid(hostStart, authorityLimit - hostStart);

    return url.left(authorityStart) + userinfo + '@' + host + url.mid(authorityLimit);
}

QString LocalCameraSource::displayName() const
{
    return name.isEmpty() ? localCameraSourceUrlForDisplay(url) : name;
}

void classifyLocalCameraSource(LocalCameraSource& source)
{
    source.seekableFile = false;
    source.durationUs = 0;

    const QString scheme = QUrl(source.streamUrl()).scheme().toLower();
    if (scheme != "http" && scheme != "https")
        return;

    LocalCameraArchiveDelegate probe(QString(), source.streamUrl(),
        /*filePlaybackExpected*/ true);
    if (!probe.open(QnResourcePtr()) || !probe.isFilePlayback())
    {
        probe.close();
        return;
    }

    source.seekableFile = true;
    source.durationUs = probe.endTime();
    probe.close();
}

} // namespace nx::vms::client::desktop
