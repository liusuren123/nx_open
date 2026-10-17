// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "nxoptx_protocol.h"

#include <cstdio>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QLibraryInfo>
#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QLocalSocket>

#include <nx/utils/log/log.h>
#include <nx/vms/client/desktop/playlists/playlist.h>
#include <nx/vms/client/desktop/playlists/playlist_manager.h>

namespace nx::vms::client::desktop::nxoptx {

namespace {

const nx::log::Tag kLogTag(QStringLiteral("nxoptx"));

// Request and response field names, kept in one place so the protocol stays consistent.
const auto kAction = QStringLiteral("action");
const auto kPlaylist = QStringLiteral("playlist");
const auto kName = QStringLiteral("name");
const auto kTitle = QStringLiteral("title");
const auto kPath = QStringLiteral("path");
const auto kPaths = QStringLiteral("paths");
const auto kUrl = QStringLiteral("url");
const auto kItems = QStringLiteral("items");
const auto kUsername = QStringLiteral("username");
const auto kPassword = QStringLiteral("password");
const auto kRow = QStringLiteral("row");
const auto kReply = QStringLiteral("reply");
const auto kOk = QStringLiteral("ok");
const auto kError = QStringLiteral("error");

/** Actions understood by execute(), in the canonical spelling. */
const QStringList& knownActions()
{
    static const QStringList actions{
        QStringLiteral("ping"),
        QStringLiteral("help"),
        QStringLiteral("listPlaylists"),
        QStringLiteral("createPlaylist"),
        QStringLiteral("renamePlaylist"),
        QStringLiteral("removePlaylist"),
        QStringLiteral("setCurrentPlaylist"),
        QStringLiteral("addFile"),
        QStringLiteral("addStream"),
        QStringLiteral("addStreams"),
        QStringLiteral("renameEntry"),
        QStringLiteral("removeEntry"),
    };
    return actions;
}

/**
 * Maps the action spelling used in a URI to the canonical one. URIs have their host part lowercased
 * by QUrl, so the comparison has to ignore case.
 */
QString canonicalAction(const QString& action)
{
    for (const QString& candidate: knownActions())
    {
        if (candidate.compare(action, Qt::CaseInsensitive) == 0)
            return candidate;
    }

    if (action.compare(QStringLiteral("playlists"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("listPlaylists");

    return QString();
}

QJsonObject failure(const QString& code)
{
    return {{kOk, false}, {kError, code}};
}

QString stringValue(const QJsonObject& object, const QString& key)
{
    const auto value = object.value(key);
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble());
    return QString();
}

QStringList stringList(const QJsonObject& object, const QString& key)
{
    QStringList result;
    const auto value = object.value(key);
    if (value.isArray())
    {
        for (const auto& item: value.toArray())
        {
            if (item.isString())
                result.append(item.toString());
        }
    }
    else if (value.isString())
    {
        result.append(value.toString());
    }
    return result;
}

/**
 * Resolves the `playlist` field, which may hold either a playlist id or a playlist name, against the
 * configured playlists. An absent reference selects the current playlist, creating a default one
 * when none exists yet, so that an external program can fill a playlist without knowing an id.
 */
QString resolvePlaylistId(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString reference = stringValue(request, kPlaylist).trimmed();
    if (reference.isEmpty())
        return playlists->ensureCurrentPlaylist();

    const auto configured = playlists->playlists();
    for (const Playlist& playlist: configured)
    {
        if (playlist.id.toSimpleString() == reference)
            return reference;
    }
    for (const Playlist& playlist: configured)
    {
        if (playlist.name.compare(reference, Qt::CaseInsensitive) == 0)
            return playlist.id.toSimpleString();
    }
    return QString();
}

QJsonObject playlistEntriesToJson(const Playlist& playlist)
{
    QJsonArray entries;
    for (int row = 0; row < playlist.entries.size(); ++row)
    {
        const PlaylistEntry& entry = playlist.entries.at(row);

        QJsonObject item;
        item[QStringLiteral("row")] = row;
        item[QStringLiteral("kind")] = entry.isStream()
            ? QStringLiteral("stream")
            : (entry.isFolder ? QStringLiteral("folder") : QStringLiteral("file"));
        if (!entry.path.isEmpty())
            item[QStringLiteral("path")] = entry.path;
        if (!entry.url.isEmpty())
            item[QStringLiteral("url")] = entry.url;
        if (!entry.title.isEmpty())
            item[QStringLiteral("title")] = entry.title;
        if (!entry.username.isEmpty())
            item[QStringLiteral("username")] = entry.username;
        entries.append(item);
    }
    return {{QStringLiteral("entries"), entries}};
}

QJsonObject listPlaylists(PlaylistManager* playlists)
{
    const QString currentId = playlists->currentPlaylistId();

    QJsonArray items;
    for (const Playlist& playlist: playlists->playlists())
    {
        const QString id = playlist.id.toSimpleString();

        QJsonObject item = playlistEntriesToJson(playlist);
        item[QStringLiteral("id")] = id;
        item[QStringLiteral("name")] = playlist.name;
        item[QStringLiteral("current")] = (id == currentId);
        items.append(item);
    }

    return {
        {kOk, true},
        {QStringLiteral("currentPlaylist"), currentId},
        {QStringLiteral("playlists"), items}};
}

QJsonObject createPlaylist(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString name = stringValue(request, kName).trimmed();
    const QString id = playlists->createPlaylist(name);
    if (id.isEmpty())
        return failure(QStringLiteral("createFailed"));

    return {
        {kOk, true},
        {QStringLiteral("id"), id},
        {kName, playlists->playlist(id).name}};
}

QJsonObject renamePlaylist(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    const QString name = stringValue(request, kName).trimmed();
    if (name.isEmpty())
        return failure(QStringLiteral("noName"));

    playlists->renamePlaylist(id, name);
    return {{kOk, true}, {QStringLiteral("id"), id}, {kName, name}};
}

QJsonObject removePlaylist(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    playlists->removePlaylist(id);
    return {{kOk, true}, {QStringLiteral("id"), id}};
}

QJsonObject setCurrentPlaylist(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    playlists->setCurrentPlaylistId(id);
    return {{kOk, true}, {QStringLiteral("id"), id}};
}

QJsonObject addFile(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    QStringList paths = stringList(request, kPaths);
    const QString singlePath = stringValue(request, kPath);
    if (!singlePath.isEmpty())
        paths.prepend(singlePath);

    if (paths.isEmpty())
        return failure(QStringLiteral("noPath"));

    const int firstRow = playlists->playlist(id).entries.size();
    if (!playlists->addPaths(id, paths))
        return failure(QStringLiteral("noFilesFound"));

    const int lastRow = playlists->playlist(id).entries.size() - 1;
    return {
        {kOk, true},
        {kPlaylist, id},
        {QStringLiteral("firstRow"), firstRow},
        {QStringLiteral("lastRow"), lastRow}};
}

QJsonObject addStream(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    const QString url = stringValue(request, kUrl).trimmed();
    if (url.isEmpty())
        return failure(QStringLiteral("noUrl"));

    PlaylistEntry entry;
    entry.url = url;
    entry.username = stringValue(request, kUsername);
    entry.password = stringValue(request, kPassword);

    // The display name is what the client shows for the entry; `title` is accepted as an alias.
    QString title = stringValue(request, kName).trimmed();
    if (title.isEmpty())
        title = stringValue(request, kTitle).trimmed();
    entry.title = title.isEmpty() ? url : title;

    const int row = playlists->playlist(id).entries.size();
    if (!playlists->addEntries(id, {entry}))
        return failure(QStringLiteral("addFailed"));

    return {
        {kOk, true},
        {kPlaylist, id},
        {QStringLiteral("row"), row},
        {kName, entry.title}};
}

/**
 * Adds several streams at once. The items come as a JSON array in the `items` parameter, each one an
 * object with the same fields as a single addStream request: url, name (or title), username and
 * password. One command per click keeps a batch of streams from competing for the one external
 * protocol navigation a browser may allow at a time.
 */
QJsonObject addStreams(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    const QString payload = stringValue(request, kItems);
    if (payload.isEmpty())
        return failure(QStringLiteral("noItems"));

    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8());
    if (!document.isArray())
        return failure(QStringLiteral("invalidItems"));

    QList<PlaylistEntry> entries;
    for (const QJsonValue& value: document.array())
    {
        const QJsonObject item = value.toObject();
        const QString url = stringValue(item, kUrl).trimmed();
        if (url.isEmpty())
            continue;

        PlaylistEntry entry;
        entry.url = url;
        entry.username = stringValue(item, kUsername);
        entry.password = stringValue(item, kPassword);

        QString title = stringValue(item, kName).trimmed();
        if (title.isEmpty())
            title = stringValue(item, kTitle).trimmed();
        entry.title = title.isEmpty() ? url : title;
        entries.append(entry);
    }

    if (entries.isEmpty())
        return failure(QStringLiteral("noItems"));

    const int firstRow = playlists->playlist(id).entries.size();
    if (!playlists->addEntries(id, entries))
        return failure(QStringLiteral("addFailed"));

    return {
        {kOk, true},
        {kPlaylist, id},
        {QStringLiteral("firstRow"), firstRow},
        {QStringLiteral("lastRow"), firstRow + entries.size() - 1},
        {QStringLiteral("count"), entries.size()}};
}

QJsonObject renameEntry(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    const int row = request.value(kRow).toInt(-1);
    if (row < 0)
        return failure(QStringLiteral("noRow"));

    // The new name is what the entry is shown under; `title` is accepted as an alias, like in
    // addStream().
    QString title = stringValue(request, kName).trimmed();
    if (title.isEmpty())
        title = stringValue(request, kTitle).trimmed();
    if (title.isEmpty())
        return failure(QStringLiteral("noName"));

    if (!playlists->renameEntry(id, row, title))
        return failure(QStringLiteral("renameFailed"));

    return {{kOk, true}, {kPlaylist, id}, {kRow, row}, {kName, title}};
}

QJsonObject removeEntry(PlaylistManager* playlists, const QJsonObject& request)
{
    const QString id = resolvePlaylistId(playlists, request);
    if (id.isEmpty())
        return failure(QStringLiteral("playlistNotFound"));

    const int row = request.value(kRow).toInt(-1);
    if (row < 0)
        return failure(QStringLiteral("noRow"));

    if (!playlists->removeEntries(id, {row}))
        return failure(QStringLiteral("removeFailed"));

    return {{kOk, true}, {kPlaylist, id}, {kRow, row}};
}

QJsonObject help()
{
    QJsonArray actions;
    for (const QString& action: knownActions())
        actions.append(action);

    return {
        {kOk, true},
        {QStringLiteral("version"), kVersion},
        {QStringLiteral("socket"), socketName()},
        {QStringLiteral("actions"), actions}};
}

} // namespace

QString socketName()
{
    // A fixed name keeps the protocol trivially discoverable: on Windows the client listens on the
    // named pipe \\.\pipe\nxoptx, elsewhere on a socket of that name in the local socket directory.
    return QStringLiteral("nxoptx");
}

QJsonObject execute(const QJsonObject& request, PlaylistManager* playlists)
{
    if (!playlists)
        return failure(QStringLiteral("unavailable"));

    const QString requested = stringValue(request, kAction);
    const QString action = canonicalAction(requested);
    if (action.isEmpty())
    {
        NX_WARNING(kLogTag, "Rejecting a request with an unknown action '%1'.", requested);
        return failure(QStringLiteral("unknownAction"));
    }

    NX_INFO(kLogTag, "Executing '%1'.", action);

    if (action == QLatin1String("ping"))
        return {{kOk, true}, {QStringLiteral("version"), kVersion},
            {QStringLiteral("socket"), socketName()}};
    if (action == QLatin1String("help"))
        return help();
    if (action == QLatin1String("listPlaylists"))
        return listPlaylists(playlists);
    if (action == QLatin1String("createPlaylist"))
        return createPlaylist(playlists, request);
    if (action == QLatin1String("renamePlaylist"))
        return renamePlaylist(playlists, request);
    if (action == QLatin1String("removePlaylist"))
        return removePlaylist(playlists, request);
    if (action == QLatin1String("setCurrentPlaylist"))
        return setCurrentPlaylist(playlists, request);
    if (action == QLatin1String("addFile"))
        return addFile(playlists, request);
    if (action == QLatin1String("addStream"))
        return addStream(playlists, request);
    if (action == QLatin1String("addStreams"))
        return addStreams(playlists, request);
    if (action == QLatin1String("renameEntry"))
        return renameEntry(playlists, request);
    if (action == QLatin1String("removeEntry"))
        return removeEntry(playlists, request);

    NX_WARNING(kLogTag, "No handler for the action '%1'.", action);
    return failure(QStringLiteral("unknownAction"));
}

std::optional<QJsonObject> requestFromUri(const QUrl& uri, QString* error)
{
    const auto fail = [error](const QString& message) -> std::optional<QJsonObject>
    {
        if (error)
            *error = message;
        return std::nullopt;
    };

    if (uri.scheme().compare(QLatin1String(kScheme), Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("Not an %1:// URI.").arg(QLatin1String(kScheme)));

    // The action may be given as the host or as the path, so that both nxoptx://addStream?... and
    // nxoptx:///addStream?... work.
    QString action = uri.host();
    if (action.isEmpty())
        action = uri.path();
    while (action.startsWith(QLatin1Char('/')))
        action.remove(0, 1);
    while (action.endsWith(QLatin1Char('/')))
        action.chop(1);

    const QString canonical = canonicalAction(action);
    if (canonical.isEmpty())
        return fail(QStringLiteral("Unknown action '%1'.").arg(action));

    QJsonObject request{{kAction, canonical}};

    // Every parameter except path/paths becomes a string field; path/paths are collected so that a
    // command can add several files at once.
    QUrlQuery query;
    query.setQuery(uri.query(QUrl::FullyEncoded));
    query.setQueryDelimiters(QLatin1Char('='), QLatin1Char('&'));

    QJsonArray paths;
    for (const auto& [key, value]: query.queryItems(QUrl::FullyDecoded))
    {
        if (key == kReply)
            continue;
        if (key == kPath || key == kPaths)
        {
            paths.append(value);
            continue;
        }
        request.insert(key, value);
    }
    if (!paths.isEmpty())
        request.insert(kPaths, paths);

    return request;
}

std::optional<QJsonObject> sendRequest(const QJsonObject& request, int timeoutMs, QString* error)
{
    const auto fail = [error](const QString& message) -> std::optional<QJsonObject>
    {
        if (error)
            *error = message;
        return std::nullopt;
    };

    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(timeoutMs))
    {
        return fail(QStringLiteral("The client does not accept %1:// commands (is it running?): %2")
            .arg(QLatin1String(kScheme), socket.errorString()));
    }

    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(line) != line.size() || !socket.waitForBytesWritten(timeoutMs))
    {
        return fail(QStringLiteral("The command could not be sent: %1")
            .arg(socket.errorString()));
    }

    QByteArray buffer;
    while (true)
    {
        const int newline = buffer.indexOf('\n');
        if (newline >= 0)
        {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(
                buffer.left(newline), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
            {
                return fail(QStringLiteral("The client answered with malformed JSON: %1")
                    .arg(parseError.errorString()));
            }
            return document.object();
        }

        if (!socket.waitForReadyRead(timeoutMs))
        {
            return fail(socket.errorString().isEmpty()
                ? QStringLiteral("The client did not answer in time.")
                : socket.errorString());
        }
        buffer.append(socket.readAll());
    }
}

namespace {

/**
 * Directory a shell has to add to PATH for this executable to be able to start, or an empty string
 * when the executable can start on its own.
 *
 * A normal installation keeps its dependencies (Qt among them) next to the executable. Development
 * builds keep Qt in an external package that is only reachable through PATH, and a process started
 * by the shell as the URI handler does not have that PATH at all, so the directory Qt was loaded
 * from has to be added explicitly; without it the executable fails to load its DLLs.
 */
QString dependencySearchDirectory()
{
#if defined(Q_OS_WIN)
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    if (!applicationDirectory.entryList({QStringLiteral("Qt6Core*.dll")}, QDir::Files).isEmpty())
        return {};

    // Empty for the debug build of Qt, which ships the same DLLs with a different name.
    const QString qtBinaries = QLibraryInfo::path(QLibraryInfo::BinariesPath);
    return QDir(qtBinaries).exists() ? QDir::toNativeSeparators(qtBinaries) : QString();
#else
    return {};
#endif
}

} // namespace

bool registerUriHandler()
{
#if defined(Q_OS_WIN)
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString scheme = QLatin1String(kScheme);

    // The `%1` placeholder is kept verbatim: the shell replaces it with the URI being opened, and
    // the `set` before it expands `%PATH%` from its own environment.
    const QString searchDirectory = dependencySearchDirectory();
    const QString command = searchDirectory.isEmpty()
        ? QStringLiteral("\"") + executable + QStringLiteral("\" --") + scheme
            + QStringLiteral(" \"%1\"")
        : QStringLiteral("cmd.exe /c set \"PATH=") + searchDirectory
            + QStringLiteral(";%PATH%\" && \"") + executable + QStringLiteral("\" --") + scheme
            + QStringLiteral(" \"%1\"");

    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1")
        .arg(scheme), QSettings::NativeFormat);

    if (registry.value(QStringLiteral("command")).toString() == command)
        return true;

    registry.setValue(QStringLiteral("."),
        QStringLiteral("Nx Optx playback control protocol"));
    registry.setValue(QStringLiteral("URL Protocol"), QString());
    registry.setValue(QStringLiteral("command"), command);
    registry.setValue(QStringLiteral("shell/open/command/."), command);
    registry.sync();

    const bool registered = registry.status() == QSettings::NoError;
    if (registered)
    {
        NX_INFO(kLogTag, "Registered the %1:// URI handler.", scheme);
        if (!searchDirectory.isEmpty())
        {
            NX_INFO(kLogTag, "The dependencies of this build are not next to the executable, so "
                "the handler adds '%1' to PATH. A regular installation does not need this.",
                searchDirectory);
        }
    }
    else
    {
        NX_WARNING(kLogTag, "Could not register the %1:// URI handler (error %2).",
            scheme, int(registry.status()));
    }
    return registered;
#else
    return false;
#endif
}

int runCommandLineClient(const QString& command)
{
    // Strip quotes a shell may have left around the URI.
    QString text = command.trimmed();
    if (text.size() >= 2 && text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"')))
        text = text.mid(1, text.size() - 2);

    const QUrl uri(text, QUrl::TolerantMode);

    QUrlQuery query;
    query.setQuery(uri.query(QUrl::FullyEncoded));
    query.setQueryDelimiters(QLatin1Char('='), QLatin1Char('&'));
    const QString replyPath = query.queryItemValue(kReply, QUrl::FullyDecoded);

    QJsonObject response;
    QString error;
    const auto request = requestFromUri(uri, &error);
    if (!request)
    {
        response = {
            {kOk, false},
            {kError, QStringLiteral("badRequest")},
            {QStringLiteral("message"), error}};
    }
    else
    {
        QString sendError;
        const auto result = sendRequest(*request, 5000, &sendError);
        if (result)
            response = *result;
        else
            response = {
                {kOk, false},
                {kError, QStringLiteral("notRunning")},
                {QStringLiteral("message"), sendError}};
    }

    const QByteArray json = QJsonDocument(response).toJson(QJsonDocument::Compact);

    if (!replyPath.isEmpty())
    {
        QFile file(replyPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(json);
        else
            NX_WARNING(kLogTag, "Could not write the reply to '%1'.", replyPath);
    }

    // Also print it, so that a caller which captured the output can read it without a reply file.
    std::fwrite(json.constData(), 1, json.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);

    return response.value(kOk).toBool() ? 0 : 1;
}

} // namespace nx::vms::client::desktop::nxoptx
