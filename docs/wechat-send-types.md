# 发送各类消息

文本、群内 @、引用回复、图片、视频、文件、语音条、合并转发、撤回与群管理能发。图片走聊天界面自己的 `rj()` 管线，视频走微信的视频发送服务 `ab5.s`，文件、回复与合并转发走 `AppMsgLogic`（`k0.I`）。

微信 8.0.78 / versionCode 671108664。离线工具见 `tools/dex*.py`，JADX 输出里的 `f233841f` 这类改名要在注释 `renamed from` 里找原名，JNI 用原名。

## 落点一览

| | 类 / 方法 | 说明 |
| --- | --- | --- |
| 文本 | `v51.r0.<init>(String,String,int,int,long,String)` + `doScene` | 构造器自己入库，cgi `newsendmsg`，行 `type=1` |
| 群内 @ | `v51.r0.<init>(String,String,int,int,Object,String)`，`flags=1`，`Object` 是 `HashMap{"atuserlist": "<![CDATA[wxid,wxid]]>"}` | 与聊天界面的 `com.tencent.mm.ui.g1.a`（AtSomeOneHelper）同一做法。构造器把 map 的项并进 `<msgsource>`，文本里的 `@昵称` 后接 U+2005 |
| 图片 | `ph5.n0.c(kt.d1)`（`ha0.w`）`.rj(da0.g)` | 见下文。异步，库里出现 `type=3` 且 content 已带 CDN XML 的行才算发出 |
| 文件 | `pluginsdk.model.app.k0.I(dx0.r, "", "", talker, 附件路径, null)` | 行同步入库（`type=1090519089`），上传随后由微信自己做 |
| 视频 | `ph5.n0.c(ab5.s)`（运行时类 `qi0.l2`）`.cj(qi0.w2, talker)` | 行几乎立刻出现（`type=43`，status 1），上传完成后 status 2 |
| 语音 | `v61.d1.h` → 写文件 → `d1.u` → `v61.v0.dj().e()`；编码 `MediaRecorder.SilkDoEnc` | 行同步入库（`type=34`），微信自己的语音上传服务随后上传 |
| 引用回复 | `dx0.r`（`f` 标题、`i`=57、`x2`=`MsgQuoteItem`）+ `k0.I` | 新发送管线，回 `(0, null)`，行随后出现，`type=822083633` |
| 合并转发 | `dx0.r.v(appmsg XML)` + `k0.I` | 卡片是一行 `type=49`（appmsg 19），`<recorditem>` 里带全部记录 |
| 撤回 | `com.tencent.mm.modelsimple.d1.<init>(e9,String,String)` + `doScene` | cgi `revokemsg` |
| 群管理 | `qn.p` / `qn.b` / `qn.e` | 见[群管理写操作](wechat-room.md) |

## `v51.r1` 的字段对应

`v51.r1` 是发送参数构建器，字段是 `a..q` 的短名。对应关系取自 App 自己的调用点 `qs5.v5.hj(String talkerCsv, String path, String msgSource)`：

```java
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

两条判定依据：`dy1.g.k()` 的第 2 分支 `new r0(f468469g, f468468f, f468464b)` 对上 `r0(long,int,String)`，那个构造器里是 `ex0.k0.yi(str, localId)`，第三个参数是 talker，所以 `f468464b` 是 `h()` 写的字段。`b41.d2.C(String)` 的字节码里出现 `endsWith("@chatroom")`、`y3.J4/v4/G3`，返回 1/11/36，入参是会话名而不是路径。

`r1.i(int)` 写的是 `f468467e`，不是 `f468471i`。`f468471i` 是「用哪个 provider 分支」的选择器。

`qs5.v5.hj` 是「同一句话发给多个会话」，不是发图片的：`d2.C` 判会话类型，`com.tencent.mm.ui.g1.a(正文)` 取的是 msgsource。

## 图片

`rj()` 是聊天界面自己的图片管线：

```java
// ha0.w（实现 kt.d1）
public kotlinx.coroutines.flow.j rj(da0.g params) {
    ...
    qa0.g gVar = (qa0.g) fVar.a(wVar, qa0.g.class);
    i2 b17 = r2.b(1, 0, null, 6, null);                       // 进度流，只给界面画进度
    xe5.i.c(gVar.<SequenceLifecycleScope>, null, new qa0.f(params, gVar, b17, null), 1, null);
    return b17;
}
```

`rj` 自己把「准备（压缩 / 复制）、上传、入库、发送」整条协程 launch 出去，返回值只是进度流。订阅进度流与开始上传是两件事。App 自己的 `qs5.v5.kj`（`sendImg`，日志标签 `msg_mgr_send_img`）调用 `rj(gVar)` 后直接丢弃返回值。所以纯反射够用：

```
svc   = ph5.n0.c(kt.d1.class)                         // 服务注册表；运行时类是 ha0.w
ctx   = new w90.i0();  ctx.a = 4;                     // 4 = 从聊天界面发出
ctx.o = new b41.k7();                                 // 管线往里填结果的回调对象
gVar  = new da0.g(imgPath, 0, selfWxid, talker, ctx); // 0 = compressType
gVar.j = "msg_mgr_send_img";                          // 功能标签
svc.rj(gVar);                                         // 丢弃返回的进度流
```

这是 `native/wx_send.cpp` 的 `SendImage`。`com.tencent.mm.pluginsdk.ui.tools.p0.a()`（App 在同一处先调的准备）也调一次，失败不算错。所有类与成员每次调用时现查，缺任何一个只让这一次请求失败。

### 确认入库

`rj` 返回时消息行还没有。`message.create` 在发之前记下库的水位，`rj` 之后最多 6 秒、每 40 毫秒查一次 `type=3 AND isSend=1 AND talker=<会话> AND msgId>水位 AND length(content)>20` 的行（`StoreFindSentImage`）。查到才回 200 并带上真实的消息 id，查不到回 502 `image_unconfirmed`。查不到时再看有没有 content 空壳的行（`StoreFindStalledImage`）：有就是微信收下了但管线卡死、图发不出去。

退化尺寸的图发不出去。微信的图片管线对 1x1 这类图会把行插成 `type=3 isSend=1` 但 `content` 恒为 `<msg></msg>`、`status` 恒为 5，之后不再推进。8x8 及以上正常（实测 8 / 16 / 32 / 64 px 与正常图片全部 `status=2` 带 CDN XML）。

### 图片规则

`message.create` 的内容按 `<img>` / `<video>` / `<audio>` / `<file>` 切成有序的文本 / 媒体消息序列。每个媒体元素单独成条，一次最多 8 个媒体元素、其中图片最多 4 张。

- `src` 只认三种：`upload.create` 产出的 `internal:wechat/<user>/_tmp/<name>`（5 分钟有效），`data:image/…;base64,…`（解码后最多 8 MiB，落进同一个临时目录），`base64://`（社区通用 scheme，没有 mime，按魔数定格式与扩展名）。远程 `http(s)` 拒绝并提示改用 `upload.create` 或内联 base64。
- 落地的文件先核魔数：JPEG、PNG、GIF、WebP 之外一律 400 `media_unsupported`。
- 所有图片先解析、核对完再开始发第一条。坏一张整条 400，不留下半条已发的消息。开始发之后失败时，响应体的 `sent` 说明已经发出去几条。
- 文本段每段最多 4000 字节（`content_too_long`）。`message.create` 因此把请求体上限放宽到 16 MiB，只对已验证令牌的调用者。
- 返回的每条 `Message` 里，图片的 `content` 指向微信落盘那份缩略图的签名链接。
- GIF 按图片发，不变成动图表情。

## 文件

聊天界面选文件发送、第三方分享、转发最后都汇到 `AppMsgLogic`（`pluginsdk.model.app.k0`，日志标签 `MicroMsg.AppMsgLogic`）。它接收一个已解析好的 appmsg 内容对象和一个本地附件路径：

```java
dir  = k0.k()                                   // 附件目录，带结尾的 '/'
dest = k0.f(dir, title, ext)                    // 目录里一个不冲突的目标路径
link(源文件, dest)                              // 硬链接，跨文件系统才复制
r    = dx0.r.v("<msg><appmsg appid=\"\" sdkver=\"0\"><title>名字.pdf</title>…<type>6</type>…"
                 "<appattach><totallen>字节数</totallen><attachid></attachid>…<fileext>pdf</fileext>…</appattach>…</appmsg>…</msg>")
pair = k0.I(r, "", "", talker, dest, null)      // android.util.Pair(Integer 0 = 成功, Long 本地 msgId)
```

`k0.M`（`I` 的最终落点，`qs5.v5.dj` 也是它）做三件事：`k0.a` 建 `AppAttachInfo` 并入库，往 `message` 表插行（status 1），`k0.V` 启动上传。`dx0.r.v` 是微信自己解析 appmsg 的入口，字段（标题、`totallen`、`fileext`、类型 6）都从 XML 里来。

要点：

- 微信自己的路径校验：`k0.a` 拒绝落在微信外部存储前缀下、又不在附件目录里的路径，所以一律先落进 `k0.k()` 目录。
- 这一层没有单独的文件大小限制。上传走微信的 CDN 通道，大文件只是慢。
- 行在 `k0.I` 返回时就已经存在。`message.create` 用返回的 id 轮询库里的 `status`：`2` 发出、`5` 失败（回 502 `upload_failed`）、还是 `1` 就在预算用完后照样回 200。
- 回读是 `<file src="…/_msg/file/<id>/<签名>" title="…"/>`。

## 视频

视频发送是 Kotlin 协程任务，入口是特性服务 `ab5.s`（运行时类 `qi0.l2`，日志标签 `MicroMsg.VideoMsg.VideoMsgSendFeatureService`）：

```java
svc  = ph5.n0.c(ab5.s.class)
name = s61.c3.a(talker)                                   // 视频文件名基，微信自己会再加 "NS" 前缀
cross = new qi0.t2(null,null,null,null,null,null,false,null,null,null, false /*onlySendCompress*/, false /*forceSkipCompress*/, false /*writeImportPath*/)
w2   = new qi0.w2(name, 视频路径, 封面路径或"", false /*sendRaw*/, 播放秒数, cross, null)
svc.cj(w2, talker)                                        // boolean：任务已启动
```

- `w2` 的 int 参数是播放秒数，不是文件大小。它进 `<videomsg playlength>`，为 0 时微信自己用 `MediaMetadataRetriever` 量。模块自己读 MP4 的 `mvhd`（`native/mp4_probe.cpp`）算出秒数传进去，同时判断是不是真有视频轨。
- 封面 `videoThumbPath` 可以是空串，微信自己抽帧生成缩略图（`cdnthumblength` 有值）。传了 JPEG 就用传的。
- `cj` 不检查 `RepairerConfigNewSendVideo`（那是 UI 在新旧两条路之间选择用的）。旧路径 `qs5.v5.tj` 会用 `Context` 弹进度对话框，应用 Context 会崩。
- 异步：`cj` 返回时 `message` 表里已有一行 `type=43 isSend=1`（content 是 `<msg><videomsg playlength="N"></videomsg></msg>` 的空壳，status 1）。微信压缩、上传完成后行被改写成带 `cdnvideourl` 的完整 XML，status 变 2。`message.create` 只等行出现（≤ 6 秒）再等 status 离开 1（≤ 8 秒，按体积给），慢的照样回 200。
- 源文件路径要在整个压缩、上传期间可读。模块先硬链接到 `send/` 子目录（6 小时后清理）。
- 微信能压就压（`newmd5` 与 `md5` 不同就是压过），压不了原样发。MP4 之外的容器（MKV / WebM / FLV）模块直接当文件发。

## 语音

语音条是 SILK 流：`0x02` + `#!SILK_V3`，后面是若干包，每包 `uint16 小端长度 + 载荷`，一包 20 ms。行是 `type=34`，本地存成 `<wxid>:<毫秒>:0`，`voiceformat="4"` 的 XML 由上传时的 `voiceinfo` 记录生成。

发送用录音「停止」与转发语音共用的那段（`v61.d1`，日志标签 `MicroMsg.VoiceLogic`）：

```java
name = v61.d1.h(talker, "amr_")                   // 登记一个新的语音文件，回它的名字
dest = rn3.u0.Fj(ou5.x.j, name, false, true)      // 那个名字对应的路径；只算名字，目录要自己建
写文件(dest)                                      // 目录 voice2/xx/yy/ 自己 mkdir -p
v61.d1.u(name, 毫秒, 0, null, null)               // 「停止」：建消息行（status 1）、记 voiceinfo
v61.v0.dj().e()                                   // 唤醒语音上传服务，漏了行会一直停在 status 1
```

`yl.x0.stop`（录音停止）就是 `d1.u(...)` 加 `v0.dj().e()`。`d1.s` 是转发语音的整套（复制文件、`h`、`u`），入口在 `MsgRetransmitUI`。上传失败时 `d1.t(name)` 把登记的语音标成错误。

编码用微信自己的 SILK 编码器，模块不带编解码器。微信 8.0.78 本身录 Opus 再转成 SILK 兼容（`v61.w`，日志 `MicroMM.OpusToSilkConverter`）：

```java
h = MediaRecorder.SilkEncInit(16000, 16000, 4, 0)                    // com.tencent.mm.modelvoice.MediaRecorder
每 640 字节 = 320 个 16 位单声道样本（20 ms）:
  MediaRecorder.SilkDoEnc(pcm640, 640, out[1280], short[1] 长度, false, h)
MediaRecorder.SilkEncUnInit(h)
```

输入侧模块自己做：WAV 直接读（`native/audio_pcm.cpp`，8/16/24/32 位整数与 32 位浮点、任意声道与采样率），其它格式用 Android 的 `MediaExtractor` + `MediaCodec` 解成 PCM（JNI，`native/wx_voice.cpp`，20 秒预算），再用加窗 sinc 重采样到 16 kHz 单声道（缩小时先低通）。已经是合法微信 SILK 的文件原样发（缺 `0x02` 就补上）。

规则：≤ 60 秒（微信语音上限）且 ≥ 0.2 秒才发语音条。超长、太短、读不出来的作为文件发，回执里是 `<file>`。`message.create` 等库里出现 `type=34` 的行，再等 status 离开「发送中」，与文件同一套预算。

校验用微信自己的 `SKP_Silk_SDK_Decode` 解回模块产出的 `.silk`（`tools/dev/silk-check.c`）：440 Hz 3 秒接 660 Hz 2 秒的测试音，WAV 与 MediaCodec 解的 MP3 两条输入路径都得到 250 包 / 5.00 秒。M4A、OGG/Opus、AMR 都可用。

## 回复（`<quote>`）

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

- 被引用的消息在库里按「本地 id 或 svrid」找（`StoreQuoteTarget`），找不到就不带引用。找到后 `fromusr` 是会话、`chatusr` 是真正的发言人（群里取 `wxid:\n` 头，自己发的取自己，私聊取对方）。
- 被引用内容一律当文字：`type` 写 1，文本原样，图片写 `[图片]`、语音 `[语音]`、视频 `[视频]`、表情 `[动画表情]`、文件 `[文件] 名字`、链接 `[链接] 标题`。微信客户端渲染真实类型的引用要用原消息的 XML（缩略图 CDN 信息），自己拼容易错。
- 群里带 `<at>` 的回复，`q.n` 填 `<msgsource><atuserlist><![CDATA[wxid,…]]></atuserlist></msgsource>`，`@` 是真提及。
- `k0.I` 对回复回 `(0, null)`。走的是 `k0.U(s0)` 那条新发送管线，行随后才出现，id 不回。模块记水位，轮询 `message` 表里 `type=822083633 AND isSend=1` 的新行拿 id。`MsgQuote` 表的行由微信自己的管线写。

## 合并转发（`<message forward>`）

内嵌若干 `<message>` 的 `<message forward>` 对应微信的「聊天记录」卡片：一条 `type=49`（appmsg 子类型 19）的消息，全部记录装在 `<recorditem>` 里。发送与引用回复同路，入口是 `k0.I`，内容用 `dx0.r.v(xml)` 解析：

```java
r = dx0.r.v(xml)          // <msg><appmsg …><title>卡片标题</title><des>预览</des><type>19</type>
                          // <recorditem><![CDATA[<recordinfo>…]]></recorditem>…</appmsg>…
k0.I(r, "", "", talker, "", null)
```

`<recordinfo>` 的格式：`<datalist>` 里每行一个 `<dataitem>`，含 `<data>`（`datasize` / `datatype=1` / `datadesc` 正文）与 `<sourceinfo>`（`displayname` 发言人）、`<delivertime>`。

模块侧的规则（`native/wx_forward.cpp` + `wx_send_media.cpp`）：

- 卡片标题取容器的非标准 `title` 属性。没有时按微信自己的口径来：「群聊的聊天记录」（群）、「A与B的聊天记录」（两人）、「Alice的聊天记录」（一人）。
- 每行的 `<author id name avatar>` 决定发言人。缺的按会话从联系人补（群里补群名片），一个都没有时算自己。`avatar` 只收 http(s)。
- 内嵌 `<message id="…"/>` 表示引用同一会话已有的一条文本消息（带它的发言人、时间与 svrId）。媒体还不行，回 `forward_media_unsupported`。id 找不到回 `forward_message_not_found`。
- 限额对齐微信自己的多选上限：一张卡 ≤ 100 行、单行正文 ≤ 4000 字节、`<recordinfo>` ≤ 256 KiB。
- 一条请求里的卡片全部先建好再发第一条。某一行的内容有问题就让整个请求失败，不发半截。
- `k0.I` 在新管线上回 `(0, null)`，行随后才出现。模块记水位轮询拿 id（6 秒预算），再等 status 落定。拿不到 id 回 `send_unconfirmed`。
- `<message id="…" forward/>` 微信没有保持原样的对应物，回 `forward_unsupported`，提示放进 `<message forward>`。

## 其它入口（未接）

| 方法 | 用途 | 关键点 |
| --- | --- | --- |
| `qs5.v5.cj` / `dj(String toUser, byte[] contentXml, ...)` | AppMsg / 文件（直接给 XML） | 与 `k0.I` 同一落点（`k0.M`）；`dj` 要先有 `AppAttachInfo`，模块直接走 `k0.I` |
| `qs5.v5.ej` / `rj` / `fj(String,String,boolean,pc5.yl)` | 视频 / 语音 / 图片的转发入口 | `fj` 走 `v51.r0` 的媒体重载（类型 42 / 66），入库后 `status=5` 且不再推进 |
| `qs5.v5.nj` / `oj` / `pj(String,String,int,int,long,...)` | 文本发送（`r1` 构建器） | 视频在 `sj`–`vj` → `tj` |
| `qs5.v5.bj(Context,int,List,rn3.x0)` | 批量发图（朋友圈） | |
| `dy1.g.h()` → `s61.w0` / `s61.q1` / `s61.r1` | 文件 / AppMsg / 视频（`l51.m1` 子类 + `f468471i` 取 1/4/20） | 文件、视频改走上面的入口 |

## 可复用的事实

- `ActivityThread.currentApplication()` 拿 Context，`getClassLoader()` 拿宿主 ClassLoader。
- `gp0.j1.e()` 与 `b41.h9.e()` 都返回 `com.tencent.mm.modelbase.r1`（NetSceneQueue）。
- `ex0.k0.F0` 是 `ex0.j0` 单例，`k(String talker, long localId)` 取 `com.tencent.mm.storage.e9`（MsgInfo）。
- `v51.n1.a()` 的实现是 `((gp0.n) z2.f59772b).f268046a.f268074b.h(scene, 0)`，与群管理写操作的 Cgi 派发是同一个队列。
