package com.satori.wx.core;

import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * 把一次检查得到的事实翻译成界面上的结论：服务通了没有、断在哪一环、下一步做什么。
 *
 * <p>纯函数，只依赖 org.json，能在 JVM 上直接测（{@code tests/StatusTest}）。底线只有一条：
 * <b>拿不到数据时只说「未知」或「未连接」，绝不沿用上一次的在线结论。</b>
 */
public final class Status {
    private Status() {}

    // ---- 语调：决定容器色与图标；文字本身必须已经说清状态（WCAG 1.4.1） ----
    public static final int NEUTRAL = 0;
    public static final int SUCCESS = 1;
    public static final int WARNING = 2;
    public static final int ERROR = 3;

    // ---- 总状态，按排查顺序 ----
    public static final int CHECKING = 0;
    public static final int RESTARTING = 1;
    public static final int NO_ROOT = 2;
    public static final int MODULE_PENDING = 3;
    public static final int NO_MODULE = 4;
    public static final int MODULE_OFF = 5;
    public static final int CONFIG_BAD = 6;
    public static final int NO_WECHAT = 7;
    public static final int WECHAT_STOPPED = 8;
    public static final int WECHAT_FROZEN = 9;
    public static final int SERVICE_DOWN = 10;
    public static final int TOKEN_PENDING = 11;
    public static final int LOGGED_OUT = 12;
    public static final int ACCOUNT_UNSEEN = 13;
    public static final int READY = 14;

    // ---- 状态卡片上的下一步 ----
    public static final int ACTION_NONE = 0;
    public static final int ACTION_RETRY_ROOT = 1;
    public static final int ACTION_OPEN_WECHAT = 2;
    public static final int ACTION_RESTART_WECHAT = 3;
    public static final int ACTION_FIX_CONFIG = 4;
    public static final int ACTION_WECHAT_SETTINGS = 5;

    // ---- 链路每一环 ----
    public static final int STEP_OK = 0;
    public static final int STEP_WAIT = 1;
    public static final int STEP_FAIL = 2;
    public static final int STEP_OFF = 3;
    public static final int STEP_UNKNOWN = 4;

    /** 一次检查的全部输入。字段由宿主填，这里只读。 */
    public static final class Snapshot {
        public boolean checked;
        public boolean restarting;
        public Root.Device device;
        public Conf conf;
        public String confError;
        public boolean wechatInstalled = true;
        public String wechatVersion = "";
        /** 状态接口的 HTTP 结果；{@link Api#UNREACHABLE} 表示连不上。 */
        public int http = Api.UNREACHABLE;
        public JSONObject status;
        public JSONObject meta;
        /** 服务是用改动之前的端口 / 令牌应答的：新配置已保存、尚未生效。 */
        public boolean viaPrevious;
        public int port = Conf.DEFAULT_PORT;

        /** 整体替换成另一次检查的结果：界面只认完整的一次，绝不半新半旧。 */
        public void copyFrom(Snapshot s) {
            checked = s.checked;
            restarting = s.restarting;
            device = s.device;
            conf = s.conf;
            confError = s.confError;
            wechatInstalled = s.wechatInstalled;
            wechatVersion = s.wechatVersion;
            http = s.http;
            status = s.status;
            meta = s.meta;
            viaPrevious = s.viaPrevious;
            port = s.port;
        }
    }

    /** 一句结论：语调、标题、说明、下一步及其按钮文字。 */
    public static final class Line {
        public final int tone;
        public final String title;
        public final String detail;
        public final int action;
        public final String actionLabel;
        /** 次要的下一步（文字按钮）；没有时为 {@link #ACTION_NONE}。 */
        public final int secondary;
        public final String secondaryLabel;

        Line(int tone, String title, String detail) {
            this(tone, title, detail, ACTION_NONE, null);
        }

        Line(int tone, String title, String detail, int action, String actionLabel) {
            this(tone, title, detail, action, actionLabel, ACTION_NONE, null);
        }

        Line(int tone, String title, String detail, int action, String actionLabel, int secondary, String secondaryLabel) {
            this.tone = tone;
            this.title = title;
            this.detail = detail;
            this.action = action;
            this.actionLabel = actionLabel;
            this.secondary = secondary;
            this.secondaryLabel = secondaryLabel;
        }
    }

    // ------------------------------------------------------------------ 总状态

    public static int state(Snapshot s) {
        if (!s.checked) return CHECKING;
        if (s.restarting) return RESTARTING;
        Root.Device d = s.device;
        if (d == null || !d.granted) return NO_ROOT;
        if (!d.module) return d.updatePending ? MODULE_PENDING : NO_MODULE;
        if (d.disabled || d.removing) return MODULE_OFF;
        if (s.conf == null) return CONFIG_BAD;
        if (s.http == 200 && s.status != null) {
            if (login(s) != null) return READY;
            return d.wechatAuthed ? ACCOUNT_UNSEEN : LOGGED_OUT;
        }
        if (s.http == 401 || s.http == 403) return TOKEN_PENDING;
        if (!s.wechatInstalled) return NO_WECHAT;
        if (d.wechatPid <= 0) return WECHAT_STOPPED;
        if (d.wechatFrozen) return WECHAT_FROZEN;
        return SERVICE_DOWN;
    }

    public static Line hero(Snapshot s) {
        int state = state(s);
        Root.Device d = s.device;
        switch (state) {
            case CHECKING:
                return new Line(NEUTRAL, "正在检查", "正在连接微信里的知言服务…");
            case RESTARTING:
                return new Line(NEUTRAL, "正在重新启动微信", "新设置随微信启动生效；服务恢复后，客户端会自动重连。");
            case NO_ROOT:
                return new Line(ERROR, "需要 Root 授权",
                        "知言的配置在系统目录里，读写都要 Root。在 KernelSU 或 Magisk 里允许「知言」，然后重试。",
                        ACTION_RETRY_ROOT, "重试");
            case MODULE_PENDING:
                return new Line(WARNING, "重启手机后启用", "知言模块已经刷入，重启手机后生效。");
            case NO_MODULE:
                return new Line(ERROR, "没有安装知言模块",
                        "用 KernelSU 或 Magisk 刷入知言模块包（satori-wx-server），然后重启手机。模块依赖 Zygisk。");
            case MODULE_OFF:
                return d.removing
                        ? new Line(WARNING, "模块将被卸载", "知言模块已标记为卸载，重启手机后移除。要保留它，在模块管理器里撤销卸载。")
                        : new Line(WARNING, "模块已停用", "在 KernelSU 或 Magisk 里重新启用知言模块，然后重启手机。");
            case CONFIG_BAD:
                return new Line(ERROR, "配置文件无效",
                        (s.confError == null ? "配置无法读取" : s.confError) + "。服务端不会启动；在设置里重新保存一次即可修复。",
                        ACTION_FIX_CONFIG, "修复配置");
            case NO_WECHAT:
                return new Line(ERROR, "没有安装微信", "知言运行在微信里，请先安装微信。");
            case WECHAT_STOPPED:
                return new Line(WARNING, "微信没有在运行",
                        "知言服务随微信启动。系统在后台回收了微信时，服务也会一起停下。",
                        ACTION_OPEN_WECHAT, "打开微信");
            case WECHAT_FROZEN:
                return new Line(WARNING, "微信被系统冻结了",
                        "微信退到后台后被系统冻结，服务随之暂停，客户端收不到消息。打开微信即可恢复；"
                                + "要减少被冻结，在微信的应用详情里允许它后台运行（「耗电管理」一项）。",
                        ACTION_OPEN_WECHAT, "打开微信", ACTION_WECHAT_SETTINGS, "后台运行设置");
            case SERVICE_DOWN:
                return new Line(ERROR, "服务没有响应",
                        d.updatePending
                                ? "模块更新已经刷入但还没生效。重启手机后再试。"
                                : "微信在运行，但端口 " + s.port + " 没有回应。刚刷入模块时需要重启手机；否则重新启动微信通常可以恢复。",
                        ACTION_RESTART_WECHAT, "重新启动微信");
            case TOKEN_PENDING:
                return new Line(WARNING, "新令牌尚未生效",
                        "服务仍在使用旧令牌。重新启动微信后生效；客户端也要换成新令牌。",
                        ACTION_RESTART_WECHAT, "重新启动微信");
            case ACCOUNT_UNSEEN:
                return new Line(ERROR, "服务没认出已登录的账号",
                        "微信里已经登录，但知言模块读不到账号身份——微信改了登录信息的存放位置。"
                                + "在模块修好之前，客户端收不到消息、也发不出去。复制诊断报告可以帮助排查。");
            case LOGGED_OUT:
                return new Line(WARNING, "等待登录微信", "服务已就绪。在微信里登录账号后，客户端就能收到消息。",
                        ACTION_OPEN_WECHAT, "打开微信");
            default:
                JSONObject login = login(s);
                String name = displayName(login);
                return new Line(SUCCESS, "服务就绪", (name.isEmpty() ? "微信已登录。" : "已登录「" + name + "」。") + sendSummary(s));
        }
    }

    private static String sendSummary(Snapshot s) {
        JSONObject send = s.status == null ? null : s.status.optJSONObject("send");
        if (send == null || !send.optBoolean("enabled")) return "客户端可以接收消息；发送已关闭。";
        return "客户端可以接收消息，也可以向任意会话发送文字。";
    }

    /** 已登录（status = 1）的那个账号；没有则 null。 */
    /**
     * 客户端要填的服务地址：不带 {@code /v1}。Satori 把版本写进路径（{@code /v1/{资源}.{方法}}、{@code /v1/events}），
     * 标准客户端都是拿基础地址自己拼版本段——把 /v1 也复制进去反而会拼成 /v1/v1。
     */
    public static String endpoint(Snapshot s) {
        return "http://127.0.0.1:" + (s.conf != null ? s.conf.port : s.port);
    }

    public static JSONObject login(Snapshot s) {
        JSONArray logins = s.meta == null ? null : s.meta.optJSONArray("logins");
        for (int i = 0; logins != null && i < logins.length(); i++) {
            JSONObject login = logins.optJSONObject(i);
            if (login != null && login.optInt("status") == 1 && login.optJSONObject("user") != null) return login;
        }
        return null;
    }

    public static String selfId(Snapshot s) {
        JSONObject login = login(s);
        return login == null ? null : login.optJSONObject("user").optString("id", null);
    }

    static String displayName(JSONObject login) {
        if (login == null) return "";
        JSONObject user = login.optJSONObject("user");
        String nick = user.optString("nick", "");
        return nick.isEmpty() ? user.optString("name", "") : nick;
    }

    // ------------------------------------------------------------------ 链路

    /** 连接链路：微信 → 知言服务 → 微信账号 → 消息发送。 */
    public static int[] steps(Snapshot s) {
        int state = state(s);
        if (state == CHECKING || state == RESTARTING || state == NO_ROOT) {
            return new int[]{STEP_UNKNOWN, STEP_UNKNOWN, STEP_UNKNOWN, STEP_UNKNOWN};
        }
        Root.Device d = s.device;
        boolean serving = s.http == 200 && s.status != null;
        int wechat = !s.wechatInstalled ? STEP_FAIL
                : serving ? STEP_OK : d.wechatFrozen ? STEP_WAIT : d.wechatPid > 0 ? STEP_OK : STEP_WAIT;
        int service;
        if (serving) service = STEP_OK;
        else if (state == TOKEN_PENDING) service = STEP_WAIT;
        else if (state == WECHAT_STOPPED || state == NO_WECHAT) service = STEP_UNKNOWN;
        else if (state == WECHAT_FROZEN) service = STEP_WAIT;
        else service = STEP_FAIL;
        int account = !serving ? STEP_UNKNOWN : login(s) != null ? STEP_OK : d.wechatAuthed ? STEP_FAIL : STEP_WAIT;
        int send;
        JSONObject block = serving ? s.status.optJSONObject("send") : null;
        if (block == null) send = STEP_UNKNOWN;
        else if (!block.optBoolean("enabled")) send = STEP_OFF;
        else send = STEP_OK;
        return new int[]{wechat, service, account, send};
    }

    public static String[] stepValues(Snapshot s) {
        int state = state(s);
        if (state == CHECKING || state == RESTARTING) return new String[]{"检查中", "检查中", "检查中", "检查中"};
        if (state == NO_ROOT) return new String[]{"未知", "未知", "未知", "未知"};
        Root.Device d = s.device;
        boolean serving = s.http == 200 && s.status != null;
        String version = s.wechatVersion == null || s.wechatVersion.isEmpty() ? "" : " · " + s.wechatVersion;
        String wechat = !s.wechatInstalled ? "未安装" : serving ? "运行中" + version
                : d.wechatFrozen ? "在后台被系统冻结" : d.wechatPid > 0 ? "运行中" + version : "没有在运行";
        String service;
        if (serving) service = "127.0.0.1:" + s.port + " · v" + s.status.optString("version", "?");
        else if (state == TOKEN_PENDING) service = "在运行，令牌待生效";
        else if (state == NO_MODULE || state == MODULE_PENDING) service = "模块未安装";
        else if (state == MODULE_OFF) service = "模块已停用";
        else if (state == CONFIG_BAD) service = "配置无效，不会启动";
        else if (state == WECHAT_STOPPED || state == NO_WECHAT) service = "随微信启动";
        else if (state == WECHAT_FROZEN) service = "随微信冻结而暂停";
        else service = "端口 " + s.port + " 没有回应";
        String account;
        JSONObject login = serving ? login(s) : null;
        if (!serving) account = "—";
        else if (login == null) account = d.wechatAuthed ? "微信已登录，但服务没认出" : "未登录";
        else {
            String name = displayName(login);
            String id = login.optJSONObject("user").optString("id", "");
            account = name.isEmpty() ? id : name + "（" + id + "）";
        }
        String send;
        JSONObject block = serving ? s.status.optJSONObject("send") : null;
        if (block == null) send = "—";
        else if (!block.optBoolean("enabled")) send = "已关闭 · 只收不发";
        else send = "已开启 · 可发任意会话";
        return new String[]{wechat, service, account, send};
    }

    // ------------------------------------------------------------------ 生效

    /**
     * 已保存的配置是否已经被运行中的服务采用。配置只在微信主进程启动时读取，
     * 所以保存之后到重新启动微信之间，文件与运行状态会不一致。
     */
    public static Line applied(Snapshot s) {
        if (s.device != null && !s.device.granted) {
            return new Line(ERROR, "需要 Root 授权", "读取与保存知言的配置都要 Root。在 KernelSU 或 Magisk 里允许「知言」。");
        }
        if (s.device != null && !s.device.module) {
            return new Line(ERROR, "没有安装知言模块", "安装模块并重启手机后才能保存设置。");
        }
        if (s.conf == null && s.device != null && s.device.granted && s.device.module) {
            return new Line(ERROR, "配置文件无效", (s.confError == null ? "配置无法读取" : s.confError)
                    + "。保存一次即可用下面的设置重写它。");
        }
        if (s.http != 200 || s.status == null) {
            if (s.http == 401 || s.http == 403) {
                return new Line(WARNING, "已保存，重新启动微信后生效", "服务仍在使用旧令牌。",
                        ACTION_RESTART_WECHAT, "重新启动微信");
            }
            return new Line(NEUTRAL, "暂时无法确认是否生效", "连上知言服务后，这里会显示设置是否已经生效。");
        }
        List<String> pending = differences(s);
        if (pending.isEmpty()) return new Line(SUCCESS, "设置已生效", "微信里的服务正在使用这份配置。");
        return new Line(WARNING, "已保存，重新启动微信后生效",
                "还没生效：" + String.join("、", pending) + "。重新启动微信时客户端会短暂断开。",
                ACTION_RESTART_WECHAT, "重新启动微信");
    }

    /** 文件与运行状态不一致的项目，用于说明「哪些还没生效」。 */
    public static List<String> differences(Snapshot s) {
        java.util.ArrayList<String> out = new java.util.ArrayList<>();
        if (s.conf == null || s.status == null) return out;
        if (s.viaPrevious) out.add("端口或令牌");
        JSONObject send = s.status.optJSONObject("send");
        if (send == null) return out;
        if (send.optBoolean("enabled") != s.conf.send) out.add("发送开关");
        return out;
    }

    // ------------------------------------------------------------------ 被拦下的发送

    public static final int BLOCKED_DISABLED = 2;

    /** 最近一次发送被策略拦下：给首页一个「去开启发送」的捷径。 */
    public static final class Blocked {
        public final int reason;
        public final String target;
        public final long ageMs;

        Blocked(int reason, String target, long ageMs) {
            this.reason = reason;
            this.target = target;
            this.ageMs = ageMs;
        }
    }

    public static Blocked blocked(Snapshot s) {
        if (s.http != 200 || s.status == null || s.conf == null) return null;
        JSONObject send = s.status.optJSONObject("send");
        if (send == null || !send.has("last_age_ms") || send.optBoolean("last_ok", true)) return null;
        String error = send.optString("last_error", "");
        String target = send.optString("last_target", "");
        long age = send.optLong("last_age_ms");
        if (error.equals("send is disabled by configuration")) {
            return s.conf.send ? null : new Blocked(BLOCKED_DISABLED, target, age);
        }
        return null;
    }

    // ------------------------------------------------------------------ 诊断

    public static String[][] diagnostics(Snapshot s, String appVersion) {
        Root.Device d = s.device;
        String module = d == null || !d.granted ? "未知" : (!d.module ? "未安装" : (d.version.isEmpty() ? "已安装" : d.version));
        if (d != null && d.granted && !d.updateVersion.isEmpty()) module += "（" + d.updateVersion + " 待重启手机）";
        boolean serving = s.http == 200 && s.status != null;
        String running = serving ? "v" + s.status.optString("version", "?") : "未连接";
        if (serving && d != null && !d.version.isEmpty() && !d.version.equals("v" + s.status.optString("version"))) {
            running += "（与已安装的模块不同，重启手机后更新）";
        }
        String methods = "—";
        JSONObject login = serving ? login(s) : null;
        if (login != null) {
            JSONArray features = login.optJSONArray("features");
            methods = (features == null ? 0 : features.length()) + " / " + s.status.optInt("standard_methods", 37) + " 个标准方法";
        }
        JSONObject send = serving ? s.status.optJSONObject("send") : null;
        String counts = send == null ? "—" : "已发 " + send.optLong("sent") + " · 失败 " + send.optLong("failed")
                + " · 拦下 " + send.optLong("rejected") + " · 撤回 " + send.optLong("recalled");
        String last = "—";
        if (send != null && send.has("last_age_ms")) {
            last = ago(send.optLong("last_age_ms")) + " · " + (send.optBoolean("last_ok")
                    ? "已交给微信派发" : "未发出：" + reason(send.optString("last_error", "")));
        }
        String replay = serving && s.status.optBoolean("event_replay")
                ? "断线后补发最近 " + s.status.optInt("replay_capacity") + " 条" : "—";
        JSONObject keep = serving ? s.status.optJSONObject("keepalive") : null;
        String keepalive = keep == null ? "—"
                : (keep.optBoolean("notification") ? "常驻通知已发布" : "常驻通知未发布")
                        + " · 唤醒锁" + (keep.optBoolean("wakelock") ? "开启" : "关闭")
                        + (keep.has("cpu_held") ? "（CPU " + (keep.optBoolean("cpu_held") ? "持有" : "释放")
                                + " · Wi-Fi " + (keep.optBoolean("wifi_held") ? "持有" : "释放") + "）" : "");
        return new String[][]{
                {"知言应用", appVersion == null || appVersion.isEmpty() ? "未知" : appVersion},
                {"已安装的模块", module},
                {"运行中的服务", running},
                {"微信", !s.wechatInstalled ? "未安装" : (s.wechatVersion.isEmpty() ? "已安装" : s.wechatVersion)},
                {"协议方法", methods},
                {"发送计数", counts},
                {"上次发送", last},
                {"保活", keepalive},
                {"事件回放", replay},
        };
    }

    /** 服务端英文原因 → 界面上的中文说明。 */
    public static String reason(String error) {
        switch (error) {
            case "send is disabled by configuration": return "发送已关闭";
            case "empty target or content": return "目标或内容为空";
            case "JavaVM unavailable": return "微信运行环境未就绪";
            default:
                if (error.startsWith("network dispatcher unavailable")) return "微信网络层未就绪";
                if (error.startsWith("dispatch rejected")) return "微信拒绝派发";
                return error.isEmpty() ? "原因未知" : error;
        }
    }

    public static String ago(long millis) {
        long seconds = Math.max(0, millis / 1000);
        if (seconds < 60) return "刚刚";
        long minutes = seconds / 60;
        if (minutes < 60) return minutes + " 分钟前";
        long hours = minutes / 60;
        if (hours < 24) return hours + " 小时前";
        return (hours / 24) + " 天前";
    }

    /** 诊断报告：只含版本、状态与计数，不含令牌、微信号、昵称或消息内容。 */
    public static String report(Snapshot s, String appVersion, long now) {
        StringBuilder out = new StringBuilder("知言 · 诊断报告\n");
        out.append("检查时间：").append(new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.CHINA)
                .format(new java.util.Date(now))).append('\n');
        Line hero = hero(s);
        out.append("结论：").append(hero.title).append('\n');
        for (String[] row : diagnostics(s, appVersion)) out.append(row[0]).append("：").append(row[1]).append('\n');
        if (s.conf != null) {
            out.append("配置：端口 ").append(s.conf.port).append(" · 发送").append(s.conf.send ? "开启" : "关闭").append('\n');
        } else if (s.confError != null) {
            out.append("配置：无效（").append(s.confError).append("）\n");
        }
        List<String> pending = differences(s);
        if (!pending.isEmpty()) out.append("待生效：").append(String.join("、", pending)).append('\n');
        return out.append("报告不含令牌、微信号、昵称或消息内容。\n").toString();
    }
}
