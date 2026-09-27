package com.satori.wx.ui;

import android.content.res.Configuration;

/**
 * 响应式规则：Android 窗口尺寸类别决定页边距与是否双栏，字号倍率决定能不能并排。
 *
 * <p>所有「并排还是竖排」的判断都用 <b>可用宽度 ÷ 字号倍率</b>：同样 360dp 宽，100% 字号能放下两块
 * 指标卡，200% 字号时每块只剩一半的有效宽度，就回到竖排（WCAG 1.4.4 / 1.4.10）。
 * 宽度取 {@code Configuration.screenWidthDp} 而不是某个 View 的宽度——拿布局结果决定布局会自激。
 * 双栏是 M3 的列表-详情规范布局（canonical layout），断点服从 Android 的窗口尺寸类别。
 */
final class Layout {
    final int widthDp;
    final float fontScale;
    private final Tokens t;

    Layout(Tokens tokens, Configuration configuration) {
        this.t = tokens;
        this.widthDp = configuration.screenWidthDp;
        this.fontScale = Math.max(1f, configuration.fontScale);
    }

    /** 页边距：compact 16dp，medium 起 24dp。 */
    int gutter() {
        return widthDp >= t.bpMedium ? t.spaceXl : t.spaceLg;
    }

    /** expanded 宽度且放大字号后每栏仍够宽时，状态与设置并列两栏（列表-详情）。 */
    boolean twoPane() {
        if (widthDp < t.bpExpanded) return false;
        float pane = (widthDp - 3 * px2dp(t.spaceXl)) / 2f;
        return pane / fontScale >= t.reflowPaneMin;
    }

    /** 一栏里的两块小卡片能不能并排。 */
    boolean pair() {
        float column = Math.min(px2dp(t.contentMax), twoPane() ? (widthDp - 3 * px2dp(t.spaceXl)) / 2f : widthDp)
                - 2 * px2dp(gutter());
        return column / fontScale >= t.reflowPairMin;
    }

    /** 弹窗按钮与按钮组放不下时竖排。 */
    boolean stackActions() {
        return fontScale >= 1.3f || widthDp / fontScale < 360;
    }

    private float px2dp(int px) {
        return px / t.density;
    }
}
