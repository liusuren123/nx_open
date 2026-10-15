// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "hls_vod_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QtMath>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <nx/utils/log/log.h>

namespace nx::vms::client::desktop {

namespace {

constexpr char kUserAgent[] = "NxMetaClient";

/** A playlist is a small text file; a segment may take minutes on a slow server. */
constexpr int kPlaylistTimeoutUs = 6'000'000;
constexpr int kSegmentTimeoutUs = 25'000'000;
constexpr int kSegmentAttempts = 4;
constexpr std::chrono::milliseconds kSegmentRetryBaseDelay{400};

/**
 * A segment a client is waiting for is fetched as a few ranged requests at once. A single connection
 * is limited per connection by some servers, so this is what makes the wait for the first segment
 * (and for the one a seek lands on) short.
 */
constexpr int kRangeParts = 4;
constexpr qint64 kRangePartBytes = 768 * 1024;

/** Upper bound of a ranged segment download; a segment is minutes of video at most. */
constexpr int kRangeReadsMax = 64;

/** One media segment of the playlist. */
struct Segment
{
    QString url;
    double duration = 0.0;
    bool discontinuityBefore = false;
};

struct Playlist
{
    QList<Segment> segments;

    /** Encryption key location, empty when the segments are not encrypted. */
    QString keyUrl;
    QString keyIv;

    double maxDuration = 0.0;
};

int stopCallback(void* opaque)
{
    return static_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}

/** Value of an HLS attribute, e.g. URI="key.bin" of an #EXT-X-KEY tag. */
QString attributeValue(const QString& attributes, const QString& name)
{
    int pos = 0;
    while (pos < attributes.size())
    {
        const int equals = attributes.indexOf('=', pos);
        if (equals < 0)
            break;

        const QString key = attributes.mid(pos, equals - pos).trimmed();
        const int valueStart = equals + 1;
        QString value;
        int next = -1;
        if (valueStart < attributes.size() && attributes.at(valueStart) == '"')
        {
            const int closing = attributes.indexOf('"', valueStart + 1);
            if (closing < 0)
                break;

            value = attributes.mid(valueStart + 1, closing - valueStart - 1);
            next = attributes.indexOf(',', closing);
        }
        else
        {
            next = attributes.indexOf(',', valueStart);
            if (next < 0)
                next = attributes.size();

            value = attributes.mid(valueStart, next - valueStart).trimmed();
        }

        if (key == name)
            return value;

        if (next < 0)
            break;

        pos = next + 1;
    }

    return {};
}

quint64 folderSizeBytes(const QString& path)
{
    quint64 result = 0;
    const QFileInfoList entries = QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry: entries)
        result += entry.isDir() ? folderSizeBytes(entry.absoluteFilePath()) : quint64(entry.size());

    return result;
}

/** Folder the segments of a playlist are cached in. The URL may carry credentials, so only its
 * digest is used on disk. */
QString cacheFolderFor(const QString& cacheFolder, const QString& playlistUrl)
{
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(playlistUrl.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QDir(cacheFolder).absoluteFilePath(QStringLiteral("hls/") + digest);
}

/**
 * Parses a media playlist. Only what this client is able to replay is accepted; anything else makes
 * the caller fall back to playing the playlist directly, as it did before.
 */
bool parsePlaylist(const QString& playlistUrl, const QString& text, Playlist* playlist)
{
    if (!text.contains(QLatin1String("#EXTM3U")))
        return false;

    // A master playlist lists variants rather than segments; choosing one is a separate feature.
    if (text.contains(QLatin1String("#EXT-X-STREAM-INF")) || text.contains(QLatin1String("#EXT-X-MEDIA:")))
        return false;

    // Only a finite playlist may be downloaded: a live one would never end.
    if (!text.contains(QLatin1String("#EXT-X-ENDLIST")))
        return false;

    const QUrl base(playlistUrl);
    QString keyUrl;
    QString keyIv;
    bool haveKey = false;
    double pendingDuration = -1.0;
    bool pendingDiscontinuity = false;

    for (QString line: text.split('\n'))
    {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith('#'))
        {
            if (line.startsWith(QLatin1String("#EXTINF:")))
            {
                pendingDuration = line.mid(8).section(',', 0, 0).toDouble();
            }
            else if (line == QLatin1String("#EXT-X-DISCONTINUITY"))
            {
                pendingDiscontinuity = true;
            }
            else if (line.startsWith(QLatin1String("#EXT-X-KEY:")))
            {
                const QString attributes = line.mid(11);
                const QString method = attributeValue(attributes, "METHOD");
                if (method.compare(QLatin1String("NONE"), Qt::CaseInsensitive) == 0)
                    continue;

                if (method.compare(QLatin1String("AES-128"), Qt::CaseInsensitive) != 0)
                    return false; //< Sample encryption and DRM are not supported.

                const QString uri = attributeValue(attributes, "URI");
                const QString iv = attributeValue(attributes, "IV");
                if (haveKey && (uri != keyUrl || iv != keyIv))
                    return false; //< Key rotation is not supported.

                keyUrl = uri;
                keyIv = iv;
                haveKey = true;
            }
            else if (line.startsWith(QLatin1String("#EXT-X-BYTERANGE"))
                || line.startsWith(QLatin1String("#EXT-X-MAP:")))
            {
                return false; //< Segments would not be independent files.
            }

            continue;
        }

        Segment segment;
        segment.url = base.resolved(QUrl(line)).toString();
        segment.duration = pendingDuration > 0.0 ? pendingDuration : 0.0;
        segment.discontinuityBefore = pendingDiscontinuity;
        playlist->segments.append(segment);
        playlist->maxDuration = qMax(playlist->maxDuration, segment.duration);
        pendingDuration = -1.0;
        pendingDiscontinuity = false;
    }

    if (playlist->segments.isEmpty())
        return false;

    if (haveKey)
    {
        playlist->keyUrl = base.resolved(QUrl(keyUrl)).toString();
        playlist->keyIv = keyIv;
    }

    return true;
}

QString ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(buffer)).arg(code);
}

/** Reads a whole (small) HTTP resource, optionally only a byte range of it. */
bool httpGet(const QString& url, std::atomic<bool>* stop, int timeoutUs, QByteArray* out,
    QString* errorOut = nullptr, qint64 rangeStart = -1, qint64 rangeEnd = -1)
{
    AVDictionary* options = nullptr;
    av_dict_set(&options, "user_agent", kUserAgent, 0);
    // A reset connection is retried by FFmpeg itself before it gives the request up.
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_on_network_error", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "2", 0);
    if (rangeStart >= 0)
    {
        QByteArray range = "Range: bytes=" + QByteArray::number(rangeStart) + '-';
        if (rangeEnd >= 0)
            range += QByteArray::number(rangeEnd);

        // No trailing newline here: FFmpeg adds one to every header it is given.
        av_dict_set(&options, "headers", range.constData(), 0);
    }

    if (timeoutUs > 0)
        av_dict_set(&options, "rw_timeout", QByteArray::number(timeoutUs).constData(), 0);

    AVIOInterruptCB interrupt{&stopCallback, stop};
    AVIOContext* input = nullptr;
    const int result = avio_open2(&input, url.toUtf8().constData(), AVIO_FLAG_READ, &interrupt, &options);
    av_dict_free(&options);
    if (result < 0)
    {
        if (errorOut)
            *errorOut = ffmpegError(result);

        return false;
    }

    out->clear();
    std::vector<unsigned char> buffer(256 * 1024);
    for (;;)
    {
        const int read = avio_read(input, buffer.data(), static_cast<int>(buffer.size()));
        if (read > 0)
        {
            out->append(reinterpret_cast<const char*>(buffer.data()), read);
            continue;
        }

        if (read == AVERROR_EOF)
            break;

        if (read < 0)
        {
            if (errorOut)
                *errorOut = ffmpegError(read);

            avio_close(input);
            return false;
        }

        break;
    }

    avio_close(input);
    return !stop->load();
}

/** State of one segment on disk. */
enum SegmentState { Pending = 0, Downloading = 1, Ready = 2, Failed = 3 };

} // namespace

struct HlsVodSource::Private
{
    QString playlistUrl;
    Playlist playlist;
    QString folder;
    QByteArray key;
    QByteArray localPlaylist;
    QString localUrl;

    int connections = 1;
    std::atomic<bool> stop{false};

    /** Stream cache the new segments are accounted against. */
    QString cacheFolderPath;
    qint64 cacheLimitBytes = 0;

    // Segment states, guarded by mutex; shared by the download threads and the local server.
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<int> states;
    std::vector<int> attempts;
    std::set<int> priority;
    int nextIndex = 0;

    quint64 remainingBytes = 0;
    bool cacheBudgetKnown = false;

    std::unique_ptr<QTcpServer> server;
    std::thread acceptThread;
    std::vector<std::thread> workers;

    QString segmentPath(int index) const
    {
        return QDir(folder).absoluteFilePath(QString::number(index) + ".ts");
    }

    /** Next segment to download, honoring the ones a client is waiting for. */
    int takeIndex()
    {
        for (;;)
        {
            if (stop.load())
                return -1;

            if (!priority.empty())
            {
                const int index = *priority.begin();
                priority.erase(priority.begin());
                if (states[index] == Pending)
                {
                    states[index] = Downloading;
                    return index;
                }

                continue;
            }

            if (nextIndex >= static_cast<int>(states.size()))
                return -1;

            const int index = nextIndex++;
            if (states[index] == Pending)
            {
                states[index] = Downloading;
                return index;
            }
        }
    }

    void accountCache(quint64 bytes)
    {
        if (!cacheBudgetKnown)
        {
            // Measured on the first download rather than when the source is created, so that opening
            // a playlist does not have to walk the whole cache folder.
            cacheBudgetKnown = true;
            const quint64 used = folderSizeBytes(cacheFolderPath);
            remainingBytes = (cacheLimitBytes > 0 && used < quint64(cacheLimitBytes))
                ? quint64(cacheLimitBytes) - used
                : kUnlimited;
        }

        if (remainingBytes == kUnlimited)
            return;

        if (bytes > remainingBytes)
        {
            remainingBytes = kUnlimited;
            NX_WARNING(typeid(HlsVodSource), "Stream cache is full; '%1' keeps downloading without "
                "caching it. Clear the cache to make room.", playlistUrl);
            return;
        }

        remainingBytes -= bytes;
    }

    /** Places a downloaded segment in the cache, atomically. */
    static bool writeSegmentFile(const QString& path, const QByteArray& data)
    {
        // A worker and a client request may download the same segment at the same time.
        static std::atomic<quint64> counter{0};
        const QString part = path + QStringLiteral(".part%1").arg(counter.fetch_add(1));
        QFile file(part);
        if (!file.open(QIODevice::WriteOnly))
            return false;

        const bool written = (file.write(data) == data.size());
        const bool flushed = written && file.flush();
        file.close();
        if (!written || !flushed)
        {
            QFile::remove(part);
            return false;
        }

        if (!QFile::rename(part, path))
        {
            QFile::remove(part);
            return false;
        }

        return true;
    }

    bool downloadSegment(int index, QString* errorOut)
    {
        const QString path = segmentPath(index);
        if (QFile::exists(path))
            return true;

        QByteArray data;
        const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
        if (!httpGet(playlist.segments[index].url, &stop, kSegmentTimeoutUs, &data, errorOut)
            || data.isEmpty())
        {
            if (errorOut && errorOut->isEmpty())
                *errorOut = QStringLiteral("empty response");

            NX_DEBUG(typeid(HlsVodSource), "Segment %1 did not download in %2 ms: %3.",
                index, QDateTime::currentMSecsSinceEpoch() - startedMs,
                errorOut ? *errorOut : QString());
            return false;
        }

        NX_DEBUG(typeid(HlsVodSource), "Segment %1 downloaded: %2 bytes in %3 ms.",
            index, data.size(), QDateTime::currentMSecsSinceEpoch() - startedMs);

        accountCache(quint64(data.size()));
        return writeSegmentFile(path, data);
    }

    /**
     * Downloads a segment as several ranged requests at once. This is what keeps the wait for the
     * segment a client has asked for short, since a single connection is limited per connection by
     * some servers. Falls back to a single request when the server does not honor byte ranges, or when
     * a part does not come back the way it should.
     */
    bool downloadSegmentParallel(int index, QString* errorOut)
    {
        const QString path = segmentPath(index);
        if (QFile::exists(path))
            return true;

        const QString url = playlist.segments[index].url;
        const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();

        std::vector<QByteArray> parts(kRangeParts);
        std::vector<QString> errors(kRangeParts);
        std::vector<char> failed(kRangeParts, 0);

        std::vector<std::thread> threads;
        threads.reserve(kRangeParts);
        for (int part = 0; part < kRangeParts; ++part)
        {
            threads.emplace_back([&, part]
            {
                const qint64 start = qint64(part) * kRangePartBytes;
                if (!httpGet(url, &stop, kSegmentTimeoutUs, &parts[part], &errors[part], start,
                        start + kRangePartBytes - 1))
                {
                    failed[part] = 1;
                }
            });
        }

        for (std::thread& thread: threads)
            thread.join();

        if (stop.load())
            return false;

        QByteArray assembled;
        bool complete = true;
        for (int part = 0; part < kRangeParts && complete; ++part)
        {
            if (failed[part])
            {
                // Nothing usable came back: a ranged request may be refused, and a plain one still
                // gets the segment, just slower.
                if (assembled.isEmpty())
                    return downloadSegment(index, errorOut);

                break;
            }

            if (parts[part].size() > kRangePartBytes)
                return downloadSegment(index, errorOut); //< The server ignored the range.

            assembled += parts[part];
            if (parts[part].size() < kRangePartBytes)
                complete = false;
        }

        // Every part came back full, so the segment may go on past them.
        if (complete)
        {
            qint64 offset = qint64(kRangeParts) * kRangePartBytes;
            for (int read = 0; read < kRangeReadsMax; ++read)
            {
                QByteArray tail;
                if (!httpGet(url, &stop, kSegmentTimeoutUs, &tail, nullptr, offset, -1)
                    || tail.isEmpty())
                {
                    break; //< Nothing beyond that offset.
                }

                assembled += tail;
                if (tail.size() < kRangePartBytes)
                    break;

                offset += tail.size();
            }
        }

        if (assembled.isEmpty())
        {
            if (errorOut)
                *errorOut = QStringLiteral("no data in the ranged response");

            return false;
        }

        NX_DEBUG(typeid(HlsVodSource), "Segment %1 downloaded in parts: %2 bytes in %3 ms.",
            index, assembled.size(), QDateTime::currentMSecsSinceEpoch() - startedMs);

        accountCache(quint64(assembled.size()));
        return writeSegmentFile(path, assembled);
    }

    void workerLoop()
    {
        for (;;)
        {
            int index = -1;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (stop.load())
                    return;

                index = takeIndex();
                if (index < 0)
                {
                    // Idle: wait for a client to ask for a segment that is not downloaded yet.
                    condition.wait_for(lock, std::chrono::milliseconds(200));
                    continue;
                }
            }

            QString error;
            bool ok = false;
            for (int attempt = 0; attempt < kSegmentAttempts && !stop.load(); ++attempt)
            {
                if (downloadSegment(index, &error))
                {
                    ok = true;
                    break;
                }

                // Back off: a server may be limiting how often a connection can be opened.
                std::this_thread::sleep_for(kSegmentRetryBaseDelay * (1 << attempt));
            }

            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (ok)
                {
                    states[index] = Ready;
                }
                else if (!stop.load() && ++attempts[index] < kSegmentAttempts)
                {
                    // Put it back into the queue; a server may reject a burst of connections.
                    states[index] = Pending;
                    priority.insert(index);
                }
                else
                {
                    states[index] = Failed;
                }
            }

            if (!ok)
            {
                NX_WARNING(typeid(HlsVodSource), "Failed to download segment %1 of '%2': %3.",
                    index, playlistUrl, error);
            }

            condition.notify_all();
        }
    }

    /**
     * Makes the segment available for a client.
     *
     * It is downloaded right here instead of being waited for: a worker fetches over a single
     * connection, which takes tens of seconds on a server that limits connections, and every other
     * request would be stuck behind that wait since one connection is served at a time.
     */
    bool ensureSegment(int index)
    {
        if (QFile::exists(segmentPath(index)))
            return true;

        QString error;
        for (int attempt = 0; attempt < kSegmentAttempts && !stop.load(); ++attempt)
        {
            if (downloadSegmentParallel(index, &error))
            {
                const std::lock_guard<std::mutex> lock(mutex);
                states[index] = Ready;
                return true;
            }

            std::this_thread::sleep_for(kSegmentRetryBaseDelay * (1 << attempt));
        }

        NX_WARNING(typeid(HlsVodSource), "Cannot serve segment %1 of '%2': %3.",
            index, playlistUrl, error);
        return false;
    }

    static bool readRequest(QTcpSocket* socket, QString* path)
    {
        QByteArray request;
        while (!request.contains("\r\n\r\n"))
        {
            if (!socket->waitForReadyRead(5000))
                return false;

            request += socket->readAll();
            if (request.size() > 64 * 1024)
                return false;
        }

        const QList<QByteArray> firstLine = request.left(request.indexOf("\r\n")).split(' ');
        if (firstLine.size() < 2)
            return false;

        *path = QString::fromUtf8(firstLine.at(1));
        return true;
    }

    static void sendResponse(QTcpSocket* socket, int code, const char* reason,
        const QByteArray& contentType, const QByteArray& body)
    {
        QByteArray response = "HTTP/1.0 " + QByteArray::number(code) + ' ' + reason + "\r\n";
        response += "Content-Type: " + contentType + "\r\n";
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += body;
        socket->write(response);
        socket->flush();
        socket->waitForBytesWritten(30000);
    }

    void handleConnection(QTcpSocket* socket)
    {
        QString path;
        if (!readRequest(socket, &path))
        {
            socket->abort();
            return;
        }

        if (path == QLatin1String("/playlist.m3u8"))
        {
            sendResponse(socket, 200, "OK", "application/vnd.apple.mpegurl", localPlaylist);
        }
        else if (path == QLatin1String("/key"))
        {
            sendResponse(socket, 200, "OK", "application/octet-stream", key);
        }
        else if (path.startsWith(QLatin1String("/seg/")))
        {
            bool ok = false;
            const int index = path.mid(5).toInt(&ok);
            if (!ok || index < 0 || index >= static_cast<int>(states.size()))
            {
                sendResponse(socket, 404, "Not Found", "text/plain", "no such segment");
                return;
            }

            if (!ensureSegment(index))
            {
                sendResponse(socket, 503, "Service Unavailable", "text/plain", "segment unavailable");
                return;
            }

            QFile file(segmentPath(index));
            if (!file.open(QIODevice::ReadOnly))
            {
                sendResponse(socket, 500, "Internal Server Error", "text/plain", "cannot read segment");
                return;
            }

            sendResponse(socket, 200, "OK", "video/mp2t", file.readAll());
        }
        else
        {
            sendResponse(socket, 404, "Not Found", "text/plain", "no such resource");
        }

        socket->disconnectFromHost();
    }

    void acceptLoop(std::promise<quint16> port)
    {
        server.reset(new QTcpServer());
        if (!server->listen(QHostAddress::LocalHost, 0))
        {
            port.set_value(0);
            return;
        }

        port.set_value(server->serverPort());
        while (!stop.load())
        {
            if (!server->waitForNewConnection(500))
                continue;

            QTcpSocket* socket = server->nextPendingConnection();
            if (!socket)
                continue;

            handleConnection(socket);
            socket->close();
            delete socket;
        }
    }

    QByteArray buildLocalPlaylist(quint16 port)
    {
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);
        QByteArray text = "#EXTM3U\n#EXT-X-VERSION:3\n";
        text += "#EXT-X-PLAYLIST-TYPE:VOD\n";
        text += "#EXT-X-TARGETDURATION:" + QByteArray::number(qCeil(playlist.maxDuration)) + "\n";
        text += "#EXT-X-MEDIA-SEQUENCE:0\n";
        if (!key.isEmpty())
        {
            text += "#EXT-X-KEY:METHOD=AES-128,URI=\"" + (base + "/key").toUtf8() + "\"";
            if (!playlist.keyIv.isEmpty())
                text += ",IV=" + playlist.keyIv.toUtf8();
            text += "\n";
        }

        for (int i = 0; i < playlist.segments.size(); ++i)
        {
            const Segment& segment = playlist.segments.at(i);
            if (segment.discontinuityBefore)
                text += "#EXT-X-DISCONTINUITY\n";
            text += "#EXTINF:" + QByteArray::number(segment.duration, 'f', 6) + ",\n";
            text += (base + "/seg/" + QString::number(i)).toUtf8() + "\n";
        }

        text += "#EXT-X-ENDLIST\n";
        return text;
    }

    static constexpr quint64 kUnlimited = ~quint64(0);

    /** Loads a file that the source keeps alongside the segments (the playlist or the key). */
    static QByteArray readCachedFile(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};

        return file.readAll();
    }

    static bool writeCachedFile(const QString& path, const QByteArray& data)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;

        file.write(data);
        file.close();
        return true;
    }
};

HlsVodSource::HlsVodSource(std::unique_ptr<Private> d): d(std::move(d)) {}

HlsVodSource::~HlsVodSource()
{
    stop();
}

QString HlsVodSource::url() const
{
    return d ? d->localUrl : QString();
}

QString HlsVodSource::cacheFolder() const
{
    return d ? d->folder : QString();
}

std::unique_ptr<HlsVodSource> HlsVodSource::create(
    const QString& playlistUrl,
    const QString& cacheFolder,
    qint64 cacheLimitBytes,
    int connections,
    QString* errorOut)
{
    const auto fail = [errorOut](const QString& reason) -> std::unique_ptr<HlsVodSource>
    {
        if (errorOut)
            *errorOut = reason;
        return nullptr;
    };

    if (playlistUrl.isEmpty() || cacheFolder.isEmpty())
        return fail("No playlist URL or cache folder.");

    auto d = std::make_unique<Private>();
    d->playlistUrl = playlistUrl;
    d->connections = qBound(1, connections, 32);
    d->cacheFolderPath = cacheFolder;
    d->cacheLimitBytes = cacheLimitBytes;

    QElapsedTimer timer;
    timer.start();

    // The playlist identifies the cached copy; the URL may carry credentials, so only its digest is
    // used on disk. The playlist and the key are kept next to the segments so that a replay does not
    // need the network at all.
    d->folder = cacheFolderFor(cacheFolder, playlistUrl);
    if (!QDir().mkpath(d->folder))
        return fail("Cannot create the cache folder.");

    const QString cachedPlaylistPath = QDir(d->folder).absoluteFilePath("playlist.m3u8");
    const QString cachedKeyPath = QDir(d->folder).absoluteFilePath("key.bin");

    const QByteArray cachedPlaylist = Private::readCachedFile(cachedPlaylistPath);
    QByteArray playlistText = cachedPlaylist;
    if (playlistText.isEmpty())
    {
        // Opening the playlist must not wait for the network when a copy is already cached. A finite
        // playlist never changes, so the cached copy is as good as a fresh one.
        if (!httpGet(playlistUrl, &d->stop, kPlaylistTimeoutUs, &playlistText)
            || playlistText.isEmpty())
        {
            return fail("Cannot download the playlist.");
        }

        Private::writeCachedFile(cachedPlaylistPath, playlistText);
    }

    if (!parsePlaylist(playlistUrl, QString::fromUtf8(playlistText), &d->playlist))
        return fail("Not a supported VOD HLS playlist.");

    if (!d->playlist.keyUrl.isEmpty())
    {
        // The key is cached next to the segments, so a replay needs no network at all.
        QByteArray key = Private::readCachedFile(cachedKeyPath);
        if (key.isEmpty() && !httpGet(d->playlist.keyUrl, &d->stop, kPlaylistTimeoutUs, &key))
            return fail("Cannot download the encryption key.");

        if (key.isEmpty())
            return fail("Cannot obtain the encryption key.");

        d->key = key;
        Private::writeCachedFile(cachedKeyPath, key);
    }

    // Segments downloaded by an earlier playback are reused as they are.
    d->states.assign(d->playlist.segments.size(), Pending);
    d->attempts.assign(d->playlist.segments.size(), 0);

    // The first segment is what a probe waits for, so it is left to the on-demand ranged download
    // rather than being shared between the prefetch workers.
    d->nextIndex = d->playlist.segments.size() > 1 ? 1 : 0;
    int alreadyCached = 0;
    for (int i = 0; i < d->playlist.segments.size(); ++i)
    {
        const QFileInfo file(d->segmentPath(i));
        if (file.exists() && file.size() > 0)
        {
            d->states[i] = Ready;
            ++alreadyCached;
        }
    }

    // Measuring the whole cache folder is only worth it when something still has to be downloaded;
    // the size is also what decides whether the limit has been reached. It is measured on the first
    // download instead of here, so that creating a source stays cheap.

    NX_INFO(typeid(HlsVodSource), "Serving VOD playlist '%1' from %2 with %3 connections: %4 segments, "
        "%5 already cached, ready in %6 ms.",
        playlistUrl, d->folder, d->connections, d->playlist.segments.size(), alreadyCached,
        timer.elapsed());

    // The server is created inside its own thread, where it is used; the port it binds is needed to
    // build the playlist it serves.
    std::promise<quint16> port;
    std::future<quint16> boundPort = port.get_future();
    d->acceptThread = std::thread([raw = d.get(), port = std::move(port)]() mutable
    {
        raw->acceptLoop(std::move(port));
    });
    const quint16 localPort = boundPort.get();
    if (localPort == 0)
    {
        d->stop = true;
        if (d->acceptThread.joinable())
            d->acceptThread.join();
        return fail("Cannot start the local server.");
    }

    d->localPlaylist = d->buildLocalPlaylist(localPort);
    d->localUrl = QStringLiteral("http://127.0.0.1:%1/playlist.m3u8").arg(localPort);

    for (int i = 0; i < d->connections; ++i)
    {
        d->workers.emplace_back([raw = d.get()]() { raw->workerLoop(); });
    }

    return std::unique_ptr<HlsVodSource>(new HlsVodSource(std::move(d)));
}

namespace {

/** Sources are kept alive after their playback ends; only so many playlists are remembered. */
constexpr int kMaxTrackedSources = 4;

struct Registry
{
    std::mutex mutex;
    QHash<QString, std::shared_ptr<HlsVodSource>> sources;
    QList<QString> order; //< Insertion order, the oldest first.
};

Registry& registry()
{
    static Registry instance;
    return instance;
}

} // namespace

std::shared_ptr<HlsVodSource> HlsVodSource::acquire(
    const QString& playlistUrl,
    const QString& cacheFolder,
    qint64 cacheLimitBytes,
    int connections,
    QString* errorOut)
{
    Registry& reg = registry();
    {
        const std::lock_guard<std::mutex> lock(reg.mutex);
        const auto it = reg.sources.find(playlistUrl);
        if (it != reg.sources.end())
            return it.value();
    }

    std::unique_ptr<HlsVodSource> created = create(
        playlistUrl, cacheFolder, cacheLimitBytes, connections, errorOut);
    if (!created)
        return nullptr;

    std::shared_ptr<HlsVodSource> source(created.release());
    const std::lock_guard<std::mutex> lock(reg.mutex);

    // The sources left behind by the playlists that are no longer used are stopped here, which also
    // releases their downloads and their local server.
    while (reg.sources.size() >= kMaxTrackedSources && !reg.order.isEmpty())
        reg.sources.remove(reg.order.takeFirst());

    reg.sources.insert(playlistUrl, source);
    reg.order.append(playlistUrl);
    return source;
}

bool HlsVodSource::isServing(const QString& playlistUrl)
{
    Registry& reg = registry();
    const std::lock_guard<std::mutex> lock(reg.mutex);
    return reg.sources.contains(playlistUrl);
}

bool HlsVodSource::hasCachedPlaylist(const QString& playlistUrl, const QString& cacheFolder)
{
    if (playlistUrl.isEmpty() || cacheFolder.isEmpty())
        return false;

    return QFile::exists(
        QDir(cacheFolderFor(cacheFolder, playlistUrl)).absoluteFilePath("playlist.m3u8"));
}

bool HlsVodSource::isProbeReady(const QString& playlistUrl, const QString& cacheFolder)
{
    if (playlistUrl.isEmpty() || cacheFolder.isEmpty())
        return false;

    // A demuxer reads a little more than the first segment to identify the streams, so the beginning
    // of the media has to be there, not just the playlist.
    const QDir folder(cacheFolderFor(cacheFolder, playlistUrl));
    return QFile::exists(folder.absoluteFilePath("0.ts"))
        && QFile::exists(folder.absoluteFilePath("1.ts"));
}

void HlsVodSource::prepareInBackground(
    const QString& playlistUrl,
    const QString& cacheFolder,
    qint64 cacheLimitBytes,
    int connections)
{
    if (playlistUrl.isEmpty() || cacheFolder.isEmpty())
        return;

    // Deliberately leaked: a detached thread may still be using them while the library is unloaded.
    static std::mutex* const preparingMutex = new std::mutex();
    static QSet<QString>* const preparing = new QSet<QString>();
    {
        const std::lock_guard<std::mutex> lock(*preparingMutex);
        if (preparing->contains(playlistUrl))
            return;

        preparing->insert(playlistUrl);
    }

    std::thread([playlistUrl, cacheFolder, cacheLimitBytes, connections]
    {
        QString error;
        const bool prepared = acquire(playlistUrl, cacheFolder, cacheLimitBytes, connections, &error)
            != nullptr;
        if (!prepared)
        {
            NX_WARNING(typeid(HlsVodSource), "Cannot prepare '%1': %2.", playlistUrl, error);
        }

        const std::lock_guard<std::mutex> lock(*preparingMutex);
        preparing->remove(playlistUrl); //< A later attempt may well succeed.
    }).detach();
}

void HlsVodSource::stop()
{
    if (!d)
        return;

    d->stop = true;
    d->condition.notify_all();

    if (d->server)
        d->server->close();

    if (d->acceptThread.joinable())
        d->acceptThread.join();

    for (std::thread& worker: d->workers)
    {
        if (worker.joinable())
            worker.join();
    }

    d->workers.clear();
    d.reset();
}

} // namespace nx::vms::client::desktop
