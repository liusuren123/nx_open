// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <nx/reflect/json.h>
#include <nx/utils/uuid.h>
#include <nx/vms/client/desktop/playlists/playlist.h>

namespace nx::vms::client::desktop {
namespace test {

TEST(PlaylistDataTest, defaultValues)
{
    const PlaylistEntry entry;
    EXPECT_TRUE(entry.path.isEmpty());
    EXPECT_FALSE(entry.isFolder);

    const Playlist playlist;
    EXPECT_TRUE(playlist.id.isNull());
    EXPECT_TRUE(playlist.name.isEmpty());
    EXPECT_TRUE(playlist.entries.isEmpty());
}

TEST(PlaylistDataTest, equality)
{
    const PlaylistEntry fileEntry{"C:/media/video.mp4", false};
    EXPECT_EQ(fileEntry, (PlaylistEntry{"C:/media/video.mp4", false}));
    EXPECT_NE(fileEntry, (PlaylistEntry{"C:/media/other.mp4", false}));
    EXPECT_NE(fileEntry, (PlaylistEntry{"C:/media/video.mp4", true}));

    Playlist a;
    a.id = nx::Uuid::createUuid();
    a.name = "Playlist";
    a.entries = {fileEntry};

    Playlist b = a;
    EXPECT_EQ(a, b);

    b.entries.append({"C:/media/folder", true});
    EXPECT_NE(a, b);
}

TEST(PlaylistDataTest, jsonRoundTrip)
{
    Playlist playlist;
    playlist.id = nx::Uuid::createUuid();
    playlist.name = "My playlist";
    playlist.entries = {
        {"C:/media/first.mp4", false},
        {"C:/media/folder", true},
        {"C:/media/second.mkv", false},
    };

    const auto json = nx::reflect::json::serialize(playlist);
    Playlist restored;
    ASSERT_TRUE(nx::reflect::json::deserialize(json, &restored));
    EXPECT_EQ(restored, playlist);
}

TEST(PlaylistDataTest, jsonRoundTripListOfPlaylists)
{
    QList<Playlist> playlists;

    Playlist first;
    first.id = nx::Uuid::createUuid();
    first.name = "First";
    first.entries = {{"C:/a.mp4", false}};
    playlists.append(first);

    Playlist second;
    second.id = nx::Uuid::createUuid();
    second.name = "Second";
    playlists.append(second);

    const auto json = nx::reflect::json::serialize(playlists);
    QList<Playlist> restored;
    ASSERT_TRUE(nx::reflect::json::deserialize(json, &restored));
    EXPECT_EQ(restored, playlists);
}

} // namespace test
} // namespace nx::vms::client::desktop
