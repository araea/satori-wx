package com.satori.wx.xp;

import com.satori.wx.L;
import com.satori.wx.Observe;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Locale;

/**
 * 反射补挂 WCDB 类上的换点。
 *
 * <p>为什么需要它：native 侧 {@code FindClass + GetMethodID} 对 WCDB 的类拿不到方法
 * （`method-missing`）——微信的 WCDB 真身不在 native 解析到的那个类副本里；而 Java 反射
 * 用宿主 classloader 能拿到真身（`globalTraceSQL` 就是这么工作的）。这里把
 * {@link Method} 交给 native，native 用 {@code FromReflectedMethod} 取 ArtMethod 再直写。
 *
 * <p>只读参数、零库调用，纪律与 {@link Xp} 相同。
 */
public final class ReflectPatch {

    /** {类名, 方法名, JNI 签名, native 侧目标下标}。 */
    private static final Object[][] TARGETS = {
            {"com.tencent.wcdb.core.PreparedStatement", "bindText", "(JLjava/lang/String;I)V", 7},
            {"com.tencent.wcdb.core.PreparedStatement", "bindBLOB", "(J[BI)V", 8},
            {"com.tencent.wcdb.core.Handle", "executeSQL", "(JLjava/lang/String;)Z", 9},
            {"com.tencent.wcdb.core.Database", "setCipherKey", "(J[BII)V", 10},
            {"com.tencent.wcdb.database.SQLiteConnection", "nativeSetKey", "(J[B)V", 11},
    };

    private static volatile boolean done;

    private ReflectPatch() {}

    public static void install(ClassLoader host) {
        if (done) return;
        done = true;
        int ok = 0;
        for (Object[] t : TARGETS) {
            String cls = (String) t[0];
            String name = (String) t[1];
            String sig = (String) t[2];
            int idx = (Integer) t[3];
            try {
                Class<?> c = host.loadClass(cls);
                Method found = null;
                for (Method m : c.getDeclaredMethods()) {
                    if (m.getName().equals(name) && jniSig(m).equals(sig)) {
                        found = m;
                        break;
                    }
                }
                if (found == null) {
                    Observe.log("reflectpatch: " + cls + '.' + name + sig + " not found");
                    continue;
                }
                found.setAccessible(true);
                String r = Xp.nativePatchMethod(found, idx);
                Observe.log("reflectpatch: " + cls + '.' + name + " idx=" + idx
                        + " -> " + r);
                if (r != null && r.startsWith("ok")) ok++;
            } catch (Throwable e) {
                L.e("reflectpatch failed for " + cls + '.' + name, e);
                Observe.log("reflectpatch: " + cls + '.' + name + " threw " + e);
            }
        }
        Observe.log("reflectpatch: done ok=" + ok + '/' + TARGETS.length);
    }

    /** 由 Method 生成 JNI 签名（参数 + 返回）。 */
    private static String jniSig(Method m) {
        StringBuilder sb = new StringBuilder("(");
        for (Class<?> p : m.getParameterTypes()) sb.append(jniType(p));
        sb.append(')').append(jniType(m.getReturnType()));
        return sb.toString();
    }

    private static String jniType(Class<?> c) {
        if (c == void.class) return "V";
        if (c == boolean.class) return "Z";
        if (c == byte.class) return "B";
        if (c == char.class) return "C";
        if (c == short.class) return "S";
        if (c == int.class) return "I";
        if (c == long.class) return "J";
        if (c == float.class) return "F";
        if (c == double.class) return "D";
        if (c.isArray()) return "[" + jniType(c.getComponentType());
        return "L" + c.getName().replace('.', '/') + ";";
    }

    /** 未用但保留：便于排障时打印目标表。 */
    public static String describe() {
        StringBuilder sb = new StringBuilder();
        for (Object[] t : TARGETS) {
            sb.append(String.format(Locale.US, "%s.%s idx=%d ", t[0], t[1], (Integer) t[3]));
        }
        return sb.toString();
    }

    static {
        // 保持与 Xp 的静态关系（无实际操作）。
        assert Modifier.isPublic(ReflectPatch.class.getModifiers());
    }
}
