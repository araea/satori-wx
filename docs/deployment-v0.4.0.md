# v0.4.0 安装 / 重启验收记录

用户已授权安装上线和重启测试。设备：Android 16 / API 36 / arm64-v8a，KernelSU 35134，
Zygisk Next 1.5.0（843-5217106-release）。原安装版本 v0.2.6。

## 已完成

- 22 项网络测试、10 项协议测试、探针资源/并发/权限测试通过。
- 服务端 SO、探针 SO 构建检查通过；无 DEX / Java 引导 / 共享 STL / Termux RUNPATH。
- 真机检查器已在独立 native 服务进程验证 HTTP 鉴权、版本、meta、READY、心跳。
- 使用 `ksud module install` 安装 v0.4.0 到 `/data/adb/modules_update/satori_wx`，等待重启切换。
- 旧模块完整备份：`/data/adb/satori-wx-research/backup-v0.2.6/`。
- 配置 token 保留在模块内 `satori-wx.conf`（0600），未打印到日志或写入仓库。

## 重启验收

基线 boot_id：`9bfa007f-8067-4792-9fb6-8dc96691df14`。

- 检查器：`/data/adb/satori-wx-research/satori-wx-check`。
- 开机脚本：`/data/adb/satori-wx-research/verify-onboot.sh`。
- 单次标记：`pending → running → done/failed`；仅显式放置 pending 时执行，不常驻。
- 模块自身 service.sh 异步调用验收脚本；不改其他模块或全局开机服务。
- 等待 boot_completed，尝试启动微信主界面，再做 HTTP/WS 验收。
- 连续 40 秒心跳并检查微信 PID 未变，记录实际版本和本次 boot_id。
- 最终日志：`/data/adb/satori-wx-research/postboot.log`。

检查命令：

```sh
su -c 'cat /data/adb/satori-wx-research/postboot.log'
su -c '/data/adb/satori-wx-research/satori-wx-check /data/adb/modules/satori_wx/satori-wx.conf --soak'
```

**截至发起重启前，尚不能报告重启后 PASS。** 以新 boot_id 下日志中的最终 VERDICT 和 END exit 为准。
设备重启后如凭据存储仍锁定，需要先解锁；验收最长等待约 6 分钟后报告失败，不无限尝试。
实际验收结果不会把“协议服务在线”写成“微信账号/消息业务在线”。

## 回退

可通过 KernelSU 禁用 `satori_wx` 并按管理器要求重启。
旧模块目录备份与仓库中的 `build/satori-wx-v0.2.6.zip` 均保留；本轮没有改动微信数据。

## 重启前加载尝试

已原子替换当前模块 SO，并执行 `znctl znmod reload satori_wx`（工具报告 reload success），随后重新启动微信。
但新微信进程的 logcat 仍出现旧版 `verify: OnJniSetCallback` 日志，5601 尚未监听。
因此这里的 reload 成功不能证明标准 Zygisk 缓存已切换；仍需设备重启，以新进程返回的版本号验收。
重启前这个失败已记录在 `/data/adb/satori-wx-research/preboot-live.log`，不作为 v0.4.0 已上线的证据。

最终服务端 SO SHA-256：`df9d725705568a62faa7272c2247babd7e3ddb1a77ea0596838d662a8b3e4381`。
最终 ZIP SHA-256：`0b0b58d9c21946986a10b2f10455dd69f22a3d86d86c91bba57ff861d9aba033`。
