// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "m3u_parser.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

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

} // namespace

QList<IptvChannel> parseM3uPlaylist(const QByteArray& data, QString* error)
{
    if (error)
        error->clear();

    QString text = QString::fromUtf8(data);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1); //< Strip the byte order mark.

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
            pending.url = line;
            if (pending.name.isEmpty())
                pending.name = line;
            channels.append(pending);
            hasPending = false;
        }
        else
        {
            // Tolerate playlists that contain bare URLs without any #EXTINF.
            IptvChannel channel;
            channel.id = nx::Uuid::createUuid();
            channel.url = line;
            channel.name = line;
            channels.append(channel);
        }
    }

    if (channels.isEmpty() && error)
        *error = "No channels found in the playlist.";

    NX_DEBUG(typeid(parseM3uPlaylist), "Parsed %1 channel(s)", channels.size());
    return channels;
}

} // namespace nx::vms::client::desktop
