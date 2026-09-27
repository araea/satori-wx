package com.satori.wx.ui;

import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.BaseAdapter;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import com.satori.wx.R;
import com.satori.wx.core.Api;
import com.satori.wx.core.Talker;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * 「添加会话」：M3 全屏对话框。左上关闭、右上确认，中间是搜索栏、筛选按钮组与可多选的会话列表。
 *
 * <p>联系人从知言服务读（{@code guild.list} / {@code friend.list}），读不到时仍可手动输入 ID——
 * 服务没起来恰恰是最需要改白名单的时候，不能让选择页因此变成死路。已经在白名单里的会话显示为已选中且不可改。
 * 关闭时如果选了东西，先确认再丢弃（HIG：不静默丢弃用户的输入）。
 */
final class PickerPage {
    interface Actions {
        void closePicker();
        void picked(List<String> talkers);
        void reloadContacts();
    }

    static final int ALL = 0;
    static final int GROUPS = 1;
    static final int PEOPLE = 2;

    final LinearLayout root;
    final TopBar bar;
    private final Ui ui;
    private final Tokens t;
    private final Actions actions;
    private final EditText query;
    private final ImageButton clear;
    private final Btn confirm;
    private final Btn[] filters = new Btn[3];
    private final ListView list;
    private final Adapter adapter = new Adapter();
    private final LinearLayout state;
    private final LoadingIndicator loading;
    private final TextView stateTitle, stateDetail;
    private final Btn retry;
    private final Item manual;

    private final Set<String> existing = new LinkedHashSet<>();
    private final Set<String> selected = new LinkedHashSet<>();
    private List<Api.Contact> all = Collections.emptyList();
    private final List<Api.Contact> shown = new ArrayList<>();
    private int filter = ALL;
    private boolean ready;
    private String failure;

    PickerPage(Ui ui, Actions actions) {
        this.ui = ui;
        this.t = ui.t;
        this.actions = actions;

        root = ui.column();
        root.setId(R.id.picker);
        root.setBackgroundColor(t.surface);
        root.setClickable(true);
        bar = new TopBar(ui, "添加会话", t.surface, t.surfaceContainer);
        ImageButton close = ui.iconButton(Icon.CLOSE, "关闭", t.onSurface);
        close.setId(R.id.picker_close);
        close.setOnClickListener(v -> requestClose());
        bar.navigation(close);
        bar.pinTitle();
        confirm = ui.button("添加", Btn.TEXT);
        confirm.setId(R.id.picker_confirm);
        confirm.setOnClickListener(v -> actions.picked(new ArrayList<>(selected)));
        LinearLayout.LayoutParams confirmParams = Ui.wrap(0);
        confirmParams.setMarginEnd(t.spaceSm);
        bar.action(confirm);
        confirm.setLayoutParams(confirmParams);
        root.addView(bar, new LinearLayout.LayoutParams(-1, -2));

        // 限宽的内容栏：宽屏上一行不会长到难读。
        LinearLayout column = Pages.content(ui);
        column.setPadding(0, 0, 0, 0);
        FrameLayout center = new FrameLayout(t.context);
        center.addView(column, new FrameLayout.LayoutParams(-1, -1, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        root.addView(center, new LinearLayout.LayoutParams(-1, 0, 1));
        int gutter = ui.layout.gutter();

        // ---- 搜索栏（M3 search bar）----
        LinearLayout search = ui.row();
        search.setMinimumHeight(t.search);
        search.setBackground(new Shape(t.surfaceContainerHigh, Shape.FULL));
        search.setPaddingRelative(t.spaceLg, 0, t.spaceXs, 0);
        search.addView(ui.glyph(Icon.SEARCH, t.onSurfaceVariant, t.icon));
        query = t.type(new EditText(t.context), Tokens.BODY_LARGE);
        query.setId(R.id.picker_query);
        query.setBackground(null);
        query.setSingleLine(true);
        query.setHint("搜索名字或 ID");
        query.setHintTextColor(t.onSurfaceVariant);
        query.setTextColor(t.onSurface);
        query.setInputType(InputType.TYPE_CLASS_TEXT);
        query.setImeOptions(EditorInfo.IME_ACTION_SEARCH);
        query.setPaddingRelative(t.spaceLg, t.spaceMd, t.spaceSm, t.spaceMd);
        query.setSaveEnabled(false);
        query.setMinimumHeight(t.search);
        // 整条搜索栏都是输入框的触达区（56dp），而不只是文字那一行。
        search.addView(query, new LinearLayout.LayoutParams(0, t.search, 1));
        clear = ui.iconButton(Icon.CLOSE, "清除搜索", t.onSurfaceVariant);
        clear.setVisibility(View.GONE);
        clear.setOnClickListener(v -> query.setText(""));
        search.addView(clear, Ui.wrap(0));
        LinearLayout.LayoutParams searchParams = Ui.stack(t.spaceSm);
        searchParams.leftMargin = searchParams.rightMargin = gutter;
        column.addView(search, searchParams);

        // ---- 筛选（M3E 连接按钮组，单选）----
        String[] labels = {"全部", "群聊", "联系人"};
        for (int i = 0; i < filters.length; i++) {
            final int index = i;
            filters[i] = ui.button(labels[i], Btn.TONAL).toggle();
            filters[i].setOnClickListener(v -> setFilter(index, true));
        }
        filters[ALL].setId(R.id.picker_filter_all);
        filters[GROUPS].setId(R.id.picker_filter_groups);
        filters[PEOPLE].setId(R.id.picker_filter_people);
        LinearLayout group = ui.connected(filters);
        LinearLayout.LayoutParams groupParams = Ui.stack(t.spaceMd);
        groupParams.leftMargin = groupParams.rightMargin = gutter;
        column.addView(group, groupParams);

        // ---- 列表 ----
        list = new ListView(t.context);
        list.setId(R.id.picker_list);
        list.setDivider(new ColorDrawable(0));
        list.setDividerHeight(t.space2xs);
        list.setSelector(new ColorDrawable(0));
        list.setClipToPadding(false);
        list.setPadding(gutter, t.spaceMd, gutter, t.space2xl);
        list.setScrollBarStyle(View.SCROLLBARS_OUTSIDE_OVERLAY);
        list.setItemsCanFocus(true);
        manual = new Item(t, Item.ACTION, "手动输入 ID", "wxid、微信号或「数字@chatroom」").leading(Icon.EDIT, t.onSurfaceVariant);
        manual.setId(R.id.picker_manual);
        manual.corners(t.shapeLgIncreased, t.shapeLgIncreased);
        manual.setOnClickListener(v -> manualEntry());
        LinearLayout footer = ui.column();
        footer.setPadding(0, t.spaceLg, 0, 0);
        footer.addView(manual, Ui.stack(0));
        list.addFooterView(footer, null, false);
        list.setAdapter(adapter);
        column.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        bar.follow(list);

        // ---- 加载 / 出错 / 无结果 ----
        state = ui.column();
        state.setGravity(Gravity.CENTER_HORIZONTAL);
        state.setPadding(gutter, t.space2xl, gutter, t.space2xl);
        loading = new LoadingIndicator(t, t.primary);
        state.addView(loading, new LinearLayout.LayoutParams(t.loading, t.loading));
        stateTitle = Ui.live(ui.text("", Tokens.TITLE_MEDIUM, t.onSurface));
        stateTitle.setGravity(Gravity.CENTER);
        state.addView(stateTitle, Ui.stack(t.spaceLg));
        stateDetail = ui.text("", Tokens.BODY_MEDIUM, t.onSurfaceVariant);
        stateDetail.setGravity(Gravity.CENTER);
        state.addView(stateDetail, Ui.stack(t.spaceXs));
        retry = ui.button("重试", Btn.TONAL).icon(Icon.REFRESH);
        retry.setOnClickListener(v -> actions.reloadContacts());
        state.addView(retry, new LinearLayout.LayoutParams(-2, -2));
        ((LinearLayout.LayoutParams) retry.getLayoutParams()).topMargin = t.spaceMd;
        // ListView 的 header 自身不能 GONE（仍会占位）：包一层，只收起里面的内容。
        FrameLayout header = new FrameLayout(t.context);
        header.addView(state, new FrameLayout.LayoutParams(-1, -2));
        list.addHeaderView(header, null, false);

        query.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                clear.setVisibility(s.length() > 0 ? View.VISIBLE : View.GONE);
                apply();
            }
        });
        setFilter(ALL, false);
        updateConfirm();
    }

    /** 打开时调用：清空上一次的选择，记下已经在白名单里的会话。 */
    void open(List<String> current) {
        existing.clear();
        existing.addAll(current);
        selected.clear();
        query.setText("");
        setFilter(ALL, false);
        list.setSelection(0);
        updateConfirm();
        apply();
    }

    /** 联系人读完（或失败）。failure 非空时显示出错状态，但手动输入始终可用。 */
    void contacts(List<Api.Contact> value, String failure) {
        this.failure = failure;
        this.ready = value != null || failure != null;
        all = value == null ? Collections.emptyList() : value;
        apply();
    }

    private void setFilter(int index, boolean animate) {
        filter = index;
        for (int i = 0; i < filters.length; i++) filters[i].setChecked(i == index, animate);
        apply();
    }

    private void apply() {
        shown.clear();
        String needle = query.getText().toString().trim().toLowerCase(Locale.ROOT);
        boolean hasHelper = false;
        for (Api.Contact c : all) if (Talker.FILE_HELPER.equals(c.id)) hasHelper = true;
        List<Api.Contact> source = new ArrayList<>();
        if (!hasHelper && ready && failure == null) {
            source.add(new Api.Contact(Talker.FILE_HELPER, "文件传输助手", "", Talker.FILE));
        }
        source.addAll(all);
        for (Api.Contact c : source) {
            if (filter == GROUPS && c.kind != Talker.GROUP) continue;
            if (filter == PEOPLE && c.kind == Talker.GROUP) continue;
            if (!needle.isEmpty() && !c.name.toLowerCase(Locale.ROOT).contains(needle)
                    && !c.id.toLowerCase(Locale.ROOT).contains(needle)
                    && !c.detail.toLowerCase(Locale.ROOT).contains(needle)) continue;
            shown.add(c);
        }
        adapter.notifyDataSetChanged();

        boolean busy = !ready;
        boolean failed = failure != null;
        boolean empty = ready && !failed && shown.isEmpty();
        state.setVisibility(busy || failed || empty ? View.VISIBLE : View.GONE);
        loading.setVisibility(busy ? View.VISIBLE : View.GONE);
        retry.setVisibility(failed ? View.VISIBLE : View.GONE);
        if (busy) {
            Ui.set(stateTitle, "正在读取联系人…");
            Ui.set(stateDetail, "从微信里的知言服务读取群聊与联系人。");
        } else if (failed) {
            Ui.set(stateTitle, "读不到联系人");
            Ui.set(stateDetail, failure + "。仍可在下方手动输入会话 ID。");
        } else if (empty) {
            Ui.set(stateTitle, needle.isEmpty() ? "这里没有会话" : "没有匹配「" + query.getText().toString().trim() + "」的会话");
            Ui.set(stateDetail, "换个关键词，或在下方手动输入 ID。");
        }
    }

    private void updateConfirm() {
        confirm.setEnabled(!selected.isEmpty());
        Ui.set(confirm, selected.isEmpty() ? "添加" : "添加（" + selected.size() + "）");
    }

    private void manualEntry() {
        final Field field = new Field(t, "会话 ID", "wxid_… 或 123…@chatroom", false);
        field.input.setImeOptions(EditorInfo.IME_ACTION_DONE);
        field.input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        field.hint("私聊填对方的 wxid 或微信号，群聊填「数字@chatroom」。");
        Dialogs.show(ui, "手动输入 ID", null, field,
                new Dialogs.Action("取消", Btn.TEXT, null),
                new Dialogs.Action("添加", Btn.TEXT, () -> {
                    String id = field.text().trim();
                    if (existing.contains(id)) return;
                    actions.picked(Collections.singletonList(id));
                }, () -> {
                    String problem = Talker.problem(field.text());
                    if (problem == null && existing.contains(field.text().trim())) problem = "它已经在白名单里了";
                    field.error(problem);
                    return problem == null;
                }));
        field.input.requestFocus();
    }

    /** 关闭：选了东西就先确认。返回 true 表示已经处理（关闭或弹出确认）。 */
    boolean requestClose() {
        if (selected.isEmpty()) {
            actions.closePicker();
            return true;
        }
        Dialogs.show(ui, "放弃所选的 " + selected.size() + " 个会话？", "它们不会加入白名单。",
                new Dialogs.Action("取消", Btn.TEXT, null),
                new Dialogs.Action("放弃", Btn.DANGER, actions::closePicker));
        return true;
    }

    // ------------------------------------------------------------------ 实例状态

    void saveState(Bundle out) {
        out.putStringArrayList("picker_existing", new ArrayList<>(existing));
        out.putStringArrayList("picker_selected", new ArrayList<>(selected));
        out.putString("picker_query", query.getText().toString());
        out.putInt("picker_filter", filter);
    }

    void restoreState(Bundle state) {
        ArrayList<String> current = state.getStringArrayList("picker_existing");
        open(current == null ? Collections.emptyList() : current);
        ArrayList<String> chosen = state.getStringArrayList("picker_selected");
        if (chosen != null) selected.addAll(chosen);
        query.setText(state.getString("picker_query", ""));
        setFilter(state.getInt("picker_filter", ALL), false);
        updateConfirm();
    }

    // ------------------------------------------------------------------ 列表

    private final class Adapter extends BaseAdapter {
        @Override public int getCount() {
            return shown.size();
        }

        @Override public Object getItem(int position) {
            return shown.get(position);
        }

        @Override public long getItemId(int position) {
            return shown.get(position).id.hashCode();
        }

        @Override public boolean hasStableIds() {
            return true;
        }

        @Override public boolean areAllItemsEnabled() {
            return false;
        }

        @Override public boolean isEnabled(int position) {
            return false; // 点击由行自己处理（它是复选框），ListView 不另画选中态
        }

        @Override public View getView(int position, View convert, ViewGroup parent) {
            Item item = convert instanceof Item ? (Item) convert : new Item(t, Item.CHECK, "", "");
            final Api.Contact c = shown.get(position);
            boolean already = existing.contains(c.id);
            item.setHeadline(c.name);
            String detail = already ? "已在白名单" : (TextUtils.isEmpty(c.detail) ? c.id : c.detail + " · " + c.id);
            item.setSupporting(detail);
            int container = c.kind == Talker.GROUP ? t.tertiaryContainer : c.kind == Talker.FILE ? t.primaryContainer : t.secondaryContainer;
            int ink = c.kind == Talker.GROUP ? t.onTertiaryContainer : c.kind == Talker.FILE ? t.onPrimaryContainer : t.onSecondaryContainer;
            item.avatar(Icon.talker(c.kind), container, ink);
            item.setOnToggle(null);
            item.setChecked(already || selected.contains(c.id), false);
            item.setEnabled(!already);
            item.corners(position == 0 ? t.shapeLgIncreased : t.shapeXs,
                    position == shown.size() - 1 ? t.shapeLgIncreased : t.shapeXs);
            item.container(!already && selected.contains(c.id) ? t.secondaryContainer : t.surfaceContainer);
            item.setOnToggle((row, checked) -> {
                if (checked) selected.add(c.id);
                else selected.remove(c.id);
                row.container(checked ? t.secondaryContainer : t.surfaceContainer);
                updateConfirm();
            });
            return item;
        }
    }
}
