package com.satori.wx.ui;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Bundle;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.satori.wx.R;
import com.satori.wx.core.Conf;
import com.satori.wx.core.Root;
import com.satori.wx.core.Status;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
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
            home("light-home-ready", false, 1f, 360, ready());
            home("dark-home-ready", true, 1f, 360, ready());
            home("light-home-service-down", false, 1f, 360, serviceDown());
            home("dark-home-no-root", true, 1f, 360, noRoot());
            home("light-home-checking", false, 1f, 360, new Status.Snapshot());
            home("large-home-ready", false, 2f, 360, ready());
            home("narrow-home-config-bad", false, 1.3f, 320, configBad());
            home("light-home-frozen", false, 1f, 360, frozen());
            home("dark-home-account-unseen", true, 1f, 360, accountUnseen());
            // ---- 设置：有草稿、无草稿 ----
            settings("light-settings", false, 1f, 360, true);
            settings("dark-settings", true, 1f, 360, true);
            settings("large-settings", false, 2f, 360, true);
            // ---- 宽屏双栏 ----
            wide("wide", false, 900);
        } catch (Exception error) {
            throw new RuntimeException(error);
        }
    }

    // ------------------------------------------------------------------ 状态样本

    private static Status.Snapshot ready() throws Exception {
        Status.Snapshot s = new Status.Snapshot();
        s.checked = true;
        s.device = Root.parse("module=1\nversion=v0.7.0\npid=6312\n");
        s.conf = new Conf(5601, TOKEN);
        s.wechatVersion = "8.0.78";
        s.http = 200;
        s.port = 5601;
        s.status = new JSONObject("{\"version\":\"0.7.0\",\"standard_methods\":37,\"event_replay\":true,\"replay_capacity\":64,"
                + "\"send\":{\"ready\":true,\"resolved\":true,\"dispatcher\":true,\"sent\":128,\"failed\":1,\"rejected\":3,\"recalled\":2},"
                + "\"keepalive\":{\"notification\":true,\"wakelock\":true}}");
        s.meta = new JSONObject("{\"logins\":[{\"sn\":1,\"status\":1,\"features\":[\"message.get\",\"message.list\",\"user.get\","
                + "\"friend.list\",\"guild.get\",\"guild.list\",\"message.create\",\"message.delete\"],"
                + "\"user\":{\"id\":\"wxid_example0001\",\"nick\":\"知言示例\",\"name\":\"zhiyan\"}}]}");
        return s;
    }

    private static Status.Snapshot serviceDown() {
        Status.Snapshot s = new Status.Snapshot();
        s.checked = true;
        s.device = Root.parse("module=1\nversion=v0.7.0\npid=6312\n");
        s.conf = new Conf(5601, TOKEN);
        s.wechatVersion = "8.0.78";
        return s;
    }

    private static Status.Snapshot frozen() {
        Status.Snapshot s = serviceDown();
        s.device = Root.parse("module=1\nversion=v0.7.0\npid=6312\nfrozen=1\nauth=1114861342\n");
        return s;
    }

    private static Status.Snapshot accountUnseen() throws Exception {
        Status.Snapshot s = ready();
        s.device = Root.parse("module=1\nversion=v0.7.0\npid=6312\nauth=1114861342\n");
        s.meta = new JSONObject("{\"logins\":[]}");
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
        s.device = Root.parse("module=1\nversion=v0.7.0\npid=0\n");
        s.confError = "无法识别的设置「sned」";
        return s;
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
        model.snapshot.copyFrom(s);
        page.render(model);
        shoot(name, page.root, ui.t, widthDp, 0);
        check(name, page.root, ui.t);
        String title = ((TextView) page.root.findViewById(R.id.hero_title)).getText().toString();
        if (!title.equals(Status.hero(s).title)) failures.add(name + ": 状态标题未渲染（" + title + "）");
        boolean secondary = page.root.findViewById(R.id.hero_secondary).getVisibility() == View.VISIBLE;
        if (secondary != (Status.hero(s).secondary != Status.ACTION_NONE)) failures.add(name + ": 次要动作显隐不对");
    }

    private SettingsPage settingsPage(Ui ui, boolean dirty) throws Exception {
        SettingsPage page = new SettingsPage(ui, stub(SettingsPage.Actions.class), false);
        Status.Snapshot s = ready();
        page.load(s.conf, s.conf, true);
        if (dirty) {
            Bundle draft = new Bundle();
            draft.putString("draft_port", "5602");
            draft.putString("draft_token", TOKEN);
            page.restoreState(draft);
            page.notice(Status.applied(s));
        }
        if (dirty && !page.dirty()) failures.add("改了端口后设置页应处于有草稿状态");
        return page;
    }

    private void settings(String name, boolean dark, float font, int widthDp, boolean dirty) throws Exception {
        Ui ui = ui(dark, font, widthDp);
        SettingsPage page = settingsPage(ui, dirty);
        shoot(name, page.root, ui.t, widthDp, 0);
        check(name, page.root, ui.t);
        if (page.root.findViewById(R.id.save_bar).getVisibility() != View.VISIBLE) failures.add(name + ": 有草稿时保存栏应出现");
    }

    private void wide(String name, boolean dark, int widthDp) throws Exception {
        Ui ui = ui(dark, 1f, widthDp);
        if (!ui.layout.twoPane()) failures.add(name + ": " + widthDp + "dp 应为双栏");
        HomePage home = new HomePage(ui, stub(HomePage.Actions.class), false);
        HomePage.Model model = new HomePage.Model();
        model.snapshot.copyFrom(ready());
        home.render(model);
        SettingsPage settings = new SettingsPage(ui, stub(SettingsPage.Actions.class), true);
        Status.Snapshot s = ready();
        settings.load(s.conf, s.conf, true);
        settings.notice(Status.applied(s));
        LinearLayout panes = ui.row();
        panes.setGravity(Gravity.FILL_VERTICAL);
        panes.addView(home.root, new LinearLayout.LayoutParams(0, -1, 1));
        panes.addView(settings.root, new LinearLayout.LayoutParams(0, -1, 1));
        shoot(name, panes, ui.t, widthDp, 900);
        check(name, panes, ui.t);
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
                && !(view instanceof EditText)) {
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
