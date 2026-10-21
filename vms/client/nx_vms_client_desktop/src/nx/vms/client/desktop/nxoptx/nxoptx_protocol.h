// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>

#include <QtCore/QJsonObject>
#include <QtCore/QString>

class QUrl;

namespace nx::vms::client::desktop {

class PlaylistManager;
class LocalSettings;
class StreamCache;

/**
 * The nxoptx protocol.
 *
 * A small local request/response API that lets an external program inspect and fill the local
 * playlists of a running client, including creating playlists and adding network streams with a
 * name to show in the client. Requests and responses are single line JSON objects.
 *
 * A running client serves the protocol on a local socket, which is a named pipe on Windows; see
 * socketName(). The very same requests may also be passed as nxoptx:// URIs on the command line of
 * the client executable, which is what the URI handler registered by registerUriHandler() does.
 */
namespace nxoptx {

/** URI scheme of the protocol. */
inline constexpr auto kScheme = "nxoptx";

/** Version reported by the `ping` request. */
inline constexpr int kVersion = 1;

/** Name of the local socket the protocol is served on. */
QString socketName();

/**
 * Executes one request and returns the response object. Every request is answered, so the result is
 * never an empty object; a failure carries `{"ok": false, "error": "<code>"}`.
 */
QJsonObject execute(const QJsonObject& request, PlaylistManager* playlists,
    LocalSettings* settings = nullptr, StreamCache* streamCache = nullptr);

/**
 * Converts an nxoptx:// URI into a request object. Returns nullopt and describes the problem in
 * error when the URI is malformed or names an unknown action.
 */
std::optional<QJsonObject> requestFromUri(const QUrl& uri, QString* error);

/**
 * Sends a request to a running client and waits for its response. Returns nullopt when no client is
 * listening or it does not answer in time, and describes the problem in error.
 */
std::optional<QJsonObject> sendRequest(const QJsonObject& request, int timeoutMs = 5000,
    QString* error = nullptr);

/**
 * Registers this executable as the handler of the nxoptx:// URI scheme for the current user, so
 * that `nxoptx://...` links can be opened from a shell or another program. Does nothing on
 * platforms without such a mechanism. Returns whether the handler is in place.
 */
bool registerUriHandler();

/**
 * Runs a command received on the command line from the URI handler: parses the nxoptx:// URI, sends
 * the resulting request to the running client and, when the URI carries a `reply` parameter, writes
 * the response to that file. The response is also printed to the standard output. A QCoreApplication
 * instance has to exist. Returns the process exit code.
 */
int runCommandLineClient(const QString& command);

} // namespace nxoptx
} // namespace nx::vms::client::desktop
