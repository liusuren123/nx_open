// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "playlist_manager.h"

#include <algorithm>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
}

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <core/resource/file_processor.h>
#include <platform/environment.h>
#include <nx/utils/property_storage/property.h>
#include <nx/vms/client/desktop/settings/local_settings.h>

namespace nx::vms::client::desktop {

namespace {

int indexOf(const QList<Playlist>& playlists, const nx::Uuid& id)
{
    for (int i = 0; i < playlists.size(); ++i)
    {
        if (playlists[i].id == id)
            return i;
    }
    return -1;
}

QString toId(const nx::Uuid& id)
{
    return id.isNull() ? QString() : id.toSimpleString();
}

nx::Uuid fromId(const QString& id)
{
    return id.isEmpty() ? nx::Uuid() : nx::Uuid::fromStringSafe(id);
}

QString normalizedPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

/**
 * Reads the duration of a media file with ffmpeg. Runs in a worker thread; does not decode any
 * frames, only the container header (and stream info when the header has no duration).
 */
qint64 probeMediaDurationMs(const QString& path)
{
    AVFormatContext* context = nullptr;
    const QByteArray fileName = path.toUtf8();
    if (avformat_open_input(&context, fileName.constData(), nullptr, nullptr) < 0)
        return -1;

    // Most containers expose the duration right from the header; read the streams only when it is
    // missing.
    if (context->duration == AV_NOPTS_VALUE || context->duration <= 0)
        avformat_find_stream_info(context, nullptr);

    qint64 result = -1;
    if (context->duration != AV_NOPTS_VALUE && context->duration > 0)
        result = context->duration / (AV_TIME_BASE / 1000);

    avformat_close_input(&context);
    return result;
}

} // namespace

PlaylistManager::PlaylistManager(LocalSettings* settings, QObject* parent):
    QObject(parent),
    m_settings(settings)
{
    if (m_settings)
    {
        connect(&m_settings->playlists, &nx::utils::property_storage::BaseProperty::changed,
            this, &PlaylistManager::playlistsChanged);
    }
}

QList<Playlist> PlaylistManager::playlists() const
{
    return m_settings ? m_settings->playlists() : QList<Playlist>();
}

Playlist PlaylistManager::playlist(const QString& id) const
{
    const auto list = playlists();
    const int index = indexOf(list, fromId(id));
    return index >= 0 ? list.at(index) : Playlist();
}

QString PlaylistManager::currentPlaylistId() const
{
    return m_settings ? toId(m_settings->currentPlaylistId()) : QString();
}

QString PlaylistManager::currentPlaylistName() const
{
    const QString id = currentPlaylistId();
    return id.isEmpty() ? QString() : playlist(id).name;
}

void PlaylistManager::setPlaylists(const QList<Playlist>& playlists)
{
    if (!m_settings)
        return;

    // Assignment emits Property::changed, which is re-emitted as playlistsChanged().
    m_settings->playlists = playlists;
}

void PlaylistManager::setCurrentPlaylistId(const QString& id)
{
    if (!m_settings)
        return;

    const nx::Uuid uuid = fromId(id);
    if (m_settings->currentPlaylistId() == uuid)
        return;

    m_settings->currentPlaylistId = uuid;
    emit currentPlaylistChanged();
}

QString PlaylistManager::createPlaylist(const QString& name)
{
    Playlist newPlaylist;
    newPlaylist.id = nx::Uuid::createUuid();
    newPlaylist.name = name.isEmpty() ? tr("New playlist") : name;

    auto list = playlists();
    list.append(newPlaylist);
    setPlaylists(list);

    if (currentPlaylistId().isEmpty())
        setCurrentPlaylistId(toId(newPlaylist.id));

    emit playlistChanged(toId(newPlaylist.id));
    return toId(newPlaylist.id);
}

bool PlaylistManager::removePlaylist(const QString& id)
{
    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0)
        return false;

    list.removeAt(index);
    setPlaylists(list);

    if (currentPlaylistId() == id)
    {
        if (list.isEmpty())
            setCurrentPlaylistId(QString());
        else
            setCurrentPlaylistId(toId(list.first().id));
    }

    emit playlistChanged(id);
    return true;
}

bool PlaylistManager::renamePlaylist(const QString& id, const QString& name)
{
    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0 || list[index].name == name)
        return false;

    list[index].name = name;
    setPlaylists(list);
    emit playlistChanged(id);
    return true;
}

bool PlaylistManager::addPaths(const QString& id, const QStringList& paths)
{
    QList<PlaylistEntry> entries;
    for (const QString& path: paths)
    {
        // Folders are expanded into the media files they contain (recursively), so that every video
        // gets its own playlist row, like in a conventional media player. For a single file this
        // yields just that file, and unsupported files are filtered out.
        for (const QString& file: QnFileProcessor::findAcceptedFiles(path))
            entries.append({normalizedPath(file), /*isFolder*/ false});
    }

    if (entries.isEmpty())
        return false;

    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0)
        return false;

    list[index].entries.append(entries);
    setPlaylists(list);
    emit playlistChanged(id);
    return true;
}

bool PlaylistManager::addFiles(const QString& id, const QList<QUrl>& urls)
{
    QStringList paths;
    for (const QUrl& url: urls)
    {
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        if (!path.isEmpty())
            paths.append(path);
    }
    return addPaths(id, paths);
}

QString PlaylistManager::ensureCurrentPlaylist(const QString& name)
{
    const QString id = currentPlaylistId();
    if (!id.isEmpty())
        return id;

    return createPlaylist(name.isEmpty() ? tr("Default") : name);
}

int PlaylistManager::addPathsToCurrentPlaylist(const QStringList& paths)
{
    const QString id = ensureCurrentPlaylist();
    if (id.isEmpty())
        return -1;

    const int firstRow = playlist(id).entries.size();
    if (!addPaths(id, paths))
        return -1;

    emit playbackRequested(id, firstRow);
    return firstRow;
}

int PlaylistManager::addFilesToCurrentPlaylist(const QList<QUrl>& urls)
{
    QStringList paths;
    for (const QUrl& url: urls)
    {
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        if (!path.isEmpty())
            paths.append(path);
    }
    return addPathsToCurrentPlaylist(paths);
}

bool PlaylistManager::sortEntriesByName(const QString& id)
{
    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0)
        return false;

    auto& entries = list[index].entries;
    std::stable_sort(entries.begin(), entries.end(),
        [](const PlaylistEntry& left, const PlaylistEntry& right)
        {
            return QString::compare(QFileInfo(left.path).fileName(),
                QFileInfo(right.path).fileName(), Qt::CaseInsensitive) < 0;
        });

    setPlaylists(list);
    emit playlistChanged(id);
    return true;
}

bool PlaylistManager::setEntries(const QString& id, const QList<PlaylistEntry>& entries)
{
    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0 || list[index].entries == entries)
        return false;

    list[index].entries = entries;
    setPlaylists(list);
    emit playlistChanged(id);
    return true;
}

bool PlaylistManager::removeEntries(const QString& id, const QList<int>& rows)
{
    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0)
        return false;

    auto& entries = list[index].entries;

    // Remove from the highest row to keep the remaining indices valid.
    QList<int> sortedRows = rows;
    std::sort(sortedRows.begin(), sortedRows.end(), std::greater<int>());

    bool removed = false;
    for (int row: sortedRows)
    {
        if (row >= 0 && row < entries.size())
        {
            entries.removeAt(row);
            removed = true;
        }
    }

    if (!removed)
        return false;

    setPlaylists(list);
    emit playlistChanged(id);
    return true;
}

bool PlaylistManager::moveEntry(const QString& id, int fromRow, int toRow)
{
    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0)
        return false;

    auto& entries = list[index].entries;
    if (fromRow < 0 || fromRow >= entries.size() || toRow < 0 || toRow >= entries.size())
        return false;

    if (fromRow == toRow)
        return false;

    entries.move(fromRow, toRow);
    setPlaylists(list);
    emit playlistChanged(id);
    return true;
}

bool PlaylistManager::clearEntries(const QString& id)
{
    auto list = playlists();
    const int index = indexOf(list, fromId(id));
    if (index < 0 || list[index].entries.isEmpty())
        return false;

    list[index].entries.clear();
    setPlaylists(list);
    emit playlistChanged(id);
    return true;
}

QStringList PlaylistManager::expandedFilePaths(const Playlist& playlist)
{
    QStringList result;
    for (const auto& entry: playlist.entries)
        result.append(QnFileProcessor::findAcceptedFiles(entry.path));
    return result;
}

qint64 PlaylistManager::durationMs(const QString& path) const
{
    if (path.isEmpty())
        return -1;

    const auto it = m_durations.constFind(path);
    if (it != m_durations.constEnd())
        return it.value();

    if (!m_pendingDurations.contains(path))
        requestDuration(path);

    return -1;
}

void PlaylistManager::requestDuration(const QString& path) const
{
    m_pendingDurations.insert(path);

    auto* const self = const_cast<PlaylistManager*>(this);
    const QPointer<PlaylistManager> guard(self);
    QtConcurrent::run([guard, path]()
    {
        const qint64 duration = probeMediaDurationMs(path);
        if (!guard)
            return;

        QMetaObject::invokeMethod(guard.data(),
            [guard, path, duration]()
            {
                if (guard)
                    guard->onDurationProbed(path, duration);
            },
            Qt::QueuedConnection);
    });
}

void PlaylistManager::onDurationProbed(const QString& path, qint64 duration)
{
    m_pendingDurations.remove(path);
    m_durations.insert(path, duration);
    emit durationChanged(path, duration);
}

QString PlaylistManager::entryPath(const QString& playlistId, int row) const
{
    const auto entries = playlist(playlistId).entries;
    return row >= 0 && row < static_cast<int>(entries.size())
        ? entries.at(row).path
        : QString();
}

void PlaylistManager::showInFileManager(const QString& path) const
{
    if (!path.isEmpty())
        QnEnvironment::showInGraphicalShell(path);
}

} // namespace nx::vms::client::desktop
