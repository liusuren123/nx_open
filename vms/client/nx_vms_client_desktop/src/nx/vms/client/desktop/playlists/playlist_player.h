// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <nx/vms/client/desktop/window_context_aware.h>

namespace nx::vms::client::desktop {

/**
 * QML-facing bridge that plays playlist entries in the workbench.
 *
 * Opening a media file is delegated to the regular "drop resources" action, so the whole existing
 * local file playback pipeline is reused. The player keeps a flat, folder-expanded list of files of
 * the current playlist to implement the previous/next navigation.
 */
class PlaylistPlayer: public QObject, public WindowContextAware
{
    Q_OBJECT

public:
    explicit PlaylistPlayer(WindowContext* context, QObject* parent = nullptr);

    /** Plays the file(s) behind the given playlist entry row. */
    Q_INVOKABLE void playEntry(const QString& playlistId, int row);

    Q_INVOKABLE void playNext();
    Q_INVOKABLE void playPrevious();

    /**
     * Selects the playlist to navigate in. Rebuilds the flat file list; the current position is
     * reset.
     */
    Q_INVOKABLE void setCurrentPlaylist(const QString& playlistId);

    static void registerQmlType();

private:
    void rebuildFiles();
    void openCurrentFile();

    QString m_playlistId;
    QStringList m_files;
    int m_index = -1;
};

} // namespace nx::vms::client::desktop
