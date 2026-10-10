// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QString>

namespace nx::vms::client::desktop {

class PlaylistManager;

/**
 * Lists the entries of a single playlist identified by its string id. Used to build the media list
 * of the local player sidebar.
 */
class PlaylistModel: public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString playlistId READ playlistId WRITE setPlaylistId NOTIFY playlistIdChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    enum Role
    {
        FileNameRole = Qt::UserRole + 1,
        FilePathRole,
        IsFolderRole,
        DurationMsRole,
    };
    Q_ENUM(Role)

    explicit PlaylistModel(QObject* parent = nullptr);

    QString playlistId() const;
    void setPlaylistId(const QString& id);

    QString filter() const;
    void setFilter(const QString& filter);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    static void registerQmlType();

signals:
    void playlistIdChanged();
    void filterChanged();

private:
    PlaylistManager* manager() const;
    void refresh();
    void rebuildFilteredRows();
    void onDurationChanged(const QString& path);

    QString m_playlistId;
    QString m_filter;
    QList<int> m_filteredRows;
};

} // namespace nx::vms::client::desktop
