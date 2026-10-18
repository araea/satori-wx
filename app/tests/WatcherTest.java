import com.satori.wx.core.Root;
import com.satori.wx.core.Watcher;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * root 侧订阅的真机测试（需要 su；没有时 test.sh 跳过）：在临时目录上真起一条订阅，
 * 状态文件被替换、看守状态被改写、模块目录里出现停用标记时各来一行事件；没有变化时一行也没有；
 * 结束订阅之后不留下 inotifyd 孤儿。不碰真正的模块、微信与看守目录。
 */
public final class WatcherTest {
    private static final String BASE = "/data/local/tmp/zhiyan-watch-test";
    private static int checks;

    public static void main(String[] args) throws Exception {
        String pub = BASE + "/pub", guard = BASE + "/guard", module = BASE + "/mod", update = BASE + "/upd";
        check(Root.run("set -e; rm -rf " + BASE + "; mkdir -p " + pub + " " + guard + " " + module + " " + update).ok(), "准备临时目录");
        BlockingQueue<String> lines = new LinkedBlockingQueue<>();
        BlockingQueue<Boolean> ended = new LinkedBlockingQueue<>();
        Watcher watcher = new Watcher(new Watcher.Listener() {
            @Override public void changed(String line) {
                lines.add(line);
            }

            @Override public void ended(boolean ran) {
                ended.add(ran);
            }
        });
        try {
            watcher.start(Watcher.script("zhiyan.none", BASE + "/no-cgroup", pub, guard, module, update));
            Thread.sleep(1500);
            check(watcher.running(), "订阅在跑");
            check(lines.poll(1, TimeUnit.SECONDS) == null, "没有变化时一行也没有（订阅不是轮询）");

            // 模块发布状态：写临时文件再改名，一个干净的「移入」事件。
            check(Root.run("echo '{}' > " + pub + "/state.json.tmp; mv " + pub + "/state.json.tmp " + pub + "/state.json").ok(), "写状态文件");
            String line = lines.poll(5, TimeUnit.SECONDS);
            check(line != null && line.startsWith("m\t") && line.contains("/pub"), "状态文件被替换要有事件：" + line);
            check(lines.poll(1, TimeUnit.SECONDS) == null, "一次替换只有一次事件：改名之外的那份临时文件不算");

            // 保活看守：状态文件同样是改名写入的。
            check(Root.run("echo MODE=PAUSED > " + guard + "/guard.tmp; mv " + guard + "/guard.tmp " + guard + "/guard.state").ok(), "写看守状态");
            line = lines.poll(5, TimeUnit.SECONDS);
            check(line != null && line.startsWith("m\t") && line.contains("/guard"), "看守状态变了要有事件：" + line);
            drain(lines);

            // 模块目录里出现停用标记。
            check(Root.run("touch " + module + "/disable").ok(), "放停用标记");
            line = lines.poll(5, TimeUnit.SECONDS);
            check(line != null && line.contains("disable"), "模块目录里的变化要有事件：" + line);

            check(Watcher.entriesChanged("n\t/sys/fs/cgroup/apps/uid_10419\tpid_123")
                    && Watcher.entriesChanged("d\t/sys/fs/cgroup/apps/uid_10419\tpid_123")
                    && !Watcher.entriesChanged("c\t/sys/fs/cgroup/apps/uid_10419/cgroup.events")
                    && !Watcher.entriesChanged("m\t/data/adb/satori-wx\tguard.state"), "cgroup 里增减子项才需要重新列要盯的文件");
        } finally {
            watcher.stop();
        }
        check(!watcher.running(), "结束订阅");

        // 脚本自己说盯不了（缺 inotifyd、inotifyd 一起就退）：要报成「没起来」，应用才会退回定时检查。
        ended.clear();
        Watcher broken = new Watcher(new Watcher.Listener() {
            @Override public void changed(String line) {
                lines.add(line);
            }

            @Override public void ended(boolean ran) {
                ended.add(ran);
            }
        });
        broken.start("echo " + Watcher.MARK + "; echo " + Watcher.FAILED + "; sleep 30");
        Boolean ran = ended.poll(5, TimeUnit.SECONDS);
        check(ran != null && !ran, "盯不了时要报「没起来」：" + ran);
        check(!broken.running(), "盯不了时订阅不算在跑");
        broken.stop();
        // 结束之后不留下 inotifyd：标准输入一关，整个进程组就被收掉。
        String left = "";
        for (int i = 0; i < 6; i++) {
            Thread.sleep(500);
            left = Root.run("ps -A -o PID,ARGS | grep -E '^ *[0-9]+ (timeout [0-9]+ )?inotifyd - .*zhiyan-watch-test'").out.trim();
            if (left.isEmpty()) break;
        }
        check(left.isEmpty(), "结束订阅后留下了进程：\n" + left);
        Root.run("rm -rf " + BASE);
        System.out.println("WatcherTest: " + checks + " checks（su，临时目录）");
    }

    private static void drain(BlockingQueue<String> lines) throws InterruptedException {
        while (lines.poll(300, TimeUnit.MILLISECONDS) != null) {
            // 丢掉这一步之前残余的事件。
        }
    }

    private static void check(boolean ok, String message) {
        checks++;
        if (!ok) throw new AssertionError(message);
    }
}
