import com.satori.wx.core.Conf;
import com.satori.wx.core.Talker;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Conf.parse / Conf.write 与服务端 satori::ReadConfig 的一致性。
 *
 * <p>服务端是 fail-closed：它拒绝的文件会让知言服务不启动。所以每个样例都同时交给两边判定，
 * 结论（接受与否、端口、send、白名单）必须完全相同；Conf.write 写出的每个文件都必须被服务端接受。
 * 服务端判定来自 test.sh 编译的 build/tests/conf-parity（链接 ../native/server.cpp）。
 */
public final class ConfTest {
    private static final String TOKEN = "0123456789abcdef0123456789abcdef";
    private static int checks;

    public static void main(String[] args) throws Exception {
        String parity = System.getProperty("parity");
        if (parity == null) throw new AssertionError("-Dparity=<conf-parity 路径> 未设置");
        List<String> samples = new ArrayList<>(Arrays.asList(
                "token=" + TOKEN + "\n",
                "port=5601\ntoken=" + TOKEN + "\nsend=off\n",
                "port=5601\r\ntoken=" + TOKEN + "\r\nsend=on\r\nsend_allow=filehelper;123@chatroom\r\n",
                "# 注释\n\n\nport=1024\ntoken=" + TOKEN + "\n",
                "port=65535\ntoken=" + TOKEN + "\n",
                "port=65536\ntoken=" + TOKEN + "\n",
                "port=1023\ntoken=" + TOKEN + "\n",
                "port=\ntoken=" + TOKEN + "\n",
                "port= 5601\ntoken=" + TOKEN + "\n",
                "port=5601\n",
                "token=short\n",
                "token=" + TOKEN + "!\n",
                "token=" + TOKEN + "\ntoken=" + TOKEN + "\n",
                "token=" + TOKEN + "\nport=5601\nport=5602\n",
                "token=" + TOKEN + "\nsend=yes\n",
                "token=" + TOKEN + "\nsend=\n",
                "token=" + TOKEN + "\nsend_allow=\n",
                "token=" + TOKEN + "\nsend_allow=a b\n",
                "token=" + TOKEN + "\nsend_allow=wxid_a;;wxid_b;\n",
                "token=" + TOKEN + "\nsend_allow=" + repeat('a', 511) + "\n",
                "token=" + TOKEN + "\nsend_allow=" + repeat('a', 512) + "\n",
                "token=" + TOKEN + "\nunknown=1\n",
                " token=" + TOKEN + "\n",
                "token=" + TOKEN + "\n#" + repeat('x', 900) + "\n",
                "token=" + TOKEN + "\n#" + repeat('x', 1100) + "\n",
                "token=" + repeat('A', 128) + "\n",
                "token=" + repeat('A', 129) + "\n",
                "token=" + TOKEN + "\nsend_allow=中文\n",
                ""));
        // 恰好 1023 与 1024 字节：服务端读满 1024 即判失败。
        String head = "token=" + TOKEN + "\n#";
        samples.add(head + repeat('x', 1023 - head.length() - 1) + "\n");
        samples.add(head + repeat('x', 1024 - head.length() - 1) + "\n");

        // Conf.write 的输出也要过服务端。
        List<String> written = new ArrayList<>();
        written.add(new Conf(5601, TOKEN, false, new ArrayList<>()).write());
        written.add(new Conf(1024, Conf.newToken(), true, Arrays.asList("filehelper", "123@chatroom", "wxid_x-y.z")).write());
        List<String> many = new ArrayList<>();
        for (int i = 0; Conf.allowLength(many) + 20 <= Conf.MAX_ALLOW; i++) many.add("wxid_" + String.format("%014d", i));
        written.add(new Conf(65535, repeat('Z', 128), true, many).write());
        samples.addAll(written);

        File dir = Files.createTempDirectory("conf-parity").toFile();
        List<String> paths = new ArrayList<>();
        paths.add(parity);
        for (int i = 0; i < samples.size(); i++) {
            File f = new File(dir, i + ".conf");
            Files.write(f.toPath(), samples.get(i).getBytes(StandardCharsets.UTF_8));
            paths.add(f.getPath());
        }
        Process process = new ProcessBuilder(paths).redirectErrorStream(true).start();
        List<String> verdicts = new ArrayList<>();
        try (BufferedReader in = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
            for (String line; (line = in.readLine()) != null; ) verdicts.add(line);
        }
        if (process.waitFor() != 0 || verdicts.size() != samples.size()) {
            throw new AssertionError("conf-parity 输出异常：" + verdicts);
        }
        for (int i = 0; i < samples.size(); i++) {
            String expected = verdicts.get(i);
            String actual;
            try {
                Conf c = Conf.parse(samples.get(i));
                actual = "ok " + c.port + " " + (c.send ? "on" : "off") + " " + (c.allow.isEmpty() ? "-" : String.join(";", c.allow));
            } catch (Conf.Invalid invalid) {
                actual = "bad";
            }
            // 服务端保留原始的 send_allow（含空段），Java 拆成列表后去掉了空段；按语义比较。
            String normalized = expected.startsWith("ok ") ? normalize(expected) : expected;
            check(normalized.equals(actual), "样例 " + i + " 判定不一致：服务端 " + expected + " / 应用 " + actual
                    + "\n  内容：" + samples.get(i).replace("\n", "\\n").substring(0, Math.min(120, samples.get(i).length())));
        }
        for (int i = samples.size() - written.size(); i < samples.size(); i++) {
            check(verdicts.get(i).startsWith("ok "), "Conf.write 写出的文件被服务端拒绝：" + samples.get(i));
        }
        for (File f : dir.listFiles()) f.delete();
        dir.delete();

        // 写出前的防线：写不出一个坏文件。
        expectInvalid(() -> new Conf(80, TOKEN, false, new ArrayList<>()).write());
        expectInvalid(() -> new Conf(5601, "short", false, new ArrayList<>()).write());
        expectInvalid(() -> new Conf(5601, TOKEN, true, Arrays.asList("a;b")).write());
        List<String> tooMany = new ArrayList<>(many);
        tooMany.add("wxid_overflow_overflow_overflow");
        expectInvalid(() -> new Conf(5601, TOKEN, true, tooMany).write());

        // 会话 ID 规则
        check(Talker.valid("filehelper") && Talker.valid("wxid_abc") && Talker.valid("12345@chatroom"), "合法 ID 被拒");
        check(!Talker.valid("abc@chatroom") && !Talker.valid("@chatroom") && !Talker.valid("a;b")
                && !Talker.valid("") && !Talker.valid("a b") && !Talker.valid("x@openim"), "非法 ID 被接受");
        check(Talker.problem(" wxid_abc ") == null, "首尾空格应被容忍");
        check(Talker.problem("a b") != null && Talker.problem("abc@chatroom").contains("chatroom"), "ID 错误说明不对");
        check(Talker.kind("1@chatroom") == Talker.GROUP && Talker.kind("filehelper") == Talker.FILE
                && Talker.kind("wxid_a") == Talker.PERSON, "会话类型判断错误");
        check(Conf.tokenValid(Conf.newToken()) && Conf.newToken().length() == 64, "生成的令牌不合法");
        check(Conf.split("a;;b;").equals(Arrays.asList("a", "b")), "白名单拆分错误");
        check(new Conf(5601, TOKEN, true, Arrays.asList("a", "b", "a")).allow.equals(Arrays.asList("a", "b")), "白名单未去重");
        System.out.println("ConfTest: " + checks + " checks, " + samples.size() + " samples against native ReadConfig");
    }

    private static String normalize(String verdict) {
        String[] parts = verdict.split(" ", 4);
        List<String> list = parts[3].equals("-") ? new ArrayList<>() : Conf.split(parts[3]);
        return parts[0] + " " + parts[1] + " " + parts[2] + " " + (list.isEmpty() ? "-" : String.join(";", list));
    }

    private interface Body { void run() throws Exception; }

    private static void expectInvalid(Body body) throws Exception {
        try {
            body.run();
        } catch (Conf.Invalid expected) {
            checks++;
            return;
        }
        throw new AssertionError("应当拒绝写出");
    }

    private static String repeat(char c, int n) {
        char[] out = new char[n];
        Arrays.fill(out, c);
        return new String(out);
    }

    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }
}
