// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <memory>

#include <QtCore/QObject>
#include <QtCore/QString>

struct AVIOContext;

namespace nx::vms::client::desktop {

class LocalSettings;
class StreamCacheStorage;

/**
 * Random access reader over a cached stream resource. The requested byte ranges are served from the
 * disk cache; missing ones are downloaded with HTTP range requests and stored for later use, so a
 * seek or a replay does not download the same data twice.
 *
 * A reader is owned by a single playback; it is not meant to be shared between threads.
 */
class NX_VMS_CLIENT_DESKTOP_API StreamCacheReader
{
public:
    StreamCacheReader(const StreamCacheReader&) = delete;
    StreamCacheReader& operator=(const StreamCacheReader&) = delete;
    ~StreamCacheReader();

    /** FFmpeg input the resource is read through. Owned by the reader. */
    AVIOContext* avioContext() const;

    /** Total size of the resource, in bytes. */
    qint64 size() const;

    /**
     * Asks the reader to give up as soon as possible. Pending and subsequent reads fail, which
     * unblocks playback shutdown.
     */
    void requestStop();

private:
    friend class StreamCache;

    StreamCacheReader(const QString& url, const std::shared_ptr<StreamCacheStorage>& storage);

    static int readCallback(void* opaque, unsigned char* buffer, int bufferSize);
    static int64_t seekCallback(void* opaque, int64_t offset, int whence);

    struct Private;
    std::unique_ptr<Private> d;
};

/**
 * Disk cache of the streamed media files played back by the client.
 *
 * Only sources with a finite duration are cached; a live stream is not, because it would grow
 * without bound and cannot be seeked to anyway. The cached data is never removed automatically: the
 * user picks the folder and clears it in the settings.
 */
class NX_VMS_CLIENT_DESKTOP_API StreamCache: public QObject
{
    Q_OBJECT

public:
    explicit StreamCache(LocalSettings* settings, QObject* parent = nullptr);
    virtual ~StreamCache() override;

    /** Folder the cached data is written to when the user has not chosen one. */
    static QString defaultFolder();

    /** Folder the cached data is written to; configurable in the settings. */
    QString folder() const;

    /** Upper bound of the cache size, in bytes, as configured by the user. */
    qint64 limitBytes() const;

    /** Total size of the cached data, in bytes. */
    qint64 usedBytes() const;

    /** Whether new data may be cached: the folder is usable and the limit is not exceeded. */
    bool writable() const;

    /**
     * Opens the cache of the given resource. Returns null when the resource must not be cached, so
     * that the caller falls back to reading it directly.
     */
    std::shared_ptr<StreamCacheReader> open(const QString& url, qint64 sizeBytes);

    /** Deletes all the cached data. */
    void clear();

signals:
    void changed();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace nx::vms::client::desktop
