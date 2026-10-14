// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

#include <nx/reflect/instrument.h>
#include <nx/utils/json/qt_containers_reflect.h>
#include <nx/utils/uuid.h>

namespace nx::vms::client::desktop {

/**
 * A single IPTV channel parsed from an M3U/M3U8 playlist. The stream itself is described the same
 * way as a local camera source (url + optional credentials), so it can be opened through the very
 * same media pipeline.
 */
struct IptvChannel
{
    /** Stable identifier; also used as the id of the resource created for the channel. */
    nx::Uuid id;
    QString name;
    QString url;
    QString username;
    QString password;
    /** Value of the `group-title` attribute, empty when the playlist does not set it. */
    QString group;
    /** Value of the `tvg-logo` attribute, an URL the UI may show next to the channel. */
    QString logo;

    bool operator==(const IptvChannel& other) const = default;
};

/** A playlist imported by the user. Stored in the local settings, never on a server. */
struct IptvPlaylist
{
    nx::Uuid id;
    QString name;
    /** Where the playlist was imported from: an http(s) URL or a local file path. */
    QString source;
    QList<IptvChannel> channels;

    bool operator==(const IptvPlaylist& other) const = default;
};

NX_REFLECTION_INSTRUMENT(IptvChannel, (id)(name)(url)(username)(password)(group)(logo))
NX_REFLECTION_INSTRUMENT(IptvPlaylist, (id)(name)(source)(channels))

} // namespace nx::vms::client::desktop
