package com.satori.wx.core;

/**
 * 微信会话 ID（服务端叫 talker）：私聊是对方的 wxid 或微信号，群是 {@code <数字>@chatroom}，
 * 另有 {@code filehelper}（文件传输助手）等内置会话。用于校验客户端传来的 channel_id。
 */
public final class Talker {
    private Talker() {}

    public static final String FILE_HELPER = "filehelper";

    public static final int PERSON = 0;
    public static final int GROUP = 1;
    public static final int FILE = 2;

    /** 会话 ID 的字符集，且不能为空；群 ID 须是数字加 @chatroom。 */
    public static boolean valid(String id) {
        if (id == null || id.isEmpty() || id.length() > 95) return false;
        for (int i = 0; i < id.length(); i++) if (!Conf.talkerChar(id.charAt(i))) return false;
        int at = id.indexOf('@');
        if (at < 0) return true;
        if (!id.endsWith("@chatroom") || at != id.length() - 9 || at == 0) return false;
        for (int i = 0; i < at; i++) if (id.charAt(i) < '0' || id.charAt(i) > '9') return false;
        return true;
    }

    /** 手动输入时给出的具体原因；合法时返回 null。 */
    public static String problem(String id) {
        if (id == null || id.trim().isEmpty()) return "请输入会话 ID";
        String value = id.trim();
        if (value.length() > 95) return "太长了，会话 ID 至多 95 个字符";
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (!Conf.talkerChar(c)) {
                return c == ' ' ? "不能含空格" : "不能含「" + c + "」：只能用英文字母、数字与 _ - . @";
            }
        }
        if (value.indexOf('@') >= 0 && !valid(value)) return "群 ID 的格式是「数字@chatroom」";
        return null;
    }

    public static int kind(String id) {
        if (FILE_HELPER.equals(id)) return FILE;
        return id != null && id.endsWith("@chatroom") ? GROUP : PERSON;
    }

    /** 没有名字可用时的称呼。 */
    public static String fallbackName(String id) {
        if (FILE_HELPER.equals(id)) return "文件传输助手";
        return kind(id) == GROUP ? "群聊" : "联系人";
    }
}
