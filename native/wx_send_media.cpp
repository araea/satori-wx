// Reflection-only senders for the media WeChat carries as more than a picture: files and videos.
//
// Same approach as wx_send.cpp: call WeChat's own classes through the host class loader, hook
// nothing, load no dex. The classes and members are looked up on every call (a file or a video
// is rare next to text), so a WeChat build that renamed one of them costs that one request and
// never the whole sender.
//
// Verified against WeChat 8.0.78 (versionCode 671108664):
//   File   pluginsdk.model.app.k0.k()                       -> attachment directory (with '/')
//          pluginsdk.model.app.k0.f(dir, title, ext)        -> free destination path in it
//          dx0.r.v(xml)                                     -> parsed <appmsg> content (type 6)
//          pluginsdk.model.app.k0.I(r, appId, appName, talker, attachPath, thumb) -> Pair(code, msgId)
//            (AppMsgLogic.sendAppMsg: builds the AppAttachInfo, inserts the message row, starts the
//             upload; the same call the app's own qs5.v5.dj() ends in)
#include "wx_send.h"
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

namespace satori {
namespace {
void Detail(char *out, size_t size, const char *format, ...) {
    if (!out || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(out, size, format, args);
    va_end(args);
}

// Failed lookups leave a pending NoSuchMethodError; clear it right away and report a null id.
jmethodID StaticMethod(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jmethodID id = env->GetStaticMethodID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}
jmethodID Method(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jmethodID id = env->GetMethodID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}
jfieldID Field(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jfieldID id = env->GetFieldID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

// Owns local references so every early return releases them.
struct Locals {
    JNIEnv *env;
    jobject items[24];
    int count = 0;
    explicit Locals(JNIEnv *e) : env(e) {}
    ~Locals() { for (int i = 0; i < count; ++i) if (items[i]) env->DeleteLocalRef(items[i]); }
    template <typename T> T keep(T ref) { if (ref && count < 24) items[count++] = ref; return ref; }
};

// XML text/attribute escaping for the appmsg content.
void AppendEscaped(char *out, size_t capacity, size_t *used, const char *text) {
    for (; *text; ++text) {
        const char *entity = nullptr;
        switch (*text) {
            case '&': entity = "&amp;"; break;
            case '<': entity = "&lt;"; break;
            case '>': entity = "&gt;"; break;
            case '"': entity = "&quot;"; break;
            case '\'': entity = "&apos;"; break;
            default: break;
        }
        const size_t need = entity ? strlen(entity) : 1;
        if (*used + need + 1 > capacity) return;
        if (entity) memcpy(out + *used, entity, need); else out[*used] = *text;
        *used += need;
    }
    out[*used] = 0;
}

// Hard link, else copy: the destination is WeChat's, so the caller's temporary can expire.
bool LinkOrCopy(const char *from, const char *to, char *detail, size_t size) {
    if (link(from, to) == 0) return true;
    const int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) { Detail(detail, size, "cannot read the file: %s", strerror(errno)); return false; }
    const int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) { close(in); Detail(detail, size, "cannot create %s: %s", to, strerror(errno)); return false; }
    char buffer[65536];
    bool ok = true;
    for (;;) {
        const ssize_t got = read(in, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ok = got == 0; break; }
        for (ssize_t done = 0; done < got;) {
            const ssize_t put = write(out, buffer + done, static_cast<size_t>(got - done));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) { ok = false; break; }
            done += put;
        }
        if (!ok) break;
    }
    close(in);
    close(out);
    if (!ok) { unlink(to); Detail(detail, size, "copy failed: %s", strerror(errno)); }
    return ok;
}

// Gives a file a second name in a `send/` directory next to it, so the caller's own temporary can
// expire while WeChat is still working through it. Names older than six hours are swept.
bool Stage(const char *path, char *out, size_t capacity, char *detail, size_t size) {
    char directory[1100];
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (!slash) { Detail(detail, size, "no directory in the path"); return false; }
    *slash = 0;
    char staging[1200];
    snprintf(staging, sizeof(staging), "%s/send", directory);
    if (mkdir(staging, 0700) && errno != EEXIST) { Detail(detail, size, "cannot create %s: %s", staging, strerror(errno)); return false; }
    const time_t cutoff = time(nullptr) - 6 * 3600;
    if (DIR *dir = opendir(staging)) {
        while (const dirent *entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            char old[1400];
            snprintf(old, sizeof(old), "%s/%s", staging, entry->d_name);
            struct stat info{};
            if (!lstat(old, &info) && S_ISREG(info.st_mode) && info.st_ctime < cutoff) unlink(old);
        }
        closedir(dir);
    }
    static unsigned counter = 0;
    const char *base = strrchr(path, '/') + 1;
    const char *dot = strrchr(base, '.');
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    if (snprintf(out, capacity, "%s/%lld-%u%s", staging, static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000, ++counter,
                 dot && strlen(dot) < 12 ? dot : "") >= static_cast<int>(capacity)) { Detail(detail, size, "path too long"); return false; }
    return LinkOrCopy(path, out, detail, size);
}
} // namespace

SendResult SendFile(const char *talker, const char *path, const char *title) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !path || !*path || !title || !*title) {
        Detail(result.detail, sizeof(result.detail), "empty target, path or title");
        return result;
    }
    struct stat info{};
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "the file is missing or empty");
        return result;
    }
    JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
    if (!env) { Detail(result.detail, sizeof(result.detail), "JavaVM unavailable"); return result; }
    if (!ReflectResolve(result.detail, sizeof(result.detail))) return result;
    Locals locals(env);
    jclass k0 = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.pluginsdk.model.app.k0")));
    jclass content_class = locals.keep(static_cast<jclass>(ReflectLoad("dx0.r")));
    jclass pair_class = locals.keep(env->FindClass("android/util/Pair"));
    jclass integer_class = locals.keep(env->FindClass("java/lang/Integer"));
    jclass long_class = locals.keep(env->FindClass("java/lang/Long"));
    jclass prepare_class = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.pluginsdk.ui.tools.p0")));
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID attach_dir = StaticMethod(env, k0, "k", "()Ljava/lang/String;");
    jmethodID free_path = StaticMethod(env, k0, "f", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID send_app = StaticMethod(env, k0, "I",
        "(Ldx0/r;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;");
    jmethodID parse = StaticMethod(env, content_class, "v", "(Ljava/lang/String;)Ldx0/r;");
    jfieldID pair_first = Field(env, pair_class, "first", "Ljava/lang/Object;");
    jfieldID pair_second = Field(env, pair_class, "second", "Ljava/lang/Object;");
    jmethodID int_value = Method(env, integer_class, "intValue", "()I");
    jmethodID long_value = Method(env, long_class, "longValue", "()J");
    jmethodID prepare = StaticMethod(env, prepare_class, "a", "()V");
    if (!k0 || !content_class || !pair_class || !attach_dir || !free_path || !send_app || !parse || !pair_first || !pair_second ||
        !int_value || !long_value) {
        Detail(result.detail, sizeof(result.detail), "file classes not found (version mismatch?)");
        return result;
    }
    if (prepare) { env->CallStaticVoidMethod(prepare_class, prepare); if (env->ExceptionCheck()) env->ExceptionClear(); }

    // The extension WeChat shows on the bubble and the icon it picks.
    char extension[24] = {};
    if (const char *dot = strrchr(title, '.')) if (dot[1] && strlen(dot + 1) < sizeof(extension) && !strchr(dot, '/')) snprintf(extension, sizeof(extension), "%s", dot + 1);

    // Where WeChat keeps the attachment: its own naming (collision-free), then our file goes in.
    jstring jtitle = locals.keep(env->NewStringUTF(title));
    jstring jext = locals.keep(env->NewStringUTF(extension));
    jstring directory = locals.keep(static_cast<jstring>(env->CallStaticObjectMethod(k0, attach_dir)));
    if (env->ExceptionCheck()) { env->ExceptionClear(); directory = nullptr; }
    jstring destination = directory ? locals.keep(static_cast<jstring>(env->CallStaticObjectMethod(k0, free_path, directory, jtitle, jext))) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); destination = nullptr; }
    if (!jtitle || !jext || !destination) { Detail(result.detail, sizeof(result.detail), "attachment directory unavailable"); return result; }
    const char *target = env->GetStringUTFChars(destination, nullptr);
    if (!target) { Detail(result.detail, sizeof(result.detail), "attachment path unavailable"); return result; }
    char attach_path[1024];
    snprintf(attach_path, sizeof(attach_path), "%s", target);
    env->ReleaseStringUTFChars(destination, target);
    // WeChat creates its directories lazily; make sure this one exists before linking into it.
    for (char *slash = strchr(attach_path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        mkdir(attach_path, 0700);
        *slash = '/';
    }
    if (!LinkOrCopy(path, attach_path, result.detail, sizeof(result.detail))) return result;

    // The content WeChat parses back into its AppMessage: a plain file, type 6.
    char xml[4096];
    size_t used = 0;
    xml[0] = 0;
    auto put = [&](const char *text) { const size_t n = strlen(text); if (used + n + 1 < sizeof(xml)) { memcpy(xml + used, text, n + 1); used += n; } };
    char number[32];
    put("<msg><appmsg appid=\"\" sdkver=\"0\"><title>");
    AppendEscaped(xml, sizeof(xml), &used, title);
    put("</title><des></des><action></action><type>6</type><showtype>0</showtype><mediatagname></mediatagname>"
        "<messageext></messageext><messageaction></messageaction><content></content><contentattr>0</contentattr>"
        "<url></url><lowurl></lowurl><dataurl></dataurl><lowdataurl></lowdataurl><appattach><totallen>");
    snprintf(number, sizeof(number), "%lld", static_cast<long long>(info.st_size));
    put(number);
    put("</totallen><attachid></attachid><emoticonmd5></emoticonmd5><fileext>");
    AppendEscaped(xml, sizeof(xml), &used, extension);
    put("</fileext><cdnattachurl></cdnattachurl><aeskey></aeskey><encryver>0</encryver></appattach><extinfo></extinfo>"
        "<sourceusername></sourceusername><sourcedisplayname></sourcedisplayname><thumburl></thumburl><md5></md5>"
        "<statextstr></statextstr></appmsg><fromusername></fromusername><scene>0</scene>"
        "<appinfo><version>1</version><appname></appname></appinfo><commenturl></commenturl></msg>");
    jstring jxml = locals.keep(env->NewStringUTF(xml));
    jobject content = jxml ? locals.keep(env->CallStaticObjectMethod(content_class, parse, jxml)) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); content = nullptr; }
    if (!content) { unlink(attach_path); Detail(result.detail, sizeof(result.detail), "WeChat could not parse the file message"); return result; }

    jstring jtalker = locals.keep(env->NewStringUTF(talker));
    jstring jempty = locals.keep(env->NewStringUTF(""));
    jstring jattach = locals.keep(env->NewStringUTF(attach_path));
    jobject pair = jtalker && jempty && jattach
        ? locals.keep(env->CallStaticObjectMethod(k0, send_app, content, jempty, jempty, jtalker, jattach, static_cast<jbyteArray>(nullptr)))
        : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); pair = nullptr; Detail(result.detail, sizeof(result.detail), "WeChat's file send threw"); }
    if (!pair) {
        if (!result.detail[0]) Detail(result.detail, sizeof(result.detail), "WeChat's file send returned nothing");
        unlink(attach_path);
        return result;
    }
    jobject first = locals.keep(env->GetObjectField(pair, pair_first));
    jobject second = locals.keep(env->GetObjectField(pair, pair_second));
    const jint code = first ? env->CallIntMethod(first, int_value) : -1;
    const jlong local_id = second ? env->CallLongMethod(second, long_value) : -1;
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    result.net_id = static_cast<int>(code);
    if (code != 0 || local_id <= 0) {
        unlink(attach_path);
        Detail(result.detail, sizeof(result.detail), "WeChat refused the file (code %d, id %lld)", static_cast<int>(code), static_cast<long long>(local_id));
        return result;
    }
    result.ok = true;
    result.local_id = local_id;
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "file %s handed to WeChat for %s (local id %lld)", title, talker, static_cast<long long>(local_id));
    return result;
}

SendResult SendVideo(const char *talker, const char *path, const char *thumb_path, int duration_s) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !path || !*path) {
        Detail(result.detail, sizeof(result.detail), "empty target or path");
        return result;
    }
    struct stat info{};
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "the video is missing or empty");
        return result;
    }
    JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
    if (!env) { Detail(result.detail, sizeof(result.detail), "JavaVM unavailable"); return result; }
    if (!ReflectResolve(result.detail, sizeof(result.detail))) return result;
    Locals locals(env);
    jclass n0 = locals.keep(static_cast<jclass>(ReflectLoad("ph5.n0")));
    jclass service_interface = locals.keep(static_cast<jclass>(ReflectLoad("ab5.s")));
    jclass element_class = locals.keep(static_cast<jclass>(ReflectLoad("qi0.w2")));
    jclass cross_class = locals.keep(static_cast<jclass>(ReflectLoad("qi0.t2")));
    jclass names_class = locals.keep(static_cast<jclass>(ReflectLoad("s61.c3")));
    jclass prepare_class = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.pluginsdk.ui.tools.p0")));
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID lookup = StaticMethod(env, n0, "c", "(Ljava/lang/Class;)Lph5/m;");
    jmethodID element_ctor = Method(env, element_class, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZILqi0/t2;Lb41/k7;)V");
    jmethodID cross_ctor = Method(env, cross_class, "<init>",
        "(Lb41/i7;Lpc5/qn6;Ljava/lang/String;Lpc5/p87;Ljava/lang/String;Lpc5/qn4;ZLqi0/r2;Ljava/lang/String;Lp95/f;ZZZ)V");
    jmethodID unique_name = StaticMethod(env, names_class, "a", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID prepare = StaticMethod(env, prepare_class, "a", "()V");
    if (!n0 || !service_interface || !element_class || !cross_class || !lookup || !element_ctor || !cross_ctor || !unique_name) {
        Detail(result.detail, sizeof(result.detail), "video classes not found (version mismatch?)");
        return result;
    }
    jobject service = locals.keep(env->CallStaticObjectMethod(n0, lookup, service_interface));
    if (env->ExceptionCheck()) { env->ExceptionClear(); service = nullptr; }
    jclass service_class = service ? locals.keep(env->GetObjectClass(service)) : nullptr;
    jmethodID send = service_class ? Method(env, service_class, "cj", "(Lqi0/w2;Ljava/lang/String;)Z") : nullptr;
    if (!service || !send) {
        Detail(result.detail, sizeof(result.detail), service ? "video service has no cj() (version mismatch?)" : "video service unavailable");
        return result;
    }
    if (prepare) { env->CallStaticVoidMethod(prepare_class, prepare); if (env->ExceptionCheck()) env->ExceptionClear(); }
    char staged[1300];
    if (!Stage(path, staged, sizeof(staged), result.detail, sizeof(result.detail))) return result;
    jstring jtalker = locals.keep(env->NewStringUTF(talker));
    jstring jpath = locals.keep(env->NewStringUTF(staged));
    jstring jthumb = locals.keep(env->NewStringUTF(thumb_path ? thumb_path : ""));
    jstring jname = jtalker ? locals.keep(static_cast<jstring>(env->CallStaticObjectMethod(names_class, unique_name, jtalker))) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); jname = nullptr; }
    // The chat UI's own arguments: compress as WeChat sees fit, no import copy.
    jobject cross = locals.keep(env->NewObject(cross_class, cross_ctor, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                               JNI_FALSE, nullptr, nullptr, nullptr, JNI_FALSE, JNI_FALSE, JNI_FALSE));
    if (env->ExceptionCheck()) { env->ExceptionClear(); cross = nullptr; }
    jobject element = (jname && jpath && jthumb && cross)
        ? locals.keep(env->NewObject(element_class, element_ctor, jname, jpath, jthumb, JNI_FALSE, static_cast<jint>(duration_s > 0 ? duration_s : 0), cross, nullptr))
        : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); element = nullptr; }
    if (!element) { unlink(staged); Detail(result.detail, sizeof(result.detail), "video task construction failed"); return result; }
    const jboolean started = env->CallBooleanMethod(service, send, element, jtalker);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(result.detail, sizeof(result.detail), "WeChat's video send threw");
        return result;
    }
    if (!started) { unlink(staged); Detail(result.detail, sizeof(result.detail), "WeChat declined to start the video send"); return result; }
    result.ok = true;
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "video handed to WeChat for %s", talker);
    return result;
}
} // namespace satori
