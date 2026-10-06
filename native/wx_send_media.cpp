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
#include "textbuf.h"
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
    ~Locals() {
        for (int i = 0; i < count; ++i)
            if (items[i]) env->DeleteLocalRef(items[i]);
    }
    template <typename T> T keep(T ref) {
        if (ref && count < 24) items[count++] = ref;
        return ref;
    }
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
        if (entity)
            memcpy(out + *used, entity, need);
        else
            out[*used] = *text;
        *used += need;
    }
    out[*used] = 0;
}

// mkdir -p for the directories above `path`.
void MakeParents(const char *path) {
    char copy[1200];
    snprintf(copy, sizeof(copy), "%s", path);
    for (char *slash = strchr(copy + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        mkdir(copy, 0700);
        *slash = '/';
    }
}

// Hard link, else copy: the destination is WeChat's, so the caller's temporary can expire.
bool LinkOrCopy(const char *from, const char *to, char *detail, size_t size) {
    if (link(from, to) == 0) return true;
    const int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        Detail(detail, size, "cannot read the file: %s", strerror(errno));
        return false;
    }
    const int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        close(in);
        Detail(detail, size, "cannot create %s: %s", to, strerror(errno));
        return false;
    }
    char buffer[65536];
    bool ok = true;
    for (;;) {
        const ssize_t got = read(in, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ok = got == 0;
            break;
        }
        for (ssize_t done = 0; done < got;) {
            const ssize_t put = write(out, buffer + done, static_cast<size_t>(got - done));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) {
                ok = false;
                break;
            }
            done += put;
        }
        if (!ok) break;
    }
    close(in);
    close(out);
    if (!ok) {
        unlink(to);
        Detail(detail, size, "copy failed: %s", strerror(errno));
    }
    return ok;
}

// Gives a file a second name in a `send/` directory next to it, so the caller's own temporary can
// expire while WeChat is still working through it. Names older than six hours are swept.
bool Stage(const char *path, char *out, size_t capacity, char *detail, size_t size) {
    char directory[1100];
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (!slash) {
        Detail(detail, size, "no directory in the path");
        return false;
    }
    *slash = 0;
    char staging[1200];
    snprintf(staging, sizeof(staging), "%s/send", directory);
    if (mkdir(staging, 0700) && errno != EEXIST) {
        Detail(detail, size, "cannot create %s: %s", staging, strerror(errno));
        return false;
    }
    const time_t cutoff = time(nullptr) - 6 * 3600;
    if (DIR *dir = opendir(staging)) {
        while (const dirent *entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            char old[1400];
            snprintf(old, sizeof(old), "%s/%s", staging, entry->d_name);
            struct stat info{};
            if (!lstat(old, &info) && S_ISREG(info.st_mode) && info.st_mtime < cutoff) unlink(old);
        }
        closedir(dir);
    }
    static unsigned counter = 0;
    const char *base = strrchr(path, '/') + 1;
    const char *dot = strrchr(base, '.');
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    if (snprintf(out, capacity, "%s/%lld-%u%s", staging,
                 static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000, ++counter,
                 dot && strlen(dot) < 12 ? dot : "") >= static_cast<int>(capacity)) {
        Detail(detail, size, "path too long");
        return false;
    }
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
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
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
    jmethodID free_path =
        StaticMethod(env, k0, "f", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID send_app = StaticMethod(
        env, k0, "I",
        "(Ldx0/r;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;");
    jmethodID parse = StaticMethod(env, content_class, "v", "(Ljava/lang/String;)Ldx0/r;");
    jfieldID pair_first = Field(env, pair_class, "first", "Ljava/lang/Object;");
    jfieldID pair_second = Field(env, pair_class, "second", "Ljava/lang/Object;");
    jmethodID int_value = Method(env, integer_class, "intValue", "()I");
    jmethodID long_value = Method(env, long_class, "longValue", "()J");
    jmethodID prepare = StaticMethod(env, prepare_class, "a", "()V");
    if (!k0 || !content_class || !pair_class || !attach_dir || !free_path || !send_app || !parse || !pair_first ||
        !pair_second || !int_value || !long_value) {
        Detail(result.detail, sizeof(result.detail), "file classes not found (version mismatch?)");
        return result;
    }
    if (prepare) {
        env->CallStaticVoidMethod(prepare_class, prepare);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // The extension WeChat shows on the bubble and the icon it picks.
    char extension[24] = {};
    if (const char *dot = strrchr(title, '.'))
        if (dot[1] && strlen(dot + 1) < sizeof(extension) && !strchr(dot, '/'))
            snprintf(extension, sizeof(extension), "%s", dot + 1);

    // Where WeChat keeps the attachment: its own naming (collision-free), then our file goes in.
    jstring jtitle = locals.keep(env->NewStringUTF(title));
    jstring jext = locals.keep(env->NewStringUTF(extension));
    jstring directory = locals.keep(static_cast<jstring>(env->CallStaticObjectMethod(k0, attach_dir)));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        directory = nullptr;
    }
    jstring destination =
        directory
            ? locals.keep(static_cast<jstring>(env->CallStaticObjectMethod(k0, free_path, directory, jtitle, jext)))
            : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        destination = nullptr;
    }
    if (!jtitle || !jext || !destination) {
        Detail(result.detail, sizeof(result.detail), "attachment directory unavailable");
        return result;
    }
    const char *target = env->GetStringUTFChars(destination, nullptr);
    if (!target) {
        Detail(result.detail, sizeof(result.detail), "attachment path unavailable");
        return result;
    }
    char attach_path[1024];
    snprintf(attach_path, sizeof(attach_path), "%s", target);
    env->ReleaseStringUTFChars(destination, target);
    // WeChat creates its directories lazily; make sure this one exists before linking into it.
    MakeParents(attach_path);
    if (!LinkOrCopy(path, attach_path, result.detail, sizeof(result.detail))) return result;

    // The content WeChat parses back into its AppMessage: a plain file, type 6.
    char xml[4096];
    size_t used = 0;
    xml[0] = 0;
    auto put = [&](const char *text) {
        const size_t n = strlen(text);
        if (used + n + 1 < sizeof(xml)) {
            memcpy(xml + used, text, n + 1);
            used += n;
        }
    };
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
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        content = nullptr;
    }
    if (!content) {
        unlink(attach_path);
        Detail(result.detail, sizeof(result.detail), "WeChat could not parse the file message");
        return result;
    }

    jstring jtalker = locals.keep(env->NewStringUTF(talker));
    jstring jempty = locals.keep(env->NewStringUTF(""));
    jstring jattach = locals.keep(env->NewStringUTF(attach_path));
    jobject pair = jtalker && jempty && jattach
                       ? locals.keep(env->CallStaticObjectMethod(k0, send_app, content, jempty, jempty, jtalker,
                                                                 jattach, static_cast<jbyteArray>(nullptr)))
                       : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        pair = nullptr;
        Detail(result.detail, sizeof(result.detail), "WeChat's file send threw");
    }
    if (!pair) {
        if (!result.detail[0]) Detail(result.detail, sizeof(result.detail), "WeChat's file send returned nothing");
        unlink(attach_path);
        return result;
    }
    jobject first = locals.keep(env->GetObjectField(pair, pair_first));
    jobject second = locals.keep(env->GetObjectField(pair, pair_second));
    const jint code = first ? env->CallIntMethod(first, int_value) : -1;
    const jlong local_id = second ? env->CallLongMethod(second, long_value) : -1;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    result.net_id = static_cast<int>(code);
    if (code != 0 || local_id <= 0) {
        unlink(attach_path);
        Detail(result.detail, sizeof(result.detail), "WeChat refused the file (code %d, id %lld)",
               static_cast<int>(code), static_cast<long long>(local_id));
        return result;
    }
    result.ok = true;
    result.local_id = local_id;
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "file %s handed to WeChat for %s (local id %lld)", title, talker,
                        static_cast<long long>(local_id));
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
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
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
    jmethodID element_ctor = Method(env, element_class, "<init>",
                                    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZILqi0/t2;Lb41/k7;)V");
    jmethodID cross_ctor = Method(
        env, cross_class, "<init>",
        "(Lb41/i7;Lpc5/qn6;Ljava/lang/String;Lpc5/p87;Ljava/lang/String;Lpc5/qn4;ZLqi0/r2;Ljava/lang/String;Lp95/f;ZZZ)V");
    jmethodID unique_name = StaticMethod(env, names_class, "a", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID prepare = StaticMethod(env, prepare_class, "a", "()V");
    if (!n0 || !service_interface || !element_class || !cross_class || !lookup || !element_ctor || !cross_ctor ||
        !unique_name) {
        Detail(result.detail, sizeof(result.detail), "video classes not found (version mismatch?)");
        return result;
    }
    jobject service = locals.keep(env->CallStaticObjectMethod(n0, lookup, service_interface));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        service = nullptr;
    }
    jclass service_class = service ? locals.keep(env->GetObjectClass(service)) : nullptr;
    jmethodID send = service_class ? Method(env, service_class, "cj", "(Lqi0/w2;Ljava/lang/String;)Z") : nullptr;
    if (!service || !send) {
        Detail(result.detail, sizeof(result.detail),
               service ? "video service has no cj() (version mismatch?)" : "video service unavailable");
        return result;
    }
    if (prepare) {
        env->CallStaticVoidMethod(prepare_class, prepare);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    char staged[1300];
    if (!Stage(path, staged, sizeof(staged), result.detail, sizeof(result.detail))) return result;
    jstring jtalker = locals.keep(env->NewStringUTF(talker));
    jstring jpath = locals.keep(env->NewStringUTF(staged));
    jstring jthumb = locals.keep(env->NewStringUTF(thumb_path ? thumb_path : ""));
    jstring jname =
        jtalker ? locals.keep(static_cast<jstring>(env->CallStaticObjectMethod(names_class, unique_name, jtalker)))
                : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        jname = nullptr;
    }
    // The chat UI's own arguments: compress as WeChat sees fit, no import copy.
    jobject cross =
        locals.keep(env->NewObject(cross_class, cross_ctor, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                   JNI_FALSE, nullptr, nullptr, nullptr, JNI_FALSE, JNI_FALSE, JNI_FALSE));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        cross = nullptr;
    }
    jobject element =
        (jname && jpath && jthumb && cross)
            ? locals.keep(env->NewObject(element_class, element_ctor, jname, jpath, jthumb, JNI_FALSE,
                                         static_cast<jint>(duration_s > 0 ? duration_s : 0), cross, nullptr))
            : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        element = nullptr;
    }
    if (!element) {
        unlink(staged);
        Detail(result.detail, sizeof(result.detail), "video task construction failed");
        return result;
    }
    const jboolean started = env->CallBooleanMethod(service, send, element, jtalker);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(result.detail, sizeof(result.detail), "WeChat's video send threw");
        return result;
    }
    if (!started) {
        unlink(staged);
        Detail(result.detail, sizeof(result.detail), "WeChat declined to start the video send");
        return result;
    }
    result.ok = true;
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "video handed to WeChat for %s", talker);
    return result;
}

SendResult SendQuote(const char *talker, const char *text, const QuoteRef &quote, const char *mention_ids) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !text || !*text || quote.svr_id <= 0 || !*quote.sender) {
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "empty target or text, or nothing to quote");
        return result;
    }
    JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
    if (!ReflectResolve(result.detail, sizeof(result.detail))) return result;
    Locals locals(env);
    jclass k0 = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.pluginsdk.model.app.k0")));
    jclass content_class = locals.keep(static_cast<jclass>(ReflectLoad("dx0.r")));
    jclass item_class =
        locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.plugin.msgquote.model.MsgQuoteItem")));
    jclass pair_class = locals.keep(env->FindClass("android/util/Pair"));
    jclass integer_class = locals.keep(env->FindClass("java/lang/Integer"));
    jclass long_class = locals.keep(env->FindClass("java/lang/Long"));
    jclass prepare_class = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.pluginsdk.ui.tools.p0")));
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID send_app = StaticMethod(
        env, k0, "I",
        "(Ldx0/r;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;");
    jmethodID content_ctor = Method(env, content_class, "<init>", "()V");
    jmethodID item_ctor = Method(env, item_class, "<init>", "()V");
    jfieldID title = Field(env, content_class, "f", "Ljava/lang/String;");
    jfieldID kind = Field(env, content_class, "i", "I");
    jfieldID quote_field = Field(env, content_class, "x2", "Lcom/tencent/mm/plugin/msgquote/model/MsgQuoteItem;");
    jfieldID item_type = Field(env, item_class, "d", "I");
    jfieldID item_svr = Field(env, item_class, "e", "J");
    jfieldID item_from = Field(env, item_class, "f", "Ljava/lang/String;");
    jfieldID item_chat = Field(env, item_class, "g", "Ljava/lang/String;");
    jfieldID item_name = Field(env, item_class, "h", "Ljava/lang/String;");
    jfieldID item_source = Field(env, item_class, "i", "Ljava/lang/String;");
    jfieldID item_content = Field(env, item_class, "m", "Ljava/lang/String;");
    jfieldID item_merged = Field(env, item_class, "n", "Ljava/lang/String;");
    jfieldID item_strid = Field(env, item_class, "p", "Ljava/lang/String;");
    jfieldID item_created = Field(env, item_class, "q", "J");
    jfieldID pair_first = Field(env, pair_class, "first", "Ljava/lang/Object;");
    jfieldID pair_second = Field(env, pair_class, "second", "Ljava/lang/Object;");
    jmethodID int_value = Method(env, integer_class, "intValue", "()I");
    jmethodID long_value = Method(env, long_class, "longValue", "()J");
    jmethodID prepare = StaticMethod(env, prepare_class, "a", "()V");
    const struct {
        const char *name;
        const void *found;
    } needed[] = {
        {"k0", k0},
        {"dx0.r", content_class},
        {"MsgQuoteItem", item_class},
        {"Pair", pair_class},
        {"k0.I", send_app},
        {"dx0.r.<init>", content_ctor},
        {"MsgQuoteItem.<init>", item_ctor},
        {"dx0.r.f", title},
        {"dx0.r.i", kind},
        {"dx0.r.x2", quote_field},
        {"item.d", item_type},
        {"item.e", item_svr},
        {"item.f", item_from},
        {"item.g", item_chat},
        {"item.h", item_name},
        {"item.i", item_source},
        {"item.m", item_content},
        {"item.n", item_merged},
        {"item.p", item_strid},
        {"item.q", item_created},
        {"Pair.first", pair_first},
        {"Pair.second", pair_second},
        {"Integer.intValue", int_value},
        {"Long.longValue", long_value},
    };
    for (const auto &entry : needed) {
        if (!entry.found) {
            Detail(result.detail, sizeof(result.detail), "quote classes not found (version mismatch?): %s", entry.name);
            return result;
        }
    }
    if (prepare) {
        env->CallStaticVoidMethod(prepare_class, prepare);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // What a reply carries about the quoted line, as the chat UI fills it. The quoted content is text
    // (the store turns anything else into a "[图片]"-style line), so the type is 1 whatever it was.
    jobject item = locals.keep(env->NewObject(item_class, item_ctor));
    jobject content = locals.keep(env->NewObject(content_class, content_ctor));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        item = content = nullptr;
    }
    if (!item || !content) {
        Detail(result.detail, sizeof(result.detail), "quote objects could not be built");
        return result;
    }
    char merged[2400] = {};
    if (mention_ids && *mention_ids)
        snprintf(merged, sizeof(merged), "<msgsource><atuserlist><![CDATA[%s]]></atuserlist></msgsource>", mention_ids);
    jstring jtext = locals.keep(env->NewStringUTF(text));
    jstring jtalker = locals.keep(env->NewStringUTF(quote.talker));
    jstring jsender = locals.keep(env->NewStringUTF(quote.sender));
    jstring jname = locals.keep(env->NewStringUTF(quote.display[0] ? quote.display : quote.sender));
    jstring jquoted = locals.keep(env->NewStringUTF(quote.text));
    jstring jempty = locals.keep(env->NewStringUTF(""));
    jstring jmerged = locals.keep(env->NewStringUTF(merged));
    jstring jtarget = locals.keep(env->NewStringUTF(talker));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(result.detail, sizeof(result.detail), "string allocation failed");
        return result;
    }
    env->SetIntField(item, item_type, 1);
    env->SetLongField(item, item_svr, static_cast<jlong>(quote.svr_id));
    env->SetObjectField(item, item_from, jtalker);
    env->SetObjectField(item, item_chat, jsender);
    env->SetObjectField(item, item_name, jname);
    env->SetObjectField(item, item_source, jempty);
    env->SetObjectField(item, item_content, jquoted);
    env->SetObjectField(item, item_merged, jmerged);
    env->SetObjectField(item, item_strid, jempty);
    env->SetLongField(item, item_created, static_cast<jlong>(quote.created_s));
    env->SetObjectField(content, title, jtext);
    env->SetIntField(content, kind, 57);
    env->SetObjectField(content, quote_field, item);

    jobject pair = locals.keep(env->CallStaticObjectMethod(k0, send_app, content, jempty, jempty, jtarget, jempty,
                                                           static_cast<jbyteArray>(nullptr)));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        pair = nullptr;
        Detail(result.detail, sizeof(result.detail), "WeChat's reply send threw");
    }
    if (!pair) {
        if (!result.detail[0]) Detail(result.detail, sizeof(result.detail), "WeChat's reply send returned nothing");
        return result;
    }
    jobject first = locals.keep(env->GetObjectField(pair, pair_first));
    jobject second = locals.keep(env->GetObjectField(pair, pair_second));
    const jint code = first ? env->CallIntMethod(first, int_value) : -1;
    // A reply goes through WeChat's newer send pipeline, which answers (0, null): accepted, with the
    // row inserted a moment later and its id not handed back. The caller finds the row in the store.
    const jlong local_id = second ? env->CallLongMethod(second, long_value) : -1;
    if (env->ExceptionCheck()) env->ExceptionClear();
    result.net_id = static_cast<int>(code);
    if (code != 0) {
        Detail(result.detail, sizeof(result.detail), "WeChat refused the reply (code %d)", static_cast<int>(code));
        return result;
    }
    result.ok = true;
    result.local_id = local_id > 0 ? local_id : -1;

    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "reply to %lld handed to WeChat for %s (local id %lld)",
                        quote.svr_id, talker, static_cast<long long>(result.local_id));
    return result;
}

SendResult SendForward(const char *talker, const char *title, const char *desc, const char *record_info) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !title || !*title || !record_info || !*record_info) {
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "empty target or nothing to forward");
        return result;
    }
    JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
    if (!ReflectResolve(result.detail, sizeof(result.detail))) return result;
    Locals locals(env);
    jclass k0 = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.pluginsdk.model.app.k0")));
    jclass content_class = locals.keep(static_cast<jclass>(ReflectLoad("dx0.r")));
    jclass pair_class = locals.keep(env->FindClass("android/util/Pair"));
    jclass integer_class = locals.keep(env->FindClass("java/lang/Integer"));
    jclass long_class = locals.keep(env->FindClass("java/lang/Long"));
    jclass prepare_class = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.pluginsdk.ui.tools.p0")));
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID send_app = StaticMethod(
        env, k0, "I",
        "(Ldx0/r;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;");
    jmethodID parse = StaticMethod(env, content_class, "v", "(Ljava/lang/String;)Ldx0/r;");
    jfieldID pair_first = Field(env, pair_class, "first", "Ljava/lang/Object;");
    jfieldID pair_second = Field(env, pair_class, "second", "Ljava/lang/Object;");
    jmethodID int_value = Method(env, integer_class, "intValue", "()I");
    jmethodID long_value = Method(env, long_class, "longValue", "()J");
    jmethodID prepare = StaticMethod(env, prepare_class, "a", "()V");
    if (!k0 || !content_class || !pair_class || !send_app || !parse || !pair_first || !pair_second || !int_value ||
        !long_value) {
        Detail(result.detail, sizeof(result.detail), "forward classes not found (version mismatch?)");
        return result;
    }
    if (prepare) {
        env->CallStaticVoidMethod(prepare_class, prepare);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // The appmsg WeChat parses back into its AppMessage: type 19, the records in <recorditem>. The
    // <url> is the page WeChat shows on a client too old to open a record. The records are XML
    // that is itself inside this XML, so they travel as CDATA (the record escapes its own '>', a
    // "]]>" cannot occur in it).
    TextBuf xml;
    xml.Append("<msg><appmsg appid=\"\" sdkver=\"0\"><title>");
    xml.Text(title);
    xml.Append("</title><des>");
    xml.Text(desc ? desc : "");
    xml.Append(
        "</des><action>view</action><type>19</type><showtype>0</showtype><content></content>"
        "<url>https://support.weixin.qq.com/cgi-bin/mmsupport-bin/readtemplate?t=page/favorite_record__w_unsupport&amp;from=singlemessage&amp;isappinstalled=0</url>"
        "<dataurl></dataurl><lowurl></lowurl><lowdataurl></lowdataurl><recorditem><![CDATA[");
    xml.Append(record_info);
    xml.Append(
        "]]></recorditem><thumburl></thumburl><messageaction></messageaction><extinfo></extinfo>"
        "<sourceusername></sourceusername><sourcedisplayname></sourcedisplayname><commenturl></commenturl>"
        "<appattach><totallen>0</totallen><attachid></attachid><emoticonmd5></emoticonmd5><fileext></fileext><aeskey></aeskey></appattach>"
        "</appmsg><fromusername></fromusername><scene>0</scene><appinfo><version>1</version><appname></appname></appinfo>"
        "<commenturl></commenturl></msg>");
    if (xml.failed || !xml.data) {
        Detail(result.detail, sizeof(result.detail), "out of memory");
        return result;
    }
    jstring jxml = locals.keep(env->NewStringUTF(xml.data));
    jobject content = jxml ? locals.keep(env->CallStaticObjectMethod(content_class, parse, jxml)) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        content = nullptr;
    }
    if (!content) {
        Detail(result.detail, sizeof(result.detail), "WeChat could not parse the chat record");
        return result;
    }

    jstring jtalker = locals.keep(env->NewStringUTF(talker));
    jstring jempty = locals.keep(env->NewStringUTF(""));
    jobject pair = jtalker && jempty
                       ? locals.keep(env->CallStaticObjectMethod(k0, send_app, content, jempty, jempty, jtalker, jempty,
                                                                 static_cast<jbyteArray>(nullptr)))
                       : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        pair = nullptr;
        Detail(result.detail, sizeof(result.detail), "WeChat's chat-record send threw");
    }
    if (!pair) {
        if (!result.detail[0])
            Detail(result.detail, sizeof(result.detail), "WeChat's chat-record send returned nothing");
        return result;
    }
    jobject first = locals.keep(env->GetObjectField(pair, pair_first));
    jobject second = locals.keep(env->GetObjectField(pair, pair_second));
    const jint code = first ? env->CallIntMethod(first, int_value) : -1;
    // The classic path answers (0, local id); the newer pipeline answers (0, null) and inserts the row later.
    const jlong local_id = second ? env->CallLongMethod(second, long_value) : -1;
    if (env->ExceptionCheck()) env->ExceptionClear();
    result.net_id = static_cast<int>(code);
    if (code != 0) {
        Detail(result.detail, sizeof(result.detail), "WeChat refused the chat record (code %d)",
               static_cast<int>(code));
        return result;
    }
    result.ok = true;
    result.local_id = local_id > 0 ? local_id : -1;
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "chat record handed to WeChat for %s (local id %lld)", talker,
                        static_cast<long long>(result.local_id));
    return result;
}

namespace {
// A registered voice file that never became a message would sit in WeChat's voice table as a recording
// in progress; mark it failed the way the recorder does when it gives up (VoiceLogic.setError).
void AbandonVoice(JNIEnv *env, jclass logic, jstring name) {
    jmethodID set_error = StaticMethod(env, logic, "t", "(Ljava/lang/String;)Z");
    if (!set_error) return;
    env->CallStaticBooleanMethod(logic, set_error, name);
    if (env->ExceptionCheck()) env->ExceptionClear();
}
} // namespace

SendResult SendVoice(const char *talker, const char *silk_path, int duration_ms) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !silk_path || !*silk_path || duration_ms <= 0) {
        Detail(result.detail, sizeof(result.detail), "empty target or path, or no duration");
        return result;
    }
    struct stat info{};
    if (stat(silk_path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "the voice file is missing or empty");
        return result;
    }
    JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
    if (!ReflectResolve(result.detail, sizeof(result.detail))) return result;
    Locals locals(env);
    jclass logic = locals.keep(static_cast<jclass>(ReflectLoad("v61.d1")));
    jclass n0 = locals.keep(static_cast<jclass>(ReflectLoad("ph5.n0")));
    jclass paths_interface = locals.keep(static_cast<jclass>(ReflectLoad("rn3.u0")));
    jclass kind_class = locals.keep(static_cast<jclass>(ReflectLoad("ou5.x")));
    jclass message_class = locals.keep(static_cast<jclass>(ReflectLoad("com.tencent.mm.storage.e9")));
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID start = StaticMethod(env, logic, "h", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID finish =
        StaticMethod(env, logic, "u", "(Ljava/lang/String;IILcom/tencent/mm/storage/e9;Ljava/lang/String;)Z");
    jmethodID lookup = StaticMethod(env, n0, "c", "(Ljava/lang/Class;)Lph5/m;");
    jclass subcore = locals.keep(static_cast<jclass>(ReflectLoad("v61.v0")));
    jclass uploader_class = locals.keep(static_cast<jclass>(ReflectLoad("yl.y0")));
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID uploader_of = StaticMethod(env, subcore, "dj", "()Lyl/y0;");
    jmethodID uploader_run = Method(env, uploader_class, "e", "()V");
    jfieldID legacy = kind_class ? env->GetStaticFieldID(kind_class, "j", "Lou5/x;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    const struct {
        const char *name;
        const void *found;
    } needed[] = {
        {"v61.d1", logic},      {"ph5.n0", n0},         {"rn3.u0", paths_interface},
        {"ou5.x", kind_class},  {"e9", message_class},  {"d1.h", start},
        {"d1.u", finish},       {"n0.c", lookup},       {"ou5.x.j", legacy},
        {"v0.dj", uploader_of}, {"y0.e", uploader_run},
    };
    for (const auto &entry : needed) {
        if (!entry.found) {
            Detail(result.detail, sizeof(result.detail), "voice classes not found (version mismatch?): %s", entry.name);
            return result;
        }
    }
    jobject paths = locals.keep(env->CallStaticObjectMethod(n0, lookup, paths_interface));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        paths = nullptr;
    }
    jclass paths_class = paths ? locals.keep(env->GetObjectClass(paths)) : nullptr;
    jmethodID voice_path =
        paths_class ? Method(env, paths_class, "Fj", "(Lou5/x;Ljava/lang/String;ZZ)Ljava/lang/String;") : nullptr;
    if (!voice_path) {
        Detail(result.detail, sizeof(result.detail), "voice path service unavailable");
        return result;
    }
    jobject kind = locals.keep(env->GetStaticObjectField(kind_class, legacy));

    // 1) WeChat registers a new voice file for this conversation and hands back its name.
    jstring jtalker = locals.keep(env->NewStringUTF(talker));
    jstring jprefix =
        locals.keep(env->NewStringUTF("amr_")); // every voice file WeChat writes is named amr_…, SILK included
    jstring name = locals.keep(static_cast<jstring>(env->CallStaticObjectMethod(logic, start, jtalker, jprefix)));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        name = nullptr;
    }
    if (!name) {
        Detail(result.detail, sizeof(result.detail), "WeChat would not register a voice file");
        return result;
    }
    // 2) The file goes where WeChat keeps voice files for that name.
    jstring destination =
        locals.keep(static_cast<jstring>(env->CallObjectMethod(paths, voice_path, kind, name, JNI_FALSE, JNI_TRUE)));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        destination = nullptr;
    }
    const char *target = destination ? env->GetStringUTFChars(destination, nullptr) : nullptr;
    if (!target || !*target) {
        Detail(result.detail, sizeof(result.detail), "voice file path unavailable");
        return result;
    }
    char path[1200];
    snprintf(path, sizeof(path), "%s", target);
    env->ReleaseStringUTFChars(destination, target);
    // Both the directories and the file are ours to create: the path service only computes the name.
    MakeParents(path);
    if (!LinkOrCopy(silk_path, path, result.detail, sizeof(result.detail))) {
        AbandonVoice(env, logic, name);
        return result;
    }
    // 3) The recorder's "stop": builds the message row and lets WeChat's uploader take it.
    const jboolean ok =
        env->CallStaticBooleanMethod(logic, finish, name, static_cast<jint>(duration_ms), static_cast<jint>(0),
                                     static_cast<jobject>(nullptr), static_cast<jstring>(nullptr));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(result.detail, sizeof(result.detail), "WeChat's voice send threw");
        unlink(path);
        AbandonVoice(env, logic, name);
        return result;
    }
    if (!ok) {
        Detail(result.detail, sizeof(result.detail), "WeChat refused the voice file");
        unlink(path);
        AbandonVoice(env, logic, name);
        return result;
    }
    // ...and, as the recorder does right after, wake the voice uploader so it picks the file up now.
    jobject uploader = locals.keep(env->CallStaticObjectMethod(subcore, uploader_of));
    if (uploader) env->CallVoidMethod(uploader, uploader_run);
    if (env->ExceptionCheck()) env->ExceptionClear();
    result.ok = true;
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "voice (%d ms) handed to WeChat for %s", duration_ms, talker);
    return result;
}
} // namespace satori
