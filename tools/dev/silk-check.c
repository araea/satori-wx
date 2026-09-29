// Decodes a voice file made by the module with WeChat's own SILK decoder and prints, per second, the
// level and the rough pitch: an independent check that what was encoded is the audio that went in.
//
//   cp $(ls /data/app/*/com.tencent.mm-*/lib/arm64/libwechatvoicesilk.so) libcxxstl.200.so .   (root)
//   clang -O1 -o silk-check tools/dev/silk-check.c -ldl -lm
//   LD_LIBRARY_PATH=. ./silk-check voice.silk        (the staged files are in files/satori-wx-tmp*/send/)
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { int32_t API_sampleRate, frameSize, framesPerPacket, moreInternalDecoderFrames, inBandFECOffset; } DecCtl;
int main(int argc, char **argv) {
    void *lib = dlopen("./libwechatvoicesilk.so", RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    int (*size_fn)(int32_t *) = dlsym(lib, "SKP_Silk_SDK_Get_Decoder_Size");
    int (*init_fn)(void *) = dlsym(lib, "SKP_Silk_SDK_InitDecoder");
    int (*dec_fn)(void *, DecCtl *, int, const uint8_t *, int, int16_t *, int16_t *) = dlsym(lib, "SKP_Silk_SDK_Decode");
    if (!size_fn || !init_fn || !dec_fn) { puts("missing symbols"); return 1; }
    int32_t sz = 0; size_fn(&sz);
    void *state = malloc(sz);
    init_fn(state);
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(n); fread(data, 1, n, f); fclose(f);
    long pos = 0;
    if (data[0] == 2) pos = 1;
    pos += 9;
    DecCtl ctl = {16000, 0, 0, 0, 0};
    int16_t *pcm = malloc(16000 * 70 * 2); long total = 0; int packets = 0;
    while (pos + 2 <= n) {
        int len = data[pos] | data[pos + 1] << 8; pos += 2;
        if (len == 0xFFFF) break;
        int16_t got = 0;
        do {
            int rc = dec_fn(state, &ctl, 0, data + pos, len, pcm + total, &got);
            if (rc) { printf("decode error %d at packet %d\n", rc, packets); return 1; }
            total += got;
        } while (ctl.moreInternalDecoderFrames);
        pos += len; ++packets;
    }
    printf("packets %d samples %ld (%.2f s) rate %d\n", packets, total, total / 16000.0, ctl.API_sampleRate);
    // frequency + rms in windows of 1 s
    for (long w = 0; w + 16000 <= total; w += 16000) {
        double sum = 0; long cross = 0;
        for (long i = w + 1; i < w + 16000; ++i) { sum += (double)pcm[i] * pcm[i]; if (pcm[i-1] < 0 && pcm[i] >= 0) ++cross; }
        printf("second %ld: rms %.0f, ~%ld Hz\n", w / 16000, sqrt(sum / 16000), cross);
    }
    return 0;
}
