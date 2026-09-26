# 知言（satori-wx）

微信（`com.tencent.mm`）的 Satori v1 实现端，satori-qq「知弦」的姊妹项目。Zygisk API v4 注入，
目标是不挂 hook 引擎的干净实现。

**Why this name:** 微信内部代号 mm / 微消息；显示名「知言」沿用知微（acumen）改名前的旧名。

## 里程碑 1：探针（当前）

回答 go/no-go 问题：微信有没有一条能用 `RegisterNatives` 干净接管的 native↔Java 边界。

探针 = 纯 native Zygisk 模块（无 dex、无 APK）：

- 在 `postAppSpecialize`（微信 Application 还没跑）把 libart 全局 `JNINativeInterface`
  函数表的 `RegisterNatives` 槽位换成记录 wrapper——数据补丁，不写代码字节、无 trampoline、
  不碰 `PROT_EXEC` 页。
- 记录微信每个 native 方法注册：类、方法名、签名、函数指针、归属 .so（含文件内偏移）。
- 90 秒或 2 万次注册后 mprotect 写回原指针；失败自动降级为纯被动清单。
- 产物在 `/data/data/com.tencent.mm/files/satori-wx-probe/`（boundary/maps/meta），
  不发网络请求、不改微信行为。

### 构建与装机

```sh
./build.sh          # 产出 build/module/ 与 zip
su -c 'cp -r build/module /data/adb/modules/satori_wx_probe'
# 重启（Zygisk Next 只在开机读模块）。重启会打断 Termux 会话，先推代码。
```

回滚：`su -c 'rm -rf /data/adb/modules/satori_wx_probe'` + 重启。

### 分析

```sh
su -c 'tar -cf - /data/data/com.termux/files/home/wx-probe 2>/dev/null' # 或直接 cp 到 sdcard
su -c 'cat /data/data/com.tencent.mm/files/satori-wx-probe/*.log' > probe/
python3 tools/dexindex.py base.apk -o index.json   # base.apk 从 /data/app/…/com.tencent.mm-*/ 拷
```

boundary.log 的 `类名 + 方法名/签名 + lib+偏移` 与 index.json 交叉比对，按
「注册自 libwechatnormsg/libapp.*、签名带 byte[]/String 结构化载荷、消息流语义」挑候选，
对标知弦在 QQ 上的 `IQQNTWrapperSession$CppProxy.native_onSendSSOReply`。

### go/no-go

- **GO**：存在可接管且能转交原实现的消息流边界 → 里程碑 2 抽 `SatoriHub` 接口、
  迁移 satori-qq 的 `satori/`/`net/`/`core/` 协议栈、写 `WeChatClient`。
- **NO-GO**：边界全是碎片化小方法 → 只能自写 hook 引擎，微信会扫 `PROT_EXEC`，
  风险升级，停下来重新决策。

## 里程碑 2（规划中）

- `SatoriHub` 抽客户端接口（现在直接引用 satori-qq 的 `QQClient`）
- 迁移协议栈：`satori/`（Codec 需把 `QQClient.CT_*` 常量解耦）、`net/`、`core/`
- `WeChatClient`：微信内核适配层（反编译驱动，逐版本维护）
- 管理页与看守沿用知弦的三层设计
