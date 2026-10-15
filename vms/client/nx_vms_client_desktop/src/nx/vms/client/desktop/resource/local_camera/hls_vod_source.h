// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <memory>

#include <QtCore/QString>

namespace nx::vms::client::desktop {

/**
 * Plays a finite HLS playlist (a VOD, i.e. one ended with #EXT-X-ENDLIST) through a local source that
 * downloads its media segments in parallel.
 *
 * A manifest demuxer fetches the media itself, one HTTP connection at a time, so the playback rate is
 * limited by what a single connection can deliver. Some servers limit every connection on its own,
 * and then a VOD that needs, say, 0.8 MB/s to play in real time cannot be played at all, no matter
 * how fast the network as a whole is. Downloading several segments concurrently works around that:
 * the aggregate rate is what matters, and it is typically several times the single-connection one.
 *
 * The segments are downloaded into the stream cache folder, so a replay and a seek need no network,
 * and the local server below hands them to FFmpeg as if they were being streamed: the demuxer still
 * fetches the media itself, but it now talks to the local server, which serves the already downloaded
 * segments and waits for the ones still on their way.
 *
 * Live playlists and anything that is not HLS are not handled: they are read directly, as before.
 */
class HlsVodSource
{
public:
    HlsVodSource(const HlsVodSource&) = delete;
    HlsVodSource& operator=(const HlsVodSource&) = delete;
    ~HlsVodSource();

    /**
     * Creates the source, downloading the playlist and starting the segment download and the local
     * server. Returns null when the playlist must be read directly instead: it is not an M3U playlist,
     * it is a live playlist or a master playlist, it uses a feature that is not supported, or the
     * cache folder is unusable.
     *
     * @param playlistUrl URL of the playlist that is being opened.
     * @param cacheFolder Folder the segments are stored in; usually the stream cache one.
     * @param cacheLimitBytes Upper bound of the cache size, in bytes.
     * @param connections Number of segments downloaded at the same time.
     * @param errorOut Reason the source was not created, for the log.
     */
    static std::unique_ptr<HlsVodSource> create(
        const QString& playlistUrl,
        const QString& cacheFolder,
        qint64 cacheLimitBytes,
        int connections,
        QString* errorOut);

    /**
     * Same as create(), but a source that is already serving the playlist is reused.
     *
     * A playback is closed and opened again whenever its position is reset, and stopping the source
     * in between would throw away every segment downloaded so far (and restart the probe). A running
     * source therefore outlives the handles to it, and stays alive until the application exits.
     */
    static std::shared_ptr<HlsVodSource> acquire(
        const QString& playlistUrl,
        const QString& cacheFolder,
        qint64 cacheLimitBytes,
        int connections,
        QString* errorOut);

    /** Local playlist URL to be handed to FFmpeg. Empty until the source has started. */
    QString url() const;

    /** Folder the segments are cached in. */
    QString cacheFolder() const;

    /** Stops the downloads and the local server. Safe to call more than once. */
    void stop();

private:
    struct Private;
    explicit HlsVodSource(std::unique_ptr<Private> d);

    std::unique_ptr<Private> d;
};

} // namespace nx::vms::client::desktop
