# 微信消息发送路径（v0.6.0）

结论：**发送不需要 hook、不需要改写代码、不需要内嵌 DEX**。微信自己的
`NetSceneSendMsg` 构造器和网络派发器都在**可读的 DEX**里，可以用宿主 ClassLoader
反射直接调用；发送、入库、序号、加密全部由微信自己的代码完成。

## 一、为什么之前的结论是「找不到」

v0.5.0 的交接文档（`HANDOFF.md` §7）判定「高层发送 API 在编译化 `libapp.so` 里，
只到 mars 派发层，找不到入口」。这次改从**运行时调用栈里出现过的类**反推：

- 抓到的发送栈是 `StnManager.OnJniStartTask ← StnManager.startTask ← t2.v ← j1.e ← w1.b ← Handler`；
  `t2`、`j1`、`w1` 都在普通 dex 里（`classes11.dex`），说明**派发链条本身可读**，
  不可读的只是更上层的 UI/业务入口。
- 顺着 `j1`（= `com.tencent.mm.network.j1`，实现 `com.tencent.mm.network.s`）找到
  `com.tencent.mm.network.a3`：里面有 **`static boolean b(j1, m1)`**，实现就是
  `m1Var.doScene(j1Var, new y2())`——一个现成的、带回调的派发入口。
- 消息场景类 `v51.r0`（`NetSceneSendMsg`）与其 6 参构造器、`doScene`、回调
  `com.tencent.mm.network.y2` 也都在 dex 里。

所以 **高层入口不需要**：有「场景构造器 + 派发器 + 回调配齐」就够了。

## 二、真实路径（微信 8.0.78 / versionCode 671108664）

```
com.tencent.mm.network.a3.c()                  static → j1（即 com.tencent.mm.network.s 派发器）
v51.r0.<init>(String talker, String content, int type, int flags, long localId, String msgSource)
        ↑ 该构造器内部把消息插入微信自己的 message 表（状态=SENDING，日志 "new msg inserted to db, local id = "）
v51.r0.f                    long，构造器返回的本地消息 id（即 message.msgId）
v51.r0.doScene(com.tencent.mm.network.s, com.tencent.mm.modelbase.u0)  → int（<0 表示派发被拒）
com.tencent.mm.network.y2.<init>()   微信自己在 a3.b 里用的空回调（只打日志）
```

`v51.r0.doScene(s, cb)` 内部：读回 SENDING 消息 → 组装 `/cgi-bin/micromsg-bin/newsendmsg`
（cmd 522 / 237 / 1000000237）→ 交给 mars。**发送、加密、序号全在微信侧**。

参考（同一路径的其它证据）：
- `com.tencent.mm.network.a3.b(j1, m1)` 的字节码就是 `m1.doScene(j1, new y2())`。
- 高层构造器调用点 `dy1.g.k(v51.r1)`（只有在 kernel 里）里能看出「选择器 4 →
  `new v51.r0(b, d, e, f, k7.b, q)`」，与 6 参构造器参数一一对应
  （b=talker、d=正文、e=类型、f=标志、k7.b=本地 id、q=MsgSource）。
- 低层 mars 任务（`OnJniStartTask`）的 cgi 就是 `/cgi-bin/micromsg-bin/newsendmsg`。

## 三、实现（`native/wx_send.cpp`）

- 只做反射：`ActivityThread.currentApplication()` 取宿主 ClassLoader → `loadClass`
  `v51.r0` / `com.tencent.mm.network.y2` / `com.tencent.mm.network.a3` → 缓存
  `GetMethodID` / `GetFieldID`（jmethodID 跨线程稳定）。
- 调用线程 attach 到 JVM 并 `Looper.prepare()`；`m1.dispatch` 里有无 Looper 两条分支，
  prepare 之后「无参 `new Handler()`」的错误路径不会抛。
- **默认关闭**：配置 `send=on` 才进 features；`send_allow=<talker;talker>` 白名单之外一律拒绝
  （空名单＝全部拒绝）。另加 1.5s 最小间隔、每分钟 10 条上限。
- 返回的是「场景已被接受派发」（`netId >= 0`），**不是投递确认**；`message.create`
  返回的消息 id 是本地 id，形状由实现端按 Satori Message 构造，不回读数据库。
- 全程不 hook、不改 ArtMethod、不加载 dex、不写微信代码段。

## 四、验证状态

- 离线：`tests/capabilities_test.cpp` 覆盖 features 开关、配置解析、白名单/限速/无 VM 的拒绝路径；
  `./tests/run.sh` 全绿。
- 真机：**尚未验证**（需要装模块 + 重启 + 在 `send=on` 下真发一条）。发送是风控最敏感动作，
  线上必须保持默认关闭、只对白名单开放。

## 五、已知边界

- 类/方法名随微信版本变化（当前针对 8.0.78/3180）。任一步解析失败 → 该次发送报错并记录
  detail，不影响协议层与只读后端。
- 只有纯文本（`type=1`）；富文本/图片/引用等未做。
- 发送失败时微信库里可能残留一条 SENDING 消息（构造器先入库），这是微信自己的重发语义。
- 未做并发上限（依赖微信自己的场景队列），限速在实现端。
