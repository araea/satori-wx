import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Design Tokens 与组件规范里机器可验的部分：
 * WCAG 2.2 AA 对比度（深浅两套）、两套角色集合一致、刻度单调、触达下限，
 * 以及「界面代码只经 R 取令牌、不写字面颜色、不按名字查资源」。
 *
 * <p>配对清单就是界面里真实出现的前景 / 背景组合；新增组合时先在这里加一行。
 */
public final class DesignTokenTest {
    /** 文字：≥ 4.5:1（WCAG 1.4.3，按最严的普通字号算）。 */
    private static final String[][] TEXT = {
            {"on_primary", "primary"}, {"on_primary_container", "primary_container"},
            {"on_secondary_container", "secondary_container"}, {"on_tertiary_container", "tertiary_container"},
            {"on_error", "error"}, {"on_error_container", "error_container"},
            {"on_success", "success"}, {"on_success_container", "success_container"},
            {"on_warning", "warning"}, {"on_warning_container", "warning_container"},
            {"on_surface", "surface"}, {"on_surface", "surface_container_low"}, {"on_surface", "surface_container"},
            {"on_surface", "surface_container_high"}, {"on_surface", "surface_container_highest"},
            {"on_surface_variant", "surface"}, {"on_surface_variant", "surface_container_low"},
            {"on_surface_variant", "surface_container"}, {"on_surface_variant", "surface_container_high"},
            {"on_surface_variant", "surface_container_highest"},
            {"primary", "surface"}, {"primary", "surface_container_low"}, {"primary", "surface_container"},
            {"primary", "surface_container_high"},
            {"error", "surface"}, {"error", "surface_container_low"}, {"error", "surface_container"},
            {"error", "surface_container_high"},
            {"inverse_on_surface", "inverse_surface"}, {"inverse_primary", "inverse_surface"},
            // 添加会话页里被选中的行：secondary_container 底上的标题与辅助文字
            {"on_surface", "secondary_container"}, {"on_surface_variant", "secondary_container"},
    };

    /** 非文字：承担识别控件边界或状态的图形 ≥ 3:1（WCAG 1.4.11）。 */
    private static final String[][] NON_TEXT = {
            {"outline", "surface"}, {"outline", "surface_container_low"}, {"outline", "surface_container"},
            {"outline", "surface_container_high"},
            {"primary", "surface"}, {"primary", "surface_container_low"}, {"primary", "surface_container"},
            {"primary", "secondary_container"}, {"error", "surface"}, {"error", "surface_container_low"},
            {"success", "surface_container"}, {"warning", "surface_container"}, {"error", "surface_container"},
            {"on_surface_variant", "surface_container"},
            // 状态卡片上的徽章：on_*_container 的形状放在同语调的容器上
            {"on_success_container", "success_container"}, {"on_warning_container", "warning_container"},
            {"on_error_container", "error_container"},
    };

    private static int checks;

    public static void main(String[] args) throws Exception {
        File root = new File(System.getProperty("app", "."));
        Map<String, Integer> light = colors(new File(root, "res/values/tokens.xml"));
        Map<String, Integer> dark = colors(new File(root, "res/values-night/tokens.xml"));
        check(light.keySet().equals(dark.keySet()), "深浅两套颜色角色不一致：" + light.keySet() + " / " + dark.keySet());
        for (Map<String, Integer> scheme : List.of(light, dark)) {
            String name = scheme == light ? "浅色" : "深色";
            for (String[] pair : TEXT) ratio(scheme, pair, 4.5, name);
            for (String[] pair : NON_TEXT) ratio(scheme, pair, 3.0, name);
        }

        String dimens = read(new File(root, "res/values/dimens.xml"));
        int[] space = {dp(dimens, "md_space_2xs"), dp(dimens, "md_space_xs"), dp(dimens, "md_space_sm"), dp(dimens, "md_space_md"),
                dp(dimens, "md_space_lg"), dp(dimens, "md_space_xl"), dp(dimens, "md_space_2xl"), dp(dimens, "md_space_3xl")};
        int[] shape = {dp(dimens, "md_shape_xs"), dp(dimens, "md_shape_sm"), dp(dimens, "md_shape_md"), dp(dimens, "md_shape_lg"),
                dp(dimens, "md_shape_lg_increased"), dp(dimens, "md_shape_xl"), dp(dimens, "md_shape_xl_increased"), dp(dimens, "md_shape_2xl")};
        increasing(space, "间距");
        increasing(shape, "形状");
        check(dp(dimens, "md_size_touch_target") >= 48, "触达下限必须 ≥ 48dp（WCAG 2.5.8 / HIG 44pt）");
        check(dp(dimens, "md_size_list_one_line") >= 48 && dp(dimens, "md_size_button_medium") >= 48, "可点的容器不能低于触达下限");
        check(dp(dimens, "md_size_focus_ring") >= 2, "焦点环至少 2dp（WCAG 2.4.13 的参考值）");

        // 界面代码：没有字面颜色，没有按名字查资源。
        Pattern literal = Pattern.compile("(#[0-9A-Fa-f]{6,8}\\b)|(0x[0-9A-Fa-f]{8}\\b)|Color\\.(rgb|argb|parseColor)\\(|Color\\.(RED|BLUE|GREEN|BLACK|WHITE)");
        List<String> offenders = new ArrayList<>();
        for (File f : new File(root, "src/com/satori/wx/ui").listFiles()) {
            String text = read(f);
            // Tokens 是令牌本身的读口与颜色运算（状态层混色），只有它可以做通道运算。
            if (f.getName().equals("Tokens.java")) continue;
            Matcher m = literal.matcher(text.replaceAll("(?s)/\\*.*?\\*/", "").replaceAll("//[^\\n]*", ""));
            while (m.find()) {
                // 纯黑遮罩（波纹裁边）不是颜色语义，允许 0xFF000000。
                if (m.group().equals("0xFF000000")) continue;
                offenders.add(f.getName() + ": " + m.group());
            }
            if (text.contains("getIdentifier(")) offenders.add(f.getName() + ": getIdentifier");
        }
        check(offenders.isEmpty(), "界面代码里出现字面颜色或按名字查资源：" + offenders);
        System.out.println("DesignTokenTest: " + checks + " checks（" + TEXT.length + " 对文字 + " + NON_TEXT.length + " 对非文字 × 深浅两套）");
    }

    private static void ratio(Map<String, Integer> scheme, String[] pair, double min, String name) {
        Integer fg = scheme.get(pair[0]), bg = scheme.get(pair[1]);
        check(fg != null && bg != null, name + "缺少角色 " + pair[0] + " 或 " + pair[1]);
        double value = contrast(fg, bg);
        check(value >= min, String.format("%s %s / %s 对比度 %.2f < %.1f", name, pair[0], pair[1], value, min));
    }

    private static Map<String, Integer> colors(File file) throws Exception {
        Map<String, Integer> out = new LinkedHashMap<>();
        Matcher m = Pattern.compile("<color name=\"md_(\\w+)\">#([0-9A-Fa-f]{6,8})</color>").matcher(read(file));
        while (m.find()) out.put(m.group(1), (int) Long.parseLong(m.group(2).length() == 6 ? "FF" + m.group(2) : m.group(2), 16));
        return out;
    }

    private static int dp(String dimens, String name) {
        Matcher m = Pattern.compile("<dimen name=\"" + name + "\">(\\d+)dp</dimen>").matcher(dimens);
        if (!m.find()) throw new AssertionError("缺少 " + name);
        return Integer.parseInt(m.group(1));
    }

    private static void increasing(int[] values, String name) {
        for (int i = 1; i < values.length; i++) check(values[i] > values[i - 1], name + "刻度必须单调递增");
    }

    static double contrast(int a, int b) {
        double la = luminance(a), lb = luminance(b);
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
    }

    static double luminance(int color) {
        double[] w = {0.2126, 0.7152, 0.0722};
        double value = 0;
        for (int i = 0; i < 3; i++) {
            double c = ((color >> (16 - i * 8)) & 255) / 255.0;
            value += w[i] * (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
        }
        return value;
    }

    private static String read(File file) throws Exception {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }
}
