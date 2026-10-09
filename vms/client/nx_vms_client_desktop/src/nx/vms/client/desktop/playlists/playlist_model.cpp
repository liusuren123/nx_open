// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "playlist_model.h"

#include <QtCore/QFileInfo>
#include <QtQml/qqml.h>

#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/playlists/playlist_manager.h>

namespace nx::vms::client::desktop {

PlaylistModel::PlaylistModel(QObject* parent):
    QAbstractListModel(parent)
{
    if (auto playlistManager = manager())
    {
        connect(playlistManager, &PlaylistManager::playlistChanged, this,
            [this](const QString& id)
            {
                if (id == m_playlistId)
                    refresh();
            });

        connect(playlistManager, &PlaylistManager::playlistsChanged,
            this, &PlaylistModel::refresh);
    }
}

PlaylistManager* PlaylistModel::manager() const
{
    return appContext()->playlistManager();
}

QString PlaylistModel::playlistId() const
{
    return m_playlistId;
}

void PlaylistModel::setPlaylistId(const QString& id)
{
    if (m_playlistId == id)
        return;

    m_playlistId = id;
    refresh();
    emit playlistIdChanged();
}

void PlaylistModel::refresh()
{
    beginResetModel();
    endResetModel();
}

int PlaylistModel::rowCount(const QModelIndex& parent) const
{
    const auto playlistManager = manager();
    if (parent.isValid() || !playlistManager || m_playlistId.isEmpty())
        return 0;

    return playlistManager->playlist(m_playlistId).entries.size();
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const
{
    const auto playlistManager = manager();
    if (!index.isValid() || !playlistManager)
        return {};

    const auto playlist = playlistManager->playlist(m_playlistId);
    if (index.row() < 0 || index.row() >= playlist.entries.size())
        return {};

    const auto& entry = playlist.entries.at(index.row());
    switch (role)
    {
        case FileNameRole:
            return QFileInfo(entry.path).fileName();

        case FilePathRole:
            return entry.path;

        case IsFolderRole:
            return entry.isFolder;

        default:
            return {};
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const
{
    return {
        {FileNameRole, "fileName"},
        {FilePathRole, "filePath"},
        {IsFolderRole, "isFolder"},
    };
}

void PlaylistModel::registerQmlType()
{
    qmlRegisterType<PlaylistModel>("nx.vms.client.desktop", 1, 0, "PlaylistModel");
}

} // namespace nx::vms::client::desktop
