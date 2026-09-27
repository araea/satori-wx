> v0.3.0 历史设计记录。当前版本见 [v0.6.0 协议矩阵](satori-conformance.md)、[消息发送路径](wechat-send.md) 和 [部署记录](deployment-v0.4.0.md)。

# v0.3.0 native 服务端研究记录

## 范围与入口

本轮用户选择优先 native Satori 服务端。标准 Zygisk `zygisk_module_entry` + API v4
用于普通 Android/ART 微信应用；`postAppSpecialize` 之后只启动 POSIX 网络线程。
线程不 attach JVM、不访问 ActivityThread、不加载宿主类、不申请 hidden API exemption。

ZygiskNext 的 `zygisk_next_api.h` 是另一套公开接口，有自己的 `zn_module` 导出，
面向 init-oriented 进程及已声明的 runtime。
[HyperOS Rust runtime 文档](https://github.com/LSPosed/ZygiskNext/blob/main/docs/hyos_runtime.md)
明确其不提供 ART/JNI。普通微信应用的 native 模块不应因此切换到 Hyos 回调。
旧构建生成的 `zn_modules.txt` 不再打包；`zygisk/arm64-v8a.so` 按
[标准布局](https://github.com/topjohnwu/zygisk-module-sample)装载。
这里是基于公开接口的实现，没有提取 ZygiskNext 的内部实现或发布二进制。

## 源码职责

| 文件 | 职责 |
| --- | --- |
| `native/module.cpp` | 精确进程筛选、pre 阶段读取配置、post 阶段启动线程 |
| `native/server.cpp` | 受限 HTTP、Satori RPC、RFC 6455 帧处理、鉴权、poll 事件循环 |
| `native/server.h` | 独立的配置/监听/运行接口，能在 Termux 原生进程直接测试 |
| `native/ws_crypto.h` | RFC 6455 固定握手输入的 SHA-1 + Base64，不用于其他密码学用途 |
| `native/vendor/cjson/` | cJSON 1.7.19，固定版本源码及 MIT 许可，最大 JSON 深度 16 |
| `native/wx_account.{h,cpp}` | v0.5.0：只读解析微信偏好，生成 Satori 登录事件 |
| `native/wx_adapter.{h,cpp}` | v0.5.0：每 3 秒扫描一次并做身份状态机（added/updated/removed） |
| `native/webhook.{h,cpp}` | v0.5.0：可选 WebHook 推送，独立线程 + 有界队列，仅 http |
| `native/probe.cpp`、`native/data_slot.h` | 单独构建的可选 JNI 观测实验 |

## 协议边界

`/v1/meta` 和 READY 使用同一份登录快照。v0.3.0 时没有经验证的账号，因此不构造虚假 `user.id`，
也不把主进程存活当作 ONLINE（该决定保留）。v0.5.0 起快照来自微信自己的偏好文件，
但仍只把 `isLogin` 当作上线依据，不把它当成网络层在线；详见 [wechat-account.md](wechat-account.md)。不支持的账号适配 API 返回 501；未知路由返回 404。
所有 HTTP RPC 必须鉴权，WebSocket 必须在建立后 10 秒内以 IDENTIFY 鉴权。
对同一连接重复 IDENTIFY、未鉴权 PING、非法 JSON/重复对象键，关闭连接。

请求结构和 cJSON 字符串边界都受检查；含 NUL 的输入、`\u0000`、非法 UTF-8 被拒绝。
当前对任何原始 `\u0000` 字节序列保守拒绝（包括双重转义后的字面量）。
请求 JSON 必须是对象。cJSON 前增加词法检查，拒绝前导零、缺小数位、裸控制字符。
WebSocket 不协商扩展/压缩和子协议；二进制消息返回 1003，超限返回 1009。

当前没有消息生产者；v0.5.0 加入的登录事件生产者是唯一真实事件源，且登录事件不参与回放。
之前不提供模拟 EVENT / 注入事件 API 或持久化回放的决定仍然有效；测试后端仅在测试二进制中。
非零恢复序号关闭为 4009，客户端应清空旧序号重新 IDENTIFY。
未来有真实事件源后需以单线程拥有的序列/有界环形缓冲实现广播和会话恢复，不能只接受 `sn` 后忽略。

## 生命周期与配置

配置文件只在 specialize 前读取，进入应用沙箱前即关闭相关 fd。
worker 和 socket 在 specialize 后创建，避免继承特权网络 fd 或跨 fork 线程状态。
非目标进程请求 `DLCLOSE_MODULE_LIBRARY`；目标进程在线程运行期间保留模块映射。
服务与微信进程同生命周期，当前没有通过网络停服/改配置的接口。
端口冲突、线程创建或配置错误只记 logcat，不重试占用其他端口、不阻塞主线程。

安装器只写本模块配置，保存旧 token；新 token 来自 `/dev/urandom` 的 32 字节。
仅支持 arm64，构建环境为 arm64 Termux。跨 ABI/完整 NDK 构建尚未实现。

## 验证与未验证项

`tests/run.sh` 使用同一份 `native/server.cpp` 编译独立原生进程，Python 标准库通过真实 socket
验证 HTTP 状态、鉴权、JSON 错误、大小限制、WebSocket 握手参考向量、分片、掩码、控制帧、
合包/分包、会话恢复拒绝、IDENTIFY 超时、慢连接隔离。
探针测试以假的 JNI 表验证引用计数、并发队列、原异常保留和还原竞争，并在真实 mmap 页面验证权限处理。

已验证 Termux 原生编译和协议行为；这不能代替装入微信进程后的验证。
尚未验证：设备当前 ZygiskNext 的实际加载、SELinux 下监听、微信启动稳定性、后台保活和真实微信账号/消息适配。
本轮未安装或重启设备。
