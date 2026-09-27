package com.satori.wx.ui;

import android.text.TextUtils;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.satori.wx.core.Status;

/**
 * 状态信息卡：图标 + 结论 + 说明，可带一个动作。M3 的填充卡片（filled card）换上语调容器色——
 * 成功 / 警告 / 错误各有容器角色，中性用 surface_container_high。状态色只加强，结论文字本身已说清楚（WCAG 1.4.1）。
 * 文字整块是 polite live region，状态变化时读屏朗读一次（WCAG 4.1.3）。
 */
final class Notice extends LinearLayout {
    private final Tokens t;
    private final Shape shape;
    private final ImageView mark;
    private final TextView title;
    private final TextView body;
    private final LinearLayout words;
    private Btn action;
    private int tone = -1;

    Notice(Tokens tokens) {
        super(tokens.context);
        this.t = tokens;
        setOrientation(HORIZONTAL);
        setBaselineAligned(false);
        setPadding(tokens.spaceLg, tokens.spaceLg, tokens.spaceLg, tokens.spaceLg);
        shape = new Shape(tokens.surfaceContainerHigh, tokens.shapeLg);
        setBackground(shape);

        mark = new ImageView(tokens.context);
        mark.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        LayoutParams markParams = new LayoutParams(tokens.icon, tokens.icon);
        markParams.setMarginEnd(tokens.spaceMd);
        addView(mark, markParams);

        words = new LinearLayout(tokens.context);
        words.setOrientation(VERTICAL);
        words.setAccessibilityLiveRegion(ACCESSIBILITY_LIVE_REGION_POLITE);
        title = tokens.type(new TextView(tokens.context), Tokens.TITLE_MEDIUM);
        words.addView(title, new LayoutParams(-1, -2));
        body = tokens.type(new TextView(tokens.context), Tokens.BODY_MEDIUM);
        LayoutParams bodyParams = new LayoutParams(-1, -2);
        bodyParams.topMargin = tokens.spaceXs;
        words.addView(body, bodyParams);
        addView(words, new LayoutParams(0, -2, 1));
    }

    /** 可选动作：文字按钮，放在说明下方、与文字左缘对齐（按钮自身的内边距向外让出）。 */
    Btn action(String label, View.OnClickListener listener) {
        action = new Btn(t, label, Btn.TEXT, false);
        action.setOnClickListener(listener);
        LayoutParams params = new LayoutParams(-2, -2);
        params.topMargin = t.spaceXs;
        params.setMarginStart(-t.spaceMd);
        words.addView(action, params);
        return action;
    }

    void show(int tone, String heading, String detail, String actionLabel) {
        if (this.tone != tone) {
            this.tone = tone;
            int ink = t.toneOnContainer(tone);
            int kind;
            switch (tone) {
                case Status.SUCCESS: kind = Icon.OK; break;
                case Status.WARNING: kind = Icon.WARNING; break;
                case Status.ERROR: kind = Icon.ERROR; break;
                default: kind = Icon.INFO; break;
            }
            shape.fill(t.toneContainer(tone));
            mark.setImageDrawable(Icon.of(t, kind, tone == Status.NEUTRAL ? t.onSurfaceVariant : ink));
            title.setTextColor(ink);
            body.setTextColor(tone == Status.NEUTRAL ? t.onSurfaceVariant : ink);
            if (action != null) action.setTextColor(tone == Status.NEUTRAL ? t.primary : ink);
        }
        Ui.set(title, heading);
        Ui.set(body, detail);
        if (action != null) {
            action.setVisibility(actionLabel != null ? VISIBLE : GONE);
            if (actionLabel != null) Ui.set(action, actionLabel);
        }
    }
}
