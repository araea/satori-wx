# 痕迹

模块在微信进程里，微信能看见的东西只有几类：自己的 `/proc/self`、自己的数据目录、自己建的通知渠道与唤醒锁。防检测就是把这几类收拾到「看不出装过东西」，代价是不碰功能面。

## 落在哪

微信数据目录下只有一个隐藏目录 `files/.svc`（0700，文件 0600）：开关、`state.json`、名字缓存、`key.log`、临时上传 `tmp/`、诊断 `store.log`。文件名不带模块名，目录名也不带——扫描器按 `satori` / `xposed` / `zygisk` 这类名字匹配时不会命中。

升级后第一次启动会做两件事：把旧的 `files/satori-wx/enabled` 的值搬进 `.svc/enabled`（开关状态不丢），再删掉 `files/` 下所有以 `satori-wx` 开头的条目（老目录、老日志、开发探针留下的 `-dev` / `-probe` / `-observe`）。只在 app 进程里做：preAppSpecialize 时还在 zygote 域，写不了微信的数据目录。

## 线程

线程名从 `/proc/<pid>/task/*/comm` 读得到，所以七条线程一律用 JVM 线程池的默认风格（`pool-6-thread-1` … `pool-6-thread-7`），微信自己也有大量 `pool-N-thread-M`。

## 通知与锁

通知渠道 id 是 `service_status_channel`（备用 `service_status_channel_2`），按微信自己的渠道命名风格取；显示名仍是「知言服务状态」，那是给用户看的。唤醒锁 tag 是 `svc:hold`。渠道 id 与 tag 宿主都能枚举，显示名不能改，也不能不发通知——常驻通知是功能本身。

## 不做

- 不挂 ART hook 引擎、不改 ArtMethod。这是 0.22.0 的结论：自带 LSPlant 的版本功能完全正常，人脸验证仍被判失败，而「零动作只注入」的探针能过。
- `wx_key` 对 `JNIEnv->RegisterNatives` 的指针替换是整个模块唯一一处 JNI 层改动，微信若比对函数表就能发现；没有更轻的替代（拿不到 SQLCipher 密钥就没有消息后端），保持现状。
- 回环端口与 `files/.svc/enabled` 是功能面：前者要监听才收得到请求，后者是每秒热切换开关的唯一通道。端口已经要令牌，`/healthz` 免鉴权但只回运行状态。
- 模块自己的 `.so` 怎么映射由 Zygisk Next 决定，模块不额外处理。

## 自查

```sh
su -c 'ls -la /data/data/com.tencent.mm/files/.svc'
su -c 'for p in /proc/$(pidof com.tencent.mm)/task/*/comm; do cat $p; done | sort | uniq -c | sort -rn | head'
su -c 'dumpsys notification --noredact | grep -c satori'   # 0
```

设备上还留着早期版本的 `files/satori-wx*` 时，升级后重启一次微信就会清掉。卸载模块时 `uninstall.sh` 会停掉看守并清掉 `.svc` 与旧条目；绕过模块卸载（直接删模块目录）时手动跑一次 `ctl.sh purge`，否则卸载之后模块就没有机会自己清了。
