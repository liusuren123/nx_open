// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "playlist_player.h"

#include <QtCore/QPointer>
#include <QtQml/qqml.h>

#include <core/resource/file_processor.h>
#include <core/resource/resource.h>
#include <core/resource_management/resource_pool.h>
#include <nx/streaming/abstract_archive_stream_reader.h>
#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/menu/action_manager.h>
#include <nx/vms/client/desktop/menu/actions.h>
#include <nx/vms/client/desktop/playlists/playlist_manager.h>
#include <nx/vms/client/desktop/system_context.h>
#include <nx/vms/client/desktop/workbench/workbench.h>
#include <camera/resource_display.h>
#include <ui/graphics/items/resource/media_resource_widget.h>
#include <ui/graphics/items/resource/resource_widget.h>
#include <ui/workbench/workbench_display.h>
#include <ui/workbench/workbench_layout.h>

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

    // Playlist entries are played one after another, so the widget of the entry being played must
    // be tracked to stop it from looping and to advance on the end of playback.
    if (const auto workbenchDisplay = display())
    {
        connect(workbenchDisplay, &QnWorkbenchDisplay::widgetAdded, this,
            [this](QnResourceWidget* widget) { hookWidget(widget); });
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

    m_currentResource = resources.first();

    // Playlist entries are added to the current layout, so that several of them can be watched at
    // the same time, exactly like resources opened from the resource tree.
    menu()->trigger(menu::DropResourcesAction, resources);

    // The widget may already exist, e.g. when the same file is opened again.
    if (const auto workbenchDisplay = display())
    {
        for (const auto widget: workbenchDisplay->widgets(m_currentResource))
            hookWidget(widget);
    }
}

void PlaylistPlayer::hookWidget(QnResourceWidget* widget)
{
    const auto resource = widget ? widget->resource() : QnResourcePtr();
    if (!resource || !m_currentResource || resource->getId() != m_currentResource->getId())
        return;

    const auto mediaWidget = qobject_cast<QnMediaResourceWidget*>(widget);
    if (!mediaWidget)
        return;

    const auto resourceDisplay = mediaWidget->display();
    if (!resourceDisplay)
        return;

    const auto reader = resourceDisplay->archiveReader();
    if (!reader)
        return;

    // Local files loop by default; a playlist entry is expected to end.
    reader->setCycleMode(false);

    // When a single playlist video is on the layout, playback advances to the next entry. When
    // several entries are shown at once (which is what opening them one after another does), every
    // video just stops at its end so that the layout does not keep growing.
    const QPointer<QnResourceWidget> widgetGuard(widget);
    reader->setEndOfPlaybackHandler(
        [this, widgetGuard]()
        {
            if (m_index + 1 >= static_cast<int>(m_files.size()))
                return; //< Nothing to advance to.

            const auto layout = workbench() ? workbench()->currentLayout() : nullptr;
            if (!layout || layout->items().size() > 1)
                return;

            if (widgetGuard)
            {
                if (const auto item = widgetGuard->item())
                {
                    if (layout->items().contains(item))
                        layout->removeItem(item);
                }
            }

            playNext();
        });
}

void PlaylistPlayer::registerQmlType()
{
    qmlRegisterUncreatableType<PlaylistPlayer>("nx.vms.client.desktop", 1, 0, "PlaylistPlayer",
        "PlaylistPlayer can be created from C++ code only.");
}

} // namespace nx::vms::client::desktop
