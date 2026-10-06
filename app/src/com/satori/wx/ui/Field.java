package com.satori.wx.ui;

import android.animation.ValueAnimator;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.text.Editable;
import android.text.InputType;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.text.method.PasswordTransformationMethod;
import android.view.Gravity;
import android.view.View;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.satori.wx.R;

/**
 * M3 描边输入框（outlined text field）：标签在框内充当提示，获得焦点或有内容时缩小并浮到上边框的缺口里；
 * 辅助文字在框下；出错时描边、标签、辅助文字换成错误色，并在框内末端出现错误图标。
 *
 * <p>无障碍：标签经 {@code hintText} 进入输入框自己的节点（读屏聚焦输入框时读出名称，标签视图本身不重复朗读）；
 * 错误经 {@code contentInvalid} + {@code error} 暴露，辅助文字是 polite live region——
 * 描边换色 + 图标 + 文字三重表达，不只靠颜色（WCAG 1.3.1 / 1.4.1 / 3.3.1 / 4.1.2）。
 * 不用框架的 {@code setError} 气泡：那是另一套视觉。
 */
final class Field extends LinearLayout {
    private static final float FLOAT_SCALE = 12f / 16f; // body large → body small

    final EditText input;
    private final Tokens t;
    private final String label;
    private final TextView labelView;
    private final TextView helper;
    private final ImageView errorMark;
    private final Frame frame;
    private final FrameLayout box;
    private final int offset;
    private String hint = "";
    private String placeholder;
    private String errorText;
    private float lift;
    private ValueAnimator motion;

    Field(Tokens tokens, String label, String placeholder, boolean secret) {
        super(tokens.context);
        this.t = tokens;
        this.label = label;
        this.placeholder = placeholder;
        setOrientation(VERTICAL);

        int line = tokens.res.getDimensionPixelSize(R.dimen.md_type_body_large_line);
        offset = Math.round(line * FLOAT_SCALE / 2f);

        box = new FrameLayout(tokens.context);
        box.setClipChildren(false);
        box.setClipToPadding(false);
        frame = new Frame();
        box.setBackground(frame);
        box.setPadding(0, offset, 0, 0);

        input = tokens.type(new EditText(tokens.context), Tokens.BODY_LARGE);
        input.setId(View.generateViewId());
        input.setSaveEnabled(false);
        input.setSingleLine(true);
        input.setTextColor(tokens.onSurface);
        input.setHintTextColor(tokens.onSurfaceVariant);
        input.setMinimumHeight(tokens.field);
        input.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
        input.setPaddingRelative(tokens.spaceLg, tokens.spaceLg, tokens.spaceLg, tokens.spaceLg);
        input.setBackground(null);
        if (secret) {
            input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            input.setTransformationMethod(PasswordTransformationMethod.getInstance());
            input.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING);
            input.setImportantForAutofill(IMPORTANT_FOR_AUTOFILL_NO);
        } else {
            input.setImeOptions(EditorInfo.IME_ACTION_NEXT);
        }
        input.setAccessibilityDelegate(new AccessibilityDelegate() {
            @Override public void onInitializeAccessibilityNodeInfo(View host, AccessibilityNodeInfo info) {
                super.onInitializeAccessibilityNodeInfo(host, info);
                info.setHintText(Field.this.label);
                info.setContentInvalid(errorText != null);
                info.setError(errorText);
            }
        });
        box.addView(input, new FrameLayout.LayoutParams(-1, -2));

        labelView = tokens.type(new TextView(tokens.context), Tokens.BODY_LARGE);
        labelView.setText(label);
        labelView.setSingleLine(true);
        labelView.setEllipsize(TextUtils.TruncateAt.END);
        labelView.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        labelView.setPivotX(0);
        labelView.setPivotY(0);
        FrameLayout.LayoutParams labelParams = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START);
        labelParams.setMarginStart(tokens.spaceLg);
        box.addView(labelView, labelParams);

        errorMark = new ImageView(tokens.context);
        errorMark.setImageDrawable(Icon.of(tokens, Icon.ERROR, tokens.error));
        errorMark.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        errorMark.setVisibility(GONE);
        FrameLayout.LayoutParams markParams = new FrameLayout.LayoutParams(tokens.icon, tokens.icon, Gravity.END | Gravity.CENTER_VERTICAL);
        markParams.setMarginEnd(tokens.spaceMd);
        box.addView(errorMark, markParams);
        addView(box, new LayoutParams(-1, -2));

        helper = Ui.live(tokens.type(new TextView(tokens.context), Tokens.BODY_SMALL));
        helper.setTextColor(tokens.onSurfaceVariant);
        LayoutParams helperParams = new LayoutParams(-1, -2);
        helperParams.topMargin = tokens.spaceXs;
        helperParams.setMarginStart(tokens.spaceLg);
        helperParams.setMarginEnd(tokens.spaceLg);
        addView(helper, helperParams);

        input.setOnFocusChangeListener((v, focused) -> refresh(true));
        input.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) { refresh(true); }
        });
        box.addOnLayoutChangeListener((v, l, top, r, b, ol, ot, or, ob) -> placeLabel());
        refresh(false);
    }

    /** 尾部图标按钮（例如令牌的显示 / 隐藏），放在输入框内侧末端。 */
    void trailing(ImageButton button) {
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(-2, -2, Gravity.END | Gravity.CENTER_VERTICAL);
        params.setMarginEnd(t.spaceXs);
        box.addView(button, params);
        ((FrameLayout.LayoutParams) errorMark.getLayoutParams()).setMarginEnd(t.touchTarget + t.spaceXs);
        input.setPaddingRelative(t.spaceLg, t.spaceLg, t.touchTarget + t.spaceXs, t.spaceLg);
    }

    void hint(String text) {
        hint = text == null ? "" : text;
        if (errorText == null) Ui.set(helper, hint);
        helper.setVisibility(TextUtils.isEmpty(helper.getText()) ? GONE : VISIBLE);
    }

    String text() {
        return input.getText().toString();
    }

    void setText(String value) {
        if (!input.getText().toString().equals(value)) {
            input.setText(value);
            input.setSelection(input.length());
        }
    }

    /** 显示或清除错误。显示时把焦点移到输入框并滚入可见区域（WCAG 3.3.1）。 */
    void error(String message) {
        errorText = message;
        Ui.set(helper, message != null ? message : hint);
        helper.setVisibility(TextUtils.isEmpty(helper.getText()) ? GONE : VISIBLE);
        errorMark.setVisibility(message != null ? VISIBLE : GONE);
        if (message != null) {
            input.requestFocus();
            input.post(() -> input.requestRectangleOnScreen(new Rect(0, 0, input.getWidth(), getHeight()), false));
        }
        refresh(false);
    }

    private boolean floated() {
        return input.hasFocus() || input.length() > 0;
    }

    private void refresh(boolean animate) {
        boolean focused = input.hasFocus();
        boolean failed = errorText != null;
        frame.focused = focused;
        frame.failed = failed;
        int labelColor = failed ? t.error : (focused ? t.primary : t.onSurfaceVariant);
        labelView.setTextColor(labelColor);
        helper.setTextColor(failed ? t.error : t.onSurfaceVariant);
        // M3：占位文字只在标签浮起、框内为空时出现。
        input.setHint(floated() && placeholder != null ? placeholder : null);
        float target = floated() ? 1 : 0;
        if (motion != null) motion.cancel();
        if (!animate || !Spring.enabled() || !isAttachedToWindow() || lift == target) {
            lift = target;
            placeLabel();
            frame.invalidateSelf();
            return;
        }
        motion = t.spatialFast.apply(ValueAnimator.ofFloat(lift, target));
        motion.addUpdateListener(a -> {
            lift = (float) a.getAnimatedValue();
            placeLabel();
        });
        motion.start();
    }

    /** 标签在「框内居中」与「上边框中线」之间插值；缺口宽度跟随标签缩放。 */
    private void placeLabel() {
        int inputHeight = input.getHeight() > 0 ? input.getHeight() : t.field;
        int labelHeight = labelView.getHeight() > 0 ? labelView.getHeight() : offset * 2;
        // 标签与输入框都从盒子的上内边距（offset）处起排：静止时在输入框里垂直居中，
        // 浮起时缩小后的中线落在上边框上。
        float restTop = (inputHeight - labelHeight) / 2f;
        float floatTop = -labelHeight * FLOAT_SCALE / 2f;
        float scale = 1 + (FLOAT_SCALE - 1) * lift;
        labelView.setTranslationY(restTop + (floatTop - restTop) * lift);
        labelView.setScaleX(scale);
        labelView.setScaleY(scale);
        int maxLabel = Math.max(0, box.getWidth() - 2 * t.spaceLg - (errorText != null ? t.icon : 0));
        if (labelView.getMaxWidth() != maxLabel && maxLabel > 0) labelView.setMaxWidth(maxLabel);
        frame.gap = lift <= 0 ? 0 : (labelView.getWidth() * FLOAT_SCALE + 2 * t.spaceXs) * lift;
        frame.invalidateSelf();
    }

    @Override protected void onDetachedFromWindow() {
        if (motion != null) motion.cancel();
        super.onDetachedFromWindow();
    }

    /** 描边框：上边框在标签处留出缺口；聚焦 2dp 主色，出错 2dp 错误色（对表面 ≥ 3:1，WCAG 1.4.11）。 */
    private final class Frame extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        boolean focused, failed;
        float gap;

        Frame() {
            paint.setStyle(Paint.Style.STROKE);
        }

        @Override public void draw(Canvas canvas) {
            Rect b = getBounds();
            float width = failed || focused ? t.focusRing : t.border;
            int color = failed ? t.error : (focused ? t.primary : t.outline);
            paint.setColor(color);
            paint.setStrokeWidth(width);
            float half = width / 2f;
            int inputBottom = offset + (input.getHeight() > 0 ? input.getHeight() : t.field);
            rect.set(b.left + half, b.top + offset + half, b.right - half, b.top + inputBottom - half);
            int save = canvas.save();
            if (gap > 0) {
                float start = t.spaceLg - t.spaceXs;
                boolean rtl = getLayoutDirection() == LAYOUT_DIRECTION_RTL;
                float left = rtl ? b.right - start - gap : b.left + start;
                canvas.clipOutRect(left, b.top, left + gap, b.top + offset + width + 1);
            }
            canvas.drawRoundRect(rect, t.shapeXs, t.shapeXs, paint);
            canvas.restoreToCount(save);
        }

        @Override public void setAlpha(int alpha) {}

        @Override public void setColorFilter(ColorFilter filter) {}

        @Override public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }
}
