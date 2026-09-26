package com.satori.wx;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;

/**
 * 观测日志：单写线程、有界队列，写不进就丢并计数。落盘
 * {@code /data/data/com.tencent.mm/files/satori-wx-observe/observe.log}。
 *
 * <p>回调与 startTask 都在微信自己的线程上触发，记录必须快：入队 O(1)，I/O 全在写线程。
 */
public final class Observe {
    private static final String DIR = "/data/data/com.tencent.mm/files/satori-wx-observe";
    private static final String FILE = DIR + "/observe.log";
    private static final int QUEUE_MAX = 4096;

    private static final BlockingQueue<String> queue = new ArrayBlockingQueue<>(QUEUE_MAX);
    private static volatile boolean started;
    private static volatile long dropped;

    private Observe() {}

    private static void ensureStarted() {
        if (started) return;
        synchronized (Observe.class) {
            if (started) return;
            started = true;
            Thread t = new Thread(Observe::run, "SatoriWxObserve");
            t.setDaemon(true);
            t.start();
        }
    }

    /** 记一行；队列满即丢。线程安全。 */
    public static void log(String line) {
        ensureStarted();
        String ts = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US).format(new Date());
        if (!queue.offer(ts + ' ' + line)) {
            dropped++;
        }
    }

    public static long droppedCount() { return dropped; }

    private static void run() {
        File dir = new File(DIR);
        if (!dir.exists() && !dir.mkdirs()) {
            android.util.Log.e(L.TAG, "observe: cannot mkdir " + DIR);
            return;
        }
        try (OutputStream out = new FileOutputStream(FILE, true)) {
            StringBuilder sb = new StringBuilder(1 << 16);
            java.util.ArrayList<String> batch = new java.util.ArrayList<>(128);
            // 写线程常驻：进程不死它不死，无需退出条件。
            while (true) {
                batch.clear();
                batch.add(queue.take());
                queue.drainTo(batch, 128);
                for (String line : batch) {
                    sb.append(line).append('\n');
                }
                out.write(sb.toString().getBytes("UTF-8"));
                out.flush();
                sb.setLength(0);
            }
        } catch (Throwable t) {
            android.util.Log.e(L.TAG, "observe writer died", t);
        }
    }

    /** 摘要一个参数：byte[] 给长度+头部 hex，String 截断，其余 toString 截断。 */
    public static String summarize(Object v) {
        if (v == null) return "null";
        if (v instanceof byte[]) {
            byte[] b = (byte[]) v;
            StringBuilder hex = new StringBuilder(Math.min(b.length, 24) * 2);
            for (int i = 0; i < b.length && i < 24; i++) {
                hex.append(String.format(Locale.US, "%02x", b[i]));
            }
            return "bytes(" + b.length + ")[" + hex + (b.length > 24 ? "…" : "") + ']';
        }
        String s = String.valueOf(v);
        return s.length() > 300 ? s.substring(0, 300) + "…" : s;
    }
}
