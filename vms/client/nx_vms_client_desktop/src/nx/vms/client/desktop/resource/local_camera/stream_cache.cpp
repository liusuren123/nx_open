// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "stream_cache.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QStandardPaths>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <nx/utils/file_system.h>
#include <nx/utils/log/log.h>
#include <nx/vms/client/desktop/settings/local_settings.h>

#include "local_camera_source.h"

namespace nx::vms::client::desktop {

namespace {

/** The cached data is stored, and downloaded from the network, one block at a time. */
constexpr int kBlockSize = 1024 * 1024;

/** How many blocks ahead of the playhead are downloaded in the background. */
constexpr int kReadAheadBlocks = 8;

/** Size of the buffer FFmpeg reads the resource through. */
constexpr int kAvioBufferSize = 64 * 1024;

constexpr int kDefaultLimitGigabytes = 50;

/** Fraction of the index file header that records the resource size. */
constexpr int kIndexHeaderSize = sizeof(qint64);

QByteArray encodedSize(qint64 size)
{
    // A fixed layout keeps the header comparable across runs and platforms.
    QByteArray result(kIndexHeaderSize, '\0');
    for (int i = 0; i < kIndexHeaderSize; ++i)
        result[i] = char((quint64(size) >> (8 * i)) & 0xff);

    return result;
}

QString defaultFolder()
{
    const QStringList locations =
        QStandardPaths::standardLocations(QStandardPaths::AppLocalDataLocation);
    if (locations.isEmpty())
        return {};

    return QDir(locations.first()).absoluteFilePath("stream_cache");
}

qint64 folderSizeBytes(const QString& path)
{
    const QDir folder(path);
    if (!folder.exists())
        return 0;

    qint64 result = 0;
    const QFileInfoList entries =
        folder.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry: entries)
        result += entry.isDir() ? folderSizeBytes(entry.absoluteFilePath()) : entry.size();

    return result;
}

int interruptCallback(void* opaque)
{
    return static_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}

/** Options a resource is downloaded with; the same ones playback uses. */
void setSourceOptions(AVDictionary** options, const QString& url)
{
    av_dict_set(options, "rw_timeout", "15000000", 0);
    av_dict_set(options, "reconnect", "1", 0);
    av_dict_set(options, "reconnect_streamed", "1", 0);
    av_dict_set(options, "reconnect_delay_max", "4", 0);

    const HttpSourceHeaders headers = httpSourceHeaders(url);
    av_dict_set(options, "user_agent", headers.userAgent.toUtf8().constData(), 0);
    if (!headers.referer.isEmpty())
        av_dict_set(options, "referer", headers.referer.toUtf8().constData(), 0);
}

/** Path of a cache file of the resource; the URL may carry credentials, so it is hashed. */
QString cacheFilePath(const QString& folder, const QString& url, const char* suffix)
{
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    return QDir(folder).absoluteFilePath(name + QLatin1String(suffix));
}

/** Size of a remote resource, or 0 when it cannot be told without downloading it. */
qint64 remoteSize(const QString& url, std::atomic<bool>* stop)
{
    AVIOContext* source = nullptr;
    AVDictionary* options = nullptr;
    setSourceOptions(&options, url);
    AVIOInterruptCB interrupt{&interruptCallback, stop};
    const int result = avio_open2(
        &source, url.toUtf8().constData(), AVIO_FLAG_READ, &interrupt, &options);
    av_dict_free(&options);

    if (result < 0)
        return 0;

    const qint64 size = avio_size(source);
    avio_close(source);
    return size;
}

/** Copies a remote resource into a file, adding the bytes read to the counter. */
bool downloadToFile(const QString& url, const QString& path, std::atomic<bool>* stop,
    std::atomic<qint64>* done)
{
    AVIOContext* source = nullptr;
    AVDictionary* options = nullptr;
    setSourceOptions(&options, url);
    AVIOInterruptCB interrupt{&interruptCallback, stop};
    const int result = avio_open2(
        &source, url.toUtf8().constData(), AVIO_FLAG_READ, &interrupt, &options);
    av_dict_free(&options);

    if (result < 0)
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        avio_close(source);
        return false;
    }

    bool ok = true;
    std::vector<unsigned char> buffer(256 * 1024);
    while (!stop->load())
    {
        const int read = avio_read(source, buffer.data(), int(buffer.size()));
        if (read == AVERROR_EOF)
            break;

        if (read < 0)
        {
            ok = false;
            break;
        }

        if (read == 0)
            break;

        if (file.write(reinterpret_cast<const char*>(buffer.data()), read) != read)
        {
            ok = false;
            break;
        }

        done->fetch_add(read);
    }

    avio_close(source);
    file.close();

    if (!ok || stop->load())
    {
        QFile::remove(path);
        return false;
    }

    return true;
}

/** Timestamp a packet is ordered by; the decode one when it is there, the presentation one else. */
qint64 packetTimestamp(const AVPacket* packet)
{
    return packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
}

/**
 * Remuxes a video-only file and an audio-only file into one MP4. Nothing is re-encoded: the tracks
 * are already exactly what the provider handed out.
 */
bool mergeTracks(const QString& videoPath, const QString& audioPath, const QString& outputPath)
{
    AVFormatContext* videoIn = nullptr;
    AVFormatContext* audioIn = nullptr;
    AVFormatContext* out = nullptr;
    AVPacket* videoPacket = av_packet_alloc();
    AVPacket* audioPacket = av_packet_alloc();
    bool ok = false;

    do
    {
        if (!videoPacket || !audioPacket)
            break;

        if (avformat_open_input(&videoIn, videoPath.toUtf8().constData(), nullptr, nullptr) < 0)
            break;
        if (avformat_find_stream_info(videoIn, nullptr) < 0)
            break;
        if (avformat_open_input(&audioIn, audioPath.toUtf8().constData(), nullptr, nullptr) < 0)
            break;
        if (avformat_find_stream_info(audioIn, nullptr) < 0)
            break;

        const int videoIndex =
            av_find_best_stream(videoIn, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        const int audioIndex =
            av_find_best_stream(audioIn, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (videoIndex < 0 || audioIndex < 0)
            break;

        if (avformat_alloc_output_context2(
            &out, nullptr, nullptr, outputPath.toUtf8().constData()) < 0 || !out)
        {
            break;
        }

        AVStream* videoOut = avformat_new_stream(out, nullptr);
        AVStream* audioOut = avformat_new_stream(out, nullptr);
        if (!videoOut || !audioOut)
            break;

        if (avcodec_parameters_copy(videoOut->codecpar, videoIn->streams[videoIndex]->codecpar) < 0
            || avcodec_parameters_copy(audioOut->codecpar, audioIn->streams[audioIndex]->codecpar) < 0)
        {
            break;
        }

        videoOut->codecpar->codec_tag = 0;
        audioOut->codecpar->codec_tag = 0;
        videoOut->time_base = videoIn->streams[videoIndex]->time_base;
        audioOut->time_base = audioIn->streams[audioIndex]->time_base;

        if (avio_open(&out->pb, outputPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0)
            break;
        if (avformat_write_header(out, nullptr) < 0)
            break;

        // Reads the next packet of one track into its packet, rescaling it to the output stream.
        const auto readNext = [](AVFormatContext* in, int streamIndex, AVStream* outStream,
            AVPacket* packet, bool* done, bool* have)
        {
            *have = false;
            if (*done)
                return;

            while (av_read_frame(in, packet) >= 0)
            {
                if (packet->stream_index != streamIndex)
                {
                    av_packet_unref(packet);
                    continue;
                }

                av_packet_rescale_ts(packet, in->streams[streamIndex]->time_base,
                    outStream->time_base);
                packet->stream_index = outStream->index;
                *have = true;
                return;
            }

            *done = true;
        };

        bool videoDone = false;
        bool audioDone = false;
        bool haveVideo = false;
        bool haveAudio = false;
        bool written = true;

        readNext(videoIn, videoIndex, videoOut, videoPacket, &videoDone, &haveVideo);
        readNext(audioIn, audioIndex, audioOut, audioPacket, &audioDone, &haveAudio);

        while (haveVideo || haveAudio)
        {
            const bool takeVideo = haveVideo && (!haveAudio
                || packetTimestamp(videoPacket) <= packetTimestamp(audioPacket));

            if (takeVideo)
            {
                written = av_interleaved_write_frame(out, videoPacket) >= 0;
                if (!written)
                    break;

                readNext(videoIn, videoIndex, videoOut, videoPacket, &videoDone, &haveVideo);
            }
            else
            {
                written = av_interleaved_write_frame(out, audioPacket) >= 0;
                if (!written)
                    break;

                readNext(audioIn, audioIndex, audioOut, audioPacket, &audioDone, &haveAudio);
            }
        }

        ok = written && av_write_trailer(out) >= 0;
    } while (false);

    av_packet_free(&videoPacket);
    av_packet_free(&audioPacket);

    if (out)
    {
        if (out->pb)
            avio_closep(&out->pb);
        avformat_free_context(out);
    }
    if (videoIn)
        avformat_close_input(&videoIn);
    if (audioIn)
        avformat_close_input(&audioIn);

    return ok;
}

} // namespace

/**
 * On-disk cache of a single resource. Every reader of the same URL shares one storage, so two
 * layouts showing the same stream download it once.
 *
 * The data file holds the resource verbatim; which of its blocks have been downloaded is recorded
 * in a separate index file, so a partially cached resource survives a restart and is never
 * downloaded twice.
 */
class StreamCacheStorage
{
public:
    StreamCacheStorage(const QString& dataPath, const QString& indexPath, qint64 sizeBytes);
    ~StreamCacheStorage();

    bool isValid() const { return m_valid; }
    qint64 size() const { return m_size; }
    int blockCount() const { return m_blockCount; }

    /** Number of bytes of block #index; the last block may be shorter than the others. */
    int blockSize(int index) const;

    bool hasBlock(int index) const;
    bool tryReadBlock(int index, QByteArray* data) const;
    void writeBlock(int index, const QByteArray& data);

    /**
     * Removes the cached files as soon as this storage is released. The files of a resource that is
     * being played are open, and Windows does not remove an open file, so the removal is deferred
     * until playback of it stops.
     */
    void removeOnRelease();

private:
    bool isPresent(int index) const;

    const QString m_dataPath;
    const QString m_indexPath;

    mutable QFile m_data;
    mutable QFile m_index;

    const qint64 m_size = 0;
    const int m_blockCount = 0;
    bool m_valid = false;
    std::atomic<bool> m_removeOnRelease{false};

    mutable std::mutex m_mutex;
};

StreamCacheStorage::StreamCacheStorage(
    const QString& dataPath, const QString& indexPath, qint64 sizeBytes):
    m_dataPath(dataPath),
    m_indexPath(indexPath),
    m_data(dataPath),
    m_index(indexPath),
    m_size(sizeBytes),
    m_blockCount(qMax(1, int((sizeBytes + kBlockSize - 1) / kBlockSize)))
{
    const QByteArray header = encodedSize(sizeBytes);

    // Reuse the cached data only when it was made from exactly this resource; otherwise the stale
    // file would be served for a URL whose content changed.
    bool reuse = false;
    QFile index(indexPath);
    if (QFileInfo(dataPath).size() > 0 && index.open(QIODevice::ReadOnly))
    {
        reuse = index.read(kIndexHeaderSize) == header
            && index.size() == kIndexHeaderSize + qint64(m_blockCount);
        index.close();
    }

    if (!reuse)
    {
        QFile::remove(dataPath);
        QFile::remove(indexPath);
    }

    if (!nx::utils::file_system::ensureDir(QFileInfo(dataPath).absoluteDir()))
        return;

    m_valid = m_data.open(QIODevice::ReadWrite) && m_index.open(QIODevice::ReadWrite);
    if (!m_valid)
    {
        NX_WARNING(typeid(StreamCache), "Failed to open the stream cache file '%1'.",
            dataPath);
        return;
    }

    if (reuse)
        return;

    // The data file is deliberately not preallocated: only the downloaded blocks take up space.
    m_index.write(header);
    m_index.write(QByteArray(m_blockCount, '\0'));
    m_index.flush();
}

StreamCacheStorage::~StreamCacheStorage()
{
    if (!m_removeOnRelease.load())
        return;

    // The files must be closed before they can be removed, and the members are still alive here.
    m_data.close();
    m_index.close();
    QFile::remove(m_dataPath);
    QFile::remove(m_indexPath);
}

void StreamCacheStorage::removeOnRelease()
{
    m_removeOnRelease.store(true);
}

int StreamCacheStorage::blockSize(int index) const
{
    const qint64 start = qint64(index) * kBlockSize;
    return int(qBound<qint64>(qint64(0), m_size - start, qint64(kBlockSize)));
}

bool StreamCacheStorage::isPresent(int index) const
{
    if (!m_index.seek(kIndexHeaderSize + index))
        return false;

    const QByteArray value = m_index.read(1);
    return value.size() == 1 && value.at(0) != 0;
}

bool StreamCacheStorage::hasBlock(int index) const
{
    if (!m_valid || index < 0 || index >= m_blockCount)
        return false;

    const std::lock_guard<std::mutex> lock(m_mutex);
    return isPresent(index);
}

bool StreamCacheStorage::tryReadBlock(int index, QByteArray* data) const
{
    if (!m_valid || !data || index < 0 || index >= m_blockCount)
        return false;

    const std::lock_guard<std::mutex> lock(m_mutex);
    if (!isPresent(index))
        return false;

    const int size = blockSize(index);
    if (!m_data.seek(qint64(index) * kBlockSize))
        return false;

    data->resize(size);
    return m_data.read(data->data(), size) == size;
}

void StreamCacheStorage::writeBlock(int index, const QByteArray& data)
{
    if (!m_valid || index < 0 || index >= m_blockCount || data.isEmpty())
        return;

    const std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_data.seek(qint64(index) * kBlockSize) || m_data.write(data) != data.size())
    {
        NX_WARNING(typeid(StreamCache), "Failed to write %1 bytes to the stream cache file '%2'.",
            data.size(), m_dataPath);
        return;
    }

    // Marking the block present last keeps a partial write from being served as valid data.
    if (!m_index.seek(kIndexHeaderSize + index) || m_index.write("\1", 1) != 1)
    {
        NX_WARNING(typeid(StreamCache), "Failed to update the stream cache index '%1'.",
            m_indexPath);
    }
}

struct StreamCacheReader::Private
{
    Private(const QString& url, const std::shared_ptr<StreamCacheStorage>& storage);

    const QString url;
    const std::shared_ptr<StreamCacheStorage> storage;

    /** Cancellation of the pending network operations, including the ones in the worker. */
    AVIOInterruptCB interrupt{};
    std::atomic<bool> stopRequested{false};

    /** Network source the missing blocks are downloaded from; opened on demand. */
    std::mutex sourceMutex;
    AVIOContext* source = nullptr;

    /** Read ahead state, guarded by workerMutex. */
    std::thread worker;
    std::mutex workerMutex;
    std::condition_variable workerCondition;
    int prefetchFrom = -1;
    bool stopping = false;

    /** FFmpeg input. The reader owns both the context and its buffer. */
    AVIOContext* avio = nullptr;
    unsigned char* avioBuffer = nullptr;

    /** Position the read callback serves the next byte from. */
    qint64 position = 0;
    QByteArray blockBuffer;

    int read(unsigned char* buffer, int bufferSize);
    int64_t seek(int64_t offset, int whence);
    bool ensureBlock(int index);
    bool fetchBlock(int index);
    bool openSource();
    void closeSource();
    bool openAvio();
    void startWorker();
    void stopWorker();
    void prefetchLoop();
    void updatePrefetch(int block);
};

StreamCacheReader::Private::Private(
    const QString& url, const std::shared_ptr<StreamCacheStorage>& storage):
    url(url),
    storage(storage)
{
    interrupt.callback = &interruptCallback;
    interrupt.opaque = &stopRequested;
}

int StreamCacheReader::Private::read(unsigned char* buffer, int bufferSize)
{
    if (stopRequested.load())
        return AVERROR_EXIT;

    const qint64 total = storage->size();
    int done = 0;
    const char* reason = "end of resource";

    while (done < bufferSize)
    {
        const qint64 current = position + done;
        if (current >= total)
            break;

        const int index = int(current / kBlockSize);
        if (!ensureBlock(index))
        {
            reason = "block unavailable";
            break;
        }

        if (!storage->tryReadBlock(index, &blockBuffer))
        {
            reason = "block unreadable";
            break;
        }

        const int offsetInBlock = int(current % kBlockSize);
        if (offsetInBlock >= blockBuffer.size())
        {
            reason = "short block";
            break;
        }

        const int chunk = qMin(bufferSize - done, blockBuffer.size() - offsetInBlock);
        memcpy(buffer + done, blockBuffer.constData() + offsetInBlock, chunk);
        done += chunk;
    }

    position += done;

    if (done > 0)
        updatePrefetch(int(position / kBlockSize));

    if (done > 0)
        return done;

    NX_DEBUG(typeid(StreamCacheReader), "No data at %1 of %2 bytes of '%3': %4",
        position, total, localCameraSourceUrlForDisplay(url), reason);

    return stopRequested.load() ? AVERROR_EXIT : AVERROR_EOF;
}

int64_t StreamCacheReader::Private::seek(int64_t offset, int whence)
{
    if (whence == AVSEEK_SIZE)
        return storage->size();

    if (stopRequested.load())
        return AVERROR_EXIT;

    whence &= ~AVSEEK_FORCE;

    qint64 target = 0;
    switch (whence)
    {
        case SEEK_SET:
            target = offset;
            break;

        case SEEK_CUR:
            target = position + offset;
            break;

        case SEEK_END:
            target = storage->size() + offset;
            break;

        default:
            return AVERROR(EINVAL);
    }

    if (target < 0)
        return AVERROR(EINVAL);

    position = target;
    updatePrefetch(int(target / kBlockSize));

    return target;
}

bool StreamCacheReader::Private::ensureBlock(int index)
{
    if (index < 0 || index >= storage->blockCount())
        return false;

    if (storage->hasBlock(index))
        return true;

    return fetchBlock(index);
}

bool StreamCacheReader::Private::fetchBlock(int index)
{
    // Downloads are serialized: the source is a single connection that is seeked around.
    const std::lock_guard<std::mutex> lock(sourceMutex);

    // The worker may have downloaded it while this thread waited for the lock.
    if (storage->hasBlock(index))
        return true;

    const qint64 offset = qint64(index) * kBlockSize;
    const int expected = storage->blockSize(index);

    QByteArray data;

    // The connection is reused when possible and reopened once on failure: a server that drops an
    // idle connection must not break playback.
    for (int attempt = 0; attempt < 2 && !stopRequested.load(); ++attempt)
    {
        if (!openSource())
            return false;

        if (avio_seek(source, offset, SEEK_SET) < 0)
        {
            closeSource();
            continue;
        }

        data.resize(expected);
        int total = 0;
        while (total < expected && !stopRequested.load())
        {
            const int read = avio_read(source,
                reinterpret_cast<unsigned char*>(data.data()) + total, expected - total);
            if (read <= 0)
                break;

            total += read;
        }

        if (total == expected)
            break;

        data.clear();
        closeSource();
    }

    if (data.size() != expected)
    {
        if (!stopRequested.load())
        {
            NX_DEBUG(typeid(StreamCacheReader), "Failed to download %1 bytes at offset %2 of '%3'.",
                expected, offset, localCameraSourceUrlForDisplay(url));
        }

        return false;
    }

    storage->writeBlock(index, data);
    return true;
}

bool StreamCacheReader::Private::openSource()
{
    if (source)
        return true;

    AVDictionary* options = nullptr;
    setSourceOptions(&options, url);

    const int result = avio_open2(
        &source, url.toUtf8().constData(), AVIO_FLAG_READ, &interrupt, &options);
    av_dict_free(&options);

    if (result < 0)
    {
        source = nullptr;
        NX_DEBUG(typeid(StreamCacheReader), "Failed to open '%1' for caching: %2",
            localCameraSourceUrlForDisplay(url), result);
        return false;
    }

    return true;
}

void StreamCacheReader::Private::closeSource()
{
    if (source)
        avio_closep(&source);

    source = nullptr;
}

bool StreamCacheReader::Private::openAvio()
{
    avioBuffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (!avioBuffer)
        return false;

    avio = avio_alloc_context(avioBuffer, kAvioBufferSize, /*write_flag*/ 0, this,
        &StreamCacheReader::readCallback, /*write_packet*/ nullptr,
        &StreamCacheReader::seekCallback);
    if (!avio)
    {
        av_free(avioBuffer);
        avioBuffer = nullptr;
        return false;
    }

    // The cached resource is always randomly accessible; this is what makes seeking work.
    avio->seekable = AVIO_SEEKABLE_NORMAL;
    return true;
}

void StreamCacheReader::Private::startWorker()
{
    worker = std::thread([this] { prefetchLoop(); });
}

void StreamCacheReader::Private::stopWorker()
{
    {
        const std::lock_guard<std::mutex> lock(workerMutex);
        stopping = true;
    }

    workerCondition.notify_all();
    if (worker.joinable())
        worker.join();
}

void StreamCacheReader::Private::updatePrefetch(int block)
{
    if (block < 0)
        return;

    {
        const std::lock_guard<std::mutex> lock(workerMutex);
        if (prefetchFrom == block + 1)
            return;

        prefetchFrom = block + 1;
    }

    workerCondition.notify_all();
}

void StreamCacheReader::Private::prefetchLoop()
{
    std::unique_lock<std::mutex> lock(workerMutex);

    for (;;)
    {
        workerCondition.wait(lock, [this]
            {
                return stopping || stopRequested.load() || prefetchFrom >= 0;
            });

        if (stopping || stopRequested.load())
            return;

        const int from = prefetchFrom;
        lock.unlock();

        const int last = qMin(from + kReadAheadBlocks, storage->blockCount() - 1);
        for (int index = from; index <= last; ++index)
        {
            if (stopRequested.load())
                return;

            if (!ensureBlock(index))
                break;

            // A reposition during the pass is picked up by the next iteration below.
        }

        lock.lock();
        if (stopping || stopRequested.load())
            return;

        // Idle until the playhead moves again; the pass itself is not worth repeating.
        if (prefetchFrom == from)
            prefetchFrom = -1;
    }
}

StreamCacheReader::StreamCacheReader(
    const QString& url, const std::shared_ptr<StreamCacheStorage>& storage):
    d(new Private(url, storage))
{
    if (d->openAvio())
        d->startWorker();
}

StreamCacheReader::~StreamCacheReader()
{
    requestStop();
    d->stopWorker();
    d->closeSource();

    // FFmpeg frees the buffer along with the context, so it must not be released here.
    if (d->avio)
        avio_context_free(&d->avio);
}

AVIOContext* StreamCacheReader::avioContext() const
{
    return d->avio;
}

qint64 StreamCacheReader::size() const
{
    return d->storage->size();
}

void StreamCacheReader::requestStop()
{
    d->stopRequested.store(true);
    d->workerCondition.notify_all();
}

int StreamCacheReader::readCallback(void* opaque, unsigned char* buffer, int bufferSize)
{
    const auto d = static_cast<Private*>(opaque);
    return d->read(buffer, bufferSize);
}

int64_t StreamCacheReader::seekCallback(void* opaque, int64_t offset, int whence)
{
    const auto d = static_cast<Private*>(opaque);
    return d->seek(offset, whence);
}

struct StreamCache::Private
{
    Private(StreamCache* q, LocalSettings* settings): q(q), settings(settings) {}

    /** A background download of a whole resource, started from the playlist context menu. */
    struct CachingJob
    {
        CachingJob(StreamCache* cache, const QString& url): cache(cache), url(url) {}

        StreamCache* const cache = nullptr;
        const QString url;

        /** Set for a job that downloads two tracks and merges them into one file. */
        bool merged = false;
        QString videoUrl;
        QString audioUrl;
        QString group;

        std::atomic<bool> stop{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> failed{false};

        /** Blocks of the resource, or bytes of the two tracks when they are merged. */
        std::atomic<qint64> done{0};
        std::atomic<qint64> total{0};

        std::thread thread;
    };

    /** Storage of the resource that is created on demand and shared by its readers. */
    std::shared_ptr<StreamCacheStorage> storageFor(const QString& url, qint64 sizeBytes);

    /** Body of the background thread; must not be called before the job is in the map. */
    void runCaching(const std::shared_ptr<CachingJob>& job);

    /** Body of the background thread for the jobs that merge two tracks. */
    void runMerged(const std::shared_ptr<CachingJob>& job);

    StreamCache* const q = nullptr;
    LocalSettings* const settings = nullptr;

    /** One storage per cache file, shared by the readers of the same resource. */
    std::mutex mutex;
    QHash<QString, std::weak_ptr<StreamCacheStorage>> storages;

    /** Downloads in progress, one per resource. */
    QHash<QString, std::shared_ptr<CachingJob>> jobs;
};

std::shared_ptr<StreamCacheStorage> StreamCache::Private::storageFor(
    const QString& url, qint64 sizeBytes)
{
    if (url.isEmpty() || sizeBytes <= 0)
        return nullptr;

    const QString folder = q->folder();
    const QString dataPath = cacheFilePath(folder, url, ".stream");

    std::shared_ptr<StreamCacheStorage> storage;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        storage = storages.value(dataPath).lock();
        if (!storage)
        {
            storage = std::make_shared<StreamCacheStorage>(
                dataPath, cacheFilePath(folder, url, ".index"), sizeBytes);
            if (!storage->isValid())
                return nullptr;

            storages.insert(dataPath, storage);
        }
    }

    if (storage->size() != sizeBytes)
    {
        NX_WARNING(typeid(StreamCache), "Cached size of '%1' changed from %2 to %3 bytes.",
            localCameraSourceUrlForDisplay(url), storage->size(), sizeBytes);
        return nullptr;
    }

    return storage;
}

void StreamCache::Private::runCaching(const std::shared_ptr<CachingJob>& job)
{
    if (job->merged)
    {
        runMerged(job);
        return;
    }

    StreamCache* const cache = job->cache;
    const QString url = job->url;
    const QString displayUrl = localCameraSourceUrlForDisplay(url);

    AVIOInterruptCB interrupt{&interruptCallback, const_cast<std::atomic<bool>*>(&job->stop)};

    // The size of the resource has to be known before the cache can be laid out on disk.
    qint64 size = 0;
    {
        AVIOContext* probe = nullptr;
        AVDictionary* options = nullptr;
        setSourceOptions(&options, url);
        const int result = avio_open2(
            &probe, url.toUtf8().constData(), AVIO_FLAG_READ, &interrupt, &options);
        av_dict_free(&options);

        if (result < 0)
        {
            NX_WARNING(typeid(StreamCache), "Cannot cache '%1': it cannot be opened (%2).",
                displayUrl, result);
            job->failed = true;
            job->finished = true;
            emit cache->cachingChanged(url);
            return;
        }

        size = avio_size(probe);
        avio_close(probe);
    }

    if (size <= 0)
    {
        // A live stream, or a playlist whose media is fetched separately: there is nothing finite
        // to download once and keep.
        NX_WARNING(typeid(StreamCache), "Cannot cache '%1': its size is unknown.", displayUrl);
        job->failed = true;
        job->finished = true;
        emit cache->cachingChanged(url);
        return;
    }

    std::shared_ptr<StreamCacheStorage> storage = storageFor(url, size);
    if (!storage)
    {
        job->failed = true;
        job->finished = true;
        emit cache->cachingChanged(url);
        return;
    }

    const int blocks = storage->blockCount();
    job->total = blocks;

    AVIOContext* source = nullptr;
    const auto openSource = [&source, &url, &interrupt]() -> bool
    {
        if (source)
            return true;

        AVDictionary* options = nullptr;
        setSourceOptions(&options, url);
        const int result = avio_open2(
            &source, url.toUtf8().constData(), AVIO_FLAG_READ, &interrupt, &options);
        av_dict_free(&options);

        if (result < 0)
            source = nullptr;

        return source != nullptr;
    };

    int done = 0;
    bool failed = false;
    for (int index = 0; index < blocks && !job->stop; ++index)
    {
        if (storage->hasBlock(index))
        {
            job->done = ++done;
            continue;
        }

        const qint64 offset = qint64(index) * kBlockSize;
        const int expected = storage->blockSize(index);

        QByteArray data;

        // One reconnect on failure, like playback does: a server that drops an idle connection must
        // not end the download.
        for (int attempt = 0; attempt < 2 && !job->stop; ++attempt)
        {
            if (!openSource())
                break;

            if (avio_seek(source, offset, SEEK_SET) < 0)
            {
                avio_close(source);
                source = nullptr;
                continue;
            }

            data.resize(expected);
            int total = 0;
            while (total < expected && !job->stop)
            {
                const int read = avio_read(source,
                    reinterpret_cast<unsigned char*>(data.data()) + total, expected - total);
                if (read <= 0)
                    break;

                total += read;
            }

            if (total == expected)
                break;

            data.clear();
            avio_close(source);
            source = nullptr;
        }

        if (job->stop)
            break;

        if (data.size() != expected)
        {
            NX_WARNING(typeid(StreamCache), "Cannot cache '%1': block %2 was not downloaded.",
                displayUrl, index);
            failed = true;
            break;
        }

        storage->writeBlock(index, data);
        job->done = ++done;

        // The interface shows the progress, and redrawing it for every block is pointless.
        if ((index % 16) == 0)
            emit cache->cachingChanged(url);
    }

    if (source)
        avio_close(source);

    if (!failed && !job->stop)
    {
        NX_INFO(typeid(StreamCache), "Cached '%1' completely (%2 bytes).",
            displayUrl, size);
    }

    job->failed = failed;
    job->finished = true;
    emit cache->cachingChanged(url);
}

void StreamCache::Private::runMerged(const std::shared_ptr<CachingJob>& job)
{
    StreamCache* const cache = job->cache;
    const QString displayUrl = localCameraSourceUrlForDisplay(job->url);

    const auto finish = [&](bool ok)
    {
        job->failed = !ok;
        job->finished = true;
        emit cache->cachingChanged(job->url);
    };

    const QString folder = q->folder();
    const QString mergedFolder = QDir(folder).absoluteFilePath("merged");
    const QString temporaryFolder = QDir(folder).absoluteFilePath("merged_tmp");
    if (!nx::utils::file_system::ensureDir(QDir(mergedFolder))
        || !nx::utils::file_system::ensureDir(QDir(temporaryFolder)))
    {
        NX_WARNING(typeid(StreamCache), "Cannot cache '%1': the cache folder is not usable.",
            displayUrl);
        finish(false);
        return;
    }

    // The merged file is remembered by the identity of the media, not by its address: the address is
    // refreshed every couple of hours, and the cached copy has to survive that.
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(job->group.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString videoPath = QDir(temporaryFolder).absoluteFilePath(name + "-video.m4s");
    const QString audioPath = QDir(temporaryFolder).absoluteFilePath(name + "-audio.m4s");
    const QString outputPath = QDir(mergedFolder).absoluteFilePath(name + ".mp4");

    job->total = remoteSize(job->videoUrl, &job->stop)
        + remoteSize(job->audioUrl, &job->stop);

    const bool videoOk = downloadToFile(job->videoUrl, videoPath, &job->stop, &job->done);
    const bool audioOk =
        !job->stop && downloadToFile(job->audioUrl, audioPath, &job->stop, &job->done);

    if (!videoOk || !audioOk || job->stop)
    {
        QFile::remove(videoPath);
        QFile::remove(audioPath);
        if (!job->stop)
        {
            NX_WARNING(typeid(StreamCache), "Cannot cache '%1': the tracks were not downloaded.",
                displayUrl);
        }
        finish(false);
        return;
    }

    const bool merged = mergeTracks(videoPath, audioPath, outputPath);

    QFile::remove(videoPath);
    QFile::remove(audioPath);

    if (!merged)
    {
        QFile::remove(outputPath);
        NX_WARNING(typeid(StreamCache), "Cannot cache '%1': the tracks could not be merged.",
            displayUrl);
        finish(false);
        return;
    }

    NX_INFO(typeid(StreamCache), "Cached '%1' merged from two tracks (%2 bytes).",
        displayUrl, QFileInfo(outputPath).size());

    job->done = job->total.load();
    job->failed = false;
    job->finished = true;
    emit cache->cachingChanged(job->url);
    emit cache->mergedStreamReady(job->group, outputPath);
}

StreamCache::StreamCache(LocalSettings* settings, QObject* parent):
    QObject(parent),
    d(new Private(this, settings))
{
}

StreamCache::~StreamCache()
{
    std::vector<std::shared_ptr<Private::CachingJob>> jobs;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        for (const auto& job: d->jobs)
        {
            job->stop = true;
            jobs.push_back(job);
        }
        d->jobs.clear();
    }

    for (const auto& job: jobs)
    {
        if (job->thread.joinable())
            job->thread.join();
    }
}

QString StreamCache::defaultFolder()
{
    const QStringList locations =
        QStandardPaths::standardLocations(QStandardPaths::AppLocalDataLocation);
    if (locations.isEmpty())
        return {};

    return QDir(locations.first()).absoluteFilePath("stream_cache");
}

QString StreamCache::folder() const
{
    const QString configured = d->settings ? d->settings->streamCacheFolder() : QString();
    return configured.isEmpty() ? defaultFolder() : configured;
}

qint64 StreamCache::limitBytes() const
{
    const qint64 configured = d->settings ? d->settings->streamCacheLimitBytes() : 0;
    return configured > 0
        ? configured
        : qint64(kDefaultLimitGigabytes) * 1024 * 1024 * 1024;
}

qint64 StreamCache::usedBytes() const
{
    return folderSizeBytes(folder());
}

bool StreamCache::writable() const
{
    const QString path = folder();
    if (path.isEmpty() || !nx::utils::file_system::ensureDir(QDir(path)))
        return false;

    return usedBytes() < limitBytes();
}

std::shared_ptr<StreamCacheReader> StreamCache::open(const QString& url, qint64 sizeBytes)
{
    if (url.isEmpty() || sizeBytes <= 0)
        return nullptr;

    if (!writable())
    {
        NX_DEBUG(typeid(StreamCache), "Not caching '%1': the cache is limited to %2 bytes.",
            localCameraSourceUrlForDisplay(url), limitBytes());
        return nullptr;
    }

    // The URL itself may carry credentials, so only its digest is used on disk.
    std::shared_ptr<StreamCacheStorage> storage = d->storageFor(url, sizeBytes);
    if (!storage)
        return nullptr;

    auto reader = std::shared_ptr<StreamCacheReader>(new StreamCacheReader(url, storage));
    if (!reader->avioContext())
        return nullptr;

    NX_DEBUG(typeid(StreamCache), "Caching '%1'.", localCameraSourceUrlForDisplay(url));

    return reader;
}

qreal StreamCache::cachedShare(const QString& url) const
{
    if (url.isEmpty())
        return 0;

    const QString dataPath = cacheFilePath(folder(), url, ".stream");
    std::shared_ptr<StreamCacheStorage> storage;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        storage = d->storages.value(dataPath).lock();
    }

    if (!storage || storage->blockCount() <= 0)
        return 0;

    int cached = 0;
    for (int index = 0; index < storage->blockCount(); ++index)
    {
        if (storage->hasBlock(index))
            ++cached;
    }

    return qreal(cached) / storage->blockCount();
}

bool StreamCache::isFullyCached(const QString& url) const
{
    return cachedShare(url) >= 1.0;
}

qreal StreamCache::cachingProgress(const QString& url)
{
    reapFinishedCaching();

    std::shared_ptr<Private::CachingJob> job;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        job = d->jobs.value(url);
    }

    if (!job)
        return -1;

    const qint64 total = job->total.load();
    if (total <= 0)
        return 0;

    return qMin(qreal(1), qreal(job->done.load()) / total);
}

bool StreamCache::cacheStream(const QString& url)
{
    if (url.isEmpty() || !writable())
        return false;

    reapFinishedCaching();

    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        if (d->jobs.contains(url))
            return true;
    }

    auto job = std::make_shared<Private::CachingJob>(this, url);
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        d->jobs.insert(url, job);
    }

    job->thread = std::thread([this, job] { d->runCaching(job); });
    return true;
}

bool StreamCache::cacheMergedStream(const QString& url, const QString& videoUrl,
    const QString& audioUrl, const QString& group)
{
    if (url.isEmpty() || videoUrl.isEmpty() || audioUrl.isEmpty() || group.isEmpty())
        return false;

    if (!writable())
        return false;

    reapFinishedCaching();

    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        if (d->jobs.contains(url))
            return true;
    }

    auto job = std::make_shared<Private::CachingJob>(this, url);
    job->merged = true;
    job->videoUrl = videoUrl;
    job->audioUrl = audioUrl;
    job->group = group;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        d->jobs.insert(url, job);
    }

    job->thread = std::thread([this, job] { d->runCaching(job); });
    return true;
}

void StreamCache::stopCaching(const QString& url)
{
    std::shared_ptr<Private::CachingJob> job;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        job = d->jobs.value(url);
    }

    if (!job)
        return;

    // The download notices the flag between blocks, and the interrupt aborts the one in flight.
    job->stop = true;
    emit cachingChanged(url);
}

void StreamCache::reapFinishedCaching()
{
    std::vector<std::shared_ptr<Private::CachingJob>> finished;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        for (auto it = d->jobs.begin(); it != d->jobs.end();)
        {
            if (!it.value()->finished)
            {
                ++it;
                continue;
            }

            finished.push_back(it.value());
            it = d->jobs.erase(it);
        }
    }

    for (const auto& job: finished)
    {
        if (job->thread.joinable())
            job->thread.join();
    }
}

void StreamCache::clear()
{
    std::vector<std::shared_ptr<StreamCacheStorage>> live;
    {
        const std::lock_guard<std::mutex> lock(d->mutex);
        for (auto it = d->storages.cbegin(); it != d->storages.cend(); ++it)
        {
            if (const auto storage = it->lock())
                live.push_back(storage);
        }

        d->storages.clear();
    }

    // The files of a resource that is being played are open and cannot be removed yet.
    for (const auto& storage: live)
        storage->removeOnRelease();

    const QString path = folder();
    if (path.isEmpty())
        return;

    const QDir folder(path);
    const QFileInfoList entries =
        folder.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry: entries)
    {
        const bool removed = entry.isDir()
            ? QDir(entry.absoluteFilePath()).removeRecursively()
            : QFile::remove(entry.absoluteFilePath());
        if (!removed)
        {
            NX_DEBUG(typeid(StreamCache), "Cannot remove the cached file '%1' yet: it is in use.",
                entry.absoluteFilePath());
        }
    }

    emit changed();
}

} // namespace nx::vms::client::desktop
