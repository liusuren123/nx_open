// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <algorithm>
#include <memory>

#include <gtest/gtest.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtTest/QSignalSpy>

#include <nx/utils/uuid.h>
#include <nx/vms/client/desktop/playlists/playlist_manager.h>
#include <nx/vms/client/desktop/settings/local_settings.h>
#include <nx/vms/client/desktop/test_support/test_context.h>

namespace nx::vms::client::desktop {
namespace test {

namespace {

QString clean(const QString& path)
{
    return QDir::cleanPath(path);
}

} // namespace

class PlaylistManagerTest: public ContextBasedTest
{
protected:
    void SetUp() override
    {
        m_tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(m_tempDir->isValid());

        // Use an isolated settings directory so the test never touches the real local settings.
        m_settingsPath = m_tempDir->path() + "/settings";
        m_settings = std::make_unique<LocalSettings>(m_settingsPath);
        m_manager = std::make_unique<PlaylistManager>(m_settings.get());
    }

    void TearDown() override
    {
        m_manager.reset();
        m_settings.reset();
        ContextBasedTest::TearDown();
    }

    QString mediaDir() const
    {
        const QString path = m_tempDir->path() + "/media";
        QDir().mkpath(path);
        return path;
    }

    /** Creates a non-empty file with the given relative name inside the media dir. */
    QString createFile(const QString& relativeName) const
    {
        const QString path = mediaDir() + "/" + relativeName;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("data");
        file.close();
        return path;
    }

    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_settingsPath;
    std::unique_ptr<LocalSettings> m_settings;
    std::unique_ptr<PlaylistManager> m_manager;
};

TEST_F(PlaylistManagerTest, startsEmpty)
{
    EXPECT_TRUE(m_manager->playlists().isEmpty());
    EXPECT_TRUE(m_manager->currentPlaylistId().isEmpty());
}

TEST_F(PlaylistManagerTest, createPlaylist)
{
    QSignalSpy spy(m_manager.get(), &PlaylistManager::playlistsChanged);

    const QString id = m_manager->createPlaylist("My list");
    EXPECT_FALSE(id.isEmpty());

    ASSERT_EQ(m_manager->playlists().size(), 1);
    EXPECT_EQ(m_manager->playlists().first().name, "My list");
    EXPECT_EQ(m_manager->playlist(id).name, "My list");

    // The first created playlist becomes the current one.
    EXPECT_EQ(m_manager->currentPlaylistId(), id);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(PlaylistManagerTest, createPlaylistWithoutNameGetsDefaultName)
{
    const QString id = m_manager->createPlaylist();
    EXPECT_FALSE(id.isEmpty());
    EXPECT_FALSE(m_manager->playlist(id).name.isEmpty());
}

TEST_F(PlaylistManagerTest, playlistLookupByUnknownIdReturnsEmpty)
{
    const Playlist playlist = m_manager->playlist("not-an-id");
    EXPECT_TRUE(playlist.id.isNull());
    EXPECT_TRUE(playlist.entries.isEmpty());
}

TEST_F(PlaylistManagerTest, removePlaylist)
{
    const QString first = m_manager->createPlaylist("First");
    const QString second = m_manager->createPlaylist("Second");

    EXPECT_TRUE(m_manager->removePlaylist(second));
    ASSERT_EQ(m_manager->playlists().size(), 1);
    EXPECT_EQ(m_manager->currentPlaylistId(), first);

    EXPECT_TRUE(m_manager->removePlaylist(first));
    EXPECT_TRUE(m_manager->playlists().isEmpty());
    EXPECT_TRUE(m_manager->currentPlaylistId().isEmpty());

    EXPECT_FALSE(m_manager->removePlaylist(first));
}

TEST_F(PlaylistManagerTest, renamePlaylist)
{
    const QString id = m_manager->createPlaylist("Old");

    EXPECT_TRUE(m_manager->renamePlaylist(id, "New"));
    EXPECT_EQ(m_manager->playlist(id).name, "New");

    // Renaming to the same name is a no-op.
    EXPECT_FALSE(m_manager->renamePlaylist(id, "New"));
    EXPECT_FALSE(m_manager->renamePlaylist("unknown", "Name"));
}

TEST_F(PlaylistManagerTest, addPathsDetectsFilesAndFolders)
{
    const QString id = m_manager->createPlaylist();
    const QString video = createFile("clip.mp4");
    const QString text = createFile("notes.txt");
    const QString folder = mediaDir() + "/sub";
    QDir().mkpath(folder);

    EXPECT_TRUE(m_manager->addPaths(id, {video, text, folder}));

    const auto entries = m_manager->playlist(id).entries;
    ASSERT_EQ(entries.size(), 2); // The .txt file is filtered out.

    EXPECT_FALSE(entries[0].isFolder);
    EXPECT_EQ(clean(entries[0].path), clean(video));

    EXPECT_TRUE(entries[1].isFolder);
    EXPECT_EQ(clean(entries[1].path), clean(folder));
}

TEST_F(PlaylistManagerTest, addPathsRejectsUnsupportedFiles)
{
    const QString id = m_manager->createPlaylist();
    const QString text = createFile("readme.txt");

    EXPECT_FALSE(m_manager->addPaths(id, {text}));
    EXPECT_TRUE(m_manager->playlist(id).entries.isEmpty());
    EXPECT_FALSE(m_manager->addPaths("unknown", {text}));
}

TEST_F(PlaylistManagerTest, addFilesAcceptsUrls)
{
    const QString id = m_manager->createPlaylist();
    const QString video = createFile("url_clip.mkv");

    EXPECT_TRUE(m_manager->addFiles(id, {QUrl::fromLocalFile(video)}));

    const auto entries = m_manager->playlist(id).entries;
    ASSERT_EQ(entries.size(), 1);
    EXPECT_FALSE(entries.first().isFolder);
    EXPECT_EQ(clean(entries.first().path), clean(video));
}

TEST_F(PlaylistManagerTest, removeEntries)
{
    const QString id = m_manager->createPlaylist();
    const QString a = createFile("a.mp4");
    const QString b = createFile("b.mp4");
    const QString c = createFile("c.mp4");
    m_manager->addPaths(id, {a, b, c});
    ASSERT_EQ(m_manager->playlist(id).entries.size(), 3);

    // Out-of-range rows are ignored.
    EXPECT_TRUE(m_manager->removeEntries(id, {1, 99}));

    const auto entries = m_manager->playlist(id).entries;
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(clean(entries[0].path), clean(a));
    EXPECT_EQ(clean(entries[1].path), clean(c));

    EXPECT_FALSE(m_manager->removeEntries(id, {}));
}

TEST_F(PlaylistManagerTest, moveEntry)
{
    const QString id = m_manager->createPlaylist();
    const QString a = createFile("a.mp4");
    const QString b = createFile("b.mp4");
    m_manager->addPaths(id, {a, b});

    EXPECT_TRUE(m_manager->moveEntry(id, 0, 1));

    const auto entries = m_manager->playlist(id).entries;
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(clean(entries[0].path), clean(b));
    EXPECT_EQ(clean(entries[1].path), clean(a));

    EXPECT_FALSE(m_manager->moveEntry(id, 0, 0));
    EXPECT_FALSE(m_manager->moveEntry(id, 5, 0));
}

TEST_F(PlaylistManagerTest, clearAndSetEntries)
{
    const QString id = m_manager->createPlaylist();
    m_manager->addPaths(id, {createFile("a.mp4")});

    const QList<PlaylistEntry> entries{{"C:/x.mp4", false}, {"C:/dir", true}};
    EXPECT_TRUE(m_manager->setEntries(id, entries));
    EXPECT_EQ(m_manager->playlist(id).entries, entries);

    // Setting the same entries again is a no-op.
    EXPECT_FALSE(m_manager->setEntries(id, entries));

    EXPECT_TRUE(m_manager->clearEntries(id));
    EXPECT_TRUE(m_manager->playlist(id).entries.isEmpty());
    EXPECT_FALSE(m_manager->clearEntries(id));
}

TEST_F(PlaylistManagerTest, expandedFilePaths)
{
    const QString root = mediaDir() + "/album";
    createFile("album/one.mp4");
    createFile("album/two.avi");
    createFile("album/nested/three.mkv");
    createFile("album/readme.txt"); // Unsupported extension.

    Playlist playlist;
    playlist.entries = {{clean(root), true}};

    auto paths = PlaylistManager::expandedFilePaths(playlist);
    std::sort(paths.begin(), paths.end());

    QStringList expected{
        clean(root + "/nested/three.mkv"),
        clean(root + "/one.mp4"),
        clean(root + "/two.avi"),
    };
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(paths, expected);
}

TEST_F(PlaylistManagerTest, persistsAcrossInstances)
{
    const QString id = m_manager->createPlaylist("Persisted");
    m_manager->addPaths(id, {createFile("persist.mp4")});
    m_manager->setCurrentPlaylistId(id);

    // Recreate the settings and the manager from the same directory.
    m_manager.reset();
    m_settings.reset();

    m_settings = std::make_unique<LocalSettings>(m_settingsPath);
    m_manager = std::make_unique<PlaylistManager>(m_settings.get());

    ASSERT_EQ(m_manager->playlists().size(), 1);
    EXPECT_EQ(m_manager->playlists().first().name, "Persisted");
    ASSERT_EQ(m_manager->playlists().first().entries.size(), 1);
    EXPECT_FALSE(m_manager->playlists().first().entries.first().isFolder);
    EXPECT_EQ(m_manager->currentPlaylistId(), id);
}

} // namespace test
} // namespace nx::vms::client::desktop
