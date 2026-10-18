import com.satori.wx.core.Api;
import com.satori.wx.core.Conf;
import com.satori.wx.core.Root;
import com.satori.wx.core.Status;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Base64;
import org.json.JSONObject;

/**
 * 状态推导：每一种断点都要落到正确的结论与下一步；拿不到数据时不沿用在线结论；
 * 「已保存待生效」要说清是哪一项；诊断报告不泄露令牌、微信号与昵称。
 */
public final class StatusTest {
    private static final String TOKEN = "0123456789abcdef0123456789abcdef";
    private static int checks;

    public static void main(String[] args) throws Exception {
        // ---- 探测脚本输出的解析 ----
        String conf = "port=5601\ntoken=" + TOKEN + "\nsend=on\nsend_allow=filehelper\n";
        Root.Device d = Root.parse("module=1\nversion=v0.7.0\nconf=" + b64(conf) + "\npid=6312\n");
        check(d.granted && d.module && !d.disabled && d.wechatPid == 6312 && d.version.equals("v0.7.0"), "解析探测输出");
        check(conf.equals(d.confText), "配置原文经 base64 往返");
        check(Root.parse("pid=\n").wechatPid == 0 && Root.parse("module=1\nconf=%%%\n").confText == null, "空 pid 与坏 base64");

        // ---- 总状态 ----
        Status.Snapshot s = new Status.Snapshot();
        check(Status.state(s) == Status.CHECKING, "未检查 → 检查中");
        s.checked = true;
        s.device = Root.Device.DENIED;
        expect(s, Status.NO_ROOT, Status.ACTION_RETRY_ROOT);
        s.device = Root.parse("update=1\n");
        expect(s, Status.MODULE_PENDING, Status.ACTION_NONE);
        s.device = Root.parse("pid=0\n");
        expect(s, Status.NO_MODULE, Status.ACTION_NONE);
        s.device = Root.parse("module=1\ndisabled=1\n");
        expect(s, Status.MODULE_OFF, Status.ACTION_NONE);
        s.device = Root.parse("module=1\npid=0\n");
        s.confError = "缺少令牌（token）";
        expect(s, Status.CONFIG_BAD, Status.ACTION_FIX_CONFIG);
        check(Status.hero(s).detail.startsWith("缺少令牌"), "配置无效要说出原因");
        s.conf = Conf.parse(conf);
        s.confError = null;
        expect(s, Status.WECHAT_STOPPED, Status.ACTION_OPEN_WECHAT);
        s.wechatInstalled = false;
        expect(s, Status.NO_WECHAT, Status.ACTION_NONE);
        s.wechatInstalled = true;
        s.device = Root.parse("module=1\npid=77\nfrozen=1\n");
        check(s.device.wechatFrozen, "冻结标记");
        expect(s, Status.WECHAT_FROZEN, Status.ACTION_OPEN_WECHAT);
        check(Status.hero(s).secondary == Status.ACTION_WECHAT_SETTINGS, "冻结时给出后台运行设置入口");
        check(Status.stepValues(s)[0].contains("冻结") && Status.steps(s)[1] == Status.STEP_WAIT, "链路要说出冻结");
        check(!Root.parse("module=1\nfrozen=1\npid=\n").wechatFrozen, "没有进程就谈不上冻结");
        s.device = Root.parse("module=1\npid=77\n");
        expect(s, Status.SERVICE_DOWN, Status.ACTION_RESTART_WECHAT);
        s.http = 401;
        expect(s, Status.TOKEN_PENDING, Status.ACTION_RESTART_WECHAT);

        s.http = 200;
        s.status = new JSONObject("{\"version\":\"0.7.0\",\"standard_methods\":37,\"event_replay\":true,\"replay_capacity\":64,"
                + "\"send\":{\"ready\":true,\"resolved\":true,\"dispatcher\":true,\"sent\":3,\"failed\":2,\"rejected\":1,\"recalled\":0},"
                + "\"keepalive\":{\"notification\":true,\"wakelock\":false}}");
        s.meta = new JSONObject("{\"logins\":[]}");
        expect(s, Status.LOGGED_OUT, Status.ACTION_OPEN_WECHAT);
        s.device = Root.parse("module=1\npid=77\nauth=1114861342\n");
        expect(s, Status.ACCOUNT_UNSEEN, Status.ACTION_NONE);
        check(Status.stepValues(s)[2].contains("没认出") && Status.steps(s)[2] == Status.STEP_FAIL, "账号一环要说清");
        check(!Root.parse("auth=0\n").wechatAuthed && !Root.parse("auth=\n").wechatAuthed, "uin 为 0 或空不算已登录");
        s.device = Root.parse("module=1\npid=77\n");
        s.meta = new JSONObject("{\"logins\":[{\"sn\":1,\"status\":1,\"features\":[\"message.create\"],"
                + "\"user\":{\"id\":\"wxid_secret\",\"nick\":\"小明\",\"name\":\"xm\"}}]}");
        expect(s, Status.READY, Status.ACTION_NONE);
        check(Status.hero(s).detail.contains("小明") && Status.hero(s).detail.contains("收发"), "就绪要说出账号与可收发");
        check(!Status.hero(s).detail.contains("发送已关闭") && !Status.hero(s).detail.contains("只收不发"), "发送没有关闭这回事");
        check(Arrays.equals(Status.steps(s), new int[]{Status.STEP_OK, Status.STEP_OK, Status.STEP_OK, Status.STEP_OK}), "就绪时链路全通");
        check(Status.stepValues(s)[3].equals("就绪"), "发送一环就绪时只说「就绪」：" + Status.stepValues(s)[3]);
        // 发送没有开关：这一环只反映微信网络层是否可派发。
        s.status.getJSONObject("send").put("dispatcher", false);
        check(Status.steps(s)[3] == Status.STEP_WAIT && Status.stepValues(s)[3].contains("网络层"), "网络层未就绪时发送一环是等待");
        s.status.getJSONObject("send").put("dispatcher", true);

        // 拿不到数据时不沿用在线结论
        s.http = Api.UNREACHABLE;
        check(Status.state(s) == Status.SERVICE_DOWN, "连不上时不能还是就绪");
        check(Status.steps(s)[1] == Status.STEP_FAIL && Status.steps(s)[2] == Status.STEP_UNKNOWN, "连不上时账号未知");
        s.http = 200;

        // ---- 已保存待生效 ----
        check(Status.applied(s).tone == Status.SUCCESS, "文件与运行一致 → 已生效");
        // 配置里遗留的 send=off 不再有任何效果：不算「待生效」，也不改变结论。
        s.conf = Conf.parse("token=" + TOKEN + "\nsend=off\n");
        check(Status.applied(s).tone == Status.SUCCESS && Status.differences(s).isEmpty(), "旧文件里的 send=off 不是待生效项");
        s.conf = Conf.parse(conf);
        s.viaPrevious = true;
        check(Status.applied(s).detail.contains("端口或令牌"), "旧入口应答 → 端口或令牌待生效");
        s.viaPrevious = false;

        // ---- 诊断报告不泄露 ----
        String report = Status.report(s, "1.0.0", 0);
        check(!report.contains(TOKEN) && !report.contains("wxid_secret") && !report.contains("小明")
                && !report.contains("filehelper"), "报告泄露了令牌 / 微信号 / 昵称 / 会话：\n" + report);
        check(report.contains("v0.7.0") && report.contains("配置：端口 5601") && !report.contains("发送开启") && !report.contains("发送关闭"), "报告缺少版本或配置摘要");
        check(report.contains("已发 3 · 失败 2 · 撤回 0") && !report.contains("拦下"), "发送计数不再有「拦下」：\n" + report);

        paused();
        publishedOnly();
        sendAbility();
        guardAndProbe();

        check(Status.ago(30_000).equals("刚刚") && Status.ago(5 * 60_000).equals("5 分钟前") && Status.ago(3 * 3600_000L).equals("3 小时前"), "时间描述");
        check(Status.reason("JavaVM unavailable").contains("未就绪"), "原因翻译");
        check(Status.endpoint(s).equals("http://127.0.0.1:5601"), "服务地址不带 /v1，客户端自己拼版本段");
        System.out.println("StatusTest: " + checks + " checks");
    }

    private static final String LIVE = "{\"version\":\"0.16.0\",\"standard_methods\":37,\"event_replay\":true,\"replay_capacity\":64,"
            + "\"send\":{\"ready\":true,\"resolved\":true,\"dispatcher\":true,\"state\":\"ready\",\"sent\":3,\"failed\":0,\"recalled\":0},"
            + "\"keepalive\":{\"notification\":true,\"wakelock\":true,\"uptime_ms\":11520000}}";
    private static final String META = "{\"logins\":[{\"sn\":1,\"status\":1,\"features\":[\"message.create\"],"
            + "\"user\":{\"id\":\"wxid_secret\",\"nick\":\"小明\",\"name\":\"xm\"}}]}";

    private static Status.Snapshot base(String probe) throws Exception {
        Status.Snapshot s = new Status.Snapshot();
        s.checked = true;
        s.device = Root.parse(probe);
        s.conf = Conf.parse("port=5601\ntoken=" + TOKEN + "\n");
        s.wechatVersion = "8.0.79";
        s.now = 1_000_000;
        return s;
    }

    /** 微信被冻结、服务暂停：沿用同一个进程里最近一次应答，说清是暂停中，而不是整排变成「未知」。 */
    private static void paused() throws Exception {
        Status.Snapshot s = base("module=1\nversion=v0.16.0\npid=77\nfrozen=1\n");
        s.carriedStatus = new JSONObject(LIVE);
        s.carriedMeta = new JSONObject(META);
        s.carriedPid = 77;
        expect(s, Status.WECHAT_FROZEN, Status.ACTION_OPEN_WECHAT);
        check(Status.hero(s).detail.contains("冻结前已登录「小明」"), "暂停时要说出冻结前的账号：" + Status.hero(s).detail);
        check(Status.hero(s).secondary == Status.ACTION_WECHAT_SETTINGS, "没有看守记录时次要入口是后台运行设置");
        String[] values = Status.stepValues(s);
        check(values[0].contains("冻结") && values[1].startsWith("已暂停") && values[1].contains("v0.16.0"), "链路要说出暂停：" + Arrays.toString(values));
        check(values[2].contains("小明") && values[3].equals("就绪"), "暂停时账号与发送沿用暂停前的：" + Arrays.toString(values));
        int[] steps = Status.steps(s);
        check(steps[1] == Status.STEP_WAIT && steps[2] == Status.STEP_OK && steps[3] == Status.STEP_OK, "暂停时服务是等待、账号与发送沿用");
        check(Status.statusView(s) == s.carriedStatus, "要显示的是沿用的状态");

        // 进程换了：那份应答是上一个进程的，绝不沿用。
        s.carriedPid = 70;
        check(Status.statusView(s) == null && Status.login(s) == null, "pid 对不上就不沿用");
        check(Status.stepValues(s)[2].equals("—") && Status.steps(s)[2] == Status.STEP_UNKNOWN, "换了进程，账号一环回到未知");
        check(Status.state(s) == Status.WECHAT_FROZEN, "冻结本身仍是冻结");
        s.carriedPid = 77;

        // 探测的那一刻恰好解冻、连接却超时：只要之前确实在服务、又没超过宽限，也是暂停，不是「服务没有响应」。
        s.device = Root.parse("module=1\nversion=v0.16.0\npid=77\n");
        s.failure = Api.FAIL_TIMEOUT;
        s.failSince = s.now - 3000;
        expect(s, Status.WECHAT_FROZEN, Status.ACTION_OPEN_WECHAT);
        check(Status.hero(s).title.contains("暂时没有回应") && !Status.hero(s).title.contains("冻结"),
                "没有冻结的证据只是超时：不能断言是冻结：" + Status.hero(s).title);
        check(Status.stepValues(s)[1].contains("暂时没有应答") && Status.stepValues(s)[0].contains("暂时没有应答"), "链路也只说暂时没有应答");
        s.failSince = s.now - 45_000;
        expect(s, Status.SERVICE_DOWN, Status.ACTION_RESTART_WECHAT); // 超过半分钟还没恢复，就提示重启
        s.failSince = s.now - 3 * 60_000;
        expect(s, Status.SERVICE_DOWN, Status.ACTION_RESTART_WECHAT);
        check(Status.hero(s).detail.contains("没有回应"), "超时太久才说服务没有响应");
        s.failure = Api.FAIL_REFUSED;
        s.failSince = s.now - 1000;
        expect(s, Status.SERVICE_DOWN, Status.ACTION_RESTART_WECHAT);
        check(Status.hero(s).detail.contains("拒绝连接"), "端口拒绝连接要说出来：" + Status.hero(s).detail);
        s.failure = Api.FAIL_TIMEOUT;
        s.carriedStatus = null;
        s.carriedMeta = null;
        s.failSince = s.now - 1000;
        expect(s, Status.SERVICE_DOWN, Status.ACTION_RESTART_WECHAT); // 什么都不知道，也就不能说「暂停」
    }

    /** 冷启动撞上冻结：应用没见过活应答，但模块发布的状态文件还在。 */
    private static void publishedOnly() throws Exception {
        Status.Snapshot s = base("module=1\nversion=v0.16.0\npid=77\nfrozen=1\n");
        s.published = Status.parsePublished("{\"schema\":1,\"version\":\"0.16.0\",\"pid\":77,\"port\":5601,\"listening\":true,"
                + "\"login\":{\"online\":1,\"id\":\"wxid_secret\",\"name\":\"小明\"},\"clients\":1,\"serving_since\":900000,"
                + "\"send\":{\"state\":\"incompatible\",\"detail\":\"send methods not found\"},\"changed_at\":990000}");
        expect(s, Status.WECHAT_FROZEN, Status.ACTION_OPEN_WECHAT);
        check(Status.hero(s).detail.contains("冻结前已登录「小明」"), "发布的状态里有账号：" + Status.hero(s).detail);
        check(Status.stepValues(s)[2].contains("小明"), "账号一环用发布的状态");
        check(Status.sendState(s).equals("incompatible") && Status.steps(s)[3] == Status.STEP_FAIL, "发送能力也来自发布的状态");
        check(Status.diagnostics(s, "1")[10][1].startsWith("已发布 · 刚刚变化") || Status.diagnostics(s, "1")[10][1].startsWith("已发布"), "状态发布一行");
        // 上一个微信进程留下的文件不算数。
        s.published.put("pid", 70);
        check(Status.published(s) == null && Status.login(s) == null, "pid 对不上的发布状态是遗物");
        check(Status.parsePublished("not json") == null && Status.parsePublished("") == null && Status.parsePublished(null) == null, "坏文件当没有");
        // 服务在应答时，听服务的，不听文件。
        s.published.put("pid", 77);
        s.device = Root.parse("module=1\npid=77\n");
        s.http = 200;
        s.status = new JSONObject(LIVE);
        s.meta = new JSONObject("{\"logins\":[]}");
        check(Status.login(s) == null && Status.state(s) == Status.LOGGED_OUT, "活数据说没登录，就是没登录，不看文件");
    }

    /** 发送能力：新模块直接说；老模块只有两个布尔；与微信版本对不上要明说，且只能收不能发。 */
    private static void sendAbility() throws Exception {
        Status.Snapshot s = base("module=1\nversion=v0.16.0\npid=77\n");
        s.http = 200;
        s.meta = new JSONObject(META);
        s.status = new JSONObject(LIVE);
        expect(s, Status.READY, Status.ACTION_NONE);
        check(Status.stepValues(s)[1].contains("已在线 3 小时 12 分"), "服务一行带在线时长：" + Status.stepValues(s)[1]);
        s.status.getJSONObject("send").put("state", "incompatible").put("detail", "send methods not found (version mismatch?)");
        expect(s, Status.READY_NO_SEND, Status.ACTION_NONE);
        check(Status.hero(s).tone == Status.WARNING && Status.hero(s).title.contains("暂不能发送"), "只能收不能发是警告");
        check(Status.hero(s).detail.contains("8.0.79") && Status.hero(s).detail.contains("接收消息不受影响"), "说清是哪个微信版本、收不受影响：" + Status.hero(s).detail);
        check(Status.hero(s).detail.contains("（v0.16.0）"), "说的是正在运行的服务的版本，不是磁盘上待重启的：" + Status.hero(s).detail);
        check(Status.steps(s)[3] == Status.STEP_FAIL && Status.stepValues(s)[3].contains("8.0.79"), "发送一环是失败并带版本：" + Status.stepValues(s)[3]);
        check(Status.diagnostics(s, "1")[5][1].contains("version mismatch"), "诊断里有服务端给的原因");
        s.status.getJSONObject("send").put("state", "waiting");
        check(Status.state(s) == Status.READY && Status.steps(s)[3] == Status.STEP_WAIT && Status.stepValues(s)[3].contains("网络层"), "等待网络层不是告警");
        // 老模块：没有 state，只有 resolved / dispatcher。解析不到类，就别说「等待网络层」。
        s.status = new JSONObject("{\"version\":\"0.15.1\",\"send\":{\"ready\":true,\"resolved\":false,\"dispatcher\":false,\"sent\":0,\"failed\":0},"
                + "\"keepalive\":{\"uptime_ms\":120000}}");
        check(Status.sendState(s).equals("unready") && Status.stepValues(s)[3].contains("可能与当前微信版本不兼容"), "老模块刚上线还没解析到类：" + Status.stepValues(s)[3]);
        check(Status.state(s) == Status.READY && Status.steps(s)[3] == Status.STEP_WAIT, "老模块还在试，不升级成告警");
        s.status.getJSONObject("keepalive").put("uptime_ms", 3_600_000);
        check(Status.sendState(s).equals("incompatible") && Status.state(s) == Status.READY_NO_SEND, "老模块试了 15 分钟还解析不到，就是不兼容");
        s.status = new JSONObject("{\"version\":\"0.15.1\",\"send\":{\"ready\":true,\"resolved\":true,\"dispatcher\":false}}");
        check(Status.sendState(s).equals("waiting"), "老模块解析到了类但还没网络层");
        s.status = new JSONObject("{\"version\":\"0.15.1\",\"send\":{\"ready\":true,\"resolved\":true,\"dispatcher\":true}}");
        check(Status.sendState(s).equals("ready"), "老模块就绪");
        check(Status.duration(5 * 60_000).equals("5 分钟") && Status.duration(3 * 3600_000L + 12 * 60_000).equals("3 小时 12 分")
                && Status.duration(49 * 3600_000L).equals("2 天 1 小时"), "时长");
    }

    /** 探测输出里的发布状态与保活看守；看守停着时，冻结的状态卡要给出「开启保活看守」。 */
    private static void guardAndProbe() throws Exception {
        String published = "{\"schema\":1,\"pid\":77,\"login\":{\"online\":0}}";
        Root.Device d = Root.parse("module=1\npid=77\nguard=PAUSED\nguard_reason=user-stop\nguard_watch=1\npublished=" + b64(published) + "\n");
        check(d.guardPaused() && d.guardKnown() && d.guardWatching && d.guardReason.equals("user-stop"), "解析看守状态");
        check(published.equals(d.publishedText), "发布的状态经 base64 往返");
        check(!Root.parse("module=1\n").guardKnown() && !Root.parse("module=1\n").guardPaused() && Root.parse("module=1\n").publishedText == null,
                "老模块没有这些：看守未知、没有发布状态");
        check(!Root.parse("published=%%%\n").guardKnown() && Root.parse("published=%%%\n").publishedText == null, "坏的 base64 当没有");
        check(Root.parse("guard=ARMED\n").guardKnown() && !Root.parse("guard=ARMED\n").guardPaused(), "开启的看守");

        Status.Snapshot s = base("module=1\npid=77\nfrozen=1\nguard=PAUSED\nguard_reason=user-stop\n");
        s.carriedStatus = new JSONObject(LIVE);
        s.carriedMeta = new JSONObject(META);
        s.carriedPid = 77;
        Status.Line line = Status.hero(s);
        check(line.secondary == Status.ACTION_GUARD_ON && line.secondaryLabel.contains("看守") && line.detail.contains("保活看守已暂停"),
                "看守停着时给出开启入口：" + line.detail);
        check(Status.guardText(s.device).contains("已暂停（手动停止）"), "看守一行：" + Status.guardText(s.device));
        check(Status.guardText(Root.parse("module=1\n")).equals("—"), "没有记录就不说");
        check(Status.guardText(Root.parse("guard=ARMED\nguard_watch=1\n")).contains("立即解冻"), "开着的看守");
        check(Status.guardHint(s.device).contains("已暂停") && !Status.guardHint(Root.parse("guard=ARMED\n")).contains("已暂停"), "开关下的说明");
        check(Status.diagnostics(s, "1")[9][0].equals("保活看守"), "诊断里有保活看守");
        // 报告仍然不含账号信息，即使这些信息现在也是沿用来的。
        String report = Status.report(s, "1.0.0", 0);
        check(!report.contains(TOKEN) && !report.contains("wxid_secret") && !report.contains("小明"), "报告泄露了账号信息：\n" + report);
        check(report.contains("保活看守：已暂停"), "报告里有看守状态：\n" + report);
    }

    private static void expect(Status.Snapshot s, int state, int action) {
        check(Status.state(s) == state, "期望状态 " + state + "，实际 " + Status.state(s));
        check(Status.hero(s).action == action, "状态 " + state + " 的下一步应为 " + action + "，实际 " + Status.hero(s).action);
    }

    private static String b64(String text) {
        return Base64.getEncoder().encodeToString(text.getBytes(StandardCharsets.UTF_8));
    }

    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }
}
