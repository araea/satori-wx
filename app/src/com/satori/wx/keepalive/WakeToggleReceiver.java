package com.satori.wx.keepalive;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * 微信里那条常驻通知上的「获取 / 释放唤醒锁」按钮的落点。
 *
 * <p>通知由模块在微信进程内发布，但模块是纯 native、不加载任何 dex，进程里没有能注册
 * {@link BroadcastReceiver} 的代码；所以按钮是一个显式广播，指向知言应用的这个导出接收器。
 * 它带着当前端口、令牌与目标状态，转到回环上的 {@code POST /v1/internal/wakelock}，
 * 由模块真正切换它自己持有的 CPU / Wi-Fi 唤醒锁，并在下一次刷新时更新按钮文字。
 *
 * <p>{@code on} 是「切到哪个状态」（true 获取、false 释放），由模块按当前状态生成；重复点击
 * 幂等。接收器只认同显式组件与合法端口，不做任何其他事。
 */
public final class WakeToggleReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null) return;
        final int port = intent.getIntExtra("port", 0);
        final String token = intent.getStringExtra("token");
        final boolean on = intent.getBooleanExtra("on", false);
        if (token == null || token.isEmpty() || port < 1024 || port > 65535) return;
        final Context app = context.getApplicationContext();
        final PendingResult pending = goAsync();
        new Thread(() -> {
            String body = null;
            try {
                body = post(port, token, on);
            } catch (Exception ignored) {
                body = null;
            } finally {
                pending.finish();
            }
            final String message;
            if (body == null) message = "唤醒锁：切换失败（知言服务没有响应）";
            else if (body.contains("\"on\":true")) message = "唤醒锁：已开启";
            else message = "唤醒锁：已关闭";
            new Handler(Looper.getMainLooper()).post(() -> {
                try {
                    Toast.makeText(app, message, Toast.LENGTH_SHORT).show();
                } catch (Exception ignored) {
                    // 后台弹出 Toast 被系统拒绝时忽略：切换本身已经完成。
                }
            });
        }, "zhiyan-wake-toggle").start();
    }

    private static String post(int port, String token, boolean on) throws Exception {
        HttpURLConnection connection =
                (HttpURLConnection) new URL("http://127.0.0.1:" + port + "/v1/internal/wakelock").openConnection();
        connection.setRequestMethod("POST");
        connection.setConnectTimeout(3000);
        connection.setReadTimeout(3000);
        connection.setDoOutput(true);
        connection.setRequestProperty("Content-Type", "application/json");
        connection.setRequestProperty("Authorization", "Bearer " + token);
        byte[] payload = ("{\"on\":" + (on ? "true" : "false") + "}").getBytes(StandardCharsets.UTF_8);
        try (OutputStream out = connection.getOutputStream()) {
            out.write(payload);
        }
        if (connection.getResponseCode() != 200) return null;
        try (InputStream in = connection.getInputStream()) {
            byte[] buffer = new byte[256];
            int n = in.read(buffer);
            return n > 0 ? new String(buffer, 0, n, StandardCharsets.UTF_8) : "";
        }
    }
}
