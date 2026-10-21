// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
//
// Content script of the Nx Optx media sniffer: draws the "media found" card in the bottom right
// corner of the page and turns a click into an nxoptx:// command for the running player.

(async () =>
{
    if (window.top !== window)
        return; //< Only the top document gets a panel.
    if (window.__nxoptxInstalled)
        return;
    window.__nxoptxInstalled = true;

    const SCHEME = 'nxoptx';
    const DEFAULT_SETTINGS = {showPanel: false, playlist: '', autoExpand: false};

    const KIND_INFO = {
        hls: {label: 'HLS', color: '#e2a03f'},
        dash: {label: 'DASH', color: '#8d7bd6'},
        video: {label: '视频', color: '#3a911e'},
        audio: {label: '音频', color: '#3d8fd4'},
    };

    let settings = {...DEFAULT_SETTINGS};
    let items = [];
    let expanded = false;
    let dismissed = false;
    let lastCount = 0;
    let lastKey = null;
    let rescanButton = null;
    let rescanUntil = 0;
    let rescanTimer = 0;

    // ---------------------------------------------------------------- messaging

    function request(message)
    {
        return new Promise((resolve) =>
        {
            try
            {
                chrome.runtime.sendMessage(message, (reply) =>
                {
                    void chrome.runtime.lastError; //< The worker may be restarting.
                    resolve(reply || null);
                });
            }
            catch (error)
            {
                resolve(null); //< The extension was reloaded; the panel is stale.
            }
        });
    }

    // ---------------------------------------------------------------- panel

    const host = document.createElement('div');
    host.id = 'nxoptx-host';
    const shadow = host.attachShadow({mode: 'open'});

    // A link to the packaged stylesheet rather than a fetch, so the shadow root is themed without
    // depending on the page's request rules. panel.css is listed in web_accessible_resources.
    const style = document.createElement('link');
    style.rel = 'stylesheet';
    style.href = chrome.runtime.getURL('panel.css');
    shadow.append(style);

    const root = document.createElement('div');
    root.className = 'nx-root';
    shadow.append(root);

    const pill = document.createElement('button');
    pill.className = 'nx-pill';
    pill.type = 'button';

    const card = document.createElement('div');
    card.className = 'nx-card';

    root.append(pill, card);

    function hide()
    {
        host.style.display = 'none';
    }

    function show()
    {
        host.style.display = 'block';
    }

    function renderPill()
    {
        pill.textContent = '';
        const dot = document.createElement('span');
        dot.className = 'nx-dot';
        const text = document.createElement('span');
        text.textContent = `发现 ${items.length} 个媒体资源`;
        pill.append(dot, text);
    }

    function setStatus(row, text, isError)
    {
        const status = row.querySelector('.nx-status');
        status.textContent = text;
        status.classList.toggle('nx-error', !!isError);
        if (!text)
            return;
        clearTimeout(status.__nxTimer);
        status.__nxTimer = setTimeout(() => { status.textContent = ''; }, 4000);
    }

    function suggestedName(item)
    {
        const stored = (item.name || '').replace(/\s+/g, ' ').trim();
        if (stored)
            return stored;

        // Entries remembered before names were stored have none; derive one the same way the
        // background does.
        let base = (item.title || document.title || '').replace(/\s+/g, ' ').trim();
        if (!base)
        {
            try
            {
                const parsed = new URL(item.url);
                base = decodeURIComponent(
                    parsed.pathname.split('/').filter(Boolean).pop() || '') || parsed.hostname;
            }
            catch (error)
            {
                base = item.url;
            }
        }
        if (item.label)
            base = `${base} ${item.label}`;
        if (base.length > 60)
            base = base.slice(0, 59) + '…';
        return base;
    }

    function buildUri(item, name)
    {
        const playlist = settings.playlist.trim();
        const encode = encodeURIComponent;
        let uri = `${SCHEME}://addStream?url=${encode(item.url)}&name=${encode(name)}`;

        // The high quality tracks are only used when the item is cached locally.
        if (item.videoUrl)
            uri += `&videoUrl=${encode(item.videoUrl)}`;
        if (item.audioUrl)
            uri += `&audioUrl=${encode(item.audioUrl)}`;
        if (item.group)
            uri += `&group=${encode(item.group)}`;

        return uri + (playlist ? `&playlist=${encode(playlist)}` : '');
    }

    /**
     * Chrome refuses to start an external protocol navigation for a URL longer than about 2 KB, and
     * a batch of stream addresses is easily longer. The payload is therefore deflated and
     * base64-encoded; Qt's qUncompress() expects the uncompressed size as a big-endian 32-bit
     * prefix.
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
     * A batch is one command, not one command per item: the browser may only start one external
     * protocol navigation at a time, so separate launches cancel each other and only the last
     * stream would arrive.
     */
    async function buildBatchUri(entries)
    {
        const payload = JSON.stringify(entries.map(
            ({url, name, videoUrl, audioUrl, group}) => ({url, name, videoUrl, audioUrl, group})));
        const packed = await packPayload(payload);
        const playlist = settings.playlist.trim();
        const encode = encodeURIComponent;
        return `${SCHEME}://addStreams?data=${encode(packed)}`
            + (playlist ? `&playlist=${encode(playlist)}` : '');
    }

    /**
     * Hands the command to the browser, which passes it to the player through the registered
     * nxoptx:// handler. The page itself is not navigated anywhere.
     */
    function openUri(uri)
    {
        const anchor = document.createElement('a');
        anchor.href = uri;
        anchor.style.display = 'none';
        document.documentElement.append(anchor);
        anchor.click();
        anchor.remove();
    }

    function addItem(item, name, row)
    {
        openUri(buildUri(item, name || suggestedName(item)));
        setStatus(row, '已发送到播放器', false);
    }

    async function copyText(text, row)
    {
        try
        {
            await navigator.clipboard.writeText(text);
            setStatus(row, '已复制', false);
        }
        catch (error)
        {
            setStatus(row, '复制失败，请手动选择', true);
        }
    }

    function renderItem(item, index)
    {
        const info = KIND_INFO[item.kind] || KIND_INFO.video;

        const row = document.createElement('li');
        row.className = 'nx-item';

        const top = document.createElement('div');
        top.className = 'nx-item-top';

        const kind = document.createElement('span');
        kind.className = 'nx-kind';
        kind.textContent = info.label;
        kind.style.color = info.color;
        kind.style.borderColor = info.color;

        const name = document.createElement('input');
        name.className = 'nx-name';
        name.type = 'text';
        name.value = suggestedName(item);
        name.title = '在播放器里显示的名称';

        // The list outlives the page, so a name the user types has to be stored as well.
        name.addEventListener('input', () =>
        {
            const target = items.find((candidate) => candidate.url === item.url);
            if (target)
                target.name = name.value;

            clearTimeout(name.renameTimer);
            name.renameTimer = setTimeout(() =>
            {
                request({type: 'rename', url: item.url, name: name.value});
            }, 400);
        });

        const remove = document.createElement('button');
        remove.className = 'nx-icon';
        remove.type = 'button';
        remove.title = '从列表中移除';
        remove.textContent = '×';
        remove.addEventListener('click', async () =>
        {
            const reply = await request({type: 'remove', url: item.url});
            if (reply)
                applyItems(reply.items);
        });

        top.append(kind, name, remove);

        const url = document.createElement('div');
        url.className = 'nx-url';
        url.textContent = item.url;
        url.title = item.url;

        const actions = document.createElement('div');
        actions.className = 'nx-actions';

        const add = document.createElement('button');
        add.className = 'nx-primary';
        add.type = 'button';
        add.textContent = '添加到播放器';
        add.addEventListener('click', () => addItem(item, name.value.trim(), row));

        const copy = document.createElement('button');
        copy.className = 'nx-ghost';
        copy.type = 'button';
        copy.textContent = '复制链接';
        copy.addEventListener('click', () => copyText(item.url, row));

        const status = document.createElement('span');
        status.className = 'nx-status';

        actions.append(add, copy, status);
        row.append(top, url, actions);
        return row;
    }

    function renderCard()
    {
        card.textContent = '';

        const head = document.createElement('div');
        head.className = 'nx-head';

        const logo = document.createElement('span');
        logo.className = 'nx-logo';

        const title = document.createElement('span');
        title.className = 'nx-title';
        title.append(`发现 `);
        const count = document.createElement('b');
        count.textContent = String(items.length);
        title.append(count, ' 个媒体资源');

        const close = document.createElement('button');
        close.className = 'nx-icon';
        close.type = 'button';
        close.title = '关闭（本页不再自动弹出）';
        close.textContent = '×';
        close.addEventListener('click', () =>
        {
            dismissed = true;
            expanded = false;
            updateVisibility();
        });

        head.append(logo, title, close);

        const playlistRow = document.createElement('label');
        playlistRow.className = 'nx-playlist-row';
        const playlistLabel = document.createElement('span');
        playlistLabel.textContent = '播放列表';
        const playlistInput = document.createElement('input');
        playlistInput.className = 'nx-playlist';
        playlistInput.type = 'text';
        playlistInput.placeholder = '（当前播放列表）';
        playlistInput.value = settings.playlist;
        playlistInput.addEventListener('change', () =>
        {
            settings.playlist = playlistInput.value;
            chrome.storage.sync.set({playlist: settings.playlist});
        });
        playlistRow.append(playlistLabel, playlistInput);

        const list = document.createElement('ul');
        list.className = 'nx-list';
        items.forEach((item, index) => list.append(renderItem(item, index)));

        const foot = document.createElement('div');
        foot.className = 'nx-foot';

        const addAll = document.createElement('button');
        addAll.className = 'nx-primary';
        addAll.type = 'button';
        addAll.textContent = '全部添加';
        addAll.addEventListener('click', async () =>
        {
            if (!items.length)
                return;

            const rows = [...list.children];
            const entries = items.map((item, index) =>
            {
                const input = rows[index]?.querySelector('.nx-name');
                return {
                    url: item.url,
                    name: input?.value.trim() || suggestedName(item),
                    videoUrl: item.videoUrl || '',
                    audioUrl: item.audioUrl || '',
                    group: item.group || '',
                };
            });

            openUri(await buildBatchUri(entries));

            for (const row of rows)
                setStatus(row, '已发送到播放器', false);
        });

        rescanButton = document.createElement('button');
        rescanButton.className = 'nx-ghost';
        rescanButton.type = 'button';
        rescanButton.title = '重新扫描当前页面的媒体资源';
        rescanButton.addEventListener('click', () => rescan());
        updateRescanButton();

        const clear = document.createElement('button');
        clear.className = 'nx-ghost';
        clear.type = 'button';
        clear.textContent = '清空';
        clear.addEventListener('click', async () =>
        {
            const reply = await request({type: 'clear', pageUrl: location.href});
            applyItems(reply ? reply.items : []);
        });

        const hint = document.createElement('span');
        hint.className = 'nx-hint';
        hint.textContent = '需要播放器正在运行';

        foot.append(addAll, rescanButton, clear, hint);
        card.append(head, playlistRow, list, foot);
    }

    function updateVisibility()
    {
        if (!settings.showPanel || items.length === 0)
        {
            hide();
            return;
        }

        // A dismissed panel stays out of the way, unless something new shows up.
        if (dismissed && items.length <= lastCount)
        {
            hide();
            return;
        }

        show();
        pill.style.display = expanded ? 'none' : 'flex';
        card.style.display = expanded ? 'block' : 'none';
    }

    function applyItems(next)
    {
        // The background keeps one list for the whole browser; the panel shows the resources that
        // were found in this document.
        const list = (Array.isArray(next) ? next : []).filter(
            (item) => !item.pageUrl || item.pageUrl === location.href);

        // The same list arrives on every media event of the page; rebuilding the card each time
        // would restart its animation and throw away the names the user is typing.
        const key = list.map((item) => item.url).join('\n');
        if (key === lastKey)
            return;

        lastKey = key;
        items = list;
        renderPill();
        renderCard();
        updateVisibility();
        lastCount = items.length;
    }

    pill.addEventListener('click', () =>
    {
        expanded = true;
        updateVisibility();
    });

    // ---------------------------------------------------------------- wiring

    document.documentElement.append(host);
    hide();

    settings = {...DEFAULT_SETTINGS, ...(await chrome.storage.sync.get(DEFAULT_SETTINGS))};
    expanded = settings.autoExpand;

    chrome.runtime.onMessage.addListener((message, sender, sendResponse) =>
    {
        if (message?.type === 'mediaUpdated')
            applyItems(message.items);
        else if (message?.type === 'rescan')
        {
            rescan();
            sendResponse({ok: true}); //< The popup awaits the reply.
        }
    });

    chrome.storage.onChanged.addListener((changes, area) =>
    {
        if (area !== 'sync')
            return;
        for (const [key, change] of Object.entries(changes))
            settings[key] = change.newValue;
        updateVisibility();
    });

    /** Media set through the DOM is not always visible to webRequest (a cached source, for one). */
    function scanDocument()
    {
        const found = [];
        for (const element of document.querySelectorAll('video, audio, source'))
        {
            const raw = element.currentSrc || element.src || element.getAttribute('src');
            if (!raw || raw.startsWith('blob:') || raw.startsWith('data:'))
                continue;
            try
            {
                found.push({url: new URL(raw, location.href).href, mime: ''});
            }
            catch (error)
            {
                // Not a usable URL.
            }
        }
        return found;
    }

    async function registerItems(found, options)
    {
        const {origin = 'dom', replace = false} = options || {};
        if (!found.length && !replace)
            return;

        const reply = await request({type: 'register', items: found, origin, replace,
            pageUrl: location.href, pageTitle: document.title});
        if (reply)
            applyItems(reply.items);
    }

    async function scanAndReport()
    {
        await registerItems(scanDocument());
    }

    function updateRescanButton()
    {
        if (!rescanButton || !rescanButton.isConnected)
            return;
        const busy = Date.now() < rescanUntil;
        rescanButton.disabled = busy;
        rescanButton.textContent = busy ? '扫描中…' : '重新扫描';
    }

    /**
     * Re-runs both sniffers: the DOM one and the page-world one. Each source replaces what it
     * reported before, because a signed stream address stops working after a while.
     */
    async function rescan()
    {
        rescanUntil = Date.now() + 1200;
        updateRescanButton();
        clearTimeout(rescanTimer);
        rescanTimer = setTimeout(updateRescanButton, 1200);

        await registerItems(scanDocument(), {origin: 'dom', replace: true});
        window.postMessage({source: 'nxoptx-content', type: 'rescan'}, '*');
    }

    // Some players keep their media in their own JSON rather than in the DOM or in requests the
    // extension can read (Bilibili, for one). A page-world script reports those URLs here; see
    // bilibili-sniffer.js.
    window.addEventListener('message', (event) =>
    {
        if (event.source !== window)
            return;
        const data = event.data;
        if (!data || data.source !== 'nxoptx-page' || data.type !== 'media')
            return;
        registerItems(Array.isArray(data.items) ? data.items : [],
            {origin: 'page', replace: true});
    });

    // The page-world script may have found something before this script started.
    window.postMessage({source: 'nxoptx-content', type: 'hello'}, '*');

    const initial = await request({type: 'getMedia'});
    if (initial)
        applyItems(initial.items);

    await scanAndReport();

    document.addEventListener('loadedmetadata', scanAndReport, true);
    document.addEventListener('loadstart', scanAndReport, true);
})();
