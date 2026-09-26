package com.satori.wx;

import android.util.Log;

/**
 * 知言的日志：错误进 logcat（tag {@link #TAG}），观测内容走 {@link Observe} 落盘。
 * 详细日志默认关——它们可观测、也可能留在盘上。
 */
public final class L {
    public static final String TAG = "SatoriWx";
    private static volatile boolean verbose;
    private L() {}
    public static void configure(boolean enableVerbose) { verbose = enableVerbose; }
    public static boolean verbose() { return verbose; }
    public static void i(String m) {
        if (!verbose) return;
        Log.i(TAG, m);
    }
    public static void w(String m) {
        if (!verbose) return;
        Log.w(TAG, m);
    }
    public static void e(String m, Throwable t) {
        Log.e(TAG, m, t);
    }
}
