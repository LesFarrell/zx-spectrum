#define CHIPS_IMPL
#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/mem.h"
#include "chips/kbd.h"
#include "chips/clk.h"
#include "systems/zx.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

typedef struct AudioCapture {
    unsigned callbacks;
    unsigned samples;
    float first;
    float last;
    bool changed;
    bool finite;
} AudioCapture;

static void capture_audio(const float *samples, int num_samples, void *user_data)
{
    AudioCapture *capture = (AudioCapture *)user_data;
    for (int i = 0; i < num_samples; ++i) {
        if (capture->samples == 0)
            capture->first = samples[i];
        if (!isfinite(samples[i]))
            capture->finite = false;
        if (capture->samples > 0 && samples[i] != capture->last)
            capture->changed = true;
        capture->last = samples[i];
        ++capture->samples;
    }
    ++capture->callbacks;
}

static void test_audio_waveforms(void)
{
    uint8_t rom[0x4000] = {0};
    AudioCapture capture = {0, 0, 0.0f, 0.0f, false, true};
    zx_t machine;
    zx_desc_t desc;

    memset(&desc, 0, sizeof(desc));
    desc.type = ZX_TYPE_128;
    desc.audio.callback.func = capture_audio;
    desc.audio.callback.user_data = &capture;
    desc.audio.num_samples = 8;
    desc.audio.sample_rate = 44100;
    desc.roms.zx48k = (chips_range_t){ rom, sizeof(rom) };
    desc.roms.zx128_0 = (chips_range_t){ rom, sizeof(rom) };
    desc.roms.zx128_1 = (chips_range_t){ rom, sizeof(rom) };
    zx_init(&machine, &desc);

    beeper_set(&machine.beeper, true);
    zx_exec_ticks(&machine, 3000);
    beeper_set(&machine.beeper, false);
    zx_exec_ticks(&machine, 3000);

    CHECK(capture.callbacks >= 2);
    CHECK(capture.samples == capture.callbacks * 8);
    CHECK(capture.finite);
    CHECK(capture.changed);
    CHECK(capture.first != capture.last);
    zx_discard(&machine);
}

static void test_contention_timing(void)
{
    static const zx_type_t types[] = {
        ZX_TYPE_48K, ZX_TYPE_128, ZX_TYPE_PLUS2A, ZX_TYPE_PLUS3
    };
    static const uint32_t starts[] = { 14335u, 14361u, 14361u, 14361u };
    static const uint8_t patterns[][8] = {
        { 6, 5, 4, 3, 2, 1, 0, 0 },
        { 6, 5, 4, 3, 2, 1, 0, 0 },
        { 1, 0, 7, 6, 5, 4, 3, 2 },
        { 1, 0, 7, 6, 5, 4, 3, 2 },
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        zx_t machine;
        memset(&machine, 0, sizeof(machine));
        machine.type = types[i];

        CHECK(_zx_contention_delay_at(&machine, starts[i] - 1u) == 0);
        for (unsigned phase = 0; phase < 8; ++phase)
            CHECK(_zx_contention_delay_at(&machine, starts[i] + phase) == patterns[i][phase]);
        CHECK(_zx_contention_delay_at(&machine, starts[i] + 128u) == 0);
        CHECK(_zx_contention_delay_at(&machine, starts[i] + 192u *
            (types[i] == ZX_TYPE_48K ? 224u : 228u)) == 0);
    }
}

int main(void)
{
    static const zx_type_t types[] = {
        ZX_TYPE_48K, ZX_TYPE_128, ZX_TYPE_PLUS2A, ZX_TYPE_PLUS3
    };
    static const uint64_t frequencies[] = {
        3500000u, 3546894u, 3546894u, 3546894u
    };
    static const int frame_lines[] = { 312, 311, 311, 311 };
    static const int line_periods[] = { 224, 228, 228, 228 };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        uint8_t rom[0x4000];
        zx_t machine;
        zx_desc_t desc;

        memset(rom, 0, sizeof(rom));
        memset(&desc, 0, sizeof(desc));
        desc.type = types[i];
        desc.roms.zx48k = (chips_range_t){ rom, sizeof(rom) };
        desc.roms.zx128_0 = (chips_range_t){ rom, sizeof(rom) };
        desc.roms.zx128_1 = (chips_range_t){ rom, sizeof(rom) };
        desc.roms.zxplus3_0 = (chips_range_t){ rom, sizeof(rom) };
        desc.roms.zxplus3_1 = (chips_range_t){ rom, sizeof(rom) };
        desc.roms.zxplus3_2 = (chips_range_t){ rom, sizeof(rom) };
        desc.roms.zxplus3_3 = (chips_range_t){ rom, sizeof(rom) };
        zx_init(&machine, &desc);

        CHECK(machine.freq_hz == frequencies[i]);
        CHECK(machine.frame_scan_lines == frame_lines[i]);
        CHECK(machine.scanline_period == line_periods[i]);
        CHECK((uint32_t)machine.frame_scan_lines *
            (uint32_t)machine.scanline_period ==
            (types[i] == ZX_TYPE_48K ? 69888u : 70908u));
        CHECK(machine.frame_tstate == 0);
        CHECK(machine.scanline_y == 0);
        CHECK(machine.scanline_counter == line_periods[i]);
        CHECK(machine.beeper.period ==
            (int)((frequencies[i] * 16u) / 44100u));

        zx_exec_ticks(&machine, 10);
        CHECK(machine.ay.tick ==
            (types[i] == ZX_TYPE_48K ? 0u : 5u));
        zx_reset(&machine);
        CHECK(machine.ay.tick == 0u);

        zx_exec_ticks(&machine, (uint32_t)frame_lines[i] *
            (uint32_t)line_periods[i]);
        CHECK(machine.frame_tstate == 0);
        CHECK(machine.scanline_y == 0);
        CHECK(machine.scanline_counter == line_periods[i]);
        CHECK((machine.pins & Z80_INT) != 0);

        zx_reset(&machine);
        CHECK(machine.frame_tstate == 0);
        CHECK(machine.scanline_y == 0);
        CHECK(machine.scanline_counter == line_periods[i]);
        zx_discard(&machine);
    }
    test_contention_timing();
    test_audio_waveforms();

    if (failures != 0) {
        fprintf(stderr, "%d timing checks failed.\n", failures);
        return 1;
    }
    puts("All timing checks passed.");
    return 0;
}
