# 只读账号身份适配层

本层把微信自己持久化的登录信息转成 Satori `Login` 快照，不再让 `/v1/meta` 与 READY 永远返回空。它完全运行在微信主进程内，使用微信自己的 uid 直接读取共享偏好文件，**不 hook、不写文件、不打开数据库、不联网**。

## 数据来源

| 文件 | 读取的键 | 用途 |
| --- | --- | --- |
| `shared_prefs/com.tencent.mm_preferences.xml` | `login_weixin_username` | `user.id`（wxid） |
| 同上 | `last_login_uin` | uin |
| 同上 | `isLogin` | `status`（`true` → 1，否则 0） |
| 同上 | `last_login_nick_name` | `user.nick` |
| 同上 | `last_login_alias` | `user.name` |
| 同上 | `last_login_bind_mobile`，回退 `login_user_name` | 手机号（当前不进入登录事件） |
| `shared_prefs/auth_info_key_prefs.xml` | `_auth_uin` | 主文件缺 uin 时的回退 |

`exists` 要求同时有 wxid 与 uin；只有 uin 或只有 wxid 时不构造身份，也不猜测。

## 事件与快照

`native/wx_account.{h,cpp}` 负责解析与生成事件，`native/wx_adapter.{h,cpp}` 负责状态机：

- 身份首次出现：`login-added`，分配新的 `sn`
- 同一身份的昵称 / 别名 / 在线状态变化：`login-updated`，复用 `sn`
- 身份切换（wxid 或 uin 变化）：先 `login-removed`，下一轮再 `login-added` 并分配新 `sn`
- 身份消失：`login-removed`

生成的事件交给现有 `EventBus` → `Hub::Apply`，与测试后端使用的是同一条路径；`/v1/meta`、`login.get` 与 READY 都读取同一份快照。`sn` 与事件时序都由 Hub 统一管理。

模块内每 3 秒扫描一次，`postAppSpecialize` 之后启动，随进程结束。扫描失败时保留上一个快照并打 `logcat` 警告，不清空登录。

## 安全边界

- 解析器只认识 `<map>` 下的 `string` / `int` / `long` / `boolean`，不展开外部实体，单文件上限 128 KiB、最多 160 条键值，超限即判定不可读。
- 字段有长度上限并做 UTF-8 校验：非法 UTF-8 的字段被丢弃，不污染 JSON；身份字段损坏只会让该次快照缺少身份，不会崩溃或阻塞服务线程。
- 只读打开（`O_RDONLY | O_CLOEXEC | O_NOFOLLOW`），日志不打印 token，也不打印消息内容。

## 已验证 / 未验证

已验证（`tests/account_test.cpp`、`tests/account_e2e_test.py`）：

- 真实形状的偏好 XML、`&amp;` / `&#x4e2d;` 等实体、非法 UTF-8、截断 / 未闭合 XML、空 `<map/>`
- 添加 / 更新 / 切换 / 登出 / 消失的状态机，以及 Hub 接受生成的事件并同步 meta
- 端到端：夹具偏好文件驱动真实适配层，`/v1/meta`、`login.get` 与 WebSocket READY 返回同一快照
- 在本机对**真实**微信数据目录只读运行 `build/satori-wx-account`，得到 `wxid_8zxjsghrk8vz41` / `uin=1114861342` / `nick,alias=nawyjx`；测试服务读取同一目录时 `/v1/meta` 与 `login.get` 返回该身份（root 只读验证）

未验证：

- 装入微信主进程后长稳运行尚未部署重启验收
- 微信修改偏好文件名或字段时的影响；只做尽力解析
- `status` 与真实网络状态（CONNECT / DISCONNECT）的对应关系
