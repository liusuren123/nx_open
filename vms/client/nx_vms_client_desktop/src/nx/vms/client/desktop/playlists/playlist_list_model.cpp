// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "playlist_list_model.h"

#include <QtQml/qqml.h>

#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/playlists/playlist_manager.h>

namespace nx::vms::client::desktop {

PlaylistListModel::PlaylistListModel(QObject* parent):
    QAbstractListModel(parent)
{
    if (auto playlistManager = manager())
    {
        connect(playlistManager, &PlaylistManager::playlistsChanged,
            this, &PlaylistListModel::resetModel);
    }
}

PlaylistManager* PlaylistListModel::manager() const
{
    return appContext()->playlistManager();
}

void PlaylistListModel::resetModel()
{
    beginResetModel();
    endResetModel();
}

int PlaylistListModel::rowCount(const QModelIndex& parent) const
{
    const auto playlistManager = manager();
    return (!parent.isValid() && playlistManager) ? playlistManager->playlists().size() : 0;
}

QVariant PlaylistListModel::data(const QModelIndex& index, int role) const
{
    const auto playlistManager = manager();
    if (!index.isValid() || !playlistManager)
        return {};

    const auto list = playlistManager->playlists();
    if (index.row() < 0 || index.row() >= list.size())
        return {};

    const auto& playlist = list.at(index.row());
    switch (role)
    {
        case IdRole:
            return playlist.id.isNull() ? QString() : playlist.id.toSimpleString();

        case NameRole:
            return playlist.name;

        case EntriesCountRole:
            return playlist.entries.size();

        default:
            return {};
    }
}

QHash<int, QByteArray> PlaylistListModel::roleNames() const
{
    return {
        {IdRole, "playlistId"},
        {NameRole, "name"},
        {EntriesCountRole, "entriesCount"},
    };
}

void PlaylistListModel::registerQmlType()
{
    qmlRegisterType<PlaylistListModel>(
        "nx.vms.client.desktop", 1, 0, "PlaylistListModel");
}

} // namespace nx::vms::client::desktop
