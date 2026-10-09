// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QAbstractListModel>

namespace nx::vms::client::desktop {

class PlaylistManager;

/**
 * Lists all local playlists. Used to build the playlist selector of the local player sidebar.
 */
class PlaylistListModel: public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role
    {
        IdRole = Qt::UserRole + 1,
        NameRole,
        EntriesCountRole,
    };
    Q_ENUM(Role)

    explicit PlaylistListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    static void registerQmlType();

private:
    PlaylistManager* manager() const;
    void resetModel();
};

} // namespace nx::vms::client::desktop
