package com.satori.wx.core;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * 与 127.0.0.1 上的知言服务说话：只读的状态、元信息与联系人列表。时限与响应大小都有上界，
 * 服务卡住或回了个超大包都不会拖住界面线程之外的工作线程太久。
 *
 * <p>从不发消息、从不改服务端状态——那些是 Satori 客户端的事。
 */
public final class Api {
    public static final int UNREACHABLE = 0;
    /** 连不上时的原因（{@link Reply#failure}）：没有失败 / 端口拒绝连接 / 连上了却没人应答（进程被冻结或卡住）/ 其他。 */
    public static final int FAIL_NONE = 0;
    public static final int FAIL_REFUSED = 1;
    public static final int FAIL_TIMEOUT = 2;
    public static final int FAIL_OTHER = 3;
    private static final int MAX_BODY = 1 << 20;
    private static final int MAX_CONTACTS = 5000;

    private final int port;
    private final String token;

    public Api(int port, String token) {
        this.port = port;
        this.token = token;
    }

    /** 一次调用的结果：code 为 HTTP 状态，连不上时是 {@link #UNREACHABLE}。 */
    public static final class Reply {
        public final int code;
        public final JSONObject body;
        /** 连不上时的原因；有应答（不论状态码）时为 {@link #FAIL_NONE}。 */
        public final int failure;

        Reply(int code, JSONObject body) {
            this(code, body, FAIL_NONE);
        }

        Reply(int code, JSONObject body, int failure) {
            this.code = code;
            this.body = body;
            this.failure = failure;
        }

        public boolean ok() {
            return code == 200 && body != null;
        }
    }

    public Reply status() {
        return post("internal/status", new JSONObject(), null);
    }

    public Reply meta() {
        return post("meta", new JSONObject(), null);
    }

    /** 一个可以放进白名单的会话。 */
    public static final class Contact {
        public final String id;
        public final String name;
        /** 名字之外的辅助信息：昵称或微信号；可能为空。 */
        public final String detail;
        public final int kind;

        public Contact(String id, String name, String detail, int kind) {
            this.id = id;
            this.name = name;
            this.detail = detail;
            this.kind = kind;
        }
    }

    /** 好友与群，按服务端顺序（最近联系在前），翻页读完；任何一页失败就整体失败。 */
    public List<Contact> contacts(String self) throws Exception {
        List<Contact> out = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (String method : new String[]{"guild.list", "friend.list"}) {
            String next = null;
            do {
                JSONObject params = new JSONObject().put("limit", 200);
                if (next != null) params.put("next", next);
                Reply reply = post(method, params, self);
                if (!reply.ok()) throw new IllegalStateException(method + " → " + reply.code);
                JSONArray data = reply.body.optJSONArray("data");
                for (int i = 0; data != null && i < data.length(); i++) {
                    Contact contact = method.equals("guild.list") ? guild(data.optJSONObject(i)) : friend(data.optJSONObject(i));
                    if (contact != null && seen.add(contact.id)) out.add(contact);
                }
                String cursor = reply.body.optString("next", "");
                next = cursor.isEmpty() || cursor.equals(next) ? null : cursor;
            } while (next != null && out.size() < MAX_CONTACTS);
        }
        return out;
    }

    static Contact guild(JSONObject item) {
        if (item == null) return null;
        String id = item.optString("id", "");
        if (!Talker.valid(id)) return null;
        String name = item.optString("name", "").trim();
        return new Contact(id, name.isEmpty() ? Talker.fallbackName(id) : name, "", Talker.GROUP);
    }

    /**
     * 服务端的 {@code user.name} 是备注（没有备注时是微信号），{@code user.nick} 是昵称。
     * 微信自己优先显示备注，这里也一样；辨不出是备注还是微信号时（纯 ASCII），显示昵称更好认。
     */
    static Contact friend(JSONObject item) {
        if (item == null) return null;
        JSONObject user = item.optJSONObject("user");
        if (user == null) return null;
        String id = user.optString("id", "");
        if (!Talker.valid(id)) return null;
        String name = user.optString("name", "").trim();
        String nick = user.optString("nick", item.optString("nick", "")).trim();
        boolean remark = !name.isEmpty() && !name.matches("[A-Za-z0-9_\\-.]+");
        String title, detail;
        if (remark) {
            title = name;
            detail = nick.equals(name) ? "" : nick;
        } else if (!nick.isEmpty()) {
            title = nick;
            detail = name;
        } else {
            title = name.isEmpty() ? Talker.fallbackName(id) : name;
            detail = "";
        }
        int kind = Talker.kind(id);
        return new Contact(id, title, detail, kind == Talker.GROUP ? Talker.PERSON : kind);
    }

    // ------------------------------------------------------------------ HTTP

    Reply post(String path, JSONObject body, String self) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL("http://127.0.0.1:" + port + "/v1/" + path).openConnection();
            connection.setConnectTimeout(1500);
            connection.setReadTimeout(3000);
            connection.setInstanceFollowRedirects(false);
            connection.setUseCaches(false);
            connection.setRequestMethod("POST");
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/json");
            connection.setRequestProperty("Authorization", "Bearer " + token);
            if (self != null) {
                connection.setRequestProperty("Satori-Platform", "wechat");
                connection.setRequestProperty("Satori-User-ID", self);
            }
            byte[] payload = body.toString().getBytes(StandardCharsets.UTF_8);
            connection.setFixedLengthStreamingMode(payload.length);
            try (OutputStream out = connection.getOutputStream()) {
                out.write(payload);
            }
            int code = connection.getResponseCode();
            InputStream stream = code < 400 ? connection.getInputStream() : connection.getErrorStream();
            JSONObject json = null;
            if (stream != null) {
                try (InputStream in = stream; ByteArrayOutputStream buffer = new ByteArrayOutputStream()) {
                    byte[] chunk = new byte[8192];
                    int count;
                    while ((count = in.read(chunk)) != -1) {
                        if (buffer.size() + count > MAX_BODY) throw new IllegalStateException("response too large");
                        buffer.write(chunk, 0, count);
                    }
                    String text = new String(buffer.toByteArray(), StandardCharsets.UTF_8).trim();
                    if (text.startsWith("{")) json = new JSONObject(text);
                }
            }
            return new Reply(code, json);
        } catch (SocketTimeoutException timeout) {
            // 连得上、没人应答：微信进程被冻结或卡住时是这样（内核替它接了连接）。
            return new Reply(UNREACHABLE, null, FAIL_TIMEOUT);
        } catch (ConnectException refused) {
            // 端口没人监听：服务没起来，或微信没在运行。
            return new Reply(UNREACHABLE, null, FAIL_REFUSED);
        } catch (Exception error) {
            // 半截响应、被重置等一律算「连不上」，这一次的结论里不会出现在线。
            return new Reply(UNREACHABLE, null, FAIL_OTHER);
        } finally {
            if (connection != null) connection.disconnect();
        }
    }
}
