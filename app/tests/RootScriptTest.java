import com.satori.wx.core.Conf;
import com.satori.wx.core.Root;

/**
 * 在临时目录上真跑一遍 root 脚本（需要 su；没有时 test.sh 跳过这项）：
 * 原子写入后内容逐字节一致、权限 0600、属主 0:0、SELinux 上下文跟随 module.prop；
 * 有待生效的更新目录时两处都写；探测脚本能读回模块状态与配置原文。不碰真正的模块目录。
 */
public final class RootScriptTest {
    private static final String BASE = "/data/local/tmp/zhiyan-root-test";
    private static int checks;

    public static void main(String[] args) throws Exception {
        String module = BASE + "/modules/satori_wx", update = BASE + "/modules_update/satori_wx";
        Root.Result setup = Root.run(String.join("\n",
                "set -e", "rm -rf " + BASE, "mkdir -p " + module + " " + update,
                "printf 'id=satori_wx\\nversion=v9.9.9\\n' > " + module + "/module.prop",
                "printf 'id=satori_wx\\nversion=v9.9.10\\n' > " + update + "/module.prop",
                "chcon u:object_r:system_file:s0 " + module + "/module.prop " + update + "/module.prop",
                "printf 'token=old\\n' > " + module + "/satori-wx.conf", "chmod 0644 " + module + "/satori-wx.conf"));
        check(setup.ok(), "准备临时目录失败：" + setup.out);
        try {
            String text = new Conf(5602, Conf.newToken()).write();
            Root.Result write = Root.run(Root.writeScript(text, module, update));
            check(write.ok(), "写入失败：" + write.out);
            for (String dir : new String[]{module, update}) {
                Root.Result stat = Root.run("stat -c '%a %u %g %C' " + dir + "/satori-wx.conf; ls -a " + dir);
                check(stat.out.startsWith("600 0 0 u:object_r:system_file:s0"), dir + " 权限 / 属主 / 上下文不对：" + stat.out);
                check(!stat.out.contains(".satori-wx.conf.new"), "临时文件没有清理：" + stat.out);
            }
            Root.Device device = Root.parse(Root.run(Root.probeScript(module, update) + "\ntouch " + module + "/disable").out);
            check(device.module && device.updatePending && device.version.equals("v9.9.9")
                    && device.updateVersion.equals("v9.9.10"), "探测读错了模块状态");
            check(text.equals(device.confText), "读回的配置与写入不一致");
            Conf back = Conf.parse(device.confText);
            check(back.port == 5602, "读回的配置解析不对");
            check(Root.parse(Root.run(Root.probeScript(module, update)).out).disabled, "停用标记没读到");
            // 模块目录不存在时必须报失败，而不是「成功地什么也没写」。
            check(!Root.run(Root.writeScript(text, BASE + "/nope", BASE + "/nope2")).ok(), "没有模块目录时应失败");
        } finally {
            Root.run("rm -rf " + BASE);
        }
        System.out.println("RootScriptTest: " + checks + " checks（su，临时目录）");
    }

    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }
}
