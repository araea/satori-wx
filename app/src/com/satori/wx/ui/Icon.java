package com.satori.wx.ui;

import android.graphics.drawable.Drawable;
import com.satori.wx.R;

/**
 * 界面图标：全部是 Google 官方 Material Symbols Rounded（Apache-2.0）的矢量，
 * 由 tools/make-icons.py 生成为 {@code res/drawable/ms_*.xml}，颜色一律按令牌着色。
 *
 * <p>描边样式表示常态，实心样式（状态图标）表示结论——与 Material Symbols 的「fill 轴表达选中与强调」一致。
 * 图标永远不单独承载语义：装饰性的不进无障碍树，图标按钮的名称由 contentDescription 给出（WCAG 1.1.1 / 4.1.2）。
 * 方向性图标在矢量里声明了 autoMirrored，从右到左的语言里自动翻转。
 */
final class Icon {
    private Icon() {}

    static final int REFRESH = R.drawable.ms_refresh;
    static final int SETTINGS = R.drawable.ms_settings;
    static final int BACK = R.drawable.ms_arrow_back;
    static final int CLOSE = R.drawable.ms_close;
    static final int COPY = R.drawable.ms_content_copy;
    static final int SHARE = R.drawable.ms_share;
    static final int CHECK = R.drawable.ms_check;
    static final int OK = R.drawable.ms_check_circle;
    static final int WAIT = R.drawable.ms_schedule;
    static final int ERROR = R.drawable.ms_error;
    static final int WARNING = R.drawable.ms_warning;
    static final int INFO = R.drawable.ms_info;
    static final int UNKNOWN = R.drawable.ms_pending;
    static final int SHOW = R.drawable.ms_visibility;
    static final int HIDE = R.drawable.ms_visibility_off;
    static final int KEY = R.drawable.ms_key;
    static final int NEXT = R.drawable.ms_chevron_right;
    static final int OPEN = R.drawable.ms_open_in_new;
    static final int RESTART = R.drawable.ms_restart_alt;
    static final int ADD = R.drawable.ms_add;
    static final int SEARCH = R.drawable.ms_search;
    static final int PERSON = R.drawable.ms_person;
    static final int GROUP = R.drawable.ms_group;
    static final int EDIT = R.drawable.ms_edit;
    static final int LINK = R.drawable.ms_link;
    static final int FOLDER = R.drawable.ms_folder;

    static Drawable of(Tokens t, int kind, int color) {
        Drawable drawable = t.context.getDrawable(kind).mutate();
        drawable.setTint(color);
        return drawable;
    }

    /** 会话类型对应的图标：私聊、群、文件传输助手。 */
    static int talker(int kind) {
        switch (kind) {
            case com.satori.wx.core.Talker.GROUP: return GROUP;
            case com.satori.wx.core.Talker.FILE: return FOLDER;
            default: return PERSON;
        }
    }
}
