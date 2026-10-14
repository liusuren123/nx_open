// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "iptv_manager.h"

#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <core/resource_management/resource_pool.h>
#include <nx/utils/log/log.h>
#include <nx/utils/property_storage/property.h>
#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/menu/action_manager.h>
#include <nx/vms/client/desktop/menu/action_parameters.h>
#include <nx/vms/client/desktop/menu/actions.h>
#include <nx/vms/client/desktop/settings/local_settings.h>
#include <nx/vms/client/desktop/system_context.h>
#include <nx/vms/client/desktop/window_context.h>

#include "../resource/local_camera/local_camera_resource.h"
#include "../resource/local_camera/local_camera_source.h"
#include "m3u_parser.h"

namespace nx::vms::client::desktop {

namespace {

constexpr int kDownloadTimeoutMs = 20000;

bool isRemoteSource(const QString& source)
{
    return source.startsWith("http://", Qt::CaseInsensitive)
        || source.startsWith("https://", Qt::CaseInsensitive);
}

} // namespace

IptvManager::IptvManager(LocalSettings* settings, QObject* parent):
    QObject(parent),
    m_settings(settings)
{
    if (!m_settings)
        return;

    connect(&m_settings->iptvPlaylists,
        &nx::utils::property_storage::BaseProperty::changed,
        this,
        [this]() { emit playlistsChanged(); });
}

IptvManager::~IptvManager() = default;

bool IptvManager::importing() const
{
    return m_importing;
}

QVariantList IptvManager::playlists() const
{
    QVariantList result;
    if (!m_settings)
        return result;

    for (const IptvPlaylist& playlist: m_settings->iptvPlaylists())
    {
        result.append(QVariantMap{
            {"id", playlist.id.toString()},
            {"name", playlist.name},
            {"source", playlist.source},
            {"channelCount", playlist.channels.size()}});
    }

    return result;
}

QVariantList IptvManager::channels(const QString& playlistId) const
{
    QVariantList result;
    const auto playlist = findPlaylist(nx::Uuid::fromStringSafe(playlistId));
    if (!playlist)
        return result;

    for (const IptvChannel& channel: playlist->channels)
    {
        result.append(QVariantMap{
            {"id", channel.id.toString()},
            {"name", channel.name},
            {"group", channel.group},
            {"logo", channel.logo}});
    }

    return result;
}

QString IptvManager::lastError() const
{
    return m_lastError;
}

bool IptvManager::importFromUrl(const QString& url, const QString& name)
{
    return startImport(url.trimmed(), name, nx::Uuid());
}

bool IptvManager::importFromFile(const QString& path, const QString& name)
{
    return startImport(path.trimmed(), name, nx::Uuid());
}

bool IptvManager::reloadPlaylist(const QString& id)
{
    const auto playlist = findPlaylist(nx::Uuid::fromStringSafe(id));
    if (!playlist)
        return false;

    return startImport(playlist->source, playlist->name, playlist->id);
}

bool IptvManager::removePlaylist(const QString& id)
{
    if (!m_settings)
        return false;

    const nx::Uuid uuid = nx::Uuid::fromStringSafe(id);
    if (uuid.isNull())
        return false;

    auto list = m_settings->iptvPlaylists();
    for (int i = 0; i < list.size(); ++i)
    {
        if (list[i].id != uuid)
            continue;

        list.removeAt(i);
        m_settings->iptvPlaylists = list;
        return true;
    }

    return false;
}

bool IptvManager::openChannel(const QString& playlistId, const QString& channelId)
{
    const auto playlist = findPlaylist(nx::Uuid::fromStringSafe(playlistId));
    if (!playlist)
        return false;

    const nx::Uuid channelUuid = nx::Uuid::fromStringSafe(channelId);
    const IptvChannel* channel = nullptr;
    for (const IptvChannel& candidate: playlist->channels)
    {
        if (candidate.id == channelUuid)
        {
            channel = &candidate;
            break;
        }
    }

    if (!channel || channel->url.isEmpty())
        return false;

    auto systemContext = appContext()->currentSystemContext();
    auto pool = systemContext ? systemContext->resourcePool() : nullptr;
    if (!pool)
        return false;

    // A resource is created on demand and kept, so re-opening a channel is cheap. The rest of the
    // playlist is never materialized.
    QnResourcePtr resource = pool->getResourceById(channel->id);
    if (!resource)
    {
        LocalCameraSource source;
        source.id = channel->id;
        source.name = channel->group.isEmpty()
            ? channel->name
            : QString("%1 \u00b7 %2").arg(channel->group, channel->name); //< Group separator dot.
        source.url = channel->url;
        source.username = channel->username;
        source.password = channel->password;

        resource = QnResourcePtr(new LocalCameraResource(source));
        pool->addResources(QnResourceList{resource});
    }

    auto windowContext = appContext()->mainWindowContext();
    if (!windowContext || !windowContext->menu())
        return false;

    windowContext->menu()->trigger(menu::OpenInCurrentLayoutAction, menu::Parameters(resource));
    return true;
}

bool IptvManager::startImport(
    const QString& source, const QString& name, const nx::Uuid& replaceId)
{
    if (!m_settings)
        return false;

    m_lastError.clear();

    if (source.isEmpty())
    {
        m_lastError = tr("No playlist address given.");
        return false;
    }

    if (m_importing)
    {
        m_lastError = tr("Another playlist is already being imported.");
        return false;
    }

    m_pendingSource = source;
    m_pendingName = name;
    m_pendingReplaceId = replaceId;

    if (!isRemoteSource(source))
    {
        // A local file is cheap to read, so it is handled inline.
        QFile file(source);
        if (!file.open(QIODevice::ReadOnly))
        {
            m_lastError = tr("Failed to read the playlist from '%1'.").arg(source);
            return false;
        }

        setImporting(true);
        const bool ok = finishImport(file.readAll());
        setImporting(false);
        return ok;
    }

    setImporting(true);
    m_pendingReply = m_network.get(QNetworkRequest(QUrl(source)));
    connect(m_pendingReply, &QNetworkReply::finished, this, &IptvManager::onDownloadFinished);

    if (!m_timeout)
    {
        m_timeout = new QTimer(this);
        m_timeout->setSingleShot(true);
        connect(m_timeout, &QTimer::timeout, this, &IptvManager::onImportTimeout);
    }
    m_timeout->start(kDownloadTimeoutMs);

    return true;
}

bool IptvManager::finishImport(const QByteArray& data)
{
    QString parseError;
    const QList<IptvChannel> channels = parseM3uPlaylist(data, &parseError);
    if (channels.isEmpty())
    {
        m_lastError = parseError.isEmpty() ? tr("The playlist contains no channels.") : parseError;
        NX_WARNING(this, "Failed to import playlist '%1': %2", m_pendingSource, m_lastError);
        emit importFinished(false);
        return false;
    }

    IptvPlaylist playlist;
    playlist.id = m_pendingReplaceId.isNull() ? nx::Uuid::createUuid() : m_pendingReplaceId;
    playlist.name = m_pendingName.trimmed().isEmpty()
        ? m_pendingSource
        : m_pendingName.trimmed();
    playlist.source = m_pendingSource;
    playlist.channels = channels;

    NX_INFO(typeid(IptvManager), "Imported playlist '%1' from '%2': %3 channel(s)",
        playlist.name, m_pendingSource, channels.size());

    storePlaylist(playlist);
    emit importFinished(true);
    return true;
}

void IptvManager::onDownloadFinished()
{
    if (m_timeout)
        m_timeout->stop();

    QNetworkReply* reply = m_pendingReply;
    m_pendingReply = nullptr;
    if (!reply)
        return;

    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString = reply->errorString();
    const QByteArray data = (error == QNetworkReply::NoError) ? reply->readAll() : QByteArray();
    reply->deleteLater();

    if (error != QNetworkReply::NoError)
    {
        m_lastError = tr("Failed to read the playlist from '%1' (%2).")
            .arg(m_pendingSource, errorString);
        NX_WARNING(this, "Failed to download playlist '%1': %2", m_pendingSource, m_lastError);
        setImporting(false);
        emit importFinished(false);
        return;
    }

    finishImport(data);
    setImporting(false);
}

void IptvManager::onImportTimeout()
{
    if (m_pendingReply)
        m_pendingReply->abort(); //< Emits finished() and lands in onDownloadFinished().
}

void IptvManager::setImporting(bool value)
{
    if (m_importing == value)
        return;

    m_importing = value;
    emit importingChanged();
}

std::optional<IptvPlaylist> IptvManager::findPlaylist(const nx::Uuid& id) const
{
    if (!m_settings || id.isNull())
        return std::nullopt;

    // The settings property returns its value by value, so a copy is returned to the caller rather
    // than a pointer into this temporary list.
    for (const IptvPlaylist& playlist: m_settings->iptvPlaylists())
    {
        if (playlist.id == id)
            return playlist;
    }

    return std::nullopt;
}

bool IptvManager::storePlaylist(IptvPlaylist playlist)
{
    if (!m_settings)
        return false;

    auto list = m_settings->iptvPlaylists();
    bool replaced = false;
    for (int i = 0; i < list.size(); ++i)
    {
        if (list[i].id == playlist.id)
        {
            list[i] = playlist;
            replaced = true;
            break;
        }
    }

    if (!replaced)
        list.append(playlist);

    m_settings->iptvPlaylists = list; //< Emits changed => playlistsChanged().
    return true;
}

} // namespace nx::vms::client::desktop
