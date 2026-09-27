# 微信消息后端设计（native、低特征）

目标：在**尽量少留下特征**的前提下，原生地实现消息接收/发送与其余业务方法。前提结论是：
微信 8.0.78 的消息库是加密的，且没有像 QQ-NT `IKernelMsgService` 那样稳定的接口，所以需要
一个受控的进入点，但**不需要 hook 引擎、不需要改写微信代码、不需要 Java 助手**。

## 安全边界（重要教训）

**不要用候选密钥去打开微信正在使用的活库。** 2026-09-27 的一次实验就是这样做：
捕获 spec 后立即用 libWCDB 反复 `sqlite3_open_v2` + `sqlite3_key` 打开 `EnMicroMsg.db` 试读，
结果微信主进程在 WCDB 内部 `__memset_aarch64_nt` 触发 `SIGBUS BUS_ADRERR` 崩溃（多个 mmap 页面失效）。
原因：错误密钥会让 SQLite 把库当成损坏库，而第二个连接与微信共享 WAL/`-shm`，并发下导致 mmap 失效。

修正后的原则：

- 密钥捕获（现在是主模块的 `native/wx_key.cpp`，以前是独立 probe）**只捕获 spec**（`(key, page, version)`）并写入应用私有的 0600 `key.log`，**不打开任何活库**。
- 解密验证、参数匹配全部在**离线副本**上做（先 `cp` 出 `EnMicroMsg.db`/`-wal`/`-shm`，再试）。
- 只有拿到**确定的**正确密钥与参数后，主模块才去开一个只读连接；且打开后先 `PRAGMA query_only`，
  只发 `SELECT`。
- 任何能写 WAL/`-shm` 或让 SQLite 进入恢复/回滚路径的操作都不允许出现在常驻进程里。

## 已验证的事实

- `EnMicroMsg.db` 是 SQLCipher/WCDB 加密库：文件头是随机 salt，不是 `SQLite format 3`；
  `.li` 只索引明文表名（`message`、`appattach`、`ImgInfo2`…）。
- 密钥来源：`com.tencent.wcdb.core.Database.setCipherKey (J[BII)V`，参数为 `(handle, key[], page_size, cipher_version)`。
- **真机确认**：EnMicroMsg 的密钥是 `setCipherKey` 传入的 **7 字节随机 key**（不可从账号目录名派生），
  正确参数是 `sqlite3_key` 之后 `PRAGMA cipher_compatibility = 1`（SQLCipher v1 / page 1024）。
  密钥值属于账号机密，不写入仓库、不写日志；仅在本机私有文件与内存中。
- 用微信自己的 `libWCDB.so` 在**离线副本**上已读出真实 `message` 表：群（`<数字>@chatroom`）、
  私聊（`wxid_...`）、`isSend` 区分收发、`type` 区分消息类型；`native/wcdb.cpp` 与
  `build/satori-wx-wcdb` 已能按该参数读库。

## 三层结构

```
[1] 密钥捕获（唯一进入点，只换函数指针）
        │
        ▼
[2] 只读 WCDB 客户端（dlopen libWCDB + sqlite3_key + SELECT）
        │
        ▼
[3] 轮询/游标 → 现有 EventBus → Satori message-created 等事件
```

### 1. 密钥捕获：只替换 RegisterNatives 里的一个函数指针

微信自己调用 `RegisterNatives` 时会把 `nativeSetKey` 的 `fnPtr` 传进来。我们只在
**注册的那一刻**把这一项的 `fnPtr` 换成我们的透传包装：记录一次密钥，然后**原样转发**给
微信的实现。要点：

- 不改微信代码段、不写 `mprotect`、无 trampoline、不碰 ArtMethod、不做 inline hook。
- 包装函数行为与微信原实现一致，只是多复制一份密钥；捕获一次后不再记录。
- 密钥只留在内存，不写盘、不进日志、不出现在任何 HTTP 响应里。
- 这比 JNI 表 CAS（`native/wx_key.cpp` 已有）、GOT 改写、inline hook 的特征都低。

### 2. 只读客户端：`native/wcdb.cpp`

- `dlopen("libWCDB.so")` 拿到微信已加载的库，`dlsym` 出 SQLCipher API。
- `sqlite3_open_v2` 打开 `EnMicroMsg.db`，`sqlite3_key` 传捕获到的密钥。
- 只发 `SELECT`；不碰微信自己的连接、页缓存和写路径。
- 已验证：`tests/wcdb_test.cpp`（明文库读写与错误处理）；真机上用 WeChat 的 `libWCDB.so`
  做了加密库往返：正确密钥读出、错误密钥与无密钥干净失败。

### 3. 事件与业务方法

- 接收：按 `message.rowid`/`createTime` 维护水位，轮询新行（或观察 `-wal` mtime），
  转成 `message-created` 投递到现有 native 总线。**不 hook 消息路径**，因此没有包解析、没有网络改写。
- 数据：`rcontact`（用户/群）、`chatroom`（群成员）、`message` 等表都能从同一只读连接读取，
  用于 `user.get`、`friend.list`、`guild.*`、`channel.*`、`message.list/get`。
- 群 ID 直接用 `<数字>@chatroom`，作为 `guild_id` / `channel_id`。

## 发送（v0.6.0 已实现，见 [wechat-send.md](wechat-send.md)）

发送不能只写库（写了不会发出去）。v0.6.0 的结论是走**反射微信自己的发送链路**
（宿主 ClassLoader，`native FindClass` 对 App 类会 method-missing）：

- `com.tencent.mm.network.a3.c()` 取网络派发器；
- `new v51.r0(talker, content, 1, 0, 0, "")`（NetSceneSendMsg）构造器自己入库；
- `scene.doScene(dispatcher, new com.tencent.mm.network.y2())` 交给微信 mars。

不 hook、不改代码、不加载 dex、不发原始封包。实现端默认关闭 + 限速（白名单已取消），
返回「已派发」而非投递确认。完整逆向与验证状态见 [wechat-send.md](wechat-send.md)。

## 分步计划

| 步骤 | 内容 | 验证 |
| --- | --- | --- |
| M3.1 | 在 RegisterNatives 边界替换 nativeSetKey/setCipherKey 函数指针，只把 spec 写进私有 key.log，**不打开活库** | 微信正常运行；key.log 出现全部 spec（含 page/version）；已完成，现为主模块 `native/wx_key.cpp` |
| M3.1b | **离线**用副本（.db/-wal/-shm）匹配正确密钥与参数 | 副本上能 `SELECT count(*) FROM message`，微信不受影响 |
| M3.2 | 用确定后的密钥开只读连接（或读副本）接成 `message-created` 事件 | 真机收到真实消息事件，WebHook/WS 都能看到 |
| M3.3 | 读 `rcontact`/`chatroom`，实现 user/friend/guild/channel/message.list/get | 已完成：另含 `guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`user.channel.create` |
| M3.4 | 反射微信发送 API，实现 `message.create` 等写操作 | 真机发出真实消息，并做失败回滚 |
M3.1 先在可选 probe（独立模块）里验证，真机确认可行且无副作用后，v0.6.7 已把同一个小包装
并进主模块 `native/wx_key.cpp`，可选 probe 已删除。

## 与已上线版本的关系

- v0.5.0 服务端（纯 native、无 hook）保持不变；第 1 步先在可选 probe 里做，现已并入主模块。
- 主模块引入密钥捕获后，仍应保持"只在取到密钥时启用消息层、失败即降级为空 features"，
  不让消息后端影响协议层稳定性。
