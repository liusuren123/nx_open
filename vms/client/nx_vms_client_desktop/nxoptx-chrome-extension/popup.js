// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
//
// Toolbar popup: the same list of found media as the in-page panel, plus the settings.

const SCHEME = 'nxoptx';
const DEFAULT_SETTINGS = {showPanel: false, playlist: '', autoExpand: false};
const REFRESH_INTERVAL_MS = 1000;

const KIND_INFO = {
    hls: {label: 'HLS', color: '#e2a03f'},
    dash: {label: 'DASH', color: '#8d7bd6'},
    video: {label: '视频', color: '#3a911e'},
    audio: {label: '音频', color: '#3d8fd4'},
};

const elements = {
    subtitle: document.getElementById('subtitle'),
    clear: document.getElementById('clear'),
    list: document.getElementById('list'),
    empty: document.getElementById('empty'),
    addAll: document.getElementById('addAll'),
    rescan: document.getElementById('rescan'),
    showPanel: document.getElementById('showPanel'),
    autoExpand: document.getElementById('autoExpand'),
    playlist: document.getElementById('playlist'),
};

let settings = {...DEFAULT_SETTINGS};
let items = [];
let tabId = -1;
let rescanning = false;
let lastKey = null;
let refreshSeq = 0;

function request(message)
{
    return new Promise((resolve) =>
    {
        chrome.runtime.sendMessage(message, (reply) =>
        {
            void chrome.runtime.lastError;
            resolve(reply || null);
        });
    });
}

function suggestedName(item, index)
{
    let base = (item.title || '').trim();
    if (!base)
    {
        try
        {
            const parsed = new URL(item.url);
            const name = decodeURIComponent(parsed.pathname.split('/').filter(Boolean).pop() || '');
            base = name || parsed.hostname;
        }
        catch (error)
        {
            base = item.url;
        }
    }
    if (item.label)
        return `${base} ${item.label}`;
    return items.length > 1 ? `${base} #${index + 1}` : base;
}

function buildUri(item, name)
{
    const playlist = settings.playlist.trim();
    const encode = encodeURIComponent;
    return `${SCHEME}://addStream?url=${encode(item.url)}&name=${encode(name)}`
        + (playlist ? `&playlist=${encode(playlist)}` : '');
}

/**
 * Chrome refuses to start an external protocol navigation for a URL longer than about 2 KB, and a
 * batch of stream addresses is easily longer. The payload is therefore deflated and base64-encoded;
 * Qt's qUncompress() expects the uncompressed size as a big-endian 32-bit prefix.
 */
async function packPayload(text)
{
    const bytes = new TextEncoder().encode(text);
    const compressed = new Uint8Array(await new Response(
        new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate')))
        .arrayBuffer());

    const packed = new Uint8Array(4 + compressed.length);
    new DataView(packed.buffer).setUint32(0, bytes.length, false);
    packed.set(compressed, 4);

    let binary = '';
    for (let i = 0; i < packed.length; i += 0x8000)
        binary += String.fromCharCode(...packed.subarray(i, i + 0x8000));

    return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_');
}

/**
 * Builds one command that adds several streams at once. Sending one command per stream would make
 * the browser cancel the earlier launches.
 */
async function buildBatchUri(entries)
{
    const payload = JSON.stringify(entries.map(({url, name}) => ({url, name})));
    const packed = await packPayload(payload);
    const playlist = settings.playlist.trim();
    const encode = encodeURIComponent;
    return `${SCHEME}://addStreams?data=${encode(packed)}`
        + (playlist ? `&playlist=${encode(playlist)}` : '');
}

/**
 * Chrome ignores an external protocol URL that is too long, and a dozen stream addresses are
 * enough to get there even after compression. A batch that does not fit is therefore sent as
 * several commands, spaced out so that the browser handles them one after another.
 */
const MAX_COMMAND_LENGTH = 1500;
const CHUNK_DELAY_MS = 1500;

function delay(ms)
{
    return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Greedy split: every command but the last one gets as many entries as it can hold. */
async function splitIntoCommands(entries)
{
    const commands = [];
    let command = [];
    for (const entry of entries)
    {
        const candidate = [...command, entry];
        if (command.length && (await buildBatchUri(candidate)).length > MAX_COMMAND_LENGTH)
        {
            commands.push(command);
            command = [entry];
        }
        else
        {
            command = candidate;
        }
    }
    if (command.length)
        commands.push(command);
    return commands;
}

/**
 * Hands the command to the browser, which passes it to the player through the registered
 * nxoptx:// handler.
 *
 * The navigation is started on the page's tab rather than in this popup. Chrome attaches its
 * "open this app?" prompt to the document that starts the navigation, and inside the small
 * popup the prompt is clipped.
 */
async function openUri(uri)
{
    if (tabId >= 0)
    {
        try
        {
            await chrome.tabs.update(tabId, {url: uri});
            return 'tab';
        }
        catch (error)
        {
            // The tab is gone; fall through to a link from this document.
        }
    }

    const anchor = document.createElement('a');
    anchor.href = uri;
    anchor.style.display = 'none';
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    return 'anchor';
}

function setStatus(container, text, isError)
{
    const status = container.querySelector('.status');
    status.textContent = text;
    status.classList.toggle('error', !!isError);
    clearTimeout(status.__timer);
    if (text)
        status.__timer = setTimeout(() => { status.textContent = ''; }, 4000);
}

function renderItem(item, index)
{
    const info = KIND_INFO[item.kind] || KIND_INFO.video;

    const row = document.createElement('li');
    row.className = 'item';

    const top = document.createElement('div');
    top.className = 'item-top';

    const kind = document.createElement('span');
    kind.className = 'kind';
    kind.textContent = info.label;
    kind.style.color = info.color;

    const name = document.createElement('input');
    name.className = 'name';
    name.type = 'text';
    name.value = suggestedName(item, index);
    name.title = '在播放器里显示的名称';

    top.append(kind, name);

    const url = document.createElement('div');
    url.className = 'url';
    url.textContent = item.url;
    url.title = item.url;

    const actions = document.createElement('div');
    actions.className = 'actions';

    const add = document.createElement('button');
    add.type = 'button';
    add.className = 'primary';
    add.textContent = '添加到播放器';
    add.addEventListener('click', () =>
    {
        openUri(buildUri(item, name.value.trim() || suggestedName(item, index)));
        setStatus(row, '已发送到播放器', false);
    });

    const copy = document.createElement('button');
    copy.type = 'button';
    copy.className = 'ghost';
    copy.textContent = '复制';
    copy.addEventListener('click', async () =>
    {
        try
        {
            await navigator.clipboard.writeText(item.url);
            setStatus(row, '已复制', false);
        }
        catch (error)
        {
            setStatus(row, '复制失败', true);
        }
    });

    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'icon';
    remove.title = '从列表中移除';
    remove.textContent = '×';
    remove.addEventListener('click', async () =>
    {
        const reply = await request({type: 'remove', tabId, url: item.url});
        if (reply)
            apply(reply.items);
    });

    const status = document.createElement('span');
    status.className = 'status';

    actions.append(add, copy, remove, status);
    row.append(top, url, actions);
    return row;
}

function render()
{
    elements.list.textContent = '';
    items.forEach((item, index) => elements.list.append(renderItem(item, index)));

    const hasItems = items.length > 0;
    elements.list.style.display = hasItems ? 'block' : 'none';
    elements.empty.style.display = hasItems ? 'none' : 'block';
    elements.addAll.disabled = !hasItems;
    elements.clear.disabled = !hasItems;
    elements.rescan.disabled = tabId < 0 || rescanning;

    elements.subtitle.textContent = hasItems
        ? `发现 ${items.length} 个媒体资源`
        : '还没有发现媒体资源';
}

function apply(next)
{
    const list = Array.isArray(next) ? next : [];

    // Refreshes come from a timer while the popup is open; rebuilding the list every second would
    // throw away the names the user is typing.
    const key = list.map((item) => item.url).join('\n');
    if (key === lastKey)
        return;

    lastKey = key;
    items = list;
    render();
}

/** The popup is open for a moment only, and polls so it never shows a stale or empty list. */
async function refresh()
{
    if (tabId < 0)
        return;

    const seq = ++refreshSeq;
    const reply = await request({type: 'getMedia', tabId});
    if (seq !== refreshSeq)
        return; //< A newer refresh is already on its way.
    if (reply)
        apply(reply.items);
}

elements.addAll.addEventListener('click', async () =>
{
    if (!items.length)
        return;

    const rows = [...elements.list.children];
    const entries = items.map((item, index) =>
    {
        const input = rows[index]?.querySelector('.name');
        return {url: item.url, name: input?.value.trim() || suggestedName(item, index)};
    });

    try
    {
        const commands = await splitIntoCommands(entries);
        let sent = 0;
        for (const [index, command] of commands.entries())
        {
            if (index > 0)
                await delay(CHUNK_DELAY_MS);

            const uri = await buildBatchUri(command);
            await openUri(uri);
            sent += command.length;

            // The length is shown on purpose: a longer command is silently ignored by the browser.
            const row = elements.list.firstElementChild || rows[0];
            setStatus(row, commands.length > 1
                ? `已发送 ${sent}/${entries.length} 条，第 ${index + 1}/${commands.length} 批`
                : `已发送 ${sent} 条（命令 ${uri.length} 字符）`,
                uri.length > MAX_COMMAND_LENGTH);
        }
    }
    catch (error)
    {
        setStatus(elements.list.firstElementChild || rows[0],
            `批量添加失败：${error?.message || error}`, true);
        return;
    }

    for (const row of [...elements.list.children].slice(1))
        setStatus(row, '已发送到播放器', false);
});

elements.clear.addEventListener('click', async () =>
{
    const reply = await request({type: 'clear', tabId});
    if (reply)
        apply(reply.items);
});

/** The popup cannot run the sniffer itself, so it asks the content script of the tab to do it. */
elements.rescan.addEventListener('click', async () =>
{
    if (tabId < 0)
        return;

    elements.rescan.disabled = true;
    elements.rescan.textContent = '扫描中…';
    rescanning = true;
    try
    {
        await chrome.tabs.sendMessage(tabId, {type: 'rescan'});
    }
    catch (error)
    {
        // A restricted page, or a document without the content script.
        elements.subtitle.textContent = '这个页面无法重新扫描';
    }
    setTimeout(() =>
    {
        rescanning = false;
        elements.rescan.disabled = tabId < 0;
        elements.rescan.textContent = '重新扫描当前页面';
    }, 1000);
});

// The background tells every extension page, including this one, when the list of a tab changes.
chrome.runtime.onMessage.addListener((message) =>
{
    if (message?.type === 'mediaUpdated' && message.tabId === tabId)
        apply(message.items);
});

for (const key of ['showPanel', 'autoExpand'])
{
    elements[key].addEventListener('change', () =>
    {
        settings[key] = elements[key].checked;
        chrome.storage.sync.set({[key]: settings[key]});
    });
}

elements.playlist.addEventListener('change', () =>
{
    settings.playlist = elements.playlist.value;
    chrome.storage.sync.set({playlist: settings.playlist});
});

(async () =>
{
    settings = {...DEFAULT_SETTINGS, ...(await chrome.storage.sync.get(DEFAULT_SETTINGS))};
    elements.showPanel.checked = settings.showPanel;
    elements.autoExpand.checked = settings.autoExpand;
    elements.playlist.value = settings.playlist;

    const [tab] = await chrome.tabs.query({active: true, currentWindow: true});
    if (!tab)
    {
        elements.subtitle.textContent = '没有活动标签页';
        return;
    }

    tabId = tab.id;
    await refresh();
    setInterval(refresh, REFRESH_INTERVAL_MS);
})();
