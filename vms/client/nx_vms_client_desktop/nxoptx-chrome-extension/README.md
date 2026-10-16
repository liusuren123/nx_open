# Nx Optx 媒体嗅探器（Chrome 扩展）

嗅探当前网页上的媒体流，在**页面右下角**弹出提示卡片，点一下就能把流加到正在运行的
**Nx Optx 播放器**里（通过 `nxoptx://` 协议，见 `../src/nx/vms/client/desktop/nxoptx/README.md`）。

## 安装

两种方式，任选一种（Chrome 的「加载已解压的扩展程序」需要一个**文件夹**）：

**A. 从 zip 安装（推荐）**

1. 解压 `nxoptx-chrome-extension.zip` 到一个**长期保留**的目录，例如
   `D:\tools\nxoptx-chrome-extension`（Chrome 会记住这个路径，别放在会被清理的地方）；
2. 打开 `chrome://extensions/`，右上角打开「开发者模式」；
3. 点「加载已解压的扩展程序」，选择解压出来的 `nxoptx-chrome-extension` 目录。

**B. 直接用仓库里的源码目录**

直接选本目录（`vms/client/nx_vms_client_desktop/nxoptx-chrome-extension`）即可，省一步解压，
但仓库目录一旦移动或重新 clone，扩展就会失效（报「无法加载」），需要重新选一次目录。

**C. 重新打包**

改完源码后重新生成 zip：

```
python package.py --out <输出路径>.zip
```

> 不建议用 `.crx`：当前 Chrome 只允许从官方商店安装 crx，本地拖入会被拒绝，
> 所以「加载已解压的扩展程序」是自用/内测唯一可靠的方式。另外开发者模式的扩展在每次
> 启动浏览器时会有一次提示，可以忽略。

## 工作原理

扩展从两个来源发现媒体：

1. **网络请求**：`chrome.webRequest` 观察每个标签页的请求，用响应头 `Content-Type`
   和 URL 后缀一起判断类型；
2. **页面 DOM**：内容脚本扫描页面上的 `<video>` / `<audio>` / `<source>`，覆盖那些
   直接从缓存读取、或由脚本动态设置、`webRequest` 看不到的情况。

识别出的类型：

| 类型 | 判定 |
| --- | --- |
| `HLS` | URL 以 `.m3u8` / `.m3u` 结尾，或响应头是 mpegurl |
| `DASH` | URL 以 `.mpd` 结尾，或响应头是 `dash+xml` |
| `视频` | `video/*` 或 `.mp4 .m4v .webm .mkv .mov .avi .flv .3gp .ogv` |
| `音频` | `audio/*` 或 `.mp3 .m4a .aac .ogg .oga .opus .flac .wav` |

**URL 后缀优先于响应头**：很多服务器把媒体当作 `application/octet-stream` 返回。

会被**丢弃**的东西：

* 分片文件 `.ts .m4s .cmfv .cmfa .vtt .key .part`——播放器要的是播放列表或容器文件，
  分片只会刷屏；
* `blob:` / `data:` / `mediasource:`（MSE 的内部数据，取不到真实地址）；
* 图片、脚本、JSON 等非媒体响应；
* 重复的同一个 URL（每个标签页最多保留 40 条）。

每个标签页的列表在地址栏徽标上显示条数；页面开始加载新文档时清空（SPA 的
`pushState` 不算）。

## 页面右下角面板

* 折叠态是一个小胶囊：「发现 N 个媒体资源」，带脉冲绿点；
* 点开后是卡片，每条资源可以：
  * **改名字**——这个名称会作为 `name` 参数传给播放器，也就是它在播放器里显示的名字；
  * **添加到播放器** / **复制链接**；
  * 从列表里移除（×）；
* 顶部可以填**目标播放列表**（留空 = 播放器的当前播放列表）；
* 底部有「全部添加」和「清空」；
* 右上角的 × 会关闭面板，本页不再自动弹出（除非后面又发现了新的资源）。

面板放在 **Shadow DOM** 里，所以网页的样式影响不到它，它也不会污染网页；固定在视口右下角，
不抢占焦点，不阻塞页面点击（胶囊和卡片之外的地方鼠标事件会穿透）。

## 工具栏弹窗

点扩展图标可以看到同一个列表，以及设置项：

* **在页面右下角显示提示面板**（默认开）
* **面板默认展开**
* **目标播放列表**

设置存在 `chrome.storage.sync`，跟随 Chrome 账号同步。

## 与播放器的联动

「添加到播放器」会构造出这样一条命令，然后交给浏览器：

```
nxoptx://addStream?url=<URL 编码的流地址>&name=<URL 编码的名称>&playlist=<URL 编码的播放列表>
```

浏览器调用已注册的 `nxoptx://` 处理器，客户端用它转发给**正在运行的**播放器，不会多开窗口。
因此：

* 播放器必须正在运行，并且已经注册过 `nxoptx://`（客户端启动时会注册）；
* 名称、URL 全部用 `encodeURIComponent` 编码，空格是 `%20`，和播放器端的
  `QUrlQuery` 解码一致（不是 `+`）；
* 浏览器首次会弹一次「是否打开 Nx MetaVMS？」的确认框，勾选记住即可。

## 已知限制

* **MSE / DRM 流**：像 YouTube、Netflix 这类用 `MediaSource` 拼帧的站点，真实分片地址
  在 JS 里，扩展拿不到；这类站点不会出现在列表里。
* **页面全屏播放时面板不可见**：浏览器全屏只渲染全屏元素，这是浏览器的限制；
  退出全屏或用工具栏弹窗即可。
* 分片（`.ts` / `.m4s`）被有意过滤掉了；如果某个站点只暴露分片、没有播放列表，
  列表会是空的。
* 有些站点把流地址放在 iframe 里，而内容脚本只在顶层文档运行，此时用工具栏弹窗仍能看到
  （嗅探是按标签页做的，不受此限制）。

## 文件

| 文件 | 说明 |
| --- | --- |
| `manifest.json` | Manifest V3 清单 |
| `background.js` | Service Worker：嗅探、过滤、去重、徽标、与页面通信 |
| `content.js` | 内容脚本：右下角面板、DOM 扫描、构造 `nxoptx://` 命令 |
| `panel.css` | 面板样式（注入 Shadow DOM） |
| `popup.html` / `popup.js` / `popup.css` | 工具栏弹窗与设置 |
| `package.py` | 把扩展打成 `nxoptx-chrome-extension.zip`（单层目录，解压即用） |
| `make_icons.py` | 重新生成 `icons/` 里的图标（需要 Pillow） |
