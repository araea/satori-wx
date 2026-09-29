#pragma once
#include <stddef.h>
// Turning an <audio> element's file into WeChat's voice format.
//
// A WeChat voice message is a SILK stream (see SendVoice). Clients send whatever audio they have,
// so the module decodes it (WAV natively, everything else with Android's own MediaExtractor /
// MediaCodec through JNI), brings it to 16 kHz mono and encodes it with WeChat's own SILK encoder
// (com.tencent.mm.modelvoice.MediaRecorder.SilkEncInit / SilkDoEnc, the calls WeChat makes to
// turn a recording into a voice file). No codec is shipped and none is hooked.
namespace satori {
enum class VoicePrep {
    Ready,      // `out_path` is a SILK file WeChat will play (it may be the input itself)
    TooLong,    // WeChat's voice limit is 60 seconds; the caller sends a file instead
    Failed,     // not audio we can read, or the encoder is unavailable; the caller sends a file
};
constexpr unsigned kVoiceMaxMs = 60000;
// `in_path` is the uploaded file. When the result is Ready, `out_path` names the SILK file (a
// staging file next to the input unless the input already was a valid WeChat SILK stream) and
// `duration_ms` its length.
VoicePrep VoicePrepare(const char *in_path, char *out_path, size_t out_capacity, unsigned *duration_ms,
                       char *detail, size_t detail_size);
} // namespace satori
