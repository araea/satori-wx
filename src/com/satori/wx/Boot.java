package com.satori.wx;

import com.satori.wx.xp.Xp;

/**
 * 注入后的引导入口：native 侧轮询到宿主（微信）的 Application 后调用
 * {@link #start(String, ClassLoader)}。在 native 起的引导线程里跑，可以直接做重活。
 *
 * <p>与知弦的差异：这里不启动 HTTP 服务，也不等 ContentProvider——只把 mars 层的两个
 * 换点装上，然后靠 {@link Observe} 落盘。
 */
public final class Boot {

    private static volatile boolean started;

    private Boot() {}

    /** 由 native 在内嵌 dex 加载完成、拿到宿主 classloader 之后调用。 */
    public static void start(String process, ClassLoader host) {
        if (started) return;
        started = true;
        if (host == null) {
            L.e("bootstrap without host classloader in " + process, null);
            return;
        }
        Xp.attach(host, process);
        L.i("zygisk bootstrap in " + process);
        Observe.log("boot: process=" + process);
        installHooksLoop(host);
    }

    /**
     * 反复尝试装换点，直到成功或超时。
     *
     * <p>为什么重试：装换点要求两件事同时成立——宿主的类能加载到（classloader 就绪），
     * 且微信自己已把 natives 注册进 ArtMethod（JNI_OnLoad 跑过）。任一没就绪 native 都回
     * {@code retry:…}，这里 500ms 一次，最多 90 秒。
     *
     * <p>风险是微信如果在引导线程装好之前就调了 {@code OnJniSetCallback}，这一轮就截不到，
     * 只能等它下一次 setCallback（账号切换/网络变化会重设）。装好与否都写进观测日志。
     */
    private static void installHooksLoop(ClassLoader host) {
        long deadline = android.os.SystemClock.elapsedRealtime() + 90_000;
        String status = "not-attempted";
        while (true) {
            try {
                status = Xp.nativeInstallHooks(host);
            } catch (Throwable t) {
                status = "retry:exception " + t;
                L.e("nativeInstallHooks threw", t);
            }
            if (!status.startsWith("retry")) break;
            if (android.os.SystemClock.elapsedRealtime() > deadline) break;
            try {
                Thread.sleep(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        String info;
        try {
            info = Xp.nativeHookInfo();
        } catch (Throwable t) {
            info = "unavailable";
        }
        Observe.log("install: " + status);
        Observe.log("hookinfo: " + info);
        L.i("install: " + status);
        if (!status.startsWith("ok")) {
            L.e("mars hooks not fully installed: " + status, null);
            return;
        }
        // 早钩在 natives 注册瞬间就位，但那半秒里 Java 助手还没好——被暂存的那次
        // setCallback 在这里补做包装并回注。
        try {
            Observe.log("flush: " + Xp.nativeFlushPending());
        } catch (Throwable t) {
            L.e("flush pending failed", t);
        }
        verifyLoop();
        // WCDB 全局 SQL 追踪（公开 API）：接收侧的干净路线。
        com.satori.wx.SqlTrace.install(host);
    }

    /**
     * 装好后的反扑观测：每 3 秒校验一次 data_ 槽，共两分钟；之后降频到 30 秒一次再来
     * 20 次。有翻转（YTAG 重新断言）才会写日志，稳定就不刷屏。
     */
    private static void verifyLoop() {
        Thread t = new Thread(() -> {
            String last = null;
            for (int i = 0; i < 40; i++) {
                String s = Xp.verifyHooks();
                if (!s.equals(last)) {
                    Observe.log("verify: " + s);
                    last = s;
                }
                sleep(3_000);
            }
            for (int i = 0; i < 20; i++) {
                String s = Xp.verifyHooks();
                if (!s.equals(last)) {
                    Observe.log("verify: " + s);
                    last = s;
                }
                sleep(30_000);
            }
            Observe.log("verify: watch ended, " + last);
        }, "SatoriWxVerify");
        t.setDaemon(true);
        t.start();
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
