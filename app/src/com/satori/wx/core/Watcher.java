package com.satori.wx.core;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * root 侧的变化订阅：一条常驻的 {@code su}，里面是系统自带的 {@code inotifyd}，盯着与状态有关的几处——
 * 微信的 cgroup（冻结与解冻、进程出现与消失）、模块自己发布的状态文件、保活看守的状态目录、模块目录。
 * 其中任何一处变了，就往标准输出写一行，应用据此重新检查，而不是隔几秒问一遍。
 *
 * <p>只在界面可见时开着：{@link #start()} 与 {@link #stop()} 对应前台与后台。su 起不来（没有授权）时
 * {@link Listener#ended(boolean)} 会说明，应用退回慢速的兜底刷新。
 *
 * <p>脚本每 10 秒重新列一遍要盯的文件（微信重启后会长出新的 {@code pid_*} cgroup）。结束得很干净：脚本把标准输入
 * 留着不读，应用这头一关（结束订阅、或应用进程没了），它读到 EOF 就把整个进程组收掉，不会留下孤儿。
 */
public final class Watcher {
    /** 回调都在读取线程上，只做轻量的转交。 */
    public interface Listener {
        /** 某一处变了；{@code line} 是 inotifyd 的原样一行（事件字母、路径）。 */
        void changed(String line);

        /** 订阅结束；{@code ran} 为 false 表示 su 压根没起来（没有 root 授权）。 */
        void ended(boolean ran);
    }

    public static final String MARK = "__satori_wx_watch__";
    /** 脚本自己发现盯不了（缺 inotifyd / timeout，或 inotifyd 一起就退）时的最后一行。 */
    public static final String FAILED = "__satori_wx_watch_failed__";
    private static final long RELIST_GAP_MS = 2000;

    private final Listener listener;
    private Process process;
    private String script;
    private boolean closed = true;
    private long relistedAt;

    public Watcher(Listener listener) {
        this.listener = listener;
    }

    /**
     * 订阅脚本。要盯的路径是参数，测试把它们指到临时目录；真正使用时是微信的 cgroup、状态文件目录、
     * 看守目录与模块目录。路径后的字母是 inotifyd 的事件掩码：{@code c} 内容变了、{@code m} 有文件移入（状态文件都是
     * 写临时文件再改名，只听这一种就没有杂音）、
     * {@code n} 新建、{@code d} 删除。
     */
    public static String script(String wechat, String cgroupApps, String publishedDir, String guardDir,
                                String moduleDir, String updateDir) {
        return String.join("\n",
                "exec 2>/dev/null",
                // 标准输入是应用这头的管道，留着不读：应用一关（结束订阅、应用进程没了）就读到 EOF。后台任务的标准输入
                // 默认是 /dev/null，所以先把它复制到 3。
                "exec 3<&0",
                // 少了系统自带的 inotifyd 或 timeout，就不要假装在订阅：应用退回定时检查。
                "command -v inotifyd >/dev/null && command -v timeout >/dev/null || { echo " + FAILED + "; exit 1; }",
                "echo " + MARK,
                "W=" + wechat,
                "C=" + cgroupApps,
                "me=$(sed -n 's/^PPid:[[:space:]]*//p' /proc/$$/status)",
                // 兜底：父进程变了也收工。
                "alive() { [ \"$(sed -n 's/^PPid:[[:space:]]*//p' /proc/$$/status)\" = \"$me\" ]; }",
                "watch() {",
                "  quick=0",
                "  while alive; do",
                "    uid=$(stat -c %u /data/data/$W 2>/dev/null)",
                "    set --",
                "    for p in " + moduleDir + ":ndm " + updateDir + ":ndm " + guardDir + ":m " + publishedDir + ":m; do",
                "      [ -e \"${p%:*}\" ] && set -- \"$@\" \"$p\"",
                "    done",
                "    if [ -n \"$uid\" ] && [ -d \"$C/uid_$uid\" ]; then",
                "      set -- \"$@\" \"$C/uid_$uid:nd\" \"$C/uid_$uid/cgroup.events:c\"",
                "      for f in \"$C\"/uid_$uid/pid_*/cgroup.events; do [ -r \"$f\" ] && set -- \"$@\" \"$f:c\"; done",
                "    fi",
                "    t=$SECONDS",
                "    if [ $# -gt 0 ]; then timeout 10 inotifyd - \"$@\"; else sleep 5; fi",
                // inotifyd 正常会一直跑到 timeout 才退；一起就退（被 SELinux 拦了、路径全没了）连续几次，说明
                // 订阅根本没在工作：报一声，让应用退回定时检查，不要装作在订阅。
                "    if [ $((SECONDS - t)) -lt 2 ]; then quick=$((quick + 1)); else quick=0; fi",
                "    if [ $quick -ge 5 ]; then echo " + FAILED + "; return; fi",
                "    sleep 0.3",
                "  done",
                "}",
                "watch &",
                "w=$!",
                // 读到 EOF 才往下走：把盯文件的整串进程（循环、timeout、inotifyd）收掉，不留孤儿。
                // 不能用 kill 0：进程组是和应用共用的。
                "cat <&3 >/dev/null",
                "for t in $(pgrep -P $w); do pkill -P $t; kill $t; done",
                "kill $w");
    }

    static String defaultScript() {
        return script(Root.WECHAT, "/sys/fs/cgroup/apps", "/data/user/0/" + Root.WECHAT + "/files/satori-wx",
                Root.GUARD_DIR, Root.MODULE, "/data/adb/modules_update");
    }

    /** 开始订阅；已经在订阅时什么也不做。 */
    public synchronized void start() {
        start(defaultScript());
    }

    /** 以给定的脚本订阅；测试把要盯的路径指到临时目录。 */
    public synchronized void start(String script) {
        if (process != null && process.isAlive()) return;
        closed = false;
        this.script = script;
        launch();
    }

    private synchronized void launch() {
        final Process started;
        try {
            // 脚本走 -c 参数，标准输入留空着不用：它是「应用还在」的信号。
            started = new ProcessBuilder(Root.SU[0], Root.SU[1], "-c", script).redirectErrorStream(true).start();
        } catch (Exception error) {
            listener.ended(false);
            return;
        }
        process = started;
        Thread reader = new Thread(() -> read(started), "satori-wx-watch");
        reader.setDaemon(true);
        reader.start();
    }

    private void read(Process started) {
        boolean ran = false, failed = false;
        try (BufferedReader lines = new BufferedReader(new InputStreamReader(started.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = lines.readLine()) != null) {
                if (line.contains(FAILED)) {
                    failed = true;
                    break;
                }
                if (!ran) {
                    // 暗号之前的输出（su 的提示之类）不算事件。
                    if (line.contains(MARK)) ran = true;
                    continue;
                }
                if (line.isEmpty()) continue;
                listener.changed(line);
                if (entriesChanged(line)) relist(started);
            }
        } catch (Exception ignored) {
            // 进程被结束时流会断开。
        }
        boolean current;
        synchronized (this) {
            current = process == started;
            if (current) process = null;
        }
        if (failed) started.destroy();
        if (current) listener.ended(ran && !failed);
    }

    /** cgroup 里出现或消失了子项（微信进程来了又走了）：旧的订阅盯不到新的 pid，要换一轮。 */
    public static boolean entriesChanged(String line) {
        return (line.startsWith("n\t") || line.startsWith("d\t")) && line.contains("/uid_");
    }

    private void relist(Process started) {
        long now = System.currentTimeMillis();
        synchronized (this) {
            if (closed || process != started || now - relistedAt < RELIST_GAP_MS) return;
            relistedAt = now;
            process = null;
        }
        started.destroy();
        synchronized (this) {
            // 换进程的这一小段里应用可能已经 stop() 了：那就不要再起。
            if (!closed && process == null) launch();
        }
    }

    /** 结束订阅。 */
    public synchronized void stop() {
        closed = true;
        Process current = process;
        process = null;
        if (current != null) current.destroy();
    }

    public synchronized boolean running() {
        return process != null && process.isAlive();
    }
}
