// 知言探针（satori-wx 里程碑 1）：微信进程里的 JNI 边界侦察。
//
// 只回答一个问题：微信有没有一条能像知弦在 QQ 上换 native_onSendSSOReply 那样，
// 用 RegisterNatives 干净接管的 native↔Java 边界。
//
// 手段与边界：
//   - 拦截 = 改 libart 全局 JNINativeInterface 函数表里的 RegisterNatives 一个函数指针
//     （数据补丁；不写任何代码字节、无 trampoline、不碰 PROT_EXEC 页），补丁窗口 90 秒
//     或 2 万次注册（先到者），到点 mprotect 写回原指针。
//   - wrapper 里只做纯 C 的字符串拷贝与入队 + 一次 NewGlobalRef（叶子级调用，无锁序问题）；
//     类名解析放到 writer 线程做。
//   - 拦截失败（mprotect 被拒）自动降级为纯被动清单，进程零影响。
//   - 不发任何网络请求，不改微信行为。
//
// 产物：/data/data/com.tencent.mm/files/satori-wx-probe/
//   boundary.log  每条 native 方法注册：时间 tid 文件偏移 函数指针 归属.so 类名 方法名 签名
//   maps.log      还原时刻的原生库映射清单
//   meta.log      时间线与计数
#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <android/log.h>

#include "zygisk.hpp"
#include "jni_helpers.h"

// ---- 常量 ---------------------------------------------------------------------

static const char *kTarget = "com.tencent.mm";
static const char *kLogDir = "/data/data/com.tencent.mm/files/satori-wx-probe";
static constexpr int kRingSize = 4096;        // 环形缓冲条目数
static constexpr int kNameMax = 96;           // 方法名上限
static constexpr int kSigMax = 224;           // 签名上限
static constexpr int kMaxClasses = 4096;      // 唯一类名计数上限（哈希集合）
static constexpr int kCapCalls = 20000;       // 拦截的 RegisterNatives 调用上限
static constexpr int kWindowSec = 90;         // 补丁窗口
static constexpr int kMapMax = 16384;         // maps 缓存行数上限

// ---- 数据结构 ------------------------------------------------------------------

struct Entry {
    uint64_t fn;
    int32_t tid;
    jobject gref;               // 本条目专属的 global ref（writer 解析后 Delete）
    char name[kNameMax];
    char sig[kSigMax];
};

struct MapRange {
    uintptr_t start, end;
    uint64_t offset;            // 文件内偏移
    bool has_path;
    char path[256];
};

// ---- 全局 ----------------------------------------------------------------------

static JavaVM *g_vm = nullptr;
static bool g_target = false;

static const JNINativeInterface *g_table = nullptr;
static void **g_slot = nullptr;               // &table->RegisterNatives
static void *g_orig = nullptr;                // 原 RegisterNatives
static int g_page_prot_saved = -1;
static uintptr_t g_page_start = 0;
static size_t g_page_size = 0;

static volatile int g_active = 0;             // wrapper 是否在记录
static volatile int g_restored = 0;
static volatile int g_done = 0;               // writer 可以收尾

static long g_call_count = 0;                 // 拦截到的 RegisterNatives 调用数
static long g_method_count = 0;
static long g_drop_count = 0;
static int g_unique_classes = 0;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static Entry g_ring[kRingSize];
static int g_ring_head = 0;   // 写入位
static int g_ring_tail = 0;   // 消费位

static MapRange g_maps[kMapMax];
static int g_map_count = 0;
static int64_t g_last_maps_load_ms = 0;

static int g_fd_boundary = -1;
static int g_fd_meta = -1;

static int64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void LoadMapsLocked();

static jint MyRegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
                              jint n);

static void *TimerThread(void *);

// ---- maps 缓存：把 fn 归属到 .so ------------------------------------------------

static void LoadMapsLocked() {
    g_map_count = 0;
    FILE *f = fopen("/proc/self/maps", "re");
    if (f == nullptr) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr && g_map_count < kMapMax) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        uint64_t off = 0;
        unsigned dev_maj = 0, dev_min = 0;
        unsigned long inode = 0;
        int consumed = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %x:%x %lu%n", &start, &end, perms, &off,
                   &dev_maj, &dev_min, &inode, &consumed) != 7) continue;
        MapRange &r = g_maps[g_map_count];
        r.start = start; r.end = end; r.offset = off;
        const char *p = line + consumed;
        while (*p == ' ') p++;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == ' ')) n--;
        if (n == 0) { r.has_path = false; r.path[0] = '\0'; }
        else {
            r.has_path = true;
            snprintf(r.path, sizeof(r.path), "%.*s", (int) n, p);
        }
        g_map_count++;
    }
    fclose(f);
    g_last_maps_load_ms = NowMs();
}

/** fn → (路径, 文件内偏移)。找不到返回 false。 */
static bool AttributeFn(uint64_t fn, char *out, size_t out_size, uint64_t *file_off) {
    pthread_mutex_lock(&g_mu);
    if (g_map_count == 0) LoadMapsLocked();
    for (int i = 0; i < g_map_count; i++) {
        const MapRange &r = g_maps[i];
        if (fn >= r.start && fn < r.end) {
            if (!r.has_path) { pthread_mutex_unlock(&g_mu); return false; }
            snprintf(out, out_size, "%s", r.path);
            *file_off = r.offset + (fn - r.start);
            pthread_mutex_unlock(&g_mu);
            return true;
        }
    }
    // 新库后加载：重读一次 maps（限频 2 秒）。
    if (NowMs() - g_last_maps_load_ms > 2000) {
        LoadMapsLocked();
        for (int i = 0; i < g_map_count; i++) {
            const MapRange &r = g_maps[i];
            if (fn >= r.start && fn < r.end && r.has_path) {
                snprintf(out, out_size, "%s", r.path);
                *file_off = r.offset + (fn - r.start);
                pthread_mutex_unlock(&g_mu);
                return true;
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    return false;
}

// ---- 唯一类名计数（FNV 哈希 + 线性探开） -------------------------------------------

static uint64_t g_class_hashes[kMaxClasses];

static void CountClassName(const char *name) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = name; *p; p++) { h ^= (uint8_t) *p; h *= 1099511628211ull; }
    if (h == 0) h = 1;
    size_t i = (size_t) (h % kMaxClasses);
    for (;;) {
        if (g_class_hashes[i] == 0) { g_class_hashes[i] = h; g_unique_classes++; return; }
        if (g_class_hashes[i] == h) return;
        i = (i + 1) % kMaxClasses;
    }
}

// ---- 落盘 -----------------------------------------------------------------------

static void EnsureDir() {
    mkdir(kLogDir, 0700);   // 已存在则 EEXIST，忽略
}

static int OpenLog(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", kLogDir, name);
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    return fd;
}

static void WriteAll(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w <= 0) {
            if (errno == EINTR) continue;
            return;
        }
        buf += w;
        len -= (size_t) w;
    }
}

// ---- 环形缓冲（多生产者单消费者，互斥锁 + 条件变量） ---------------------------------

static void RingPush(const Entry &e) {
    pthread_mutex_lock(&g_mu);
    int next = (g_ring_head + 1) % kRingSize;
    if (next == g_ring_tail) {
        g_drop_count++;
    } else {
        g_ring[g_ring_head] = e;
        g_ring_head = next;
        pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
}

static int RingDrain(Entry *out, int max) {
    pthread_mutex_lock(&g_mu);
    int n = 0;
    while (g_ring_tail != g_ring_head && n < max) {
        out[n++] = g_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1) % kRingSize;
    }
    if (n == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

// ---- 补丁 ------------------------------------------------------------------------

/** 读出某地址所在页当前的保护位（"r"/"w"/"x"），失败返回 -1。 */
static int PageProt(uintptr_t addr, uintptr_t *page_start, size_t *page_size) {
    long sz = sysconf(_SC_PAGESIZE);
    if (sz <= 0) sz = 4096;
    *page_size = (size_t) sz;
    *page_start = addr & ~((uintptr_t) sz - 1);
    FILE *f = fopen("/proc/self/maps", "re");
    if (f == nullptr) return -1;
    char line[1024];
    int prot = -1;
    while (fgets(line, sizeof(line), f) != nullptr) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (addr >= start && addr < end) {
            prot = 0;
            if (strchr(perms, 'r')) prot |= PROT_READ;
            if (strchr(perms, 'w')) prot |= PROT_WRITE;
            if (strchr(perms, 'x')) prot |= PROT_EXEC;
            break;
        }
    }
    fclose(f);
    return prot;
}

static bool PatchTable(JNIEnv *env) {
    g_table = env->functions;
    auto *slot = &g_table->RegisterNatives;   // jint (*const *)(...)
    g_slot = reinterpret_cast<void **>(
            const_cast<void *>(reinterpret_cast<const void *>(slot)));
    g_orig = (void *) g_table->RegisterNatives;

    g_page_prot_saved = PageProt((uintptr_t) g_slot, &g_page_start, &g_page_size);
    if (g_page_prot_saved < 0) {
        WLOGE("cannot read page prot of RegisterNatives slot");
        return false;
    }
    if (mprotect((void *) g_page_start, g_page_size,
                 g_page_prot_saved | PROT_WRITE) != 0) {
        WLOGE("mprotect RW failed: %s（降级为纯被动清单）", strerror(errno));
        return false;
    }
    *g_slot = (void *) &MyRegisterNatives;
    if (g_page_prot_saved != (PROT_READ | PROT_WRITE)) {
        mprotect((void *) g_page_start, g_page_size, g_page_prot_saved);
    }
    return true;
}

static void RestoreTable() {
    if (__atomic_exchange_n(&g_active, 0, __ATOMIC_SEQ_CST) == 0) {
        // 尚未激活或已还原过
        __atomic_store_n(&g_restored, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_done, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&g_mu);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    mprotect((void *) g_page_start, g_page_size, g_page_prot_saved | PROT_WRITE);
    *g_slot = g_orig;
    if (g_page_prot_saved != (PROT_READ | PROT_WRITE)) {
        mprotect((void *) g_page_start, g_page_size, g_page_prot_saved);
    }
    WLOGI("RegisterNatives restored: calls=%ld methods=%ld drops=%ld",
          g_call_count, g_method_count, g_drop_count);
    __atomic_store_n(&g_restored, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_done, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&g_mu);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// ---- wrapper ---------------------------------------------------------------------
//
// 只在 g_active 时记录。注意：这里在 ART 的 RegisterNatives 调用路径上，
// 除 NewGlobalRef（叶子级）外不做任何 JNI 调用；类名解析全部推迟到 writer 线程。

static void Record(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
    for (jint i = 0; i < n; i++) {
        Entry e = {};
        e.fn = (uint64_t) methods[i].fnPtr;
        e.tid = (int32_t) syscall(SYS_gettid);
        e.gref = env->NewGlobalRef(clazz);   // 每条目持一个，writer 解析后 Delete
        const char *nm = methods[i].name ? methods[i].name : "";
        const char *sg = methods[i].signature ? methods[i].signature : "";
        snprintf(e.name, sizeof(e.name), "%s", nm);
        snprintf(e.sig, sizeof(e.sig), "%s", sg);
        pthread_mutex_lock(&g_mu);
        g_method_count++;
        pthread_mutex_unlock(&g_mu);
        RingPush(e);
    }
}

static jint MyRegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
                              jint n) {
    if (__atomic_load_n(&g_active, __ATOMIC_SEQ_CST) && n > 0) {
        long c = __atomic_add_fetch(&g_call_count, 1, __ATOMIC_SEQ_CST);
        if (c <= kCapCalls) {
            Record(env, clazz, methods, n);
            if (c == kCapCalls) RestoreTable();
        }
    }
    using OrigFn = jint (*)(JNIEnv *, jclass, const JNINativeMethod *, jint);
    return reinterpret_cast<OrigFn>(g_orig)(env, clazz, methods, n);
}

// ---- writer 线程 -------------------------------------------------------------------

static jclass g_cls_class = nullptr;
static jmethodID g_mid_get_name = nullptr;

static bool SetupNameResolver(JNIEnv *env) {
    g_cls_class = env->FindClass("java/lang/Class");
    if (g_cls_class == nullptr) { env->ExceptionClear(); return false; }
    g_cls_class = (jclass) env->NewGlobalRef(g_cls_class);
    g_mid_get_name = env->GetMethodID(g_cls_class, "getName", "()Ljava/lang/String;");
    if (g_mid_get_name == nullptr) { env->ExceptionClear(); return false; }
    return true;
}

/** 解析一个 global ref 的类名到 buf；返回 false 表示解析失败（ref 已释放）。 */
static bool ResolveClassName(JNIEnv *env, jobject gref, char *buf, size_t buf_size) {
    jstring s = (jstring) env->CallObjectMethod(gref, g_mid_get_name);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteGlobalRef(gref);
        return false;
    }
    bool ok = false;
    if (s != nullptr) {
        const char *c = env->GetStringUTFChars(s, nullptr);
        if (c != nullptr) {
            snprintf(buf, buf_size, "%s", c);
            env->ReleaseStringUTFChars(s, c);
            ok = true;
        }
        env->DeleteLocalRef(s);
    }
    env->DeleteGlobalRef(gref);
    return ok;
}

static const char *BaseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void *WriterThread(void *) {
    bool attached = false;
    JNIEnv *env = WxGetEnv(g_vm, &attached);
    if (env != nullptr) SetupNameResolver(env);

    Entry batch[256];
    char line[640];
    char out[8192];
    size_t out_len = 0;

    while (true) {
        int n = RingDrain(batch, 256);
        if (n == 0 && __atomic_load_n(&g_done, __ATOMIC_SEQ_CST)) break;
        for (int i = 0; i < n; i++) {
            Entry &e = batch[i];
            char lib[256] = "(unknown)";
            uint64_t off = 0;
            char loc[300];
            if (AttributeFn(e.fn, lib, sizeof(lib), &off)) {
                snprintf(loc, sizeof(loc), "%s+0x%llx", BaseName(lib),
                         (unsigned long long) off);
            } else {
                snprintf(loc, sizeof(loc), "?+0x%llx", (unsigned long long) e.fn);
            }
            char cls[224] = "(unresolved)";
            if (env != nullptr && e.gref != nullptr) {
                if (!ResolveClassName(env, e.gref, cls, sizeof(cls))) {
                    snprintf(cls, sizeof(cls), "(resolve-failed)");
                }
                CountClassName(cls);
            } else if (e.gref != nullptr) {
                env = WxGetEnv(g_vm, &attached);   // 重试 attach
                if (env != nullptr && SetupNameResolver(env) &&
                    ResolveClassName(env, e.gref, cls, sizeof(cls))) {
                    CountClassName(cls);
                }
            }
            int m = snprintf(line, sizeof(line), "%lld %d %s %s %s %s\n",
                             (long long) NowMs(), e.tid, loc, cls, e.name, e.sig);
            if (m > 0 && g_fd_boundary >= 0) {
                if (out_len + (size_t) m >= sizeof(out)) {
                    WriteAll(g_fd_boundary, out, out_len);
                    out_len = 0;
                }
                memcpy(out + out_len, line, (size_t) m);
                out_len += (size_t) m;
            }
        }
        if (out_len > 0 && g_fd_boundary >= 0) {
            WriteAll(g_fd_boundary, out, out_len);
            out_len = 0;
        }
    }
    if (out_len > 0 && g_fd_boundary >= 0) WriteAll(g_fd_boundary, out, out_len);

    // 还原后：maps 快照与 meta。
    if (g_fd_boundary >= 0) { close(g_fd_boundary); g_fd_boundary = -1; }
    pthread_mutex_lock(&g_mu);
    LoadMapsLocked();
    pthread_mutex_unlock(&g_mu);

    int fd_maps = OpenLog("maps.log");
    if (fd_maps >= 0) {
        char hdr[128];
        int h = snprintf(hdr, sizeof(hdr), "# maps snapshot at %lld\n",
                         (long long) NowMs());
        WriteAll(fd_maps, hdr, (size_t) h);
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_map_count; i++) {
            const MapRange &r = g_maps[i];
            if (!r.has_path) continue;
            const char *b = BaseName(r.path);
            // 只留微信自有库与其直接依赖，日志不要全量 maps 那么大。
            if (strncmp(b, "libwechat", 9) == 0 || strncmp(b, "libmm", 5) == 0 ||
                strncmp(b, "libapp.", 7) == 0 || strncmp(b, "libWCDB", 7) == 0 ||
                strncmp(b, "libMMProtocalJni", 16) == 0 ||
                strstr(b, "cso") != nullptr) {
                char row[512];
                int m = snprintf(row, sizeof(row), "%08llx-%08llx +%08llx %s\n",
                                 (unsigned long long) r.start, (unsigned long long) r.end,
                                 (unsigned long long) r.offset, r.path);
                WriteAll(fd_maps, row, (size_t) m);
            }
        }
        pthread_mutex_unlock(&g_mu);
        close(fd_maps);
    }

    if (g_fd_meta >= 0) {
        char buf[1024];
        int m = snprintf(buf, sizeof(buf),
                         "target=%s\npatched=%s\nwindow_sec=%d cap_calls=%d\n"
                         "calls=%ld methods=%ld unique_classes=%d drops=%ld\n"
                         "start_ms=%lld end_ms=%lld\n",
                         kTarget, g_orig != nullptr && g_slot != nullptr ? "yes" : "no",
                         kWindowSec, kCapCalls, g_call_count, g_method_count,
                         g_unique_classes, g_drop_count, 0LL, (long long) NowMs());
        WriteAll(g_fd_meta, buf, (size_t) m);
        close(g_fd_meta);
        g_fd_meta = -1;
    }
    if (attached && env != nullptr) WxReleaseEnv(g_vm, attached);
    WLOGI("writer finished: calls=%ld methods=%ld classes=%d drops=%ld",
          g_call_count, g_method_count, g_unique_classes, g_drop_count);
    return nullptr;
}

static void *TimerThread(void *) {
    for (int i = 0; i < kWindowSec && !__atomic_load_n(&g_restored, __ATOMIC_SEQ_CST); i++) {
        sleep(1);
    }
    RestoreTable();
    return nullptr;
}

// ---- 启动 -------------------------------------------------------------------------

static void Start(JNIEnv *env) {
    int64_t t0 = NowMs();
    EnsureDir();
    g_fd_boundary = OpenLog("boundary.log");
    g_fd_meta = OpenLog("meta.log");

    bool patched = PatchTable(env);
    if (patched) {
        __atomic_store_n(&g_active, 1, __ATOMIC_SEQ_CST);
    }
    char buf[512];
    if (g_fd_meta >= 0) {
        int m = snprintf(buf, sizeof(buf),
                         "target=%s\npatched=%s\nstart_ms=%lld\n",
                         kTarget, patched ? "yes" : "no", (long long) t0);
        WriteAll(g_fd_meta, buf, (size_t) m);
    }
    WLOGI("probe start: patched=%s log=%s", patched ? "yes" : "no", kLogDir);

    pthread_t w, t;
    if (pthread_create(&w, nullptr, WriterThread, nullptr) == 0) pthread_detach(w);
    // TimerThread 到点调 RestoreTable：补丁成功时还原函数指针，失败时只置收尾标志
    // （RestoreTable 走 g_active==0 分支，幂等），writer 照常跑完 maps/meta 后退出。
    if (pthread_create(&t, nullptr, TimerThread, nullptr) == 0) pthread_detach(t);
}

// ---- Zygisk 模块 ------------------------------------------------------------------

class WxProbeModule : public zygisk::ModuleBase {
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
        if (strcmp(name, kTarget) == 0) {   // 精确匹配，:push 等子进程不进
            g_target = true;
        }
        env_->ReleaseStringUTFChars(args->nice_name, name);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!g_target) return;
        // 此刻 specialize 已完成、微信的 Application/静态初始化还没跑：
        // 它所有 JNI_OnLoad 的 RegisterNatives 都会被拦到。
        Start(env_);
    }

private:
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(WxProbeModule)
