package com.satori.wx.ui;

import android.animation.ArgbEvaluator;
import android.animation.ValueAnimator;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.drawable.Drawable;
import android.view.View;
import com.satori.wx.core.Status;

/**
 * 状态卡片上的徽章：48dp 的 M3E 形状里放一枚状态图标。结论变了，形状在两种之间形变
 * （空间弹簧），颜色走效果弹簧——检查中是圆，就绪是九瓣饼干，待处理是四瓣，出错是柔和爆炸形。
 *
 * <p>形状只是加强：同一个结论同时由标题文字、图标与容器色表达（WCAG 1.4.1）。纯装饰，不进无障碍树。
 */
final class Badge extends View {
    private static final ArgbEvaluator ARGB = new ArgbEvaluator();

    private final Tokens t;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private Morph.Spec from = Morph.CIRCLE, to = Morph.CIRCLE;
    private int fromColor, toColor;
    private float f = 1;
    private Drawable icon;
    private ValueAnimator motion;
    private int tone = -1;

    Badge(Tokens tokens) {
        super(tokens.context);
        this.t = tokens;
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        fromColor = toColor = tokens.surfaceContainerHighest;
    }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(t.badge, t.badge);
    }

    void show(int tone, boolean animate) {
        if (this.tone == tone) return;
        this.tone = tone;
        Morph.Spec spec;
        int kind, fill, ink;
        switch (tone) {
            // 徽章放在同语调的容器上：填充取 on_*_container、图标取容器色，两者对比度即文字配对的 ≥ 4.5。
            case Status.SUCCESS: spec = Morph.COOKIE_9; kind = Icon.CHECK; fill = t.onSuccessContainer; ink = t.successContainer; break;
            case Status.WARNING: spec = Morph.COOKIE_4; kind = Icon.WAIT; fill = t.onWarningContainer; ink = t.warningContainer; break;
            case Status.ERROR: spec = Morph.SOFT_BURST; kind = Icon.ERROR; fill = t.onErrorContainer; ink = t.errorContainer; break;
            default: spec = Morph.CIRCLE; kind = Icon.UNKNOWN; fill = t.surfaceContainerHighest; ink = t.onSurfaceVariant; break;
        }
        from = current();
        fromColor = currentColor();
        to = spec;
        toColor = fill;
        icon = Icon.of(t, kind, ink);
        if (motion != null) motion.cancel();
        if (!animate || !Spring.enabled() || !isAttachedToWindow()) {
            f = 1;
            invalidate();
            return;
        }
        f = 0;
        motion = t.expressiveDefault.apply(ValueAnimator.ofFloat(0, 1));
        motion.addUpdateListener(a -> {
            f = (float) a.getAnimatedValue();
            invalidate();
        });
        motion.start();
    }

    /** 形变中途被打断时，从当前的中间形状继续，而不是跳回起点。 */
    private Morph.Spec current() {
        if (f >= 1) return to;
        float clamped = Math.max(0, Math.min(1, f));
        if (from.lobes == to.lobes || clamped < 0.5f) {
            return new Morph.Spec(from.lobes, from.amplitude * (1 - clamped));
        }
        return new Morph.Spec(to.lobes, to.amplitude * clamped);
    }

    private int currentColor() {
        return (Integer) ARGB.evaluate(Math.max(0, Math.min(1, f)), fromColor, toColor);
    }

    @Override protected void onDraw(Canvas canvas) {
        float size = Math.min(getWidth(), getHeight());
        float radius = size / 2f / 1.12f;
        float rotation = (float) (Math.PI / 2 * (1 - Math.min(1, f)) * 0.25);
        Morph.fill(path, getWidth() / 2f, getHeight() / 2f, radius, from, to, f, rotation);
        paint.setColor(currentColor());
        canvas.drawPath(path, paint);
        if (icon != null) {
            int half = t.icon / 2;
            int cx = getWidth() / 2, cy = getHeight() / 2;
            icon.setBounds(cx - half, cy - half, cx + half, cy + half);
            icon.draw(canvas);
        }
    }

    @Override protected void onDetachedFromWindow() {
        if (motion != null) motion.cancel();
        f = 1;
        super.onDetachedFromWindow();
    }
}
