package com.satori.wx.ui;

import android.app.Dialog;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ScrollView;

/**
 * M3 基础对话框：标题、说明、可选的内容视图、右对齐的文字按钮；surface_container_high 容器、28dp 圆角。
 *
 * <p>按钮顺序服从 Android（肯定操作在最右）。从 HIG 只取交互：破坏性操作用错误色、默认焦点落在取消上。
 * 放大字号或窄屏时按钮竖排。系统返回与点遮罩都等同取消；内容过长时可滚动；宽度夹在 280–560dp。
 */
final class Dialogs {
    private Dialogs() {}

    static final class Action {
        final String label;
        final int style;
        final Runnable run;
        /** 返回 false 时对话框保持打开（例如输入校验没过）。 */
        final java.util.function.BooleanSupplier check;

        Action(String label, int style, Runnable run) {
            this(label, style, run, null);
        }

        Action(String label, int style, Runnable run, java.util.function.BooleanSupplier check) {
            this.label = label;
            this.style = style;
            this.run = run;
            this.check = check;
        }
    }

    /** @param actions 从左到右；约定第一个是「取消」，它拿默认焦点，run 可为 null */
    static Dialog show(Ui ui, String title, String message, Action... actions) {
        return show(ui, title, message, null, actions);
    }

    static Dialog show(Ui ui, String title, String message, View body, Action... actions) {
        Tokens t = ui.t;
        Dialog dialog = new Dialog(t.context);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout content = ui.column();
        content.setPadding(t.spaceXl, t.spaceXl, t.spaceXl, t.spaceLg);
        content.setBackground(new Shape(t.surfaceContainerHigh, t.shapeXl));
        content.addView(ui.heading(title, Tokens.HEADLINE_SMALL, t.onSurface), Ui.stack(0));
        if (message != null) content.addView(ui.text(message, Tokens.BODY_MEDIUM, t.onSurfaceVariant), Ui.stack(t.spaceLg));
        if (body != null) content.addView(body, Ui.stack(t.spaceXl));

        boolean stacked = ui.layout.stackActions();
        LinearLayout bar = stacked ? ui.column() : ui.row();
        if (!stacked) bar.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        Btn first = null;
        for (int i = 0; i < actions.length; i++) {
            final Action action = actions[stacked ? actions.length - 1 - i : i];
            Btn button = ui.button(action.label, action.style);
            button.setOnClickListener(v -> {
                if (action.check != null && !action.check.getAsBoolean()) return;
                dialog.dismiss();
                if (action.run != null) action.run.run();
            });
            if (action == actions[0]) first = button;
            if (stacked) {
                bar.addView(button, Ui.stack(0));
            } else {
                bar.addView(button, Ui.wrap(i == 0 ? 0 : t.spaceSm));
            }
        }
        content.addView(bar, Ui.stack(t.spaceXl));

        ScrollView scroll = new ScrollView(t.context);
        scroll.addView(content);
        dialog.setContentView(scroll);
        dialog.setCanceledOnTouchOutside(true);
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(0));
            window.setDimAmount(t.scrimPct / 100f);
            int screen = t.res.getDisplayMetrics().widthPixels;
            int width = Math.max(Math.min(t.dialogMin, screen - 2 * t.spaceLg),
                    Math.min(t.dialogMax, screen - 2 * t.spaceXl));
            window.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
            if (body != null) window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                    | WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        }
        dialog.show();
        if (body == null && first != null) {
            first.setFocusableInTouchMode(true);
            first.requestFocus();
            final View focus = first;
            // 触摸模式下保留焦点会让下一次点击先「聚焦」而不触发；拿到初始焦点后立刻恢复常规行为。
            focus.post(() -> focus.setFocusableInTouchMode(false));
        }
        return dialog;
    }
}
