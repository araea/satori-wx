package com.satori.wx.xp;

import com.satori.wx.L;
import com.satori.wx.Observe;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/**
 * 本进程的运行环境：宿主（微信）的 classloader，以及到 native 侧的 JNI 入口。
 *
 * <p>与知弦同构：native 只暴露「把指定 native 方法换成自己的实现」这一种能力（纯
 * {@code RegisterNatives}，不改 ArtMethod、不挂 hook 引擎）。这里换的是 mars 传输层：
 *
 * <ul>
 *   <li>{@code MMStnManager.OnJniSetCallback} —— 截获管理器实例（thiz）并把回调对象用
 *       {@link Proxy} 包一层（记录后委托原实现），之后 onTaskEnd/onPush 的回包都过我们；</li>
 *   <li>{@code StnManager.OnJniStartTask} —— 记录出站 Task（cmdUri + 载荷形状）。</li>
 * </ul>
 *
 * <p>状态每进程一份；native 侧在建好这个类之后自己 RegisterNatives，这里不 loadLibrary。
 */
public final class Xp {

    private static volatile ClassLoader hostLoader;
    private static volatile String processName;
    private static volatile boolean ready;

    /** 微信自己的 MMStnManager 实例（OnJniSetCallback 的 thiz），M2-2 发送要用。 */
    private static volatile Object mmStnManager;

    private Xp() {}

    public static void attach(ClassLoader host, String process) {
        hostLoader = host;
        processName = process;
        ready = true;
    }

    public static boolean attached() { return ready; }
    public static ClassLoader host() { return hostLoader; }
    public static String process() { return processName; }
    public static Object manager() { return mmStnManager; }

    // 注意：没有 System.loadLibrary。.so 由 Zygisk Next 从模块目录直接加载，不在应用的
    // 库搜索路径里；native 侧在建好这个类之后自己 RegisterNatives。

    /**
     * 装 mars 层的两个换点。幂等；未就绪（类没加载/原函数指针还没注册）时返回
     * {@code retry:…}，调用方隔一会儿再试。
     *
     * @param loader 模块自己的 classloader（父加载器是宿主的，两边的类都能查到）。
     */
    public static native String nativeInstallHooks(ClassLoader loader);

    /** native 侧对换点的说明（原函数在哪个 .so/匿名段），排障用。 */
    public static native String nativeHookInfo();

    /**
     * 周期校验：data_ 槽是否还是我们的函数；被翻回（YTAG 重新断言）就改回并计数。
     * 随时可调，幂等。
     */
    public static native String nativeVerifyHooks();

    /** 补做暂存期 setCallback 的包装（Java 就绪前错过的那次），Boot 装好钩后调一次。 */
    public static native String nativeFlushPending();

    public static String verifyHooks() {
        try {
            return nativeVerifyHooks();
        } catch (Throwable t) {
            return "unavailable";
        }
    }

    // ---- 以下由 native 在换点触发，绝不能往外抛 ----

    /**
     * {@code OnJniSetCallback} 的前置：记录实例、包装回调。返回值交给原实现——
     * 包装失败时原样返回，绝不改变微信行为。
     */
    public static Object onSetCallback(Object manager, Object callback) {
        try {
            if (manager != null && mmStnManager != manager) {
                mmStnManager = manager;
                dumpClassShape(manager.getClass(), "manager");
            }
            if (callback == null) return null;
            if (Proxy.isProxyClass(callback.getClass())) return callback; // 已包过
            Class<?>[] ifaces = callback.getClass().getInterfaces();
            if (ifaces.length == 0) {
                Observe.log("setcb: " + callback.getClass().getName()
                        + " 无接口，无法 Proxy，放行原对象");
                return callback;
            }
            StringBuilder names = new StringBuilder();
            for (Class<?> i : ifaces) {
                names.append(i.getName()).append(' ');
                dumpClassShape(i, "iface");
            }
            Object proxy = Proxy.newProxyInstance(
                    Xp.class.getClassLoader(), ifaces, new DelegateHandler(callback));
            Observe.log("setcb: wrapped " + callback.getClass().getName()
                    + " ifaces=[" + names.toString().trim() + ']');
            return proxy;
        } catch (Throwable t) {
            L.e("onSetCallback wrap failed", t);
            Observe.log("setcb: wrap FAILED " + t);
            return callback;
        }
    }

    /** {@code OnJniStartTask} 的前置：记录出站 Task 的字段形状、顺手捕获管理器实例，
     *  然后原样放行。 */
    public static void onStartTask(Object manager, Object task) {
        try {
            if (manager != null && mmStnManager == null) {
                mmStnManager = manager;
                Observe.log("manager: captured " + manager.getClass().getName()
                        + " @" + Integer.toHexString(System.identityHashCode(manager)));
                dumpInstanceFields(manager);
            }
            if (task == null) return;
            StringBuilder sb = new StringBuilder("startTask ").append(shape(task)).append(" {");
            for (Class<?> c = task.getClass(); c != null && c != Object.class; c = c.getSuperclass()) {
                for (Field f : c.getDeclaredFields()) {
                    f.setAccessible(true);
                    sb.append(f.getName()).append('=')
                      .append(Observe.summarize(f.get(task))).append(' ');
                }
            }
            Observe.log(sb.append('}').toString());
        } catch (Throwable t) {
            L.e("onStartTask dump failed", t);
        }
    }

    /** 逐包编解码的观测口（encode/decode 的进出载荷）。native 侧每包调两次。 */
    public static void onPkg(String tag, Object payload) {
        try {
            Observe.log("pkg " + tag + ' ' + Observe.summarize(payload));
        } catch (Throwable ignored) {
            // 记录失败绝不影响委托
        }
    }

    /** 一次性 dump 实例字段（找回调可能存在的 Java 侧落脚点）。 */
    private static void dumpInstanceFields(Object o) {
        try {
            StringBuilder sb = new StringBuilder("manager fields {");
            for (Class<?> c = o.getClass(); c != null && c != Object.class; c = c.getSuperclass()) {
                for (Field f : c.getDeclaredFields()) {
                    if (java.lang.reflect.Modifier.isStatic(f.getModifiers())) continue;
                    f.setAccessible(true);
                    sb.append(f.getType().getSimpleName()).append(' ').append(f.getName())
                      .append('=').append(Observe.summarize(f.get(o))).append(' ');
                }
            }
            Observe.log(sb.append('}').toString());
        } catch (Throwable t) {
            L.e("dumpInstanceFields failed", t);
        }
    }

    /** 一次性 dump 类形状：接口与声明方法（回答「CallBack 确切方法集」）。 */
    private static void dumpClassShape(Class<?> c, String tag) {
        try {
            StringBuilder sb = new StringBuilder("shape[").append(tag).append("] ")
                    .append(c.getName()).append(" methods:");
            for (Method m : c.getDeclaredMethods()) {
                sb.append(' ').append(m.getName()).append('(');
                Class<?>[] ps = m.getParameterTypes();
                for (int i = 0; i < ps.length; i++) {
                    if (i > 0) sb.append(',');
                    sb.append(ps[i].getSimpleName());
                }
                sb.append(')');
            }
            Observe.log(sb.toString());
        } catch (Throwable t) {
            L.e("dumpClassShape failed", t);
        }
    }

    private static String shape(Object o) {
        return o.getClass().getName();
    }

    /** 记录后委托：所有回调方法先过 {@link Observe}，再调原对象。 */
    private static final class DelegateHandler implements InvocationHandler {
        private final Object target;

        DelegateHandler(Object target) { this.target = target; }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            try {
                StringBuilder sb = new StringBuilder("cb ").append(method.getName()).append('(');
                if (args != null) {
                    for (int i = 0; i < args.length; i++) {
                        if (i > 0) sb.append(", ");
                        sb.append(Observe.summarize(args[i]));
                    }
                }
                Observe.log(sb.append(')').toString());
            } catch (Throwable ignored) {
                // 记录失败绝不影响委托
            }
            return method.invoke(target, args);
        }
    }
}
