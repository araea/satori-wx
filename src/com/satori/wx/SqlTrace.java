package com.satori.wx;

import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/**
 * WCDB 全局 SQL 追踪：{@code Database.globalTraceSQL(SQLTracer)} 是 WCDB 的公开静态 API，
 * 注册自己的 tracer 就能收到所有库执行的每条 SQL——消息的 INSERT 就在其中。
 *
 * <p>这是纯公开 API 调用，零补丁零钩子。注意它会顶掉微信自己注册的 tracer（若有）——
 * 只影响微信自己的 SQL 监控数据，不影响功能；观测阶段可接受。
 *
 * <p>SQLTracer 是宿主 dex 里的接口，这里用 {@link Proxy} 动态实现，方法签名由
 * shape dump 现场确认。
 */
public final class SqlTrace {

    private static volatile boolean installed;

    private SqlTrace() {}

    public static void install(ClassLoader host) {
        if (installed) return;
        installed = true;
        try {
            Class<?> dbCls = host.loadClass("com.tencent.wcdb.core.Database");
            Class<?> tracerIf = host.loadClass("com.tencent.wcdb.core.Database$SQLTracer");
            dumpShape(tracerIf);
            Object tracer = Proxy.newProxyInstance(SqlTrace.class.getClassLoader(),
                    new Class<?>[]{tracerIf}, (proxy, method, args) -> {
                        try {
                            StringBuilder sb = new StringBuilder("sql ");
                            sb.append(method.getName()).append('(');
                            if (args != null) {
                                for (int i = 0; i < args.length; i++) {
                                    if (i > 0) sb.append(", ");
                                    sb.append(Observe.summarize(args[i]));
                                }
                            }
                            Observe.log(sb.append(')').toString());
                        } catch (Throwable ignored) {
                            // 记录失败绝不影响转发
                        }
                        return null;   // tracer 回调按 void 用
                    });
            Method m = dbCls.getMethod("globalTraceSQL", tracerIf);
            m.invoke(null, tracer);
            Observe.log("sqltrace: globalTraceSQL registered");
            L.i("sqltrace registered");
        } catch (Throwable t) {
            L.e("sqltrace install failed", t);
            Observe.log("sqltrace: install FAILED " + t);
        }
    }

    /** 一次性 dump 接口形状（确认回调方法名与参数）。 */
    private static void dumpShape(Class<?> c) {
        try {
            StringBuilder sb = new StringBuilder("sqltrace shape ").append(c.getName())
                    .append(" methods:");
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
            L.e("sqltrace shape dump failed", t);
        }
    }
}
