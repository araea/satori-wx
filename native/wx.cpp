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

// ---- ArtMethod 数据位：读出已注册的原 JNI 函数（只读不写） ---------------------------
//
// 与知弦的 InstallSsoHook 相同的认法：值必须落在可执行映射里，且不在 libart / boot.oat /
// 本模块自己的 .so。原实现必须在某个第三方 .so 里，否则装了也转交不了原实现。

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

static bool LooksLikeThirdPartyJni(void *p, char *path, size_t path_size) {
    MapInfo info;
    if (!MappingOf(p, &info) || !info.exec) return false;
    if (info.path[0] == '\0') return false;                       // 匿名映射：说不清是谁的
    if (strstr(info.path, "/system/") != nullptr) return false;
    if (strstr(info.path, "/apex/") != nullptr) return false;
    if (strstr(info.path, ".oat") != nullptr) return false;
    if (strstr(info.path, "libsatori") != nullptr) return false;  // 自己的不算
    return strstr(info.path, ".so") != nullptr;
}

static int FindJniEntrySlot(uintptr_t *words, int kWords, void **out, char *path,
                            size_t path_size) {
    for (int i = 0; i < kWords; ++i) {
        auto *p = reinterpret_cast<void *>(words[i]);
        char candidate[512];
        if (!LooksLikeThirdPartyJni(p, candidate, sizeof(candidate))) continue;
        *out = p;
        snprintf(path, path_size, "%s", candidate);
        return i;
    }
    return -1;
}

// ---- 换点表 -------------------------------------------------------------------------

enum Kind { K_SET_CB, K_START_TASK };

struct Target {
    const char *cls;
    const char *method;
    const char *sig;
    Kind kind;
    bool required;
    // 状态
    bool installed;
    void *orig;
    char note[160];
};

// 原实现取自微信自己的 .so；回调对象在进原实现之前先过 Java 侧的包装。

static Target kTargets[] = {
        {"com.tencent.mars.mm.MMStnManager", "OnJniSetCallback",
         "(Ljava/lang/Object;)V", K_SET_CB, true,
         false, nullptr, "class-missing"},
        {"com.tencent.mars.stn.StnManager", "OnJniStartTask",
         "(Lcom/tencent/mars/stn/StnManager$Task;)V", K_START_TASK, true,
         false, nullptr, "class-missing"},
        {"com.tencent.mars.stn.StnManager", "OnJniSetCallback",
         "(Ljava/lang/Object;)V", K_SET_CB, false,
         false, nullptr, "class-missing"},
        {"com.tencent.mars.account.AccountManager", "OnJniSetCallback",
         "(Ljava/lang/Object;)V", K_SET_CB, false,
         false, nullptr, "class-missing"},
};
static constexpr int kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

// ---- 替换实现 ----------------------------------------------------------------------

static void MySetCallback(JNIEnv *env, jobject thiz, jobject callback, Target *t) {
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

static void *TargetFnFor(int idx) {
    switch (idx) {
        case 0: return (void *) &WxSetCb0;
        case 1: return (void *) &WxStartTask1;
        case 2: return (void *) &WxSetCb2;
        case 3: return (void *) &WxSetCb3;
    }
    return nullptr;
}

// ---- 安装 ---------------------------------------------------------------------------

/**
 * 装一个目标。返回：
 *   0 = 已装（幂等）；1 = 本次装上；2 = 类还没加载（retry）；3 = natives 还没注册（retry）；
 *   4 = 其它失败（不重试）。
 */
static int InstallOne(JNIEnv *env, jobject loader, Target *t, int idx) {
    if (t->installed) return 0;
    jclass cls = env->FindClass(t->cls);   // 内嵌 loader 的父是宿主，直接找得到
    if (cls == nullptr) {
        env->ExceptionClear();
        snprintf(t->note, sizeof(t->note), "class-missing %s", t->cls);
        return 2;
    }
    jmethodID mid = env->GetMethodID(cls, t->method, t->sig);
    if (mid == nullptr) {
        env->ExceptionClear();
        snprintf(t->note, sizeof(t->note), "method-missing %s.%s", t->cls, t->method);
        return 4;
    }

    auto *words = reinterpret_cast<uintptr_t *>(mid);
    constexpr int kWords = 8;
    void *orig = nullptr;
    char lib[512] = {0};
    int slot = FindJniEntrySlot(words, kWords, &orig, lib, sizeof(lib));
    if (slot < 0) {
        snprintf(t->note, sizeof(t->note),
                 "natives-not-registered-yet %s.%s", t->cls, t->method);
        return 3;
    }

    JNINativeMethod m{const_cast<char *>(t->method), const_cast<char *>(t->sig),
                      TargetFnFor(idx)};
    if (env->RegisterNatives(cls, &m, 1) != JNI_OK) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        snprintf(t->note, sizeof(t->note), "RegisterNatives-rejected %s", t->method);
        return 4;
    }
    auto *fn = reinterpret_cast<uintptr_t *>(TargetFnFor(idx));
    if (words[slot] != *fn) {
        JNINativeMethod back{const_cast<char *>(t->method), const_cast<char *>(t->sig), orig};
        env->RegisterNatives(cls, &back, 1);
        env->ExceptionClear();
        snprintf(t->note, sizeof(t->note), "slot %d did not take our fn %s", slot, t->method);
        return 4;
    }
    t->installed = true;
    t->orig = orig;
    const char *base = strrchr(lib, '/');
    base = base ? base + 1 : lib;
    snprintf(t->note, sizeof(t->note), "installed slot=%d orig=%p in %s (%s)", slot, orig,
             base, t->method);
    NLog("install: %s.%s %s", t->cls, t->method, t->note);
    return 1;
}

/** 装全部目标；返回给 Java 的状态串（required 没全装上则带 retry: 前缀）。 */
static jstring NativeInstallHooks(JNIEnv *env, jclass, jobject loader) {
    char status[512] = {0};
    size_t off = 0;
    bool all_required = true;
    for (int i = 0; i < kTargetCount; i++) {
        int r = InstallOne(env, loader, &kTargets[i], i);
        if (kTargets[i].required && r >= 2) all_required = false;
        off += (size_t) snprintf(status + off, sizeof(status) - off, "%s%s=%s",
                                 off ? "; " : "", kTargets[i].method, kTargets[i].note);
        if (off >= sizeof(status) - 1) break;
    }
    NLog("installHooks: %s", status);
    char out[600];
    snprintf(out, sizeof(out), "%s%s", all_required ? "ok " : "retry:", status);
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
    };
    if (env->RegisterNatives(xp, kXpMethods, 2) != JNI_OK) {
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
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

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
