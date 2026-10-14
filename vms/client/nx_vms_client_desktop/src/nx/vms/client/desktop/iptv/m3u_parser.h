// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

#include "iptv_playlist.h"

namespace nx::vms::client::desktop {

/**
 * Parses an M3U/M3U8 playlist of IPTV streams.
 *
 * Only the multi-channel playlist form is understood (`#EXTINF` entries, each followed by a stream
 * URL); an HLS media playlist (a list of `#EXT-X-*` tags with segment URLs) yields no channels.
 * Every channel gets a freshly generated, stable id.
 *
 * @param baseUrl URL the playlist was loaded from, used to resolve relative stream URLs (HLS master
 *     playlists list them relatively). May be empty for local files.
 * @param error Set to a human readable reason when no channel could be parsed.
 */
QList<IptvChannel> parseM3uPlaylist(const QByteArray& data, const QString& baseUrl, QString* error);

} // namespace nx::vms::client::desktop
