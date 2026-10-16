// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
//
// Toolbar popup: the same list of found media as the in-page panel, plus the settings.

const SCHEME = 'nxoptx';
const DEFAULT_SETTINGS = {showPanel: true, playlist: '', autoExpand: false};

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
    showPanel: document.getElementById('showPanel'),
    autoExpand: document.getElementById('autoExpand'),
    playlist: document.getElementById('playlist'),
};

let settings = {...DEFAULT_SETTINGS};
let items = [];
let tabId = -1;

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
    try
    {
        const parsed = new URL(item.url);
        const name = decodeURIComponent(parsed.pathname.split('/').filter(Boolean).pop() || '');
        const base = name || parsed.hostname;
        return items.length > 1 ? `${base} #${index + 1}` : base;
    }
    catch (error)
    {
        return item.url;
    }
}

function buildUri(item, name)
{
    const playlist = settings.playlist.trim();
    const encode = encodeURIComponent;
    return `${SCHEME}://addStream?url=${encode(item.url)}&name=${encode(name)}`
        + (playlist ? `&playlist=${encode(playlist)}` : '');
}

/** The browser hands the command to the player through the registered nxoptx:// handler. */
function openUri(uri)
{
    const anchor = document.createElement('a');
    anchor.href = uri;
    anchor.style.display = 'none';
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
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

    elements.subtitle.textContent = hasItems
        ? `发现 ${items.length} 个媒体资源`
        : '还没有发现媒体资源';
}

function apply(next)
{
    items = Array.isArray(next) ? next : [];
    render();
}

elements.addAll.addEventListener('click', () =>
{
    const rows = [...elements.list.children];
    items.forEach((item, index) =>
    {
        const input = rows[index]?.querySelector('.name');
        const name = input?.value.trim() || suggestedName(item, index);
        openUri(buildUri(item, name));
        if (rows[index])
            setStatus(rows[index], '已发送到播放器', false);
    });
});

elements.clear.addEventListener('click', async () =>
{
    const reply = await request({type: 'clear', tabId});
    if (reply)
        apply(reply.items);
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
    const reply = await request({type: 'getMedia', tabId});
    apply(reply ? reply.items : []);
})();
