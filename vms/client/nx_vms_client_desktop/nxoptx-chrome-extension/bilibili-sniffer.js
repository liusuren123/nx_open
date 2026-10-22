// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
//
// Page-world part of the Nx Optx media sniffer, for Bilibili.
//
// Bilibili keeps its media out of the DOM and out of the network in a form the extension could use:
// the player receives a DASH manifest as JSON, plays it through MediaSource (so the <video> element
// only has a blob: URL), and the tracks themselves are separate .m4s files. None of that is a
// stream a player can be pointed at.
//
// This script runs in the page's own world, where the API responses and the page state live. It asks
// the same playurl API for a *progressive* MP4 instead: a single file with both picture and sound,
// which the player can open as it is. The answer is reported to content.js via postMessage, because
// a page-world script has no access to chrome.runtime.

(() =>
{
    if (window.__nxoptxPageSniffer)
        return;
    try
    {
        Object.defineProperty(window, '__nxoptxPageSniffer', {value: true});
    }
    catch (error)
    {
        window.__nxoptxPageSniffer = true;
    }

    const SOURCE = 'nxoptx-page';
    const CONTENT_SOURCE = 'nxoptx-content';
    const INITIAL_POLL_MS = 250;
    const INITIAL_POLL_TRIES = 40; //< Ten seconds: the page state appears shortly after the scripts.

    /** Player quality ids, as named by the API. */
    const QUALITY_NAMES = {
        127: '8K', 126: '杜比视界', 125: 'HDR', 120: '4K', 116: '1080P60', 112: '1080P+',
        80: '1080P', 74: '720P60', 64: '720P', 32: '480P', 16: '360P', 6: '240P',
    };

    const reported = new Set();
    const items = [];
    let lastVideoKey = '';
    let querying = false;

    function post()
    {
        window.postMessage({source: SOURCE, type: 'media', items}, '*');
    }

    /** Adds an item once. Returns it when it was not reported before. */
    function add(videoKey, url, kind, label, title)
    {
        if (!url || typeof url !== 'string' || !/^https?:\/\//i.test(url))
            return null;

        const key = `${kind}|${url}`;
        if (reported.has(key))
            return null;

        reported.add(key);
        const item = {url, kind, label: label || '', title: title || '', videoKey};
        items.push(item);
        return item;
    }

    /** The video currently open, as far as the page has told us about it. */
    function currentVideo()
    {
        const state = window.__INITIAL_STATE__ || {};
        const data = state.videoData || {};
        const fromUrl = String(location.pathname).match(/\/video\/(BV[0-9A-Za-z]+)/);
        const cid = data.cid || (data.pages && data.pages[0] ? data.pages[0].cid : 0);

        // A multi-part video is named after the part that is playing. Plenty of videos have a part
        // whose title repeats the video title, and that should not be duplicated in the name.
        const pages = data.pages || [];
        const page = pages.find((candidate) => candidate.cid === cid);
        const title = (page && pages.length > 1 && page.part && page.part !== data.title)
            ? `${data.title} - ${page.part}`
            : (data.title || '');

        return {
            bvid: data.bvid || (fromUrl ? fromUrl[1] : ''),
            cid,
            title: String(title).replace(/\s+/g, ' ').trim(),
        };
    }

    /**
     * The episode currently open on a bangumi page, or null for any other page.
     *
     * Bangumi pages keep their data in `epInfo` / `epList` rather than in `videoData`, and their
     * player asks a different endpoint, which takes an episode id instead of a bvid.
     */
    function currentEpisode()
    {
        if (!String(location.pathname).startsWith('/bangumi/play/'))
            return null;

        const state = window.__INITIAL_STATE__ || {};
        const episodes = state.epList || [];
        const fromUrl = String(location.pathname).match(/\/bangumi\/play\/ep(\d+)/);

        let info = state.epInfo || null;
        if (fromUrl)
        {
            const playing = episodes.find((candidate) => String(candidate.id) === fromUrl[1]);
            if (playing)
                info = playing;
        }

        if (!info && episodes.length)
            info = episodes[0];

        if (!info || !info.id || !info.cid)
            return null;

        const media = state.mediaInfo || {};
        const seasonTitle = media.title || (state.ssInfo && state.ssInfo.title) || '';
        const episodeTitle = info.titleFormat || info.longTitle || info.title || '';
        const title = seasonTitle && episodeTitle && episodeTitle !== seasonTitle
            ? `${seasonTitle} ${episodeTitle}`
            : (episodeTitle || seasonTitle);

        return {
            epId: String(info.id),
            avid: info.aid || '',
            bvid: info.bvid || '',
            cid: info.cid,
            title: String(title).replace(/\s+/g, ' ').trim(),
        };
    }

    /**
     * Asks the playurl API for one form of the stream.
     *
     * `params.fnval` selects it: 16 is DASH, whose tracks carry 1080P+ / 4K / HDR / 8K, and 1 is a
     * progressive MP4 - one file with both tracks that a player can open as it is, but no better
     * than 1080P. `qn=127` asks for the highest quality; the API answers with what the account is
     * allowed to play. The account cookie is sent along, so a signed-in user gets their tier.
     */
    async function requestPlayurl(endpoint, params)
    {
        const query = new URLSearchParams({
            qn: '127',
            fnver: '0',
            fourk: '1',
            ...params,
        });

        try
        {
            const response = await fetch(`${endpoint}?${query}`, {credentials: 'include'});
            if (!response.ok)
                return null;

            const json = await response.json();
            if (!json || json.code !== 0)
                return null;

            // The bangumi endpoint answers with `result`, the regular one with `data`.
            return json.data || json.result || null;
        }
        catch (error)
        {
            return null;
        }
    }

    /** Name of a player quality id, e.g. 80 -> "1080P". */
    function qualityLabel(quality)
    {
        const id = Number(quality) || 0;
        return QUALITY_NAMES[id] || (id ? `qn${id}` : '');
    }

    function trackUrl(track)
    {
        return track ? (track.baseUrl || track.base_url || '') : '';
    }

    /**
     * Collects what the account can play: the progressive MP4 and, when it is better than the
     * progressive one, the highest DASH video track. The tracks themselves need their signatures,
     * so they are asked for separately: a single request stops at 1080P.
     */
    async function collect(video, endpoint, identity)
    {
        const common = {cid: String(video.cid)};
        if (video.bvid)
            common.bvid = video.bvid;
        if (video.epId)
            common.ep_id = video.epId;

        const [dashData, mp4Data] = await Promise.all([
            requestPlayurl(endpoint, {...common, fnval: '16', qn: '127', platform: 'pc'}),
            requestPlayurl(endpoint,
                {...common, fnval: '1', qn: '80', platform: 'html5', high_quality: '1'}),
        ]);

        const dash = (dashData && dashData.dash) || {};
        const videoTrack = best(dash.video, ['height', 'bandwidth']);
        const audioTrack = best(dash.audio, ['bandwidth']);
        const highVideo = trackUrl(videoTrack);
        const highAudio = trackUrl(audioTrack);

        const found = [];

        const durl = (mp4Data && mp4Data.durl) || (dashData && dashData.durl) || [];
        const progressiveQuality = Number(
            (mp4Data && mp4Data.quality) || (dashData && dashData.quality) || 0);

        for (const entry of durl)
        {
            const item = add(`${identity}|progressive`,
                entry.url || entry.base_url, 'video', qualityLabel(progressiveQuality),
                video.title);
            if (item)
            {
                attachHighTracks(item, identity, highVideo, highAudio);
                found.push(item);
            }
        }

        // 1080P+ / 4K / HDR exist only as a separate video track, so they get an entry of their
        // own. It has no sound until the two tracks are cached and merged into one file.
        const videoQuality = videoTrack ? Number(videoTrack.id) || 0 : 0;
        if (highVideo && videoQuality > progressiveQuality)
        {
            const item = add(`${identity}|dash-video`, highVideo, 'video',
                `${qualityLabel(videoQuality)}（无音轨）`, video.title);
            if (item)
            {
                attachHighTracks(item, identity, highVideo, highAudio);
                found.push(item);
            }
        }

        return found;
    }

    /** Keeps the high quality tracks on the item; only the local cache uses them. */
    function attachHighTracks(item, identity, videoUrl, audioUrl)
    {
        if (!videoUrl || !audioUrl)
            return;

        item.videoUrl = videoUrl;
        item.audioUrl = audioUrl;
        item.group = identity;
    }

    /** Lexicographic comparison of two numeric key tuples. */
    function isBetter(current, other)
    {
        for (let i = 0; i < current.length; ++i)
        {
            if (current[i] !== other[i])
                return current[i] > other[i];
        }
        return false;
    }

    /** Largest representation of a DASH track by the given comparison keys. */
    function best(list, keys)
    {
        let result = null;
        let resultKeys = null;
        for (const representation of list || [])
        {
            if (!representation || !(representation.baseUrl || representation.base_url))
                continue;

            const current = keys.map((key) => representation[key] || 0);
            if (!result || isBetter(current, resultKeys))
            {
                result = representation;
                resultKeys = current;
            }
        }
        return result;
    }

    /**
     * The page's own player result, used when the progressive request is refused. Its tracks are
     * separate, so the video has no sound; the labels say so.
     */
    function fromPlayInfo(video, identity)
    {
        const info = window.__playinfo__;
        const data = info && (info.data || info.result);
        if (!data)
            return [];

        const found = [];
        for (const entry of data.durl || [])
        {
            const item = add(`${identity}|progressive`,
                entry.url || entry.base_url, 'video', '', video.title);
            if (item)
                found.push(item);
        }

        const dash = data.dash || {};
        const videoTrack = best(dash.video, ['height', 'bandwidth']);
        if (videoTrack)
        {
            const label = videoTrack.height ? `${videoTrack.height}P（无音轨）` : '无音轨';
            const item = add(`${identity}|dash-video`,
                videoTrack.baseUrl || videoTrack.base_url, 'video', label, video.title);
            if (item)
                found.push(item);
        }

        const audioTrack = best(dash.audio, ['bandwidth']);
        if (audioTrack)
        {
            const item = add(`${identity}|dash-audio`,
                audioTrack.baseUrl || audioTrack.base_url, 'audio', '音频', video.title);
            if (item)
                found.push(item);
        }

        return found;
    }

    async function scan()
    {
        if (querying)
            return;

        const episode = currentEpisode();
        const video = episode || currentVideo();

        // A bangumi episode is identified by its episode id: it may have no bvid of its own, and
        // the episode is what the player switches when another part is chosen.
        const identity = episode ? `ep${episode.epId}|${episode.cid}` : `${video.bvid}|${video.cid}`;
        const endpoint = episode
            ? 'https://api.bilibili.com/pgc/player/web/playurl'
            : 'https://api.bilibili.com/x/player/playurl';

        if (!video.cid || !(episode || video.bvid))
            return;

        if (identity === lastVideoKey)
            return;

        lastVideoKey = identity;
        querying = true;
        try
        {
            if (!(await collect(video, endpoint, identity)).length)
                fromPlayInfo(video, identity);
            post();
        }
        catch (error)
        {
            // The API may be unreachable or the response may not be what we expect; the page's own
            // result, when there is one, is still worth reporting.
            if (fromPlayInfo(video, identity).length)
                post();
        }
        finally
        {
            querying = false;
        }
    }

    /** Runs scan() now and a few more times, for state that the page updates after a navigation. */
    function scanSoon()
    {
        for (const delay of [0, 300, 1000, 2500, 5000])
            setTimeout(scan, delay);
    }

    let tries = 0;
    const initialTimer = setInterval(() =>
    {
        scan();
        if (++tries >= INITIAL_POLL_TRIES)
            clearInterval(initialTimer);
    }, INITIAL_POLL_MS);

    // Moving between videos does not always reload the document.
    for (const name of ['pushState', 'replaceState'])
    {
        const original = history[name];
        if (typeof original !== 'function')
            continue;

        history[name] = function(...args)
        {
            const result = original.apply(this, args);
            scanSoon();
            return result;
        };
    }
    window.addEventListener('popstate', scanSoon);

    // content.js may start after this script has found something, and may ask for a fresh search
    // when the addresses it reported have expired.
    window.addEventListener('message', (event) =>
    {
        if (event.source !== window)
            return;
        const data = event.data;
        if (!data || data.source !== CONTENT_SOURCE)
            return;

        if (data.type === 'hello')
        {
            if (items.length)
                post();
            return;
        }

        if (data.type === 'rescan')
        {
            // Start over: the address reported before carries a signature that may have expired,
            // so the new answer replaces it rather than joining it.
            reported.clear();
            items.length = 0;
            lastVideoKey = '';
            scan();
        }
    });
})();
