package com.satori.wx.core;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.concurrent.TimeUnit;

/**
 * 需要 root 的那一半：读模块状态与配置、原子地写配置、重新启动微信。
 *
 * <p>配置文件在 {@code /data/adb/modules/satori_wx/}，属主 root、权限 0600，普通应用读不到。
 * 每个动作是一次 {@code su}，脚本从标准输入喂进去（不经命令行拼接，没有引号转义问题），
 * 配置内容走 base64，所以任何字节都原样往返。所有调用都有时限；只在工作线程里调用。
 */
public final class Root {
    private Root() {}

    public static final String MODULE = "/data/adb/modules/satori_wx";
    public static final String MODULE_UPDATE = "/data/adb/modules_update/satori_wx";
    public static final String WECHAT = "com.tencent.mm";
    private static final long TIMEOUT_MS = 20000;
    private static final int MAX_OUTPUT = 64 * 1024;

    /** 一次 su 的结果。{@code granted} 为 false 表示没有 root 或被拒绝授权。 */
    public static final class Result {
        public final boolean granted;
        public final int exit;
        public final String out;

        Result(boolean granted, int exit, String out) {
            this.granted = granted;
            this.exit = exit;
            this.out = out;
        }

        public boolean ok() {
            return granted && exit == 0;
        }
    }

    public static Result run(String script) {
        Process process = null;
        try {
            process = new ProcessBuilder("su").redirectErrorStream(true).start();
            try (OutputStream in = process.getOutputStream()) {
                in.write(("echo __satori_wx_root__\n" + script + "\nexit\n").getBytes(StandardCharsets.UTF_8));
            }
            final InputStream stream = process.getInputStream();
            final ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            Thread reader = new Thread(() -> {
                byte[] chunk = new byte[4096];
                int count;
                try {
                    while ((count = stream.read(chunk)) != -1) {
                        synchronized (buffer) {
                            if (buffer.size() + count <= MAX_OUTPUT) buffer.write(chunk, 0, count);
                        }
                    }
                } catch (Exception ignored) {
                    // 进程被结束时流会断开，读到哪算哪。
                }
            }, "satori-wx-su");
            reader.start();
            if (!process.waitFor(TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                reader.join(500);
                return new Result(false, -1, "");
            }
            reader.join(1000);
            String out;
            synchronized (buffer) {
                out = new String(buffer.toByteArray(), StandardCharsets.UTF_8);
            }
            // 第一行是我们自己 echo 的暗号：拿到它才说明脚本真的在 su 里跑起来了。
            int mark = out.indexOf("__satori_wx_root__");
            if (mark < 0) return new Result(false, process.exitValue(), out);
            int start = out.indexOf('\n', mark);
            return new Result(true, process.exitValue(), start < 0 ? "" : out.substring(start + 1));
        } catch (Exception error) {
            return new Result(false, -1, "");
        } finally {
            if (process != null) process.destroy();
        }
    }

    // ------------------------------------------------------------------ 读

    /** 模块、配置与微信进程的一次快照。 */
    public static final class Device {
        public final boolean granted;
        public final boolean module;
        public final boolean disabled;
        public final boolean removing;
        public final boolean updatePending;
        public final String version;
        public final String updateVersion;
        /** 配置文件原文；文件不存在时为 null。 */
        public final String confText;
        /** 微信主进程 pid；0 = 没在运行。 */
        public final int wechatPid;
        /** 微信主进程被系统冻结（cgroup freezer）：进程在，但服务线程不会应答。 */
        public final boolean wechatFrozen;
        /** 微信自己记着一个已登录的账号（auth_info_key_prefs 的 _auth_uin 非零）。 */
        public final boolean wechatAuthed;

        Device(boolean granted, boolean module, boolean disabled, boolean removing, boolean updatePending,
               String version, String updateVersion, String confText, int wechatPid, boolean wechatFrozen,
               boolean wechatAuthed) {
            this.granted = granted;
            this.module = module;
            this.disabled = disabled;
            this.removing = removing;
            this.updatePending = updatePending;
            this.version = version;
            this.updateVersion = updateVersion;
            this.confText = confText;
            this.wechatPid = wechatPid;
            this.wechatFrozen = wechatFrozen;
            this.wechatAuthed = wechatAuthed;
        }

        public static final Device DENIED = new Device(false, false, false, false, false, "", "", null, 0, false, false);
    }

    /** 探测脚本；目录可换，只为让 tests/RootScriptTest 在临时目录上真跑一遍。 */
    public static String probeScript(String module, String update) {
        return String.join("\n",
                "M=" + module,
                "U=" + update,
                "[ -d \"$M\" ] && echo module=1",
                "[ -f \"$M/disable\" ] && echo disabled=1",
                "[ -f \"$M/remove\" ] && echo removing=1",
                "{ [ -d \"$U\" ] || [ -f \"$M/update\" ]; } && echo update=1",
                "[ -f \"$M/module.prop\" ] && sed -n 's/^version=/version=/p' \"$M/module.prop\" | head -n 1",
                "[ -f \"$U/module.prop\" ] && sed -n 's/^version=/update_version=/p' \"$U/module.prop\" | head -n 1",
                "[ -f \"$M/satori-wx.conf\" ] && echo \"conf=$(base64 -w 0 < \"$M/satori-wx.conf\")\"",
                "pid=$(pidof " + WECHAT + " | cut -d ' ' -f 1)",
                "echo \"pid=$pid\"",
                // ColorOS 的 Hans 等机制会用 cgroup freezer 冻住后台微信：进程还在，服务线程却停着。
                "if [ -n \"$pid\" ]; then",
                "  cg=$(sed -n 's/^0:://p' /proc/$pid/cgroup 2>/dev/null)",
                "  if [ \"$(sed -n 's/^frozen //p' /sys/fs/cgroup$cg/cgroup.events 2>/dev/null)\" = 1 ]"
                        + " || grep -q freezer /proc/$pid/wchan 2>/dev/null; then echo frozen=1; fi",
                "fi",
                // 微信自己的鉴权记录：非零 uin 表示微信里有已登录的账号（用来区分「没登录」与「服务没认出」）。
                "sed -n 's/.*name=\"_auth_uin\" value=\"\\(-\\{0,1\\}[0-9]*\\)\".*/auth=\\1/p' /data/data/" + WECHAT
                        + "/shared_prefs/auth_info_key_prefs.xml 2>/dev/null | head -n 1",
                "true");
    }

    public static Device probe() {
        Result result = run(probeScript(MODULE, MODULE_UPDATE));
        if (!result.granted) return Device.DENIED;
        return parse(result.out);
    }

    /** 解析探测脚本的输出；公开只为在 JVM 上测（tests/StatusTest）。 */
    public static Device parse(String out) {
        boolean module = false, disabled = false, removing = false, update = false, frozen = false, authed = false;
        String version = "", updateVersion = "", conf = null;
        int pid = 0;
        for (String line : out.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String key = line.substring(0, eq);
            String value = line.substring(eq + 1).trim();
            switch (key) {
                case "module": module = true; break;
                case "disabled": disabled = true; break;
                case "removing": removing = true; break;
                case "update": update = true; break;
                case "frozen": frozen = true; break;
                case "auth": authed = !value.isEmpty() && !value.equals("0"); break;
                case "version": version = value; break;
                case "update_version": updateVersion = value; break;
                case "conf":
                    try {
                        conf = new String(Base64.getDecoder().decode(value), StandardCharsets.UTF_8);
                    } catch (IllegalArgumentException broken) {
                        conf = null;
                    }
                    break;
                case "pid":
                    try {
                        pid = value.isEmpty() ? 0 : Integer.parseInt(value);
                    } catch (NumberFormatException ignored) {
                        pid = 0;
                    }
                    break;
                default: break;
            }
        }
        return new Device(true, module, disabled, removing, update, version, updateVersion, conf, pid, frozen && pid > 0, authed);
    }

    // ------------------------------------------------------------------ 写

    /**
     * 原子写入配置：先写同目录的临时文件、设好属主 0:0、权限 0600 与模块目录的 SELinux 上下文，
     * 再 rename 覆盖，任何时刻都不会留下半个文件。模块有待生效的更新（modules_update）时一并写入，
     * 否则重启后更新包里的旧配置会把这次修改盖掉。最后读回比对。
     */
    public static Result writeConf(String text) {
        return run(writeScript(text, MODULE, MODULE_UPDATE));
    }

    /** 写配置的脚本；目录可换，理由同 {@link #probeScript}。 */
    public static String writeScript(String text, String module, String update) {
        String b64 = Base64.getEncoder().encodeToString(text.getBytes(StandardCharsets.UTF_8));
        return String.join("\n",
                "set -e",
                "umask 077",
                "wrote=0",
                "for D in " + module + " " + update + "; do",
                "  [ -d \"$D\" ] || continue",
                "  T=\"$D/.satori-wx.conf.new\"",
                "  printf '%s' '" + b64 + "' | base64 -d > \"$T\"",
                "  chown 0:0 \"$T\"",
                "  chmod 0600 \"$T\"",
                // 上下文以安装器设好的 module.prop 为准：旧配置若被 sed -i 之类改坏过，不能把错误延续下去。
                "  ctx=$(stat -c %C \"$D/module.prop\" 2>/dev/null || stat -c %C \"$D/satori-wx.conf\" 2>/dev/null || true)",
                "  [ -n \"$ctx\" ] && chcon \"$ctx\" \"$T\" 2>/dev/null || true",
                "  mv -f \"$T\" \"$D/satori-wx.conf\"",
                "  wrote=1",
                "done",
                "[ \"$wrote\" = 1 ]",
                "[ \"$(base64 -w 0 < " + module + "/satori-wx.conf)\" = '" + b64 + "' ]");
    }

    /**
     * 结束微信并重新打开：配置只在微信主进程启动时读取，这是让新设置生效的唯一办法。
     * 客户端会断开，微信回到前台后服务随之恢复。
     */
    public static Result restartWeChat() {
        return run(String.join("\n",
                "am force-stop " + WECHAT,
                "sleep 1",
                "am start --user 0 -n " + WECHAT + "/.ui.LauncherUI >/dev/null 2>&1"
                        + " || monkey -p " + WECHAT + " -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1"));
    }
}
