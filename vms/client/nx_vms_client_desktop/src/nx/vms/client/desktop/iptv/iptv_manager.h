// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtNetwork/QNetworkAccessManager>

#include <nx/utils/uuid.h>

#include "iptv_playlist.h"

class QNetworkReply;
class QTimer;

namespace nx::vms::client::desktop {

class LocalSettings;
class SceneBanner;

/**
 * Owns the IPTV playlists imported by the user and opens their channels in the current layout.
 *
 * A playlist is imported either from an http(s) URL or from a local .m3u file, then parsed and kept
 * in the local settings. Channels are not turned into resources up front: a resource is created only
 * when a channel is actually opened, so a playlist with thousands of entries stays cheap.
 *
 * Importing a remote playlist is asynchronous: `importing` turns true, the download runs in the
 * background and `importFinished` is emitted when it is done. A playlist of tens of thousands of
 * entries downloads in seconds, so the UI must stay responsive instead of blocking on the request.
 */
class NX_VMS_CLIENT_DESKTOP_API IptvManager: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged)
    Q_PROPERTY(bool hasCurrentChannel READ hasCurrentChannel NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelName READ currentChannelName NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelGroup READ currentChannelGroup NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelLogo READ currentChannelLogo NOTIFY currentChannelChanged)
    Q_PROPERTY(int currentChannelNumber READ currentChannelNumber NOTIFY currentChannelChanged)
    Q_PROPERTY(int currentChannelCount READ currentChannelCount NOTIFY currentChannelChanged)
    /** Digits of the channel number being typed on the keyboard, empty when nothing is typed. */
    Q_PROPERTY(QString typedChannelNumber READ typedChannelNumber NOTIFY typedChannelNumberChanged)

public:
    explicit IptvManager(LocalSettings* settings, QObject* parent = nullptr);
    virtual ~IptvManager() override;

    /** Imported playlists as a list of { id, name, source, channelCount } maps. */
    QVariantList playlists() const;

    /** True while a remote playlist is being downloaded. */
    bool importing() const;

    /** True when a channel has been opened and is still part of its playlist. */
    bool hasCurrentChannel() const;
    QString currentChannelName() const;
    QString currentChannelGroup() const;
    QString currentChannelLogo() const;
    /** 1-based number of the current channel, or 0 when nothing is playing. */
    int currentChannelNumber() const;
    int currentChannelCount() const;

    /** Digits of the channel number being typed, empty when nothing is typed. */
    QString typedChannelNumber() const;

    /** Channels of a playlist as a list of { id, name, group, logo } maps. */
    Q_INVOKABLE QVariantList channels(const QString& playlistId) const;

    /**
     * Starts importing a playlist from an http(s) URL. Returns false if the request could not be
     * started; the download itself is asynchronous and reports through importFinished().
     */
    Q_INVOKABLE bool importFromUrl(const QString& url, const QString& name);

    /** Imports a playlist from a local .m3u/.m3u8 file. */
    Q_INVOKABLE bool importFromFile(const QString& path, const QString& name);

    /** Re-downloads or re-reads a previously imported playlist. */
    Q_INVOKABLE bool reloadPlaylist(const QString& id);

    /** Removes the playlist with the given id. */
    Q_INVOKABLE bool removePlaylist(const QString& id);

    /** Last import error, for the UI. Empty when the last operation succeeded. */
    Q_INVOKABLE QString lastError() const;

    /** Opens a channel of a playlist in the current layout. */
    Q_INVOKABLE bool openChannel(const QString& playlistId, const QString& channelId);

    /** Switches to the next channel of the current playlist, wrapping around. */
    Q_INVOKABLE bool playNext();

    /** Switches to the previous channel of the current playlist, wrapping around. */
    Q_INVOKABLE bool playPrevious();

    /** Opens the channel at the given 0-based index of the current playlist, wrapping around. */
    Q_INVOKABLE bool playChannelAt(int index);

    /** Opens a channel by its 1-based number, as it would be typed on a remote control. */
    Q_INVOKABLE bool playChannelNumber(int number);

    /** Forgets the current channel, hiding the on-screen display. */
    Q_INVOKABLE void clearCurrentChannel();

    /** Adds one digit to the channel number being typed; it is committed after a short pause. */
    Q_INVOKABLE void typeChannelDigit(const QString& digit);

    /** Discards the channel number being typed. */
    Q_INVOKABLE void clearTypedNumber();

protected:
    /**
     * Channel switching works while the video area is focused, which is a native widget rather than
     * part of the QML scene, so the hotkeys are handled at the application level.
     */
    virtual bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void playlistsChanged();
    void importingChanged();
    /** Emitted once an import completes; lastError() is set when it failed. */
    void importFinished(bool ok);
    /** Emitted whenever the current channel or its position changes. */
    void currentChannelChanged();
    /** Emitted when the channel number being typed changes. */
    void typedChannelNumberChanged();

private:
    bool startImport(const QString& source, const QString& name, const nx::Uuid& replaceId);
    /** Parses, stores and reports a downloaded/read playlist. Returns false on a parse failure. */
    bool finishImport(const QByteArray& data);
    /** Creates the resource for a channel and opens it, making it the current channel. */
    bool openChannelAt(const IptvPlaylist& playlist, int index);
    std::optional<IptvChannel> currentChannel() const;
    void onDownloadFinished();
    void onImportTimeout();
    void setImporting(bool value);
    /** Shows the given text over the video area; a non-positive timeout keeps it on screen. */
    void showBanner(const QString& text, int timeoutMs);
    /** Name, group and position of the current channel, for the on-screen display. */
    QString channelBannerText() const;
    /** Opens the channel whose number has been typed, then clears the typed digits. */
    void commitTypedNumber();
    /** True when the keyboard focus is in a text field, where digits must not switch channels. */
    static bool isTextInputFocused();
    /** True while the main window (rather than a dialog) is the active window. */
    static bool isMainWindowActive();
    /** Returns a copy: the settings property hands out its value by value. */
    std::optional<IptvPlaylist> findPlaylist(const nx::Uuid& id) const;
    bool storePlaylist(IptvPlaylist playlist);

    LocalSettings* const m_settings;
    QString m_lastError;

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_pendingReply;
    QTimer* m_timeout = nullptr;

    QString m_pendingSource;
    QString m_pendingName;
    nx::Uuid m_pendingReplaceId;
    bool m_importing = false;

    nx::Uuid m_currentPlaylistId;
    int m_currentChannelIndex = -1;

    QTimer* m_numberTimeout = nullptr;
    QString m_typedNumber;

    QPointer<SceneBanner> m_banner;
};

} // namespace nx::vms::client::desktop
