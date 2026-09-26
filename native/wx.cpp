// 知言（satori-wx）的 Zygisk 原生模块——M2 观测版。
//
// 与知弦（satori-qq）同构的纯 JNI 路线：内嵌 dex + InMemoryDexClassLoader 引导，
// 然后用 RegisterNatives 把 mars 传输层的几个 native 方法换成自己的实现。
// 不改写任何 ArtMethod、不挂 hook 引擎、无 trampoline。
//
// 换点（表驱动，见 kTargets）：
//   - MMStnManager.OnJniSetCallback —— 截获管理器实例（thiz），把回调对象交给 Java 侧
//     Proxy 包装（记录后委托原实现）；
//   - StnManager.OnJniStartTask —— 记录出站 Task 后原样放行；
//   - StnManager/AccountManager.OnJniSetCallback —— 备用的回调观察点。
//
// 每个目标独立装：类没加载或原函数指针还没注册时回 retry，Java 侧负责重试。
#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <android/log.h>

#include "zygisk.hpp"
#include "jni_helpers.h"

// 由 build.sh 生成的 dex_blob.S 提供（.incbin 进来的 classes.dex）。
extern "C" const uint8_t satori_dex_start[];
extern "C" const uint8_t satori_dex_end[];

static const char *kNLogFile = "/data/data/com.tencent.mm/files/satori-wx-observe/native.log";
static const char *kTarget = "com.tencent.mm";
static const char *kXpClass = "com.satori.wx.xp.Xp";

static JavaVM *g_vm = nullptr;
static char g_process[256] = {0};
static bool g_bootstrap_started = false;

static jclass g_xp_class = nullptr;            // 全局引用
static jmethodID g_mid_on_set_cb = nullptr;    // static Object onSetCallback(Object, Object)
static jmethodID g_mid_on_start_task = nullptr;// static void onStartTask(Object, Object)
static jmethodID g_mid_on_pkg = nullptr;       // static void onPkg(String, Object)

// Java 助手就绪前的 setCallback 调用暂存（早钩在 natives 注册瞬间就位，而 Java 侧要等
// Application；这半秒窗口内的调用先记下，等 Boot 装好钩后补做包装并回注原实现）。
struct Target;
static jobject g_pending_thiz = nullptr;
static jobject g_pending_cb = nullptr;
static Target *g_pending_t = nullptr;
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_java_ready = 0;

// ---- 日志 -------------------------------------------------------------------------

static void NLog(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, WX_TAG, "%s", buf);
    FILE *f = fopen(kNLogFile, "ae");
    if (f == nullptr) return;
    fprintf(f, "%s\n", buf);
    fclose(f);
}

// ---- ArtMethod 数据位：data_ 的定位与分类 -------------------------------------------
//
// jmethodID 即 ArtMethod*；arm64 上 data_（native 方法注册后的 JNI 入口）在第 2 个字
// （字节偏移 16）——知弦在 QQ 上验证过的同一定位。

struct MapInfo {
    bool exec;
    char path[512];
};

static bool MappingOf(const void *p, MapInfo *info) {
    info->exec = false;
    info->path[0] = '\0';
    if (p == nullptr) return false;
    FILE *f = fopen("/proc/self/maps", "re");
    if (f == nullptr) return false;
    char line[1024];
    auto target = reinterpret_cast<uintptr_t>(p);
    bool found = false;
    while (fgets(line, sizeof(line), f) != nullptr) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (target < start || target >= end) continue;
        info->exec = perms[2] == 'x';
        const char *sp = strchr(line, ' ');
        for (int i = 0; sp != nullptr && i < 4; i++) sp = strchr(sp + 1, ' ');
        if (sp != nullptr) {
            while (*sp == ' ') sp++;
            snprintf(info->path, sizeof(info->path), "%s", sp);
            size_t n = strlen(info->path);
            while (n > 0 && (info->path[n - 1] == '\n' || info->path[n - 1] == ' ')) {
                info->path[--n] = '\0';
            }
        }
        found = true;
        break;
    }
    fclose(f);
    return found;
}

// ---- 换点表 -------------------------------------------------------------------------

enum Kind { K_SET_CB, K_START_TASK, K_ENCODE, K_DECODE, K_LOG };

struct Target {
    const char *cls;
    const char *method;
    const char *sig;
    Kind kind;
    bool required;
    // 状态
    bool installed;
    void *orig;
    uintptr_t *slot;     // ArtMethod 的 data_ 槽（verify 用）
    char note[160];
};

// 原实现取自微信自己的 .so；回调对象在进原实现之前先过 Java 侧的包装。

static Target kTargets[] = {
        {"com.tencent.mars.mm.MMStnManager", "OnJniSetCallback",
         "(Ljava/lang/Object;)V", K_SET_CB, true,
         false, nullptr, nullptr, "class-missing"},
        {"com.tencent.mars.stn.StnManager", "OnJniStartTask",
         "(Lcom/tencent/mars/stn/StnManager$Task;)V", K_START_TASK, true,
         false, nullptr, nullptr, "class-missing"},
        {"com.tencent.mars.stn.StnManager", "OnJniSetCallback",
         "(Ljava/lang/Object;)V", K_SET_CB, false,
         false, nullptr, nullptr, "class-missing"},
        {"com.tencent.mars.account.AccountManager", "OnJniSetCallback",
         "(Ljava/lang/Object;)V", K_SET_CB, false,
         false, nullptr, nullptr, "class-missing"},
        {"com.tencent.mars.account.AccountManager", "OnJniEncodeWxPkg",
         "([BI)[B", K_ENCODE, true,
         false, nullptr, nullptr, "class-missing"},
        {"com.tencent.mars.account.AccountManager", "OnJniDecodeWxPkg",
         "([B[I[I)[B", K_DECODE, true,
         false, nullptr, nullptr, "class-missing"},
        // AppManager 是 mars 的中枢管理器（M1 边界里也有它的 SetCallback）——微信很可能
        // 经它注册回调，v0.1.4 之前一直没挂钩。
        {"com.tencent.mars.app.AppManager", "OnJniSetCallback",
         "(Ljava/lang/Object;)V", K_SET_CB, true,
         false, nullptr, nullptr, "class-missing"},
        // WCDB 的值侧：INSERT 的 SQL 只有 ?N 占位符，值在 bind 调用里。聊天内容是 TEXT 列。
        {"com.tencent.wcdb.core.PreparedStatement", "bindText",
         "(JLjava/lang/String;I)V", K_LOG, false,
         false, nullptr, nullptr, "class-missing"},
        {"com.tencent.wcdb.core.PreparedStatement", "bindBLOB",
         "(J[BI)V", K_LOG, false,
         false, nullptr, nullptr, "class-missing"},
        {"com.tencent.wcdb.core.Handle", "executeSQL",
         "(JLjava/lang/String;)Z", K_LOG, false,
         false, nullptr, nullptr, "class-missing"},
};
static constexpr int kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

// ---- 替换实现 ----------------------------------------------------------------------

static void MySetCallback(JNIEnv *env, jobject thiz, jobject callback, Target *t) {
    if (!__atomic_load_n(&g_java_ready, __ATOMIC_SEQ_CST)) {
        // Java 助手未就绪：暂存最新的 (thiz, callback)，原样放行（微信行为不变）。
        pthread_mutex_lock(&g_pending_mu);
        if (g_pending_thiz != nullptr) env->DeleteGlobalRef(g_pending_thiz);
        if (g_pending_cb != nullptr) env->DeleteGlobalRef(g_pending_cb);
        g_pending_thiz = thiz ? env->NewGlobalRef(thiz) : nullptr;
        g_pending_cb = callback ? env->NewGlobalRef(callback) : nullptr;
        g_pending_t = t;
        pthread_mutex_unlock(&g_pending_mu);
        NLog("setcb pre-ready: stashed cb=%p thiz=%p (%s)", (void *) callback, (void *) thiz,
             t->method);
        using OrigFn0 = void (*)(JNIEnv *, jobject, jobject);
        if (t->orig != nullptr) {
            reinterpret_cast<OrigFn0>(t->orig)(env, thiz, callback);
        }
        return;
    }
    jobject forward = callback;
    if (g_mid_on_set_cb != nullptr) {
        forward = env->CallStaticObjectMethod(g_xp_class, g_mid_on_set_cb, thiz, callback);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            forward = callback;
        }
    }
    using OrigFn = void (*)(JNIEnv *, jobject, jobject);
    if (t->orig != nullptr) {
        reinterpret_cast<OrigFn>(t->orig)(env, thiz, forward);
    }
}

static void MyStartTask(JNIEnv *env, jobject thiz, jobject task, Target *t) {
    if (g_mid_on_start_task != nullptr) {
        env->CallStaticVoidMethod(g_xp_class, g_mid_on_start_task, thiz, task);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
    using OrigFn = void (*)(JNIEnv *, jobject, jobject);
    if (t->orig != nullptr) {
        reinterpret_cast<OrigFn>(t->orig)(env, thiz, task);
    }
}

// 逐包编解码：OnJniEncodeWxPkg/OnJniDecodeWxPkg 每个网络包都会经过（不像 SetCallback 只在
// 启动时调一次），装晚了也赶得上；decode 出来的就是解密载荷。记进出，原样转发。
static void LogPkg(JNIEnv *env, const char *tag, jobject payload) {
    if (g_mid_on_pkg == nullptr) return;
    jstring s = env->NewStringUTF(tag);
    if (s == nullptr) { env->ExceptionClear(); return; }
    env->CallStaticVoidMethod(g_xp_class, g_mid_on_pkg, s, payload);
    env->DeleteLocalRef(s);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

static jbyteArray MyEncode(JNIEnv *env, jobject thiz, jbyteArray in, jint len, Target *t) {
    LogPkg(env, "encode.in", in);
    using OrigFn = jbyteArray (*)(JNIEnv *, jobject, jbyteArray, jint);
    jbyteArray out = t->orig != nullptr
        ? reinterpret_cast<OrigFn>(t->orig)(env, thiz, in, len) : nullptr;
    LogPkg(env, "encode.out", out);
    return out;
}

static jbyteArray MyDecode(JNIEnv *env, jobject thiz, jbyteArray in, jintArray a1,
                           jintArray a2, Target *t) {
    LogPkg(env, "decode.in", in);
    using OrigFn = jbyteArray (*)(JNIEnv *, jobject, jbyteArray, jintArray, jintArray);
    jbyteArray out = t->orig != nullptr
        ? reinterpret_cast<OrigFn>(t->orig)(env, thiz, in, a1, a2) : nullptr;
    LogPkg(env, "decode.out", out);
    return out;
}

// 为每个 Target 生成一个具名 JNI 函数（JNINativeMethod 需要独立地址）。
static void WxSetCb0(JNIEnv *env, jobject thiz, jobject a0) {
    MySetCallback(env, thiz, a0, &kTargets[0]);
}
static void WxStartTask1(JNIEnv *env, jobject thiz, jobject a0) {
    MyStartTask(env, thiz, a0, &kTargets[1]);
}
static void WxSetCb2(JNIEnv *env, jobject thiz, jobject a0) {
    MySetCallback(env, thiz, a0, &kTargets[2]);
}
static void WxSetCb3(JNIEnv *env, jobject thiz, jobject a0) {
    MySetCallback(env, thiz, a0, &kTargets[3]);
}
static jbyteArray WxEncode4(JNIEnv *env, jobject thiz, jbyteArray a0, jint a1) {
    return MyEncode(env, thiz, a0, a1, &kTargets[4]);
}
static jbyteArray WxDecode5(JNIEnv *env, jobject thiz, jbyteArray a0, jintArray a1,
                            jintArray a2) {
    return MyDecode(env, thiz, a0, a1, a2, &kTargets[5]);
}
static void WxSetCb6(JNIEnv *env, jobject thiz, jobject a0) {
    MySetCallback(env, thiz, a0, &kTargets[6]);
}

// WCDB 值侧：参数在 native 侧就地格式化（不走 Java 助手，随时可用），经 onPkg 落盘。
static void LogArgs(JNIEnv *env, const char *tag, const char *fmt, ...) {
    char buf[560] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jstring s = env->NewStringUTF(buf);
    if (s == nullptr) { env->ExceptionClear(); return; }
    LogPkg(env, tag, s);
    env->DeleteLocalRef(s);
}

static void WxBindText7(JNIEnv *env, jobject thiz, jlong h, jstring s, jint i) {
    const char *c = s ? env->GetStringUTFChars(s, nullptr) : nullptr;
    LogArgs(env, "bindText", "h=%lld i=%d text=%s", (long long) h, i, c ? c : "null");
    if (c) env->ReleaseStringUTFChars(s, c);
    using F = void (*)(JNIEnv *, jobject, jlong, jstring, jint);
    if (kTargets[7].orig) reinterpret_cast<F>(kTargets[7].orig)(env, thiz, h, s, i);
}
static void WxBindBlob8(JNIEnv *env, jobject thiz, jlong h, jbyteArray a, jint i) {
    char buf[160] = {0};
    if (a) {
        jsize n = env->GetArrayLength(a);
        jbyte tmp[16] = {0};
        env->GetByteArrayRegion(a, 0, n < 16 ? n : 16, tmp);
        int off = snprintf(buf, sizeof(buf), "h=%lld i=%d blob(%d)[", (long long) h, i, n);
        for (int k = 0; k < n && k < 8 && off < (int) sizeof(buf) - 4; k++) {
            off += snprintf(buf + off, sizeof(buf) - off, "%02x", (unsigned char) tmp[k]);
        }
        snprintf(buf + off, sizeof(buf) - off, "%s]", n > 8 ? "…" : "");
    }
    LogArgs(env, "bindBlob", "%s", buf);
    using F = void (*)(JNIEnv *, jobject, jlong, jbyteArray, jint);
    if (kTargets[8].orig) reinterpret_cast<F>(kTargets[8].orig)(env, thiz, h, a, i);
}
static jboolean WxExecSql9(JNIEnv *env, jobject thiz, jlong h, jstring s) {
    const char *c = s ? env->GetStringUTFChars(s, nullptr) : nullptr;
    LogArgs(env, "execSQL", "h=%lld sql=%s", (long long) h, c ? c : "null");
    if (c) env->ReleaseStringUTFChars(s, c);
    using F = jboolean (*)(JNIEnv *, jobject, jlong, jstring);
    return kTargets[9].orig ? reinterpret_cast<F>(kTargets[9].orig)(env, thiz, h, s) : JNI_FALSE;
}

static void *TargetFnFor(int idx) {
    switch (idx) {
        case 0: return (void *) &WxSetCb0;
        case 1: return (void *) &WxStartTask1;
        case 2: return (void *) &WxSetCb2;
        case 3: return (void *) &WxSetCb3;
        case 4: return (void *) &WxEncode4;
        case 5: return (void *) &WxDecode5;
        case 6: return (void *) &WxSetCb6;
        case 7: return (void *) &WxBindText7;
        case 8: return (void *) &WxBindBlob8;
        case 9: return (void *) &WxExecSql9;
    }
    return nullptr;
}

// ---- 安装 ---------------------------------------------------------------------------
//
// 为什么不经过 RegisterNatives：M2 观测确认微信自己的 libYTAGReflectLiveCheck.so 接管了全局
// RegisterNatives——我们的注册先进它的 wrapper、落回它自己的 trampoline，data_ 永远不会变成
// 我们的函数（主进程 317 次尝试全部 `slot 2 did not take`，确定性复现）。
//
// data_ 正是 RegisterNatives 自己写的那个槽；绕开 wrapper 直接写这一个指针，是同一类数据补丁，
// 依旧不改任何代码字节、无 trampoline。原指针照旧保存用于委托。

/** data_ 指针分类。返回 true = libart/JNI 存根（natives 还没注册，retry）；
 *  false = 可委托的真函数（第三方 .so 或匿名可执行映射，path 给出归属）。 */
static bool IsArtStub(uintptr_t p, char *path, size_t path_size) {
    MapInfo info;
    if (!MappingOf((const void *) p, &info) || !info.exec) return true;
    if (info.path[0] == '\0') {
        snprintf(path, path_size, "(anon-exec)");
        return false;   // 匿名可执行映射：真函数（YTAG 的 trampoline 常落在这）
    }
    if (strstr(info.path, "/system/") != nullptr) return true;
    if (strstr(info.path, "/apex/") != nullptr) return true;
    if (strstr(info.path, ".oat") != nullptr) return true;
    snprintf(path, path_size, "%s", info.path);
    return false;
}

static const char *BaseOf(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/**
 * 直写一个目标的 data_。返回：
 *   0 = 已装（幂等）；1 = 本次装上；2 = 类还没加载（retry）；
 *   3 = data_ 还是存根（natives 未注册，retry）；4 = 其它失败。
 */
/**
 * 对已解析的类直写 data_。返回码：0=已装（幂等）；1=本次装上；
 * 3=data_ 还是存根（retry）；4=其它失败。
 */
static int WriteDataFor(JNIEnv *env, Target *t, int idx, jclass cls) {
    if (t->installed) return 0;
    jmethodID mid = env->GetMethodID(cls, t->method, t->sig);
    if (mid == nullptr) {
        env->ExceptionClear();
        snprintf(t->note, sizeof(t->note), "method-missing %s.%s", t->cls, t->method);
        return 4;
    }

    auto *words = reinterpret_cast<uintptr_t *>(mid);
    uintptr_t cur = __atomic_load_n(&words[2], __ATOMIC_SEQ_CST);
    uintptr_t ours = reinterpret_cast<uintptr_t>(TargetFnFor(idx));
    if (cur == ours) {   // 上一次重试已写成功
        t->installed = true;
        t->slot = &words[2];
        snprintf(t->note, sizeof(t->note), "already-ours %s", t->method);
        return 0;
    }

    char path[512];
    if (IsArtStub(cur, path, sizeof(path))) {
        snprintf(t->note, sizeof(t->note), "stub data_=%p (%s.%s)", (void *) cur,
                 t->cls, t->method);
        return 3;
    }

    // 诊断快照：写之前的 8 个字（回答「data_ 到底在哪、原来是什么」）。
    char before[240] = {0};
    size_t boff = 0;
    for (int i = 0; i < 8 && boff < sizeof(before) - 1; i++) {
        boff += (size_t) snprintf(before + boff, sizeof(before) - boff, "%s%lx",
                                  i ? " " : "", (unsigned long) words[i]);
    }

    t->orig = (void *) cur;
    t->slot = &words[2];
    __atomic_store_n(&words[2], ours, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&words[2], __ATOMIC_SEQ_CST) != ours) {
        snprintf(t->note, sizeof(t->note), "write did not stick %s", t->method);
        return 4;
    }
    t->installed = true;
    snprintf(t->note, sizeof(t->note), "installed orig=%p in %s (%s)", t->orig,
             BaseOf(path), t->method);
    NLog("install: %s.%s %s | before: %s", t->cls, t->method, t->note, before);
    return 1;
}

static int InstallOne(JNIEnv *env, jobject loader, Target *t, int idx) {
    if (t->installed) return 0;
    jclass cls = env->FindClass(t->cls);   // 内嵌 loader 的父是宿主，直接找得到
    if (cls == nullptr) {
        env->ExceptionClear();
        snprintf(t->note, sizeof(t->note), "class-missing %s", t->cls);
        return 2;
    }
    int r = WriteDataFor(env, t, idx, cls);
    env->DeleteLocalRef(cls);
    return r;
}

/** 装全部目标；返回给 Java 的状态串（required 没全装上则带 retry: 前缀）。 */
static jstring NativeInstallHooks(JNIEnv *env, jclass, jobject loader) {
    char status[1500] = {0};
    size_t off = 0;
    bool all_required = true;
    for (int i = 0; i < kTargetCount; i++) {
        int r = InstallOne(env, loader, &kTargets[i], i);
        if (kTargets[i].required && r >= 2) all_required = false;
        off += (size_t) snprintf(status + off, sizeof(status) - off, "%s%s=%s",
                                 off ? "; " : "", kTargets[i].method, kTargets[i].note);
        if (off >= sizeof(status) - 1) break;
    }
    // 状态没变化就不重复落盘（重试循环每 500ms 一次，全量打会把 native.log 刷成几百 KB）。
    static char last_status[1600] = {0};
    if (strncmp(last_status, status, sizeof(last_status) - 1) != 0) {
        snprintf(last_status, sizeof(last_status), "%s", status);
        NLog("installHooks: %s", status);
    }
    char out[1600];
    snprintf(out, sizeof(out), "%s%s", all_required ? "ok " : "retry:", status);
    return env->NewStringUTF(out);
}

/**
 * 补做暂存期 setCallback 的包装：Java 就绪后由 Boot 调一次。取暂存的 (thiz, callback)，
 * 走一遍正常包装，再把包装后的对象回注给原实现（native 侧的回调被替换成我们的 Proxy，
 * 原回调被包在里面，行为不变）。
 */
static jstring NativeFlushPending(JNIEnv *env, jclass) {
    pthread_mutex_lock(&g_pending_mu);
    jobject thiz = g_pending_thiz;
    jobject cb = g_pending_cb;
    Target *t = g_pending_t;
    g_pending_thiz = nullptr;
    g_pending_cb = nullptr;
    g_pending_t = nullptr;
    pthread_mutex_unlock(&g_pending_mu);
    if (thiz == nullptr || cb == nullptr || t == nullptr || t->orig == nullptr) {
        if (thiz != nullptr) env->DeleteGlobalRef(thiz);
        if (cb != nullptr) env->DeleteGlobalRef(cb);
        return env->NewStringUTF("flush: nothing pending");
    }
    jobject wrapped = cb;
    if (g_mid_on_set_cb != nullptr) {
        wrapped = env->CallStaticObjectMethod(g_xp_class, g_mid_on_set_cb, thiz, cb);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            wrapped = cb;
        }
    }
    using OrigFn = void (*)(JNIEnv *, jobject, jobject);
    reinterpret_cast<OrigFn>(t->orig)(env, thiz, wrapped);
    env->DeleteGlobalRef(thiz);
    env->DeleteGlobalRef(cb);
    NLog("pending setcb flushed via %s (wrapped=%s)", t->method,
         wrapped != cb ? "yes" : "no");
    return env->NewStringUTF(wrapped != cb ? "flush: wrapped" : "flush: passthrough");
}

/**
 * 周期校验：data_ 是否还是我们的函数。被翻回（YTAG 重新断言的话会这样）就改回并计数，
 * 返回状态串。这是「YTAG 会不会反扑」的直接观测口。
 */
static jstring NativeVerifyHooks(JNIEnv *env, jclass) {
    static int g_flips = 0;
    char status[1500] = {0};
    size_t off = 0;
    for (int i = 0; i < kTargetCount && off < sizeof(status) - 1; i++) {
        Target *t = &kTargets[i];
        if (!t->installed || t->slot == nullptr) continue;
        uintptr_t ours = reinterpret_cast<uintptr_t>(TargetFnFor(i));
        uintptr_t cur = __atomic_load_n(t->slot, __ATOMIC_SEQ_CST);
        if (cur != ours) {
            __atomic_store_n(t->slot, ours, __ATOMIC_SEQ_CST);
            g_flips++;
            NLog("verify: %s flipped to %p, rewrote (%d time)", t->method, (void *) cur,
                 g_flips);
            off += (size_t) snprintf(status + off, sizeof(status) - off, "%s%s=FLIPPED(%d)",
                                     off ? "; " : "", t->method, g_flips);
        }
    }
    char out[1600];
    snprintf(out, sizeof(out), "stable flips=%d %s", g_flips, status);
    return env->NewStringUTF(out);
}

static jstring NativeHookInfo(JNIEnv *env, jclass) {
    char buf[512] = {0};
    size_t off = 0;
    for (int i = 0; i < kTargetCount && off < sizeof(buf) - 1; i++) {
        off += (size_t) snprintf(buf + off, sizeof(buf) - off, "%s%s", i ? "; " : "",
                                 kTargets[i].note);
    }
    return env->NewStringUTF(buf);
}

// ---- 全局 RegisterNatives 表槽：抢在 SetCallback 之前就位 ----------------------------
//
// SetCallback 在 mars 初始化时毫秒级就被调用，Boot 的 500ms 重试循环追不上（v0.1.2 实测
// setcb 零条）。这里把 libart 函数表的 RegisterNatives 槽也换一层（M1 探针验证过的数据
// 补丁手法）：微信注册 natives 的那一刻，同步直写我们的函数——时序上必然早于 SetCallback。
// 目标装齐后立刻把表还原。

static const JNINativeInterface *g_jni_table = nullptr;
static void **g_rn_slot = nullptr;
static void *g_orig_rn = nullptr;
static uintptr_t g_rn_page = 0;
static size_t g_rn_page_size = 0;
static int g_rn_prot = -1;
static volatile int g_rn_active = 0;
static pthread_mutex_t g_rn_mu = PTHREAD_MUTEX_INITIALIZER;

static bool AllRequiredInstalled() {
    for (int i = 0; i < kTargetCount; i++) {
        if (kTargets[i].required && !kTargets[i].installed) return false;
    }
    return true;
}

static void RestoreGlobalRN() {
    if (__atomic_exchange_n(&g_rn_active, 0, __ATOMIC_SEQ_CST) == 0) return;
    mprotect((void *) g_rn_page, g_rn_page_size, g_rn_prot | PROT_WRITE);
    *g_rn_slot = g_orig_rn;
    if (g_rn_prot != (PROT_READ | PROT_WRITE)) {
        mprotect((void *) g_rn_page, g_rn_page_size, g_rn_prot);
    }
    NLog("global RegisterNatives restored");
}

static jint MyGlobalRegisterNatives(JNIEnv *env, jclass clazz,
                                    const JNINativeMethod *methods, jint n) {
    using OrigRn = jint (*)(JNIEnv *, jclass, const JNINativeMethod *, jint);
    jint r = g_orig_rn != nullptr
        ? reinterpret_cast<OrigRn>(g_orig_rn)(env, clazz, methods, n) : JNI_ERR;
    if (r == JNI_OK && __atomic_load_n(&g_rn_active, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&g_rn_mu);
        for (jint i = 0; i < n; i++) {
            const char *name = methods[i].name ? methods[i].name : "";
            const char *sig = methods[i].signature ? methods[i].signature : "";
            for (int k = 0; k < kTargetCount; k++) {
                Target *t = &kTargets[k];
                if (t->installed || strcmp(name, t->method) != 0 ||
                    strcmp(sig, t->sig) != 0) {
                    continue;
                }
                // 此时 data_ 已落成微信（或 YTAG）的实现；同步直写，必然早于 SetCallback。
                if (WriteDataFor(env, t, k, clazz) == 1) {
                    NLog("early-hook via RegisterNatives: %s.%s", t->cls, t->method);
                }
            }
        }
        bool done = AllRequiredInstalled();
        pthread_mutex_unlock(&g_rn_mu);
        if (done) RestoreGlobalRN();
    }
    return r;
}

static bool PatchGlobalRN(JNIEnv *env) {
    g_jni_table = env->functions;
    auto *slot = &g_jni_table->RegisterNatives;
    g_rn_slot = reinterpret_cast<void **>(
            const_cast<void *>(reinterpret_cast<const void *>(slot)));
    g_orig_rn = *g_rn_slot;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    g_rn_page_size = (size_t) ps;
    g_rn_page = (uintptr_t) g_rn_slot & ~((uintptr_t) ps - 1);
    int prot = -1;
    FILE *f = fopen("/proc/self/maps", "re");
    if (f != nullptr) {
        char line[1024];
        while (fgets(line, sizeof(line), f) != nullptr) {
            uintptr_t s = 0, e = 0;
            char perms[8] = {0};
            if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3) continue;
            if ((uintptr_t) g_rn_slot >= s && (uintptr_t) g_rn_slot < e) {
                prot = 0;
                if (strchr(perms, 'r')) prot |= PROT_READ;
                if (strchr(perms, 'w')) prot |= PROT_WRITE;
                if (strchr(perms, 'x')) prot |= PROT_EXEC;
                break;
            }
        }
        fclose(f);
    }
    if (prot < 0) { NLog("global RN: cannot read page prot, skip"); return false; }
    g_rn_prot = prot;
    if (mprotect((void *) g_rn_page, g_rn_page_size, prot | PROT_WRITE) != 0) {
        NLog("global RN: mprotect failed: %s", strerror(errno));
        return false;
    }
    *g_rn_slot = (void *) &MyGlobalRegisterNatives;
    if (prot != (PROT_READ | PROT_WRITE)) {
        mprotect((void *) g_rn_page, g_rn_page_size, prot);
    }
    __atomic_store_n(&g_rn_active, 1, __ATOMIC_SEQ_CST);
    NLog("global RegisterNatives wrapped (orig=%p)", g_orig_rn);
    return true;
}

/** 120 秒兜底：目标没装齐也把表还原，Boot 的重试直写循环继续兜底。 */
static void *GlobalRnWatchdog(void *) {
    for (int i = 0; i < 120 && __atomic_load_n(&g_rn_active, __ATOMIC_SEQ_CST); i++) {
        sleep(1);
    }
    RestoreGlobalRN();
    return nullptr;
}

static void StartEarly(JNIEnv *env) {
    if (PatchGlobalRN(env)) {
        pthread_t t;
        if (pthread_create(&t, nullptr, GlobalRnWatchdog, nullptr) == 0) pthread_detach(t);
    }
}

// ---- 引导（与知弦同构） -------------------------------------------------------------

/** 轮询宿主的 Application：currentApplication() → currentActivityThread().getApplication()
 *  → mInitialApplication。 */
static jobject WaitForApplication(JNIEnv *env, int timeout_ms) {
    jclass at_cls = env->FindClass("android/app/ActivityThread");
    if (at_cls == nullptr) { env->ExceptionClear(); return nullptr; }
    jmethodID current_app = env->GetStaticMethodID(at_cls, "currentApplication",
                                                   "()Landroid/app/Application;");
    env->ExceptionClear();
    jmethodID current_at = env->GetStaticMethodID(at_cls, "currentActivityThread",
                                                  "()Landroid/app/ActivityThread;");
    env->ExceptionClear();
    jmethodID get_app = env->GetMethodID(at_cls, "getApplication",
                                         "()Landroid/app/Application;");
    env->ExceptionClear();
    jfieldID initial_app = env->GetFieldID(at_cls, "mInitialApplication",
                                           "Landroid/app/Application;");
    env->ExceptionClear();
    if (current_app == nullptr && current_at == nullptr) {
        NLog("ActivityThread has no currentApplication/currentActivityThread");
        return nullptr;
    }
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        jobject a = nullptr;
        if (current_app != nullptr) {
            a = env->CallStaticObjectMethod(at_cls, current_app);
            if (env->ExceptionCheck()) { env->ExceptionClear(); a = nullptr; }
        }
        if (a == nullptr && current_at != nullptr) {
            jobject at = env->CallStaticObjectMethod(at_cls, current_at);
            if (env->ExceptionCheck()) { env->ExceptionClear(); at = nullptr; }
            if (at != nullptr) {
                if (get_app != nullptr) {
                    a = env->CallObjectMethod(at, get_app);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); a = nullptr; }
                }
                if (a == nullptr && initial_app != nullptr) {
                    a = env->GetObjectField(at, initial_app);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); a = nullptr; }
                }
            }
        }
        if (a != nullptr) return a;
        usleep(20 * 1000);
    }
    return nullptr;
}

static jobject HostLoaderFromApplication(JNIEnv *env, jobject app) {
    jclass context_cls = env->FindClass("android/content/Context");
    jmethodID get_loader = env->GetMethodID(context_cls, "getClassLoader",
                                            "()Ljava/lang/ClassLoader;");
    if (get_loader == nullptr) { env->ExceptionClear(); return nullptr; }
    jobject loader = env->CallObjectMethod(app, get_loader);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return loader;
}

static jobject MakeEmbeddedLoader(JNIEnv *env, jobject parent) {
    const auto *begin = satori_dex_start;
    size_t size = static_cast<size_t>(satori_dex_end - satori_dex_start);
    if (size == 0) { NLog("embedded dex is empty"); return nullptr; }
    jobject buffer = env->NewDirectByteBuffer(const_cast<uint8_t *>(begin), size);
    if (buffer == nullptr) { NLog("NewDirectByteBuffer failed"); return nullptr; }
    jclass loader_cls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (loader_cls == nullptr) { env->ExceptionClear(); return nullptr; }
    jmethodID ctor = env->GetMethodID(loader_cls, "<init>",
                                      "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (ctor == nullptr) { env->ExceptionClear(); return nullptr; }
    jobject loader = env->NewObject(loader_cls, ctor, buffer, parent);
    if (loader == nullptr) { env->ExceptionClear(); return nullptr; }
    return loader;
}

static bool StartJava(JNIEnv *env, jobject loader, const char *process) {
    jclass boot = WxLoadClass(env, loader, "com.satori.wx.Boot");
    if (boot == nullptr) { NLog("com.satori.wx.Boot not found in embedded dex"); return false; }
    jmethodID start = env->GetStaticMethodID(boot, "start",
                                             "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    if (start == nullptr) { NLog("Boot.start(String, ClassLoader) not found"); return false; }

    jclass xp = WxLoadClass(env, loader, kXpClass);
    if (xp == nullptr) { NLog("Xp class not found"); return false; }
    static const JNINativeMethod kXpMethods[] = {
            {"nativeInstallHooks", "(Ljava/lang/ClassLoader;)Ljava/lang/String;",
             reinterpret_cast<void *>(&NativeInstallHooks)},
            {"nativeHookInfo", "()Ljava/lang/String;",
             reinterpret_cast<void *>(&NativeHookInfo)},
            {"nativeVerifyHooks", "()Ljava/lang/String;",
             reinterpret_cast<void *>(&NativeVerifyHooks)},
            {"nativeFlushPending", "()Ljava/lang/String;",
             reinterpret_cast<void *>(&NativeFlushPending)},
    };
    if (env->RegisterNatives(xp, kXpMethods, 4) != JNI_OK) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        NLog("RegisterNatives(Xp) failed");
        return false;
    }
    g_xp_class = static_cast<jclass>(env->NewGlobalRef(xp));
    g_mid_on_set_cb = env->GetStaticMethodID(
            xp, "onSetCallback",
            "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    g_mid_on_start_task = env->GetStaticMethodID(xp, "onStartTask",
                                                 "(Ljava/lang/Object;Ljava/lang/Object;)V");
    g_mid_on_pkg = env->GetStaticMethodID(xp, "onPkg",
                                          "(Ljava/lang/String;Ljava/lang/Object;)V");
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    __atomic_store_n(&g_java_ready, 1, __ATOMIC_SEQ_CST);

    jstring proc = env->NewStringUTF(process);
    env->CallStaticVoidMethod(boot, start, proc, loader);
    env->DeleteLocalRef(proc);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    NLog("Boot.start(%s) returned", process);
    return true;
}

static void *BootstrapThread(void *) {
    bool attached = false;
    JNIEnv *env = WxGetEnv(g_vm, &attached);
    if (env == nullptr) { NLog("cannot attach bootstrap thread"); return nullptr; }

    // 先放开 hidden API：下面要问的 ActivityThread 那几个入口都是 hidden 的。
    WxExemptHiddenApis(env);

    jobject app = WaitForApplication(env, 120000);
    if (app == nullptr) { NLog("Application never appeared"); WxReleaseEnv(g_vm, attached); return nullptr; }
    NLog("application ready in %s", g_process);

    jobject host = HostLoaderFromApplication(env, app);
    if (host == nullptr) { NLog("no host classloader"); WxReleaseEnv(g_vm, attached); return nullptr; }
    NLog("host classloader captured");

    // 内嵌 dex 的父加载器用宿主的：模块代码可以直接按名字引用微信的类。
    jobject loader = MakeEmbeddedLoader(env, host);
    if (loader == nullptr) { NLog("cannot create embedded dex loader"); WxReleaseEnv(g_vm, attached); return nullptr; }

    StartJava(env, loader, g_process);
    WxReleaseEnv(g_vm, attached);
    return nullptr;
}

// ---- Zygisk 模块 -------------------------------------------------------------------

class WxModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        (void) api;
        env_ = env;
        env->GetJavaVM(&g_vm);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (args == nullptr || args->nice_name == nullptr || env_ == nullptr) return;
        const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (name == nullptr) return;
        if (strncmp(name, kTarget, strlen(kTarget)) == 0) {
            strncpy(g_process, name, sizeof(g_process) - 1);
        }
        env_->ReleaseStringUTFChars(args->nice_name, name);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (g_process[0] == '\0' || g_bootstrap_started) return;
        g_bootstrap_started = true;
        StartEarly(env_);   // 抢在微信任何业务代码之前包住全局 RegisterNatives
        pthread_t t;
        if (pthread_create(&t, nullptr, BootstrapThread, nullptr) == 0) {
            pthread_detach(t);
            NLog("bootstrap thread started for %s", g_process);
        } else {
            NLog("cannot start bootstrap thread");
        }
    }

private:
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(WxModule)
