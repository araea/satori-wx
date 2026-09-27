package com.satori.wx.ui;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Build;
import android.os.Bundle;


import android.view.ContextThemeWrapper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.satori.wx.R;
import com.satori.wx.core.Api;
import com.satori.wx.core.Conf;
import com.satori.wx.core.Root;
import com.satori.wx.core.Status;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONObject;

/**
 * 知言界面的真机设计冒烟：在真实的 Android 框架里离屏构造各页面、注入各种状态，检查
 * 可触达面积 ≥ 48dp、图标按钮有名字、读屏可见的文字没有被截断，并输出长页截图供人工复核。
 *
 * <p>不连服务、不调 su、不改配置：页面直接用注入的 {@link Status.Snapshot} 渲染。测试类与应用同包、
 * 同一个类加载器（instrumentation 会把两个 APK 并进目标进程的类加载器），所以可以直接调包内接口。
 * 离屏渲染绕开了锁屏时 Activity 立刻被停、截图拿到陈旧布局的问题。
 */
public final class DesignSmoke extends Instrumentation {
    private static final String TOKEN = "3f9c0d1e2b4a59687766554433221100ffeeddccbbaa99887766554433221100";
    private final StringBuilder report = new StringBuilder();
    private final List<String> failures = new ArrayList<>();
    private File shots;

    @Override public void onCreate(Bundle args) {
        super.onCreate(args);
        start();
    }

    @Override public void onStart() {
        Bundle out = new Bundle();
        try {
            shots = new File(getTargetContext().getFilesDir(), "design-review");
            shots.mkdirs();
            for (File old : shots.listFiles()) old.delete();
            runOnMainSync(this::run);
            if (!failures.isEmpty()) throw new AssertionError(String.join("\n", failures));
            report.append("PASS: 知言界面设计冒烟\n");
            out.putString(Instrumentation.REPORT_KEY_STREAMRESULT, report.toString());
            finish(Activity.RESULT_OK, out);
        } catch (Throwable error) {
            report.append("FAIL: ").append(error).append('\n');
            for (StackTraceElement e : error.getStackTrace()) report.append("    at ").append(e).append('\n');
            out.putString(Instrumentation.REPORT_KEY_STREAMRESULT, report.toString());
            finish(Activity.RESULT_CANCELED, out);
        }
    }

    private void run() {
        try {
            // ---- 首页：各种结论 ----
            home("light-home-ready", false, 1f, 360, ready(true));
            home("dark-home-ready", true, 1f, 360, ready(true));
            home("light-home-service-down", false, 1f, 360, serviceDown());
            home("dark-home-no-root", true, 1f, 360, noRoot());
            home("light-home-checking", false, 1f, 360, new Status.Snapshot());
            home("large-home-ready", false, 2f, 360, ready(true));
            home("narrow-home-config-bad", false, 1.3f, 320, configBad());
            // ---- 设置：有草稿、白名单有名字 ----
            settings("light-settings", false, 1f, 360, true);
            settings("dark-settings", true, 1f, 360, true);
            settings("large-settings", false, 2f, 360, true);
            // ---- 添加会话 ----
            picker("light-picker", false, 1f, 360, false);
            picker("dark-picker-error", true, 1f, 360, true);
            // ---- 宽屏双栏 ----
            wide("wide", false, 900);
        } catch (Exception error) {
            throw new RuntimeException(error);
        }
    }

    // ------------------------------------------------------------------ 状态样本

    private static Status.Snapshot ready(boolean blocked) throws Exception {
        Status.Snapshot s = new Status.Snapshot();
        s.checked = true;
        s.device = Root.parse("module=1\nversion=v0.6.7\npid=6312\n");
        s.conf = new Conf(5601, TOKEN, true, Arrays.asList("filehelper", "39179319508@chatroom"));
        s.wechatVersion = "8.0.78";
        s.http = 200;
        s.port = 5601;
        s.status = new JSONObject("{\"version\":\"0.6.7\",\"standard_methods\":37,\"event_replay\":true,\"replay_capacity\":64,"
                + "\"send\":{\"enabled\":true,\"allow\":\"filehelper;39179319508@chatroom\",\"sent\":128,\"failed\":1,\"rejected\":3,\"recalled\":2}}");
        if (blocked) {
            s.status.getJSONObject("send").put("last_age_ms", 7 * 60_000).put("last_ok", false)
                    .put("last_error", "target not in send_allow").put("last_target", "38992867588@chatroom");
        }
        s.meta = new JSONObject("{\"logins\":[{\"sn\":1,\"status\":1,\"features\":[\"message.get\",\"message.list\",\"user.get\","
                + "\"friend.list\",\"guild.get\",\"guild.list\",\"message.create\",\"message.delete\"],"
                + "\"user\":{\"id\":\"wxid_example0001\",\"nick\":\"知言示例\",\"name\":\"zhiyan\"}}]}");
        return s;
    }

    private static Status.Snapshot serviceDown() {
        Status.Snapshot s = new Status.Snapshot();
        s.checked = true;
        s.device = Root.parse("module=1\nversion=v0.6.7\npid=6312\n");
        s.conf = new Conf(5601, TOKEN, false, new ArrayList<>());
        s.wechatVersion = "8.0.78";
        return s;
    }

    private static Status.Snapshot noRoot() {
        Status.Snapshot s = new Status.Snapshot();
        s.checked = true;
        s.device = Root.Device.DENIED;
        return s;
    }

    private static Status.Snapshot configBad() {
        Status.Snapshot s = new Status.Snapshot();
        s.checked = true;
        s.device = Root.parse("module=1\nversion=v0.6.7\npid=0\n");
        s.confError = "无法识别的设置「sned」";
        return s;
    }

    private static List<Api.Contact> contacts() {
        List<Api.Contact> list = new ArrayList<>();
        list.add(new Api.Contact("39179319508@chatroom", "417", "", 1));
        list.add(new Api.Contact("38992867588@chatroom", "远天乐海宿舍", "", 1));
        list.add(new Api.Contact("59012484892@chatroom", "七个葫芦娃", "", 1));
        list.add(new Api.Contact("wxid_example0002", "陈师傅", "渡一场风", 0));
        list.add(new Api.Contact("wxid_example0003", "ALICE", "LMMALICE", 0));
        list.add(new Api.Contact("wxid_example0004", "奶奶", "闲不住", 0));
        list.add(new Api.Contact("weixin", "微信团队", "", 0));
        return list;
    }

    // ------------------------------------------------------------------ 页面

    private Ui ui(boolean dark, float fontScale, int widthDp) {
        Context base = getTargetContext();
        Configuration config = new Configuration(base.getResources().getConfiguration());
        config.uiMode = (config.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
                | (dark ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO);
        config.fontScale = fontScale;
        config.screenWidthDp = widthDp;
        Context context = new ContextThemeWrapper(base.createConfigurationContext(config), R.style.AppTheme);
        Tokens t = new Tokens(context);
        return new Ui(t, new Layout(t, config));
    }

    @SuppressWarnings("unchecked")
    private static <T> T stub(Class<T> type) {
        return (T) Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type}, (proxy, method, args) -> {
            Class<?> r = method.getReturnType();
            if (r == boolean.class) return false;
            if (r == int.class) return 0;
            return null;
        });
    }

    private void home(String name, boolean dark, float font, int widthDp, Status.Snapshot s) throws Exception {
        Ui ui = ui(dark, font, widthDp);
        HomePage page = new HomePage(ui, stub(HomePage.Actions.class), true);
        HomePage.Model model = new HomePage.Model();
        model.appVersion = "1.0.0";
        copy(s, model.snapshot);
        Map<String, String> names = new HashMap<>();
        for (Api.Contact c : contacts()) names.put(c.id, c.name);
        model.names = names;
        page.render(model);
        shoot(name, page.root, ui.t, widthDp, 0);
        check(name, page.root, ui.t);
        String title = ((TextView) page.root.findViewById(R.id.hero_title)).getText().toString();
        if (!title.equals(Status.hero(s).title)) failures.add(name + ": 状态标题未渲染（" + title + "）");
    }

    private SettingsPage settingsPage(Ui ui, boolean dirty) throws Exception {
        SettingsPage page = new SettingsPage(ui, stub(SettingsPage.Actions.class), false);
        Status.Snapshot s = ready(false);
        page.load(s.conf, s.conf, true);
        Map<String, Api.Contact> map = new HashMap<>();
        for (Api.Contact c : contacts()) map.put(c.id, c);
        page.contacts(map);
        page.notice(Status.applied(s));
        if (dirty) {
            page.add(Arrays.asList("wxid_example0002"));
            Status.Snapshot pending = ready(false);
            pending.conf = new Conf(5601, TOKEN, true, Arrays.asList("filehelper", "39179319508@chatroom", "wxid_example0003"));
            page.notice(Status.applied(pending));
        }
        if (dirty && !page.dirty()) failures.add("加入会话后设置页应处于有草稿状态");
        return page;
    }

    private void settings(String name, boolean dark, float font, int widthDp, boolean dirty) throws Exception {
        Ui ui = ui(dark, font, widthDp);
        SettingsPage page = settingsPage(ui, dirty);
        shoot(name, page.root, ui.t, widthDp, 0);
        check(name, page.root, ui.t);
        if (page.root.findViewById(R.id.save_bar).getVisibility() != View.VISIBLE) failures.add(name + ": 有草稿时保存栏应出现");
    }

    private void picker(String name, boolean dark, float font, int widthDp, boolean failed) throws Exception {
        Ui ui = ui(dark, font, widthDp);
        PickerPage page = new PickerPage(ui, stub(PickerPage.Actions.class));
        page.open(Arrays.asList("39179319508@chatroom"));
        page.contacts(failed ? null : contacts(), failed ? "知言服务没有返回联系人" : null);
        shoot(name, page.root, ui.t, widthDp, 780);
        check(name, page.root, ui.t);
    }

    private void wide(String name, boolean dark, int widthDp) throws Exception {
        Ui ui = ui(dark, 1f, widthDp);
        if (!ui.layout.twoPane()) failures.add(name + ": " + widthDp + "dp 应为双栏");
        HomePage home = new HomePage(ui, stub(HomePage.Actions.class), false);
        HomePage.Model model = new HomePage.Model();
        copy(ready(false), model.snapshot);
        home.render(model);
        SettingsPage settings = new SettingsPage(ui, stub(SettingsPage.Actions.class), true);
        Status.Snapshot s = ready(false);
        settings.load(s.conf, s.conf, true);
        settings.notice(Status.applied(s));
        LinearLayout panes = ui.row();
        panes.setGravity(android.view.Gravity.FILL_VERTICAL);
        panes.addView(home.root, new LinearLayout.LayoutParams(0, -1, 1));
        panes.addView(settings.root, new LinearLayout.LayoutParams(0, -1, 1));
        shoot(name, panes, ui.t, widthDp, 900);
        check(name, panes, ui.t);
    }

    private static void copy(Status.Snapshot from, Status.Snapshot to) {
        to.checked = from.checked;
        to.restarting = from.restarting;
        to.device = from.device;
        to.conf = from.conf;
        to.confError = from.confError;
        to.wechatInstalled = from.wechatInstalled;
        to.wechatVersion = from.wechatVersion;
        to.http = from.http;
        to.status = from.status;
        to.meta = from.meta;
        to.viaPrevious = from.viaPrevious;
        to.port = from.port;
    }

    // ------------------------------------------------------------------ 渲染与检查

    /** 离屏测量与排版：heightDp 为 0 时取整页高度（长图），否则固定一屏。 */
    private void layout(View root, Tokens t, int widthDp, int heightDp) {
        int width = t.dp(widthDp);
        int heightSpec = heightDp == 0 ? View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED)
                : View.MeasureSpec.makeMeasureSpec(t.dp(heightDp), View.MeasureSpec.EXACTLY);
        root.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), heightSpec);
        root.layout(0, 0, root.getMeasuredWidth(), root.getMeasuredHeight());
        // ListView 在第二遍排版后才把行填满。
        root.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), heightSpec);
        root.layout(0, 0, root.getMeasuredWidth(), root.getMeasuredHeight());
    }

    private void shoot(String name, View root, Tokens t, int widthDp, int heightDp) throws Exception {
        layout(root, t, widthDp, heightDp);
        Bitmap bitmap = Bitmap.createBitmap(root.getWidth(), Math.max(1, root.getHeight()), Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(t.surface);
        root.draw(canvas);
        try (FileOutputStream out = new FileOutputStream(new File(shots, name + ".png"))) {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
        }
        bitmap.recycle();
        report.append("   shot ").append(name).append(" ").append(root.getWidth()).append('×').append(root.getHeight()).append('\n');
    }

    private void check(String name, View root, Tokens t) {
        walk(name, root, t);
    }

    private void walk(String name, View view, Tokens t) {
        if (view.getVisibility() != View.VISIBLE) return;
        int min = t.touchTarget - 1;
        if (view.isClickable() && view.isEnabled() && view.getWidth() > 0
                && (view.getWidth() < min || view.getHeight() < min)) {
            failures.add(name + ": 可点控件小于 48dp：" + describe(view) + " " + view.getWidth() + "×" + view.getHeight());
        }
        if (view instanceof ImageButton && (view.getContentDescription() == null || view.getContentDescription().length() == 0)) {
            failures.add(name + ": 图标按钮没有名字：" + describe(view));
        }
        if (view instanceof TextView && view.getImportantForAccessibility() != View.IMPORTANT_FOR_ACCESSIBILITY_NO
                && !(view instanceof android.widget.EditText)) {
            android.text.Layout layout = ((TextView) view).getLayout();
            for (int i = 0; layout != null && i < layout.getLineCount(); i++) {
                if (layout.getEllipsisCount(i) > 0) {
                    failures.add(name + ": 文字被截断：" + ((TextView) view).getText());
                    break;
                }
            }
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) walk(name, group.getChildAt(i), t);
        }
    }

    private static String describe(View view) {
        String id = view.getId() == View.NO_ID ? "" : "#" + view.getResources().getResourceEntryName(view.getId());
        CharSequence label = view.getContentDescription();
        if (label == null && view instanceof TextView) label = ((TextView) view).getText();
        return view.getClass().getSimpleName() + id + (label == null ? "" : "「" + label + "」");
    }
}
