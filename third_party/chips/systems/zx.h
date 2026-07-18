#pragma once
/*#
    # zx.h

    A ZX Spectrum 48K / 128 emulator in a C header.

    Do this:
    ~~~C
    #define CHIPS_IMPL
    ~~~
    before you include this file in *one* C or C++ file to create the
    implementation.

    Optionally provide the following macros with your own implementation

    ~~~C
    CHIPS_ASSERT(c)
    ~~~
        your own assert macro (default: assert(c))

    You need to include the following headers before including zx.h:

    - chips/chips_common.h
    - chips/z80.h
    - chips/beeper.h
    - chips/ay38910.h
    - chips/mem.h
    - chips/kbd.h
    - chips/clk.h

    ## The ZX Spectrum 48K

    TODO!

    ## The ZX Spectrum 128

    TODO!

    ## TODO:
    - 'contended memory' timing and IO port timing
    - reads from port 0xFF must return 'current VRAM bytes
    - video decoding only has scanline accuracy, not pixel accuracy

    ## zlib/libpng license

    Copyright (c) 2018 Andre Weissflog
    This software is provided 'as-is', without any express or implied warranty.
    In no event will the authors be held liable for any damages arising from the
    use of this software.
    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:
        1. The origin of this software must not be misrepresented; you must not
        claim that you wrote the original software. If you use this software in a
        product, an acknowledgment in the product documentation would be
        appreciated but is not required.
        2. Altered source versions must be plainly marked as such, and must not
        be misrepresented as being the original software.
        3. This notice may not be removed or altered from any source
        distribution.
#*/
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>

#ifdef __cplusplus
extern "C" {
#endif

// bump this whenever the zx_t struct layout changes
#define ZX_SNAPSHOT_VERSION (0x0005)

#define ZX_MAX_AUDIO_SAMPLES (1024)      // max number of audio samples in internal sample buffer
#define ZX_DEFAULT_AUDIO_SAMPLES (128)   // default number of samples in internal sample buffer
#define ZX_FRAMEBUFFER_WIDTH (512)
#define ZX_FRAMEBUFFER_HEIGHT (256)
#define ZX_FRAMEBUFFER_SIZE_BYTES (ZX_FRAMEBUFFER_WIDTH * ZX_FRAMEBUFFER_HEIGHT)
#define ZX_DISPLAY_WIDTH (320)
#define ZX_DISPLAY_HEIGHT (256)
#define ZX_PLUS3_MAX_SECTOR_SIZE (8192)

// ZX Spectrum models
typedef enum {
    ZX_TYPE_48K,
    ZX_TYPE_128,
    ZX_TYPE_PLUS3,
} zx_type_t;

// ZX Spectrum joystick types
typedef enum {
    ZX_JOYSTICKTYPE_NONE,
    ZX_JOYSTICKTYPE_KEMPSTON,
    ZX_JOYSTICKTYPE_SINCLAIR_1,
    ZX_JOYSTICKTYPE_SINCLAIR_2,
} zx_joystick_type_t;

// joystick mask bits
#define ZX_JOYSTICK_RIGHT   (1<<0)
#define ZX_JOYSTICK_LEFT    (1<<1)
#define ZX_JOYSTICK_DOWN    (1<<2)
#define ZX_JOYSTICK_UP      (1<<3)
#define ZX_JOYSTICK_BTN     (1<<4)

typedef bool (*zx_tape_input_callback_t)(void *user_data, uint64_t tick_count);
typedef bool (*zx_tape_load_trap_callback_t)(void *user_data, void *machine);
typedef bool (*zx_disk_ready_callback_t)(void *user_data, uint8_t drive);
typedef bool (*zx_disk_read_sector_callback_t)(
    void *user_data,
    uint8_t drive,
    uint8_t c,
    uint8_t h,
    uint8_t r,
    uint8_t n,
    uint8_t *buffer,
    uint32_t buffer_size,
    uint32_t *data_size,
    uint8_t *st1,
    uint8_t *st2);
typedef bool (*zx_disk_write_sector_callback_t)(
    void *user_data,
    uint8_t drive,
    uint8_t c,
    uint8_t h,
    uint8_t r,
    uint8_t n,
    const uint8_t *buffer,
    uint32_t data_size);
typedef bool (*zx_disk_sector_id_callback_t)(
    void *user_data,
    uint8_t drive,
    uint8_t c,
    uint8_t h,
    uint8_t index,
    uint8_t *out_c,
    uint8_t *out_h,
    uint8_t *out_r,
    uint8_t *out_n,
    uint8_t *st1,
    uint8_t *st2);

// config parameters for zx_init()
typedef struct {
    zx_type_t type;                     // default is ZX_TYPE_48K
    zx_joystick_type_t joystick_type;   // what joystick to emulate, default is ZX_JOYSTICK_NONE
    chips_debug_t debug;                // optional debugger hook
    struct {
        chips_audio_callback_t callback;
        int num_samples;
        int sample_rate;
        float beeper_volume;
        float ay_volume;
    } audio;
    struct {
        zx_tape_input_callback_t callback;
        zx_tape_load_trap_callback_t load_trap;
        void *user_data;
    } tape;
    struct {
        zx_disk_ready_callback_t ready;
        zx_disk_read_sector_callback_t read_sector;
        zx_disk_write_sector_callback_t write_sector;
        zx_disk_sector_id_callback_t sector_id;
        void *user_data;
    } disk;
    // ROM images
    struct {
        // ZX Spectrum 48K
        chips_range_t zx48k;
        // ZX Spectrum 128
        chips_range_t zx128_0;
        chips_range_t zx128_1;
        // ZX Spectrum +3
        chips_range_t zxplus3_0;
        chips_range_t zxplus3_1;
        chips_range_t zxplus3_2;
        chips_range_t zxplus3_3;
    } roms;
} zx_desc_t;

// ZX emulator state
typedef struct {
    z80_t cpu;
    beeper_t beeper;
    ay38910_t ay;
    zx_type_t type;
    zx_joystick_type_t joystick_type;
    bool memory_paging_disabled;
    uint8_t kbd_joymask;        // joystick mask from keyboard joystick emulation
    uint8_t joy_joymask;        // joystick mask from zx_joystick()
    uint32_t tick_count;
    uint32_t frame_tstate;
    uint8_t last_mem_config;    // last out to 0x7FFD
    uint8_t last_plus3_mem_config; // last out to 0x1FFD
    uint8_t last_fe_out;        // last out value to 0xFE port
    uint8_t blink_counter;      // incremented on each vblank
    uint8_t border_color;
    int frame_scan_lines;
    int top_border_scanlines;
    int scanline_period;
    int scanline_counter;
    int scanline_y;
    int int_counter;
    uint32_t display_ram_bank;
    kbd_t kbd;
    mem_t mem;
    uint64_t pins;
    uint64_t freq_hz;
    bool valid;
    chips_debug_t debug;
    zx_tape_input_callback_t tape_callback;
    zx_tape_load_trap_callback_t tape_load_trap;
    void *tape_user_data;
    zx_disk_ready_callback_t disk_ready;
    zx_disk_read_sector_callback_t disk_read_sector;
    zx_disk_write_sector_callback_t disk_write_sector;
    zx_disk_sector_id_callback_t disk_sector_id;
    void *disk_user_data;
    struct {
        chips_audio_callback_t callback;
        int num_samples;
        int sample_pos;
        float sample_buffer[ZX_MAX_AUDIO_SAMPLES];
    } audio;
    struct {
        uint8_t command;
        uint8_t params[8];
        uint8_t results[7];
        uint8_t param_count;
        uint8_t param_expected;
        uint8_t result_count;
        uint8_t result_index;
        uint8_t last_drive;
        uint8_t cylinder[4];
        uint8_t id_index[4][2];
        uint8_t transfer_mode;
        uint8_t transfer_c;
        uint8_t transfer_h;
        uint8_t transfer_r;
        uint8_t transfer_n;
        uint8_t transfer_eot;
        uint8_t transfer_st1;
        uint8_t transfer_st2;
        uint8_t last_bad_c;
        uint8_t last_bad_h;
        uint8_t last_bad_r;
        uint8_t last_bad_n;
        uint8_t bad_sector_repeats;
        uint32_t transfer_size;
        uint32_t transfer_pos;
        uint32_t data_ready_tick;
        uint8_t transfer[ZX_PLUS3_MAX_SECTOR_SIZE];
        bool interrupt_pending;
    } plus3_fdc;
    uint8_t ram[8][0x4000];
    uint8_t rom[4][0x4000];
    uint8_t junk[0x4000];
    alignas(64) uint8_t fb[ZX_FRAMEBUFFER_SIZE_BYTES];
} zx_t;

// initialize a new ZX Spectrum instance
void zx_init(zx_t* sys, const zx_desc_t* desc);
// discard a ZX Spectrum instance
void zx_discard(zx_t* sys);
// reset a ZX Spectrum instance
void zx_reset(zx_t* sys);
// query information about display requirements, can be called with nullptr
chips_display_info_t zx_display_info(zx_t* sys);
// run ZX Spectrum instance for a given number of microseconds, return number of ticks
uint32_t zx_exec(zx_t* sys, uint32_t micro_seconds);
// send a key-down event
void zx_key_down(zx_t* sys, int key_code);
// send a key-up event
void zx_key_up(zx_t* sys, int key_code);
// enable/disable joystick emulation
void zx_set_joystick_type(zx_t* sys, zx_joystick_type_t type);
// get current joystick emulation type
zx_joystick_type_t zx_joystick_type(zx_t* sys);
// set joystick mask (combination of ZX_JOYSTICK_*)
void zx_joystick(zx_t* sys, uint8_t mask);
// set a callback used for EAR/tape input sampling during ULA reads
void zx_set_tape_input(zx_t* sys, zx_tape_input_callback_t callback, void *user_data);
// set a callback used to fast-trap the ROM tape loader for standard blocks
void zx_set_tape_load_trap(zx_t* sys, zx_tape_load_trap_callback_t callback, void *user_data);
// set callbacks used by the Spectrum +3 floppy controller
void zx_set_disk_callbacks(
    zx_t* sys,
    zx_disk_ready_callback_t ready,
    zx_disk_read_sector_callback_t read_sector,
    zx_disk_write_sector_callback_t write_sector,
    zx_disk_sector_id_callback_t sector_id,
    void *user_data);
// notify the +3 controller that media was inserted or ejected
void zx_notify_disk_changed(zx_t* sys);
// load a ZX Z80 file into the emulator
bool zx_quickload(zx_t* sys, chips_range_t data);
// save a snapshot, patches any pointers to zero, returns a snapshot version
uint32_t zx_save_snapshot(zx_t* sys, zx_t* dst);
// load a snapshot, returns false if snapshot version doesn't match
bool zx_load_snapshot(zx_t* sys, uint32_t version, zx_t* src);

#ifdef __cplusplus
} // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>
#ifndef CHIPS_ASSERT
    #include <assert.h>
    #define CHIPS_ASSERT(c) assert(c)
#endif

static void _zx_init_memory_map(zx_t* sys);
static void _zx_init_keyboard_matrix(zx_t* sys);
static uint8_t _zx_contention_delay(zx_t* sys);
static bool _zx_is_contended_addr(zx_t* sys, uint16_t addr);
static bool _zx_should_apply_memory_contention(zx_t* sys, uint64_t pins);

#define _ZX_DEFAULT(val,def) (((val) != 0) ? (val) : (def))

#define _ZX_48K_FREQUENCY (3500000)
#define _ZX_128_FREQUENCY (3546894)

void zx_init(zx_t* sys, const zx_desc_t* desc) {
    CHIPS_ASSERT(sys && desc);
    if (desc->debug.callback.func) { CHIPS_ASSERT(desc->debug.stopped); }

    memset(sys, 0, sizeof(zx_t));
    sys->valid = true;
    sys->type = desc->type;
    sys->joystick_type = desc->joystick_type;
    sys->freq_hz = (sys->type == ZX_TYPE_48K) ? _ZX_48K_FREQUENCY : _ZX_128_FREQUENCY;
    sys->audio.callback = desc->audio.callback;
    sys->audio.num_samples = _ZX_DEFAULT(desc->audio.num_samples, ZX_DEFAULT_AUDIO_SAMPLES);
    CHIPS_ASSERT(sys->audio.num_samples <= ZX_MAX_AUDIO_SAMPLES);
    sys->debug = desc->debug;
    sys->tape_callback = desc->tape.callback;
    sys->tape_load_trap = desc->tape.load_trap;
    sys->tape_user_data = desc->tape.user_data;
    sys->disk_ready = desc->disk.ready;
    sys->disk_read_sector = desc->disk.read_sector;
    sys->disk_write_sector = desc->disk.write_sector;
    sys->disk_sector_id = desc->disk.sector_id;
    sys->disk_user_data = desc->disk.user_data;

    // initalize the hardware
    sys->border_color = 0;
    if (ZX_TYPE_128 == sys->type) {
        CHIPS_ASSERT(desc->roms.zx128_0.ptr && (desc->roms.zx128_0.size == 0x4000));
        CHIPS_ASSERT(desc->roms.zx128_1.ptr && (desc->roms.zx128_1.size == 0x4000));
        memcpy(sys->rom[0], desc->roms.zx128_0.ptr, 0x4000);
        memcpy(sys->rom[1], desc->roms.zx128_1.ptr, 0x4000);
        sys->display_ram_bank = 5;
        sys->frame_scan_lines = 311;
        sys->top_border_scanlines = 63;
        sys->scanline_period = 228;
    }
    else if (ZX_TYPE_PLUS3 == sys->type) {
        CHIPS_ASSERT(desc->roms.zxplus3_0.ptr && (desc->roms.zxplus3_0.size == 0x4000));
        CHIPS_ASSERT(desc->roms.zxplus3_1.ptr && (desc->roms.zxplus3_1.size == 0x4000));
        CHIPS_ASSERT(desc->roms.zxplus3_2.ptr && (desc->roms.zxplus3_2.size == 0x4000));
        CHIPS_ASSERT(desc->roms.zxplus3_3.ptr && (desc->roms.zxplus3_3.size == 0x4000));
        memcpy(sys->rom[0], desc->roms.zxplus3_0.ptr, 0x4000);
        memcpy(sys->rom[1], desc->roms.zxplus3_1.ptr, 0x4000);
        memcpy(sys->rom[2], desc->roms.zxplus3_2.ptr, 0x4000);
        memcpy(sys->rom[3], desc->roms.zxplus3_3.ptr, 0x4000);
        sys->display_ram_bank = 5;
        sys->frame_scan_lines = 311;
        sys->top_border_scanlines = 63;
        sys->scanline_period = 228;
    }
    else {
        CHIPS_ASSERT(desc->roms.zx48k.ptr && (desc->roms.zx48k.size == 0x4000));
        memcpy(sys->rom[0], desc->roms.zx48k.ptr, 0x4000);
        sys->display_ram_bank = 0;
        sys->frame_scan_lines = 312;
        sys->top_border_scanlines = 64;
        sys->scanline_period = 224;
    }
    sys->scanline_counter = sys->scanline_period;

    sys->pins = z80_init(&sys->cpu);

    const int audio_hz = _ZX_DEFAULT(desc->audio.sample_rate, 44100);
    beeper_init(&sys->beeper, &(beeper_desc_t){
        .tick_hz = (int)sys->freq_hz,
        .sound_hz = audio_hz,
        .base_volume = _ZX_DEFAULT(desc->audio.beeper_volume, 0.25f),
    });
    if (ZX_TYPE_48K != sys->type) {
        ay38910_init(&sys->ay, &(ay38910_desc_t){
            .type = AY38910_TYPE_8912,
            .tick_hz = (int)sys->freq_hz / 2,
            .sound_hz = audio_hz,
            .magnitude = _ZX_DEFAULT(desc->audio.ay_volume, 0.5f)
        });
    }
    _zx_init_memory_map(sys);
    _zx_init_keyboard_matrix(sys);
}

void zx_discard(zx_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->valid = false;
}

void zx_reset(zx_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->pins = z80_reset(&sys->cpu);
    beeper_reset(&sys->beeper);
    if (sys->type != ZX_TYPE_48K) {
        ay38910_reset(&sys->ay);
    }
    sys->memory_paging_disabled = false;
    sys->kbd_joymask = 0;
    sys->joy_joymask = 0;
    sys->frame_tstate = 0;
    sys->last_mem_config = 0;
    sys->last_plus3_mem_config = 0;
    sys->last_fe_out = 0;
    sys->scanline_counter = sys->scanline_period;
    sys->scanline_y = 0;
    sys->blink_counter = 0;
    if (sys->type == ZX_TYPE_48K) {
        sys->display_ram_bank = 0;
    }
    else {
        sys->display_ram_bank = 5;
    }
    if (sys->type == ZX_TYPE_PLUS3) {
        memset(&sys->plus3_fdc, 0, sizeof(sys->plus3_fdc));
    }
    _zx_init_memory_map(sys);
}

static bool _zx_decode_scanline(zx_t* sys) {
    /* this is called by the timer callback for every PAL line, controlling
        the vidmem decoding and vblank interrupt

        detailed information about frame timings is here:
        for 48K:    http://rk.nvg.ntnu.no/sinclair/faq/tech_48.html#48K
        for 128K:   http://rk.nvg.ntnu.no/sinclair/faq/tech_128.html

        one PAL line takes 224 T-states on 48K, and 228 T-states on 128K
        one PAL frame is 312 lines on 48K, and 311 lines on 128K

        decode the next videomem line into the emulator framebuffer,
        the border area of a real Spectrum is bigger than the emulator
        (the emu has 32 pixels border on each side, the hardware has:

        63 or 64 lines top border
        56 border lines bottom border
        48 pixels on each side horizontal border
    */
    const int top_decode_line = sys->top_border_scanlines - 32;
    const int btm_decode_line = sys->top_border_scanlines + 192 + 32;
    if ((sys->scanline_y >= top_decode_line) && (sys->scanline_y < btm_decode_line)) {
        const uint16_t y = sys->scanline_y - top_decode_line;
        uint8_t* dst = &sys->fb[y * ZX_FRAMEBUFFER_WIDTH];
        const uint8_t* vidmem_bank = sys->ram[sys->display_ram_bank];
        const bool blink = 0 != (sys->blink_counter & 0x10);
        if ((y < 32) || (y >= 224)) {
            // upper/lower border
            for (int x = 0; x < ZX_DISPLAY_WIDTH; x++) {
                *dst++ = sys->border_color;
            }
        }
        else {
            /* compute video memory Y offset (inside 256x192 area)
                this is how the 16-bit video memory address is computed
                from X and Y coordinates:
                | 0| 1| 0|Y7|Y6|Y2|Y1|Y0|Y5|Y4|Y3|X4|X3|X2|X1|X0|
            */
            const uint16_t yy = y-32;
            const uint16_t y_offset = ((yy & 0xC0)<<5) | ((yy & 0x07)<<8) | ((yy & 0x38)<<2);

            // left border
            for (int x = 0; x < (4*8); x++) {
                *dst++ = sys->border_color;
            }

            // valid 256x192 vidmem area
            for (uint16_t x = 0; x < 32; x++) {
                const uint16_t pix_offset = y_offset | x;
                const uint16_t clr_offset = 0x1800 + (((yy & ~0x7)<<2) | x);

                // pixel mask and color attribute bytes
                const uint8_t pix = vidmem_bank[pix_offset];
                const uint8_t clr = vidmem_bank[clr_offset];

                // foreground and background color
                uint8_t fg, bg;
                if ((clr & (1<<7)) && blink) {
                    fg = (clr>>3) & 7;
                    bg = clr & 7;
                }
                else {
                    fg = clr & 7;
                    bg = (clr>>3) & 7;
                }
                // color bit 6: standard vs bright
                fg |= (clr & (1<<6)) >> 3;
                bg |= (clr & (1<<6)) >> 3;

                for (int px = 7; px >=0; px--) {
                    *dst++ = pix & (1<<px) ? fg : bg;
                }
            }

            // right border
            for (int x = 0; x < (4*8); x++) {
                *dst++ = sys->border_color;
            }
        }
    }

    if (sys->scanline_y++ >= sys->frame_scan_lines) {
        // start new frame, request vblank interrupt
        sys->scanline_y = 0;
        sys->blink_counter++;
        return true;
    }
    else {
        return false;
    }
}

// ZX128 memory mapping
static void _zx_update_memory_map_zx128(zx_t* sys, uint8_t data) {
    if (!sys->memory_paging_disabled) {
        sys->last_mem_config = data;
        // bit 3 defines the video scanout memory bank (5 or 7)
        sys->display_ram_bank = (data & (1<<3)) ? 7 : 5;
        // only last memory bank is mappable
        mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->ram[data & 0x7]);

        // ROM0 or ROM1
        if (data & (1<<4)) {
            // bit 4 set: ROM1
            mem_map_rom(&sys->mem, 0, 0x0000, 0x4000, sys->rom[1]);
        }
        else {
            // bit 4 clear: ROM0
            mem_map_rom(&sys->mem, 0, 0x0000, 0x4000, sys->rom[0]);
        }
    }
    if (data & (1<<5)) {
        /* bit 5 prevents further changes to memory pages
            until computer is reset, this is used when switching
            to the 48k ROM
        */
        sys->memory_paging_disabled = true;
    }
}

/* Applies the +3's normal four-ROM layout or one of its four all-RAM maps
   from the values last written to ports 7FFD and 1FFD. */
static void _zx_update_memory_map_plus3(zx_t* sys) {
    static const uint8_t all_ram_banks[4][4] = {
        { 0, 1, 2, 3 },
        { 4, 5, 6, 7 },
        { 4, 5, 6, 3 },
        { 4, 7, 6, 3 },
    };

    if (sys->last_plus3_mem_config & 1) {
        const uint8_t config = (sys->last_plus3_mem_config >> 1) & 3;
        for (uint8_t slot = 0; slot < 4; ++slot) {
            mem_map_ram(
                &sys->mem,
                0,
                (uint16_t)(slot * 0x4000),
                0x4000,
                sys->ram[all_ram_banks[config][slot]]);
        }
    }
    else {
        const uint8_t rom_index =
            (uint8_t)(((sys->last_plus3_mem_config & (1<<2)) >> 1) |
                      ((sys->last_mem_config & (1<<4)) >> 4));
        mem_map_rom(&sys->mem, 0, 0x0000, 0x4000, sys->rom[rom_index]);
        mem_map_ram(&sys->mem, 0, 0x4000, 0x4000, sys->ram[5]);
        mem_map_ram(&sys->mem, 0, 0x8000, 0x4000, sys->ram[2]);
        mem_map_ram(
            &sys->mem,
            0,
            0xC000,
            0x4000,
            sys->ram[sys->last_mem_config & 0x7]);
    }
}

static void _zx_write_memory_control_plus3(zx_t* sys, uint16_t addr, uint8_t data) {
    if (sys->memory_paging_disabled) {
        return;
    }

    if ((addr & 0xF002) == 0x1000) {
        sys->last_plus3_mem_config = data;
        _zx_update_memory_map_plus3(sys);
    }
    else if ((addr & 0xC002) == 0x4000) {
        sys->last_mem_config = data;
        sys->display_ram_bank = (data & (1<<3)) ? 7 : 5;
        _zx_update_memory_map_plus3(sys);
        if (data & (1<<5)) {
            sys->memory_paging_disabled = true;
        }
    }
}

enum {
    _ZX_PLUS3_FDC_TRANSFER_NONE,
    _ZX_PLUS3_FDC_TRANSFER_READ,
    _ZX_PLUS3_FDC_TRANSFER_WRITE
};

static bool _zx_plus3_disk_ready(const zx_t* sys, uint8_t drive) {
    return sys->disk_ready != 0 && sys->disk_ready(sys->disk_user_data, drive);
}

static bool _zx_plus3_fdc_data_ready(const zx_t* sys) {
    return (int32_t)(sys->tick_count - sys->plus3_fdc.data_ready_tick) >= 0;
}

static uint32_t _zx_plus3_fdc_byte_ticks(const zx_t* sys) {
    const uint32_t ticks = (uint32_t)(sys->freq_hz / 31250);
    return ticks != 0 ? ticks : 1;
}

/* Reports the uPD765 phase through RQM, DIO, non-DMA, and busy bits. */
static uint8_t _zx_plus3_fdc_status(const zx_t* sys) {
    if (sys->plus3_fdc.result_index < sys->plus3_fdc.result_count) {
        return 0xD0; /* RQM | DIO | controller busy */
    }
    if (sys->plus3_fdc.transfer_mode == _ZX_PLUS3_FDC_TRANSFER_READ) {
        return (uint8_t)(_zx_plus3_fdc_data_ready(sys) ? 0xF0 : 0x70);
    }
    if (sys->plus3_fdc.transfer_mode == _ZX_PLUS3_FDC_TRANSFER_WRITE) {
        return (uint8_t)(_zx_plus3_fdc_data_ready(sys) ? 0xB0 : 0x30);
    }
    if (sys->plus3_fdc.param_count < sys->plus3_fdc.param_expected) {
        return 0x90; /* RQM | controller busy */
    }
    return 0x80; /* ready to accept a command */
}

static void _zx_plus3_fdc_set_results(
    zx_t* sys,
    const uint8_t* results,
    uint8_t count)
{
    CHIPS_ASSERT(count <= sizeof(sys->plus3_fdc.results));
    memcpy(sys->plus3_fdc.results, results, count);
    sys->plus3_fdc.result_count = count;
    sys->plus3_fdc.result_index = 0;
    sys->plus3_fdc.param_count = 0;
    sys->plus3_fdc.param_expected = 0;
    sys->plus3_fdc.transfer_mode = _ZX_PLUS3_FDC_TRANSFER_NONE;
    sys->plus3_fdc.transfer_pos = 0;
    sys->plus3_fdc.transfer_size = 0;
}

/*
    Enumerate a track in descriptor order.  dsk_get_sector_id() returns false
    at the index hole, allowing the controller to keep Read ID and data
    commands on one shared rotational position.
*/
static uint8_t _zx_plus3_fdc_sector_count(
    zx_t* sys,
    uint8_t drive,
    uint8_t cylinder,
    uint8_t head)
{
    uint8_t count = 0;
    if (sys->disk_sector_id == 0) {
        return 0;
    }
    while (count != 0xFF &&
           sys->disk_sector_id(
               sys->disk_user_data, drive, cylinder, head, count,
               0, 0, 0, 0, 0, 0)) {
        count++;
    }
    return count;
}

static bool _zx_plus3_fdc_next_id(
    zx_t* sys,
    uint8_t drive,
    uint8_t cylinder,
    uint8_t head,
    uint8_t* c,
    uint8_t* h,
    uint8_t* r,
    uint8_t* n,
    uint8_t* st1,
    uint8_t* st2)
{
    const uint8_t count =
        _zx_plus3_fdc_sector_count(sys, drive, cylinder, head);
    if (count == 0) {
        return false;
    }
    uint8_t index = sys->plus3_fdc.id_index[drive][head];
    if (index >= count) {
        index = 0;
    }
    if (!sys->disk_sector_id(
            sys->disk_user_data, drive, cylinder, head, index,
            c, h, r, n, st1, st2)) {
        return false;
    }
    sys->plus3_fdc.id_index[drive][head] =
        (uint8_t)((index + 1) < count ? index + 1 : 0);
    return true;
}

static void _zx_plus3_fdc_follow_sector(
    zx_t* sys,
    uint8_t drive,
    uint8_t cylinder,
    uint8_t head,
    uint8_t r,
    uint8_t n)
{
    const uint8_t count =
        _zx_plus3_fdc_sector_count(sys, drive, cylinder, head);
    if (count == 0) {
        return;
    }
    uint8_t start = sys->plus3_fdc.id_index[drive][head];
    if (start >= count) {
        start = 0;
    }
    for (uint8_t offset = 0; offset < count; offset++) {
        const uint8_t index = (uint8_t)((start + offset) % count);
        uint8_t id_c = 0;
        uint8_t id_h = 0;
        uint8_t id_r = 0;
        uint8_t id_n = 0;
        if (sys->disk_sector_id(
                sys->disk_user_data, drive, cylinder, head, index,
                &id_c, &id_h, &id_r, &id_n, 0, 0) &&
            id_c == cylinder && id_h == head && id_r == r && id_n == n) {
            sys->plus3_fdc.id_index[drive][head] =
                (uint8_t)((index + 1) < count ? index + 1 : 0);
            return;
        }
    }
}

static void _zx_plus3_fdc_finish_data(zx_t* sys, uint8_t st0, uint8_t st1, uint8_t st2) {
    uint8_t results[7] = {0};
    results[0] = st0;
    results[1] = st1;
    results[2] = st2;
    results[3] = sys->plus3_fdc.transfer_c;
    results[4] = sys->plus3_fdc.transfer_h;
    results[5] = sys->plus3_fdc.transfer_r;
    results[6] = sys->plus3_fdc.transfer_n;
    _zx_plus3_fdc_set_results(sys, results, 7);
}

static bool _zx_plus3_fdc_load_read_sector(zx_t* sys) {
    uint32_t data_size = 0;
    uint8_t st1 = 0;
    uint8_t st2 = 0;
    const uint8_t drive = sys->plus3_fdc.last_drive;
    const uint8_t head_drive = (uint8_t)(drive | ((sys->plus3_fdc.transfer_h & 1) << 2));

    if (!_zx_plus3_disk_ready(sys, drive)) {
        _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x48 | head_drive), 0, 0);
        return false;
    }
    if (sys->disk_read_sector == 0 ||
        !sys->disk_read_sector(
            sys->disk_user_data,
            drive,
            sys->plus3_fdc.transfer_c,
            sys->plus3_fdc.transfer_h,
            sys->plus3_fdc.transfer_r,
            sys->plus3_fdc.transfer_n,
            sys->plus3_fdc.transfer,
            sizeof(sys->plus3_fdc.transfer),
            &data_size,
            &st1,
            &st2) ||
        data_size == 0 ||
        data_size > sizeof(sys->plus3_fdc.transfer)) {
        _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x40 | head_drive), 0x04, 0);
        return false;
    }

    const bool sector_deleted = (st2 & 0x40) != 0;
    const bool wants_deleted = (sys->plus3_fdc.command & 0x1F) == 0x0C;
    if (sector_deleted == wants_deleted) {
        /* A matching data-address mark is not an error for this command. */
        st2 &= (uint8_t)~0x40;
    }
    else {
        /* Control Mark: Read Data and Read Deleted Data saw the other mark. */
        st2 |= 0x40;
    }

    /*
        A bad data CRC can be the on-disk representation of weak bits.  EDSK
        has no weak-bit encoding for a normal-sized sector, so vary repeated
        reads in the same deterministic pattern used by established +3
        emulators.  Speedlock loaders depend on the read not being identical.
    */
    if ((st1 & 0x20) != 0 && (st2 & 0x20) != 0) {
        if (sys->plus3_fdc.last_bad_c == sys->plus3_fdc.transfer_c &&
            sys->plus3_fdc.last_bad_h == sys->plus3_fdc.transfer_h &&
            sys->plus3_fdc.last_bad_r == sys->plus3_fdc.transfer_r &&
            sys->plus3_fdc.last_bad_n == sys->plus3_fdc.transfer_n) {
            if (sys->plus3_fdc.bad_sector_repeats != 0xFF) {
                sys->plus3_fdc.bad_sector_repeats++;
            }
        }
        else {
            sys->plus3_fdc.last_bad_c = sys->plus3_fdc.transfer_c;
            sys->plus3_fdc.last_bad_h = sys->plus3_fdc.transfer_h;
            sys->plus3_fdc.last_bad_r = sys->plus3_fdc.transfer_r;
            sys->plus3_fdc.last_bad_n = sys->plus3_fdc.transfer_n;
            sys->plus3_fdc.bad_sector_repeats = 0;
        }
        if (sys->plus3_fdc.bad_sector_repeats != 0) {
            for (uint32_t offset = 28; offset < data_size; offset += 29) {
                sys->plus3_fdc.transfer[offset] ^= (uint8_t)(offset + 1);
            }
        }
    }

    _zx_plus3_fdc_follow_sector(
        sys,
        drive,
        sys->plus3_fdc.transfer_c,
        sys->plus3_fdc.transfer_h,
        sys->plus3_fdc.transfer_r,
        sys->plus3_fdc.transfer_n);
    sys->plus3_fdc.transfer_st1 = st1;
    sys->plus3_fdc.transfer_st2 = st2;
    sys->plus3_fdc.transfer_size = data_size;
    sys->plus3_fdc.transfer_pos = 0;
    sys->plus3_fdc.data_ready_tick =
        sys->tick_count + _zx_plus3_fdc_byte_ticks(sys);
    sys->plus3_fdc.transfer_mode = _ZX_PLUS3_FDC_TRANSFER_READ;
    return true;
}

static void _zx_plus3_fdc_complete_sector(zx_t* sys, bool writing) {
    const uint8_t drive = sys->plus3_fdc.last_drive;
    const uint8_t head_drive = (uint8_t)(drive | ((sys->plus3_fdc.transfer_h & 1) << 2));
    bool ok = true;

    if (writing) {
        if (sys->disk_write_sector == 0) {
            _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x40 | head_drive), 0x02, 0);
            return;
        }
        ok = sys->disk_write_sector(
                sys->disk_user_data,
                drive,
                sys->plus3_fdc.transfer_c,
                sys->plus3_fdc.transfer_h,
                sys->plus3_fdc.transfer_r,
                sys->plus3_fdc.transfer_n,
                sys->plus3_fdc.transfer,
                sys->plus3_fdc.transfer_size);
        if (!ok) {
            _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x40 | head_drive), 0x04, 0);
            return;
        }
    }

    /*
        CRC, missing-address-mark, and control-mark conditions terminate after
        the affected sector has been transferred.  Continuing silently into
        the next sector loses the protection status Batman deliberately uses.
    */
    if ((sys->plus3_fdc.transfer_st1 | sys->plus3_fdc.transfer_st2) != 0) {
        _zx_plus3_fdc_finish_data(
            sys,
            (uint8_t)(0x40 | head_drive),
            sys->plus3_fdc.transfer_st1,
            sys->plus3_fdc.transfer_st2);
        return;
    }

    if (sys->plus3_fdc.transfer_r < sys->plus3_fdc.transfer_eot) {
        sys->plus3_fdc.transfer_r++;
        if (writing) {
            sys->plus3_fdc.transfer_pos = 0;
            sys->plus3_fdc.data_ready_tick =
                sys->tick_count + _zx_plus3_fdc_byte_ticks(sys);
            sys->plus3_fdc.transfer_mode = _ZX_PLUS3_FDC_TRANSFER_WRITE;
        }
        else {
            _zx_plus3_fdc_load_read_sector(sys);
        }
        return;
    }

    if ((sys->plus3_fdc.command & 0x80) != 0 &&
        sys->plus3_fdc.transfer_h == 0) {
        sys->plus3_fdc.transfer_h = 1;
        sys->plus3_fdc.transfer_r = sys->plus3_fdc.params[3];
        if (writing) {
            sys->plus3_fdc.transfer_pos = 0;
            sys->plus3_fdc.data_ready_tick =
                sys->tick_count + _zx_plus3_fdc_byte_ticks(sys);
            sys->plus3_fdc.transfer_mode = _ZX_PLUS3_FDC_TRANSFER_WRITE;
        }
        else {
            _zx_plus3_fdc_load_read_sector(sys);
        }
        return;
    }

    /*
        Reaching EOT without a terminal-count input ends a non-DMA +3
        transfer with End Of Cylinder, as on a real uPD765.
    */
    _zx_plus3_fdc_finish_data(
        sys, (uint8_t)(0x40 | head_drive), 0x80, 0);
}

static uint32_t _zx_plus3_fdc_requested_size(const zx_t* sys) {
    const uint8_t n = sys->plus3_fdc.params[4];
    if (n == 0 && sys->plus3_fdc.params[7] != 0) {
        return sys->plus3_fdc.params[7];
    }
    if (n > 6) {
        return 0;
    }
    return 128u << n;
}

static void _zx_plus3_fdc_begin_data(zx_t* sys, bool writing) {
    const uint8_t drive = sys->plus3_fdc.params[0] & 3;
    const uint8_t head_drive = sys->plus3_fdc.params[0] & 7;
    sys->plus3_fdc.last_drive = drive;
    sys->plus3_fdc.transfer_c = sys->plus3_fdc.params[1];
    sys->plus3_fdc.transfer_h = sys->plus3_fdc.params[2];
    sys->plus3_fdc.transfer_r = sys->plus3_fdc.params[3];
    sys->plus3_fdc.transfer_n = sys->plus3_fdc.params[4];
    sys->plus3_fdc.transfer_eot = sys->plus3_fdc.params[5];
    sys->plus3_fdc.transfer_st1 = 0;
    sys->plus3_fdc.transfer_st2 = 0;

    if (!_zx_plus3_disk_ready(sys, drive)) {
        _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x48 | head_drive), 0, 0);
        return;
    }
    if (writing) {
        const uint32_t size = _zx_plus3_fdc_requested_size(sys);
        if (size == 0 || size > sizeof(sys->plus3_fdc.transfer)) {
            _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x40 | head_drive), 0x04, 0);
            return;
        }
        sys->plus3_fdc.transfer_size = size;
        sys->plus3_fdc.transfer_pos = 0;
        sys->plus3_fdc.data_ready_tick =
            sys->tick_count + _zx_plus3_fdc_byte_ticks(sys);
        sys->plus3_fdc.transfer_mode = _ZX_PLUS3_FDC_TRANSFER_WRITE;
    }
    else {
        _zx_plus3_fdc_load_read_sector(sys);
    }
}

static void _zx_plus3_fdc_execute(zx_t* sys) {
    const uint8_t operation = sys->plus3_fdc.command & 0x1F;
    const uint8_t drive = sys->plus3_fdc.param_count > 0
        ? sys->plus3_fdc.params[0] & 3
        : sys->plus3_fdc.last_drive;
    const uint8_t head_drive = sys->plus3_fdc.param_count > 0
        ? sys->plus3_fdc.params[0] & 7
        : drive;
    uint8_t results[7] = {0};

    sys->plus3_fdc.last_drive = drive;
    switch (operation) {
        case 0x03: /* Specify */
            sys->plus3_fdc.param_count = 0;
            sys->plus3_fdc.param_expected = 0;
            break;
        case 0x04: { /* Sense Drive Status */
            const bool ready = _zx_plus3_disk_ready(sys, drive);
            bool double_sided = false;
            if (ready && sys->disk_sector_id != 0) {
                double_sided = sys->disk_sector_id(
                    sys->disk_user_data,
                    drive,
                    sys->plus3_fdc.cylinder[drive],
                    1,
                    0,
                    0, 0, 0, 0, 0, 0);
            }
            results[0] = (uint8_t)(
                head_drive |
                (sys->plus3_fdc.cylinder[drive] == 0 ? 0x10 : 0) |
                (ready ? 0x20 : 0) |
                (double_sided ? 0x08 : 0));
            _zx_plus3_fdc_set_results(sys, results, 1);
            break;
        }
        case 0x07: /* Recalibrate */
            sys->plus3_fdc.cylinder[drive] = 0;
            sys->plus3_fdc.interrupt_pending = true;
            sys->plus3_fdc.param_count = 0;
            sys->plus3_fdc.param_expected = 0;
            break;
        case 0x08: /* Sense Interrupt Status */
            if (sys->plus3_fdc.interrupt_pending) {
                results[0] = (uint8_t)(
                    (_zx_plus3_disk_ready(sys, sys->plus3_fdc.last_drive) ? 0x20 : 0x48) |
                    sys->plus3_fdc.last_drive);
                results[1] = sys->plus3_fdc.cylinder[sys->plus3_fdc.last_drive];
                sys->plus3_fdc.interrupt_pending = false;
                _zx_plus3_fdc_set_results(sys, results, 2);
            }
            else {
                results[0] = 0x80;
                _zx_plus3_fdc_set_results(sys, results, 1);
            }
            break;
        case 0x0F: /* Seek */
            sys->plus3_fdc.cylinder[drive] = sys->plus3_fdc.params[1];
            sys->plus3_fdc.interrupt_pending = true;
            sys->plus3_fdc.param_count = 0;
            sys->plus3_fdc.param_expected = 0;
            break;
        case 0x02: /* Read Track */
        case 0x06: /* Read Data */
        case 0x0C: /* Read Deleted Data */
            _zx_plus3_fdc_begin_data(sys, false);
            break;
        case 0x05: /* Write Data */
        case 0x09: /* Write Deleted Data */
            _zx_plus3_fdc_begin_data(sys, true);
            break;
        case 0x0A: { /* Read ID */
            uint8_t c = sys->plus3_fdc.cylinder[drive];
            uint8_t h = (head_drive >> 2) & 1;
            uint8_t r = 0;
            uint8_t n = 0;
            uint8_t st1 = 0;
            uint8_t st2 = 0;
            if (!_zx_plus3_disk_ready(sys, drive)) {
                sys->plus3_fdc.transfer_c = c;
                sys->plus3_fdc.transfer_h = h;
                sys->plus3_fdc.transfer_r = 0;
                sys->plus3_fdc.transfer_n = 0;
                _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x48 | head_drive), 0, 0);
            }
            else if (!_zx_plus3_fdc_next_id(
                         sys, drive, c, h,
                         &c, &h, &r, &n, &st1, &st2)) {
                sys->plus3_fdc.transfer_c = c;
                sys->plus3_fdc.transfer_h = h;
                sys->plus3_fdc.transfer_r = r;
                sys->plus3_fdc.transfer_n = n;
                _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x40 | head_drive), 0x04, 0);
            }
            else {
                /*
                    Read ID examines the ID field only.  EDSK stores data-field
                    CRC and deleted-mark information beside the descriptor;
                    neither belongs in a successful Read ID result.
                */
                if ((st2 & 0x20) != 0) {
                    st1 &= (uint8_t)~0x20;
                }
                st2 &= (uint8_t)~0x60;
                sys->plus3_fdc.transfer_c = c;
                sys->plus3_fdc.transfer_h = h;
                sys->plus3_fdc.transfer_r = r;
                sys->plus3_fdc.transfer_n = n;
                _zx_plus3_fdc_finish_data(
                    sys,
                    (uint8_t)(((st1 | st2) != 0 ? 0x40 : 0) | head_drive),
                    st1,
                    st2);
            }
            break;
        }
        case 0x0D: /* Format Track */
            sys->plus3_fdc.transfer_c = sys->plus3_fdc.cylinder[drive];
            sys->plus3_fdc.transfer_h = (head_drive >> 2) & 1;
            sys->plus3_fdc.transfer_r = 0;
            sys->plus3_fdc.transfer_n = sys->plus3_fdc.params[1];
            _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x40 | head_drive), 0x02, 0);
            break;
        case 0x11: /* Scan Equal */
        case 0x19: /* Scan Low Or Equal */
        case 0x1D: /* Scan High Or Equal */
            sys->plus3_fdc.transfer_c = sys->plus3_fdc.params[1];
            sys->plus3_fdc.transfer_h = sys->plus3_fdc.params[2];
            sys->plus3_fdc.transfer_r = sys->plus3_fdc.params[3];
            sys->plus3_fdc.transfer_n = sys->plus3_fdc.params[4];
            _zx_plus3_fdc_finish_data(sys, (uint8_t)(0x40 | head_drive), 0x04, 0);
            break;
        default: /* Invalid command */
            results[0] = 0x80;
            _zx_plus3_fdc_set_results(sys, results, 1);
            break;
    }
}

static uint8_t _zx_plus3_fdc_parameter_count(uint8_t command) {
    switch (command & 0x1F) {
        case 0x03: return 2;
        case 0x04:
        case 0x07:
        case 0x0A: return 1;
        case 0x08: return 0;
        case 0x0D: return 5;
        case 0x0F: return 2;
        case 0x02:
        case 0x05:
        case 0x06:
        case 0x09:
        case 0x0C:
        case 0x11:
        case 0x19:
        case 0x1D: return 8;
        default: return 0;
    }
}

static void _zx_plus3_fdc_write(zx_t* sys, uint8_t data) {
    if (sys->plus3_fdc.result_index < sys->plus3_fdc.result_count) {
        return;
    }
    if (sys->plus3_fdc.transfer_mode == _ZX_PLUS3_FDC_TRANSFER_WRITE) {
        if (!_zx_plus3_fdc_data_ready(sys)) {
            return;
        }
        sys->plus3_fdc.transfer[sys->plus3_fdc.transfer_pos++] = data;
        sys->plus3_fdc.data_ready_tick =
            sys->tick_count + _zx_plus3_fdc_byte_ticks(sys);
        if (sys->plus3_fdc.transfer_pos == sys->plus3_fdc.transfer_size) {
            _zx_plus3_fdc_complete_sector(sys, true);
        }
        return;
    }
    if (sys->plus3_fdc.param_count < sys->plus3_fdc.param_expected) {
        sys->plus3_fdc.params[sys->plus3_fdc.param_count++] = data;
        if (sys->plus3_fdc.param_count == sys->plus3_fdc.param_expected) {
            _zx_plus3_fdc_execute(sys);
        }
        return;
    }

    sys->plus3_fdc.command = data;
    sys->plus3_fdc.param_count = 0;
    sys->plus3_fdc.param_expected = _zx_plus3_fdc_parameter_count(data);
    if (sys->plus3_fdc.param_expected == 0) {
        _zx_plus3_fdc_execute(sys);
    }
}

static uint8_t _zx_plus3_fdc_read(zx_t* sys) {
    uint8_t data = 0xFF;
    if (sys->plus3_fdc.transfer_mode == _ZX_PLUS3_FDC_TRANSFER_READ &&
        sys->plus3_fdc.transfer_pos < sys->plus3_fdc.transfer_size) {
        if (!_zx_plus3_fdc_data_ready(sys)) {
            return data;
        }
        data = sys->plus3_fdc.transfer[sys->plus3_fdc.transfer_pos++];
        sys->plus3_fdc.data_ready_tick =
            sys->tick_count + _zx_plus3_fdc_byte_ticks(sys);
        if (sys->plus3_fdc.transfer_pos == sys->plus3_fdc.transfer_size) {
            _zx_plus3_fdc_complete_sector(sys, false);
        }
    }
    else if (sys->plus3_fdc.result_index < sys->plus3_fdc.result_count) {
        data = sys->plus3_fdc.results[sys->plus3_fdc.result_index++];
        if (sys->plus3_fdc.result_index == sys->plus3_fdc.result_count) {
            sys->plus3_fdc.result_index = 0;
            sys->plus3_fdc.result_count = 0;
        }
    }
    return data;
}

/* Returns the current machine's active contention delay for the current
   frame t-state, or zero when the ULA/gate array is not stealing the bus. */
static uint8_t _zx_contention_delay(zx_t* sys) {
    static const uint8_t ula_pattern[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
    const uint32_t t = sys->frame_tstate;

    if (sys->type == ZX_TYPE_48K) {
        if (t >= 14335) {
            const uint32_t rel = t - 14335;
            if (rel < (192u * 224u)) {
                const uint32_t line_pos = rel % 224u;
                if (line_pos < 128u) {
                    return ula_pattern[line_pos & 7u];
                }
            }
        }
        return 0;
    }

    if (sys->type == ZX_TYPE_128 || sys->type == ZX_TYPE_PLUS3) {
        if (t >= 14361) {
            const uint32_t rel = t - 14361;
            if (rel < (192u * 228u)) {
                const uint32_t line_pos = rel % 228u;
                if (line_pos < 128u) {
                    return ula_pattern[line_pos & 7u];
                }
            }
        }
        return 0;
    }

    return 0;
}

/* Reports whether the supplied address currently maps to a RAM page that is
   contended on the active machine model. */
static bool _zx_is_contended_addr(zx_t* sys, uint16_t addr) {
    if (sys->type == ZX_TYPE_48K) {
        return addr >= 0x4000 && addr < 0x8000;
    }

    if (sys->type == ZX_TYPE_128) {
        if (addr >= 0x4000 && addr < 0x8000) {
            return true;
        }
        if (addr >= 0xC000) {
            return 0 != ((sys->last_mem_config & 0x7) & 1);
        }
        return false;
    }

    if (sys->type == ZX_TYPE_PLUS3) {
        uint8_t bank;
        if (sys->last_plus3_mem_config & 1) {
            static const uint8_t all_ram_banks[4][4] = {
                { 0, 1, 2, 3 },
                { 4, 5, 6, 7 },
                { 4, 5, 6, 3 },
                { 4, 7, 6, 3 },
            };
            const uint8_t config = (sys->last_plus3_mem_config >> 1) & 3;
            bank = all_ram_banks[config][addr >> 14];
        }
        else if (addr < 0x4000) {
            return false;
        }
        else if (addr < 0x8000) {
            bank = 5;
        }
        else if (addr < 0xC000) {
            bank = 2;
        }
        else {
            bank = sys->last_mem_config & 7;
        }
        return bank >= 4;
    }

    return false;
}

/* Ferranti ULA machines can be stalled whenever a contended address is on the
   bus. */
static bool _zx_should_apply_memory_contention(zx_t* sys, uint64_t pins) {
    if (!_zx_is_contended_addr(sys, Z80_GET_ADDR(pins))) {
        return false;
    }
    return 0 == (pins & Z80_IORQ);
}

static uint64_t _zx_tick(zx_t* sys, uint64_t pins) {
    bool new_frame = false;

    if (_zx_contention_delay(sys) > 0 && _zx_should_apply_memory_contention(sys, pins)) {
        pins |= Z80_WAIT;
    }
    else {
        pins &= ~Z80_WAIT;
    }
    pins = z80_tick(&sys->cpu, pins);
    /*
        The Spectrum does not place a peripheral-supplied opcode on the data
        bus during interrupt acknowledge.  Model the resulting 0xFF bus value
        explicitly; leaving stale data from the preceding bus cycle makes IM2
        jump through an effectively random vector.
    */
    if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
        Z80_SET_DATA(pins, 0xFF);
    }

    // video decoding and vblank interrupt
    if (--sys->scanline_counter <= 0) {
        sys->scanline_counter += sys->scanline_period;
        // decode next video scanline
        if (_zx_decode_scanline(sys)) {
            // request vblank interrupt
            pins |= Z80_INT;
            // hold the INT pin for 32 ticks
            sys->int_counter = 32;
            new_frame = true;
        }
    }

    // clear INT pin after 32 ticks
    if (pins & Z80_INT) {
        if (--sys->int_counter < 0) {
            pins &= ~Z80_INT;
        }
    }

    if (pins & Z80_MREQ) {
        // a memory request
        // FIXME: 'contended memory'
        const uint16_t addr = Z80_GET_ADDR(pins);
        if (pins & Z80_RD) {
            Z80_SET_DATA(pins, mem_rd(&sys->mem, addr));
        }
        else if (pins & Z80_WR) {
            mem_wr(&sys->mem, addr, Z80_GET_DATA(pins));
        }
    }
    else if ((pins & Z80_IORQ) && !(pins & Z80_M1)) {
        if ((pins & Z80_A0) == 0) {
            /* Spectrum ULA (...............0)
                Bits 5 and 7 as read by INning from Port 0xfe are always one
            */
            if (pins & Z80_RD) {
                // read from ULA
                uint8_t data = (1<<7)|(1<<5);
                bool ear_high = false;
                if (sys->tape_callback != NULL) {
                    ear_high = sys->tape_callback(sys->tape_user_data, sys->tick_count);
                }
                else if (sys->last_fe_out & (1<<3|1<<4)) {
                    ear_high = true;
                }
                if (ear_high) {
                    data |= (1<<6);
                }
                // keyboard matrix bits are encoded in the upper 8 bit of the port address
                uint16_t column_mask = (~(Z80_GET_ADDR(pins)>>8)) & 0x00FF;
                const uint16_t kbd_lines = kbd_test_lines(&sys->kbd, column_mask);
                data |= (~kbd_lines) & 0x1F;
                Z80_SET_DATA(pins, data);
            }
            else if (pins & Z80_WR) {
                // write to ULA
                // FIXME: bit 3: MIC output (CAS SAVE, 0=On, 1=Off)
                const uint8_t data = Z80_GET_DATA(pins);
                sys->border_color = data & 7;
                sys->last_fe_out = data;
                beeper_set(&sys->beeper, 0 != (data & (1<<4)));
            }
        }
        else if ((pins & Z80_WR) && (sys->type == ZX_TYPE_PLUS3) &&
                 (((Z80_GET_ADDR(pins) & 0xF002) == 0x1000) ||
                  ((Z80_GET_ADDR(pins) & 0xC002) == 0x4000))) {
            _zx_write_memory_control_plus3(
                sys,
                Z80_GET_ADDR(pins),
                Z80_GET_DATA(pins));
        }
        else if ((sys->type == ZX_TYPE_PLUS3) &&
                 ((Z80_GET_ADDR(pins) & 0xF002) == 0x2000) &&
                 (pins & Z80_RD)) {
            Z80_SET_DATA(pins, _zx_plus3_fdc_status(sys));
        }
        else if ((sys->type == ZX_TYPE_PLUS3) &&
                 ((Z80_GET_ADDR(pins) & 0xF002) == 0x3000)) {
            if (pins & Z80_RD) {
                Z80_SET_DATA(pins, _zx_plus3_fdc_read(sys));
            }
            else if (pins & Z80_WR) {
                _zx_plus3_fdc_write(sys, Z80_GET_DATA(pins));
            }
        }
        else if (((pins & (Z80_WR|Z80_A15|Z80_A1)) == Z80_WR) && (sys->type == ZX_TYPE_128)) {
            /* Spectrum 128 memory control (0.............0.)
                http://8bit.yarek.pl/computer/zx.128/
            */
            _zx_update_memory_map_zx128(sys, Z80_GET_DATA(pins));
        }
        else if (((pins & (Z80_A15|Z80_A1)) == Z80_A15) && (sys->type != ZX_TYPE_48K)) {
            // AY-3-8912 access (1*............0.)
            if (pins & Z80_A14) { pins |= AY38910_BC1; }
            if (pins & Z80_WR) { pins |= AY38910_BDIR; }
            pins = ay38910_iorq(&sys->ay, pins) & Z80_PIN_MASK;
        }
        else if ((pins & (Z80_RD|Z80_A7|Z80_A6|Z80_A5)) == Z80_RD) {
            // Kempston Joystick (........000.....)
            Z80_SET_DATA(pins, sys->kbd_joymask | sys->joy_joymask);
        }
    }

    // tick the AY at half frequency, use the buffered chip select
    // pin mask so that the AY doesn't miss any IO requests
    if (++sys->tick_count & 1) {
        ay38910_tick(&sys->ay);
    }

    // tick the beeper
    if (beeper_tick(&sys->beeper)) {
        // new sample ready (if this is not a ZX128, sys->ay.sample will be 0)
        const float sample = sys->beeper.sample + sys->ay.sample;
        sys->audio.sample_buffer[sys->audio.sample_pos++] = sample;
        if (sys->audio.sample_pos == sys->audio.num_samples) {
            if (sys->audio.callback.func) {
                sys->audio.callback.func(sys->audio.sample_buffer, sys->audio.num_samples, sys->audio.callback.user_data);
            }
            sys->audio.sample_pos = 0;
        }
    }
    if (new_frame) {
        sys->frame_tstate = 0;
    }
    else {
        sys->frame_tstate++;
    }
    return pins;
}

uint32_t zx_exec(zx_t* sys, uint32_t micro_seconds) {
    CHIPS_ASSERT(sys && sys->valid);
    const uint32_t num_ticks = clk_us_to_ticks(sys->freq_hz, micro_seconds);
    uint64_t pins = sys->pins;
    if (0 == sys->debug.callback.func) {
        // run without debug hook
        for (uint32_t tick = 0; tick < num_ticks; tick++) {
            if ((sys->cpu.step == 192) &&
                ((sys->cpu.pc == 0x056C) || (sys->cpu.pc == 0x0112)) &&
                (sys->tape_load_trap != NULL)) {
                if (sys->tape_load_trap(sys->tape_user_data, sys)) {
                    pins = z80_prefetch(&sys->cpu, sys->cpu.pc);
                }
            }
            pins = _zx_tick(sys, pins);
        }
    }
    else {
        // run with debug hook
        for (uint32_t tick = 0; (tick < num_ticks) && !(*sys->debug.stopped); tick++) {
            if ((sys->cpu.step == 192) &&
                ((sys->cpu.pc == 0x056C) || (sys->cpu.pc == 0x0112)) &&
                (sys->tape_load_trap != NULL)) {
                if (sys->tape_load_trap(sys->tape_user_data, sys)) {
                    pins = z80_prefetch(&sys->cpu, sys->cpu.pc);
                }
            }
            pins = _zx_tick(sys, pins);
            sys->debug.callback.func(sys->debug.callback.user_data, pins);
        }
    }
    sys->pins = pins;
    kbd_update(&sys->kbd, micro_seconds);
    return num_ticks;
}

void zx_key_down(zx_t* sys, int key_code) {
    CHIPS_ASSERT(sys && sys->valid);
    switch (sys->joystick_type) {
        case ZX_JOYSTICKTYPE_NONE:
            kbd_key_down(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_KEMPSTON:
            switch (key_code) {
                case 0x20:  sys->kbd_joymask |= ZX_JOYSTICK_BTN; break;
                case 0x08:  sys->kbd_joymask |= ZX_JOYSTICK_LEFT; break;
                case 0x09:  sys->kbd_joymask |= ZX_JOYSTICK_RIGHT; break;
                case 0x0A:  sys->kbd_joymask |= ZX_JOYSTICK_DOWN; break;
                case 0x0B:  sys->kbd_joymask |= ZX_JOYSTICK_UP; break;
                default:    kbd_key_down(&sys->kbd, key_code); break;
            }
            break;
        // the Sinclair joystick ports work as normal keys
        case ZX_JOYSTICKTYPE_SINCLAIR_1:
            switch (key_code) {
                case 0x20:  key_code = '5'; break;    // fire
                case 0x08:  key_code = '1'; break;    // left
                case 0x09:  key_code = '2'; break;    // right
                case 0x0A:  key_code = '3'; break;    // down
                case 0x0B:  key_code = '4'; break;    // up
                default: break;
            }
            kbd_key_down(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_SINCLAIR_2:
            switch (key_code) {
                case 0x20:  key_code = '0'; break;    // fire
                case 0x08:  key_code = '6'; break;    // left
                case 0x09:  key_code = '7'; break;    // right
                case 0x0A:  key_code = '8'; break;    // down
                case 0x0B:  key_code = '9'; break;    // up
                default: break;
            }
            kbd_key_down(&sys->kbd, key_code);
            break;
    }
}

void zx_key_up(zx_t* sys, int key_code) {
    CHIPS_ASSERT(sys && sys->valid);
    switch (sys->joystick_type) {
        case ZX_JOYSTICKTYPE_NONE:
            kbd_key_up(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_KEMPSTON:
            switch (key_code) {
                case 0x20:  sys->kbd_joymask &= ~ZX_JOYSTICK_BTN; break;
                case 0x08:  sys->kbd_joymask &= ~ZX_JOYSTICK_LEFT; break;
                case 0x09:  sys->kbd_joymask &= ~ZX_JOYSTICK_RIGHT; break;
                case 0x0A:  sys->kbd_joymask &= ~ZX_JOYSTICK_DOWN; break;
                case 0x0B:  sys->kbd_joymask &= ~ZX_JOYSTICK_UP; break;
                default:    kbd_key_up(&sys->kbd, key_code); break;
            }
            break;
        // the Sinclair joystick ports work as normal keys
        case ZX_JOYSTICKTYPE_SINCLAIR_1:
            switch (key_code) {
                case 0x20:  key_code = '5'; break;    // fire
                case 0x08:  key_code = '1'; break;    // left
                case 0x09:  key_code = '2'; break;    // right
                case 0x0A:  key_code = '3'; break;    // down
                case 0x0B:  key_code = '4'; break;    // up
                default: break;
            }
            kbd_key_up(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_SINCLAIR_2:
            switch (key_code) {
                case 0x20:  key_code = '0'; break;    // fire
                case 0x08:  key_code = '6'; break;    // left
                case 0x09:  key_code = '7'; break;    // right
                case 0x0A:  key_code = '8'; break;    // down
                case 0x0B:  key_code = '9'; break;    // up
                default: break;
            }
            kbd_key_up(&sys->kbd, key_code);
            break;
    }
}

void zx_set_joystick_type(zx_t* sys, zx_joystick_type_t type) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->joystick_type = type;
}

zx_joystick_type_t zx_joystick_type(zx_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    return sys->joystick_type;
}

void zx_joystick(zx_t* sys, uint8_t mask) {
    CHIPS_ASSERT(sys && sys->valid);
    if (sys->joystick_type == ZX_JOYSTICKTYPE_SINCLAIR_1) {
        if (mask & ZX_JOYSTICK_BTN)   { kbd_key_down(&sys->kbd, '5'); }
        else                          { kbd_key_up(&sys->kbd, '5'); }
        if (mask & ZX_JOYSTICK_LEFT)  { kbd_key_down(&sys->kbd, '1'); }
        else                          { kbd_key_up(&sys->kbd, '1'); }
        if (mask & ZX_JOYSTICK_RIGHT) { kbd_key_down(&sys->kbd, '2'); }
        else                          { kbd_key_up(&sys->kbd, '2'); }
        if (mask & ZX_JOYSTICK_DOWN)  { kbd_key_down(&sys->kbd, '3'); }
        else                          { kbd_key_up(&sys->kbd, '3'); }
        if (mask & ZX_JOYSTICK_UP)    { kbd_key_down(&sys->kbd, '4'); }
        else                          { kbd_key_up(&sys->kbd, '4'); }
    }
    else if (sys->joystick_type == ZX_JOYSTICKTYPE_SINCLAIR_2) {
        if (mask & ZX_JOYSTICK_BTN)   { kbd_key_down(&sys->kbd, '0'); }
        else                          { kbd_key_up(&sys->kbd, '0'); }
        if (mask & ZX_JOYSTICK_LEFT)  { kbd_key_down(&sys->kbd, '6'); }
        else                          { kbd_key_up(&sys->kbd, '6'); }
        if (mask & ZX_JOYSTICK_RIGHT) { kbd_key_down(&sys->kbd, '7'); }
        else                          { kbd_key_up(&sys->kbd, '7'); }
        if (mask & ZX_JOYSTICK_DOWN)  { kbd_key_down(&sys->kbd, '8'); }
        else                          { kbd_key_up(&sys->kbd, '8'); }
        if (mask & ZX_JOYSTICK_UP)    { kbd_key_down(&sys->kbd, '9'); }
        else                          { kbd_key_up(&sys->kbd, '9'); }
    }
    else {
        sys->joy_joymask = mask;
    }
}

void zx_set_tape_input(zx_t* sys, zx_tape_input_callback_t callback, void *user_data) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->tape_callback = callback;
    sys->tape_user_data = user_data;
}

void zx_set_tape_load_trap(zx_t* sys, zx_tape_load_trap_callback_t callback, void *user_data) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->tape_load_trap = callback;
    sys->tape_user_data = user_data;
}

void zx_set_disk_callbacks(
    zx_t* sys,
    zx_disk_ready_callback_t ready,
    zx_disk_read_sector_callback_t read_sector,
    zx_disk_write_sector_callback_t write_sector,
    zx_disk_sector_id_callback_t sector_id,
    void *user_data)
{
    CHIPS_ASSERT(sys && sys->valid);
    sys->disk_ready = ready;
    sys->disk_read_sector = read_sector;
    sys->disk_write_sector = write_sector;
    sys->disk_sector_id = sector_id;
    sys->disk_user_data = user_data;
}

void zx_notify_disk_changed(zx_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    memset(&sys->plus3_fdc, 0, sizeof(sys->plus3_fdc));
}

static void _zx_init_memory_map(zx_t* sys) {
    mem_init(&sys->mem);
    if (sys->type == ZX_TYPE_128 || sys->type == ZX_TYPE_PLUS3) {
        mem_map_ram(&sys->mem, 0, 0x4000, 0x4000, sys->ram[5]);
        mem_map_ram(&sys->mem, 0, 0x8000, 0x4000, sys->ram[2]);
        mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->ram[0]);
        mem_map_rom(&sys->mem, 0, 0x0000, 0x4000, sys->rom[0]);
    }
    else {
        mem_map_ram(&sys->mem, 0, 0x4000, 0x4000, sys->ram[0]);
        mem_map_ram(&sys->mem, 0, 0x8000, 0x4000, sys->ram[1]);
        mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->ram[2]);
        mem_map_rom(&sys->mem, 0, 0x0000, 0x4000, sys->rom[0]);
    }
}

static void _zx_init_keyboard_matrix(zx_t* sys) {
    // setup keyboard matrix
    kbd_init(&sys->kbd, 1);
    // caps-shift is column 0, line 0
    kbd_register_modifier(&sys->kbd, 0, 0, 0);
    // sym-shift is column 7, line 1
    kbd_register_modifier(&sys->kbd, 1, 7, 1);
    // alpha-numeric keys
    const char* keymap =
        /* no shift */
        " zxcv"         // A8       shift,z,x,c,v
        "asdfg"         // A9       a,s,d,f,g
        "qwert"         // A10      q,w,e,r,t
        "12345"         // A11      1,2,3,4,5
        "09876"         // A12      0,9,8,7,6
        "poiuy"         // A13      p,o,i,u,y
        " lkjh"         // A14      enter,l,k,j,h
        "  mnb"         // A15      space,symshift,m,n,b

        // shift
        " ZXCV"         // A8
        "ASDFG"         // A9
        "QWERT"         // A10
        "     "         // A11
        "     "         // A12
        "POIUY"         // A13
        " LKJH"         // A14
        "  MNB"         // A15

        // symshift
        " : ?/"         // A8
        "     "         // A9
        "   <>"         // A10
        "!@#$%"         // A11
        "_)('&"         // A12
        "\";   "        // A13
        " =+-^"         // A14
        "  .,*";        // A15
    for (int layer = 0; layer < 3; layer++) {
        for (int column = 0; column < 8; column++) {
            for (int line = 0; line < 5; line++) {
                const uint8_t c = keymap[layer*40 + column*5 + line];
                if (c != 0x20) {
                    kbd_register_key(&sys->kbd, c, column, line, (layer>0) ? (1<<(layer-1)) : 0);
                }
            }
        }
    }

    // special keys
    kbd_register_key(&sys->kbd, ' ', 7, 0, 0);  // Space
    kbd_register_key(&sys->kbd, 0x0F, 7, 1, 0); // SymShift
    kbd_register_key(&sys->kbd, 0x08, 3, 4, 1); // Cursor Left (Shift+5)
    kbd_register_key(&sys->kbd, 0x0A, 4, 4, 1); // Cursor Down (Shift+6)
    kbd_register_key(&sys->kbd, 0x0B, 4, 3, 1); // Cursor Up (Shift+7)
    kbd_register_key(&sys->kbd, 0x09, 4, 2, 1); // Cursor Right (Shift+8)
    kbd_register_key(&sys->kbd, 0x07, 3, 0, 1); // Edit (Shift+1)
    kbd_register_key(&sys->kbd, 0x0C, 4, 0, 1); // Delete (Shift+0)
    kbd_register_key(&sys->kbd, 0x0D, 6, 0, 0); // Enter
}

/*=== FILE LOADING ===========================================================*/

// ZX Z80 file format header (http://www.worldofspectrum.org/faq/reference/z80format.htm)
typedef struct {
    uint8_t A, F;
    uint8_t C, B;
    uint8_t L, H;
    uint8_t PC_l, PC_h;
    uint8_t SP_l, SP_h;
    uint8_t I, R;
    uint8_t flags0;
    uint8_t E, D;
    uint8_t C_, B_;
    uint8_t E_, D_;
    uint8_t L_, H_;
    uint8_t A_, F_;
    uint8_t IY_l, IY_h;
    uint8_t IX_l, IX_h;
    uint8_t EI;
    uint8_t IFF2;
    uint8_t flags1;
} _zx_z80_header;

typedef struct {
    uint8_t len_l;
    uint8_t len_h;
    uint8_t PC_l, PC_h;
    uint8_t hw_mode;
    uint8_t out_7ffd;
    uint8_t rom1;
    uint8_t flags;
    uint8_t out_fffd;
    uint8_t audio[16];
    uint8_t tlow_l;
    uint8_t tlow_h;
    uint8_t spectator_flags;
    uint8_t mgt_rom_paged;
    uint8_t multiface_rom_paged;
    uint8_t rom_0000_1fff;
    uint8_t rom_2000_3fff;
    uint8_t joy_mapping[10];
    uint8_t kbd_mapping[10];
    uint8_t mgt_type;
    uint8_t disciple_button_state;
    uint8_t disciple_flags;
    uint8_t out_1ffd;
} _zx_z80_ext_header;

typedef struct {
    uint8_t len_l;
    uint8_t len_h;
    uint8_t page_nr;
} _zx_z80_page_header;

static bool _zx_overflow(const uint8_t* ptr, intptr_t num_bytes, const uint8_t* end_ptr) {
    return (ptr + num_bytes) > end_ptr;
}

bool zx_quickload(zx_t* sys, chips_range_t data) {
    CHIPS_ASSERT(data.ptr && (data.size > 0));
    uint8_t* ptr = data.ptr;
    const uint8_t* end_ptr = ptr + data.size;
    if (_zx_overflow(ptr, sizeof(_zx_z80_header), end_ptr)) {
        return false;
    }
    const _zx_z80_header* hdr = (const _zx_z80_header*) ptr;
    ptr += sizeof(_zx_z80_header);
    const _zx_z80_ext_header* ext_hdr = 0;
    uint16_t pc = (hdr->PC_h<<8 | hdr->PC_l) & 0xFFFF;
    const bool is_version1 = 0 != pc;
    if (!is_version1) {
        if (_zx_overflow(ptr, sizeof(_zx_z80_ext_header), end_ptr)) {
            return false;
        }
        ext_hdr = (_zx_z80_ext_header*) ptr;
        int ext_hdr_len = (ext_hdr->len_h<<8)|ext_hdr->len_l;
        ptr += 2 + ext_hdr_len;
        if (ext_hdr->hw_mode < 3) {
            if (sys->type != ZX_TYPE_48K) {
                return false;
            }
        }
        else {
            if (sys->type != ZX_TYPE_128) {
                return false;
            }
        }
    }
    else {
        if (sys->type != ZX_TYPE_48K) {
            return false;
        }
    }
    const bool v1_compr = 0 != (hdr->flags0 & (1<<5));
    while (ptr < end_ptr) {
        int page_index = 0;
        int src_len = 0;
        if (is_version1) {
            src_len = data.size - sizeof(_zx_z80_header);
        }
        else {
            _zx_z80_page_header* phdr = (_zx_z80_page_header*) ptr;
            if (_zx_overflow(ptr, sizeof(_zx_z80_page_header), end_ptr)) {
                return false;
            }
            ptr += sizeof(_zx_z80_page_header);
            src_len = (phdr->len_h<<8 | phdr->len_l) & 0xFFFF;
            page_index = phdr->page_nr - 3;
            if ((sys->type == ZX_TYPE_48K) && (page_index == 5)) {
                page_index = 0;
            }
            if ((page_index < 0) || (page_index > 7)) {
                page_index = -1;
            }
        }
        uint8_t* dst_ptr;
        if (-1 == page_index) {
            dst_ptr = sys->junk;
        }
        else {
            dst_ptr = sys->ram[page_index];
        }
        if (0xFFFF == src_len) {
            // FIXME: uncompressed not supported yet
            return false;
        }
        else {
            // compressed
            int src_pos = 0;
            bool v1_done = false;
            uint8_t val[4];
            while ((src_pos < src_len) && !v1_done) {
                val[0] = ptr[src_pos];
                val[1] = ptr[src_pos+1];
                val[2] = ptr[src_pos+2];
                val[3] = ptr[src_pos+3];
                // check for version 1 end marker
                if (v1_compr && (0==val[0]) && (0xED==val[1]) && (0xED==val[2]) && (0==val[3])) {
                    v1_done = true;
                    src_pos += 4;
                }
                else if (0xED == val[0]) {
                    if (0xED == val[1]) {
                        uint8_t count = val[2];
                        CHIPS_ASSERT(0 != count);
                        src_pos += 4;
                        for (int i = 0; i < count; i++) {
                            *dst_ptr++ = val[3];
                        }
                    }
                    else {
                        // single ED
                        *dst_ptr++ = val[0];
                        src_pos++;
                    }
                }
                else {
                    // any value
                    *dst_ptr++ = val[0];
                    src_pos++;
                }
            }
            CHIPS_ASSERT(src_pos == src_len);
        }
        if (0xFFFF == src_len) {
            ptr += 0x4000;
        }
        else {
            ptr += src_len;
        }
    }

    // start loaded image
    z80_reset(&sys->cpu);
    sys->cpu.a = hdr->A; sys->cpu.f = hdr->F;
    sys->cpu.b = hdr->B; sys->cpu.c = hdr->C;
    sys->cpu.d = hdr->D; sys->cpu.e = hdr->E;
    sys->cpu.h = hdr->H; sys->cpu.l = hdr->L;
    sys->cpu.ix = (hdr->IX_h<<8)|hdr->IX_l;
    sys->cpu.iy = (hdr->IY_h<<8)|hdr->IY_l;
    sys->cpu.af2 = (hdr->A_<<8)|hdr->F_;
    sys->cpu.bc2 = (hdr->B_<<8)|hdr->C_;
    sys->cpu.de2 = (hdr->D_<<8)|hdr->E_;
    sys->cpu.hl2 = (hdr->H_<<8)|hdr->L_;
    sys->cpu.sp = (hdr->SP_h<<8)|hdr->SP_l;
    sys->cpu.i = hdr->I;
    sys->cpu.r = (hdr->R & 0x7F) | ((hdr->flags0 & 1)<<7);
    sys->cpu.iff2 = (hdr->IFF2 != 0);
    sys->cpu.iff1 = (hdr->EI != 0);
    if (hdr->flags1 != 0xFF) {
        sys->cpu.im = hdr->flags1 & 3;
    }
    else {
        sys->cpu.im = 1;
    }
    if (ext_hdr) {
        sys->pins = z80_prefetch(&sys->cpu, (ext_hdr->PC_h<<8)|ext_hdr->PC_l);
        if (sys->type != ZX_TYPE_48K) {
            ay38910_reset(&sys->ay);
            for (uint8_t i = 0; i < AY38910_NUM_REGISTERS; i++) {
                ay38910_set_register(&sys->ay, i, ext_hdr->audio[i]);
            }
            ay38910_set_addr_latch(&sys->ay, ext_hdr->out_fffd);
            _zx_update_memory_map_zx128(sys, ext_hdr->out_7ffd);
        }
    }
    else {
        sys->pins = z80_prefetch(&sys->cpu, (hdr->PC_h<<8)|hdr->PC_l);
    }
    sys->border_color = (hdr->flags0>>1) & 7;
    return true;
}

chips_display_info_t zx_display_info(zx_t* sys) {
    static const uint32_t palette[16] = {
        0xFF000000,     // std black
        0xFFD70000,     // std blue
        0xFF0000D7,     // std red
        0xFFD700D7,     // std magenta
        0xFF00D700,     // std green
        0xFFD7D700,     // std cyan
        0xFF00D7D7,     // std yellow
        0xFFD7D7D7,     // std white
        0xFF000000,     // bright black
        0xFFFF0000,     // bright blue
        0xFF0000FF,     // bright red
        0xFFFF00FF,     // bright magenta
        0xFF00FF00,     // bright green
        0xFFFFFF00,     // bright cyan
        0xFF00FFFF,     // bright yellow
        0xFFFFFFFF,     // bright white
    };
    const chips_display_info_t res = {
        .frame = {
            .dim = {
                .width = ZX_FRAMEBUFFER_WIDTH,
                .height = ZX_FRAMEBUFFER_HEIGHT,
            },
            .buffer = {
                .ptr = sys ? sys->fb : 0,
                .size = ZX_FRAMEBUFFER_SIZE_BYTES,
            },
            .bytes_per_pixel = 1,
        },
        .screen = {
            .x = 0,
            .y = 0,
            .width = ZX_DISPLAY_WIDTH,
            .height = ZX_DISPLAY_HEIGHT,
        },
        .palette = {
            .ptr = (void*)palette,
            .size = sizeof(palette),
        }
    };
    CHIPS_ASSERT(((sys == 0) && (res.frame.buffer.ptr == 0)) || ((sys != 0) && (res.frame.buffer.ptr != 0)));
    return res;
}

uint32_t zx_save_snapshot(zx_t* sys, zx_t* dst) {
    CHIPS_ASSERT(sys && dst);
    *dst = *sys;
    dst->tape_callback = 0;
    dst->tape_load_trap = 0;
    dst->tape_user_data = 0;
    dst->disk_ready = 0;
    dst->disk_read_sector = 0;
    dst->disk_write_sector = 0;
    dst->disk_sector_id = 0;
    dst->disk_user_data = 0;
    chips_debug_snapshot_onsave(&dst->debug);
    chips_audio_callback_snapshot_onsave(&dst->audio.callback);
    ay38910_snapshot_onsave(&dst->ay);
    mem_snapshot_onsave(&dst->mem, sys);
    return ZX_SNAPSHOT_VERSION;
}

bool zx_load_snapshot(zx_t* sys, uint32_t version, zx_t* src) {
    CHIPS_ASSERT(sys && src);
    if (version != ZX_SNAPSHOT_VERSION) {
        return false;
    }
    static zx_t im;
    im = *src;
    im.tape_callback = sys->tape_callback;
    im.tape_load_trap = sys->tape_load_trap;
    im.tape_user_data = sys->tape_user_data;
    im.disk_ready = sys->disk_ready;
    im.disk_read_sector = sys->disk_read_sector;
    im.disk_write_sector = sys->disk_write_sector;
    im.disk_sector_id = sys->disk_sector_id;
    im.disk_user_data = sys->disk_user_data;
    chips_debug_snapshot_onload(&im.debug, &sys->debug);
    chips_audio_callback_snapshot_onload(&im.audio.callback, &sys->audio.callback);
    ay38910_snapshot_onload(&im.ay, &sys->ay);
    mem_snapshot_onload(&im.mem, sys);
    *sys = im;
    return true;
}

#endif // CHIPS_IMPL
