#ifndef NEXT_MACHINE_H
#define NEXT_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/kbd.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "next_copper.h"
#include "next_dma.h"
#include "next_sd.h"
#include "next_sprite.h"
#include "next_rtc.h"

enum {
    NEXT_ADDRESS_SPACE_SIZE = 0x10000,
    NEXT_MMU_SLOT_SIZE = 0x2000,
    NEXT_MMU_SLOT_COUNT = 8,
    NEXT_RAM_PAGE_COUNT = 256,
    NEXT_RAM_SIZE = NEXT_RAM_PAGE_COUNT * NEXT_MMU_SLOT_SIZE,
    NEXT_ROM_SIZE = 0x10000,
    NEXT_BOOT_ROM_SIZE = 0x2000,
    NEXT_MULTIFACE_SIZE = 0x4000,
    NEXT_REG_COUNT = 256,
    NEXT_NEX_HEADER_SIZE = 512,
    NEXT_NEX_PALETTE_SIZE = 512,
    NEXT_NEX_BANK_SIZE = 0x4000,
    NEXT_NEX_BANK_COUNT = 112,
    NEXT_COPPER_SIZE = 2048,
    NEXT_DISPLAY_MAX_WIDTH = 640,
    NEXT_DISPLAY_HEIGHT = 256,
    NEXT_FRAME_TSTATES = 69888,
    NEXT_FRAME_LINES = 312,
    NEXT_LINE_TSTATES = 224,
    NEXT_COPPER_COLUMNS_PER_LINE = 448,
    NEXT_TILEMAP_MAX_WIDTH = 640,
    NEXT_AUDIO_MAX_SAMPLES = 1024
};

typedef enum NextNexVersion {
    NEXT_NEX_VERSION_1_0 = 0,
    NEXT_NEX_VERSION_1_1,
    NEXT_NEX_VERSION_1_2,
    NEXT_NEX_VERSION_1_3
} NextNexVersion;

typedef struct NextNexLoadResult {
    NextNexVersion version;
    uint16_t stack_pointer;
    uint16_t program_counter;
    uint16_t file_handle_address;
    uint8_t entry_bank;
    uint8_t border_color;
    uint8_t banks_loaded;
    bool preserve_nextregs;
    bool runnable;
    size_t appended_data_offset;
} NextNexLoadResult;

/* The Next is deliberately a separate machine from the classic Spectrum
   wrapper. Its 2 MB RAM, MMU, registers, ROMs and media lifetime do not alter
   zx_t or the snapshot layout used by the Sinclair models. */
typedef struct NextMachine {
    z80_t cpu;
    kbd_t keyboard;
    beeper_t beeper;
    ay38910_t ay[3];
    uint64_t cpu_pins;
    uint64_t ticks;
    uint64_t frame_count;
    uint32_t frame_tstate;
    int int_counter;
    uint64_t line_int_offset_tstates;
    bool line_int_pending;
    bool frame_int_disabled;
    int cpu_speed_multiplier;
    int audio_cpu_divider;
    int ay_clock_divider;
    chips_audio_callback_t audio_callback;
    /* Audio is delivered as interleaved left/right pairs; audio_num_samples
       counts stereo frames per callback. */
    int audio_num_samples;
    int audio_sample_pos;
    float audio_samples[NEXT_AUDIO_MAX_SAMPLES];
    float audio_ay_left;
    float audio_ay_right;
    int audio_ay_count;
    /* Current AY stereo level, refreshed when tone state or registers change. */
    float audio_ay_level_left;
    float audio_ay_level_right;
    bool audio_ay_dirty;
    float audio_dc_in[2];
    float audio_dc_out[2];
    uint8_t selected_ay;
    uint8_t ay_pan[3];
    uint8_t dac[4];
    NextDma dma;
    NextCopper copper_state;
    int copper_debt;
    NextSpriteEngine sprites;
    NextSdCard sd_card;
    NextRtc rtc;
    uint8_t *ram;
    uint8_t rom[NEXT_ROM_SIZE];
    uint8_t divmmc_rom[NEXT_BOOT_ROM_SIZE];
    uint8_t divmmc_ram[16 * NEXT_BOOT_ROM_SIZE];
    uint8_t alt_rom[2][NEXT_NEX_BANK_SIZE];
    /* 8K Multiface ROM followed by its 8K RAM (SRAM config page 5). */
    uint8_t multiface_mem[NEXT_MULTIFACE_SIZE];
    uint8_t loader_rom[NEXT_BOOT_ROM_SIZE];
    bool rom_loaded;
    bool divmmc_rom_loaded;
    bool loader_rom_loaded;
    bool loader_active;
    bool config_mode;
    uint8_t config_page;
    bool divmmc_automap;
    bool divmmc_automap_pending;
    uint8_t divmmc_control;
    bool multiface_paged;
    bool multiface_nmi_active;
    bool multiface_invisible;
    bool nmi_request;
    bool last_m1_ed_prefix;
    bool nmi_stackless_retn;
    uint8_t nmi_retn_reads;
    uint8_t nmi_write_count;
    uint16_t nmi_write_address[2];
    uint8_t nmi_write_old_value[2];

    uint8_t mmu[NEXT_MMU_SLOT_COUNT];
    uint8_t nextregs[NEXT_REG_COUNT];
    uint8_t selected_nextreg;
    uint8_t reset_type;
    uint8_t rom_bank;
    uint8_t port_7ffd;
    uint8_t port_1ffd;
    uint8_t port_dffd;
    uint8_t timex_video_mode;
    uint8_t mmu_override_mask;
    bool paging_locked;
    uint8_t layer2_control;
    uint8_t layer2_map_control;
    uint8_t layer2_map_offset;
    uint8_t border_color;
    uint8_t joystick_mask;
    uint8_t palette[NEXT_NEX_PALETTE_SIZE];
    uint16_t hw_palette[8][256];
    uint8_t hw_palette_priority[8][256];
    uint8_t palette_index;
    uint8_t palette_latch;
    bool palette_second_write;
    uint8_t copper[NEXT_COPPER_SIZE];
    bool palette_loaded;
    bool copper_loaded;
    uint8_t sprite_clip[4];
    uint8_t sprite_clip_index;
    uint8_t layer2_clip[4];
    uint8_t layer2_clip_index;
    uint8_t ula_clip[4];
    uint8_t ula_clip_index;
    int tilemap_scroll_x;
    int tilemap_scroll_y;
    uint8_t tilemap_clip[4];
    uint8_t tilemap_clip_index;

    /* Optional per-tick debugger hook, matching the classic core's. When
       *stopped becomes true the exec functions return early. */
    chips_debug_t debug;
    uint16_t initial_pc;
    uint16_t initial_sp;
    uint32_t *framebuffer;
    int framebuffer_width;
    bool initialized;
} NextMachine;

bool next_machine_init(
    NextMachine *machine,
    char *error_buffer,
    size_t error_buffer_size
);
bool next_machine_load_loader_rom_data(
    NextMachine *machine,
    const uint8_t *loader_rom,
    size_t loader_rom_size,
    char *error_buffer,
    size_t error_buffer_size
);
void next_machine_discard(NextMachine *machine);
void next_machine_reset(NextMachine *machine);
void next_machine_configure_audio(
    NextMachine *machine,
    chips_audio_callback_t callback,
    int sample_rate,
    int num_samples,
    float beeper_volume,
    float ay_volume
);

bool next_machine_load_rom_data(
    NextMachine *machine,
    const uint8_t *rom,
    size_t rom_size,
    const uint8_t *boot_rom,
    size_t boot_rom_size,
    char *error_buffer,
    size_t error_buffer_size
);
bool next_machine_open_sd_image(
    NextMachine *machine,
    const char *path,
    bool writable,
    char *error_buffer,
    size_t error_buffer_size
);

uint8_t next_machine_read_memory(const NextMachine *machine, uint16_t address);
void next_machine_write_memory(NextMachine *machine, uint16_t address, uint8_t value);
uint8_t next_machine_read_nextreg(const NextMachine *machine, uint8_t reg);
void next_machine_write_nextreg(NextMachine *machine, uint8_t reg, uint8_t value);
uint8_t next_machine_read_port(NextMachine *machine, uint16_t port);
void next_machine_write_port(NextMachine *machine, uint16_t port, uint8_t value);
uint64_t next_machine_tick(NextMachine *machine);
void next_machine_exec_ticks(NextMachine *machine, uint32_t ticks);
void next_machine_exec_frame(NextMachine *machine);
void next_machine_render_frame(NextMachine *machine);
void next_machine_key_down(NextMachine *machine, int key_code);
void next_machine_key_up(NextMachine *machine, int key_code);
void next_machine_set_joystick_mask(NextMachine *machine, uint8_t mask);

bool next_machine_load_nex_data(
    NextMachine *machine,
    const uint8_t *data,
    size_t data_size,
    NextNexLoadResult *result,
    char *error_buffer,
    size_t error_buffer_size
);

bool next_machine_load_snx_data(
    NextMachine *machine,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size
);

/* Loads a NEX the way the reference harness does: the banks are copied
   straight into the memory pool, SP/PC come from the header and the entry
   bank is paged at 0xC000. No reset, palette, Copper or loading screen is
   applied, so the machine keeps the state it already had. */
bool next_machine_load_nex_direct(
    NextMachine *machine,
    const uint8_t *data,
    size_t data_size,
    NextNexLoadResult *result,
    char *error_buffer,
    size_t error_buffer_size
);

#endif
