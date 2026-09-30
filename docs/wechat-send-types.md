# 微信发送各类消息的逆向记录

文本、群内 @、引用回复、图片、视频、文件能发（`<audio>` 转成 SILK 发语音条，超长或读不出来的作为文件），合并转发能发成「聊天记录」卡片。图片走聊天界面自己的 `rj()` 管线，视频走微信的视频发送服务，文件、回复与合并转发走 `AppMsgLogic`，见下文；转发路径那条死路也记在下面。

微信 8.0.78 / versionCode 671108664。工具见 `tools/dex*.py`（`dexmethodsig.py` / `dexinvokes.py` / `dexmethodstrings.py` 最常用；`dexfindclass.py`、`dexrefs.py` 有误报，别单独信）。**结论性事实要落到「某个类的某个字段 / 方法」上，并且能和 App 自己的调用点对上。**

## 已经落地的

| | 类 / 方法 | 说明 |
| --- | --- | --- |
| 文本 | `v51.r0.<init>(String,String,int,int,long,String)` + `doScene` | 构造器自己入库，cgi `newsendmsg` |
| 群内 @ | `v51.r0.<init>(String,String,int,int,Object,String)`，`flags=1`，`Object` 是 `HashMap{"atuserlist": "<![CDATA[wxid,wxid]]>"}` | 与聊天界面的 `com.tencent.mm.ui.g1.a`（AtSomeOneHelper）同一做法；构造器把 map 的项并进 `<msgsource>`；文本里的 `@昵称` 后接 U+2005 |
| 图片 | `ph5.n0.c(kt.d1)`（`ha0.w`）`.rj(da0.g)` | 见下文；异步，库里出现 `type=3` 的行才算发出 |
| 文件 | `pluginsdk.model.app.k0.I(dx0.r, "", "", talker, 附件路径, null)` | 见下文「文件」；行同步入库，上传随后由微信自己做 |
| 视频 | `ph5.n0.c(ab5.s)`（运行时类 `qi0.l2`）`.cj(qi0.w2, talker)` | 见下文「视频」；异步，行几乎立刻出现（status 1），上传完成后 status 2 |
| 语音 | `v61.d1.h` → 写文件 → `d1.u` → `v61.v0.dj().e()`；编码 `MediaRecorder.SilkDoEnc` | 见下文「语音」；行同步入库，微信自己的语音上传服务随后上传 |
| 引用回复 | `dx0.r`（`f` 标题、`i`=57、`x2`=`MsgQuoteItem`）+ `k0.I` | 见下文「回复」；新发送管线，回 `(0, null)`，行随后出现 |
| 合并转发 | `dx0.r.v(appmsg XML)` + `k0.I` | 见下文「合并转发」；卡片是一行 `type=49`（appmsg 19），`<recorditem>` 里带全部记录 |
| 撤回 | `com.tencent.mm.modelsimple.d1.<init>(e9,String,String)` + `doScene` | cgi `revokemsg` |
| 群管理 | `qn.p` / `qn.b` / `qn.e` | 见 [群管理写操作](wechat-room.md) |

## 一个反复踩的坑：`v51.r1` 的字段名不是语义

`v51.r1` 是发送参数构建器，字段是 `a..q` 的短名。曾按「`r1.h()` 设路径、`r1.e()` 设目标」记过一版，**是错的**。正确对应关系要从 App 自己的代码反推：

```java
// qs5.v5.hj(String talkerCsv, String path, String msgSource)  App 的「把一句话发给多个会话」
v51.r1 a19 = v51.s1.a(str4);   // str4 来自 talkerCsv.split(",")
a19.h(str4);                   // 目标 talker
a19.e(str);                    // 正文
a19.i(b41.d2.C(str4));         // 消息类型：d2.C 的入参是 talker（内部只在结尾判 @chatroom）
a19.f468468f = 1;              // flags
a19.f468470h = a17;            // msgsource 附加项（HashMap）
a19.g(str3);                   // msgSource
a19.f468471i = 5;              // 选择器：选中 dy1.g.k() 的第 5 分支
a19.a().a();                   // 构建并提交
```

判定依据（两条互相独立）：

- `dy1.g.k()` 的第 2 分支 `new r0(f468469g, f468468f, f468464b)` 对上 `r0(long,int,String)`，而那个构造器里是 `ex0.k0.yi(str, localId)`，第三个参数是 **talker**。`f468464b` 就是 `h()` 写的字段，所以 `h()` 是 talker。
- `b41.d2.C(String)` 的字节码里出现 `endsWith("@chatroom")`、`y3.J4/v4/G3`，返回 1/11/36：入参是会话名而不是路径。

`r1.i(int)` 写的是 `f468467e`，不是 `f468471i`；`f468471i` 是「用哪个 provider 分支」的选择器。

另外：`qs5.v5.hj` **不是**发图片的，是「同一句话发给多个会话」（`d2.C` 判会话类型、`com.tencent.mm.ui.g1.a(正文)` 取的是粘贴相似度之类的 msgsource）。

## 图片

### 走不通的那条：转发路径（v0.9.0 试过，v0.9.2 撤回）

App 的 `qs5.v5.fj` / `gj`（`MsgRetransmitUI` 那套）用 `v51.r0` 的媒体重载（`(String,String,int,int,Object,String)`，类型 42 / 66，本地路径放在「正文」位置）转发媒体。v0.9.0 照这条路发到 filehelper，返回 200，库里却是：

```
msgId | type | isSend | status | talker     | content                              | imgPath
3710  | 42   | 1      | 5      | filehelper | /data/user/0/.../satori-wx-tmp/x.png | NULL
```

`status=5`、`imgPath` 为空，之后不再推进。对照真发出去的图片：`type=3`、`status=2`、`content` 是带 CDN 密钥的 `<msg><img aeskey=…>` 全文。`42`/`66` 是「转发一段已经有 CDN 信息的媒体」的中间态，不是「把一个本地文件发成图片」。

### 走得通的那条：`rj()` 图片管线（v0.10.0）

v0.9.x 的结论是「App 发新图要跑 Kotlin 协程，收尾回调是 Kotlin 接口，native 造不出来，除非引入 Java 助手」。这个判断的依据是 `qs5.v5.b(…)` 里的 `dVar`（收集者）传 null 就没人订阅进度流。**订阅进度流与开始上传是两件事**，前者不是后者的前提：

```java
// ha0.w（实现 kt.d1）
public kotlinx.coroutines.flow.j rj(da0.g params) {
    ...
    qa0.g gVar = (qa0.g) fVar.a(wVar, qa0.g.class);
    i2 b17 = r2.b(1, 0, null, 6, null);                       // 进度流，只是给 UI 看的
    xe5.i.c(gVar.<SequenceLifecycleScope>, null, new qa0.f(params, gVar, b17, null), 1, null);   // <- 在这里 launch 整条发送协程
    return b17;
}
```

`rj` 自己把「准备（压缩 / 复制）→ 上传 → 入库 → 发送」整条协程 launch 出去，返回值只是给界面画进度。App 自己的 `qs5.v5.kj`（`sendImg`，日志标签 `msg_mgr_send_img`）就是调用 `rj(gVar)` 然后**直接丢弃返回值**。所以纯反射够了，不用定义 Java 类，不用加载 dex：

```
svc   = ph5.n0.c(kt.d1.class)                         // 服务注册表；运行时类是 ha0.w
ctx   = new w90.i0();  ctx.a = 4;                     // 4 = 从聊天界面发出
ctx.o = new b41.k7();                                 // 管线往里填结果的回调对象
gVar  = new da0.g(imgPath, 0, selfWxid, talker, ctx); // 0 = compressType
gVar.j = "msg_mgr_send_img";                          // 功能标签
svc.rj(gVar);                                         // 丢弃返回的进度流
```

`native/wx_send.cpp` 的 `SendImage`。`com.tencent.mm.pluginsdk.ui.tools.p0.a()`（App 在同一处先调的准备）也调一次，失败不算错。所有类与成员每次调用时现查：缺任何一个只让这一次请求失败，不影响文本发送。

**异步意味着「返回了」不等于「发出了」。** `rj` 返回时消息行还没有。`message.create` 的做法：发之前记下库的水位，`rj` 之后最多 6 秒、每 40 毫秒查一次 `type=3 AND isSend=1 AND talker=<会话> AND msgId>水位 AND length(content)>20` 的行（`StoreFindSentImage`，content 要已带 CDN XML 才算数）；查到才回 200 并带上真实的消息 id，查不到回 502 `image_unconfirmed`。查不到时再看有没有 content 空壳的行（`StoreFindStalledImage`）：有 = 微信收下了但管线卡死、图发不出去；没有 = 图片可能仍会发出。说明白而不是假报成功。等的时候占着服务线程，所以 6 秒是上限；实际管线在几百毫秒内入库。

**退化尺寸的图发不出去。** 微信自己的图片管线对 1x1 这类图会把行插成 `type=3 isSend=1` 但 `content` 恒为 `<msg></msg>`、`status` 恒为 5，之后不再推进。观察上是「收下了但永远在转圈」。8x8 及以上正常（真机实测 8 / 16 / 32 / 64 px 与正常图片全部 `status=2` 带 CDN XML）。这不是反射调用特有的坑，微信自己面对退化图也发不出。

`message.create` 的内容按 `<img>` / `<video>` / `<audio>` / `<file>` 切成有序的文本 / 媒体消息序列（图片规则如下，视频、文件、音频见各自章节；每个媒体元素单独成条，一次最多 8 个媒体元素、其中图片最多 4 张）：

- `src` 只认三种：本模块 `upload.create` 产出的 `internal:wechat/<user>/_tmp/<name>`（5 分钟有效），`data:image/…;base64,…`（解码后最多 8 MiB，落进同一个临时目录），和 `base64://`（社区通用 scheme，知微发图片就是这种；没有 mime，按魔数定格式与扩展名）。远程 `http(s)` 明确拒绝，说明改用 `upload.create` 或内联 base64：没有 HTTP 客户端，也不假装抓得到。
- 落地的文件先核魔数：JPEG、PNG、GIF、WebP 之外一律 400 `media_unsupported`。
- **所有图片先解析、核对完再开始发第一条**：任何一张坏了整条 400，不留下半条已发的消息。开始发之后失败，响应体的 `sent` 说明已经发出去几条。
- 一次最多 4 张。文本段每段最多 4000 字节（`content_too_long`）。
- `message.create` 因此把请求体上限放宽到 16 MiB，只对已验证令牌的调用者。
- 返回的每条 `Message` 里，图片的 `content` 是指向微信落盘那份缩略图的签名链接。
- GIF 按图片发，不会变成动图表情（表情走另一条路，没做）。

**这条路已在真机验证（v0.10.0 落地，v0.11.1 验收）。** 上传与 data URI 两条路、混合文本 + 图片的顺序、落库行的 `type=3 / status=2 / CDN XML` 都核过。失败的话看 `internal/status` 的 `send.last_error`（哪个类或成员没找到）与 `send.media` 计数。

## 文件（v0.12.0）

聊天界面选文件发送、第三方分享、转发，最后都汇到 `AppMsgLogic`（`pluginsdk.model.app.k0`，日志标签 `MicroMsg.AppMsgLogic`）。它接收一个已经解析好的 appmsg 内容对象和一个本地附件路径：

```java
dir  = k0.k()                                   // 附件目录，带结尾的 '/'
dest = k0.f(dir, title, ext)                    // 目录里一个不冲突的目标路径（微信自己的命名）
link(源文件, dest)                              // 硬链接，跨文件系统才复制；这样调用方的临时文件过期不影响上传
r    = dx0.r.v("<msg><appmsg appid=\"\" sdkver=\"0\"><title>名字.pdf</title>…<type>6</type>…"
                 "<appattach><totallen>字节数</totallen><attachid></attachid>…<fileext>pdf</fileext>…</appattach>…</appmsg>…</msg>")
pair = k0.I(r, "", "", talker, dest, null)      // android.util.Pair(Integer 0 = 成功, Long 本地 msgId)
```

`k0.M`（`I` 的最终落点，`qs5.v5.dj` 也是它）做三件事：`k0.a` 建 `AppAttachInfo` 并入库、往 `message` 表插行（status 1）、`k0.V` 启动上传。`dx0.r.v` 是微信自己解析 appmsg 的入口，字段（标题、`totallen`、`fileext`、类型 6）都从 XML 里来，不用去碰 `dx0.r` 那堆混淆字段。

真机验证（v0.12.0 开发通道，`filehelper`）：行 `type=1090519089`（0x41000031，文件 appmsg 的行类型）、`status=2`、content 里有 `<attachid>@cdn_…`（CDN 上传完成）。中文文件名、PDF、MP3 都通过。`message.get` 读回来是 `<file src="…/_msg/file/<id>/<签名>" title="…"/>`。

要点：

- 微信自己的路径校验：`k0.a` 拒绝落在微信外部存储前缀下、又不在附件目录里的路径，所以一律先落进 `k0.k()` 目录。
- 没有单独的「文件大小」限制在这一层；上传走微信的 CDN 通道，大文件只是慢。
- 行在 `k0.I` 返回时就已经存在，`message.create` 用返回的 id 直接轮询库里的 `status`：`2` 发出、`5` 失败（回 502 `upload_failed`）、还是 `1` 就在预算用完后照样回 200（上传还在继续）。

## 视频（v0.12.0）

新版视频发送是一个 Kotlin 协程任务，入口是特性服务 `ab5.s`（运行时类 `qi0.l2`，日志标签 `MicroMsg.VideoMsg.VideoMsgSendFeatureService`）：

```java
svc  = ph5.n0.c(ab5.s.class)
name = s61.c3.a(talker)                                   // 视频文件名基（时间戳 + 会话尾巴），微信自己会再加 "NS" 前缀
cross = new qi0.t2(null,null,null,null,null,null,false,null,null,null, false /*onlySendCompress*/, false /*forceSkipCompress*/, false /*writeImportPath*/)
w2   = new qi0.w2(name, 视频路径, 封面路径或"", false /*sendRaw*/, 播放秒数, cross, null)
svc.cj(w2, talker)                                        // boolean：任务已启动
```

- **`w2` 的 int 参数是播放秒数，不是文件大小。**它进 `<videomsg playlength>`，为 0 时微信自己用 `MediaMetadataRetriever` 量。v0.12.0 开发早期传过文件字节数，`playlength` 就成了几千万秒，别再犯。模块自己读 MP4 的 `mvhd`（`native/mp4_probe.cpp`）算出秒数传进去，同时判断「是不是真有视频轨」。
- 封面 `videoThumbPath` 可以是空串：微信自己抽帧生成缩略图（`cdnthumblength` 有值）。传了 JPEG 就用传的。
- `cj` 不检查 `RepairerConfigNewSendVideo`（那是 UI 在新旧两条路之间选择用的）；旧路径 `qs5.v5.tj` 里会用 `Context` 弹进度对话框，应用 Context 会崩，所以不走它。
- 异步：`cj` 返回时 `message` 表里已经有一行 `type=43 isSend=1`（content 是 `<msg><videomsg playlength="N"></videomsg></msg>` 的空壳，status 1）；微信压缩、上传完成后行被改写成带 `cdnvideourl` 的完整 XML，status 变 2。40 MB 的视频真机实测 20 秒到几分钟不等（取决于网络），所以 `message.create` 只等行出现（≤ 6 秒）再等 status 离开 1（≤ 8 秒，按体积给），慢的照样回 200。
- 源文件路径要在整个压缩、上传期间可读：模块先硬链接到 `send/` 子目录（6 小时后清理），临时文件 5 分钟过期不影响。
- 微信的视频管线能压缩就压（`newmd5` 与 `md5` 不同就是压过），压不了原样发；MP4 之外的容器（MKV / WebM / FLV）没让它试，模块直接当文件发。

## 语音（v0.12.1）

微信语音条是一个 SILK 流：`0x02` + `#!SILK_V3`，后面是若干包，每包 `uint16 小端长度 + 载荷`，一包 20 ms；行是 `type=34`，本地存成 `<wxid>:<毫秒>:0`，`voiceformat="4"` 的 XML 由上传时的 `voiceinfo` 记录生成。

发送用录音「停止」与转发语音共用的那段（`v61.d1`，日志标签 `MicroMsg.VoiceLogic`）：

```java
name = v61.d1.h(talker, "amr_")                   // 登记一个新的语音文件，回它的名字（所有语音文件都叫 amr_…，SILK 也是）
dest = rn3.u0.Fj(ou5.x.j, name, false, true)      // 那个名字对应的路径；只算名字，目录要自己建
写文件(dest)                                      // 目录 voice2/xx/yy/ 自己 mkdir -p
v61.d1.u(name, 毫秒, 0, null, null)               // 「停止」：建消息行（status 1）、记 voiceinfo
v61.v0.dj().e()                                   // 唤醒语音上传服务。录音停止后紧跟的那一句；漏了行会一直停在 status 1
```

`yl.x0.stop`（录音停止）就是 `d1.u(...)` 加 `v0.dj().e()`；`d1.s` 是转发语音的整套（复制文件、`h`、`u`），入口在 `MsgRetransmitUI`。真机验证：行 `type=34`、status 2、`voiceinfo` 有 TotalLen 与 VoiceLength。上传失败时 `d1.t(name)` 把登记的语音标成错误，别留一条「录音中」。

**编码用微信自己的 SILK 编码器，不带编解码器。**微信 8.0.78 本身录 Opus，再转成 SILK 兼容（`v61.w`，日志 `MicroMM.OpusToSilkConverter`）：

```java
h = MediaRecorder.SilkEncInit(16000, 16000, 4, 0)                    // com.tencent.mm.modelvoice.MediaRecorder（JNI 在 libwechatvoicesilk.so）
每 640 字节 = 320 个 16 位单声道样本（20 ms）:
  MediaRecorder.SilkDoEnc(pcm640, 640, out[1280], short[1] 长度, false, h)  // 返回 0；out[0..长度) 原样写文件，第一包自带 0x02#!SILK_V3
MediaRecorder.SilkEncUnInit(h)
```

输入侧模块自己做：WAV 直接读（`native/audio_pcm.cpp`，8/16/24/32 位整数与 32 位浮点、任意声道与采样率），其它格式用 Android 的 `MediaExtractor` + `MediaCodec` 解成 PCM（JNI，`native/wx_voice.cpp`，20 秒预算），再用加窗 sinc 重采样到 16 kHz 单声道（缩小时先低通，不会把 12 kHz 折叠成 4 kHz）。已经是合法微信 SILK 的文件原样发（缺 `0x02` 就补上）。

验证：把模块产出的 `.silk` 用微信自己的 `SKP_Silk_SDK_Decode` 解回来（`tools/dev/silk-check.c`）。440 Hz 3 秒接 660 Hz 2 秒的测试音，两种输入路径（WAV 与 MediaCodec 解的 MP3）都得到 250 包 / 5.00 秒、音高与电平对得上。M4A、OGG/Opus、AMR 都通过。

规则：≤ 60 秒（微信语音上限）且 ≥ 0.2 秒才发语音条；超长、太短、读不出来（比如图片当 `<audio>`）作为文件发，回执里是 `<file>`。`message.create` 等库里出现 `type=34` 的行，再等 status 离开「发送中」，与文件同一套预算。

## 回复（`<quote>`，v0.12.0）

聊天界面的「引用」发送在 `ChatFooter.V0`：

```java
r = new dx0.r();  r.f = 回复正文;  r.i = 57;
q = new MsgQuoteItem();               // com.tencent.mm.plugin.msgquote.model.MsgQuoteItem
q.d = 被引用消息的类型;  q.e = 被引用消息的 svrid;  q.f = 会话（fromusr）;  q.g = 发言人（chatusr）;
q.h = 发言人昵称;  q.i = 被引用行的 msgsource;  q.m = 被引用内容;  q.n = 新消息的 msgsource（群里带 atuserlist）;
q.p = strid;  q.q = 被引用消息的发送时间（秒）
r.x2 = q
k0.I(r, "", "", talker, "", null)
```

`dx0.r.u` 在 `i == 57` 时用 `p95.a` 把 `MsgQuoteItem` 序列化成 `<refermsg>`：`type` / `svrid` / `fromusr` / `chatusr` / `displayname` / `content` / `msgsource` / `strid` / `createtime`。微信自己发出去的引用回复，行类型 `822083633`。

模块的做法：

- 被引用的消息在库里按「本地 id 或 svrid」找（`StoreQuoteTarget`），找不到就不带引用；找到后 `fromusr` 是会话、`chatusr` 是真正的发言人（群里取 `wxid:\n` 头，自己发的取自己，私聊取对方）。
- **被引用内容一律当文字**：`type` 写 1，文本原样，图片写 `[图片]`、语音 `[语音]`、视频 `[视频]`、表情 `[动画表情]`、文件 `[文件] 名字`、链接 `[链接] 标题`。微信客户端渲染真实类型的引用要用原消息的 XML（缩略图 CDN 信息），自己拼容易错，这种写法在收件端显示成一行文字，稳。
- 群里带 `<at>` 的回复，`q.n` 填 `<msgsource><atuserlist><![CDATA[wxid,…]]></atuserlist></msgsource>`，`@` 是真提及。
- **`k0.I` 对回复回 `(0, null)`**：走的是 `k0.U(s0)` 那条「新发送管线」，行随后才出现，id 不回。模块记水位，轮询 `message` 表里 `type=822083633 AND isSend=1` 的新行拿到 id。`MsgQuote` 表的行（回复 → 被引用）也是微信自己的管线写的，模块不用再插（曾经自己插过一次，多余）。
- 真机验证：`filehelper` 里引用自己的一条消息，行 `type=822083633 status=2`，`<refermsg>` 内容齐全，`MsgQuote` 有配对行，`message.get` 读回 `<quote id="…"/>正文`。

## 合并转发（`<message forward>`，v0.14.0）

Satori 的 `<message forward>`（内嵌若干 `<message>`）对应微信的「聊天记录」卡片：一条 `type=49`（appmsg 子类型 19）的消息，全部记录装在 `<recorditem>` 里。发送与引用回复同路，入口是 `k0.I`，内容用 `dx0.r.v(xml)` 解析：

```java
r = dx0.r.v(xml)          // <msg><appmsg …><title>卡片标题</title><des>预览</des><type>19</type>
                          // <recorditem><![CDATA[<recordinfo>…]]></recorditem>…</appmsg>…
k0.I(r, "", "", talker, "", null)
```

`<recordinfo>` 的格式拿库里真实的聊天记录行对过（`<datalist>` → 每行一个 `<dataitem>` → `<data>`（`datasize/datatype=1/datadesc` 正文）+ `<sourceinfo>`（`displayname` 发言人）+ `<delivertime>`）。

模块侧的规则（`native/wx_forward.cpp` + `wx_send_media.cpp`）：

- 卡片标题取容器的非标准 `title` 属性；没有时按微信自己的口径来。「群聊的聊天记录」（群）、「A与B的聊天记录」（两人）、「Alice的聊天记录」（一人）。
- 每行的 `<author id name avatar>` 决定发言人；缺的按会话从联系人补（群里补群名片），一个都没有时算自己。`avatar` 只收 http(s)。
- 内嵌 `<message id="…"/>` 表示引用同一会话已有的一条**文本**消息（带它的发言人、时间与 svrId）；媒体还不行，回 `forward_media_unsupported`。id 找不到回 `forward_message_not_found`。
- 限额对齐微信自己的多选上限：一张卡 ≤ 100 行、单行正文 ≤ 4000 字节、`<recordinfo>` ≤ 256 KiB。
- 一条请求里的卡片**全部先建好再发第一条**：某一行的内容有问题就让整个请求失败，不发半截。
- `k0.I` 在新管线上回 `(0, null)`，行随后才出现：模块记水位轮询拿 id（6 秒预算），再等 status 落定。拿不到 id 回 `send_unconfirmed`。
- `<message id="…" forward/>`（按 id 转发单条）微信没有保持原样的对应物，再发一遍正文等于发成别的东西，仍然拒绝（`forward_unsupported`），提示放进 `<message forward>` 合并转发。

真机验证（v0.14.0，dev 热注入）：`filehelper` 收到卡片，行 `type=49 status=2`、msgSvrId 已回，`<appmsg>` 全文与微信自己发的卡片同构；屏幕上卡片渲染正确（标题、预览行数）；`message.get` 读回 `<message forward title="群聊的聊天记录">`，三行的发言人、头像、正文（含转义与换行）都在。

## jadx 字段名的坑

jadx 输出里 `f233841f` 这类名字是它为避免同名冲突改的，注释里写着 `renamed from: f`。**JNI 里要用原名（`f`），不是 jadx 名。**没有 `renamed from` 注释的字段（如 `field_msgId`）原样可用。这次 `dx0.r.f` / `.i` / `.x2` 与 `MsgQuoteItem` 的 `d…r` 就是这样确认的。

## 其它入口（供参考，都没接）

| 方法 | 用途 | 关键点 |
| --- | --- | --- |
| `qs5.v5.cj` / `dj(String toUser, byte[] contentXml, ...)` | AppMsg / 文件（直接给 XML） | 与 `k0.I` 同一落点（`k0.M`）；`dj` 要先有 `AppAttachInfo`，模块直接走 `k0.I` |
| `qs5.v5.ej` / `rj` / `fj(String,String,boolean,pc5.yl)` | 视频 / 语音 / 图片的转发入口 | `fj` 就是上面那条 type 42 的路 |
| `qs5.v5.nj` / `oj` / `pj(String,String,int,int,long,...)` | 文本发送（`r1` 构建器），不是视频；视频在 `sj`–`vj` → `tj` | `tj` 新路径调 `ab5.s.cj`，旧路径要 Context 弹窗 |
| `qs5.v5.bj(Context,int,List,rn3.x0)` | 批量发图（朋友圈） | |
| `dy1.g.h()` → `s61.w0` / `s61.q1` / `s61.r1` | 文件 / AppMsg / 视频（`l51.m1` 子类 + `f468471i` 取 1/4/20） | 没用；文件、视频改走上面的入口 |

## 其它可复用的事实

- `ActivityThread.currentApplication()` 拿 Context；`getClassLoader()` 拿宿主 ClassLoader。
- `gp0.j1.e()` 与 `b41.h9.e()` 都返回 `com.tencent.mm.modelbase.r1`（NetSceneQueue）。
- `ex0.k0.F0` 是 `ex0.j0` 单例，`k(String talker, long localId)` 取 `com.tencent.mm.storage.e9`（MsgInfo）。
- `v51.n1.a()` 的实现是 `((gp0.n) z2.f59772b).f268046a.f268074b.h(scene, 0)`，与群管理写操作的 Cgi 派发是同一个队列。
- 这些类都在可读 dex 里；**优先用可读 dex 拼路径，别去碰 `libapp.so`**。
