package com.satori.wx.ui;

import android.animation.ArgbEvaluator;
import android.animation.ValueAnimator;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

/**
 * M3 复选框的图形：18dp 方框、2dp 圆角。未选中是 2dp on_surface_variant 描边，
 * 选中填主色并画出 on_primary 的对勾（对勾沿路径画出，颜色走效果弹簧）。
 *
 * <p>只负责画。点击、焦点与读屏语义由所在的整行 {@link Item} 承担：这个视图不可聚焦、不进无障碍树。
 */
final class Checkbox extends View {
    private static final ArgbEvaluator ARGB = new ArgbEvaluator();

    private final Tokens t;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path mark = new Path();
    private final Path partial = new Path();
    private final RectF box = new RectF();
    private final android.graphics.PathMeasure measure = new android.graphics.PathMeasure();
    private float progress;
    private boolean checked;
    private boolean enabledLook = true;
    private ValueAnimator motion;

    Checkbox(Tokens tokens) {
        super(tokens.context);
        this.t = tokens;
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        setFocusable(false);
        setClickable(false);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(t.iconButton, t.iconButton);
    }

    void setChecked(boolean value, boolean animate) {
        if (checked == value && (motion == null || !motion.isRunning())) {
            progress = value ? 1 : 0;
            invalidate();
            return;
        }
        checked = value;
        if (motion != null) motion.cancel();
        float target = value ? 1 : 0;
        if (!animate || !Spring.enabled() || !isAttachedToWindow()) {
            progress = target;
            invalidate();
            return;
        }
        motion = t.effectsDefault.apply(ValueAnimator.ofFloat(progress, target));
        motion.addUpdateListener(a -> {
            progress = (float) a.getAnimatedValue();
            invalidate();
        });
        motion.start();
    }

    void setEnabledLook(boolean enabled) {
        enabledLook = enabled;
        invalidate();
    }

    @Override protected void onDraw(Canvas canvas) {
        float size = t.checkbox;
        float left = (getWidth() - size) / 2f;
        float top = (getHeight() - size) / 2f;
        float corner = t.dp(2);
        float stroke = t.dp(2);
        float p = Math.max(0, Math.min(1, progress));
        int on = enabledLook ? t.primary : t.disabledContent();
        int off = enabledLook ? t.onSurfaceVariant : t.disabledContent();

        box.set(left, top, left + size, top + size);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Tokens.alpha(on, Math.round(p * 100)));
        if (p > 0) canvas.drawRoundRect(box, corner, corner, paint);
        if (p < 1) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(stroke);
            paint.setColor((Integer) ARGB.evaluate(p, off, on));
            box.inset(stroke / 2, stroke / 2);
            canvas.drawRoundRect(box, corner, corner, paint);
        }
        if (p <= 0) return;
        mark.reset();
        mark.moveTo(left + size * 0.22f, top + size * 0.52f);
        mark.lineTo(left + size * 0.42f, top + size * 0.72f);
        mark.lineTo(left + size * 0.80f, top + size * 0.32f);
        measure.setPath(mark, false);
        partial.reset();
        measure.getSegment(0, measure.getLength() * p, partial, true);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(stroke);
        paint.setColor(enabledLook ? t.onPrimary : t.surface);
        canvas.drawPath(partial, paint);
    }

    @Override protected void onDetachedFromWindow() {
        if (motion != null) motion.cancel();
        super.onDetachedFromWindow();
    }
}
