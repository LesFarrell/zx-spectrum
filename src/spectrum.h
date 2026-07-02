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
    int rom48_index;

    uint8_t rom[2][0x4000];
    bool rom_loaded[2];

    uint32_t framebuffer[ZX_SCREEN_WIDTH * ZX_SCREEN_HEIGHT];
} Spectrum;

/* Clears the wrapper state and records which machine model should be created
   once ROM data has been provided. */
void spectrum_init(Spectrum *spec, SpectrumModel model);

/* Loads one or two ROM files into the local ROM buffers, validates the layout
   for 48K or 128K mode, and initializes the embedded emulator instance. */
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

/* Rebuilds the wrapped machine for the requested model using the ROM data that
   has already been loaded, then resets into the new power-on state. */
bool spectrum_set_model(
    Spectrum *spec,
    SpectrumModel model,
    char *error_buffer,
    size_t error_buffer_size
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
