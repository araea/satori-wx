package com.satori.wx.ui;

import android.content.res.ColorStateList;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.InsetDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/**
 * 组件工厂：把令牌组装成页面用的积木。依赖方向固定为 令牌 → 组件 → 页面。
 *
 * <p>视觉只有一个体系——Material 3 Expressive：颜色角色、字阶、形状刻度、弹簧与形状形变、
 * 分段列表、连接按钮组。Apple HIG 只在交互层出现：每屏一个主操作、破坏性操作先确认、
 * 即时反馈与撤销、保留草稿、大标题随滚动收进顶栏。冲突时平台惯例、可用性与无障碍优先。
 */
final class Ui {
    final Tokens t;
    final Layout layout;

    Ui(Tokens tokens, Layout layout) {
        this.t = tokens;
        this.layout = layout;
    }

    // ------------------------------------------------------------------ 文本

    TextView text(CharSequence value, int role, int color) {
        TextView view = t.type(new TextView(t.context), role);
        view.setText(value);
        view.setTextColor(color);
        return view;
    }

    /** 标题：读屏可按标题跳转（WCAG 1.3.1 / 2.4.6）。 */
    TextView heading(CharSequence value, int role, int color) {
        TextView view = text(value, role, color);
        if (Build.VERSION.SDK_INT >= 28) view.setAccessibilityHeading(true);
        return view;
    }

    static <T extends View> T live(T view) {
        view.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        return view;
    }

    /** 只在内容真的变了才写：避免轮询时反复触发重新布局与 live region 播报。 */
    static void set(TextView view, CharSequence value) {
        if (!TextUtils.equals(view.getText(), value)) view.setText(value);
    }

    // ------------------------------------------------------------------ 布局

    LinearLayout column() {
        LinearLayout layout = new LinearLayout(t.context);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    LinearLayout row() {
        LinearLayout layout = new LinearLayout(t.context);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        layout.setBaselineAligned(false);
        return layout;
    }

    static LinearLayout.LayoutParams stack(int top) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = top;
        return params;
    }

    static LinearLayout.LayoutParams share(int start) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, -2, 1);
        params.setMarginStart(start);
        return params;
    }

    static LinearLayout.LayoutParams wrap(int start) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-2, -2);
        params.setMarginStart(start);
        return params;
    }

    // ------------------------------------------------------------------ 分组

    /** 列表小标题（M3 list subheader）：title small、主色，与列表容器内的文字左缘对齐。 */
    TextView sectionTitle(String title) {
        TextView view = heading(title, Tokens.TITLE_SMALL, t.primary);
        view.setPaddingRelative(t.spaceLg, 0, t.spaceLg, t.spaceSm);
        return view;
    }

    Group group() {
        return new Group();
    }

    /**
     * M3E 分段列表：各项之间 2dp 缝隙，组首尾大圆角、中间小圆角。
     * 某项显隐变化后调用 {@link #refresh()} 重新分配圆角。
     */
    final class Group extends LinearLayout {
        private final List<Item> items = new ArrayList<>();

        Group() {
            super(t.context);
            setOrientation(VERTICAL);
        }

        Item add(Item item) {
            items.add(item);
            addView(item, stack(0));
            refresh();
            return item;
        }

        void remove(Item item) {
            items.remove(item);
            removeView(item);
            refresh();
        }

        void clear() {
            items.clear();
            removeAllViews();
        }

        int size() {
            return items.size();
        }

        void refresh() {
            Item first = null, last = null;
            for (Item item : items) {
                if (item.getVisibility() == GONE) continue;
                if (first == null) first = item;
                last = item;
            }
            boolean gap = false;
            for (Item item : items) {
                if (item.getVisibility() == GONE) continue;
                ((LayoutParams) item.getLayoutParams()).topMargin = gap ? t.space2xs : 0;
                gap = true;
                item.corners(item == first ? t.shapeLgIncreased : t.shapeXs, item == last ? t.shapeLgIncreased : t.shapeXs);
            }
            requestLayout();
        }
    }

    // ------------------------------------------------------------------ 按钮

    Btn button(String label, int style) {
        return new Btn(t, label, style, false);
    }

    Btn medium(String label, int style) {
        return new Btn(t, label, style, true);
    }

    /** M3 标准图标按钮：40dp 可视圆、48dp 触达区，名称由 contentDescription 与 tooltip 承担。 */
    ImageButton iconButton(int kind, String description, int color) {
        ImageButton view = new ImageButton(t.context);
        view.setImageDrawable(Icon.of(t, kind, color));
        view.setScaleType(ImageView.ScaleType.CENTER);
        int inset = (t.touchTarget - t.iconButton) / 2;
        Shape shape = new Shape(0, Shape.FULL).ring(t.primary, t.focusRing);
        view.setBackground(new RippleDrawable(ColorStateList.valueOf(Tokens.alpha(color, t.pressedPct)),
                new InsetDrawable(shape, inset), new InsetDrawable(shape.mask(), inset)));
        view.setContentDescription(description);
        view.setMinimumWidth(t.touchTarget);
        view.setMinimumHeight(t.touchTarget);
        view.setPadding(0, 0, 0, 0);
        if (Build.VERSION.SDK_INT >= 26) view.setTooltipText(description);
        return view;
    }

    static void retarget(ImageButton button, Tokens t, int kind, int color, String description) {
        button.setImageDrawable(Icon.of(t, kind, color));
        button.setContentDescription(description);
        if (Build.VERSION.SDK_INT >= 26) button.setTooltipText(description);
    }

    /**
     * M3E 连接按钮组：同一组相关操作并排，2dp 缝隙，内侧小圆角、外侧胶囊。
     * 放不下（大字号或窄屏）时竖排，各自恢复完整胶囊。
     */
    LinearLayout connected(Btn... buttons) {
        boolean stacked = layout.stackActions();
        LinearLayout group = stacked ? column() : row();
        for (int i = 0; i < buttons.length; i++) {
            Btn button = buttons[i];
            if (stacked) {
                group.addView(button, stack(0));
                continue;
            }
            float inner = t.shapeSm;
            float start = i == 0 ? Shape.FULL : inner;
            float end = i == buttons.length - 1 ? Shape.FULL : inner;
            button.corners(start, end, end, start);
            group.addView(button, share(i == 0 ? 0 : t.space2xs));
        }
        return group;
    }

    /** 装饰性图标视图（不进无障碍树）。 */
    ImageView glyph(int kind, int color, int size) {
        ImageView view = new ImageView(t.context);
        view.setImageDrawable(Icon.of(t, kind, color));
        view.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        view.setLayoutParams(new LinearLayout.LayoutParams(size, size));
        return view;
    }

    static Drawable tinted(Drawable drawable, int color) {
        drawable.setTint(color);
        return drawable;
    }
}
