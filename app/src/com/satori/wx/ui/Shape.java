package com.satori.wx.ui;

import android.R;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;

/**
 * 组件的形状层：M3 圆角矩形的填充、描边、键盘焦点环与按压形变，四个角可各自取值。
 *
 * <p>几何只有一种——M3 的圆弧角，半径取自形状刻度（md.sys.shape.corner.*），{@link #FULL}
 * 按高度取半成胶囊。按下时向 {@code pressedRadius} 形变（M3E 的形状形变），由调用方用弹簧驱动。
 *
 * <p>{@link #insetY} 让可视形状比视图矮：M3E 的 S 号按钮可视高 40dp，而触达区必须 48dp，
 * 两者之差在上下各留 4dp 透明的触达边。焦点环画在形状内侧，外扩会被父容器裁掉；
 * 只在键盘 / 方向键焦点下出现，触摸不会给按钮焦点。
 */
final class Shape extends Drawable {
    /** 半径取高度的一半（胶囊）。 */
    static final float FULL = -1;

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF box = new RectF();
    private final float[] radii = new float[4]; // 左上、右上、右下、左下
    private final float[] resolved = new float[8];
    private float pressedRadius = FULL;
    private boolean morphs;
    private float morph;
    private int fill;
    private int stroke;
    private float strokeWidth;
    private int ringColor;
    private float ringWidth;
    private int insetY;
    private boolean focused;
    private boolean dirty = true;

    Shape(int fill, float radius) {
        fillPaint.setStyle(Paint.Style.FILL);
        strokePaint.setStyle(Paint.Style.STROKE);
        this.fill = fill;
        setRadius(radius);
    }

    Shape fill(int color) {
        if (fill != color) {
            fill = color;
            invalidateSelf();
        }
        return this;
    }

    int fillColor() {
        return fill;
    }

    Shape stroke(int color, float width) {
        if (stroke != color || strokeWidth != width) {
            stroke = color;
            strokeWidth = width;
            invalidateSelf();
        }
        return this;
    }

    Shape ring(int color, float width) {
        ringColor = color;
        ringWidth = width;
        return this;
    }

    Shape ringColor(int color) {
        ringColor = color;
        invalidateSelf();
        return this;
    }

    Shape insetY(int inset) {
        insetY = inset;
        dirty = true;
        invalidateSelf();
        return this;
    }

    Shape setRadius(float radius) {
        return setRadii(radius, radius, radius, radius);
    }

    Shape setRadii(float topStart, float topEnd, float bottomEnd, float bottomStart) {
        radii[0] = topStart;
        radii[1] = topEnd;
        radii[2] = bottomEnd;
        radii[3] = bottomStart;
        dirty = true;
        invalidateSelf();
        return this;
    }

    Shape pressedRadius(float radius) {
        pressedRadius = radius;
        morphs = true;
        return this;
    }

    /** 0 = 静止形状，1 = 按下形状。由调用方用弹簧驱动。 */
    void morph(float value) {
        if (morph == value) return;
        morph = value;
        dirty = true;
        invalidateSelf();
    }

    @Override protected void onBoundsChange(Rect bounds) {
        dirty = true;
    }

    @Override public boolean isStateful() {
        return ringWidth > 0;
    }

    @Override protected boolean onStateChange(int[] state) {
        boolean now = false;
        for (int value : state) if (value == R.attr.state_focused) now = true;
        if (now == focused) return false;
        focused = now;
        invalidateSelf();
        return true;
    }

    private void rebuild() {
        Rect b = getBounds();
        box.set(b.left, b.top + insetY, b.right, b.bottom - insetY);
        float full = Math.min(box.width(), box.height()) / 2f;
        for (int i = 0; i < 4; i++) {
            float rest = radii[i] == FULL ? full : radii[i];
            float target = pressedRadius == FULL ? full : pressedRadius;
            float r = morphs ? rest + (target - rest) * morph : rest;
            r = Math.max(0, Math.min(r, full));
            resolved[i * 2] = r;
            resolved[i * 2 + 1] = r;
        }
        path.reset();
        path.addRoundRect(box, resolved, Path.Direction.CW);
        dirty = false;
    }

    Path path() {
        if (dirty) rebuild();
        return path;
    }

    @Override public void draw(Canvas canvas) {
        Path shape = path();
        if ((fill >>> 24) != 0) {
            fillPaint.setColor(fill);
            canvas.drawPath(shape, fillPaint);
        }
        if (strokeWidth > 0 && (stroke >>> 24) != 0) drawInset(canvas, shape, stroke, strokeWidth);
        if (focused && ringWidth > 0) drawInset(canvas, shape, ringColor, ringWidth);
    }

    /** 描边整条画在形状内侧：先按形状裁剪，再画两倍宽的线，外侧那一半被裁掉。 */
    private void drawInset(Canvas canvas, Path shape, int color, float width) {
        strokePaint.setColor(color);
        strokePaint.setStrokeWidth(width * 2);
        int save = canvas.save();
        canvas.clipPath(shape);
        canvas.drawPath(shape, strokePaint);
        canvas.restoreToCount(save);
    }

    @Override public void getOutline(Outline outline) {
        Path shape = path();
        if (Build.VERSION.SDK_INT >= 30) outline.setPath(shape);
        else if (shape.isConvex()) outline.setConvexPath(shape);
        else outline.setRoundRect(getBounds(), resolved[0]);
    }

    @Override public void setAlpha(int alpha) {}

    @Override public void setColorFilter(ColorFilter filter) {}

    @Override public int getOpacity() {
        return PixelFormat.TRANSLUCENT;
    }

    /** 与本形状同几何的不透明遮罩，给波纹裁边用（透明底的按钮也需要）。 */
    Drawable mask() {
        final Shape owner = this;
        return new Drawable() {
            private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

            @Override public void draw(Canvas canvas) {
                paint.setColor(0xFF000000);
                canvas.drawPath(owner.path(), paint);
            }

            @Override protected void onBoundsChange(Rect bounds) {
                owner.setBounds(bounds);
            }

            @Override public void setAlpha(int alpha) {}

            @Override public void setColorFilter(ColorFilter filter) {}

            @Override public int getOpacity() {
                return PixelFormat.TRANSLUCENT;
            }
        };
    }

    /** 带状态层（波纹、悬停、按压）的背景：底是本形状，波纹按同一几何裁剪。 */
    RippleDrawable pressable(int stateLayer) {
        return new RippleDrawable(ColorStateList.valueOf(stateLayer), this, mask());
    }
}
