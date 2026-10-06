package com.satori.wx.ui;

import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.text.method.PasswordTransformationMethod;
import android.view.Gravity;
import android.view.View;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import com.satori.wx.R;
import com.satori.wx.core.Conf;
import com.satori.wx.core.Status;

/**
 * 设置：端口与令牌。保存只写配置文件，微信下次启动时读取。
 *
 * <p>交互约定（取自 HIG，外观全部是 M3E）：草稿跨页面切换与实例重建保留；有改动才出现底部工具栏；
 * 令牌默认隐藏，显示期间禁止截屏；校验在保存时做，出错的字段就地显示原因并获得焦点。
 */
final class SettingsPage {
    interface Actions {
        void back();
        void save(Conf value);
        void discarded();
        void restartWeChat();
        void copyToken(String token);
        void secure(boolean on);
        void message(String text, String action, Runnable run);
        /** 草稿从「无改动」变成「有改动」或反过来：宿主据此决定是否拦截返回。 */
        void draftChanged(boolean dirty);
    }

    final LinearLayout root;
    final ScrollView scroll;
    final TopBar bar;
    final Notice notice;
    private final Ui ui;
    private final Tokens t;
    private final Actions actions;
    private final Field port, token;
    private final ImageButton reveal;
    private final LinearLayout saveBar;
    private final TextView saveState;
    private final Btn save, discard;

    /** 已保存的配置（基准）；文件无效或尚未读到时为 null。 */
    private Conf saved;
    private boolean saving, revealing, loading, lastDirty, baseline;
    private int bottomInset;

    SettingsPage(Ui ui, Actions actions, boolean pane) {
        this.ui = ui;
        this.t = ui.t;
        this.actions = actions;
        int base = pane ? t.surfaceContainerLow : t.surface;

        root = ui.column();
        root.setId(R.id.settings);
        root.setBackgroundColor(base);
        bar = new TopBar(ui, "设置", base, t.surfaceContainer);
        if (!pane) {
            ImageButton up = ui.iconButton(Icon.BACK, "返回", t.onSurface);
            up.setId(R.id.navigate_up);
            up.setOnClickListener(v -> actions.back());
            bar.navigation(up);
        }
        root.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        scroll = Pages.scroll(t);
        LinearLayout content = Pages.content(ui);
        scroll.addView(Pages.center(ui, content));
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));

        TextView title = ui.heading("设置", Tokens.DISPLAY_SMALL, t.onSurface);
        content.addView(title, Ui.stack(t.spaceSm));
        content.addView(ui.text("令牌与端口。保存后，重新启动微信时生效。",
                Tokens.BODY_LARGE, t.onSurfaceVariant), Ui.stack(t.spaceXs));
        bar.follow(scroll, title);

        notice = new Notice(t);
        notice.setId(R.id.config_notice);
        notice.action("重新启动微信", v -> actions.restartWeChat());
        content.addView(notice, Ui.stack(t.spaceXl));

        // ---- 连接 ----
        content.addView(ui.sectionTitle("连接"), Ui.stack(t.space2xl));
        port = new Field(t, "本机端口", "5601", false);
        port.input.setId(R.id.port);
        port.input.setInputType(InputType.TYPE_CLASS_NUMBER);
        port.input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(5)});
        port.hint("1024–65535。改了端口，客户端里的地址也要一起改。");
        content.addView(port, Ui.stack(t.spaceXs));
        token = new Field(t, "令牌", null, true);
        token.input.setId(R.id.token);
        token.input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(Conf.MAX_TOKEN)});
        token.hint("32–128 位英文字母、数字、- 或 _。客户端必须填写相同的令牌。");
        reveal = ui.iconButton(Icon.SHOW, "显示令牌", t.onSurfaceVariant);
        reveal.setId(R.id.token_reveal);
        reveal.setOnClickListener(v -> reveal(!revealing));
        token.trailing(reveal);
        content.addView(token, Ui.stack(t.spaceLg));
        Btn generate = ui.button("生成新令牌", Btn.TONAL).icon(Icon.KEY);
        generate.setId(R.id.token_generate);
        generate.setOnClickListener(v -> {
            token.setText(Conf.newToken());
            token.error(null);
            actions.message("已生成新令牌。保存并重启微信后，客户端也要换成它。", null, null);
        });
        Btn copy = ui.button("复制令牌", Btn.TONAL).icon(Icon.COPY);
        copy.setOnClickListener(v -> {
            if (token.text().isEmpty()) actions.message("令牌为空，无需复制", null, null);
            else actions.copyToken(token.text());
        });
        content.addView(ui.connected(generate, copy), Ui.stack(t.spaceLg));

        // ---- 底部工具栏（M3E docked toolbar）：有改动才出现 ----
        saveBar = ui.column();
        saveBar.setId(R.id.save_bar);
        saveState = Ui.live(ui.text("", Tokens.LABEL_LARGE, t.onSurface));
        discard = ui.button("放弃", Btn.TEXT);
        discard.setId(R.id.discard);
        discard.setOnClickListener(v -> confirmDiscard(null));
        save = ui.button("保存", Btn.FILLED);
        save.setId(R.id.save);
        save.setOnClickListener(v -> submit());
        if (pinned()) {
            saveBar.setPadding(ui.layout.gutter(), t.spaceSm, ui.layout.gutter(), t.spaceSm);
            saveBar.setMinimumHeight(t.toolbar);
            saveBar.setGravity(Gravity.CENTER_VERTICAL);
            saveBar.setBackgroundColor(t.surfaceContainer);
            LinearLayout line = ui.row();
            line.addView(saveState, Ui.share(0));
            line.addView(discard, Ui.wrap(0));
            line.addView(save, Ui.wrap(t.spaceSm));
            saveBar.addView(line, Ui.stack(0));
            root.addView(saveBar, new LinearLayout.LayoutParams(-1, -2));
        } else {
            // 大字号：保存区进入滚动内容，不再常驻底部挤占本就不多的可视高度。
            saveBar.setPadding(t.spaceLg, t.spaceLg, t.spaceLg, t.spaceLg);
            saveBar.setBackground(new Shape(t.surfaceContainerHigh, t.shapeXl));
            saveBar.addView(saveState, Ui.stack(0));
            saveBar.addView(save, Ui.stack(t.spaceMd));
            saveBar.addView(discard, Ui.stack(0));
            content.addView(saveBar, Ui.stack(t.spaceXl));
        }
        saveBar.setVisibility(View.GONE);

        TextWatcher watcher = new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) { changed(); }
        };
        port.input.addTextChangedListener(watcher);
        token.input.addTextChangedListener(watcher);
    }

    boolean pinned() {
        return ui.layout.fontScale < 1.5f;
    }

    private boolean saveBarShown() {
        return pinned() && saveBar.getVisibility() == View.VISIBLE;
    }

    /** 底部系统栏高度：工具栏出现时由它垫在系统栏上方，否则留给滚动内容。 */
    void setBottomInset(int inset) {
        bottomInset = inset;
        applyInset();
    }

    private void applyInset() {
        scroll.setPadding(0, 0, 0, saveBarShown() ? 0 : bottomInset);
        if (pinned()) saveBar.setPadding(saveBar.getPaddingLeft(), t.spaceSm, saveBar.getPaddingRight(), t.spaceSm + bottomInset);
    }

    // ------------------------------------------------------------------ 表单

    /**
     * 载入已保存的配置。有未保存草稿时只更新基准、不覆盖草稿（除非 force）。
     * 文件无效（value 为 null）时用 fallback 填表：它来自文件里还能认出的部分，保存即修复。
     */
    void load(Conf value, Conf fallback, boolean force) {
        if (!baseline) force = true;
        baseline = true;
        saved = value;
        if (!force && (dirty() || saving)) {
            changed();
            return;
        }
        Conf form = value != null ? value : fallback;
        loading = true;
        port.setText(String.valueOf(form.port));
        token.setText(form.token);
        loading = false;
        port.error(null);
        token.error(null);
        changed();
    }

    boolean dirty() {
        if (!baseline) return false;
        if (saved == null) return true;
        if (!port.text().equals(String.valueOf(saved.port))) return true;
        return !token.text().equals(saved.token);
    }

    private void changed() {
        if (loading) return;
        boolean dirty = dirty();
        if (dirty != lastDirty) {
            lastDirty = dirty;
            actions.draftChanged(dirty);
        }
        boolean show = dirty || saving;
        Ui.set(saveState, saving ? "正在保存…" : (saved == null ? "配置需要修复" : "有未保存的修改"));
        save.setEnabled(dirty && !saving);
        discard.setEnabled(dirty && !saving && saved != null);
        discard.setVisibility(saved == null ? View.GONE : View.VISIBLE);
        Ui.set(save, saving ? "正在保存" : "保存");
        if ((saveBar.getVisibility() == View.VISIBLE) == show) return;
        if (!show) {
            saveBar.setVisibility(View.GONE);
            applyInset();
            return;
        }
        saveBar.setVisibility(View.VISIBLE);
        applyInset();
        if (Spring.enabled() && pinned()) {
            saveBar.setAlpha(0f);
            saveBar.setTranslationY(t.dp(24));
            saveBar.animate().alpha(1f).translationY(0).setDuration(t.spatialDefault.duration)
                    .setInterpolator(t.spatialDefault).start();
        }
    }

    // ------------------------------------------------------------------ 保存

    /** 校验并交给宿主保存；出错的字段就地提示并获得焦点。 */
    void submit() {
        if (saving) return;
        int number;
        try {
            number = Integer.parseInt(port.text().trim());
            if (number < Conf.MIN_PORT || number > Conf.MAX_PORT) throw new NumberFormatException();
        } catch (NumberFormatException e) {
            token.error(null);
            port.error("端口须为 " + Conf.MIN_PORT + "–" + Conf.MAX_PORT + " 的整数");
            return;
        }
        port.error(null);
        if (!Conf.tokenValid(token.text())) {
            token.error(token.text().length() < Conf.MIN_TOKEN
                    ? "令牌太短：至少 " + Conf.MIN_TOKEN + " 位，现在 " + token.text().length() + " 位"
                    : "令牌只能用英文字母、数字、- 或 _");
            return;
        }
        token.error(null);
        Conf value = new Conf(number, token.text());
        try {
            value.write();
        } catch (Conf.Invalid invalid) {
            actions.message(invalid.getMessage(), null, null);
            return;
        }
        saving = true;
        changed();
        actions.save(value);
    }

    /** 宿主保存完成后回调。 */
    void saved(Conf value, boolean ok) {
        saving = false;
        if (ok) {
            load(value, value, true);
            reveal(false);
        } else {
            changed();
        }
    }

    void discardNow() {
        if (saved != null) load(saved, saved, true);
    }

    /** 放弃草稿前确认；确认后回到已保存的设置，再执行 then。 */
    void confirmDiscard(Runnable then) {
        if (!dirty() || saving || saved == null) {
            if (then != null) then.run();
            return;
        }
        Dialogs.show(ui, "放弃这些修改？", "恢复为上次保存的设置。",
                new Dialogs.Action("取消", Btn.TEXT, null),
                new Dialogs.Action("放弃修改", Btn.DANGER, () -> {
                    discardNow();
                    actions.discarded();
                    if (then != null) then.run();
                }));
    }

    void reveal(boolean value) {
        if (revealing == value && !value) {
            actions.secure(false);
            return;
        }
        revealing = value;
        int selection = token.input.getSelectionEnd();
        token.input.setTransformationMethod(value ? null : PasswordTransformationMethod.getInstance());
        token.input.setSelection(Math.max(0, Math.min(selection, token.input.length())));
        Ui.retarget(reveal, t, value ? Icon.HIDE : Icon.SHOW, t.onSurfaceVariant, value ? "隐藏令牌" : "显示令牌");
        actions.secure(value);
    }

    void notice(Status.Line line) {
        notice.show(line.tone, line.title, line.detail, line.action == Status.ACTION_RESTART_WECHAT ? line.actionLabel : null);
    }

    // ------------------------------------------------------------------ 实例状态

    void saveState(Bundle out) {
        out.putString("draft_port", port.text());
        out.putString("draft_token", token.text());
        out.putInt("settings_scroll", scroll.getScrollY());
    }

    void restoreState(Bundle state) {
        if (state == null || !state.containsKey("draft_port")) return;
        // 恢复出来的是用户的草稿：之后异步读到的配置只更新基准，不覆盖草稿。
        baseline = true;
        loading = true;
        port.setText(state.getString("draft_port", ""));
        token.setText(state.getString("draft_token", ""));
        loading = false;
        changed();
        final int y = state.getInt("settings_scroll");
        scroll.post(() -> scroll.scrollTo(0, y));
    }
}
