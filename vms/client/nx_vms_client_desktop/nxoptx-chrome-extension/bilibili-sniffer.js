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

        // A multi-part video is named after the part that is playing.
        const pages = data.pages || [];
        const page = pages.find((candidate) => candidate.cid === cid);
        const title = (page && pages.length > 1 && page.part)
            ? `${data.title} - ${page.part}`
            : (data.title || '');

        return {
            bvid: data.bvid || (fromUrl ? fromUrl[1] : ''),
            cid,
            title: String(title).replace(/\s+/g, ' ').trim(),
        };
    }

    /**
     * Asks for a progressive stream: one MP4 that already contains both tracks. The quality is
     * whatever the account is allowed, which is up to 1080p when signed in and usually 720p
     * otherwise. Asking for more than is allowed is safe: the API answers with the best it has.
     */
    async function progressive(video)
    {
        // fnval 17 asks for both forms at once: durl is a progressive MP4 that a player can open as
        // it is (capped at 1080p), dash carries the higher qualities as separate video and audio
        // tracks. The item points at the first and keeps the other two for local caching.
        const query = `bvid=${encodeURIComponent(video.bvid)}`
            + `&cid=${encodeURIComponent(video.cid)}`
            + '&fnval=17&fnver=0&fourk=1&qn=127&platform=html5&high_quality=1';

        const response = await fetch(`https://api.bilibili.com/x/player/playurl?${query}`,
            {credentials: 'include'});
        if (!response.ok)
            return [];

        const json = await response.json();
        if (!json || json.code !== 0)
            return [];

        const data = json.data || {};
        const dash = data.dash || {};
        const videoTrack = best(dash.video, ['height', 'bandwidth']);
        const audioTrack = best(dash.audio, ['bandwidth']);
        const highVideo = videoTrack ? (videoTrack.baseUrl || videoTrack.base_url) : '';
        const highAudio = audioTrack ? (audioTrack.baseUrl || audioTrack.base_url) : '';

        const quality = QUALITY_NAMES[data.quality]
            || (data.quality ? `qn${data.quality}` : '');

        const found = [];
        for (const entry of data.durl || [])
        {
            const item = add(`${video.bvid}|${video.cid}|progressive`,
                entry.url || entry.base_url, 'video', quality, video.title);
            if (item)
            {
                attachHighTracks(item, video, highVideo, highAudio);
                found.push(item);
            }
        }

        // Some videos have no progressive form at all. The highest DASH track is then the only
        // address there is; its sound arrives once the item has been cached locally.
        if (!found.length && highVideo)
        {
            const label = videoTrack.height ? `${videoTrack.height}P（无音轨）` : '无音轨';
            const item = add(`${video.bvid}|${video.cid}|dash-video`, highVideo, 'video', label,
                video.title);
            if (item)
            {
                attachHighTracks(item, video, highVideo, highAudio);
                found.push(item);
            }
        }

        return found;
    }

    /** Keeps the high quality tracks on the item; only the local cache uses them. */
    function attachHighTracks(item, video, videoUrl, audioUrl)
    {
        if (!videoUrl || !audioUrl)
            return;

        item.videoUrl = videoUrl;
        item.audioUrl = audioUrl;
        item.group = `${video.bvid}|${video.cid}`;
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
    function fromPlayInfo(video)
    {
        const info = window.__playinfo__;
        if (!info || !info.data)
            return [];

        const data = info.data;
        const found = [];
        for (const entry of data.durl || [])
        {
            const item = add(`${video.bvid}|${video.cid}|progressive`,
                entry.url || entry.base_url, 'video', '', video.title);
            if (item)
                found.push(item);
        }

        const dash = data.dash || {};
        const videoTrack = best(dash.video, ['height', 'bandwidth']);
        if (videoTrack)
        {
            const label = videoTrack.height ? `${videoTrack.height}P（无音轨）` : '无音轨';
            const item = add(`${video.bvid}|${video.cid}|dash-video`,
                videoTrack.baseUrl || videoTrack.base_url, 'video', label, video.title);
            if (item)
                found.push(item);
        }

        const audioTrack = best(dash.audio, ['bandwidth']);
        if (audioTrack)
        {
            const item = add(`${video.bvid}|${video.cid}|dash-audio`,
                audioTrack.baseUrl || audioTrack.base_url, 'audio', '音频', video.title);
            if (item)
                found.push(item);
        }

        return found;
    }

    async function scan()
    {
        const video = currentVideo();
        if (!video.bvid || !video.cid || querying)
            return;

        const key = `${video.bvid}|${video.cid}`;
        if (key === lastVideoKey)
            return;

        lastVideoKey = key;
        querying = true;
        try
        {
            if (!(await progressive(video)).length)
                fromPlayInfo(video);
            post();
        }
        catch (error)
        {
            // The API may be unreachable or the response may not be what we expect; the page's own
            // result, when there is one, is still worth reporting.
            if (fromPlayInfo(video).length)
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
