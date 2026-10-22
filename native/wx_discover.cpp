#include "wx_discover.h"
#include "wx_paths.h"

#include "wx_derive.h"
#include "wx_names.h"

#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace satori {
namespace {

constexpr const char *kTag = "SatoriWx";
constexpr const char *kCachePrefix = "names-";

JavaVM *g_vm = nullptr;
char g_data_dir[300];
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_started = false; // a derivation has been started (or a cache was installed)

struct Job {
    char apk[420];
    char dir[300];
    char fingerprint[64];
};

// The installed APK path, through ActivityThread.currentApplication().getPackageCodePath().
bool ApkPath(JNIEnv *env, char *out, size_t size) {
    jclass activity = env->FindClass("android/app/ActivityThread");
    if (!activity) {
        env->ExceptionClear();
        return false;
    }
    jmethodID current = env->GetStaticMethodID(activity, "currentApplication", "()Landroid/app/Application;");
    jobject app = current ? env->CallStaticObjectMethod(activity, current) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    bool ok = false;
    if (app) {
        jclass context = env->FindClass("android/content/Context");
        jmethodID code = context ? env->GetMethodID(context, "getPackageCodePath", "()Ljava/lang/String;") : nullptr;
        jstring path = code ? static_cast<jstring>(env->CallObjectMethod(app, code)) : nullptr;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (path) {
            const char *text = env->GetStringUTFChars(path, nullptr);
            if (text) {
                snprintf(out, size, "%s", text);
                env->ReleaseStringUTFChars(path, text);
                ok = out[0] != 0;
            }
            env->DeleteLocalRef(path);
        }
        if (context) env->DeleteLocalRef(context);
        env->DeleteLocalRef(app);
    }
    env->DeleteLocalRef(activity);
    return ok;
}

void CachePath(char *out, size_t size, const char *dir, const char *fingerprint) {
    char leaf[96];
    snprintf(leaf, sizeof(leaf), "%s%s.conf", kCachePrefix, fingerprint);
    StorePath(out, size, dir, leaf);
}

// Reads a cache written for this very APK. Fails on a different fingerprint, a missing key or a
// value that no longer fits its column.
bool LoadCache(const char *path, const Job &job, DerivedNames *names) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool matches = false;
    int seen = 0;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char *newline = strchr(line, '\n');
        if (newline) *newline = 0;
        if (!strncmp(line, "apk ", 4)) {
            matches = !strcmp(line + 4, job.fingerprint);
            continue;
        }
        char *space = strchr(line, ' ');
        if (!space) continue;
        *space++ = 0;
        const int key = WxKeyByName(line);
        if (key < 0) continue;
        if (strlen(space) >= kWxValueSize) continue;
        snprintf(names->value[key], kWxValueSize, "%s", space);
        ++seen;
    }
    fclose(file);
    return matches && seen == kWxKeyCount && names->Complete();
}

bool SaveCache(const char *path, const Job &job, const DerivedNames &names) {
    char temporary[440];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "w");
    if (!file) return false;
    fprintf(file, "names 1\napk %s\n", job.fingerprint);
    for (int i = 0; i < kWxKeyCount; ++i) fprintf(file, "%s %s\n", WxKeyId(static_cast<WxKey>(i)), names.value[i]);
    const bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

void Install(DerivedNames *names) {
    const char *values[kWxKeyCount];
    for (int i = 0; i < kWxKeyCount; ++i) values[i] = names->value[i];
    if (!WxNamesInstall("discovered", values)) __android_log_print(ANDROID_LOG_ERROR, kTag, "name set refused");
}

void *Worker(void *raw) {
    Job *job = static_cast<Job *>(raw);
    DerivedNames *names = static_cast<DerivedNames *>(calloc(1, sizeof(DerivedNames)));
    if (!names) {
        free(job);
        return nullptr;
    }
    char error[160] = {};
    if (!DeriveNames(job->apk, nullptr, names, error, sizeof(error))) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "name discovery failed: %s", error);
    } else if (!names->Complete()) {
        int missing = 0;
        for (int i = 0; i < kWxKeyCount; ++i)
            if (!names->value[i][0]) ++missing;
        __android_log_print(ANDROID_LOG_WARN, kTag, "name discovery incomplete: %d of %d keys", missing, kWxKeyCount);
    } else {
        char path[440];
        CachePath(path, sizeof(path), job->dir, job->fingerprint);
        const bool saved = SaveCache(path, *job, *names);
        Install(names);
        __android_log_print(ANDROID_LOG_INFO, kTag, "name discovery done (%s)", saved ? "cached" : "not cached");
    }
    free(names);
    free(job);
    return nullptr;
}

} // namespace

void DiscoverInit(JavaVM *vm, const char *data_dir) {
    g_vm = vm;
    if (data_dir) snprintf(g_data_dir, sizeof(g_data_dir), "%s", data_dir);
}

void DiscoverStart(JNIEnv *env) {
    if (!env) return;
    auto start = [](bool value) {
        pthread_mutex_lock(&g_mu);
        const bool was = g_started;
        g_started = value;
        pthread_mutex_unlock(&g_mu);
        return was;
    };
    if (start(true)) return;
    // A failure before the thread exists undoes the flag: the next failed resolve may see a usable
    // APK path (the first send can come before the Application object is fully up).
    Job *job = static_cast<Job *>(calloc(1, sizeof(Job)));
    if (!job) return;
    if (!ApkPath(env, job->apk, sizeof(job->apk))) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "name discovery: APK path unavailable");
        start(false);
        free(job);
        return;
    }
    snprintf(job->dir, sizeof(job->dir), "%s", g_data_dir);
    struct stat st{};
    if (!job->dir[0] || stat(job->apk, &st) != 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "name discovery: %s not readable", job->apk);
        start(false);
        free(job);
        return;
    }
    snprintf(job->fingerprint, sizeof(job->fingerprint), "%lld-%lld", static_cast<long long>(st.st_size),
             static_cast<long long>(st.st_mtime));
    char path[440];
    CachePath(path, sizeof(path), job->dir, job->fingerprint);
    DerivedNames *cached = static_cast<DerivedNames *>(calloc(1, sizeof(DerivedNames)));
    if (cached) {
        if (LoadCache(path, *job, cached)) {
            Install(cached);
            __android_log_print(ANDROID_LOG_INFO, kTag, "name set loaded from cache");
            free(cached);
            free(job);
            return;
        }
        free(cached);
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Worker, job) != 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "name discovery: thread creation failed");
        start(false);
        free(job);
        return;
    }
    pthread_detach(thread);
}

} // namespace satori
