# 知言 satori-wx —— 交接文档（截至 v0.5.0 实验）

> 给下一个对话/会话的完整上下文。仓库：`/data/data/com.termux/files/home/dev/araea/satori-wx`
> 先读这份，再读 `README.md`、`docs/wechat-store.md`、`docs/wechat-account.md`、`docs/satori-conformance.md`。

## 1. 这是什么

微信 `com.tencent.mm` 的 Zygisk 原生模块，实现 **Satori v1 协议标准**，供本机 Koishi 等客户端连接。
纯 native C++，无 DEX/Java 助手/ArtMethod 改写/hook 引擎（发送部分仍在攻克）。

## 2. 当前进度总览

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 账号身份 | ✅ 真机验证 | 只读微信 SharedPreferences → 真实 `logins` |
| 消息接收（事件） | ✅ 真机验证 | `EnMicroMsg.db` 只读 → `message-created` 事件 |
| 消息历史 | ✅ 真机验证 | `message.get` / `message.list`（rowid 游标分页） |
| 联系人/群/频道 | ✅ 真机验证 | `user.get` / `friend.list` / `guild.get,list` / `channel.get,list` |
| 协议可选 WebHook | ✅ | `/v1/meta/webhook.create|delete`，`Satori-Opcode` 推送 |
| 协议服务端 | ✅ | HTTP RPC、WebSocket、鉴权、37 方法目录、事件回放 |
| **消息发送** | 🔶 进行中 | 链路已摸到 `NetSceneSendMsg`；高层 API 待定位 |
| guild.member.* / guild.role.* | ⬜ 未做 | 需要 `chatroom` 表成员/角色 |

**已实现并写入 `login.features` 的方法（8 个）**：
```
message.get, message.list,
user.get, friend.list,
guild.get, guild.list,
channel.get, channel.list
```
（`wx_backend.cpp` 的 `kFeatures` 与 `wx_account.cpp` 的 `kFeatures` 必须保持一致。）

## 3. 运行/构建环境

- 设备：Android 16 / API 36 / arm64-v8a，KernelSU 4.2.0-rc2，Zygisk Next 1.5.0
- 构建：arm64 Termux，`clang` / `python3` / `readelf` / `patchelf` / `zip`；不需要 JDK/SDK/D8
- 微信版本：8.0.78_3180（`versionCode 671108664`）
- 当前安装模块：`/data/adb/modules/satori_wx`（服务端）、`/data/adb/modules/satori_wx_probe`（可选探针）

### 常用命令
```sh
cd /data/data/com.termux/files/home/dev/araea/satori-wx

./build.sh          # 服务端 ZIP + satori-wx-check + satori-wx-account + satori-wx-wcdb
./build.sh probe    # 可选探针 ZIP（satori_wx_probe）
./tests/run.sh      # 22 socket + 10 协议 + account + wcdb + store + 3 webhook + 探针测试

# 部署（KernelSU，需重启生效）
su -c 'ksud module install build/satori-wx-server-v0.5.0.zip'
su -c 'nohup sh -c "sleep 20; reboot" >/dev/null 2>&1 &'
```

## 4. 关键文件职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | Zygisk 入口：进程筛选、读配置、启动服务端/账号/消息线程 |
| `native/server.cpp` | HTTP/WS 服务端、鉴权、路由、事件循环、`Run(listener,config,bus,backend)` |
| `native/protocol.cpp/.h` | cJSON 校验、37 方法表、EventBus、Hub（登录快照+事件历史回放）、`g_login_count` |
| `native/webhook.cpp/.h` | 可选 WebHook 推送（独立线程、有界队列、仅 http） |
| `native/wx_account.cpp/.h` | 只读解析微信偏好 → Satori Login（含 features） |
| `native/wx_adapter.cpp/.h` | 每 3 秒扫描身份状态机（added/updated/removed） |
| `native/wcdb.cpp/.h` | `dlopen("libWCDB.so")` + SQLCipher 只读客户端 |
| `native/wx_store.cpp/.h` | 只读 store：message/联系人查询 → Satori JSON（含互斥锁） |
| `native/wx_live.cpp/.h` | 读 probe 的 key.log，只读打开库，轮询新消息 → `message-created`；暴露 `LiveStore()` |
| `native/wx_backend.cpp/.h` | Satori Backend，实现 8 个读方法 |
| `native/probe.cpp` | 可选探针：RegisterNatives 指针替换（密钥/spec/任务栈） |
| `tools/*.py` | 离线 DEX 分析工具（见 §8） |

## 5. 微信数据层（已确认事实）

### 5.1 数据库
- 路径：`/data/data/com.tencent.mm/MicroMsg/<32位哈希>/EnMicroMsg.db`（加密 WCDB/SQLCipher）
- **密钥**：`com.tencent.wcdb.core.Database.setCipherKey (J[BII)V` 传入的 **7 字节随机 key**（不可派生，运行期捕获）
- **参数**：`sqlite3_key` 之后 `PRAGMA cipher_compatibility = 1`（SQLCipher v1 / page 1024）
- 读取方式：`native/wcdb.cpp` 的 `WcdbOpenEx(..., compatibility=1, read_only=1)`；再 `PRAGMA query_only=1`
- 表：`message`(msgId,msgSvrId,type,isSend,createTime,talker,content,…)、`rcontact`、`chatroom`

### 5.2 关键字段语义
- 群 ID：`<数字>@chatroom`（用户不可见），`channel.type=0`；私聊 `channel.type=1`
- 群消息 `content` 形如 `wxid_xxx:\n正文`（发送者前缀，需解析）
- `message.type`：`1`=文本，`10000`=系统，其余（图片 3、语音 34…）**跳过不伪造**
- `rcontact.type`：**`3`=好友**（占 489），`1`=系统号(filehelper 等)，`33`=公众号，`gh_%`=公众号；
  **群用 `username LIKE '%@chatroom'` 判定，不能用 `type&2`**（好友 type=3 也含 bit2）

### 5.3 ⚠️ 血泪教训（必须遵守）
**绝不能用候选密钥去打开微信正在使用的活库。** 曾因此导致微信主进程
`SIGBUS BUS_ADRERR`（`libWCDB` 内 `__memset_aarch64_nt`）闪退。原因：错误密钥让 SQLite 把库当损坏库，
与微信共享 WAL/`-shm`，并发下 mmap 失效。
- 只用**确定的正确密钥**（来自 `setCipherKey` spec）开只读连接
- 离线验证一律在**副本**上做（`cp EnMicroMsg.db*`）
- 探针只捕获 spec，不打开活库

## 6. 密钥捕获机制（probe / 未来主模块）

1. 目标：`RegisterNatives` 边界替换函数指针（**不改代码/ArtMethod/无 trampoline**）
2. 探针在 `ObserveRegister` 里对一份**可写副本**替换：`nativeSetKey (J[B)V`、`setCipherKey (J[BII)V`
3. ⚠️ **必须每次都替换**：微信会**多次重注册**同名 native（实测 `OnJniEncodeWxPkg` 注册 4 次），
   一次守卫会被后续注册覆盖回原函数
4. 捕获结果写 `files/satori-wx-probe/key.log`（`spec=N len=.. page=.. ver=.. hex=..`）
5. `wx_live` 读该文件，**只取带 cipher version 的 spec**（即 `setCipherKey` 的那个）打开活库

**待办**：最终版应把捕获搬进主模块内存，不再落盘 key.log（安全项）。

## 7. 发送（进行中）—— 已有结论

### 7.1 内部链路（真机 DEX 精确反查）
```
高层 API（未定位，疑在编译化 libapp.so）
   ↓
发送请求 Lv51/r1;
   ↓  Ldy1/g; k(Lv51/r1;)Lv51/m1;
NetSceneSendMsg Lv51/r0;   构造函数 (String,String,int,int,long,String)
   ↑ 构造函数自己会入库并发送（日志 "new msg inserted to db, local id = "）
MsgInfo = Lcom/tencent/mm/storage/e9;
稳定辅助 = Lcom/tencent/mm/plugin/msg/MsgIdTalker;
```

### 7.2 已排除
- `Lhy/a;` 是 `BypMsgInfoService`，不是普通文本发送
- `Lcom/tencent/mm/app/q3;` 是 `MvvmMsgDataRepo`（UI），不是发送
- `k04/h2` 是修复配置
- `SendMsgEvent` / `SendMsgSuccessEvent` 是 autogen **通知**（字段 `fm.xt`/`fm.au`），post 不会发送
- `dy1.g.k` 在 `classes*.dex` 里**没有调用者** → 高层入口在编译 dex

### 7.3 探针栈追踪结果（重要）
hook `StnManager.OnJniStartTask(Task)` 后，发送（`tasks.log` 已见
`/cgi-bin/micromsg-bin/newsendmsg`、`/cgi-bin/micromsg-bin/sendemoji`）的栈是：
```
StnManager.OnJniStartTask (Native)
StnManager.startTask
com.tencent.mm.network.t2.v
com.tencent.mm.network.j1.e
com.tencent.mm.network.w1.b
com.tencent.mm.sdk.platformtools.h8.run   ← Handler
```
**这是异步派发栈**（mars 任务队列的消费线程），**不含高层发送 API**。
另外：`Task.toString()` 只输出对象地址，**cgi 字段要反射读**（`getFieldID(taskClass,"cgi","Ljava/lang/String;")`）。
发送在 **`com.tencent.mm:push` 子进程**里执行（探针需放宽到 `com.tencent.mm*` 前缀才观察到）。

### 7.4 下一步可选的发送实现路径
| 路径 | 做法 | 备注 |
| --- | --- | --- |
| A. 反射高层 API | 仍需找到"输入 talker+content"的方法；静态在编译 dex，动态栈只到派发层 | 需要 Java 级 hook 或其它发现手段 |
| B. 直接构造 `v51.r0` 并入队 | 构造 `NetSceneSendMsg(String,String,int,int,long,String)`，再用微信场景队列入队 | 需要队列 API（可能在编译 dex）；且要自己补齐入库/序号 |
| C. mars 任务 | 反射读真实 send 任务的 `reqBuf`（protobuf `NewSendMsgRequest`），改造后经 `StnManager.startTask` 发出 | 需逆向 protobuf；特征最高 |
| D. 一次性 LSPosed 发现 | 临时挂 Java hook 记录发送调用链，再落成原生 | ⚠️ 微信可能检测 Xposed，有风控风险 |

**建议**：先做 B 的可行性验证（能否反射构造 `v51.r0` 并找到入队入口），或 D 一次性发现。

## 8. 离线 DEX 分析工具（tools/）

```sh
APK=/data/data/com.termux/files/home/tmp/satori-wx/base.apk   # 微信 base.apk（280MB）

python3 tools/dexmethods.py  $APK            # (类,方法) 对（93 万条）
python3 tools/dexmethodsig.py $APK 'Lxp3/k;' # 某类方法签名
python3 tools/dexfindstring.py $APK 'NetSceneSendMsg:MsgSource'  # 谁引用某字符串
python3 tools/dexfindclass.py  $APK 'Lv51/r0;'   # 谁 new-instance 某类
python3 tools/dexcalls.py      $APK 'Ldy1/g;' k  # 谁调用某方法（启发式）
python3 tools/dexrefs.py       $APK 'method:Lv51/r0;:<init>'     # 精确引用反查
python3 tools/dexinvokes.py    $APK 'Lcom/tencent/mm/ui/chatting/oc;' g  # 某方法调用了谁
python3 tools/dexmethodstrings.py $APK 'Lcom/tencent/mm/app/q3;' b       # 某方法引用的字符串
python3 tools/dexfields.py     $APK 'Lcom/tencent/mm/storage/e9;'        # 类的字段
```
`tools/dexlib.py` 提供精确指令解码（宽度表 + 边界），`dexrefs/dexinvokes` 依赖它。
注意：**编译化 dex（`libapp.so`）不在此列**，核心发送入口很可能在里面。

## 9. 协议层要点

- `/v1/meta`、`/v1/meta/webhook.create|delete`、`/v1/internal/status|capabilities`
- 账号 API 需 `Satori-Platform: wechat` + `Satori-User-ID: <wxid>`
- 非 login 方法：未在 features → 404；在 features 但后端没实现 → 501
- `g_login_count`（protocol.cpp）由 `Apply` 更新；`wx_live` 等它 >0 再发事件，避免事件被丢弃
- store 事件 `login.sn` 必须等于账号适配器的 sn（当前=1）

## 10. 真机验证方法

```sh
# 服务端配置（端口/token）
su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf'

# 验收日志（开机自动跑一次，见 /data/adb/satori-wx-research/verify-onboot.sh）
su -c 'cat /data/adb/satori-wx-research/postboot.log'

# 消息 store 状态
su -c 'cat /data/data/com.tencent.mm/files/satori-wx-store.log'

# 探针日志
su -c 'ls /data/data/com.tencent.mm/files/satori-wx-probe/'

# HTTP 调用示例
TOKEN=$(su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf' | sed -n 's/^token=//p')
su -c "curl -s -X POST http://127.0.0.1:5601/v1/guild.list \
  -H 'Authorization: Bearer $TOKEN' -H 'Satori-Platform: wechat' \
  -H 'Satori-User-ID: <wxid>' -H 'Content-Type: application/json' -d '{\"limit\":4}'"
```
其它：`./build/satori-wx-account <data-dir>`、`./build/satori-wx-wcdb <lib> <db> text:<key> <sql>`（配 `SATORI_WCDB_COMPAT=1`）。

## 11. 已知限制 / 安全项
1. 微信被系统冻结时轮询暂停，需要保活（前台服务/唤醒锁，参考 satori-qq）
2. probe 目前把密钥明文写 `key.log` —— 最终版搬进主模块内存
3. 微信 `:push` 子进程也在跑 mars；服务端只在主进程
4. 32 位/其它 ABI 未支持
5. 发送是风控最敏感动作：务必默认关闭 + 限速 + 白名单，不伪造成功

## 12. Git 最近提交（上下文）
```
fa9c5b3 新增 tools/dexinvokes.py
1559ec1 dexlib/dexrefs 精确引用反查
19165f6/8df8eea/f56dc10 DEX 工具集
7f7a260 message.get/list RPC
b0420b8 user/friend/guild/channel RPC
d983199 消息读取打通（store）
f004a88 store 等 hub 登录后再轮询
```

## 13. 下一位接手时的第一步
1. `./tests/run.sh` 确认全绿；`git log --oneline -10`
2. 读 `java-stack.log`（发送派发栈）、`tasks.log`（真实 cgi）
3. 选定 §7.4 的一条发送路径，先把"能反射拿到发送类/方法"验证出来
4. 发送实现后：加入 `features`、`internal/compat` 自检、限速开关
