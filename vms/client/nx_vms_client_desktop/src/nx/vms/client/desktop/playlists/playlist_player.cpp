// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "playlist_player.h"

#include <QtQml/qqml.h>

#include <core/resource/file_processor.h>
#include <core/resource_management/resource_pool.h>
#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/menu/action_manager.h>
#include <nx/vms/client/desktop/menu/actions.h>
#include <nx/vms/client/desktop/playlists/playlist_manager.h>
#include <nx/vms/client/desktop/system_context.h>

namespace nx::vms::client::desktop {

namespace {

PlaylistManager* playlistManager()
{
    return appContext()->playlistManager();
}

} // namespace

PlaylistPlayer::PlaylistPlayer(WindowContext* context, QObject* parent):
    QObject(parent),
    WindowContextAware(context)
{
    // Start playback when entries are added to the current playlist (e.g. "open file/folder").
    if (const auto manager = playlistManager())
    {
        connect(manager, &PlaylistManager::playbackRequested, this,
            [this](const QString& playlistId, int row) { playEntry(playlistId, row); });
    }
}

void PlaylistPlayer::setCurrentPlaylist(const QString& playlistId)
{
    m_playlistId = playlistId;
    m_index = -1;
    rebuildFiles();
}

void PlaylistPlayer::rebuildFiles()
{
    m_files.clear();

    const auto manager = playlistManager();
    if (!manager || m_playlistId.isEmpty())
        return;

    const auto playlist = manager->playlist(m_playlistId);
    for (const auto& entry: playlist.entries)
        m_files.append(QnFileProcessor::findAcceptedFiles(entry.path));
}

void PlaylistPlayer::playEntry(const QString& playlistId, int row)
{
    const auto manager = playlistManager();
    if (!manager || row < 0)
        return;

    const auto playlist = manager->playlist(playlistId);
    if (row >= static_cast<int>(playlist.entries.size()))
        return;

    m_playlistId = playlistId;

    // Build the flat file list, remembering the position of the requested entry.
    m_files.clear();
    int startIndex = -1;
    for (int i = 0; i < static_cast<int>(playlist.entries.size()); ++i)
    {
        if (i == row)
            startIndex = static_cast<int>(m_files.size());
        m_files.append(QnFileProcessor::findAcceptedFiles(playlist.entries[i].path));
    }

    if (startIndex < 0 || startIndex >= static_cast<int>(m_files.size()))
        return;

    m_index = startIndex;
    openCurrentFile();
}

void PlaylistPlayer::playNext()
{
    if (m_index + 1 < static_cast<int>(m_files.size()))
    {
        ++m_index;
        openCurrentFile();
    }
}

void PlaylistPlayer::playPrevious()
{
    if (m_index > 0)
    {
        --m_index;
        openCurrentFile();
    }
}

void PlaylistPlayer::openCurrentFile()
{
    if (m_index < 0 || m_index >= static_cast<int>(m_files.size()))
        return;

    const auto resources = QnFileProcessor::createResourcesForFiles(
        {m_files.at(m_index)}, system()->resourcePool());
    if (resources.isEmpty())
        return;

    menu()->trigger(menu::DropResourcesAction, resources);
}

void PlaylistPlayer::registerQmlType()
{
    qmlRegisterUncreatableType<PlaylistPlayer>("nx.vms.client.desktop", 1, 0, "PlaylistPlayer",
        "PlaylistPlayer can be created from C++ code only.");
}

} // namespace nx::vms::client::desktop
