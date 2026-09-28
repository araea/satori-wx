package com.satori.wx.core;

import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;

/**
 * {@code /data/adb/modules/satori_wx/satori-wx.conf} 的解析与写出。
 *
 * <p>规则逐条对齐服务端的 {@code satori::ReadConfig}（native/server.cpp）：那边是 fail-closed，
 * 任何一处不合法——未知键、重复键、超长——服务端就<b>不启动</b>。所以这里的 {@link #parse}
 * 必须和它判得一模一样，{@link #write} 写出的每个文件都必须能被它接受；
 * {@code tests/ConfTest} 用同一组样例钉住两边。
 *
 * <p>发送没有开关：模块里的发送永远可用。旧文件里的 {@code send=on|off} 与 {@code send_allow} 行仍然被接受
 * （键只能出现一次，值被忽略；{@code send} 的值仍须是 on 或 off），保证升级后不用手改配置。
 * {@link #write} 仍会写一行 {@code send=on}：老版本模块没有这行就把发送当作关闭，模块还没换成新版的那段时间里，
 * 应用改端口或令牌不能顺带把发送关掉。
 */
public final class Conf {
    /** 服务端读取缓冲 1025 字节，读满 1024 即判失败，所以文件至多 1023 字节。 */
    public static final int MAX_BYTES = 1023;
    public static final int DEFAULT_PORT = 5601;
    public static final int MIN_PORT = 1024;
    public static final int MAX_PORT = 65535;
    public static final int MIN_TOKEN = 32;
    public static final int MAX_TOKEN = 128;

    public final int port;
    public final String token;

    public Conf(int port, String token) {
        this.port = port;
        this.token = token;
    }

    /** 解析失败：{@link #getMessage()} 是给人看的中文原因。 */
    public static final class Invalid extends Exception {
        public Invalid(String message) {
            super(message);
        }
    }

    // ------------------------------------------------------------------ 读

    public static Conf parse(String text) throws Invalid {
        if (text == null) throw new Invalid("配置文件不存在");
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > MAX_BYTES) throw new Invalid("配置文件超过 " + MAX_BYTES + " 字节");
        if (text.indexOf('\0') >= 0) throw new Invalid("配置文件含有空字符");
        Integer port = null;
        String token = null;
        boolean sendSeen = false, allowSeen = false;
        // strtok_r 以 \n 切分并跳过空段；行尾的 \r 去掉；空行与 # 开头的行忽略。
        for (String raw : text.split("\n", -1)) {
            String line = raw.endsWith("\r") ? raw.substring(0, raw.length() - 1) : raw;
            if (line.isEmpty() || line.charAt(0) == '#') continue;
            if (line.startsWith("token=") && token == null) {
                String value = line.substring(6);
                if (!tokenValid(value)) throw new Invalid("令牌须为 32–128 位英文字母、数字、- 或 _");
                token = value;
            } else if (line.startsWith("port=") && port == null) {
                port = parsePort(line.substring(5));
            } else if (line.startsWith("send=") && !sendSeen) {
                // 已取消的开关：值仍须是 on 或 off（写错依然是错误），但不起任何作用，和 native ReadConfig 一致。
                String value = line.substring(5);
                if (!value.equals("on") && !value.equals("off")) throw new Invalid("send 只能是 on 或 off");
                sendSeen = true;
            } else if (line.startsWith("send_allow=") && !allowSeen) {
                // 已取消的白名单：接受旧键、忽略其值，和 native ReadConfig 一致。
                allowSeen = true;
            } else {
                int eq = line.indexOf('=');
                String key = eq < 0 ? line : line.substring(0, eq);
                if (key.equals("token") || key.equals("port") || key.equals("send") || key.equals("send_allow")) {
                    throw new Invalid("「" + key + "」重复出现");
                }
                throw new Invalid("无法识别的设置「" + (key.length() > 24 ? key.substring(0, 24) + "…" : key) + "」");
            }
        }
        if (token == null) throw new Invalid("缺少令牌（token）");
        return new Conf(port == null ? DEFAULT_PORT : port, token);
    }

    private static int parsePort(String value) throws Invalid {
        int port = 0;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c < '0' || c > '9') throw new Invalid("端口须为 " + MIN_PORT + "–" + MAX_PORT + " 的整数");
            port = port * 10 + (c - '0');
            if (port > MAX_PORT) throw new Invalid("端口须为 " + MIN_PORT + "–" + MAX_PORT + " 的整数");
        }
        if (port < MIN_PORT) throw new Invalid("端口须为 " + MIN_PORT + "–" + MAX_PORT + " 的整数");
        return port;
    }

    // ------------------------------------------------------------------ 写

    /** 写出的文件：规范格式，一定能被服务端接受；否则抛出原因（不会写出一个坏文件）。 */
    public String write() throws Invalid {
        if (port < MIN_PORT || port > MAX_PORT) throw new Invalid("端口须为 " + MIN_PORT + "–" + MAX_PORT + " 的整数");
        if (!tokenValid(token)) throw new Invalid("令牌须为 32–128 位英文字母、数字、- 或 _");
        StringBuilder out = new StringBuilder();
        out.append("# 由知言应用写入；未知键或非法值会让服务端拒绝启动。\n");
        out.append("port=").append(port).append('\n');
        out.append("token=").append(token).append('\n');
        out.append("send=on\n"); // 只给还没换成新版的模块看：新版忽略它，老版没有它就不能发
        String text = out.toString();
        if (text.getBytes(StandardCharsets.UTF_8).length > MAX_BYTES) {
            throw new Invalid("配置超过 " + MAX_BYTES + " 字节");
        }
        return text;
    }

    /** 与另一份配置运行效果是否相同。 */
    public boolean sameAs(Conf other) {
        return other != null && port == other.port && token.equals(other.token);
    }

    // ------------------------------------------------------------------ 规则

    public static boolean tokenValid(String token) {
        if (token == null || token.length() < MIN_TOKEN || token.length() > MAX_TOKEN) return false;
        for (int i = 0; i < token.length(); i++) {
            char c = token.charAt(i);
            boolean ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return false;
        }
        return true;
    }

    /** 会话 ID（talker）允许的字符：英文字母、数字与 {@code _ - . @}。 */
    public static boolean talkerChar(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || c == '_' || c == '@' || c == '.' || c == '-';
    }

    /** 与安装器同款：32 字节随机数的十六进制，64 位。 */
    public static String newToken() {
        byte[] bytes = new byte[32];
        new SecureRandom().nextBytes(bytes);
        StringBuilder out = new StringBuilder(64);
        for (byte b : bytes) out.append(String.format("%02x", b & 0xFF));
        return out.toString();
    }
}
