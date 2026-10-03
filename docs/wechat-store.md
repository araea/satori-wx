# 微信消息后端（native）

原生实现消息接收、发送与其余业务方法，不需要 hook 引擎、不改写微信代码、不需要 Java 助手。微信 8.0.78 的消息库是加密的，进入点受控。

## 安全边界

不要用候选密钥去打开微信正在使用的活库。错误密钥会让 SQLite 把库当成损坏库，第二个连接与微信共享 WAL / `-shm`，并发下会让 mmap 失效，微信主进程在 WCDB 内部 `__memset_aarch64_nt` 触发 `SIGBUS BUS_ADRERR`。

- 密钥捕获（`native/wx_key.cpp`）只捕获 spec（`(key, page, version)`）并写入应用私有的 0600 `key.log`，不打开任何活库。
- 解密验证、参数匹配全部在离线副本上做（先 `cp` 出 `EnMicroMsg.db` / `-wal` / `-shm`，再试）。用完删掉副本。
- 只有拿到确定的正确密钥与参数后，主模块才去开一个只读连接。打开后先 `PRAGMA query_only`，只发 `SELECT`。
- 任何能写 WAL / `-shm` 或让 SQLite 进入恢复 / 回滚路径的操作都不允许出现在常驻进程里。

## 库与密钥

- `EnMicroMsg.db` 是 SQLCipher / WCDB 加密库：文件头是随机 salt，不是 `SQLite format 3`。`.li` 只索引明文表名（`message`、`appattach`、`ImgInfo2`）。
- 路径：`/data/data/com.tencent.mm/MicroMsg/<32位哈希>/EnMicroMsg.db`
- 密钥来源：`com.tencent.wcdb.core.Database.setCipherKey (J[BII)V`，参数为 `(handle, key[], page_size, cipher_version)`。EnMicroMsg 用的是 7 字节随机 key，不可从账号目录名派生。
- 参数：`sqlite3_key` 之后 `PRAGMA cipher_compatibility = 1`（SQLCipher v1 / page 1024）
- `libWCDB.so` 在 `/data/app/~~*/com.tencent.mm-*/lib/arm64/libWCDB.so`
- 密钥值属于账号机密，不写入仓库、不写日志，只在本机私有文件与内存中

## 三层结构

```
[1] 密钥捕获（RegisterNatives 边界替换一个函数指针）
        │
        ▼
[2] 只读 WCDB 客户端（dlopen libWCDB + sqlite3_key + SELECT）
        │
        ▼
[3] 轮询 / 游标 → EventBus → Satori message-created 等事件
```

### 1. 密钥捕获

微信自己调用 `RegisterNatives` 时会把 `nativeSetKey` 的 `fnPtr` 传进来。本模块只在注册的那一刻把这一项的 `fnPtr` 换成透传包装：记录一次密钥，然后原样转发给微信的实现。

- 不改微信代码段、不写 `mprotect`、无 trampoline、不碰 ArtMethod、不做 inline hook。
- 包装函数行为与微信原实现一致，只是多复制一份密钥。捕获一次后不再记录。
- 密钥只留在内存，不写盘、不进日志、不出现在任何 HTTP 响应里。

### 2. 只读客户端：`native/wcdb.cpp`

- `dlopen("libWCDB.so")` 拿到微信已加载的库，`dlsym` 出 SQLCipher API。
- `sqlite3_open_v2` 打开 `EnMicroMsg.db`，`sqlite3_key` 传捕获到的密钥。
- 只发 `SELECT`，不碰微信自己的连接、页缓存和写路径。
- `tests/wcdb_test.cpp` 覆盖明文库读写与错误处理。

### 3. 事件与业务方法

水位与游标的当前做法见[消息内容](wechat-content.md)与[事件](wechat-events.md)。

- 接收：按 `message.rowid` / `createTime` 维护水位，轮询新行（或观察 `-wal` mtime），转成 `message-created` 投递到 native 总线。不 hook 消息路径。
- 数据：`rcontact`（用户 / 群）、`chatroom`（群成员）、`message` 等表都能从同一只读连接读取，用于 `user.get`、`friend.list`、`guild.*`、`channel.*`、`message.list/get`。
- 群 ID 直接用 `<数字>@chatroom`，作为 `guild_id` / `channel_id`。

## 发送

发送走反射微信自己的发送链路：`com.tencent.mm.network.a3.c()` 取网络派发器，`new v51.r0(talker, content, 1, 0, 0, "")` 构造器自己入库，`scene.doScene(dispatcher, new com.tencent.mm.network.y2())` 交给微信 mars。宿主 ClassLoader 加载 App 类，`native FindClass` 会 method-missing。

实现端没有开关、不限速，返回「已派发」而非投递确认。路径见[微信消息发送路径](wechat-send.md)，各媒体类型见[发送各类消息](wechat-send-types.md)。
