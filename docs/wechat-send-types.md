# 微信发送各类消息的逆向记录

文本、群内 @ 与图片能发；回复（引用）、语音、视频、文件没做。图片走的是聊天界面自己的 `rj()` 管线，见下文；转发路径那条死路也记在下面。

微信 8.0.78 / versionCode 671108664。工具见 `tools/dex*.py`（`dexmethodsig.py` / `dexinvokes.py` / `dexmethodstrings.py` 最常用；`dexfindclass.py`、`dexrefs.py` 有误报，别单独信）。**结论性事实要落到「某个类的某个字段 / 方法」上，并且能和 App 自己的调用点对上。**

## 已经落地的

| | 类 / 方法 | 说明 |
| --- | --- | --- |
| 文本 | `v51.r0.<init>(String,String,int,int,long,String)` + `doScene` | 构造器自己入库，cgi `newsendmsg` |
| 群内 @ | `v51.r0.<init>(String,String,int,int,Object,String)`，`flags=1`，`Object` 是 `HashMap{"atuserlist": "<![CDATA[wxid,wxid]]>"}` | 与聊天界面的 `com.tencent.mm.ui.g1.a`（AtSomeOneHelper）同一做法；构造器把 map 的项并进 `<msgsource>`；文本里的 `@昵称` 后接 U+2005 |
| 图片 | `ph5.n0.c(kt.d1)`（`ha0.w`）`.rj(da0.g)` | 见下文；异步，库里出现 `type=3` 的行才算发出 |
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

**异步意味着「返回了」不等于「发出了」。** `rj` 返回时消息行还没有。`message.create` 的做法：发之前记下库的水位，`rj` 之后最多 6 秒、每 40 毫秒查一次 `type=3 AND isSend=1 AND talker=<会话> AND msgId>水位` 的行（`StoreFindSentImage`）；查到才回 200 并带上真实的消息 id，查不到回 502 `image_unconfirmed`（图片可能仍会发出，说明白而不是假报成功）。等的时候占着服务线程，所以 6 秒是上限；实际管线在几百毫秒内入库。

`message.create` 的内容按 `<img>` 切成有序的文本 / 图片消息序列：

- `src` 只认两种：本模块 `upload.create` 产出的 `internal:wechat/<user>/_tmp/<name>`（5 分钟有效），和 `data:image/…;base64,…`（解码后最多 8 MiB，落进同一个临时目录）。远程 `http(s)` 明确拒绝，说明改用 `upload.create` 或 data URI：没有 HTTP 客户端，也不假装抓得到。
- 落地的文件先核魔数：JPEG、PNG、GIF、WebP 之外一律 400 `media_unsupported`。
- **所有图片先解析、核对完再开始发第一条**：任何一张坏了整条 400，不留下半条已发的消息。开始发之后失败，响应体的 `sent` 说明已经发出去几条。
- 一次最多 4 张。文本段每段最多 4000 字节（`content_too_long`）。
- `message.create` 因此把请求体上限放宽到 16 MiB，只对已验证令牌的调用者。
- 返回的每条 `Message` 里，图片的 `content` 是指向微信落盘那份缩略图的签名链接。
- GIF 按图片发，不会变成动图表情（表情走另一条路，没做）。

**这条路在真机上还没有验证过。** 写它时能核对的只有反编译出来的调用序列与签名；重启加载新 `.so` 之后，先跑 [HANDOFF](HANDOFF.md) 里的验收清单第 4 项。失败的话看 `internal/status` 的 `send.last_error`（哪个类或成员没找到）与 `send.media` 计数。

### 回复（`<quote>`）

发出去的 `<quote>` 现在被当成元素丢掉，正文照发。微信的回复是一条 `appmsg`（`<type>57</type>`，`<refermsg>` 里放被引用消息的 svrid / 发送者 / 摘要）。`qs5.v5.dj(String toUser, byte[] xml, String content, …)` 是发任意 appmsg XML 的入口，`dx0.r.v(content)` 先把它解析回结构再走 `com.tencent.mm.pluginsdk.model.app.*`；`gx0.e` 里那个 `<refermsg>` 是青少年模式的，与回复无关。要做的话：造 type 57 的 XML，找到 `qs5.v5` 的单例入口（`ph5.n0.c(…)` 要哪个接口），再在真机上核对库里出现的行是 `type=822083633`、`MsgQuote` 有配对。没做。

## 其它入口（供参考，都没接）

| 方法 | 用途 | 关键点 |
| --- | --- | --- |
| `qs5.v5.cj` / `dj(String toUser, byte[] contentXml, ...)` | AppMsg / 文件（直接给 XML） | 文件类走 app attach，attachid 要先上传 |
| `qs5.v5.ej` / `rj` / `fj(String,String,boolean,pc5.yl)` | 视频 / 语音 / 图片的转发入口 | `fj` 就是上面那条 type 42 的路 |
| `qs5.v5.nj` / `oj` / `pj(String,String,int,int,long,...)` | 视频（带时长与尺寸） | |
| `qs5.v5.bj(Context,int,List,rn3.x0)` | 批量发图（朋友圈） | |
| `dy1.g.h()` → `s61.w0` / `s61.q1` / `s61.r1` | 文件 / AppMsg / 视频（`l51.m1` 子类 + `f468471i` 取 1/4/20） | 同样要上传，没细查 |

## 其它可复用的事实

- `ActivityThread.currentApplication()` 拿 Context；`getClassLoader()` 拿宿主 ClassLoader。
- `gp0.j1.e()` 与 `b41.h9.e()` 都返回 `com.tencent.mm.modelbase.r1`（NetSceneQueue）。
- `ex0.k0.F0` 是 `ex0.j0` 单例，`k(String talker, long localId)` 取 `com.tencent.mm.storage.e9`（MsgInfo）。
- `v51.n1.a()` 的实现是 `((gp0.n) z2.f59772b).f268046a.f268074b.h(scene, 0)`，与群管理写操作的 Cgi 派发是同一个队列。
- 这些类都在可读 dex 里；**优先用可读 dex 拼路径，别去碰 `libapp.so`**。
