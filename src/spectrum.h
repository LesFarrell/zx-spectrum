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
    SPECTRUM_MODEL_128K = 1
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
    int rom48_index;

    uint8_t rom[2][0x4000];
    bool rom_loaded[2];

    uint32_t framebuffer[ZX_SCREEN_WIDTH * ZX_SCREEN_HEIGHT];
} Spectrum;

/* Clears the wrapper state and records which machine model should be created
   once ROM data has been provided. */
void spectrum_init(Spectrum *spec, SpectrumModel model);

/* Loads ROM data into the local ROM buffers, validates the layout for 48K
   or 128K mode, and initializes the embedded emulator instance. */
bool spectrum_load_roms(
    Spectrum *spec,
    const char *rom_path_a,
    const char *rom_path_b,
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

/* Resets the embedded machine back to power-on state and refreshes the cached
   Win32 framebuffer from the emulator's display buffer. */
void spectrum_reset(Spectrum *spec);

/* Runs the emulator for approximately one 50 Hz frame and updates the cached
   32-bit framebuffer that the frontend blits to the window. */
void spectrum_run_frame(Spectrum *spec);

/* Copies the emulator's indexed framebuffer into the wrapper's 32-bit RGBA
   framebuffer using the palette provided by the chips display descriptor. */
void spectrum_render_frame(Spectrum *spec);

/* Sends a host key press into the embedded emulator using the chips keyboard
   mapping configured during machine initialization. */
void spectrum_key_down(Spectrum *spec, int key_code);

/* Sends a host key release into the embedded emulator using the chips keyboard
   mapping configured during machine initialization. */
void spectrum_key_up(Spectrum *spec, int key_code);

/* Updates the emulated joystick state mask presented on the Kempston port. */
void spectrum_set_joystick_mask(Spectrum *spec, uint8_t mask);

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

/* Reads and restores a 48K or 128K SNA snapshot file. */
bool spectrum_load_snapshot_sna(
    Spectrum *spec,
    const char *snapshot_path,
    char *error_buffer,
    size_t error_buffer_size
);

/* Detects a ZX-State container for an original 48K or 128K machine. */
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

/* Reads and restores a 48K or 128K ZX-State snapshot file. */
bool spectrum_load_snapshot_szx(
    Spectrum *spec,
    const char *snapshot_path,
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

/* Loads a `.z80` snapshot file into the currently wrapped machine, rebuilding
   the 48K or 128K backend first when the snapshot format requires a different
   model than the one that is currently active. */
bool spectrum_load_snapshot_z80(
    Spectrum *spec,
    const char *snapshot_path,
    char *error_buffer,
    size_t error_buffer_size
);

#endif
