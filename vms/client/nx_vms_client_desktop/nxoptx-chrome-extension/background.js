// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
//
// Background service worker of the Nx Optx media sniffer.
//
// It watches the network requests of every tab, keeps the media resources it recognizes per tab,
// and hands them to the content script (which draws the panel in the page) and to the toolbar popup.
//
// The list is mirrored into chrome.storage.session, because an MV3 service worker is stopped when
// it goes idle and would otherwise lose everything it has collected so far.

const PLAYLIST_EXT = /\.(m3u8|m3u)(\?|#|$)/i;
const DASH_EXT = /\.mpd(\?|#|$)/i;
const VIDEO_EXT = /\.(mp4|m4v|webm|mkv|mov|avi|flv|f4v|3gp|ogv)(\?|#|$)/i;
const AUDIO_EXT = /\.(mp3|m4a|aac|ogg|oga|opus|flac|wav|weba)(\?|#|$)/i;

// Fragments of a stream rather than a stream of their own. A player is given the playlist or the
// container file, so these are only noise.
const SEGMENT_EXT = /\.(ts|m4s|cmfv|cmfa|vtt|key|part)(\?|#|$)/i;

const MAX_ITEMS_PER_TAB = 40;
const NOTIFY_DELAY_MS = 150;
const STORAGE_KEY = 'media';

/** tabId -> Map(url -> item) */
const mediaByTab = new Map();

const notifyTimers = new Map();

/** Network events that arrive before the stored list has been read back. */
const pendingReports = [];
let stateLoaded = false;

const stateReady = (async () =>
{
    const stored = await chrome.storage.session.get(STORAGE_KEY);
    for (const [tabId, items] of Object.entries(stored[STORAGE_KEY] || {}))
        mediaByTab.set(Number(tabId), new Map(items.map((item) => [item.url, item])));

    stateLoaded = true;
    for (const [tabId, url, mime] of pendingReports.splice(0))
        report(tabId, url, mime);
})();

/**
 * Type of the resource, or null when it is not something a player can be pointed at. The URL
 * extension wins over the content type: plenty of servers send media as application/octet-stream.
 */
function classify(url, mime)
{
    const lowerUrl = String(url || '').toLowerCase();
    const lowerMime = String(mime || '').toLowerCase();

    if (PLAYLIST_EXT.test(lowerUrl) || lowerMime.includes('mpegurl'))
        return 'hls';
    if (DASH_EXT.test(lowerUrl) || lowerMime.includes('dash+xml'))
        return 'dash';
    if (VIDEO_EXT.test(lowerUrl) || lowerMime.startsWith('video/'))
        return 'video';
    if (AUDIO_EXT.test(lowerUrl) || lowerMime.startsWith('audio/'))
        return 'audio';
    return null;
}

/** Kinds a page-world sniffer may assign to a resource. */
const PAGE_KINDS = new Set(['hls', 'dash', 'video', 'audio']);

function isUsableUrl(url, allowSegment)
{
    if (!url)
        return false;
    if (url.startsWith('blob:') || url.startsWith('data:') || url.startsWith('mediasource:'))
        return false;
    if (!allowSegment && SEGMENT_EXT.test(url))
        return false;
    return /^https?:\/\//i.test(url);
}

function itemsFor(tabId)
{
    const items = mediaByTab.get(tabId);
    return items ? [...items.values()] : [];
}

/**
 * Remembers a resource. Returns whether the list actually changed.
 *
 * `hint` carries what a page-world sniffer knows about the resource: it names the kind, because a
 * DASH track ends in .m4s and would otherwise be taken for a fragment, and adds the name and the
 * quality label to show.
 */
function remember(tabId, url, mime, hint)
{
    const hintedKind = hint && PAGE_KINDS.has(hint.kind) ? hint.kind : null;
    if (tabId < 0 || !isUsableUrl(url, !!hintedKind))
        return false;

    // Where the resource was found: a request, the page's DOM, or a page-world sniffer. A rescan
    // replaces the entries of the source it re-scanned and leaves the others alone.
    const origin = hint && (hint.origin === 'dom' || hint.origin === 'page') ? hint.origin : 'net';

    const kind = hintedKind || classify(url, mime);
    if (!kind)
        return false;

    let items = mediaByTab.get(tabId);
    if (!items)
    {
        items = new Map();
        mediaByTab.set(tabId, items);
    }

    const existing = items.get(url);
    if (existing)
    {
        // The same URL may have been seen first without a useful content type, or before the page
        // told us what it is.
        let changed = false;
        if (!existing.mime && mime)
        {
            existing.mime = mime;
            existing.kind = kind;
            changed = true;
        }
        if (!existing.title && hint && hint.title)
        {
            existing.title = hint.title;
            changed = true;
        }
        if (!existing.label && hint && hint.label)
        {
            existing.label = hint.label;
            changed = true;
        }
        // The page knows the resource better than a request does, so it takes over the entry.
        if (origin === 'page' && existing.origin !== 'page')
        {
            existing.origin = 'page';
            changed = true;
        }
        return changed;
    }

    if (items.size >= MAX_ITEMS_PER_TAB)
        return false;

    items.set(url, {
        url,
        kind,
        mime: mime || '',
        title: (hint && hint.title) || '',
        label: (hint && hint.label) || '',
        origin,
        foundAt: Date.now(),
    });
    return true;
}

function persist()
{
    const data = {};
    for (const [tabId, items] of mediaByTab)
        data[tabId] = [...items.values()];
    chrome.storage.session.set({[STORAGE_KEY]: data});
}

async function refreshTab(tabId)
{
    const items = itemsFor(tabId);

    try
    {
        await chrome.action.setBadgeBackgroundColor({color: '#3A911E'});
        await chrome.action.setBadgeText({
            tabId, text: items.length ? String(items.length) : ''});
    }
    catch (error)
    {
        // The tab is gone.
    }

    try
    {
        await chrome.tabs.sendMessage(tabId, {type: 'mediaUpdated', items});
    }
    catch (error)
    {
        // No content script in this tab: a restricted page, or the document is still loading.
    }

    // The toolbar popup shows the same list, and may be open at this moment.
    try
    {
        chrome.runtime.sendMessage({type: 'mediaUpdated', tabId, items},
            () => void chrome.runtime.lastError);
    }
    catch (error)
    {
        // Nobody is listening.
    }
}

/** Network events come in bursts, so updates are coalesced per tab. */
function scheduleRefresh(tabId)
{
    if (notifyTimers.has(tabId))
        return;

    notifyTimers.set(tabId, setTimeout(() =>
    {
        notifyTimers.delete(tabId);
        refreshTab(tabId);
        persist();
    }, NOTIFY_DELAY_MS));
}

function report(tabId, url, mime)
{
    if (remember(tabId, url, mime))
        scheduleRefresh(tabId);
}

function clearTab(tabId)
{
    if (mediaByTab.delete(tabId))
    {
        refreshTab(tabId);
        persist();
    }
}

function headerValue(headers, name)
{
    if (!headers)
        return '';
    const header = headers.find(
        (candidate) => candidate.name.toLowerCase() === name.toLowerCase());
    return header ? header.value || '' : '';
}

const REQUEST_FILTER = {
    urls: ['<all_urls>'],
    types: ['media', 'xmlhttprequest', 'other', 'object'],
};

chrome.webRequest.onHeadersReceived.addListener(
    (details) =>
    {
        const mime = headerValue(details.responseHeaders, 'content-type');
        if (!stateLoaded)
            pendingReports.push([details.tabId, details.url, mime]);
        else
            report(details.tabId, details.url, mime);
    },
    REQUEST_FILTER,
    ['responseHeaders']);

// A few servers answer a media request with an error, or never answer at all, but the URL is still
// worth offering to the user.
chrome.webRequest.onBeforeRequest.addListener(
    (details) =>
    {
        if (!stateLoaded)
            pendingReports.push([details.tabId, details.url, '']);
        else
            report(details.tabId, details.url, '');
    },
    REQUEST_FILTER);

chrome.tabs.onUpdated.addListener((tabId, changeInfo) =>
{
    // Only a navigation of the tab itself starts a new list. A subframe that finishes loading
    // (an ad, a player's helper page) must not drop what the page has already found.
    if (changeInfo.url)
        clearTab(tabId);
});

chrome.tabs.onRemoved.addListener((tabId) =>
{
    mediaByTab.delete(tabId);
    notifyTimers.delete(tabId);
    persist();
});

/**
 * Messages from the content script (no tabId: it comes from the sender) and from the popup (which
 * passes the id of the tab it is showing).
 */
chrome.runtime.onMessage.addListener((message, sender, sendResponse) =>
{
    const tabId = typeof message?.tabId === 'number'
        ? message.tabId
        : (sender.tab ? sender.tab.id : -1);

    const handle = () =>
    {
        if (message?.type === 'getMedia')
            return {items: itemsFor(tabId)};

        if (message?.type === 'register')
        {
            let changed = false;

            // A rescan replaces what the same source reported before: a stream address carries a
            // signature that expires, so the new answer is a fresh address, not another resource.
            const origin = message.origin === 'page' || message.origin === 'dom'
                ? message.origin
                : 'net';
            const items = mediaByTab.get(tabId);
            if (message.replace && origin !== 'net' && items)
            {
                for (const [url, item] of [...items])
                {
                    if (item.origin === origin || (origin === 'page' && item.page))
                    {
                        items.delete(url);
                        changed = true;
                    }
                }
            }

            for (const item of message.items || [])
                changed = remember(tabId, item.url, item.mime, {...item, origin}) || changed;
            if (changed)
                scheduleRefresh(tabId);
            return {items: itemsFor(tabId)};
        }

        if (message?.type === 'remove')
        {
            if (mediaByTab.get(tabId)?.delete(message.url))
                scheduleRefresh(tabId);
            return {items: itemsFor(tabId)};
        }

        if (message?.type === 'clear')
        {
            clearTab(tabId);
            return {items: itemsFor(tabId)};
        }

        return {items: itemsFor(tabId)};
    };

    (async () =>
    {
        await stateReady;
        const result = handle();
        try
        {
            sendResponse({ok: true, ...result});
        }
        catch (error)
        {
            // The popup was closed before the reply.
        }
    })();

    return true; //< The reply is asynchronous.
});
