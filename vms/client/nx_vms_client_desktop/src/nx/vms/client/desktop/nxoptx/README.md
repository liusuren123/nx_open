# nxoptx 播放列表控制协议

`nxoptx` 是一套**本地**请求/响应协议，用来让**外部程序**控制正在运行的客户端：查询播放列表、
新建播放列表、往播放列表里加本地文件、加入网络流（并指定它在客户端里显示的名称）。

协议名就是 URI scheme `nxoptx://`。

## 两种用法

### 1. 本地套接字（推荐，能拿到返回值）

客户端运行时会监听一个本地套接字：

| 平台 | 地址 |
| --- | --- |
| Windows | 命名管道 `\\.\pipe\nxoptx` |
| Linux / macOS | 本地套接字 `nxoptx` |

请求和响应都是**单行 JSON**。一个连接上可以连续发多条请求，每条请求对应一行响应。
只有运行客户端的那个用户能连上（Windows 命名管道使用创建进程的默认安全描述符）。

### 2. URI（`nxoptx://`，方便从命令行/浏览器触发）

客户端启动时会为当前用户注册 `nxoptx://` 协议处理器，对应的命令是：

```
"<客户端 exe>" --nxoptx "nxoptx://..."
```

shell 打开这样的 URI 时会启动一个**短命的转发进程**：它把命令转给正在运行的客户端，然后退出，
不会多开窗口。返回值：

* 如果 URI 里带了 `reply=<文件路径>`，响应 JSON 会写进这个文件；
* 同时也会写到标准输出。

如果客户端没在运行，返回 `{"ok":false,"error":"notRunning", ...}`。

## 动作一览

`action` 字段（大小写不敏感）：

| action | 说明 | 主要参数 |
| --- | --- | --- |
| `ping` | 探活，返回协议版本 | — |
| `help` | 列出所有 action | — |
| `listPlaylists` | 列出所有播放列表及其条目 | — |
| `createPlaylist` | 新建播放列表 | `name` |
| `renamePlaylist` | 重命名 | `playlist`, `name` |
| `removePlaylist` | 删除 | `playlist` |
| `setCurrentPlaylist` | 设为“当前播放列表” | `playlist` |
| `addFile` | 加入本地文件/文件夹 | `playlist`, `path` 或 `paths` |
| `addStream` | 加入网络流并指定显示名称 | `playlist`, `url`, `name`, `username`, `password` |
| `addStreams` | 一次加入多条网络流 | `playlist`, `items` 或 `data`（见下） |
| `removeEntry` | 删除某一行 | `playlist`, `row` |
| `getSettings` | 读取可调设置 | — |
| `setPrefetchConnections` | 设置 HLS 预取连接数（1–32），下次播放生效 | `connections` |

字段说明：

* `playlist`：播放列表 **id 或名称**（大小写不敏感）；**留空表示“当前播放列表”**，
  不存在时会自动新建一个默认列表。所以外部程序无需先知道 id。
* `name`：新建/重命名时是播放列表名；`addStream` 时是**该网络流在客户端里显示的名称**
  （也可以用别名 `title`，两者都为空时用 URL 作为名称）。
* `path` / `paths`：单个路径或路径数组。文件夹会被递归展开为其中的媒体文件。
* `items`：多条流的 JSON 数组，每个元素是 `{"url": "...", "name": "..."}`（也支持
  `username` / `password` / `title`）。用于“全部添加”：浏览器的外部协议跳转同一时间只能进行
  一个，逐条跳转会被后一条取消，所以批量添加必须是一条命令。
* `data`：与 `items` 相同的 JSON，但先做 zlib 压缩，前面加 4 字节大端未压缩长度，再 base64url
  编码。浏览器拒绝为超过约 2 KB 的外部协议 URL 发起跳转，几条流地址就会超过这个限制，所以
  “全部添加”发的是 `data`。手写请求和套接字协议用明文 `items` 即可。

## 运行期可调设置

`setPrefetchConnections` 修改有限 HLS 播放列表的并发下载连接数（默认 4，范围 1–32），
同时也会写进本地设置，下次启动客户端仍然有效：

```json
{"action": "setPrefetchConnections", "connections": 8}
```

响应：`{"ok": true, "connections": 8}`。正在播放的流保持它创建时的连接数，新的播放（或
重新打开同一个播放列表）才会用新值；空闲的下载源会被丢弃以便重建。`getSettings` 返回当前值：

```json
{"ok": true, "prefetchConnections": 4}
```

同一个值也可以在客户端「本地设置 → 高级」里的「HLS Prefetch Connections」调整。

## 响应

成功：`{"ok": true, ...}`；失败：`{"ok": false, "error": "<code>"}`。

常见 `error`：`invalidJson`、`unknownAction`、`playlistNotFound`、`noPath`、`noUrl`、
`noRow`、`noFilesFound`、`notRunning`、`badRequest`、`unavailable`。

`listPlaylists` 的响应示例：

```json
{
  "ok": true,
  "currentPlaylist": "3f0f2f2e...",
  "playlists": [
    {
      "id": "3f0f2f2e...",
      "name": "Default",
      "current": true,
      "entries": [
        {"row": 0, "kind": "file",   "path": "C:\\videos\\a.mp4"},
        {"row": 1, "kind": "folder", "path": "C:\\videos\\b"},
        {"row": 2, "kind": "stream", "url": "rtsp://host/stream", "title": "Front door"}
      ]
    }
  ]
}
```

`kind` 取值：`file` / `folder` / `stream`。

## 直接用套接字调用（示例）

Python（Windows）：

```python
import json

with open(r"\\.\pipe\nxoptx", "r+b", buffering=0) as pipe:
    def call(request):
        pipe.write((json.dumps(request) + "\n").encode())
        return json.loads(pipe.readline())

    print(call({"action": "listPlaylists"}))
    print(call({"action": "createPlaylist", "name": "My list"}))
    print(call({"action": "addStream", "playlist": "My list",
                "url": "rtsp://192.168.1.50:554/stream", "name": "前门"}))
    print(call({"action": "addFile", "playlist": "My list",
                "path": "D:/videos/movie.mp4"}))
```

PowerShell 里也可以直接调用 `nxoptx://`（把返回值写到文件）：

```powershell
Start-Process 'nxoptx://playlists?reply=C:\Temp\playlists.json'
Get-Content C:\Temp\playlists.json
```

## URI 形式

```
nxoptx://<action>?<参数>&reply=<返回文件>
```

* `action` 写在 host 或 path 里都行（`nxoptx://addStream?...` 与 `nxoptx:///addStream?...` 等价），
  大小写不敏感。
* 参数值需要做 **URL 编码**（例如 `rtsp://a/b` → `rtsp%3A%2F%2Fa%2Fb`）。
* `path` / `paths` 可以重复出现来一次加多个文件。
* `reply` 由转发进程消费，不会传给客户端。

示例（换行只是为了可读，实际是一行）：

```
nxoptx://playlists?reply=C:\Temp\playlists.json
nxoptx://createPlaylist?name=My%20list
nxoptx://addFile?playlist=My%20list&path=D%3A%2Fvideos%2Fmovie.mp4
nxoptx://addStream?playlist=My%20list&url=rtsp%3A%2F%2F192.168.1.50%3A554%2Fstream&name=%E5%89%8D%E9%97%A8
nxoptx://addStreams?data=<base64url 编码的压缩 JSON>&playlist=My%20list
```

## 注意

* 客户端必须**正在运行**；套接字协议的作用范围是本机、当前用户。
* 请求在客户端主线程里执行，加入很大的文件夹会短暂占用界面线程。
* `addFile` 会过滤不支持的文件；一个都没加进去时返回 `noFilesFound`。
* 密码不会出现在日志里；`listPlaylists` 也不返回密码。
* `nxoptx://` 处理器注册的是**当前 exe 的绝对路径**。正常安装版依赖就在 exe 旁边；
  开发构建把 Qt 放在 `.conan_short` 之类的目录里、只有 `activate_run.bat` 设过 PATH 才能加载，
  这种情况下注册的命令会自动变成 `cmd.exe /c set "PATH=<Qt 目录>;%PATH%" && ...`，
  每次启动都会重新检查并写入，所以手动设置过 PATH 之后不需要额外操作。
