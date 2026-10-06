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

        check(Status.ago(30_000).equals("刚刚") && Status.ago(5 * 60_000).equals("5 分钟前") && Status.ago(3 * 3600_000L).equals("3 小时前"), "时间描述");
        check(Status.reason("JavaVM unavailable").contains("未就绪"), "原因翻译");
        check(Status.endpoint(s).equals("http://127.0.0.1:5601"), "服务地址不带 /v1，客户端自己拼版本段");
        System.out.println("StatusTest: " + checks + " checks");
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
