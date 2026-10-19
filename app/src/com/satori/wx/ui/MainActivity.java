package com.satori.wx.ui;

import android.Manifest;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PersistableBundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.window.BackEvent;
import android.window.OnBackAnimationCallback;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import com.satori.wx.core.Api;
import com.satori.wx.core.Conf;
import com.satori.wx.core.Root;
import com.satori.wx.core.Status;
import com.satori.wx.core.Watcher;
import com.satori.wx.update.Notes;
import com.satori.wx.update.Release;
import com.satori.wx.update.UpdateJob;
import com.satori.wx.update.Updater;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONObject;

/**
 * 知言管理界面的宿主：三层导航、数据刷新、系统集成（安全区、预测性返回、剪贴板、分享）。
 *
 * <p>窄屏是「首页 → 设置 → 添加会话」的层级导航；expanded 宽度且字号允许时首页与设置并列（列表-详情），
 * 添加会话始终是覆盖全窗口的全屏对话框。本应用只看状态、改配置，从不持有服务——服务在微信里。
 *
 * <p>线程：回环探测与联系人读取走 {@link #worker}；root 调用走 {@link #rootWorker}。分开是必须的：
 * {@code su} 最长会占满 20 秒超时，与探测排在一条队列上会把刷新一起堵住。主线程不做任何 I/O。
 *
 * <p>状态是订阅来的，不是隔几秒问来的：前台时开着一条 root 侧的 {@link Watcher}（inotify 盯着微信的 cgroup、
 * 模块发布的状态文件、保活看守与模块目录），再加一个微信安装 / 更新的广播；任何一个说「变了」就重新检查
 * （短暂合并，同一时刻只有一次检查在跑）。只剩一个 60 秒的兜底，防订阅漏掉什么；订阅起不来时才退回
 * 旧的 5 / 10 秒节奏。切到后台全部停掉。
 *
 * <p>微信被冻结又解冻来得很勤，页面不能跟着一闪一闪：同一个进程里有过活应答、此后连不上（冻结或超时）
 * 还不到 {@link #DEBOUNCE} 时，先按上次的样子显示；超过了才说「冻结」，并保留冻结前的账号与发送能力。
 */
public final class MainActivity extends Activity
        implements HomePage.Actions, SettingsPage.Actions {
    private static final int HOME = 0;
    private static final int SETTINGS = 1;
    /** 订阅在时的兜底检查间隔；订阅起不来时退回旧的在线 5 秒、离线 10 秒。 */
    private static final long SAFETY = 60000;
    private static final long POLL_ONLINE = 5000;
    private static final long POLL_OFFLINE = 10000;
    private static final long POLL_RESTARTING = 2000;
    private static final long ROOT_STALE = 15000;
    private static final long RESTART_TIMEOUT = 60000;
    /**
     * 事件来了之后等多久再检查（把一串事件并成一次），以及两次检查之间至少隔多久。事件一直不断时（冻结与解冻
     * 来得很勤）每连着来一个就多等 1 秒、最多 6 秒，免得每个边沿都起一次 su。
     */
    private static final long SETTLE = 300;
    private static final long BURST_WINDOW = 5000;
    private static final long MIN_GAP = 1500;
    /** 从第一次连不上起，不到这么久页面先不改口。 */
    private static final long DEBOUNCE = 6000;

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final ExecutorService rootWorker = Executors.newSingleThreadExecutor();
    private final HomePage.Model model = new HomePage.Model();
    private final Watcher watcher = new Watcher(new Watcher.Listener() {
        @Override public void changed(String line) {
            main.post(MainActivity.this::onChanged);
        }

        @Override public void ended(boolean ran) {
            main.post(() -> onWatchEnded(ran));
        }
    });
    private final BroadcastReceiver packageChanges = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            Uri data = intent.getData();
            String name = data == null ? null : data.getSchemeSpecificPart();
            if (Root.WECHAT.equals(name)) onChanged();
        }
    };

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
    private boolean dirty;
    private boolean settlePending;
    private long lastEventAt;
    private int burst;
    private int guardPending;
    private boolean watchFailed;
    private boolean receiverOn;
    private long refreshedAt;
    // 同一个微信进程里最近一次活应答，及连不上是从什么时候开始的：暂停期间沿用，并给页面防抖。
    private JSONObject carriedStatus, carriedMeta;
    private int carriedPid;
    private long liveAt, failSince;
    private long restartAt;
    private Bundle pendingState;
    private Runnable afterSave;
    private Object backCallback;
    private boolean backRegistered;
    private int insetTop, insetBottom, insetIme, insetLeft, insetRight;
    private final Runnable poll = () -> { if (resumed) refresh(); };
    private Updater updater;
    private boolean awaitingCheck;
    private boolean sawChecking;
    private boolean askedNotifications;
    private long updateProbeAt;
    private final Updater.Listener updateListener = this::onUpdateState;
    private final Runnable settle = new Runnable() {
        @Override public void run() {
            settlePending = false;
            if (!resumed) return;
            long wait = refreshedAt + MIN_GAP - System.currentTimeMillis();
            if (wait > 0) {
                settlePending = true;
                main.postDelayed(this, wait);
            } else {
                refresh();
            }
        }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        t = new Tokens(this);
        Layout layout = new Layout(t, getResources().getConfiguration());
        ui = new Ui(t, layout);
        twoPane = layout.twoPane();
        prefs = getSharedPreferences("zhiyan", MODE_PRIVATE);
        model.appVersion = appVersion();
        updater = Updater.get(this);
        model.autoUpdate = updater.auto();
        UpdateJob.schedule(this, model.autoUpdate);
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
        String updated = updater.takeJust();
        if (!updated.isEmpty()) frame.post(() -> snackbar.show(updated, null, null));
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
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            Insets ime = insets.getInsets(WindowInsets.Type.ime());
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
            backCallback = new OnBackAnimationCallback() {
                private boolean tracking;

                @Override public void onBackStarted(BackEvent event) {
                    tracking = page == SETTINGS && settings != null && !settings.dirty() && Spring.enabled();
                    if (tracking) home.root.setVisibility(View.VISIBLE);
                }

                @Override public void onBackProgressed(BackEvent event) {
                    if (!tracking) return;
                    // M3 预测性返回：页面随手势缩到 90%、向手势方向偏移，露出下层页面。
                    float p = event.getProgress();
                    View view = settings.root;
                    float scale = 1f - 0.1f * p;
                    view.setScaleX(scale);
                    view.setScaleY(scale);
                    float direction = event.getSwipeEdge() == BackEvent.EDGE_LEFT ? 1 : -1;
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
            backCallback = (OnBackInvokedCallback) () -> { if (!handleBack()) finish(); };
        }
        updateBackCallback();
    }

    private void updateBackCallback() {
        if (Build.VERSION.SDK_INT < 33 || backCallback == null) return;
        boolean need = page == SETTINGS || (settings != null && settings.dirty());
        if (need == backRegistered) return;
        backRegistered = need;
        OnBackInvokedDispatcher dispatcher = getOnBackInvokedDispatcher();
        OnBackInvokedCallback callback = (OnBackInvokedCallback) backCallback;
        if (need) dispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback);
        else dispatcher.unregisterOnBackInvokedCallback(callback);
    }

    // ------------------------------------------------------------------ 数据

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        watchFailed = false;
        if (System.currentTimeMillis() - deviceAt > ROOT_STALE) forceRoot = true;
        if (!receiverOn) {
            IntentFilter packages = new IntentFilter();
            packages.addAction(Intent.ACTION_PACKAGE_ADDED);
            packages.addAction(Intent.ACTION_PACKAGE_REPLACED);
            packages.addAction(Intent.ACTION_PACKAGE_REMOVED);
            packages.addDataScheme("package");
            registerReceiver(packageChanges, packages);
            receiverOn = true;
        }
        refresh();
        updater.listen(updateListener);
        if (System.currentTimeMillis() - updateProbeAt > ROOT_STALE) {
            updateProbeAt = System.currentTimeMillis();
            updater.refresh();
        }
        updater.check(false);
    }

    @Override protected void onPause() {
        resumed = false;
        updater.unlisten(updateListener);
        main.removeCallbacks(poll);
        main.removeCallbacks(settle);
        settlePending = false;
        watcher.stop();
        if (receiverOn) {
            unregisterReceiver(packageChanges);
            receiverOn = false;
        }
        if (settings != null) settings.reveal(false);
        super.onPause();
    }

    @Override protected void onDestroy() {
        main.removeCallbacksAndMessages(null);
        watcher.stop();
        worker.shutdownNow();
        rootWorker.shutdownNow();
        super.onDestroy();
    }

    /** 订阅或广播说「变了」：下一次检查要重读 root 侧的事实；一串事件并成一次。 */
    private void onChanged() {
        if (!resumed) return;
        forceRoot = true;
        long now = System.currentTimeMillis();
        burst = now - lastEventAt < BURST_WINDOW ? burst + 1 : 0;
        lastEventAt = now;
        // 固定窗口，不是每来一个就重新计时：事件不断也不会一直不检查。
        if (settlePending) return;
        settlePending = true;
        main.postDelayed(settle, SETTLE + Math.min(burst, 6) * 1000L);
    }

    /** 订阅结束：没有 root 就退回慢速兜底；意外退出（被系统杀了）就再起一次。 */
    private void onWatchEnded(boolean ran) {
        if (!resumed) return;
        if (!ran) {
            watchFailed = true;
            return;
        }
        main.postDelayed(() -> { if (resumed && !watchFailed) watcher.start(); }, 2000);
    }

    /** 一次检查：必要时经 root 重读模块与配置，然后探测服务。结果整体替换，绝不半新半旧。 */
    @Override public void refresh() {
        if (isDestroyed()) return;
        if (model.probing) {
            // 检查在途时又来了新情况：这一次跑完马上再来一次，不丢。
            dirty = true;
            return;
        }
        main.removeCallbacks(poll);
        model.probing = true;
        refreshedAt = System.currentTimeMillis();
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
            int http = Api.UNREACHABLE, failure = Api.FAIL_NONE;
            JSONObject status = null, meta = null;
            boolean viaPrevious = false;
            // 刚探测到微信没在运行或被冻结：服务不可能应答，不必再等满读超时。
            boolean hopeless = needRoot && device.granted && (device.wechatPid <= 0 || device.wechatFrozen);
            if (conf != null && !hopeless) {
                Api.Reply reply = new Api(conf.port, conf.token).status();
                http = reply.code;
                failure = reply.failure;
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
                        failure = Api.FAIL_NONE;
                        status = r.body;
                        Api.Reply m = old.meta();
                        meta = m.ok() ? m.body : null;
                    }
                }
            } else if (hopeless) {
                failure = device.wechatPid <= 0 ? Api.FAIL_REFUSED : Api.FAIL_TIMEOUT;
            }
            final Conf fConf = conf;
            final String fError = confError;
            final int fHttp = http, fFailure = failure;
            final JSONObject fStatus = status, fMeta = meta;
            final boolean fVia = viaPrevious;
            main.post(() -> {
                if (isDestroyed()) return;
                long now = System.currentTimeMillis();
                boolean live = fHttp == 200 && fStatus != null;
                if (live) {
                    carriedStatus = fStatus;
                    carriedMeta = fMeta;
                    carriedPid = device.wechatPid;
                    liveAt = now;
                    failSince = 0;
                } else if (failSince == 0) {
                    failSince = now;
                }
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
                s.now = now;
                s.failure = fFailure;
                s.failSince = failSince;
                s.carriedStatus = carriedStatus;
                s.carriedMeta = carriedMeta;
                s.carriedPid = carriedPid;
                s.published = Status.parsePublished(device.publishedText);
                // 防抖：同一个进程刚有过活应答，这会儿冻结或超时只是一瞬——先按上次的样子显示，
                // 持续到超过 DEBOUNCE 才改口。冻结与解冻来得很勤，页面不能跟着一闪一闪。
                boolean settling = !live && fConf != null && carriedStatus != null && device.wechatPid > 0
                        && device.wechatPid == carriedPid && now - failSince < DEBOUNCE
                        && (device.wechatFrozen || fFailure == Api.FAIL_TIMEOUT);
                if (settling) {
                    s.http = 200;
                    s.status = carriedStatus;
                    s.meta = carriedMeta;
                }
                // 新配置直接应答了：旧端口 / 令牌的记忆没用了。
                if (fConf != null && fHttp == 200 && !fVia) clearPrevious();
                if (restartAt > 0) {
                    boolean back = live && Status.login(s) != null;
                    boolean timeout = now - restartAt > RESTART_TIMEOUT;
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
                if (!resumed) return;
                // root 侧的订阅：能读到 root 才开；起不来就退回慢速兜底，直到回到前台再试。
                if (device.granted && !watchFailed && !watcher.running()) watcher.start();
                boolean pushed = watcher.running();
                long delay = s.restarting ? POLL_RESTARTING : pushed ? SAFETY : (s.http == 200 ? POLL_ONLINE : POLL_OFFLINE);
                // 页面还在防抖：到点要重新看一眼，免得停在「就绪」上。
                if (settling) {
                    delay = Math.min(delay, DEBOUNCE - (now - failSince) + 200);
                    forceRoot = true; // 到点时重读 root 侧的事实，冻着就不必再等 HTTP 超时
                }
                main.postDelayed(poll, delay);
                if (dirty) {
                    dirty = false;
                    onChanged();
                }
            });
        });
    }

    private void applySnapshot(Status.Snapshot s) {
        Status.Snapshot old = model.snapshot;
        boolean first = !old.checked;
        boolean confChanged = old.device == null || old.conf == null != (s.conf == null)
                || (s.conf != null && !s.conf.sameAs(old.conf)) || !Objects.equals(old.confError, s.confError);
        model.snapshot.copyFrom(s);
        updater.setRunning(s.status == null ? "" : s.status.optString("version"));
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
        return new Conf(port, token == null ? Conf.newToken() : token);
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
            case Status.ACTION_GUARD_ON: setGuard(true); break;
            case Status.ACTION_MODULE_ON: setEnabled(true); break;
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
        Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", Root.WECHAT, null));
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

    /**
     * 开关保活看守（模块自带的 wxguard）：开着，微信被系统冻结后会立刻解冻；停着，就只能等系统自己唤醒。
     * 开关显示的是真实模式——动作做完重读一遍，失败时开关自己回到原样。
     */
    @Override public void setGuard(boolean on) {
        // root 那条线是顺序的：别的 root 动作在途时这一个排在后面，不丢。
        guardPending++;
        model.guardBusy = true;
        rootWorker.execute(() -> {
            Root.Result result = Root.setGuard(on);
            main.post(() -> {
                if (isDestroyed()) return;
                model.guardBusy = --guardPending > 0;
                snackbar.show(result.ok() ? (on ? "保活看守已开启" : "保活看守已暂停")
                        : result.granted ? "没能切换保活看守" : "没有 Root 授权", null, null);
                forceRoot = true;
                refresh();
            });
        });
    }

    /**
     * 模块主开关：写微信数据目录里的 enabled 文件，模块里的 gate 线程一秒内读到并停/起服务。
     * 关闭时顺手把 root 看守暂停（否则它还在保活），重新开启时恢复原来的意图。
     */
    @Override public void setEnabled(boolean on) {
        if (model.enabledBusy) return;
        model.enabledBusy = true;
        home.render(model);
        rootWorker.execute(() -> {
            Root.Device before = Root.probe();
            boolean wasArmed = before.granted && before.guardKnown() && !before.guardPaused();
            Root.Result result = Root.setEnabled(on);
            String guardWarning = null;
            if (result.ok()) {
                if (!on) {
                    prefs.edit().putBoolean("guard_resume", wasArmed).apply();
                    if (wasArmed) {
                        Root.Result g = Root.setGuard(false);
                        if (!g.ok()) guardWarning = "保活看守没有暂停";
                    }
                } else if (prefs.getBoolean("guard_resume", false)) {
                    Root.Result g = Root.setGuard(true);
                    if (!g.ok()) guardWarning = "保活看守没有恢复";
                    else prefs.edit().putBoolean("guard_resume", false).apply();
                }
            }
            final boolean ok = result.ok();
            final String warning = guardWarning;
            main.post(() -> {
                if (isDestroyed()) return;
                model.enabledBusy = false;
                if (!ok) snackbar.show(result.granted ? "没能写入模块开关" : "没有 Root 授权", null, null);
                else if (warning != null) snackbar.show((on ? "模块已开启" : "模块已关闭") + "；" + warning, null, null);
                else snackbar.show(on ? "模块已开启" : "模块已关闭", null, null);
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

    // ------------------------------------------------------------------ 更新

    private void onUpdateState(Updater.State state) {
        if (isDestroyed()) return;
        model.update = state;
        home.render(model);
        if (state.checking) sawChecking = true;
        if (awaitingCheck && sawChecking && !state.checking) {
            awaitingCheck = false;
            sawChecking = false;
            if (!state.checkError.isEmpty()) snackbar.show("检查失败：" + state.checkError, null, null);
            else if (state.plan.any() && state.release != null) snackbar.show("发现新版本 " + state.release.headline(), null, null);
            else snackbar.show("已是最新版本", null, null);
        }
        if (state.plan.any() && resumed) askNotifications();
    }

    /** Android 13 起通知要运行时授权。只在第一次发现新版本、且开着自动检查时问一次。 */
    private void askNotifications() {
        if (Build.VERSION.SDK_INT < 33 || askedNotifications || !updater.shouldAskNotifications()) return;
        askedNotifications = true;
        updater.markAsked();
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return;
        requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 1);
    }

    @Override public void updateAction(int action) {
        Updater.State state = updater.state();
        switch (action) {
            case UpdateFaces.UPDATE:
                snackbar.dismiss();
                updater.update();
                break;
            case UpdateFaces.NOTES:
                showNotes(state);
                break;
            case UpdateFaces.OPEN_MODULE:
                openExport(state, true);
                break;
            case UpdateFaces.OPEN_APP:
                openExport(state, false);
                break;
            case UpdateFaces.REBOOT:
                Dialogs.show(ui, "现在重启手机？",
                        "手机会立即重启，微信和其他应用都会关闭。重启后新模块生效，微信里的知言服务会自动恢复。",
                        new Dialogs.Action("取消", Btn.TEXT, null),
                        new Dialogs.Action("重启", Btn.DANGER, () -> {
                            updater.reboot();
                            snackbar.show("正在重启手机…", null, null);
                        }));
                break;
            default:
                break;
        }
    }

    private void showNotes(Updater.State state) {
        Release release = state.release;
        if (release == null) return;
        String notes = Notes.plain(release.notes);
        String message = notes.isEmpty() ? "这次发布没有附更新说明。" : notes;
        String title = "知言 " + release.headline() + " 更新内容";
        if (state.plan.any() && !state.working()) {
            Dialogs.show(ui, title, message,
                    new Dialogs.Action("关闭", Btn.TEXT, null),
                    new Dialogs.Action("更新", Btn.TEXT, () -> updateAction(UpdateFaces.UPDATE)));
        } else {
            Dialogs.show(ui, title, message, new Dialogs.Action("关闭", Btn.TEXT, null));
        }
    }

    private void openExport(Updater.State state, boolean module) {
        Updater.Export target = null;
        for (Updater.Export export : state.exports) if (export.module() == module) target = export;
        if (target == null) return;
        if (!module) {
            Intent permission = updater.installPermissionIntent();
            if (permission != null) {
                snackbar.show("先允许知言安装应用，再回来点一次", null, null);
                try {
                    startActivity(permission);
                } catch (ActivityNotFoundException ignored) {
                    snackbar.show("请在系统设置里允许知言安装应用", null, null);
                }
                return;
            }
        }
        Intent view = updater.viewIntent(target);
        try {
            if (view == null) throw new ActivityNotFoundException();
            startActivity(view);
        } catch (ActivityNotFoundException error) {
            snackbar.show(module ? "没有能打开它的应用，请在 root 管理器的模块页里选择「下载」文件夹里的 " + target.name
                    : "没有能安装它的应用，请在文件管理器里点开「下载」文件夹里的 " + target.name, null, null);
        }
    }

    @Override public void checkUpdate() {
        awaitingCheck = true;
        sawChecking = false;
        updater.check(true);
    }

    @Override public void setAutoUpdate(boolean on) {
        updater.setAuto(on);
        model.autoUpdate = on;
        home.render(model);
        if (on) askNotifications();
        snackbar.show(on ? "已开启自动检查更新" : "已关闭自动检查更新", null, null);
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
