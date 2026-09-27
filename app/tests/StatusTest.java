import com.satori.wx.core.Api;
import com.satori.wx.core.Conf;
import com.satori.wx.core.Root;
import com.satori.wx.core.Status;
import java.util.Arrays;
import java.util.Base64;
import java.nio.charset.StandardCharsets;
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
        Root.Device d = Root.parse("module=1\nversion=v0.6.7\nconf=" + b64(conf) + "\npid=6312\n");
        check(d.granted && d.module && !d.disabled && d.wechatPid == 6312 && d.version.equals("v0.6.7"), "解析探测输出");
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
        check(Status.stepValues(s)[0].contains("冻结") && Status.steps(s)[1] == Status.STEP_WAIT, "链路要说出冻结");
        check(!Root.parse("module=1\nfrozen=1\npid=\n").wechatFrozen, "没有进程就谈不上冻结");
        s.device = Root.parse("module=1\npid=77\n");
        expect(s, Status.SERVICE_DOWN, Status.ACTION_RESTART_WECHAT);
        s.http = 401;
        expect(s, Status.TOKEN_PENDING, Status.ACTION_RESTART_WECHAT);

        s.http = 200;
        s.status = new JSONObject("{\"version\":\"0.6.7\",\"standard_methods\":37,\"event_replay\":true,\"replay_capacity\":64,"
                + "\"send\":{\"enabled\":true,\"allow\":\"filehelper\",\"sent\":3,\"failed\":0,\"rejected\":1,\"recalled\":0}}");
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
        check(Status.hero(s).detail.contains("小明") && Status.hero(s).detail.contains("1 个白名单会话"), "就绪要说出账号与白名单");
        check(Arrays.equals(Status.steps(s), new int[]{Status.STEP_OK, Status.STEP_OK, Status.STEP_OK, Status.STEP_OK}), "就绪时链路全通");

        // 拿不到数据时不沿用在线结论
        s.http = Api.UNREACHABLE;
        check(Status.state(s) == Status.SERVICE_DOWN, "连不上时不能还是就绪");
        check(Status.steps(s)[1] == Status.STEP_FAIL && Status.steps(s)[2] == Status.STEP_UNKNOWN, "连不上时账号未知");
        s.http = 200;

        // ---- 已保存待生效 ----
        check(Status.applied(s).tone == Status.SUCCESS, "文件与运行一致 → 已生效");
        s.conf = Conf.parse("token=" + TOKEN + "\nsend=off\n");
        Status.Line pending = Status.applied(s);
        check(pending.tone == Status.WARNING && pending.action == Status.ACTION_RESTART_WECHAT
                && pending.detail.contains("发送开关") && pending.detail.contains("发送白名单"), "说清哪些没生效：" + pending.detail);
        s.conf = Conf.parse(conf);
        s.viaPrevious = true;
        check(Status.applied(s).detail.contains("端口或令牌"), "旧入口应答 → 端口或令牌待生效");
        s.viaPrevious = false;

        // ---- 被拦下的发送 ----
        s.status.getJSONObject("send").put("last_age_ms", 120000).put("last_ok", false)
                .put("last_error", "target not in send_allow").put("last_target", "123@chatroom");
        Status.Blocked blocked = Status.blocked(s);
        check(blocked != null && blocked.reason == Status.BLOCKED_NOT_ALLOWED && blocked.target.equals("123@chatroom"), "白名单拦下");
        s.conf = Conf.parse(conf.replace("filehelper", "filehelper;123@chatroom"));
        check(Status.blocked(s) == null, "已加入文件就不再提示");
        s.status.getJSONObject("send").put("last_error", "send is disabled by configuration");
        s.conf = Conf.parse("token=" + TOKEN + "\n");
        check(Status.blocked(s).reason == Status.BLOCKED_DISABLED, "发送关闭被拦");
        s.status.getJSONObject("send").put("last_ok", true);
        check(Status.blocked(s) == null, "上次成功就不提示");
        s.conf = Conf.parse(conf);

        // ---- 诊断报告不泄露 ----
        String report = Status.report(s, "1.0.0", 0);
        check(!report.contains(TOKEN) && !report.contains("wxid_secret") && !report.contains("小明")
                && !report.contains("filehelper"), "报告泄露了令牌 / 微信号 / 昵称 / 会话：\n" + report);
        check(report.contains("v0.6.7") && report.contains("白名单 1 个会话"), "报告缺少版本或配置摘要");

        check(Status.ago(30_000).equals("刚刚") && Status.ago(5 * 60_000).equals("5 分钟前") && Status.ago(3 * 3600_000L).equals("3 小时前"), "时间描述");
        check(Status.reason("rate limited").contains("限速"), "原因翻译");
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
