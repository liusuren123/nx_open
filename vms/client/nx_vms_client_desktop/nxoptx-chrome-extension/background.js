// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
//
// Background service worker of the Nx Optx media sniffer.
//
// It watches the network requests of every tab and keeps one list of media resources for the
// whole browser. A resource stays in the list after its tab navigates away, and the name the user
// gave it stays with it. The list is mirrored into chrome.storage.local, so it also survives a
// restart of the browser.

const PLAYLIST_EXT = /\.(m3u8|m3u)(\?|#|$)/i;
const DASH_EXT = /\.mpd(\?|#|$)/i;
const VIDEO_EXT = /\.(mp4|m4v|webm|mkv|mov|avi|flv|f4v|3gp|ogv)(\?|#|$)/i;
const AUDIO_EXT = /\.(mp3|m4a|aac|ogg|oga|opus|flac|wav|weba)(\?|#|$)/i;

// Fragments of a stream rather than a stream of their own. A player is given the playlist or the
// container file, so these are only noise.
const SEGMENT_EXT = /\.(ts|m4s|cmfv|cmfa|vtt|key|part)(\?|#|$)/i;

const MAX_ITEMS = 300;
const MAX_DISMISSED = 1000;
const NOTIFY_DELAY_MS = 150;
const STORAGE_KEY = 'media';
const SCHEME = 'nxoptx';

/** Commands for the player are started one at a time: the browser drops the concurrent ones. */
const LAUNCH_DELAY_MS = 1500;
const launchQueue = [];
let launching = false;

/** url -> item. One list for every tab, kept in discovery order. */
const media = new Map();

/**
 * url -> page URL of the resources the user removed on purpose. Such a URL is not sniffed back
 * while the page that reported it is still open, so clearing the list of a playing page does not
 * undo itself a moment later. The set lives as long as the service worker does.
 */
const dismissed = new Map();

/** tabId -> {url, title} of the document that is currently loaded in the tab. */
const tabPages = new Map();

let notifyTimer = 0;

/** Network events that arrive before the stored list has been read back. */
const pendingReports = [];
let stateLoaded = false;

const stateReady = (async () =>
{
    const stored = await chrome.storage.local.get(STORAGE_KEY);
    for (const item of stored[STORAGE_KEY] || [])
    {
        if (item && item.url && !media.has(item.url))
            media.set(item.url, item);
    }

    try
    {
        for (const tab of await chrome.tabs.query({}))
            rememberPage(tab);
    }
    catch (error)
    {
        // No permission for some tab, or the browser is shutting down.
    }

    stateLoaded = true;
    for (const [tabId, url, mime] of pendingReports.splice(0))
        report(tabId, url, mime);

    // Show the stored count on the badge right away.
    broadcast();
})();

function rememberPage(tab)
{
    if (tab && tab.id >= 0)
        tabPages.set(tab.id, {url: tab.url || '', title: tab.title || ''});
}

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

function allItems()
{
    return [...media.values()];
}

/**
 * Name to show for a resource that the user has not renamed. The page title beats the address,
 * which often ends in something like "proxy" and says nothing about the video.
 */
function autoName(item)
{
    let base = (item.title || item.pageTitle || '').replace(/\s+/g, ' ').trim();
    if (!base)
    {
        try
        {
            const parsed = new URL(item.url);
            base = decodeURIComponent(parsed.pathname.split('/').filter(Boolean).pop() || '')
                || parsed.hostname;
        }
        catch (error)
        {
            base = item.url;
        }
    }
    if (item.label)
        base = `${base} ${item.label}`;
    return base.length > 60 ? `${base.slice(0, 59)}…` : base;
}

/**
 * Remembers a resource. Returns whether the list actually changed.
 *
 * `hint` carries what a page-world sniffer knows about the resource: it names the kind, because a
 * DASH track ends in .m4s and would otherwise be taken for a fragment, and adds the name and the
 * quality label to show. `page` is the document the resource was found in.
 */
function remember(url, mime, hint, page)
{
    const hintedKind = hint && PAGE_KINDS.has(hint.kind) ? hint.kind : null;
    if (!isUsableUrl(url, !!hintedKind))
        return false;

    const kind = hintedKind || classify(url, mime);
    if (!kind)
        return false;

    // Removed by the user: automatic sniffing must not bring it back.
    if (dismissed.has(url))
        return false;

    // Where the resource was found: a request, the page's DOM, or a page-world sniffer. A rescan
    // replaces the entries of the source it re-scanned and leaves the others alone.
    const origin = hint && (hint.origin === 'dom' || hint.origin === 'page') ? hint.origin : 'net';

    const existing = media.get(url);
    if (existing)
    {
        // The same URL may have been seen first without a useful content type, or before the page
        // told us what it is. A name the user typed is never overwritten.
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
        if (!existing.videoKey && hint && hint.videoKey)
        {
            existing.videoKey = hint.videoKey;
            changed = true;
        }
        if (!existing.videoUrl && hint && hint.videoUrl)
        {
            existing.videoUrl = hint.videoUrl;
            changed = true;
        }
        if (!existing.audioUrl && hint && hint.audioUrl)
        {
            existing.audioUrl = hint.audioUrl;
            changed = true;
        }
        if (!existing.group && hint && hint.group)
        {
            existing.group = hint.group;
            changed = true;
        }
        if (!existing.pageUrl && page && page.url)
        {
            existing.pageUrl = page.url;
            changed = true;
        }
        if (!existing.pageTitle && page && page.title)
        {
            existing.pageTitle = page.title;
            changed = true;
        }
        if (changed && !existing.edited)
            existing.name = autoName(existing);
        return changed;
    }

    if (media.size >= MAX_ITEMS)
    {
        const oldest = [...media.values()].reduce(
            (found, item) => (!found || item.foundAt < found.foundAt ? item : found), null);
        if (oldest)
            media.delete(oldest.url);
    }

    const item = {
        url,
        kind,
        mime: mime || '',
        title: (hint && hint.title) || '',
        label: (hint && hint.label) || '',
        videoKey: (hint && hint.videoKey) || '',
        videoUrl: (hint && hint.videoUrl) || '',
        audioUrl: (hint && hint.audioUrl) || '',
        group: (hint && hint.group) || '',
        origin,
        pageUrl: (page && page.url) || '',
        pageTitle: (page && page.title) || '',
        foundAt: Date.now(),
        edited: false,
        name: '',
    };
    item.name = autoName(item);
    media.set(url, item);
    return true;
}

/**
 * Drops what one source of one document reported before, keeping the urls it just reported, and
 * returns the entries that were dropped.
 */
function replaceSource(pageUrl, origin, keep)
{
    const removed = [];
    for (const [url, item] of [...media])
    {
        if (item.pageUrl === pageUrl && item.origin === origin && !keep.has(url))
        {
            media.delete(url);
            removed.push(item);
        }
    }
    return removed;
}

/** Drops the resources found in one document, or everything when no document is given. */
function clearItems(pageUrl)
{
    const removed = [];
    for (const [url, item] of [...media])
    {
        if (!pageUrl || item.pageUrl === pageUrl)
        {
            media.delete(url);
            removed.push(item);
        }
    }
    return removed;
}

/** Keeps the given resources out of the list until the page they belong to is rescanned. */
function dismiss(items)
{
    let changed = false;
    for (const item of items)
    {
        if (dismissed.has(item.url))
            continue;

        dismissed.set(item.url, item.pageUrl || '');
        changed = true;
    }

    while (dismissed.size > MAX_DISMISSED)
        dismissed.delete(dismissed.keys().next().value);

    return changed;
}

/**
 * Forgets that resources of the given document were removed, i.e. lets a rescan find them again.
 * When no document is given, nothing stays dismissed anymore.
 */
function undismiss(pageUrl)
{
    let changed = false;
    for (const url of [...dismissed.keys()])
    {
        if (pageUrl && dismissed.get(url) !== pageUrl)
            continue;

        dismissed.delete(url);
        changed = true;
    }
    return changed;
}

function persist()
{
    chrome.storage.local.set({[STORAGE_KEY]: allItems()},
        () => void chrome.runtime.lastError);
}

async function broadcast()
{
    const items = allItems();

    try
    {
        await chrome.action.setBadgeBackgroundColor({color: '#3A911E'});
        await chrome.action.setBadgeText({text: items.length ? String(items.length) : ''});
    }
    catch (error)
    {
        // The browser is shutting down.
    }

    // The toolbar popup shows the whole list and may be open at this moment.
    try
    {
        chrome.runtime.sendMessage({type: 'mediaUpdated', items},
            () => void chrome.runtime.lastError);
    }
    catch (error)
    {
        // Nobody is listening.
    }

    // The panel of each page filters the list down to the resources of that page.
    try
    {
        for (const tab of await chrome.tabs.query({}))
        {
            if (tab.id >= 0)
            {
                chrome.tabs.sendMessage(tab.id, {type: 'mediaUpdated', items},
                    () => void chrome.runtime.lastError);
            }
        }
    }
    catch (error)
    {
        // A restricted page, or a document without the content script.
    }
}

/** Network events come in bursts, so updates are coalesced. */
function scheduleRefresh()
{
    if (notifyTimer)
        return;

    notifyTimer = setTimeout(() =>
    {
        notifyTimer = 0;
        persist();
        broadcast();
    }, NOTIFY_DELAY_MS);
}

function report(tabId, url, mime)
{
    if (remember(url, mime, null, tabPages.get(tabId)))
        scheduleRefresh();
}

/**
 * Tells the player that a signed address it may hold has been replaced by a fresh one.
 *
 * A playlist entry keeps the URL it was added with, and a stream address from a provider like
 * Bilibili stops working a couple of hours after it was issued. Scanning the page again produces a
 * new address for the same video; handing it over keeps the entry playable.
 */
function refreshClientLinks(tabId, replaced, fresh)
{
    if (tabId < 0 || !replaced.length)
        return;

    const freshByKey = new Map();
    for (const item of fresh)
    {
        if (item.videoKey)
            freshByKey.set(item.videoKey, item.url);
    }

    const encode = encodeURIComponent;
    for (const item of replaced)
    {
        const url = item.videoKey ? freshByKey.get(item.videoKey) : undefined;
        if (!url || url === item.url)
            continue;

        launchCommand(tabId,
            `${SCHEME}://replaceStream?url=${encode(item.url)}&newUrl=${encode(url)}`);
    }
}

function launchCommand(tabId, uri)
{
    launchQueue.push({tabId, uri});
    pumpLaunches();
}

function pumpLaunches()
{
    if (launching || !launchQueue.length)
        return;

    launching = true;
    const {tabId, uri} = launchQueue.shift();
    chrome.tabs.update(tabId, {url: uri}, () =>
    {
        void chrome.runtime.lastError;
        setTimeout(() =>
        {
            launching = false;
            pumpLaunches();
        }, LAUNCH_DELAY_MS);
    });
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

// A navigation or a new title only updates the page a resource is attributed to. The resources
// themselves stay: the list is cumulative.
chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) =>
{
    if (!changeInfo.url && !changeInfo.title)
        return;

    rememberPage(tab);

    // The title usually arrives after the first media request. Resources of this document that
    // were named after their address get the better name now, unless the user renamed them.
    if (changeInfo.title && tab.url)
    {
        let changed = false;
        for (const item of media.values())
        {
            if (!item.edited && !item.title && item.pageUrl === tab.url
                && item.pageTitle !== tab.title)
            {
                item.pageTitle = tab.title || '';
                item.name = autoName(item);
                changed = true;
            }
        }
        if (changed)
            scheduleRefresh();
    }
});

chrome.tabs.onRemoved.addListener((tabId) =>
{
    tabPages.delete(tabId);
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

    const known = tabPages.get(tabId);
    const page = {
        url: message?.pageUrl || (known ? known.url : ''),
        title: message?.pageTitle || (known ? known.title : ''),
    };

    const handle = () =>
    {
        if (message?.type === 'getMedia')
            return {items: allItems()};

        if (message?.type === 'register')
        {
            // A rescan replaces what the same source reported before: a stream address carries a
            // signature that expires, so the new answer is a fresh address, not another resource.
            const origin = message.origin === 'page' || message.origin === 'dom'
                ? message.origin
                : 'net';
            const list = message.items || [];
            const replaced = message.replace && origin !== 'net' && page.url
                ? replaceSource(page.url, origin, new Set(list.map((item) => item.url)))
                : [];
            let changed = replaced.length > 0;

            // A page-world sniffer names the video an address belongs to. The same video may be
            // reported with a fresh address from another page or another visit, and only the
            // newest address works, so the older ones are dropped whatever page they came from.
            const seen = new Set();
            for (const item of list)
            {
                if (!item.videoKey)
                    continue;

                for (const [url, existing] of [...media])
                {
                    if (existing.videoKey !== item.videoKey || url === item.url || seen.has(url))
                        continue;

                    seen.add(url);
                    media.delete(url);
                    replaced.push(existing);
                    changed = true;
                }
            }

            for (const item of list)
                changed = remember(item.url, item.mime, {...item, origin}, page) || changed;

            refreshClientLinks(tabId, replaced, list);
            if (changed)
                scheduleRefresh();
            return {items: allItems()};
        }

        if (message?.type === 'rename')
        {
            const item = media.get(message.url);
            if (item)
            {
                const name = String(message.name || '').replace(/\s+/g, ' ').trim();
                item.name = name || autoName(item);
                item.edited = !!name;
                scheduleRefresh();
            }
            return {items: allItems()};
        }

        if (message?.type === 'remove')
        {
            const item = media.get(message.url);
            if (item && media.delete(item.url))
            {
                dismiss([item]);
                scheduleRefresh();
            }
            return {items: allItems()};
        }

        if (message?.type === 'clear')
        {
            // The page panel clears the resources of its own document; the toolbar popup clears
            // the whole list, whatever document is open in the browser.
            const pageUrl = message.all ? '' : String(message.pageUrl || page.url || '');
            const removed = clearItems(pageUrl);

            // Removed on purpose: sniffing the same page again must not undo this.
            if (removed.length && dismiss(removed))
                scheduleRefresh();

            return {items: allItems()};
        }

        if (message?.type === 'undismiss')
        {
            // An explicit rescan is a request to find everything again, including what was
            // removed from the list.
            if (undismiss(String(message.pageUrl || page.url || '')))
                scheduleRefresh();

            return {items: allItems()};
        }

        return {items: allItems()};
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
