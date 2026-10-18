# 微信名字与版本适配

发送、撤回、图片、文件、视频、语音、引用、合并转发、群管理、拍一拍都靠反射调用微信混淆过的类。包名与类名每个版本重新排，部分服务方法名也会重新编号（`rj` → `Oj`）。模块不猜：要么用内置表，要么从安装包把这张表推导出来。

## 内置表

`native/wx_names.h` 的 `WX_NAME_LIST` 是唯一的名单：每个键一行，98 个键，8.0.78 与 8.0.79 两列。类名存点号形式，成员存名字与 JNI 描述符。发送器不写死任何混淆名；它按名字集合从新到旧逐套尝试，用第一套类与成员都在的，`send.names` 报出用了哪一套。

两个键不是混淆名：

- `ThreadLooper`：8.0.78 要 `Looper.prepare()`（`prepare`），8.0.79 不能有（`none`）。派发在调用线程有 Looper 时把服务器应答 post 给它，ours 从不运行，应答连同「发送中」一起丢。这条没有可推导的结构依据（两版的 `dispatch` 字节码形状相同），新版本沿用上一版的值。
- `MsgInfoIsSendField`：是否本人发送读字段 `field_isSend`（消息基类 `im.c8`，库类不改名），不调方法。

## 自动推导

内置表全都对不上时（微信更新），`Resolve()` 失败后调用 `DiscoverStart()`：

1. `ActivityThread.currentApplication().getPackageCodePath()` 取安装包路径，按文件大小 + mtime 算指纹；
2. `files/satori-wx/names-<指纹>.conf` 有完整缓存就直接装上（之后启动零成本）；
3. 没有就在后台线程跑 `DeriveNames()`（`native/wx_derive.cpp`），完成后写缓存并 `WxNamesInstall()`。

整轮约 11 秒、峰值内存约 70 MB（dex 缓存 56 MB 上限 + 类索引约 15 MB）。只读安装包，不执行微信代码，不需要 root 之外的东西。

## 规则

起点是代码里写死的东西，不是名字：

- 锚点：日志标签（`MicroMsg.NetSceneSendMsg`、`MicroMsg.AppMsgLogic`、`MicroMsg.ServiceManager`…）、cgi 路径（`/cgi-bin/micromsg-bin/revokemsg`、`/cgi-bin/micromsg-bin/sendpat`…）、格式化串（`[+] Run successfully: %s, funcId: %s`、`ForwardInfo{forwardFromImgId=`）。扫每个方法体里的 `const-string`，得到「哪些类加载了这个串」。
- 关系：场景的 `doScene` 声明在基类上；服务实现类挂在某个服务接口下（接口再连到注册表的 `get(Class)` 返回类型）；消息仓库被另一个类用静态字段持有；图片参数类的构造器把 `msg_raw_img_send` 写进哪个字段，那个字段就是来源标签。
- 形状：构造器与方法按参数与返回类型匹配，`L*;` 代表任意类。同形状有多个候选时，用上一版的名字选一个（类内成员名大多不变，重排的是类名）；仍不唯一就放弃这个键。

类名一律由锚点与关系推出，成员名只从同形状候选里选，不凭空造。98 个键有一个空着就 `Complete()` 为假，不安装：行为回到「对不上」，`send.state` 仍是 `incompatible`。

## 工具

- `tools/wxnames.py`：同一套规则的 Python 版，`--check` 与内置表对账，`--hints=<label>` 换用另一套做提示。
- `tools/wxnames_check.cpp`：C++ 版（模块里跑的就是它），`--check`、`--hints=`、`--against=`。
- `tests/derive_test.cpp`：坏文件、空 zip、缺 APK 的失败路径；`SATORI_WX_APK` 指向真实安装包时跑完整推导并与内置表逐键比对。

```sh
python tools/wxnames.py /path/base.apk --check
build/wxnames_check /path/base.apk --hints=8.0.78 --against=8.0.79 --check
SATORI_WX_APK=/path/base.apk ./tests/run.sh
```

## 边界

- 只认单个 base.apk 里的 `classes*.dex`（微信当前的形态）。
- 更新后的第一次发送在推导完成前会失败；之后走缓存。进程被杀时缓存可能没写，下次再来一轮。
- 推导只覆盖发送侧的名字；接收、事件、媒体读的是数据库与文件，不受版本影响。
- `tools/wxnames.py` 的 `--hints` 与 C++ 的 `--hints` 只影响同形状候选的取舍，不改变规则。
