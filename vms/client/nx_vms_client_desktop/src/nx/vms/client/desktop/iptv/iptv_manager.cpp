// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "iptv_manager.h"

#include <chrono>

#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QVariantMap>
#include <QtGui/QKeyEvent>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuickWidgets/QQuickWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsView>

#include <core/resource_management/resource_pool.h>
#include <nx/utils/log/log.h>
#include <nx/utils/property_storage/property.h>
#include <nx/vms/client/desktop/application_context.h>
#include <nx/vms/client/desktop/menu/action_manager.h>
#include <nx/vms/client/desktop/menu/action_parameters.h>
#include <nx/vms/client/desktop/menu/actions.h>
#include <nx/vms/client/desktop/playlists/playlist_manager.h>
#include <nx/vms/client/desktop/settings/local_settings.h>
#include <nx/vms/client/desktop/system_context.h>
#include <nx/vms/client/desktop/ui/scene/widgets/scene_banners.h>
#include <nx/vms/client/desktop/window_context.h>

#include "../resource/local_camera/local_camera_resource.h"
#include "../resource/local_camera/local_camera_source.h"
#include "m3u_parser.h"

namespace nx::vms::client::desktop {

namespace {

constexpr int kDownloadTimeoutMs = 20000;

/** Pause after the last typed digit before the channel is switched, in ms. */
constexpr int kNumberCommitMs = 1500;

/** How long the current channel is shown over the video after it changes, in ms. */
constexpr int kChannelBannerMs = 3500;

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
    m_numberTimeout = new QTimer(this);
    m_numberTimeout->setSingleShot(true);
    m_numberTimeout->setInterval(kNumberCommitMs);
    connect(m_numberTimeout, &QTimer::timeout, this, &IptvManager::commitTypedNumber);

    // The video area is a native widget, so pressing keys while watching never reaches the QML
    // scene; channel hotkeys are therefore handled for the whole application.
    qApp->installEventFilter(this);

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

std::optional<IptvChannel> IptvManager::currentChannel() const
{
    const auto playlist = findPlaylist(m_currentPlaylistId);
    if (!playlist || m_currentChannelIndex < 0 || m_currentChannelIndex >= playlist->channels.size())
        return std::nullopt;

    return playlist->channels[m_currentChannelIndex];
}

bool IptvManager::hasCurrentChannel() const
{
    return currentChannel().has_value();
}

QString IptvManager::currentChannelName() const
{
    const auto channel = currentChannel();
    return channel ? channel->name : QString();
}

QString IptvManager::currentChannelGroup() const
{
    const auto channel = currentChannel();
    return channel ? channel->group : QString();
}

QString IptvManager::currentChannelLogo() const
{
    const auto channel = currentChannel();
    return channel ? channel->logo : QString();
}

int IptvManager::currentChannelNumber() const
{
    return currentChannel() ? m_currentChannelIndex + 1 : 0;
}

int IptvManager::currentChannelCount() const
{
    const auto playlist = findPlaylist(m_currentPlaylistId);
    return playlist ? playlist->channels.size() : 0;
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
    return startImport(url.trimmed(), name, nx::Uuid(), ImportTarget::StorePlaylist);
}

bool IptvManager::importFromFile(const QString& path, const QString& name)
{
    return startImport(path.trimmed(), name, nx::Uuid(), ImportTarget::StorePlaylist);
}

bool IptvManager::addSourceToCurrentPlaylist(const QString& source, const QString& name)
{
    const QString trimmed = source.trimmed();
    m_lastError.clear();

    if (trimmed.isEmpty())
    {
        m_lastError = tr("No playlist address given.");
        return false;
    }

    // Anything with a non-http(s) scheme is a single stream (rtsp, rtmp, udp, ...): there is no
    // playlist to download, so it is added as one entry right away.
    if (trimmed.contains("://") && !isRemoteSource(trimmed))
    {
        const bool added = addStreamToCurrentPlaylist(trimmed, name.trimmed());
        emit importFinished(added);
        return added;
    }

    return startImport(trimmed, name, nx::Uuid(), ImportTarget::CurrentPlaylist);
}

QString IptvManager::streamTitle(const QString& url)
{
    const QUrl parsed(url);

    // Proxy URLs carry the real file name in a query parameter, as in ".../proxy?path=a%2Fb.m3u8".
    const auto items = QUrlQuery(parsed).queryItems(QUrl::FullyDecoded);
    for (const auto& item: items)
    {
        const QString& key = item.first;
        if (key.compare("path", Qt::CaseInsensitive) != 0
            && key.compare("url", Qt::CaseInsensitive) != 0
            && key.compare("file", Qt::CaseInsensitive) != 0)
        {
            continue;
        }

        const QString name = item.second.section('/', -1);
        if (!name.isEmpty())
            return name;
    }

    const QString name = parsed.path().section('/', -1);
    return name.isEmpty() ? url : name;
}

bool IptvManager::addStreamToCurrentPlaylist(const QString& url, const QString& title)
{
    const auto playlistManager = appContext()->playlistManager();
    if (!playlistManager)
    {
        m_lastError = tr("The playlist is not available.");
        return false;
    }

    PlaylistEntry entry;
    entry.url = url;
    entry.title = title.isEmpty() ? streamTitle(url) : title;

    const QString id = playlistManager->ensureCurrentPlaylist();
    if (id.isEmpty() || !playlistManager->addEntries(id, {entry}))
    {
        m_lastError = tr("Failed to add the stream to the playlist.");
        return false;
    }

    NX_INFO(typeid(IptvManager), "Added stream '%1' to the current playlist", url);
    return true;
}

bool IptvManager::reloadPlaylist(const QString& id)
{
    const auto playlist = findPlaylist(nx::Uuid::fromStringSafe(id));
    if (!playlist)
        return false;

    return startImport(playlist->source, playlist->name, playlist->id, ImportTarget::StorePlaylist);
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
    for (int i = 0; i < playlist->channels.size(); ++i)
    {
        if (playlist->channels[i].id == channelUuid)
            return openChannelAt(*playlist, i);
    }

    return false;
}

bool IptvManager::playNext()
{
    const auto playlist = findPlaylist(m_currentPlaylistId);
    if (!playlist || playlist->channels.isEmpty())
        return false;

    const int count = playlist->channels.size();
    const int next = m_currentChannelIndex < 0 ? 0 : (m_currentChannelIndex + 1) % count;
    return openChannelAt(*playlist, next);
}

bool IptvManager::playPrevious()
{
    const auto playlist = findPlaylist(m_currentPlaylistId);
    if (!playlist || playlist->channels.isEmpty())
        return false;

    const int count = playlist->channels.size();
    const int base = m_currentChannelIndex < 0 ? 0 : m_currentChannelIndex;
    return openChannelAt(*playlist, (base - 1 + count) % count);
}

bool IptvManager::playChannelAt(int index)
{
    const auto playlist = findPlaylist(m_currentPlaylistId);
    if (!playlist || playlist->channels.isEmpty())
        return false;

    const int count = playlist->channels.size();
    return openChannelAt(*playlist, ((index % count) + count) % count);
}

bool IptvManager::playChannelNumber(int number)
{
    const auto playlist = findPlaylist(m_currentPlaylistId);
    if (!playlist || number < 1 || number > playlist->channels.size())
        return false;

    return openChannelAt(*playlist, number - 1);
}

void IptvManager::clearCurrentChannel()
{
    if (m_currentChannelIndex < 0)
        return;

    m_currentChannelIndex = -1;
    emit currentChannelChanged();
    showBanner(QString(), 0);
}

QString IptvManager::channelBannerText() const
{
    const auto channel = currentChannel();
    if (!channel)
        return QString();

    QStringList parts{channel->name};
    if (!channel->group.isEmpty())
        parts.append(channel->group);
    if (currentChannelNumber() > 0 && currentChannelCount() > 0)
        parts.append(QString("%1 / %2").arg(currentChannelNumber()).arg(currentChannelCount()));

    return parts.join(QStringLiteral("   \u00b7   "));
}

void IptvManager::showBanner(const QString& text, int timeoutMs)
{
    // The banner is drawn over the workbench by the client's own always-on-top overlay widget.
    if (!SceneBanners::instance())
        return;

    if (m_banner)
    {
        m_banner->hide(/*immediately*/ true);
        m_banner = nullptr;
    }

    if (text.isEmpty())
        return;

    const auto timeout = timeoutMs > 0
        ? std::optional(std::chrono::milliseconds(timeoutMs))
        : std::nullopt;
    m_banner = SceneBanner::show(text, timeout);
}

QString IptvManager::typedChannelNumber() const
{
    return m_typedNumber;
}

void IptvManager::typeChannelDigit(const QString& digit)
{
    if (digit.isEmpty())
        return;

    m_typedNumber = m_typedNumber.length() < 3 ? m_typedNumber + digit : digit;
    m_numberTimeout->start();
    emit typedChannelNumberChanged();
    showBanner(m_typedNumber, 0);
}

void IptvManager::clearTypedNumber()
{
    if (m_typedNumber.isEmpty())
        return;

    m_typedNumber.clear();
    m_numberTimeout->stop();
    emit typedChannelNumberChanged();
    showBanner(channelBannerText(), kChannelBannerMs);
}

void IptvManager::commitTypedNumber()
{
    if (m_typedNumber.isEmpty())
        return;

    const int number = m_typedNumber.toInt();
    clearTypedNumber();
    playChannelNumber(number);
}

bool IptvManager::isTextInputFocused()
{
    QWidget* focus = QApplication::focusWidget();
    if (!focus)
        return false;

    const QByteArray widgetClass = focus->metaObject()->className();
    if (widgetClass.contains("QLineEdit") || widgetClass.contains("QTextEdit")
        || widgetClass.contains("QPlainTextEdit") || widgetClass.contains("QAbstractSpinBox"))
    {
        return true;
    }

    // Text fields of the QML panels live inside a QQuickWidget.
    if (auto* quickWidget = qobject_cast<QQuickWidget*>(focus))
    {
        QQuickItem* item = quickWidget->quickWindow()
            ? quickWidget->quickWindow()->activeFocusItem()
            : nullptr;
        for (; item; item = item->parentItem())
        {
            const QByteArray itemClass = item->metaObject()->className();
            if (itemClass.contains("TextInput") || itemClass.contains("TextEdit")
                || itemClass.contains("TextField") || itemClass.contains("TextArea"))
            {
                return true;
            }
        }
    }

    return false;
}

bool IptvManager::isVideoAreaFocused()
{
    QWidget* focus = QApplication::focusWidget();
    const auto windowContext = appContext()->mainWindowContext();
    QWidget* mainWindow = windowContext ? windowContext->mainWindowWidget() : nullptr;
    if (!focus || !mainWindow)
        return false;

    // The workbench scene is the only graphics view inside the main window. The side panels are
    // child widgets of it, so merely being a descendant does not count as focusing the video area.
    QGraphicsView* sceneView = mainWindow->findChild<QGraphicsView*>();
    if (!sceneView)
        return false;

    if (focus == sceneView || focus == sceneView->viewport())
        return true;

    // The scene itself is drawn by a QQuickWidget placed inside the viewport.
    QWidget* sceneItem = sceneView->findChild<QWidget*>(QStringLiteral("Scene"));
    return sceneItem && focus == sceneItem;
}

bool IptvManager::isMainWindowActive()
{
    const auto windowContext = appContext()->mainWindowContext();
    QWidget* mainWindow = windowContext ? windowContext->mainWindowWidget() : nullptr;
    QWidget* activeWindow = QApplication::activeWindow();
    return mainWindow && activeWindow && mainWindow->window() == activeWindow->window();
}

bool IptvManager::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress || !hasCurrentChannel() || !isMainWindowActive())
        return QObject::eventFilter(watched, event);

    auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return QObject::eventFilter(watched, event);

    switch (keyEvent->key())
    {
        case Qt::Key_PageDown:
            playNext();
            return true;

        case Qt::Key_PageUp:
            playPrevious();
            return true;

        // The arrow keys are shared with list navigation, so they switch channels only while the
        // video area itself has the focus.
        case Qt::Key_Down:
            if (!isVideoAreaFocused())
                break;
            playNext();
            return true;

        case Qt::Key_Up:
            if (!isVideoAreaFocused())
                break;
            playPrevious();
            return true;

        case Qt::Key_Backspace:
            if (m_typedNumber.isEmpty())
                break;
            clearTypedNumber();
            return true;

        default:
            if (keyEvent->key() >= Qt::Key_0 && keyEvent->key() <= Qt::Key_9
                && !isTextInputFocused())
            {
                typeChannelDigit(QString::number(keyEvent->key() - Qt::Key_0));
                return true;
            }
            break;
    }

    return QObject::eventFilter(watched, event);
}

bool IptvManager::openChannelAt(const IptvPlaylist& playlist, int index)
{
    if (index < 0 || index >= playlist.channels.size())
        return false;

    const IptvChannel& channel = playlist.channels[index];

    LocalCameraSource source;
    source.id = channel.id;
    source.name = channel.group.isEmpty()
        ? channel.name
        : QString("%1 \u00b7 %2").arg(channel.group, channel.name); //< Group separator dot.
    source.url = channel.url;
    source.username = channel.username;
    source.password = channel.password;

    const QnResourcePtr resource = ensureStreamResource(source);
    if (!resource)
        return false;

    auto windowContext = appContext()->mainWindowContext();
    if (!windowContext || !windowContext->menu())
        return false;

    // Remember what is playing so that next/previous and the on-screen display keep working.
    m_currentPlaylistId = playlist.id;
    m_currentChannelIndex = index;
    emit currentChannelChanged();
    showBanner(channelBannerText(), kChannelBannerMs);

    windowContext->menu()->trigger(menu::OpenInCurrentLayoutAction, menu::Parameters(resource));
    return true;
}

QnResourcePtr IptvManager::ensureStreamResource(const LocalCameraSource& source)
{
    if (source.url.isEmpty())
        return {};

    auto systemContext = appContext()->currentSystemContext();
    auto pool = systemContext ? systemContext->resourcePool() : nullptr;
    if (!pool)
        return {};

    // A resource is created on demand and kept, so re-opening a stream is cheap.
    if (const QnResourcePtr existing = pool->getResourceById(source.id))
        return existing;

    QnResourcePtr resource(new LocalCameraResource(source));
    pool->addResources(QnResourceList{resource});
    return resource;
}

nx::Uuid IptvManager::streamResourceId(const QString& url)
{
    const auto it = m_streamIds.constFind(url);
    if (it != m_streamIds.constEnd())
        return it.value();

    const nx::Uuid id = nx::Uuid::createUuid();
    m_streamIds.insert(url, id);
    return id;
}

bool IptvManager::openStream(const QString& url, const QString& title,
    const QString& username, const QString& password)
{
    if (url.isEmpty())
        return false;

    LocalCameraSource source;
    source.id = streamResourceId(url);
    source.name = title.isEmpty() ? url : title;
    source.url = url;
    source.username = username;
    source.password = password;

    const QnResourcePtr resource = ensureStreamResource(source);
    if (!resource)
        return false;

    auto windowContext = appContext()->mainWindowContext();
    if (!windowContext || !windowContext->menu())
        return false;

    windowContext->menu()->trigger(menu::OpenInCurrentLayoutAction, menu::Parameters(resource));
    return true;
}

bool IptvManager::startImport(
    const QString& source, const QString& name, const nx::Uuid& replaceId, ImportTarget target)
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
    m_pendingTarget = target;

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
    const QString baseUrl = isRemoteSource(m_pendingSource) ? m_pendingSource : QString();
    const QList<IptvChannel> channels = parseM3uPlaylist(data, baseUrl, &parseError);
    if (channels.isEmpty())
    {
        // An http(s) source that is not an M3U channel list is a single stream: an HLS media
        // playlist, for instance, only contains segment URLs. Add it as one entry rather than fail.
        if (m_pendingTarget == ImportTarget::CurrentPlaylist && isRemoteSource(m_pendingSource))
        {
            const bool added =
                addStreamToCurrentPlaylist(m_pendingSource, m_pendingName.trimmed());
            emit importFinished(added);
            return added;
        }

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

    if (m_pendingTarget == ImportTarget::CurrentPlaylist)
    {
        // The channels become ordinary playlist entries rather than a reusable IPTV playlist.
        const auto playlistManager = appContext()->playlistManager();
        if (!playlistManager)
        {
            m_lastError = tr("The playlist is not available.");
            emit importFinished(false);
            return false;
        }

        // An optional name identifies the source in the list; it becomes a prefix of every title so
        // entries from different sources stay distinguishable. A URL-derived name is unhelpful here,
        // so an empty name simply leaves the channel titles untouched.
        const QString prefix = m_pendingName.trimmed();

        QList<PlaylistEntry> entries;
        entries.reserve(channels.size());
        for (const IptvChannel& channel: channels)
        {
            PlaylistEntry entry;
            entry.url = channel.url;
            entry.username = channel.username;
            entry.password = channel.password;

            const QString title = channel.group.isEmpty()
                ? channel.name
                : QString("%1 \u00b7 %2").arg(channel.group, channel.name);
            entry.title = prefix.isEmpty()
                ? title
                : QString("%1 \u00b7 %2").arg(prefix, title);
            entries.append(entry);
        }

        const QString id = playlistManager->ensureCurrentPlaylist();
        if (id.isEmpty() || !playlistManager->addEntries(id, entries))
        {
            m_lastError = tr("Failed to add the channels to the playlist.");
            emit importFinished(false);
            return false;
        }

        NX_INFO(typeid(IptvManager), "Added %1 channel(s) from '%2' to the current playlist",
            channels.size(), m_pendingSource);

        emit importFinished(true);
        return true;
    }

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
