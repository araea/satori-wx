package com.satori.wx.core;

import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;

/**
 * {@code /data/adb/modules/satori_wx/satori-wx.conf} 的解析与写出。
 *
 * <p>规则逐条对齐服务端的 {@code satori::ReadConfig}（native/server.cpp）：那边是 fail-closed，
 * 任何一处不合法——未知键、重复键、超长、非法字符——服务端就<b>不启动</b>。所以这里的
 * {@link #parse} 必须和它判得一模一样，{@link #write} 写出的每个文件都必须能被它接受；
 * {@code tests/ConfTest} 用同一组样例钉住两边。
 */
public final class Conf {
    /** 服务端读取缓冲 1025 字节，读满 1024 即判失败，所以文件至多 1023 字节。 */
    public static final int MAX_BYTES = 1023;
    /** {@code send_allow} 的缓冲 512 字节，值至多 511 个字符。 */
    public static final int MAX_ALLOW = 511;
    public static final int DEFAULT_PORT = 5601;
    public static final int MIN_PORT = 1024;
    public static final int MAX_PORT = 65535;
    public static final int MIN_TOKEN = 32;
    public static final int MAX_TOKEN = 128;

    public final int port;
    public final String token;
    public final boolean send;
    /** 发送白名单，保持文件里的顺序、去重。 */
    public final List<String> allow;

    public Conf(int port, String token, boolean send, List<String> allow) {
        this.port = port;
        this.token = token;
        this.send = send;
        this.allow = Collections.unmodifiableList(new ArrayList<>(new LinkedHashSet<>(allow)));
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
        Boolean send = null;
        String allow = null;
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
            } else if (line.startsWith("send=") && send == null) {
                String value = line.substring(5);
                if (value.equals("on")) send = true;
                else if (value.equals("off")) send = false;
                else throw new Invalid("send 只能是 on 或 off");
            } else if (line.startsWith("send_allow=") && allow == null) {
                String value = line.substring(11);
                if (value.length() > MAX_ALLOW) throw new Invalid("发送白名单超过 " + MAX_ALLOW + " 个字符");
                for (int i = 0; i < value.length(); i++) {
                    char c = value.charAt(i);
                    if (!talkerChar(c) && c != ';') throw new Invalid("发送白名单含有非法字符「" + c + "」");
                }
                allow = value;
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
        return new Conf(port == null ? DEFAULT_PORT : port, token, send != null && send, split(allow));
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

    /** 把分号分隔的白名单拆开：空段忽略（服务端按精确长度比较，空段永远不会命中）。 */
    public static List<String> split(String allow) {
        List<String> out = new ArrayList<>();
        if (allow == null) return out;
        for (String part : allow.split(";")) if (!part.isEmpty()) out.add(part);
        return out;
    }

    // ------------------------------------------------------------------ 写

    /** 写出的文件：规范格式，一定能被服务端接受；否则抛出原因（不会写出一个坏文件）。 */
    public String write() throws Invalid {
        if (port < MIN_PORT || port > MAX_PORT) throw new Invalid("端口须为 " + MIN_PORT + "–" + MAX_PORT + " 的整数");
        if (!tokenValid(token)) throw new Invalid("令牌须为 32–128 位英文字母、数字、- 或 _");
        StringBuilder list = new StringBuilder();
        for (String talker : allow) {
            if (!Talker.valid(talker)) throw new Invalid("「" + talker + "」不是有效的会话 ID");
            if (list.length() > 0) list.append(';');
            list.append(talker);
        }
        if (list.length() > MAX_ALLOW) throw new Invalid("白名单太长：合计至多 " + MAX_ALLOW + " 个字符，请移除一些会话");
        StringBuilder out = new StringBuilder();
        out.append("# 由知言应用写入；未知键或非法值会让服务端拒绝启动。\n");
        out.append("port=").append(port).append('\n');
        out.append("token=").append(token).append('\n');
        out.append("send=").append(send ? "on" : "off").append('\n');
        if (list.length() > 0) out.append("send_allow=").append(list).append('\n');
        String text = out.toString();
        if (text.getBytes(StandardCharsets.UTF_8).length > MAX_BYTES) {
            throw new Invalid("配置超过 " + MAX_BYTES + " 字节，请移除一些会话");
        }
        return text;
    }

    /** 白名单序列化后的字符数（分号计入），用于在界面上提示剩余容量。 */
    public static int allowLength(List<String> allow) {
        int length = 0;
        for (String talker : allow) length += talker.length() + (length == 0 ? 0 : 1);
        return length;
    }

    public Conf with(int port, String token, boolean send, List<String> allow) {
        return new Conf(port, token, send, allow);
    }

    /** 与另一份配置运行效果是否相同（白名单按集合比较，顺序不影响行为）。 */
    public boolean sameAs(Conf other) {
        return other != null && port == other.port && token.equals(other.token) && send == other.send
                && new LinkedHashSet<>(allow).equals(new LinkedHashSet<>(other.allow));
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

    static boolean talkerChar(char c) {
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
