package com.satori.wx.ui;

import android.animation.ValueAnimator;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.View;
import android.view.animation.LinearInterpolator;

/**
 * M3E 加载指示器：一枚实心图形在七种形状之间依次形变、同时旋转，用于时长未知的等待。
 * 可视 38dp，放在 48dp 的容器里。每一段形变用表现力弹簧走完，整体旋转匀速。
 *
 * <p>只在可见且系统允许动画时运行，隐藏或脱离窗口立刻停；关闭动画时画一枚静止的形状。
 * 纯装饰，语义由旁边的状态文字承担。
 */
final class LoadingIndicator extends View {
    private static final long STEP = 650;

    private final Tokens t;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path shape = new Path();
    private ValueAnimator motion;
    private float progress;

    LoadingIndicator(Tokens tokens, int color) {
        super(tokens.context);
        this.t = tokens;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(color);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        setMinimumWidth(tokens.loading);
        setMinimumHeight(tokens.loading);
    }

    void tint(int color) {
        paint.setColor(color);
        invalidate();
    }

    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(resolveSize(getSuggestedMinimumWidth(), widthSpec),
                resolveSize(getSuggestedMinimumHeight(), heightSpec));
    }

    @Override protected void onDraw(Canvas canvas) {
        float size = Math.min(getWidth(), getHeight());
        if (size <= 0) return;
        float radius = size * 38f / 48f / 2f / 1.12f;
        int n = Morph.LOADING.length;
        int step = (int) Math.floor(progress);
        float phase = progress - step;
        Morph.Spec from = Morph.LOADING[step % n];
        Morph.Spec to = Morph.LOADING[(step + 1) % n];
        // 每段形变按表现力弹簧的位移曲线走，前 60% 时间完成形变、余下停在新形状上。
        float f = motion == null ? 0 : t.expressiveDefault.getInterpolation(Math.min(1f, phase / 0.6f));
        float rotation = (float) (progress * Math.PI * 2 / 3.5);
        Morph.fill(shape, getWidth() / 2f, getHeight() / 2f, radius, from, to, Math.min(1.2f, f), rotation);
        canvas.drawPath(shape, paint);
    }

    @Override public void onVisibilityAggregated(boolean visible) {
        super.onVisibilityAggregated(visible);
        stop();
        if (visible && Spring.enabled()) {
            int n = Morph.LOADING.length;
            motion = ValueAnimator.ofFloat(0, n);
            motion.setDuration(STEP * n);
            motion.setRepeatCount(ValueAnimator.INFINITE);
            motion.setInterpolator(new LinearInterpolator());
            motion.addUpdateListener(value -> {
                progress = (float) value.getAnimatedValue();
                invalidate();
            });
            motion.start();
        }
    }

    @Override protected void onDetachedFromWindow() {
        stop();
        super.onDetachedFromWindow();
    }

    private void stop() {
        if (motion != null) {
            motion.cancel();
            motion = null;
        }
    }
}
