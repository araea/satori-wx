# 微信消息发送路径

发送不需要 hook、不需要改写代码、不需要内嵌 DEX。微信自己的 `NetSceneSendMsg` 构造器和网络派发器都在可读的 DEX 里，可以用宿主 ClassLoader 反射直接调用。发送、入库、序号、加密全部由微信自己的代码完成。

## 派发器

派发器有两个来源，取决于进程：

- 主进程（服务端所在）：mars 在 `com.tencent.mm:push` 里，`a3.c()`（`j1`）在主进程是 null。主进程用的是 MMKernel 网络壳里的远端派发器：`com.tencent.mm.modelbase.r1` 的静态字段 `y` → `k()` 返回 `com.tencent.mm.network.s`（由 `com.tencent.mm.network.p3.a()` → `gp0.y.e(sVar)` 装入，日志 "setting up remote dispatcher"。`gp0.t/u.a()` 就是 `r1.y.d`）。
- `:push` 进程：`a3.c()` 返回真实的 `j1`（`f60887f[0]` 由 `a3.m(j1)` 设置）。

`native/wx_send.cpp` 的取法顺序是 `r1.y.k()` 优先，`a3.c()` 兜底，并打印解析掩码便于排障。

## 场景与派发

```
r1.y.k() 或 a3.c()                              → com.tencent.mm.network.s（派发器）
v51.r0.<init>(String talker, String content, int type, int flags, long localId, String msgSource)
        ↑ 该构造器内部把消息插入微信自己的 message 表（状态=SENDING，日志 "new msg inserted to db, local id = "）
v51.r0.f                    long，构造器返回的本地消息 id（即 message.msgId）
v51.r0.doScene(com.tencent.mm.network.s, com.tencent.mm.modelbase.u0)  → int（<0 表示派发被拒）
com.tencent.mm.network.y2.<init>()   微信自己在 a3.b 里用的空回调（只打日志）
```

`v51.r0.doScene(s, cb)` 内部：读回 SENDING 消息，组装 `/cgi-bin/micromsg-bin/newsendmsg`（cmd 522 / 237 / 1000000237），交给 mars。发送、加密、序号全在微信侧。

派发器要在构造场景之前拿到。构造器会写库，拿不到派发器时不能先落一条 SENDING 行。

同一路径的其它证据：

- `com.tencent.mm.network.a3.b(j1, m1)` 的字节码就是 `m1.doScene(j1, new y2())`。
- 高层构造器调用点 `dy1.g.k(v51.r1)`（只在 kernel 里）的第 5 分支是 `new v51.r0(b, d, e, f, k7.b, q)`，与 6 参构造器参数一一对应（b=talker、d=正文、e=类型、f=标志、k7.b=本地 id、q=MsgSource）。
- 低层 mars 任务（`OnJniStartTask`）的 cgi 就是 `/cgi-bin/micromsg-bin/newsendmsg`。

## 实现（`native/wx_send.cpp`）

- 只做反射：`ActivityThread.currentApplication()` 取宿主 ClassLoader，`loadClass` `v51.r0` / `com.tencent.mm.network.y2` / `com.tencent.mm.network.a3` / `com.tencent.mm.modelbase.r1`，缓存 `GetMethodID` / `GetFieldID`（jmethodID 跨线程稳定）。
- 调用线程 attach 到 JVM 并 `Looper.prepare()`。`m1.dispatch` 里有无 Looper 两条分支，prepare 之后「无参 `new Handler()`」的错误路径不会抛。
- 没有开关：`message.create` 始终在 features 里，不限目标、不限速、不加延迟。要停发就断开客户端。
- 返回的是「场景已被接受派发」（`netId >= 0`），不是投递确认。`message.create` 返回的消息 id 是本地 id，形状由实现端按 Satori Message 构造，不回读数据库。
- 全程不 hook、不改 ArtMethod、不加载 dex、不写微信代码段。

各媒体类型的实现见[发送各类消息](wechat-send-types.md)。

## 诊断：`send` 块

模块在 `postAppSpecialize` 注册一个状态提供者，`internal/status` 与 `internal/capabilities` 响应都会多出 `send` 对象：

```json
"send": {
  "ready": true,            // JavaVM 已交给发送器
  "resolved": true,         // 微信发送类已在宿主 ClassLoader 上解析成功
  "dispatcher": true,       // 上次探测时微信网络派发器可达
  "sent": 1, "failed": 0, "rejected": 0, "recalled": 0,
  "last_age_ms": 12345,     // 距上次尝试的毫秒；从未尝试则没有该字段
  "last_ok": true, "last_target": "filehelper",
  "last_net_id": 0, "last_local_id": 3257,
  "last_error": "..."       // 仅上次失败时有
}
```

`recalled` 是撤回场景被接受的条数（计入 `sent`）。

`resolved` / `dispatcher` 由预热线程维护：登录后它主动解析一次类并探一次派发器（不发送任何消息），所以这两个字段不需要先发一条消息才有值。解析失败时每 10 秒重试。`ready` 只表示 JavaVM 已接上，能不能发看 `resolved` + `dispatcher`。

计数含义：`sent` = 已交微信派发。`failed` = 到了发送管线但失败（含解析不到类、派发返回负值）。`rejected` = 在派发前被拒绝（请求不合法，或撤回的不是本账号的消息）。计数按进程计，重启归零。`message.create` 失败时返回的 502 体会带 `rejected: true`。

## 边界

- 类名与方法名随微信版本变化（当前针对 8.0.78 / versionCode 671108664 / 3180）。任一步解析失败时该次发送报错并记录 detail，不影响协议层与只读后端。
- 发送失败时微信库里可能残留一条 SENDING 消息（构造器先入库），这是微信自己的重发语义。微信 `doScene` 会把库里所有待发消息一起派发。
- 不做实现端限速或并发上限。请求原样交给微信自己的场景队列。

## 测试

`tests/capabilities_test.cpp` 覆盖 features、配置解析、连续发送不限速与无 VM 的拒绝路径，`./tests/run.sh` 全绿。JNI 调用链路的真机验收清单见 [HANDOFF](HANDOFF.md)。
