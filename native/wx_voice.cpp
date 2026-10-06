#include "wx_voice.h"
#include "audio_pcm.h"
#include "wx_send.h"
#include <errno.h>
#include <jni.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

namespace satori {
namespace {
constexpr size_t kMaxSilkFile = 4u << 20;
constexpr int kDecodeBudgetMs = 20000;

void Detail(char *out, size_t size, const char *format, ...) {
    if (!out || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(out, size, format, args);
    va_end(args);
}

jmethodID Method(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jmethodID id = env->GetMethodID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}
jmethodID StaticMethod(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jmethodID id = env->GetStaticMethodID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}
jfieldID Field(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jfieldID id = env->GetFieldID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

long long NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Everything MediaExtractor and MediaCodec need, resolved once per call.
struct Media {
    jclass extractor, codec, format, info;
    jmethodID extractor_new, set_source, track_count, track_format, select_track, read_sample, sample_time, advance,
        extractor_release;
    jmethodID get_string, contains_key, get_integer, get_long;
    jmethodID create_decoder, configure, start, dequeue_input, input_buffer, queue_input, dequeue_output, output_buffer,
        release_output, output_format, codec_stop, codec_release;
    jmethodID info_new;
    jfieldID info_offset, info_size, info_flags;
    bool Load(JNIEnv *env) {
        extractor = env->FindClass("android/media/MediaExtractor");
        codec = env->FindClass("android/media/MediaCodec");
        format = env->FindClass("android/media/MediaFormat");
        info = env->FindClass("android/media/MediaCodec$BufferInfo");
        if (env->ExceptionCheck()) env->ExceptionClear();
        extractor_new = Method(env, extractor, "<init>", "()V");
        set_source = Method(env, extractor, "setDataSource", "(Ljava/lang/String;)V");
        track_count = Method(env, extractor, "getTrackCount", "()I");
        track_format = Method(env, extractor, "getTrackFormat", "(I)Landroid/media/MediaFormat;");
        select_track = Method(env, extractor, "selectTrack", "(I)V");
        read_sample = Method(env, extractor, "readSampleData", "(Ljava/nio/ByteBuffer;I)I");
        sample_time = Method(env, extractor, "getSampleTime", "()J");
        advance = Method(env, extractor, "advance", "()Z");
        extractor_release = Method(env, extractor, "release", "()V");
        get_string = Method(env, format, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
        contains_key = Method(env, format, "containsKey", "(Ljava/lang/String;)Z");
        get_integer = Method(env, format, "getInteger", "(Ljava/lang/String;)I");
        get_long = Method(env, format, "getLong", "(Ljava/lang/String;)J");
        create_decoder =
            StaticMethod(env, codec, "createDecoderByType", "(Ljava/lang/String;)Landroid/media/MediaCodec;");
        configure = Method(env, codec, "configure",
                           "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V");
        start = Method(env, codec, "start", "()V");
        dequeue_input = Method(env, codec, "dequeueInputBuffer", "(J)I");
        input_buffer = Method(env, codec, "getInputBuffer", "(I)Ljava/nio/ByteBuffer;");
        queue_input = Method(env, codec, "queueInputBuffer", "(IIIJI)V");
        dequeue_output = Method(env, codec, "dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I");
        output_buffer = Method(env, codec, "getOutputBuffer", "(I)Ljava/nio/ByteBuffer;");
        release_output = Method(env, codec, "releaseOutputBuffer", "(IZ)V");
        output_format = Method(env, codec, "getOutputFormat", "()Landroid/media/MediaFormat;");
        codec_stop = Method(env, codec, "stop", "()V");
        codec_release = Method(env, codec, "release", "()V");
        info_new = Method(env, info, "<init>", "()V");
        info_offset = Field(env, info, "offset", "I");
        info_size = Field(env, info, "size", "I");
        info_flags = Field(env, info, "flags", "I");
        return extractor && codec && format && info && extractor_new && set_source && track_count && track_format &&
               select_track && read_sample && sample_time && advance && extractor_release && get_string &&
               contains_key && get_integer && get_long && create_decoder && configure && start && dequeue_input &&
               input_buffer && queue_input && dequeue_output && output_buffer && release_output && output_format &&
               codec_stop && codec_release && info_new && info_offset && info_size && info_flags;
    }
};

// Reads the integer `key` of a MediaFormat, or `fallback` when it has none.
int FormatInt(JNIEnv *env, const Media &m, jobject format, const char *key, int fallback) {
    jstring name = env->NewStringUTF(key);
    int value = fallback;
    if (name && env->CallBooleanMethod(format, m.contains_key, name)) {
        const jint got = env->CallIntMethod(format, m.get_integer, name);
        if (!env->ExceptionCheck()) value = got;
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (name) env->DeleteLocalRef(name);
    return value;
}

// Decodes any audio Android can play to interleaved 16-bit PCM (at most `max_frames` frames).
// The result is malloc'd; `rate` and `channels` describe it. `truncated` is set when it stopped early.
int16_t *DecodeWithMediaCodec(JNIEnv *env, const char *path, size_t max_frames_seconds, size_t *frames, int *rate,
                              int *channels, bool *truncated, char *detail, size_t detail_size) {
    Media m{};
    if (!m.Load(env)) {
        Detail(detail, detail_size, "Android's media classes are unavailable");
        return nullptr;
    }
    int16_t *pcm = nullptr;
    size_t used = 0, capacity = 0;
    *frames = 0;
    *truncated = false;
    jobject extractor = env->NewObject(m.extractor, m.extractor_new);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        extractor = nullptr;
    }
    if (!extractor) {
        Detail(detail, detail_size, "no media extractor");
        return nullptr;
    }
    jobject codec = nullptr;
    bool ok = false;
    do {
        jstring jpath = env->NewStringUTF(path);
        env->CallVoidMethod(extractor, m.set_source, jpath);
        env->DeleteLocalRef(jpath);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            Detail(detail, detail_size, "not audio Android can read");
            break;
        }
        const jint tracks = env->CallIntMethod(extractor, m.track_count);
        int selected = -1;
        jobject format = nullptr;
        jstring mime = nullptr;
        for (jint i = 0; i < tracks && selected < 0; ++i) {
            jobject candidate = env->CallObjectMethod(extractor, m.track_format, i);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                continue;
            }
            jstring key = env->NewStringUTF("mime");
            jstring kind =
                candidate ? static_cast<jstring>(env->CallObjectMethod(candidate, m.get_string, key)) : nullptr;
            env->DeleteLocalRef(key);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                kind = nullptr;
            }
            const char *text = kind ? env->GetStringUTFChars(kind, nullptr) : nullptr;
            const bool audio = text && !strncmp(text, "audio/", 6);
            if (text) env->ReleaseStringUTFChars(kind, text);
            if (audio) {
                selected = i;
                format = candidate;
                mime = kind;
            } else {
                if (candidate) env->DeleteLocalRef(candidate);
                if (kind) env->DeleteLocalRef(kind);
            }
        }
        if (selected < 0) {
            Detail(detail, detail_size, "no audio track");
            break;
        }
        env->CallVoidMethod(extractor, m.select_track, selected);
        codec = env->CallStaticObjectMethod(m.codec, m.create_decoder, mime);
        if (env->ExceptionCheck() || !codec) {
            env->ExceptionClear();
            codec = nullptr;
            Detail(detail, detail_size, "no decoder for this audio format");
            break;
        }
        env->CallVoidMethod(codec, m.configure, format, static_cast<jobject>(nullptr), static_cast<jobject>(nullptr),
                            static_cast<jint>(0));
        if (!env->ExceptionCheck()) env->CallVoidMethod(codec, m.start);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            Detail(detail, detail_size, "the decoder would not start");
            break;
        }
        *rate = FormatInt(env, m, format, "sample-rate", 44100);
        *channels = FormatInt(env, m, format, "channel-count", 2);
        int encoding = 2; // ENCODING_PCM_16BIT
        jobject info = env->NewObject(m.info, m.info_new);
        const long long stop_at = NowMs() + kDecodeBudgetMs;
        bool input_done = false, output_done = false;
        size_t limit_frames = 0;
        while (!output_done && NowMs() < stop_at) {
            if (env->PushLocalFrame(16) != 0) break;
            if (!input_done) {
                const jint index = env->CallIntMethod(codec, m.dequeue_input, static_cast<jlong>(10000));
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    env->PopLocalFrame(nullptr);
                    Detail(detail, detail_size, "decoder failed");
                    goto finished;
                }
                if (index >= 0) {
                    jobject buffer = env->CallObjectMethod(codec, m.input_buffer, index);
                    const jint size =
                        buffer ? env->CallIntMethod(extractor, m.read_sample, buffer, static_cast<jint>(0)) : -1;
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        env->PopLocalFrame(nullptr);
                        Detail(detail, detail_size, "decoder failed");
                        goto finished;
                    }
                    if (size < 0) {
                        env->CallVoidMethod(codec, m.queue_input, index, 0, 0, static_cast<jlong>(0),
                                            4 /* BUFFER_FLAG_END_OF_STREAM */);
                        input_done = true;
                    } else {
                        const jlong time = env->CallLongMethod(extractor, m.sample_time);
                        env->CallVoidMethod(codec, m.queue_input, index, 0, size, time, 0);
                        env->CallBooleanMethod(extractor, m.advance);
                    }
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        env->PopLocalFrame(nullptr);
                        Detail(detail, detail_size, "decoder failed");
                        goto finished;
                    }
                }
            }
            const jint out = env->CallIntMethod(codec, m.dequeue_output, info, static_cast<jlong>(10000));
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                env->PopLocalFrame(nullptr);
                Detail(detail, detail_size, "decoder failed");
                goto finished;
            }
            if (out == -2 /* INFO_OUTPUT_FORMAT_CHANGED */) {
                jobject changed = env->CallObjectMethod(codec, m.output_format);
                if (changed && !env->ExceptionCheck()) {
                    *rate = FormatInt(env, m, changed, "sample-rate", *rate);
                    *channels = FormatInt(env, m, changed, "channel-count", *channels);
                    encoding = FormatInt(env, m, changed, "pcm-encoding", 2);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
                limit_frames = max_frames_seconds * static_cast<size_t>(*rate);
            } else if (out >= 0) {
                if (!limit_frames) limit_frames = max_frames_seconds * static_cast<size_t>(*rate);
                const jint offset = env->GetIntField(info, m.info_offset), size = env->GetIntField(info, m.info_size),
                           flags = env->GetIntField(info, m.info_flags);
                jobject buffer = size > 0 ? env->CallObjectMethod(codec, m.output_buffer, out) : nullptr;
                auto *data = buffer ? static_cast<const unsigned char *>(env->GetDirectBufferAddress(buffer)) : nullptr;
                if (data && *channels > 0) {
                    const size_t sample_bytes = encoding == 4 ? 4 : 2;
                    const size_t samples = static_cast<size_t>(size) / sample_bytes;
                    if (used + samples > capacity) {
                        capacity = (used + samples) * 2;
                        auto *grown = static_cast<int16_t *>(realloc(pcm, capacity * sizeof(int16_t)));
                        if (!grown) {
                            env->CallVoidMethod(codec, m.release_output, out, JNI_FALSE);
                            env->PopLocalFrame(nullptr);
                            Detail(detail, detail_size, "out of memory");
                            goto finished;
                        }
                        pcm = grown;
                    }
                    for (size_t i = 0; i < samples; ++i) {
                        const unsigned char *p = data + static_cast<size_t>(offset) + i * sample_bytes;
                        if (encoding == 4) {
                            float f;
                            memcpy(&f, p, 4);
                            long v = lrintf(f * 32767.0f);
                            pcm[used + i] = static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                        } else {
                            memcpy(&pcm[used + i], p, 2);
                        }
                    }
                    used += samples;
                }
                env->CallVoidMethod(codec, m.release_output, out, JNI_FALSE);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (flags & 4) output_done = true;
                if (limit_frames && *channels > 0 && used / static_cast<size_t>(*channels) > limit_frames) {
                    *truncated = true;
                    output_done = true;
                }
            }
            env->PopLocalFrame(nullptr);
        }
        if (!output_done) {
            Detail(detail, detail_size, "decoding took too long");
            break;
        }
        ok = used > 0;
        if (!ok) Detail(detail, detail_size, "the file decoded to nothing");
    } while (false);
finished:
    if (codec) {
        env->CallVoidMethod(codec, m.codec_stop);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->CallVoidMethod(codec, m.codec_release);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->CallVoidMethod(extractor, m.extractor_release);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!ok || *channels < 1) {
        free(pcm);
        return nullptr;
    }
    *frames = used / static_cast<size_t>(*channels);
    return pcm;
}

// 16 kHz mono PCM -> a SILK file, with WeChat's own encoder: the sequence its recording-to-voice
// conversion uses (SilkEncInit(16000, 16000, 4), 640-byte packets, the first output carries the header).
bool EncodeSilk(JNIEnv *env, const int16_t *pcm, size_t count, const char *out_path, char *detail, size_t detail_size) {
    jclass recorder = static_cast<jclass>(ReflectLoad("com.tencent.mm.modelvoice.MediaRecorder"));
    if (env->ExceptionCheck()) env->ExceptionClear();
    jmethodID init = StaticMethod(env, recorder, "SilkEncInit", "(IIIJ)J");
    jmethodID encode = StaticMethod(env, recorder, "SilkDoEnc", "([BS[B[SZJ)I");
    jmethodID uninit = StaticMethod(env, recorder, "SilkEncUnInit", "(J)I");
    if (!init || !encode || !uninit) {
        if (recorder) env->DeleteLocalRef(recorder);
        Detail(detail, detail_size, "WeChat's SILK encoder was not found (version mismatch?)");
        return false;
    }
    const jlong handle = env->CallStaticLongMethod(recorder, init, 16000, 16000, 4, static_cast<jlong>(0));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(recorder);
        Detail(detail, detail_size, "WeChat's SILK encoder would not start");
        return false;
    }
    if (!handle) {
        env->DeleteLocalRef(recorder);
        Detail(detail, detail_size, "WeChat's SILK encoder would not start");
        return false;
    }
    FILE *file = fopen(out_path, "wb");
    bool ok = file != nullptr;
    if (!ok) Detail(detail, detail_size, "cannot write %s: %s", out_path, strerror(errno));
    jbyteArray in = env->NewByteArray(640), out = env->NewByteArray(1280);
    jshortArray produced = env->NewShortArray(1);
    unsigned char chunk[640], packet[1280];
    size_t packets = 0;
    for (size_t pos = 0; ok && pos < count; pos += 320) {
        const size_t take = count - pos < 320 ? count - pos : 320;
        memset(chunk, 0, sizeof(chunk));
        memcpy(chunk, pcm + pos, take * sizeof(int16_t)); // little-endian 16-bit, the last packet zero-padded
        env->SetByteArrayRegion(in, 0, 640, reinterpret_cast<const jbyte *>(chunk));
        const jint rc =
            env->CallStaticIntMethod(recorder, encode, in, static_cast<jshort>(640), out, produced, JNI_FALSE, handle);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            Detail(detail, detail_size, "WeChat's SILK encoder threw");
            ok = false;
            break;
        }
        if (rc != 0) {
            Detail(detail, detail_size, "WeChat's SILK encoder failed (code %d)", static_cast<int>(rc));
            ok = false;
            break;
        }
        jshort length = 0;
        env->GetShortArrayRegion(produced, 0, 1, &length);
        if (length > 0 && length <= 1280) {
            env->GetByteArrayRegion(out, 0, length, reinterpret_cast<jbyte *>(packet));
            if (fwrite(packet, 1, static_cast<size_t>(length), file) != static_cast<size_t>(length)) {
                Detail(detail, detail_size, "cannot write the voice file");
                ok = false;
                break;
            }
            ++packets;
        }
    }
    env->CallStaticIntMethod(recorder, uninit, handle);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (file && fclose(file) != 0) ok = false;
    if (in) env->DeleteLocalRef(in);
    if (out) env->DeleteLocalRef(out);
    if (produced) env->DeleteLocalRef(produced);
    env->DeleteLocalRef(recorder);
    if (ok && !packets) {
        Detail(detail, detail_size, "the encoder produced nothing");
        ok = false;
    }
    if (!ok) unlink(out_path);
    return ok;
}

// A path in `send/` next to the input, for the SILK file we make.
bool StagingPath(const char *in_path, char *out, size_t capacity) {
    char directory[1100];
    snprintf(directory, sizeof(directory), "%s", in_path);
    char *slash = strrchr(directory, '/');
    if (!slash) return false;
    *slash = 0;
    char staging[1200];
    snprintf(staging, sizeof(staging), "%s/send", directory);
    if (mkdir(staging, 0700) && errno != EEXIST) return false;
    static unsigned counter = 0;
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return snprintf(out, capacity, "%s/voice-%lld-%u.silk", staging,
                    static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000,
                    ++counter) < static_cast<int>(capacity);
}

unsigned char *ReadSmall(const char *path, size_t *size) {
    struct stat info{};
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<size_t>(info.st_size) > kMaxSilkFile)
        return nullptr;
    FILE *file = fopen(path, "rb");
    if (!file) return nullptr;
    auto *data = static_cast<unsigned char *>(malloc(static_cast<size_t>(info.st_size)));
    if (data && fread(data, 1, static_cast<size_t>(info.st_size), file) != static_cast<size_t>(info.st_size)) {
        free(data);
        data = nullptr;
    }
    fclose(file);
    if (data) *size = static_cast<size_t>(info.st_size);
    return data;
}
} // namespace

VoicePrep VoicePrepare(const char *in_path, char *out_path, size_t out_capacity, unsigned *duration_ms, char *detail,
                       size_t detail_size) {
    *duration_ms = 0;
    // A stream that already is WeChat's voice format needs no work (and gets the 0x02 WeChat writes if it lacks it).
    size_t size = 0;
    if (unsigned char *data = ReadSmall(in_path, &size)) {
        SilkInfo silk;
        const bool is_silk = SilkInspect(data, size, &silk) && silk.valid;
        if (is_silk) {
            if (silk.duration_ms > kVoiceMaxMs) {
                free(data);
                return VoicePrep::TooLong;
            }
            *duration_ms = silk.duration_ms;
            if (silk.prefixed) {
                free(data);
                snprintf(out_path, out_capacity, "%s", in_path);
                return VoicePrep::Ready;
            }
            char staged[1300];
            FILE *file = StagingPath(in_path, staged, sizeof(staged)) ? fopen(staged, "wb") : nullptr;
            const bool wrote = file && fputc(0x02, file) != EOF && fwrite(data, 1, size, file) == size;
            if (file) fclose(file);
            free(data);
            if (!wrote) {
                Detail(detail, detail_size, "cannot stage the voice file");
                return VoicePrep::Failed;
            }
            snprintf(out_path, out_capacity, "%s", staged);
            return VoicePrep::Ready;
        }
        free(data);
    }

    int16_t *pcm = nullptr;
    size_t count = 0;
    bool truncated = false;
    const size_t limit =
        static_cast<size_t>(kVoiceMaxMs / 1000 + 1) * 16000; // one second past the limit, to tell "too long"
    bool have = WavToPcm16k(in_path, &pcm, &count, limit, &truncated);
    if (!have) {
        JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
        if (!env) {
            Detail(detail, detail_size, "JavaVM unavailable");
            return VoicePrep::Failed;
        }
        if (!ReflectResolve(detail, detail_size)) return VoicePrep::Failed;
        size_t frames = 0;
        int rate = 0, channels = 0;
        int16_t *raw = DecodeWithMediaCodec(env, in_path, kVoiceMaxMs / 1000 + 1, &frames, &rate, &channels, &truncated,
                                            detail, detail_size);
        if (!raw) return VoicePrep::Failed;
        pcm = ResampleTo16kMono(raw, frames, channels, rate, &count);
        free(raw);
        have = pcm && count;
    }
    if (!have) {
        free(pcm);
        Detail(detail, detail_size, "the audio could not be converted");
        return VoicePrep::Failed;
    }
    const unsigned ms = static_cast<unsigned>(count * 1000 / 16000);
    if (truncated || ms > kVoiceMaxMs) {
        free(pcm);
        return VoicePrep::TooLong;
    }
    if (ms < 200) {
        free(pcm);
        Detail(detail, detail_size, "the audio is too short to be a voice message");
        return VoicePrep::Failed;
    }
    char staged[1300];
    JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
    const bool ok =
        env && StagingPath(in_path, staged, sizeof(staged)) && EncodeSilk(env, pcm, count, staged, detail, detail_size);
    free(pcm);
    if (!ok) {
        if (!detail[0]) Detail(detail, detail_size, "cannot create the voice file");
        return VoicePrep::Failed;
    }
    snprintf(out_path, out_capacity, "%s", staged);
    *duration_ms = ms;
    return VoicePrep::Ready;
}
} // namespace satori
