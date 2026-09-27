package com.satori.wx.ui;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PersistableBundle;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import com.satori.wx.core.Api;
import com.satori.wx.core.Conf;
import com.satori.wx.core.Root;
import com.satori.wx.core.Status;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * 知言管理界面的宿主：三层导航、数据刷新、系统集成（安全区、预测性返回、剪贴板、分享）。
 *
 * <p>窄屏是「首页 → 设置 → 添加会话」的层级导航；expanded 宽度且字号允许时首页与设置并列（列表-详情），
 * 添加会话始终是覆盖全窗口的全屏对话框。本应用只看状态、改配置，从不持有服务——服务在微信里。
 *
 * <p>线程：回环探测与联系人读取走 {@link #worker}；root 调用走 {@link #rootWorker}。分开是必须的：
 * {@code su} 最长会占满 20 秒超时，与探测排在一条队列上会把刷新一起堵住。主线程不做任何 I/O。
 * 轮询只在前台进行（在线 5 秒、离线 10 秒、重启微信期间 2 秒），root 状态只在必要时重读。
 */
public final class MainActivity extends Activity
        implements HomePage.Actions, SettingsPage.Actions {
    private static final int HOME = 0;
    private static final int SETTINGS = 1;
    private static final long POLL_ONLINE = 5000;
    private static final long POLL_OFFLINE = 10000;
    private static final long POLL_RESTARTING = 2000;
    private static final long ROOT_STALE = 15000;
    private static final long RESTART_TIMEOUT = 60000;

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final ExecutorService rootWorker = Executors.newSingleThreadExecutor();
    private final HomePage.Model model = new HomePage.Model();

    private Tokens t;
    private Ui ui;
    private FrameLayout frame;
    private FrameLayout stage;
    private HomePage home;
    private SettingsPage settings;
    private Snackbar snackbar;
    private SharedPreferences prefs;
    private boolean twoPane;
    private int page = HOME;
    private boolean resumed;
    private boolean rootBusy;
    private long deviceAt;
    private boolean forceRoot = true;
    private long restartAt;
    private Bundle pendingState;
    private Runnable afterSave;
    private Object backCallback;
    private boolean backRegistered;
    private int insetTop, insetBottom, insetIme, insetLeft, insetRight;
    private final Runnable poll = () -> { if (resumed) refresh(); };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        t = new Tokens(this);
        Layout layout = new Layout(t, getResources().getConfiguration());
        ui = new Ui(t, layout);
        twoPane = layout.twoPane();
        prefs = getSharedPreferences("zhiyan", MODE_PRIVATE);
        model.appVersion = appVersion();
        // 先记下重建前的状态：双栏时设置页在下面就会建好，它要在构造时恢复草稿。
        pendingState = state;

        frame = new FrameLayout(this);
        frame.setBackgroundColor(t.surface);
        home = new HomePage(ui, this, !twoPane);
        if (twoPane) {
            LinearLayout panes = ui.row();
            panes.setGravity(Gravity.FILL_VERTICAL);
            panes.addView(home.root, new LinearLayout.LayoutParams(0, -1, 1));
            panes.addView(ensureSettings().root, new LinearLayout.LayoutParams(0, -1, 1));
            frame.addView(panes, new FrameLayout.LayoutParams(-1, -1));
        } else {
            stage = new FrameLayout(this);
            stage.addView(home.root, new FrameLayout.LayoutParams(-1, -1));
            frame.addView(stage, new FrameLayout.LayoutParams(-1, -1));
        }
        snackbar = new Snackbar(ui, frame);
        frame.setOnApplyWindowInsetsListener(this::onInsets);
        setContentView(frame);
        edgeToEdge();

        if (state != null) {
            page = twoPane ? HOME : state.getInt("page", HOME);
            if (state.containsKey("draft_port") || page == SETTINGS) ensureSettings();
            final int y = state.getInt("home_scroll");
            home.scroll.post(() -> home.scroll.scrollTo(0, y));
        }
        showPage(page, false);
        home.render(model);
        if (Build.VERSION.SDK_INT >= 33) setupBack();
    }

    // ------------------------------------------------------------------ 窗口与安全区

    /** 全屏绘制：系统栏透明，内容自己按安全区留白（targetSdk 35 起系统也强制如此）。 */
    private void edgeToEdge() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                int light = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                        | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                controller.setSystemBarsAppearance(t.dark ? 0 : light, light);
            }
        } else {
            int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION;
            if (!t.dark) flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
            window.getDecorView().setSystemUiVisibility(flags);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        }
    }

    private WindowInsets onInsets(View view, WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            android.graphics.Insets ime = insets.getInsets(WindowInsets.Type.ime());
            insetTop = bars.top;
            insetBottom = bars.bottom;
            insetLeft = bars.left;
            insetRight = bars.right;
            insetIme = ime.bottom;
        } else {
            int bottom = insets.getSystemWindowInsetBottom();
            insetTop = insets.getSystemWindowInsetTop();
            insetLeft = insets.getSystemWindowInsetLeft();
            insetRight = insets.getSystemWindowInsetRight();
            boolean keyboard = bottom > getResources().getDisplayMetrics().heightPixels / 5;
            insetIme = keyboard ? bottom : 0;
            insetBottom = keyboard ? 0 : bottom;
        }
        applyInsets();
        return Build.VERSION.SDK_INT >= 30 ? WindowInsets.CONSUMED : insets.consumeSystemWindowInsets();
    }

    /**
     * 输入法弹出时把页面整体垫高（滚动区变矮，获得焦点的输入框会被滚进可见区域）；
     * 没有输入法时，底部系统栏高度留给滚动内容自己，内容可以画到手势条下方。
     */
    private void applyInsets() {
        int keyboard = insetIme > insetBottom ? insetIme : 0;
        int bars = keyboard > 0 ? 0 : insetBottom;
        frame.setPadding(insetLeft, 0, insetRight, 0);
        home.bar.setTopInset(insetTop);
        home.root.setPadding(0, 0, 0, keyboard);
        home.scroll.setPadding(0, 0, 0, bars);
        if (settings != null) {
            settings.bar.setTopInset(insetTop);
            settings.root.setPadding(0, 0, 0, keyboard);
            settings.setBottomInset(bars);
        }
        if (snackbar != null) snackbar.setBottomInset(keyboard + bars);
    }

    // ------------------------------------------------------------------ 导航

    private SettingsPage ensureSettings() {
        if (settings != null) return settings;
        settings = new SettingsPage(ui, this, twoPane);
        if (!twoPane) {
            settings.root.setVisibility(View.GONE);
            stage.addView(settings.root, new FrameLayout.LayoutParams(-1, -1));
        }
        Status.Snapshot s = model.snapshot;
        if (s.device != null && s.device.granted && s.device.module) settings.load(s.conf, fallbackConf(), true);
        if (pendingState != null && pendingState.containsKey("draft_port")) settings.restoreState(pendingState);
        if (Build.VERSION.SDK_INT >= 28) settings.root.setAccessibilityPaneTitle("设置");
        applyInsets();
        renderSettings();
        return settings;
    }

    private void showPage(int next, boolean animate) {
        page = twoPane ? HOME : next;
        setTitle(page == SETTINGS ? "知言 · 设置" : "知言");
        if (twoPane) {
            updateBackCallback();
            return;
        }
        View in = page == SETTINGS ? ensureSettings().root : home.root;
        View out = page == SETTINGS ? home.root : (settings == null ? null : settings.root);
        if (page == HOME) hideKeyboard();
        if (page == HOME && settings != null) settings.reveal(false);
        updateBackCallback();
        if (out == null || !animate || !Spring.enabled()) {
            in.setVisibility(View.VISIBLE);
            reset(in);
            if (out != null) {
                out.setVisibility(View.GONE);
                reset(out);
            }
            return;
        }
        // M3 共享轴（X）：新页从行进方向滑入并淡入，旧页向反方向让位并淡出。
        float shift = t.dp(48) * (page == SETTINGS ? 1 : -1);
        in.setVisibility(View.VISIBLE);
        in.bringToFront();
        AnimatorSet set = new AnimatorSet();
        set.playTogether(
                spring(ObjectAnimator.ofFloat(in, View.TRANSLATION_X, shift, 0), t.spatialDefault),
                spring(ObjectAnimator.ofFloat(in, View.ALPHA, 0, 1), t.effectsDefault),
                spring(ObjectAnimator.ofFloat(out, View.TRANSLATION_X, out.getTranslationX(), -shift), t.spatialDefault),
                spring(ObjectAnimator.ofFloat(out, View.ALPHA, out.getAlpha(), 0), t.effectsFast));
        set.addListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator animation) {
                out.setVisibility(View.GONE);
                reset(out);
                reset(in);
            }
        });
        set.start();
    }

    private static ObjectAnimator spring(ObjectAnimator animator, Spring spring) {
        spring.apply(animator);
        return animator;
    }

    private static void reset(View view) {
        view.animate().cancel();
        view.setTranslationX(0);
        view.setTranslationY(0);
        view.setAlpha(1);
        view.setScaleX(1);
        view.setScaleY(1);
    }

    /**
     * 返回：全屏对话框先关；设置页有未保存的修改时先问保存、放弃还是留下；设置页回首页。
     * 其余情况交给系统（Android 13+ 由系统播放返回桌面的预测性动画）。
     */
    private boolean handleBack() {
        if (settings != null && settings.dirty() && (page == SETTINGS || twoPane)) {
            confirmLeave(() -> {
                if (page == SETTINGS) showPage(HOME, true);
                else finish();
            });
            return true;
        }
        if (page == SETTINGS) {
            showPage(HOME, true);
            return true;
        }
        return false;
    }

    private void confirmLeave(Runnable proceed) {
        Dialogs.show(ui, "保存这些修改？", "设置还没有保存。保存后，重新启动微信时生效。",
                new Dialogs.Action("取消", Btn.TEXT, null),
                new Dialogs.Action("放弃", Btn.DANGER, () -> {
                    settings.discardNow();
                    proceed.run();
                }),
                new Dialogs.Action("保存", Btn.TEXT, () -> {
                    afterSave = proceed;
                    settings.submit();
                }));
    }

    @Override public void onBackPressed() {
        if (!handleBack()) super.onBackPressed();
    }

    /** Android 13+：只在需要拦截时注册回调，其余时候系统能播放预测性返回动画。 */
    private void setupBack() {
        if (Build.VERSION.SDK_INT >= 34) {
            backCallback = new android.window.OnBackAnimationCallback() {
                private boolean tracking;

                @Override public void onBackStarted(android.window.BackEvent event) {
                    tracking = page == SETTINGS && settings != null && !settings.dirty() && Spring.enabled();
                    if (tracking) home.root.setVisibility(View.VISIBLE);
                }

                @Override public void onBackProgressed(android.window.BackEvent event) {
                    if (!tracking) return;
                    // M3 预测性返回：页面随手势缩到 90%、向手势方向偏移，露出下层页面。
                    float p = event.getProgress();
                    View view = settings.root;
                    float scale = 1f - 0.1f * p;
                    view.setScaleX(scale);
                    view.setScaleY(scale);
                    float direction = event.getSwipeEdge() == android.window.BackEvent.EDGE_LEFT ? 1 : -1;
                    view.setTranslationX(direction * t.dp(24) * p);
                    view.setAlpha(1f - 0.2f * p);
                }

                @Override public void onBackInvoked() {
                    tracking = false;
                    if (!handleBack()) finish();
                }

                @Override public void onBackCancelled() {
                    if (!tracking) return;
                    tracking = false;
                    View view = settings.root;
                    view.animate().scaleX(1).scaleY(1).translationX(0).alpha(1)
                            .setDuration(t.spatialFast.duration).setInterpolator(t.spatialFast)
                            .withEndAction(() -> { if (page == SETTINGS) home.root.setVisibility(View.GONE); })
                            .start();
                }
            };
        } else {
            backCallback = (android.window.OnBackInvokedCallback) () -> { if (!handleBack()) finish(); };
        }
        updateBackCallback();
    }

    private void updateBackCallback() {
        if (Build.VERSION.SDK_INT < 33 || backCallback == null) return;
        boolean need = page == SETTINGS || (settings != null && settings.dirty());
        if (need == backRegistered) return;
        backRegistered = need;
        android.window.OnBackInvokedDispatcher dispatcher = getOnBackInvokedDispatcher();
        android.window.OnBackInvokedCallback callback = (android.window.OnBackInvokedCallback) backCallback;
        if (need) dispatcher.registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback);
        else dispatcher.unregisterOnBackInvokedCallback(callback);
    }

    // ------------------------------------------------------------------ 数据

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        if (System.currentTimeMillis() - deviceAt > ROOT_STALE) forceRoot = true;
        refresh();
    }

    @Override protected void onPause() {
        resumed = false;
        main.removeCallbacks(poll);
        if (settings != null) settings.reveal(false);
        super.onPause();
    }

    @Override protected void onDestroy() {
        main.removeCallbacksAndMessages(null);
        worker.shutdownNow();
        rootWorker.shutdownNow();
        super.onDestroy();
    }

    /** 一次检查：必要时经 root 重读模块与配置，然后探测服务。结果整体替换，绝不半新半旧。 */
    @Override public void refresh() {
        if (model.probing || isDestroyed()) return;
        main.removeCallbacks(poll);
        model.probing = true;
        home.render(model);
        final Status.Snapshot previous = model.snapshot;
        // 没有授权时不自己重试：Magisk 会为每次 su 弹一次授权框。只在「重试」或回到前台时再请求。
        boolean denied = previous.device != null && !previous.device.granted;
        final boolean needRoot = forceRoot || previous.device == null
                || (!denied && previous.http != 200 && System.currentTimeMillis() - deviceAt > ROOT_STALE / 3);
        forceRoot = false;
        final String previousPort = prefs.getString("previous_port", null);
        final String previousToken = prefs.getString("previous_token", null);
        worker.execute(() -> {
            Root.Device device = needRoot ? Root.probe() : previous.device;
            long probedAt = needRoot ? System.currentTimeMillis() : deviceAt;
            Conf conf = null;
            String confError = null;
            if (device.granted && device.module) {
                try {
                    conf = Conf.parse(device.confText);
                } catch (Conf.Invalid invalid) {
                    confError = invalid.getMessage();
                }
            }
            PackageInfo info = wechatInfo();
            int http = Api.UNREACHABLE;
            org.json.JSONObject status = null, meta = null;
            boolean viaPrevious = false;
            // 刚探测到微信没在运行或被冻结：服务不可能应答，不必再等满读超时。
            boolean hopeless = needRoot && device.granted && (device.wechatPid <= 0 || device.wechatFrozen);
            if (conf != null && !hopeless) {
                Api.Reply reply = new Api(conf.port, conf.token).status();
                http = reply.code;
                if (reply.ok()) {
                    status = reply.body;
                    Api.Reply m = new Api(conf.port, conf.token).meta();
                    meta = m.ok() ? m.body : null;
                } else if (previousPort != null && previousToken != null) {
                    int oldPort = parsePort(previousPort);
                    Api old = new Api(oldPort, previousToken);
                    Api.Reply r = old.status();
                    if (r.ok()) {
                        viaPrevious = true;
                        http = 200;
                        status = r.body;
                        Api.Reply m = old.meta();
                        meta = m.ok() ? m.body : null;
                    }
                }
            }
            final Conf fConf = conf;
            final String fError = confError;
            final int fHttp = http;
            final org.json.JSONObject fStatus = status, fMeta = meta;
            final boolean fVia = viaPrevious;
            main.post(() -> {
                if (isDestroyed()) return;
                Status.Snapshot s = new Status.Snapshot();
                s.checked = true;
                s.device = device;
                s.conf = fConf;
                s.confError = fError;
                s.wechatInstalled = info != null;
                s.wechatVersion = info == null || info.versionName == null ? "" : info.versionName;
                s.http = fHttp;
                s.status = fStatus;
                s.meta = fMeta;
                s.viaPrevious = fVia;
                s.port = fConf != null ? fConf.port : previous.port;
                // 新配置直接应答了：旧端口 / 令牌的记忆没用了。
                if (fConf != null && fHttp == 200 && !fVia) clearPrevious();
                if (restartAt > 0) {
                    boolean back = fHttp == 200 && Status.login(s) != null;
                    boolean timeout = System.currentTimeMillis() - restartAt > RESTART_TIMEOUT;
                    if (back || timeout) {
                        restartAt = 0;
                        if (timeout && !back) snackbar.show("微信已重新打开，但服务还没有恢复", null, null);
                    }
                }
                s.restarting = restartAt > 0;
                deviceAt = probedAt;
                applySnapshot(s);
                model.probing = false;
                home.render(model);
                if (resumed) {
                    long delay = s.restarting ? POLL_RESTARTING : (s.http == 200 ? POLL_ONLINE : POLL_OFFLINE);
                    main.postDelayed(poll, delay);
                }
            });
        });
    }

    private void applySnapshot(Status.Snapshot s) {
        Status.Snapshot old = model.snapshot;
        boolean first = !old.checked;
        boolean confChanged = old.device == null || old.conf == null != (s.conf == null)
                || (s.conf != null && !s.conf.sameAs(old.conf)) || !java.util.Objects.equals(old.confError, s.confError);
        model.snapshot.copyFrom(s);
        if (settings != null && s.device != null && s.device.granted && s.device.module && (confChanged || first)) {
            settings.load(s.conf, fallbackConf(), false);
        }
        renderSettings();
    }


    /** 配置文件无效时给设置页填表用：尽量保留文件里还能认出的端口与令牌，认不出的用默认值。 */
    private Conf fallbackConf() {
        Status.Snapshot s = model.snapshot;
        if (s.conf != null) return s.conf;
        int port = Conf.DEFAULT_PORT;
        String token = null;
        String text = s.device == null ? null : s.device.confText;
        if (text != null) {
            for (String line : text.split("\n")) {
                String l = line.trim();
                if (l.startsWith("token=") && Conf.tokenValid(l.substring(6)) && token == null) token = l.substring(6);
                if (l.startsWith("port=")) port = parsePort(l.substring(5));
            }
        }
        return new Conf(port, token == null ? Conf.newToken() : token, false);
    }

    private static int parsePort(String value) {
        try {
            int port = Integer.parseInt(value.trim());
            return port >= Conf.MIN_PORT && port <= Conf.MAX_PORT ? port : Conf.DEFAULT_PORT;
        } catch (NumberFormatException e) {
            return Conf.DEFAULT_PORT;
        }
    }

    private void renderSettings() {
        if (settings == null) return;
        settings.notice(Status.applied(model.snapshot));
    }

    private PackageInfo wechatInfo() {
        try {
            return getPackageManager().getPackageInfo(Root.WECHAT, 0);
        } catch (Exception missing) {
            return null;
        }
    }

    // ------------------------------------------------------------------ 首页动作

    @Override public void openSettings() {
        if (twoPane) {
            ensureSettings().scroll.requestFocus();
            return;
        }
        showPage(SETTINGS, true);
    }

    @Override public void heroAction(int action) {
        switch (action) {
            case Status.ACTION_RETRY_ROOT:
                forceRoot = true;
                refresh();
                break;
            case Status.ACTION_OPEN_WECHAT: openWeChat(); break;
            case Status.ACTION_RESTART_WECHAT: restartWeChat(); break;
            case Status.ACTION_FIX_CONFIG: openSettings(); break;
            case Status.ACTION_WECHAT_SETTINGS: openWeChatSettings(); break;
            default: break;
        }
    }

    @Override public void openWeChat() {
        Intent intent = getPackageManager().getLaunchIntentForPackage(Root.WECHAT);
        if (intent == null) {
            snackbar.show("没有找到微信", null, null);
            return;
        }
        try {
            startActivity(intent);
            forceRoot = true;
        } catch (Exception error) {
            snackbar.show("无法打开微信，请从桌面打开", null, null);
        }
    }

    /**
     * 微信的系统应用详情页：用户在那里自己决定是否允许微信后台运行（ColorOS 的「耗电管理」就在其中）。
     * 走的是系统公开的设置入口，知言不替用户改任何系统策略。
     */
    @Override public void openWeChatSettings() {
        Intent intent = new Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                android.net.Uri.fromParts("package", Root.WECHAT, null));
        try {
            startActivity(intent);
        } catch (Exception error) {
            snackbar.show("无法打开系统设置，请在「设置 → 应用」里找到微信", null, null);
        }
    }

    @Override public void restartWeChat() {
        Dialogs.show(ui, "重新启动微信？",
                "微信会被关闭并立即重新打开，新设置随之生效。连接知言的客户端会断开，服务恢复后自动重连。",
                new Dialogs.Action("取消", Btn.TEXT, null),
                new Dialogs.Action("重新启动", Btn.TEXT, this::doRestart));
    }

    private void doRestart() {
        if (rootBusy) return;
        rootBusy = true;
        restartAt = System.currentTimeMillis();
        model.snapshot.restarting = true;
        home.render(model);
        rootWorker.execute(() -> {
            Root.Result result = Root.restartWeChat();
            main.post(() -> {
                if (isDestroyed()) return;
                rootBusy = false;
                if (!result.granted) {
                    restartAt = 0;
                    model.snapshot.restarting = false;
                    snackbar.show("没有 Root 授权，无法重新启动微信", null, null);
                }
                forceRoot = true;
                main.postDelayed(this::refresh, 1500);
            });
        });
    }

    @Override public void copyEndpoint() {
        copy("服务地址", Status.endpoint(model.snapshot), false);
    }

    @Override public void copyToken() {
        Conf conf = model.snapshot.conf;
        if (conf == null) {
            snackbar.show(model.snapshot.device == null ? "还在读取令牌，请稍候" : "读不到令牌：需要 Root 与有效的配置", null, null);
            return;
        }
        copy("令牌", conf.token, true);
    }

    @Override public void copyReport() {
        copy("诊断报告", Status.report(model.snapshot, model.appVersion, System.currentTimeMillis()), false);
    }

    @Override public void shareReport() {
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                .putExtra(Intent.EXTRA_SUBJECT, "知言诊断报告")
                .putExtra(Intent.EXTRA_TEXT, Status.report(model.snapshot, model.appVersion, System.currentTimeMillis()));
        try {
            startActivity(Intent.createChooser(send, "分享诊断报告"));
        } catch (Exception error) {
            snackbar.show("没有可以分享的应用，请改用复制", null, null);
        }
    }

    @Override public void enableSend() {
        openSettings();
        snackbar.show("打开「允许客户端发送消息」，保存后重新启动微信", null, null);
    }

    // ------------------------------------------------------------------ 设置页动作

    @Override public void back() {
        handleBack();
    }

    @Override public void save(Conf value) {
        final Conf before = model.snapshot.conf;
        final String text;
        try {
            text = value.write();
        } catch (Conf.Invalid invalid) {
            settings.saved(value, false);
            snackbar.show(invalid.getMessage(), null, null);
            return;
        }
        rootWorker.execute(() -> {
            Root.Result result = Root.writeConf(text);
            main.post(() -> {
                if (isDestroyed()) return;
                boolean ok = result.ok();
                if (ok && before != null && (before.port != value.port || !before.token.equals(value.token))
                        && prefs.getString("previous_port", null) == null && model.snapshot.http == 200
                        && !model.snapshot.viaPrevious) {
                    // 记住仍在运行的旧端口与令牌：重启微信之前，状态探测还能从旧入口连上服务。
                    prefs.edit().putString("previous_port", String.valueOf(before.port))
                            .putString("previous_token", before.token).apply();
                }
                settings.saved(value, ok);
                hideKeyboard();
                Runnable next = afterSave;
                afterSave = null;
                if (!ok) {
                    snackbar.show(result.granted ? "保存失败：配置没有写进模块目录" : "保存失败：没有 Root 授权", null, null);
                    return;
                }
                boolean running = model.snapshot.http == 200 || (model.snapshot.device != null && model.snapshot.device.wechatPid > 0);
                snackbar.show("已保存，重新启动微信后生效", running ? "重新启动" : null, running ? this::restartWeChat : null);
                forceRoot = true;
                refresh();
                if (next != null) next.run();
            });
        });
    }

    private void clearPrevious() {
        if (prefs.contains("previous_port")) prefs.edit().remove("previous_port").remove("previous_token").apply();
    }

    @Override public void discarded() {
        snackbar.show("已恢复为上次保存的设置", null, null);
    }

    @Override public void copyToken(String token) {
        copy("令牌", token, true);
    }

    @Override public void secure(boolean on) {
        if (on) getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_SECURE);
    }

    @Override public void message(String text, String action, Runnable run) {
        snackbar.show(text, action, run);
    }

    @Override public void draftChanged(boolean dirty) {
        updateBackCallback();
    }

    // ------------------------------------------------------------------ 实例状态

    @Override protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("page", page);
        out.putInt("home_scroll", home.scroll.getScrollY());
        if (settings != null && settings.dirty()) settings.saveState(out);
    }

    // ------------------------------------------------------------------ 工具

    /**
     * 复制到剪贴板。Android 13 起系统自己会提示复制结果，这时不再重复提示；
     * 令牌标记为敏感内容，系统的剪贴板预览里不显示明文。
     */
    private void copy(String label, String value, boolean sensitive) {
        ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (clipboard == null) return;
        ClipData clip = ClipData.newPlainText(label, value);
        if (sensitive) {
            PersistableBundle extras = new PersistableBundle();
            extras.putBoolean("android.content.extra.IS_SENSITIVE", true);
            clip.getDescription().setExtras(extras);
        }
        clipboard.setPrimaryClip(clip);
        if (Build.VERSION.SDK_INT < 33) snackbar.show("已复制" + label, null, null);
    }

    private void hideKeyboard() {
        View focus = getCurrentFocus();
        if (focus == null) return;
        InputMethodManager keyboard = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (keyboard != null) keyboard.hideSoftInputFromWindow(focus.getWindowToken(), 0);
        focus.clearFocus();
    }

    private String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception error) {
            return "";
        }
    }
}
