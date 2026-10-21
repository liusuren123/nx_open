// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <core/resource/resource_fwd.h>
#include <nx/vms/client/desktop/window_context_aware.h>

#include "playlist.h"

class QnResourceWidget;

namespace nx::vms::client::desktop {

/**
 * QML-facing bridge that plays playlist entries in the workbench.
 *
 * Opening a media file is delegated to the regular "drop resources" action, so the whole existing
 * local file playback pipeline is reused. Network stream entries (IPTV channels) are opened through
 * the IPTV manager instead. The player keeps a flat, folder-expanded list of the entries of the
 * current playlist to implement the previous/next navigation.
 */
class PlaylistPlayer: public QObject, public WindowContextAware
{
    Q_OBJECT
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit PlaylistPlayer(WindowContext* context, QObject* parent = nullptr);

    /** Plays the file(s) or the stream behind the given playlist entry row. */
    Q_INVOKABLE void playEntry(const QString& playlistId, int row);

    Q_INVOKABLE void playNext();
    Q_INVOKABLE void playPrevious();

    /**
     * Selects the playlist to navigate in. Rebuilds the flat file list; the current position is
     * reset.
     */
    Q_INVOKABLE void setCurrentPlaylist(const QString& playlistId);

    /**
     * Why the last entry could not be played, or an empty string. Used for a stream address whose
     * signature has expired, which the user can only fix by scanning the page again.
     */
    QString lastError() const { return m_lastError; }

    static void registerQmlType();

signals:
    void lastErrorChanged();

private:
    /** Rebuilds the flat list of playable entries. */
    void rebuildEntries();
    /** Appends the given playlist entry, expanding folders into their media files. */
    void appendExpanded(const PlaylistEntry& entry);
    void openCurrentEntry();
    void setLastError(const QString& text);

    /**
     * Disables looping for the widget of the entry being played and makes it advance to the next
     * entry when playback finishes. Does nothing for widgets of other resources.
     */
    void hookWidget(QnResourceWidget* widget);

    QString m_playlistId;
    QList<PlaylistEntry> m_entries;
    int m_index = -1;
    QnResourcePtr m_currentResource;
    QString m_lastError;
};

} // namespace nx::vms::client::desktop
