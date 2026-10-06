package com.satori.wx.ui;

import android.animation.ArgbEvaluator;
import android.animation.ValueAnimator;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.view.Gravity;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.Button;

/**
 * M3E 按钮：胶囊外形，按下时向较方的圆角形变并回弹（表现力弹簧），松手弹回。
 *
 * <p>两种尺寸：S（可视 40dp，默认）与 M（可视 56dp，每屏唯一的主操作）。S 的可视形状在 48dp 的触达区里
 * 上下各留 4dp（WCAG 2.5.8，也满足 HIG 的 44pt）。五种强调层级：填充（主操作）、色调、描边、文字、
 * 危险（错误色文字，只用在确认弹窗里，破坏性动作永远先确认）。
 *
 * <p>{@link #toggle} 把它变成 M3E 的切换按钮：未选中是 surface_container 容器，选中换成主色并把角收成
 * 方圆——状态靠形状、颜色和读屏的「已选中」三重表达，不只靠颜色（WCAG 1.4.1）。
 * 标签可换行，放大字号时按钮长高而不截断；系统关闭动画时不做形变，只剩颜色与波纹反馈。
 */
final class Btn extends Button {
    static final int FILLED = 0;
    static final int TONAL = 1;
    static final int OUTLINED = 2;
    static final int TEXT = 3;
    static final int DANGER = 4;

    private static final ArgbEvaluator ARGB = new ArgbEvaluator();

    private final Tokens t;
    private final int style;
    private final boolean medium;
    final Shape shape;
    private Drawable icon;
    private ValueAnimator press, select;
    private float pressValue, selectValue;
    private boolean toggle, checked;
    private float[] rest = {Shape.FULL, Shape.FULL, Shape.FULL, Shape.FULL};
    private final int side;

    Btn(Tokens tokens, String label, int style, boolean medium) {
        super(tokens.context);
        this.t = tokens;
        this.style = style;
        this.medium = medium;
        setText(label);
        tokens.type(this, medium ? Tokens.TITLE_MEDIUM : Tokens.LABEL_LARGE);
        setAllCaps(false);
        setSingleLine(false);
        setGravity(Gravity.CENTER);
        setStateListAnimator(null);
        int visual = medium ? tokens.buttonMedium : tokens.button;
        int inset = Math.max(0, (tokens.touchTarget - visual) / 2);
        int height = Math.max(visual, tokens.touchTarget);
        setMinHeight(height);
        setMinimumHeight(height);
        setMinWidth(tokens.touchTarget);
        setMinimumWidth(tokens.touchTarget);
        side = style == TEXT || style == DANGER ? tokens.spaceMd : (medium ? tokens.spaceXl : tokens.spaceLg);
        int vertical = inset + (visual - tokens.dp(medium ? 24 : 20)) / 2;
        setPadding(side, vertical, side, vertical);
        setCompoundDrawablePadding(tokens.spaceSm);

        shape = new Shape(container(false), Shape.FULL)
                .pressedRadius(medium ? tokens.shapeLg : tokens.shapeSm)
                .insetY(inset)
                .ring(ring(false), tokens.focusRing);
        if (style == OUTLINED) shape.stroke(tokens.outlineVariant, tokens.border);
        setBackground(shape.pressable(Tokens.alpha(content(false), tokens.pressedPct)));
        applyColors();
    }

    /** 前置图标：S 号 20dp、M 号 24dp，颜色跟随文字与启用状态。 */
    Btn icon(int kind) {
        int size = medium ? t.icon : t.iconSmall;
        icon = Icon.of(t, kind, content(checked));
        icon.setBounds(0, 0, size, size);
        setCompoundDrawablesRelative(icon, null, null, null);
        applyColors();
        return this;
    }

    /** 连接按钮组：内侧角取小圆角，外侧保持胶囊。 */
    void corners(float topStart, float topEnd, float bottomEnd, float bottomStart) {
        rest = new float[]{topStart, topEnd, bottomEnd, bottomStart};
        applyShape();
    }

    /** 变成切换按钮（M3E toggle button）。 */
    Btn toggle() {
        toggle = true;
        return this;
    }

    boolean isChecked() {
        return checked;
    }

    void setChecked(boolean value, boolean animate) {
        if (!toggle || checked == value) return;
        checked = value;
        if (select != null) select.cancel();
        float target = value ? 1 : 0;
        if (!animate || !Spring.enabled() || !isAttachedToWindow()) {
            selectValue = target;
            applyShape();
            applyColors();
        } else {
            select = t.spatialFast.apply(ValueAnimator.ofFloat(selectValue, target));
            select.addUpdateListener(a -> {
                selectValue = (float) a.getAnimatedValue();
                applyShape();
                applyColors();
            });
            select.start();
        }
        if (Build.VERSION.SDK_INT >= 30) setStateDescription(value ? "已选中" : "未选中");
    }

    /** 选中的切换按钮把内侧小圆角展开成胶囊（M3E 连接按钮组的选中形状）。 */
    private void applyShape() {
        float[] r = new float[4];
        for (int i = 0; i < 4; i++) {
            if (rest[i] == Shape.FULL) {
                r[i] = Shape.FULL;
            } else {
                float full = Math.max(getHeight(), t.touchTarget) / 2f;
                r[i] = rest[i] + (full - rest[i]) * selectValue;
            }
        }
        shape.setRadii(r[0], r[1], r[2], r[3]);
    }

    private int container(boolean selected) {
        if (toggle) return selected ? t.primary : t.surfaceContainer;
        switch (style) {
            case FILLED: return t.primary;
            case TONAL: return t.secondaryContainer;
            default: return 0;
        }
    }

    private int content(boolean selected) {
        if (toggle) return selected ? t.onPrimary : t.onSurfaceVariant;
        switch (style) {
            case FILLED: return t.onPrimary;
            case TONAL: return t.onSecondaryContainer;
            case OUTLINED: return t.onSurfaceVariant;
            case DANGER: return t.error;
            default: return t.primary;
        }
    }

    private int ring(boolean selected) {
        if (toggle) return selected ? t.onPrimary : t.primary;
        return style == FILLED ? t.onPrimary : (style == DANGER ? t.error : t.primary);
    }

    private void applyColors() {
        if (shape == null) return;
        boolean enabled = isEnabled();
        int fill = toggle ? (Integer) ARGB.evaluate(selectValue, container(false), container(true)) : container(false);
        int ink = toggle ? (Integer) ARGB.evaluate(selectValue, content(false), content(true)) : content(false);
        if (!enabled) {
            fill = fill == 0 ? 0 : t.disabledContainer();
            ink = t.disabledContent();
        }
        shape.fill(fill);
        shape.ringColor(ring(selectValue > 0.5f));
        if (style == OUTLINED) shape.stroke(enabled ? t.outlineVariant : t.disabledContainer(), t.border);
        setTextColor(ink);
        if (icon != null) icon.setTint(ink);
    }

    @Override public void setEnabled(boolean enabled) {
        super.setEnabled(enabled);
        applyColors();
    }

    /**
     * 按钮比内容宽时（连接按钮组里平分宽度），图标与标签作为一组居中——M3 的图标紧贴标签，
     * 而框架的 compound drawable 只会贴在内边距边缘。标签换行时退回常规内边距。
     */
    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        super.onMeasure(widthSpec, heightSpec);
        if (icon == null) return;
        int iconSize = icon.getBounds().width();
        float label = getPaint().measureText(getText().toString());
        int content = iconSize + getCompoundDrawablePadding() + (int) Math.ceil(label);
        int inset = Math.max(side, (getMeasuredWidth() - content) / 2);
        if (getLineCount() > 1) inset = side;
        if (getPaddingStart() == inset && getPaddingEnd() == inset) return;
        setPaddingRelative(inset, getPaddingTop(), inset, getPaddingBottom());
        super.onMeasure(widthSpec, heightSpec);
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (toggle) applyShape();
    }

    @Override public void setPressed(boolean pressed) {
        boolean changed = isPressed() != pressed;
        super.setPressed(pressed);
        if (!changed || shape == null) return;
        if (press != null) press.cancel();
        float target = pressed ? 1f : 0f;
        if (!Spring.enabled()) {
            pressValue = target;
            shape.morph(target);
            return;
        }
        press = t.expressiveFast.apply(ValueAnimator.ofFloat(pressValue, target));
        press.addUpdateListener(animation -> {
            pressValue = (float) animation.getAnimatedValue();
            shape.morph(pressValue);
        });
        press.start();
    }

    @Override protected void onDetachedFromWindow() {
        if (press != null) press.cancel();
        if (select != null) select.cancel();
        super.onDetachedFromWindow();
    }

    @Override public CharSequence getAccessibilityClassName() {
        return toggle ? "android.widget.ToggleButton" : Button.class.getName();
    }

    @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
        super.onInitializeAccessibilityNodeInfo(info);
        if (toggle) {
            info.setCheckable(true);
            info.setChecked(checked);
        }
    }
}
