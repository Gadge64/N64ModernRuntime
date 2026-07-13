#include "recomp.h"
#include <cstdio>
#include <string>
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

#define VI_NTSC_CLOCK 48681812

// [wcw2k] Hardware-accurate osAiSetFrequency return, opt-in per project (default off =
// WT/Revenge unchanged). On console the AI quantizes the rate through the DAC divisor
// and osAiSetFrequency RETURNS the actual rate (28800 -> dacRate 1690 -> 28805); AKI
// audio init sizes its heap allocations from that return value. Returning the requested
// rate unquantized shrinks WM2000's audio DMA buffers by a few samples each, shifting
// every subsequent audio-heap allocation ~0xA0-0x140 LOW — low enough that the audio
// manager's scheduler-client node lands INSIDE thread 6's linker-fixed boot stack
// (top 0x800839A0). The scheduler then writes message values over the stack slot
// where the transition chain saved s0 -> thread 6 restores s0=1 -> deterministic crash
// in func_800222D8 entering gameplay (boots 48-54, session-5 part 5).
bool wcw_ai_accurate_freq = false;

extern "C" void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t freq = ctx->r4;
    if (wcw_ai_accurate_freq) {
        uint32_t dacRate = (uint32_t)(((float)VI_NTSC_CLOCK / freq) + 0.5f);
        freq = VI_NTSC_CLOCK / dacRate;
        fprintf(stderr, "[wcw][ai] osAiSetFrequency(%u) -> %u (dacRate %u)\n", (unsigned)ctx->r4, freq, dacRate);
    }
    ctx->r2 = freq;
    ultramodern::set_audio_frequency(freq);
}

extern "C" void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    // [wcw] totals every 64 calls to see whether the audio driver still feeds buffers late in a run
    { static int n = 0; if ((n++ % 64) == 0) fprintf(stderr, "[wcw][ai] osAiSetNextBuffer #%d len=0x%X\n", n - 1, (unsigned)ctx->r5); }
    ultramodern::queue_audio_buffer(rdram, ctx->r4, ctx->r5);
    ctx->r2 = 0;
}

extern "C" void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = ultramodern::get_remaining_audio_bytes();
    { static int n = 0; if ((n++ % 256) == 0) fprintf(stderr, "[wcw][ai] osAiGetLength #%d -> 0x%X\n", n - 1, (unsigned)ctx->r2); }
}

extern "C" void osAiGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    // [wcw2k] Report AI_STATUS_FULL|AI_STATUS_DMA_BUSY while the host queue is backlogged
    // so the game's own FULL-wait loop paces generation (it blocks on its frame message, so
    // this never spins). Previously hardcoded 0 ("DMAs finish instantly"), which disabled
    // WM2000's only audio throttle.
    bool full = ultramodern::is_audio_backlogged();
    ctx->r2 = full ? 0xC0000000 : 0x00000000;
    { static int n = 0, fulls = 0; if (full) fulls++;
      if ((n++ % 4096) == 0) fprintf(stderr, "[wcw][ai] osAiGetStatus #%d fulls=%d\n", n - 1, fulls); }
}
