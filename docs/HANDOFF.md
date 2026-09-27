# 知言 satori-wx —— 交接文档

> 仓库：`/data/data/com.termux/files/home/dev/araea/satori-wx`（GitHub 私有 `araea/satori-wx`，`gh` 要 `env -u GH_TOKEN`）。
> 这份是唯一入口，读完它再按需翻 `README.md`、`docs/wechat-send.md`、`docs/satori-conformance.md`、
> `docs/wechat-store.md`、`docs/wechat-account.md`。

## 1. 这是什么

微信 `com.tencent.mm`（8.0.78 / versionCode 671108664）的 Zygisk 原生模块，实现 **Satori v1 协议**，
供本机 Koishi / 知微等客户端连接。

**纯 native C++**：无 DEX、无 Java 助手、无 ArtMethod 改写、无 hook 引擎。连发送都是纯反射调用
微信自己的代码，不加载任何额外东西。

- **装机版本 v0.6.4**；**repo 版本 v0.6.5**（只差一个预热重试的小修，见 §6.4，随下次重启一起上）。
- 家账号：`wxid_8zxjsghrk8vz41`。模块配置：`send=on` + `send_allow=filehelper`。

## 2. 协议覆盖（37 个标准方法）

| | 数量 | 方法 |
| --- | --- | --- |
| ✅ 已实现、真机验证 | **15** | 读侧 13：`message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`guild.member.get/list`、`guild.role.list`、`guild.member.role.list`、`channel.get/list`、`user.channel.create`；`message.create`（可选发送）；`login.get` |
| ❌ 微信无此概念 | **8** | `message.update`（不能编辑已发消息）、`reaction.create/delete/clear/list`（没有表态）、`guild.role.create/update/delete`（没有自定义角色）——列在 `internal/capabilities.unsupported` |
| ⬜ 待逆向的写操作 | **14** | `message.delete`(撤回)、`channel.create/update/delete/mute`、`guild.member.kick/mute/role.set/role.unset`、`friend.delete`、`friend.approve`、`guild.approve`、`guild.member.approve`、`upload.create` |

读侧 + 发送可以认为做完了。`features` 的唯一来源是 `native/wx_capabilities.cpp`（`WeChatFeatures()`）；
`wx_backend.cpp` 与 `wx_account.cpp` 都读它，**不要各写一份**。

## 3. 环境 / 构建 / 部署

- 设备：Android 16 / arm64-v8a，KernelSU，Zygisk Next 1.5.0
- 构建：arm64 Termux，`clang` / `python3` / `readelf` / `patchelf` / `zip`；不需要 JDK/SDK/D8
- 模块目录：`/data/adb/modules/satori_wx`（服务端）、`/data/adb/modules/satori_wx_probe`（可选探针）

```sh
cd /data/data/com.termux/files/home/dev/araea/satori-wx
./build.sh          # 服务端 ZIP + satori-wx-check + satori-wx-account + satori-wx-wcdb
./build.sh probe    # 可选探针 ZIP
./tests/run.sh      # 22 socket + 10 协议 + account + wcdb + store + capabilities + webhook + 探针

su -c 'ksud module install build/satori-wx-server-v0.6.5.zip'   # 装机（暂存，重启才生效）
su -c 'setsid sh -c "sleep 60; /system/bin/reboot" </dev/null >/dev/null 2>&1 &'
```

**重启纪律**：构建、装机可以先行；**`reboot` 必须等用户明确确认**（重启会打断会话和用户手头的事）。
装机后没重启 = 还是旧版在跑，`module.prop` 会显示新版但行为不变，别误判。

**版本号**：`module.prop`（`version` + 独立的 `versionCode` 递增）与 `native/version.h` 两处同步。

### 配置文件 `/data/adb/modules/satori_wx/satori-wx.conf`

```ini
port=5601
token=<32-128 位字母数字-_>
send=off            # 默认关闭；on 才进 features 并允许发送
# send_allow=wxid_xxx;1234567890@chatroom   # 分号分隔白名单；为空＝一切目标都拒
```

未知键、非法值会让 `ReadConfig` 失败 → 服务端不启动（fail-closed）。

## 4. 文件职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | Zygisk 入口：进程筛选、读配置、注册状态提供者、起服务端/账号/发送预热线程 |
| `native/server.cpp` | HTTP/WS、鉴权、路由、事件循环、`StatusProvider` 钩子 |
| `native/protocol.cpp/.h` | cJSON 校验、37 方法表、EventBus、Hub（登录快照 + 事件回放）、`g_login_count` |
| `native/webhook.cpp/.h` | 可选 WebHook（独立线程、有界队列、仅 http） |
| `native/wx_account.cpp/.h` | 只读解析微信偏好 → Satori Login（含 features） |
| `native/wx_adapter.cpp/.h` | 每 3 秒扫描身份状态机（added/updated/removed） |
| `native/wcdb.cpp/.h` | `dlopen("libWCDB.so")` + SQLCipher 只读客户端 |
| `native/wx_store.cpp/.h` | 只读 store：message / rcontact / chatroom → Satori JSON |
| `native/wx_live.cpp/.h` | 读 probe 的 key.log，只读打开库，轮询新消息 → `message-created` |
| `native/wx_backend.cpp/.h` | Backend：13 个读方法 + `message.create`（`send=on` 时） |
| `native/wx_capabilities.cpp/.h` | 唯一 features 列表 + `unsupported` 列表 |
| `native/wx_send.cpp/.h` | 反射发送器 + 状态/计数快照 |
| `native/probe.cpp` | 可选探针：RegisterNatives 指针替换（密钥 / spec / mars 任务栈） |
| `tools/*.py` | 离线 DEX 分析工具（见 §9） |
| `tools/verify-onboot.sh` | 一次性开机自检脚本（见 §8） |

## 5. 微信数据层

### 5.1 库与密钥

- 路径：`/data/data/com.tencent.mm/MicroMsg/<32位哈希>/EnMicroMsg.db`（本机哈希 `aef94886e37998da5c4325f13c5caa62`）
- 密钥：`com.tencent.wcdb.core.Database.setCipherKey (J[BII)V` 的 **7 字节随机 key**，运行期捕获，不可派生
- 参数：`PRAGMA cipher_compatibility = 1`（SQLCipher v1 / page 1024）；打开后 `PRAGMA query_only=1`
- `libWCDB.so` 在 `/data/app/~~*/com.tencent.mm-*/lib/arm64/libWCDB.so`

### 5.2 ⚠️ 血泪教训（必须遵守）

**绝不能用候选密钥打开微信正在用的活库。** 曾致微信主进程 `SIGBUS BUS_ADRERR`（`libWCDB`
内 `__memset_aarch64_nt`）闪退：错误密钥让 SQLite 把库当损坏库，与微信共享 WAL/`-shm`，并发下 mmap 失效。

- 只用**确定的正确密钥**（来自 `setCipherKey` spec）开只读连接
- **离线验证一律在副本上做**：`cp EnMicroMsg.db EnMicroMsg.db-wal EnMicroMsg.db-shm` 到 `/data/local/tmp/`
- 探针只捕获 spec，不打开活库；用完删掉副本

### 5.3 已经确认的字段语义

- 群 ID = `<数字>@chatroom`（用户不可见）；`channel.type`：群 0、私聊 1
- 群消息 `content` = `wxid_xxx:\n正文`（发送者前缀需解析）
- `message.type`：`1` 文本、`10000` 系统，其余（3 图片、34 语音…）**跳过不伪造**
- `rcontact.type`：`3`=好友，`1`=系统号，`33`/`gh_%`=公众号；**群用 `username LIKE '%@chatroom'` 判定**
- `chatroom` 表（v0.6.4 起用于群成员/角色）：
  - `memberlist` = `wxid1;wxid2;…`（分号分隔）
  - `displayname` = 群内昵称，用 **U+3001 `、`（UTF-8 `E38081`）** 分隔，与 memberlist **同序**
  - `roomowner` = 群主 wxid；`memberCount` = 人数
  - 数量对不上时忽略群昵称、回落到 rcontact（已在 `wx_store.cpp` 处理）
  - ⚠️ 微信**没有**自定义角色，角色只有合成的 `owner`(群主) / `member`(成员)

## 6. 发送（已打通）

### 6.1 路径（不需要 hook、不需要 dex、不需要碰 `libapp.so`）

```
派发器 = com.tencent.mm.modelbase.r1.y.k()      ← 主进程的远端派发器
         或 com.tencent.mm.network.a3.c()       ← 兜底；只在 :push 进程有效
场景   = new v51.r0(talker, content, 1, 0, 0, "")   ← 构造器自己把消息写进微信库（SENDING）
结果   = scene.doScene(派发器, new com.tencent.mm.network.y2())   ← 返回 netId，>=0 即已交给微信
```

`v51.r0.f`(J) 是本地消息 id。加密、序号、重发全是微信自己的代码。
高层入口不用找——`com.tencent.mm.network.a3.b(j1,m1)` 的实现就是 `m1.doScene(j1, new y2())`。
完整逆向过程见 `docs/wechat-send.md`。

### 6.2 派发器分进程（v0.6.0 真机踩过）

mars 在 `com.tencent.mm:push`；主进程（服务端所在）拿不到 `a3.c()` 的 `j1`，必须用 `r1.y.k()`。
`wx_send.cpp` 的顺序是 `r1.y.k()` 优先、`a3.c()` 兜底，失败时日志打印解析掩码 `probe=0x..`。

### 6.3 纪律

- **派发器要在构造场景之前拿到**：构造器会写库，拿不到派发器时不该先落一条 SENDING 行
- 默认 `send=off`；`send_allow` 白名单外一律拒绝；1.5s 最小间隔 + 每分钟 10 条
- 成功 = 「已交给微信派发」，**不是投递确认**；不伪造成功
- 微信 `doScene` 会把库里所有待发（SENDING）消息一起派发，所以历史遗留的孤儿行会跟着出去

### 6.4 诊断接口

`internal/status` 与 `internal/capabilities` 都有 `send` 块：

```json
"send": { "enabled":true, "ready":true, "resolved":true, "dispatcher":true,
          "allowed_any":true, "allow":"filehelper",
          "sent":1, "failed":0, "rejected":0,
          "last_age_ms":24, "last_ok":true, "last_target":"filehelper",
          "last_net_id":0, "last_local_id":3292 }
```

- 计数：`sent` 已派发 / `failed` 到了管线但失败 / `rejected` 派发前被策略拒绝
- 语义：`ready` = JavaVM 已接上；`resolved` = 类已解析；`dispatcher` = 上次探测派发器可达
- `resolved`/`dispatcher` 由登录后的 `WarmSend` 线程主动探测（不发消息）维护
  - **v0.6.5 的修**：循环要「解析成功**且**派发器可达」才退出；v0.6.4 只探一次，所以刚开机
    `dispatcher` 会是 false，直到真发一条
- `message.create` 被策略拒绝时 502 体带 `rejected: true`

## 7. 协议层要点

- 端点：`/v1/meta`、`/v1/meta/webhook.create|delete`、`/v1/internal/status|capabilities`、`/v1/{resource}.{method}`
- 账号类方法要 `Satori-Platform: wechat` + `Satori-User-ID: <wxid>`
- 非 login 方法：**不在 features → 404**；在 features 但后端没实现 → 501
- `g_login_count`（protocol.cpp）由 `Apply` 更新；`wx_live` 等它 >0 再发事件，避免事件被丢
- store 事件的 `login.sn` 必须等于账号适配器的 sn（当前 = 1）
- `server.cpp` 不知道发送的存在，只用 `StatusProvider` 钩子；测试与独立工具不注册，响应保持精简

## 8. 真机验证

```sh
# 服务端配置
su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf'

# 状态 / 能力（含 send 块与 unsupported）
T=$(su -c 'cat /data/adb/modules/satori_wx/satori-wx.conf' | sed -n 's/^token=//p')
su -c "curl -s -X POST http://127.0.0.1:5601/v1/internal/status -H 'Authorization: Bearer $T' \
  -H 'Content-Type: application/json' -d '{}'"

# 业务方法（注意 wxid 与 header）
su -c "curl -s -X POST http://127.0.0.1:5601/v1/guild.list -H 'Authorization: Bearer $T' \
  -H 'Satori-Platform: wechat' -H 'Satori-User-ID: wxid_8zxjsghrk8vz41' \
  -H 'Content-Type: application/json' -d '{\"limit\":3}'"

# 日志
su -c 'logcat -d -s SatoriWx:V "*:S" | tail -20'
su -c 'cat /data/data/com.tencent.mm/files/satori-wx-store.log'
su -c 'cat /data/adb/satori-wx-research/postboot.log'
```

文件树版本与模块版本不一致时，可以用 `/data/adb/satori-wx-research/satori-wx-check` 做冒烟。

### 一次性开机自检（可选，用 `pending` 标记 arm）

`service.sh` → `/data/adb/satori-wx-research/verify-onboot.sh`（有 `pending` 才跑）。
它会等开机、拉起微信、等真实登录，然后跑 `satori-wx-check`、抓 logcat，`send=on` 时还自动
向白名单第一个目标发一条并把结果写进 `postboot.log`（看 `SEND verdict:` 与 `done`/`failed` 标记）。
arm：`su -c ': > /data/adb/satori-wx-research/pending'`。

### 离线查微信 schema（**安全做法**）

```sh
D=/data/data/com.tencent.mm/MicroMsg/aef94886e37998da5c4325f13c5caa62
C=/data/local/tmp/dbcopy; su -c "mkdir -p $C; cp $D/EnMicroMsg.db* $C/; chmod 0644 $C/*"
KEY=$(su -c 'grep "ver=1 " /data/data/com.tencent.mm/files/satori-wx-probe/key.log | sed "s/.*hex=//"' | tr -d ' \n')
LIB=/data/app/~~*/com.tencent.mm-*/lib/arm64/libWCDB.so   # 按实际展开
su -c "SATORI_WCDB_COMPAT=1 ./build/satori-wx-wcdb <展开后的LIB> $C/EnMicroMsg.db hex:$KEY \
  'PRAGMA table_info(chatroom)'"
su -c "rm -rf $C"   # 用完删
```

## 9. 离线 DEX 工具（tools/）

```sh
APK=/data/data/com.termux/files/home/tmp/satori-wx/base.apk    # 微信 base.apk
python3 tools/dexmethodsig.py     $APK 'Lv51/r0;'                        # 某类方法签名
python3 tools/dexfields.py        $APK 'Lcom/tencent/mm/modelbase/r1;'   # 某类字段
python3 tools/dexrefs.py          $APK 'method:Lv51/r0;:<init>'          # 谁引用了它
python3 tools/dexinvokes.py       $APK 'Lcom/tencent/mm/network/a3;' b   # 它调用了谁
python3 tools/dexfindclass.py     $APK 'Lgp0/y;'                          # 谁 new 了它
python3 tools/dexfindstring.py    $APK 'newsendmsg'                       # 谁引用某字符串
python3 tools/dexmethodstrings.py $APK 'Lcom/tencent/mm/app/q3;' b
```

`tools/dexlib.py` 是精确指令解码器。**注意**：`libapp.so` 里的编译化 dex 这些工具看不到，
但经验和教训是——**能用可读 dex 拼出来的路径，优先别去碰它**（发送就是这么找到的）。
JADX 也可用：`~/tools/jadx/bin/jadx --single-class <点分名> -d <输出目录> base.apk`。

## 10. 已知限制 / 安全项

1. 微信被系统冻结时轮询暂停，需要保活（前台服务 / 唤醒锁）
2. probe 把密钥明文写 `key.log`；最终版应搬进主模块内存
3. 微信 `:push` 子进程有 mars；服务端只在主进程（发送靠反射，不依赖子进程）
4. 只支持 arm64
5. 发送是风控最敏感动作：默认关闭 + 限速 + 白名单，不伪造成功
6. 写操作（§2 的 14 个）**一个都没做**；每个都必须默认关闭 + 白名单，且要先证明是谁做的再动

## 11. 下一位接手时的第一步

1. `./tests/run.sh` 确认全绿；`git log --oneline -10`
2. 读 `internal/status` 的 `send` 块确认线上状态；`send_allow` 只有 `filehelper`，
   要接真实用例（自己的测试群）就加进去并重启。
3. 想继续写功能：从 §2 的 14 个写操作里挑一个，按发送的老路子做——
   **先只读地找到微信自己的接口**（离线 DEX 反查 + 必要时探针栈），再反射调用，
   再默认关闭 + 白名单 + 限速，最后真机验一条。建议顺序：先 `message.delete`（撤回，场景熟），
   再群管理（改名 → 禁言 → 踢人），好友审批与上传最后做。
4. 纪律：**每个方法真实实现后才进 `features`**；`unsupported` 只放微信真的没有的能力；
   破坏性/风控敏感动作默认关闭。

## 12. 版本与提交

- `module.prop` / `native/version.h`：当前 **v0.6.5**（装机 **v0.6.4**，差 §6.4 那条预热重试）
- 近期：`56cbac5` 预热等派发器 → `f797dcb` 读侧补齐 + unsupported → `1b34615` v0.6.4 读侧 →
  `62228ec` 状态语义+预热 → `0895aac` 状态计数 → `d92ee8f` r1.y.k() 修复 → `8e71852` 发送打通
