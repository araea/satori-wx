package com.satori.wx.ui;

import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import com.satori.wx.R;
import com.satori.wx.core.Status;
import com.satori.wx.core.Talker;
import java.util.Map;
import org.json.JSONObject;

/**
 * 首页：服务通了没有 → 断在哪一环 → 下一步做什么；然后是客户端接入信息、微信操作与诊断。
 *
 * <p>只负责视图与把 {@link Model} 画上去；数据怎么来、按钮做什么由宿主决定（{@link Actions}）。
 * 每次渲染只改真的变了的文字，轮询不会引起整页重排或重复朗读。
 */
final class HomePage {
    interface Actions {
        void refresh();
        void openSettings();
        void heroAction(int action);
        void openWeChat();
        void restartWeChat();
        void copyEndpoint(boolean events);
        void copyToken();
        void copyReport();
        void shareReport();
        void allowBlocked(String target);
        void enableSend();
    }

    /** 首页渲染需要的全部输入。 */
    static final class Model {
        final Status.Snapshot snapshot = new Status.Snapshot();
        boolean probing;
        String appVersion = "";
        /** 会话 ID → 名字（来自联系人列表缓存），用于把被拦下的目标说成人话。 */
        Map<String, String> names;
    }

    final LinearLayout root;
    final ScrollView scroll;
    final TopBar bar;
    private final Ui ui;
    private final Tokens t;
    private final Actions actions;
    private final Shape heroShape;
    private final Badge badge;
    private final LoadingIndicator loading;
    private final TextView heroLabel, heroTitle, heroDetail;
    private final Btn heroAction;
    private final Notice blocked;
    private final TextView sent, rejected;
    private final Item[] chain = new Item[4];
    private final Item http, events, token, openWeChat, restartWeChat;
    private final Item[] data;
    private final ImageButton refresh;
    private int heroTone = -1;
    private int heroActionKind = Status.ACTION_NONE;
    private int[] stepStates = new int[0];
    private Status.Blocked blockedNow;

    HomePage(Ui ui, Actions actions, boolean showSettingsEntry) {
        this.ui = ui;
        this.t = ui.t;
        this.actions = actions;

        root = ui.column();
        root.setId(R.id.home);
        bar = new TopBar(ui, "知言", t.surface, t.surfaceContainer);
        refresh = ui.iconButton(Icon.REFRESH, "刷新状态", t.onSurfaceVariant);
        refresh.setId(R.id.refresh);
        refresh.setOnClickListener(v -> actions.refresh());
        bar.action(refresh);
        if (showSettingsEntry) {
            ImageButton settings = ui.iconButton(Icon.SETTINGS, "设置", t.onSurfaceVariant);
            settings.setId(R.id.open_settings);
            settings.setOnClickListener(v -> actions.openSettings());
            bar.action(settings);
        }
        root.addView(bar, new LinearLayout.LayoutParams(-1, -2));

        scroll = Pages.scroll(t);
        LinearLayout content = Pages.content(ui);
        scroll.addView(Pages.center(ui, content));
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));

        // ---- 大标题 ----
        LinearLayout brand = ui.row();
        ImageView logo = new ImageView(t.context);
        logo.setImageDrawable(t.context.getApplicationInfo().loadIcon(t.context.getPackageManager()));
        logo.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        brand.addView(logo, new LinearLayout.LayoutParams(t.logo, t.logo));
        brand.addView(ui.heading("知言", Tokens.DISPLAY_SMALL, t.onSurface), Ui.share(t.spaceMd));
        content.addView(brand, Ui.stack(t.spaceSm));
        content.addView(ui.text("微信的 Satori 服务", Tokens.BODY_LARGE, t.onSurfaceVariant), Ui.stack(t.spaceXs));
        bar.follow(scroll, brand);

        // ---- 状态卡片 ----
        LinearLayout hero = ui.column();
        hero.setId(R.id.hero);
        hero.setPadding(t.spaceXl, t.spaceXl, t.spaceXl, t.spaceXl);
        heroShape = new Shape(t.surfaceContainerHigh, t.shapeXlIncreased);
        hero.setBackground(heroShape);
        LinearLayout top = ui.row();
        FrameLayout mark = new FrameLayout(t.context);
        badge = new Badge(t);
        mark.addView(badge, new FrameLayout.LayoutParams(t.badge, t.badge, Gravity.CENTER));
        loading = new LoadingIndicator(t, t.primary);
        loading.setId(R.id.loading);
        mark.addView(loading, new FrameLayout.LayoutParams(t.loading, t.loading, Gravity.CENTER));
        top.addView(mark, new LinearLayout.LayoutParams(t.badge, t.badge));
        heroLabel = ui.text("本机服务", Tokens.LABEL_LARGE, t.onSurfaceVariant);
        top.addView(heroLabel, Ui.share(t.spaceMd));
        hero.addView(top, Ui.stack(0));
        heroTitle = Ui.live(ui.heading("正在检查", Tokens.HEADLINE_MEDIUM_EMPHASIZED, t.onSurface));
        heroTitle.setId(R.id.hero_title);
        hero.addView(heroTitle, Ui.stack(t.spaceLg));
        heroDetail = Ui.live(ui.text("", Tokens.BODY_LARGE, t.onSurfaceVariant));
        heroDetail.setId(R.id.hero_detail);
        hero.addView(heroDetail, Ui.stack(t.spaceSm));
        heroAction = ui.medium("", Btn.FILLED);
        heroAction.setId(R.id.hero_action);
        heroAction.setOnClickListener(v -> actions.heroAction(heroActionKind));
        heroAction.setVisibility(View.GONE);
        LinearLayout.LayoutParams actionParams = new LinearLayout.LayoutParams(-2, -2);
        actionParams.topMargin = t.spaceXl;
        hero.addView(heroAction, actionParams);
        content.addView(hero, Ui.stack(t.spaceXl));

        // ---- 被拦下的发送 ----
        blocked = new Notice(t);
        blocked.setId(R.id.blocked);
        blocked.action("", v -> {
            if (blockedNow == null) return;
            if (blockedNow.reason == Status.BLOCKED_NOT_ALLOWED) actions.allowBlocked(blockedNow.target);
            else actions.enableSend();
        });
        blocked.setVisibility(View.GONE);
        content.addView(blocked, Ui.stack(t.spaceMd));

        // ---- 两个指标 ----
        boolean pair = ui.layout.pair();
        LinearLayout metrics = pair ? ui.row() : ui.column();
        TextView[] values = new TextView[2];
        String[] labels = {"已发出", "被拦下"};
        for (int i = 0; i < 2; i++) {
            LinearLayout tile = ui.column();
            tile.setPadding(t.spaceLg + t.spaceXs, t.spaceLg, t.spaceLg + t.spaceXs, t.spaceLg);
            float big = t.shapeLgIncreased, small = t.shapeXs;
            Shape shape = new Shape(t.surfaceContainer, 0);
            if (pair) shape.setRadii(i == 0 ? big : small, i == 0 ? small : big, i == 0 ? small : big, i == 0 ? big : small);
            else shape.setRadii(i == 0 ? big : small, i == 0 ? big : small, i == 0 ? small : big, i == 0 ? small : big);
            tile.setBackground(shape);
            tile.addView(ui.text(labels[i], Tokens.LABEL_MEDIUM, t.onSurfaceVariant), Ui.stack(0));
            values[i] = ui.text("—", Tokens.HEADLINE_SMALL, t.onSurface);
            tile.addView(values[i], Ui.stack(t.spaceXs));
            if (android.os.Build.VERSION.SDK_INT >= 28) tile.setScreenReaderFocusable(true);
            if (pair) metrics.addView(tile, Ui.share(i == 0 ? 0 : t.space2xs));
            else metrics.addView(tile, Ui.stack(i == 0 ? 0 : t.space2xs));
        }
        if (pair) {
            metrics.setGravity(Gravity.FILL_VERTICAL);
            for (int i = 0; i < 2; i++) ((LinearLayout.LayoutParams) metrics.getChildAt(i).getLayoutParams()).height = -1;
        }
        sent = values[0];
        rejected = values[1];
        content.addView(metrics, Ui.stack(t.spaceMd));

        // ---- 连接链路 ----
        content.addView(ui.sectionTitle("连接链路"), Ui.stack(t.space2xl));
        Ui.Group chainGroup = ui.group();
        String[] names = {"微信", "知言服务", "微信账号", "消息发送"};
        for (int i = 0; i < chain.length; i++) {
            chain[i] = chainGroup.add(new Item(t, Item.STATIC, names[i], "检查中").leading(Icon.UNKNOWN, t.onSurfaceVariant));
        }
        content.addView(chainGroup, Ui.stack(0));

        // ---- 客户端接入 ----
        content.addView(ui.sectionTitle("客户端接入"), Ui.stack(t.space2xl));
        Ui.Group connect = ui.group();
        ImageButton copyHttp = ui.iconButton(Icon.COPY, "复制 HTTP 接口地址", t.onSurfaceVariant);
        copyHttp.setOnClickListener(v -> actions.copyEndpoint(false));
        http = connect.add(new Item(t, Item.STATIC, "http://127.0.0.1:5601/v1", "HTTP 接口 · 只供本机客户端")
                .leading(Icon.LINK, t.onSurfaceVariant).trailing(copyHttp));
        http.setId(R.id.endpoint);
        ImageButton copyEvents = ui.iconButton(Icon.COPY, "复制事件推送地址", t.onSurfaceVariant);
        copyEvents.setOnClickListener(v -> actions.copyEndpoint(true));
        events = connect.add(new Item(t, Item.STATIC, "ws://127.0.0.1:5601/v1/events", "事件推送 · WebSocket")
                .leading(Icon.STREAM, t.onSurfaceVariant).trailing(copyEvents));
        ImageButton copyToken = ui.iconButton(Icon.COPY, "复制令牌", t.onSurfaceVariant);
        copyToken.setId(R.id.copy_token);
        copyToken.setOnClickListener(v -> actions.copyToken());
        token = connect.add(new Item(t, Item.STATIC, "令牌", "读取中").leading(Icon.KEY, t.onSurfaceVariant).trailing(copyToken));
        content.addView(connect, Ui.stack(0));
        content.addView(ui.text("客户端的 Satori 适配器填这两个地址，令牌作为 Bearer 鉴权。",
                Tokens.BODY_SMALL, t.onSurfaceVariant), Pages.note(t));

        // ---- 微信 ----
        content.addView(ui.sectionTitle("微信"), Ui.stack(t.space2xl));
        Ui.Group wechat = ui.group();
        openWeChat = wechat.add(new Item(t, Item.ACTION, "打开微信", "服务随微信主进程运行").leading(Icon.OPEN, t.onSurfaceVariant));
        openWeChat.setId(R.id.open_wechat);
        openWeChat.setOnClickListener(v -> actions.openWeChat());
        restartWeChat = wechat.add(new Item(t, Item.ACTION, "重新启动微信", "让新设置生效；客户端会短暂断开")
                .leading(Icon.RESTART, t.onSurfaceVariant));
        restartWeChat.setId(R.id.restart_wechat);
        restartWeChat.setOnClickListener(v -> actions.restartWeChat());
        content.addView(wechat, Ui.stack(0));

        // ---- 诊断 ----
        content.addView(ui.sectionTitle("诊断"), Ui.stack(t.space2xl));
        Ui.Group panel = ui.group();
        panel.setId(R.id.diagnostics);
        String[][] rows = Status.diagnostics(new Status.Snapshot(), "");
        data = new Item[rows.length];
        for (int i = 0; i < rows.length; i++) data[i] = panel.add(new Item(t, Item.STATIC, rows[i][0], "—"));
        content.addView(panel, Ui.stack(0));
        Btn copyReport = ui.button("复制报告", Btn.TONAL).icon(Icon.COPY);
        copyReport.setOnClickListener(v -> actions.copyReport());
        Btn share = ui.button("分享报告", Btn.TONAL).icon(Icon.SHARE);
        share.setOnClickListener(v -> actions.shareReport());
        content.addView(ui.connected(copyReport, share), Ui.stack(t.spaceMd));
        content.addView(ui.text("报告只含版本、状态与计数，不含令牌、微信号、昵称或消息内容。",
                Tokens.BODY_SMALL, t.onSurfaceVariant), Pages.note(t));

        TextView footer = ui.text("关闭知言不影响服务：服务在微信里运行。", Tokens.BODY_SMALL, t.onSurfaceVariant);
        footer.setGravity(Gravity.CENTER);
        content.addView(footer, Ui.stack(t.space2xl));
    }

    void render(Model m) {
        Status.Snapshot s = m.snapshot;
        int state = Status.state(s);
        Status.Line line = Status.hero(s);
        boolean firstCheck = !s.checked || state == Status.RESTARTING;
        loading.setVisibility(firstCheck ? View.VISIBLE : View.GONE);
        badge.setVisibility(firstCheck ? View.INVISIBLE : View.VISIBLE);
        refresh.setEnabled(!m.probing);
        refresh.setAlpha(m.probing ? t.disabledContentPct / 100f : 1f);

        if (heroTone != line.tone) {
            heroTone = line.tone;
            int ink = t.toneOnContainer(line.tone);
            heroShape.fill(t.toneContainer(line.tone));
            heroLabel.setTextColor(line.tone == Status.NEUTRAL ? t.onSurfaceVariant : ink);
            heroTitle.setTextColor(ink);
            heroDetail.setTextColor(line.tone == Status.NEUTRAL ? t.onSurfaceVariant : ink);
        }
        badge.show(line.tone, s.checked);
        Ui.set(heroTitle, line.title);
        Ui.set(heroDetail, line.detail);
        heroActionKind = line.action;
        heroAction.setVisibility(line.action == Status.ACTION_NONE ? View.GONE : View.VISIBLE);
        if (line.actionLabel != null) Ui.set(heroAction, line.actionLabel);

        // ---- 被拦下 ----
        blockedNow = Status.blocked(s);
        if (blockedNow == null) {
            blocked.setVisibility(View.GONE);
        } else {
            String when = Status.ago(blockedNow.ageMs);
            if (blockedNow.reason == Status.BLOCKED_NOT_ALLOWED) {
                String name = m.names == null ? null : m.names.get(blockedNow.target);
                String who = name == null ? Talker.fallbackName(blockedNow.target) + " " + blockedNow.target : "「" + name + "」";
                blocked.show(Status.WARNING, "有一条消息被白名单拦下",
                        when + "，客户端想发给" + who + "，但它不在发送白名单里。", "加入白名单");
            } else {
                blocked.show(Status.WARNING, "有一条消息没有发出", when + "，客户端尝试发送，但发送已关闭。", "去开启发送");
            }
            blocked.setVisibility(View.VISIBLE);
        }

        // ---- 指标 ----
        JSONObject send = s.http == 200 && s.status != null ? s.status.optJSONObject("send") : null;
        Ui.set(sent, send == null ? "—" : String.valueOf(send.optLong("sent")));
        Ui.set(rejected, send == null ? "—" : String.valueOf(send.optLong("rejected")));

        // ---- 链路 ----
        int[] steps = Status.steps(s);
        String[] values = Status.stepValues(s);
        boolean restyle = !java.util.Arrays.equals(steps, stepStates);
        stepStates = steps;
        for (int i = 0; i < chain.length; i++) {
            if (restyle) {
                int kind, color;
                switch (steps[i]) {
                    case Status.STEP_OK: kind = Icon.OK; color = t.success; break;
                    case Status.STEP_WAIT: kind = Icon.WAIT; color = t.warning; break;
                    case Status.STEP_FAIL: kind = Icon.ERROR; color = t.error; break;
                    case Status.STEP_OFF: kind = Icon.BLOCK; color = t.onSurfaceVariant; break;
                    default: kind = Icon.UNKNOWN; color = t.onSurfaceVariant; break;
                }
                chain[i].leading(kind, color);
            }
            chain[i].setSupporting(values[i]);
        }

        // ---- 接入 ----
        int port = s.conf != null ? s.conf.port : s.port;
        http.setHeadline("http://127.0.0.1:" + port + "/v1");
        events.setHeadline("ws://127.0.0.1:" + port + "/v1/events");
        token.setSupporting(s.conf == null ? (s.device == null ? "读取中" : "不可用") : "已设置 · " + s.conf.token.length() + " 位");

        boolean root = s.device != null && s.device.granted;
        restartWeChat.setEnabled(root && s.wechatInstalled && !s.restarting);
        openWeChat.setEnabled(s.wechatInstalled);

        String[][] rows = Status.diagnostics(s, m.appVersion);
        for (int i = 0; i < rows.length; i++) data[i].setSupporting(rows[i][1]);
    }
}
