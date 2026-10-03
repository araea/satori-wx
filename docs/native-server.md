# native 服务端

## 范围与入口

native Satori 服务端基于标准 Zygisk `zygisk_module_entry` + API v4，用于普通 Android / ART 微信应用。`postAppSpecialize` 之后只启动 POSIX 网络线程：线程不 attach JVM、不访问 ActivityThread、不加载宿主类、不申请 hidden API exemption。`zygisk/arm64-v8a.so` 按[标准布局](https://github.com/topjohnwu/zygisk-module-sample)装载。

ZygiskNext 的 `zygisk_next_api.h` 是另一套公开接口，有自己的 `zn_module` 导出，面向 init-oriented 进程及已声明的 runtime。[HyperOS Rust runtime 文档](https://github.com/LSPosed/ZygiskNext/blob/main/docs/hyos_runtime.md)明确其不提供 ART / JNI。普通微信应用的 native 模块不切到 Hyos 回调。

## 源码职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | 精确进程筛选、pre 阶段读取配置、post 阶段启动线程 |
| `native/server.cpp` | 受限 HTTP、Satori RPC、RFC 6455 帧处理、鉴权、poll 事件循环 |
| `native/server.h` | 独立的配置 / 监听 / 运行接口，能在 Termux 原生进程直接测试 |
| `native/ws_crypto.h` | RFC 6455 固定握手输入的 SHA-1 + Base64 |
| `native/vendor/cjson/` | cJSON 1.7.19，固定版本源码及 MIT 许可，最大 JSON 深度 16 |
| `native/wx_account.{h,cpp}` | 只读解析微信偏好，生成 Satori 登录事件 |
| `native/wx_adapter.{h,cpp}` | 每 3 秒扫描一次并做身份状态机（added / updated / removed） |
| `native/webhook.{h,cpp}` | 可选 WebHook 推送，独立线程 + 有界队列，仅 http |
| `native/wx_key.cpp`、`native/data_slot.h` | 密钥捕获：RegisterNatives 指针替换 |
| `native/wx_live.cpp`、`native/wx_store.cpp`、`native/wx_message.cpp`、`native/wx_events.cpp` | 只读库 → 消息 / 事件，见[消息内容](wechat-content.md)、[事件](wechat-events.md) |

## 协议边界

`/v1/meta` 与 READY 使用同一份登录快照。不构造虚假 `user.id`，也不把主进程存活当作 ONLINE。快照来自微信自己的偏好文件，但仍只把 `isLogin` 当作上线依据，不把它当成网络层在线，详见[只读账号身份](wechat-account.md)。不支持的账号适配 API 返回 501，未知路由返回 404。

所有 HTTP RPC 必须鉴权。WebSocket 必须在建立后 10 秒内以 IDENTIFY 鉴权。对同一连接重复 IDENTIFY、未鉴权 PING、非法 JSON、重复对象键，关闭连接。

请求结构和 cJSON 字符串边界都受检查。含 NUL 的输入、原始 `\u0000` 字节序列（含双重转义后的字面量）、非法 UTF-8 被拒绝。请求 JSON 必须是对象。cJSON 前增加词法检查，拒绝前导零、缺小数位、裸控制字符。WebSocket 不协商扩展 / 压缩和子协议，二进制消息返回 1003，超限返回 1009。

登录事件不参与回放，消息与其它事件由 `wx_live` 生产。事件是堆上的变长字符串，单条上限 128 KiB。

IDENTIFY 不拒绝旧序号。带来的 `sn` 若属于上一个进程、或已滚出 64 条的回放窗口，服务端照常回 READY，从当前起推送。READY 的 body 除了 `logins`、`proxy_urls`，还带 `satori_wx: {session_id, sn}`：`session_id` 是本进程的标识，客户端发现它变了就知道服务端重启过、旧序号已作废。已连接的客户端读得太慢、积压超出回放窗口时仍以 4009 关闭，重连后按上面的规则继续。

## 生命周期与配置

配置文件只在 specialize 前读取，进入应用沙箱前即关闭相关 fd。worker 与 socket 在 specialize 后创建，避免继承特权网络 fd 或跨 fork 线程状态。非目标进程请求 `DLCLOSE_MODULE_LIBRARY`，目标进程在线程运行期间保留模块映射。

服务与微信进程同生命周期。端口冲突、线程创建或配置错误只记 logcat，不重试占用其他端口，不阻塞主线程。

安装器只写本模块配置，保存旧 token，新 token 来自 `/dev/urandom` 的 32 字节。只支持 arm64，构建环境为 arm64 Termux。

## 测试

`tests/run.sh` 使用同一份 `native/server.cpp` 编译独立原生进程，Python 标准库通过真实 socket 验证 HTTP 状态、鉴权、JSON 错误、大小限制、WebSocket 握手参考向量、分片、掩码、控制帧、合包 / 分包、会话恢复、IDENTIFY 超时、慢连接隔离。后台保活的实现见[常驻通知与保活](keepalive.md)。
