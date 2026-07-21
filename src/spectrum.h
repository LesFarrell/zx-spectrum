#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/mem.h"
#include "chips/kbd.h"
#include "chips/clk.h"
#include "systems/zx.h"

enum {
    ZX_SCREEN_WIDTH = ZX_DISPLAY_WIDTH,
    ZX_SCREEN_HEIGHT = ZX_DISPLAY_HEIGHT
};

typedef enum SpectrumModel {
    SPECTRUM_MODEL_48K = 0,
    SPECTRUM_MODEL_128K = 1,
    SPECTRUM_MODEL_PLUS2 = 2,
    SPECTRUM_MODEL_PLUS2A = 3,
    SPECTRUM_MODEL_PLUS3 = 4
} SpectrumModel;

/* Holds the selected machine model, loaded ROM images, the embedded chips
   ZX Spectrum instance, and the converted 32-bit framebuffer used by Win32. */
typedef struct Spectrum {
    SpectrumModel model;
    zx_t machine;
    chips_display_info_t display;
    bool machine_ready;
    chips_audio_callback_t audio_callback;
    int audio_num_samples;
    int audio_sample_rate;
    float beeper_volume;
    float ay_volume;
    zx_tape_input_callback_t tape_callback;
    zx_tape_load_trap_callback_t tape_load_trap;
    void *tape_user_data;
    zx_disk_ready_callback_t disk_ready;
    zx_disk_read_sector_callback_t disk_read_sector;
    zx_disk_write_sector_callback_t disk_write_sector;
    zx_disk_sector_id_callback_t disk_sector_id;
    void *disk_user_data;
    zx_microdrive_ready_callback_t microdrive_ready;
    zx_microdrive_write_protected_callback_t microdrive_write_protected;
    zx_microdrive_length_callback_t microdrive_length;
    zx_microdrive_read_callback_t microdrive_read;
    zx_microdrive_write_callback_t microdrive_write;
    void *microdrive_user_data;
    zx_joystick_type_t joystick_type;
    bool kempston_mouse_enabled;
    bool fuller_audio_enabled;
    bool specdrum_enabled;
    bool covox_enabled;
    bool multiface_enabled;
    uint8_t mouse_x;
    uint8_t mouse_y;
    uint8_t mouse_buttons;
    int rom48_index;

    uint8_t rom[4][0x4000];
    bool rom_loaded[4];
    uint8_t interface1_rom[ZX_INTERFACE1_ROM_SIZE];
    bool interface1_rom_loaded;
    uint8_t multiface_rom[ZX_MULTIFACE_ROM_SIZE];
    bool multiface_rom_loaded;

    uint32_t framebuffer[ZX_SCREEN_WIDTH * ZX_SCREEN_HEIGHT];
} Spectrum;

/* Clears the wrapper state and records which machine model should be created
   once ROM data has been provided. */
void spectrum_init(Spectrum *spec, SpectrumModel model);

/* Loads ROM data into the local ROM buffers, validates the layout for 48K,
   128K, or +3 mode, and initializes the embedded emulator instance. */
bool spectrum_load_roms(
    Spectrum *spec,
    const char *rom_path_a,
    const char *rom_path_b,
    char *error_buffer,
    size_t error_buffer_size
);

/* Loads the optional 8 KB Interface 1 shadow ROM used by 48K and 128K
   machines. The peripheral remains disabled if no image is configured. */
bool spectrum_load_interface1_rom(
    Spectrum *spec,
    const char *rom_path,
    char *error_buffer,
    size_t error_buffer_size
);

bool spectrum_load_multiface_rom(
    Spectrum *spec,
    const char *rom_path,
    char *error_buffer,
    size_t error_buffer_size
);

/* Configures the audio callback and output parameters that should be wired
   into the next machine initialization or model rebuild. */
void spectrum_configure_audio(
    Spectrum *spec,
    chips_audio_callback_t callback,
    int sample_rate,
    int num_samples,
    float beeper_volume,
    float ay_volume
);

/* Wires an application-owned EAR/tape input source into ULA port reads. */
void spectrum_configure_tape_input(
    Spectrum *spec,
    zx_tape_input_callback_t callback,
    void *user_data
);

/* Wires an optional ROM tape-loader fast-trap callback into the machine. */
void spectrum_configure_tape_load_trap(
    Spectrum *spec,
    zx_tape_load_trap_callback_t callback,
    void *user_data
);

/* Wires application-owned +3 disk media into the uPD765 controller. */
void spectrum_configure_disk(
    Spectrum *spec,
    zx_disk_ready_callback_t ready,
    zx_disk_read_sector_callback_t read_sector,
    zx_disk_write_sector_callback_t write_sector,
    zx_disk_sector_id_callback_t sector_id,
    void *user_data
);

/* Wires the two emulated Microdrive cartridge slots into Interface 1. */
void spectrum_configure_interface1(
    Spectrum *spec,
    zx_microdrive_ready_callback_t ready,
    zx_microdrive_write_protected_callback_t write_protected,
    zx_microdrive_length_callback_t length,
    zx_microdrive_read_callback_t read,
    zx_microdrive_write_callback_t write,
    void *user_data
);

/* Resets any active +3 controller command after media insertion/ejection. */
void spectrum_notify_disk_changed(Spectrum *spec);

/* Resets the embedded machine back to power-on state and refreshes the cached
   Win32 framebuffer from the emulator's display buffer. */
void spectrum_reset(Spectrum *spec);

/* Copies the emulator's indexed framebuffer into the wrapper's 32-bit RGBA
   framebuffer using the palette provided by the chips display descriptor. */
void spectrum_render_frame(Spectrum *spec);

/* Sends a host key press into the embedded emulator using the chips keyboard
   mapping configured during machine initialization. */
void spectrum_key_down(Spectrum *spec, int key_code);

/* Sends a host key release into the embedded emulator using the chips keyboard
   mapping configured during machine initialization. */
void spectrum_key_up(Spectrum *spec, int key_code);

/* Selects the emulated joystick interface used by host controls. */
void spectrum_set_joystick_type(Spectrum *spec, zx_joystick_type_t type);

/* Updates the emulated joystick state mask presented through that interface. */
void spectrum_set_joystick_mask(Spectrum *spec, uint8_t mask);

void spectrum_set_kempston_mouse_enabled(Spectrum *spec, bool enabled);
void spectrum_set_mouse(Spectrum *spec, uint8_t x, uint8_t y, uint8_t buttons);
void spectrum_set_expansion_audio(
    Spectrum *spec,
    bool fuller_audio,
    bool specdrum,
    bool covox
);
void spectrum_set_multiface_enabled(Spectrum *spec, bool enabled);
void spectrum_multiface_nmi(Spectrum *spec);

/* Loads or saves the 6912-byte bitmap/attribute image of the visible screen. */
bool spectrum_load_screen_scr_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size
);
bool spectrum_save_screen_scr_file(
    const Spectrum *spec,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
);

/* Saves the current 48K, 128K, or +2 state as a portable SNA file. */
bool spectrum_save_snapshot_sna_file(
    Spectrum *spec,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
);

/* Saves any supported machine as an uncompressed version-3 Z80 snapshot. */
bool spectrum_save_snapshot_z80_file(
    Spectrum *spec,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
);

/* Captures and restores an in-process core state for bounded rewind history. */
bool spectrum_save_runtime_state(
    Spectrum *spec,
    zx_t *state,
    uint32_t *version
);

bool spectrum_load_runtime_state(
    Spectrum *spec,
    SpectrumModel model,
    zx_t *state,
    uint32_t version
);

/* Detects the target machine model encoded by a `.z80` snapshot payload that
   has already been read into memory. */
bool spectrum_detect_snapshot_model_data(
    const uint8_t *data,
    size_t size,
    SpectrumModel *model
);

/* Detects whether an SNA payload is the 48K or extended 128K layout. */
bool spectrum_detect_snapshot_sna_model_data(
    const uint8_t *data,
    size_t size,
    SpectrumModel *model
);

/* Restores a 48K or 128K SNA payload, rebuilding the machine when needed. */
bool spectrum_load_snapshot_sna_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size
);

/* Detects a ZX-State container for any supported Spectrum model. */
bool spectrum_detect_snapshot_szx_model_data(
    const uint8_t *data,
    size_t size,
    SpectrumModel *model
);

/* Restores the core CPU, RAM, ULA, paging, and AY state from an SZX payload. */
bool spectrum_load_snapshot_szx_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size
);

/* Loads a `.z80` snapshot payload that is already resident in memory into the
   wrapped machine, rebuilding to the encoded model when necessary. */
bool spectrum_load_snapshot_z80_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size
);

#endif
