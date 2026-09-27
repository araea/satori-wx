package com.satori.wx.ui;

import android.os.Build;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * M3 列表项：前置元素（图标或头像）+ 标题 + 辅助文字 + 尾部（开关 / 复选框 / 箭头 / 自定义视图）。
 * 在 M3E 的分段列表（{@link Ui.Group}）里是一格。
 *
 * <p>四种角色：
 * <ul>
 *   <li>{@link #STATIC}——只展示；读屏把整行作为一个整体朗读，但不占键盘焦点；</li>
 *   <li>{@link #ACTION}——整行是一个按钮；</li>
 *   <li>{@link #SWITCH}——整行是一个开关，读作「开关，已开启/已关闭」；</li>
 *   <li>{@link #CHECK}——整行是一个复选框，读作「复选框，已选中/未选中」。</li>
 * </ul>
 * 开关与复选框的图形不进无障碍树，否则读屏会在同一行读到两个控件。
 * 最小高度单行 56dp、双行 72dp，任何角色的点击区域都是整行。
 */
final class Item extends LinearLayout {
    static final int STATIC = 0;
    static final int ACTION = 1;
    static final int SWITCH = 2;
    static final int CHECK = 3;

    interface OnToggle {
        void toggled(Item item, boolean checked);
    }

    final TextView headline;
    final TextView supporting;
    private final Tokens t;
    private final int mode;
    final Shape shape;
    private ImageView leadingView;
    private int leadingKind;
    private int leadingColor;
    private FrameLayout avatar;
    private Shape avatarShape;
    private Toggle toggle;
    private Checkbox checkbox;
    private ImageView chevron;
    private boolean checked;
    private boolean destructive;
    private OnToggle onToggle;

    Item(Tokens tokens, int mode, CharSequence title, CharSequence detail) {
        super(tokens.context);
        this.t = tokens;
        this.mode = mode;
        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setBaselineAligned(false);
        setPaddingRelative(tokens.spaceLg, tokens.spaceSm + tokens.spaceXs, tokens.spaceLg, tokens.spaceSm + tokens.spaceXs);
        setMinimumHeight(detail == null ? tokens.listOneLine : tokens.listTwoLine);

        LinearLayout words = new LinearLayout(tokens.context);
        words.setOrientation(VERTICAL);
        headline = tokens.type(new TextView(tokens.context), Tokens.BODY_LARGE);
        headline.setTextColor(tokens.onSurface);
        headline.setText(title);
        words.addView(headline, new LayoutParams(-1, -2));
        supporting = tokens.type(new TextView(tokens.context), Tokens.BODY_MEDIUM);
        supporting.setTextColor(tokens.onSurfaceVariant);
        setSupporting(detail);
        words.addView(supporting, new LayoutParams(-1, -2));
        addView(words, new LayoutParams(0, -2, 1));

        shape = new Shape(tokens.surfaceContainer, tokens.shapeXs).ring(tokens.primary, tokens.focusRing);
        if (mode == STATIC) {
            setBackground(shape);
            if (Build.VERSION.SDK_INT >= 28) setScreenReaderFocusable(true);
        } else {
            setBackground(shape.pressable(Tokens.alpha(tokens.onSurface, tokens.pressedPct)));
            setClickable(true);
            setFocusable(true);
        }
        if (mode == SWITCH) {
            toggle = new Toggle(tokens);
            LayoutParams params = new LayoutParams(-2, -2);
            params.setMarginStart(tokens.spaceLg);
            addView(toggle, params);
        } else if (mode == CHECK) {
            checkbox = new Checkbox(tokens);
            LayoutParams params = new LayoutParams(-2, -2);
            params.setMarginStart(tokens.spaceSm);
            params.setMarginEnd(-tokens.spaceSm);
            addView(checkbox, params);
        }
        if (mode == SWITCH || mode == CHECK) {
            describeState();
            super.setOnClickListener(v -> {
                boolean next = !checked;
                setChecked(next, true);
                if (onToggle != null) onToggle.toggled(this, next);
            });
        }
    }

    /** 前置图标：24dp，默认 on_surface_variant。 */
    Item leading(int kind, int color) {
        leadingColor = color;
        if (leadingView == null) {
            leadingView = new ImageView(t.context);
            leadingView.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
            LayoutParams params = new LayoutParams(t.icon, t.icon);
            params.setMarginEnd(t.spaceLg);
            addView(leadingView, 0, params);
        }
        if (leadingKind != kind) {
            leadingKind = kind;
            leadingView.setImageDrawable(Icon.of(t, kind, isEnabled() ? color : t.disabledContent()));
        } else {
            leadingView.getDrawable().setTint(isEnabled() ? color : t.disabledContent());
        }
        return this;
    }

    /** 前置头像：40dp 圆形色调容器里放一枚图标（M3 列表的 leading avatar）。 */
    Item avatar(int kind, int container, int content) {
        if (avatar == null) {
            avatar = new FrameLayout(t.context);
            avatarShape = new Shape(container, Shape.FULL);
            avatar.setBackground(avatarShape);
            avatar.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
            leadingView = new ImageView(t.context);
            avatar.addView(leadingView, new FrameLayout.LayoutParams(t.icon, t.icon, Gravity.CENTER));
            LayoutParams params = new LayoutParams(t.avatar, t.avatar);
            params.setMarginEnd(t.spaceLg);
            addView(avatar, 0, params);
        }
        avatarShape.fill(container);
        leadingKind = kind;
        leadingColor = content;
        leadingView.setImageDrawable(Icon.of(t, kind, content));
        return this;
    }

    /** 尾部箭头：表示「进入下一层」。 */
    Item chevron() {
        chevron = new ImageView(t.context);
        chevron.setImageDrawable(Icon.of(t, Icon.NEXT, t.onSurfaceVariant));
        chevron.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        LayoutParams params = new LayoutParams(t.icon, t.icon);
        params.setMarginStart(t.spaceLg);
        addView(chevron, params);
        return this;
    }

    /** 尾部自定义视图（例如复制用的图标按钮）：贴近右缘，自身的 48dp 触达区不被挤压。 */
    Item trailing(View view) {
        LayoutParams params = new LayoutParams(-2, -2);
        params.setMarginStart(t.spaceSm);
        setPaddingRelative(getPaddingStart(), getPaddingTop(), t.spaceXs, getPaddingBottom());
        addView(view, params);
        return this;
    }

    /** 破坏性动作：标题与图标用错误色；外观可辨，但动作仍须确认。 */
    Item destructive() {
        destructive = true;
        headline.setTextColor(t.error);
        leadingColor = t.error;
        if (leadingView != null && avatar == null) leadingView.getDrawable().setTint(t.error);
        return this;
    }

    void setSupporting(CharSequence text) {
        CharSequence value = text == null ? "" : text;
        if (TextUtils.equals(supporting.getText(), value)) return;
        supporting.setText(value);
        supporting.setVisibility(TextUtils.isEmpty(value) ? GONE : VISIBLE);
    }

    void setHeadline(CharSequence text) {
        if (!TextUtils.equals(headline.getText(), text)) headline.setText(text);
    }

    void setOnToggle(OnToggle listener) {
        onToggle = listener;
    }

    boolean isChecked() {
        return checked;
    }

    void setChecked(boolean value, boolean animate) {
        boolean changed = checked != value;
        checked = value;
        if (toggle != null) toggle.setChecked(value, animate);
        if (checkbox != null) checkbox.setChecked(value, animate);
        if (changed) describeState();
    }

    private void describeState() {
        if (Build.VERSION.SDK_INT < 30) return;
        if (mode == SWITCH) setStateDescription(checked ? "已开启" : "已关闭");
        if (mode == CHECK) setStateDescription(checked ? "已选中" : "未选中");
    }

    @Override public void setEnabled(boolean enabled) {
        super.setEnabled(enabled);
        if (headline == null) return;
        int dim = t.disabledContent();
        headline.setTextColor(enabled ? (destructive ? t.error : t.onSurface) : dim);
        supporting.setTextColor(enabled ? t.onSurfaceVariant : dim);
        if (leadingView != null && leadingView.getDrawable() != null) {
            leadingView.getDrawable().setTint(enabled ? leadingColor : dim);
        }
        if (avatar != null) avatar.setAlpha(enabled ? 1f : t.disabledContentPct / 100f);
        if (chevron != null) chevron.getDrawable().setTint(enabled ? t.onSurfaceVariant : dim);
        if (toggle != null) toggle.setEnabledLook(enabled);
        if (checkbox != null) checkbox.setEnabledLook(enabled);
    }

    /** 分段列表里的位置决定四个角：组首尾取大圆角，中间取小圆角。 */
    void corners(float top, float bottom) {
        shape.setRadii(top, top, bottom, bottom);
    }

    /** 容器色：默认 surface_container；选中态等场景可换色调容器。 */
    void container(int color) {
        shape.fill(color);
    }

    @Override public CharSequence getAccessibilityClassName() {
        switch (mode) {
            case SWITCH: return "android.widget.Switch";
            case CHECK: return "android.widget.CheckBox";
            case ACTION: return "android.widget.Button";
            default: return super.getAccessibilityClassName();
        }
    }

    @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfo(info);
        if (mode == SWITCH || mode == CHECK) {
            info.setCheckable(true);
            info.setChecked(checked);
        }
    }
}
