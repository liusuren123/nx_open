// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "m3u_parser.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <nx/utils/log/log.h>

namespace nx::vms::client::desktop {

namespace {

const QRegularExpression& attributePattern()
{
    static const QRegularExpression pattern(R"re(([A-Za-z0-9_-]+)="([^"]*)")re");
    return pattern;
}

/** Position of the first comma that is not inside a quoted attribute value, or -1. */
int titleSeparator(const QString& info)
{
    bool inQuotes = false;
    for (int i = 0; i < info.size(); ++i)
    {
        const QChar character = info.at(i);
        if (character == '"')
            inQuotes = !inQuotes;
        else if (character == ',' && !inQuotes)
            return i;
    }

    return -1;
}

/** Resolves a stream URL that may be relative to the playlist itself (HLS master playlists). */
QString resolveUrl(const QString& url, const QString& baseUrl)
{
    if (baseUrl.isEmpty() || url.contains("://"))
        return url;

    const QUrl resolved = QUrl(baseUrl).resolved(QUrl(url));
    return resolved.isValid() ? resolved.toString() : url;
}

/** Name of a stream that has no #EXTINF title: its last path segment. */
QString titleFromUrl(const QString& url)
{
    const QString name = QUrl(url).path().section('/', -1);
    return name.isEmpty() ? url : name;
}

/**
 * Whether the data is an HLS playlist. Its `#EXTINF` entries describe the segments of a single
 * video (a media playlist) or the variants of one stream (a master playlist), not channels, so
 * treating them as channels would turn one video into hundreds of bogus entries.
 */
bool isHlsPlaylist(const QString& text)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"re(^[ \t]*#EXT-X-)re"), QRegularExpression::MultilineOption);
    return pattern.match(text).hasMatch();
}

/**
 * Whether the data looks like an M3U playlist of channels at all. Arbitrary content (a media file,
 * an HTML page, ...) must not be parsed as a channel list: every one of its lines would otherwise
 * become a bogus entry, so a single video would be added as hundreds of "channels".
 */
bool looksLikeM3uPlaylist(const QString& text)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"re(^[ \t]*#EXT(?:M3U|INF))re"),
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    return pattern.match(text).hasMatch();
}

} // namespace

QList<IptvChannel> parseM3uPlaylist(const QByteArray& data, const QString& baseUrl, QString* error)
{
    if (error)
        error->clear();

    QString text = QString::fromUtf8(data);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1); //< Strip the byte order mark.

    if (isHlsPlaylist(text))
    {
        if (error)
            *error = "The file is an HLS playlist, not a list of channels.";

        NX_DEBUG(typeid(parseM3uPlaylist), "HLS playlist detected, no channels parsed");
        return {};
    }

    if (!looksLikeM3uPlaylist(text))
    {
        if (error)
            *error = "The data is not an M3U playlist.";

        NX_DEBUG(typeid(parseM3uPlaylist), "Not an M3U playlist, no channels parsed");
        return {};
    }

    QList<IptvChannel> channels;
    IptvChannel pending;
    bool hasPending = false;

    const QStringList lines = text.split(QRegularExpression("[\r\n]"));
    for (const QString& rawLine: lines)
    {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith('#'))
        {
            if (line.startsWith("#EXTINF:", Qt::CaseInsensitive))
            {
                const QString info = line.mid(QString("#EXTINF:").size());

                pending = IptvChannel();
                pending.id = nx::Uuid::createUuid();

                const int separator = titleSeparator(info);
                const QString attributes = separator < 0 ? info : info.left(separator);
                pending.name = separator < 0 ? QString() : info.mid(separator + 1).trimmed();

                auto it = attributePattern().globalMatch(attributes);
                while (it.hasNext())
                {
                    const auto match = it.next();
                    const QString key = match.captured(1).toLower();
                    const QString value = match.captured(2);
                    if (key == "group-title")
                        pending.group = value;
                    else if (key == "tvg-logo" || key == "logo")
                        pending.logo = value;
                    else if (key == "tvg-name" && pending.name.isEmpty())
                        pending.name = value;
                    else if (key == "http-user" || key == "user")
                        pending.username = value;
                    else if (key == "http-password" || key == "pass")
                        pending.password = value;
                }

                hasPending = true;
            }
            else if (line.startsWith("#EXTGRP:", Qt::CaseInsensitive) && hasPending)
            {
                pending.group = line.mid(QString("#EXTGRP:").size()).trimmed();
            }

            continue;
        }

        // A plain line is the stream URL of the preceding #EXTINF entry.
        if (hasPending)
        {
            pending.url = resolveUrl(line, baseUrl);
            if (pending.name.isEmpty())
                pending.name = titleFromUrl(pending.url);
            channels.append(pending);
            hasPending = false;
        }
        else
        {
            // Tolerate playlists that contain bare URLs without any #EXTINF.
            IptvChannel channel;
            channel.id = nx::Uuid::createUuid();
            channel.url = resolveUrl(line, baseUrl);
            channel.name = titleFromUrl(channel.url);
            channels.append(channel);
        }
    }

    if (channels.isEmpty() && error)
        *error = "No channels found in the playlist.";

    NX_DEBUG(typeid(parseM3uPlaylist), "Parsed %1 channel(s)", channels.size());
    return channels;
}

} // namespace nx::vms::client::desktop
