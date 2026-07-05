#define CHIPS_IMPL
#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/mem.h"
#include "chips/kbd.h"
#include "chips/clk.h"
#include "systems/zx.h"

#include "spectrum.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum {
    ZX_HOST_KEY_CAPS_SHIFT = 1,
    ZX_HOST_KEY_SYMBOL_SHIFT = 2,
    ZX_48K_FRAME_US = 19968,
    ZX_128K_FRAME_US = 19992
};

/* Reads a ROM file into the provided buffer, rejects files larger than the
   expected maximum size, and pads short reads with 0xFF bytes. */
static bool spectrum_load_file_exact(const char *path, uint8_t *buffer, size_t max_size, size_t *actual_size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }
    rewind(file);

    if ((size_t)size > max_size) {
        fclose(file);
        return false;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        return false;
    }
    fclose(file);

    if ((size_t)size < max_size) {
        memset(buffer + size, 0xFF, max_size - (size_t)size);
    }
    if (actual_size != NULL) {
        *actual_size = (size_t)size;
    }
    return true;
}

/* Reads an entire file into a freshly allocated heap buffer so container
   formats such as `.z80` snapshots can be passed intact to the chips loader. */
static bool spectrum_load_file_all(const char *path, uint8_t **buffer, size_t *size) {
    FILE *file = fopen(path, "rb");
    uint8_t *data = NULL;

    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    long file_size = ftell(file);
    if (file_size <= 0) {
        fclose(file);
        return false;
    }
    rewind(file);

    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        return false;
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        return false;
    }
    fclose(file);

    *buffer = data;
    *size = (size_t)file_size;
    return true;
}

/* Extracts the target machine model from a `.z80` snapshot header so the
   wrapper can rebuild the emulator as 48K or 128K before loading the state. */
static bool spectrum_detect_snapshot_model(
    const uint8_t *data,
    size_t size,
    SpectrumModel *model
) {
    if (size < 30) {
        return false;
    }

    uint16_t pc = (uint16_t)(data[6] | (data[7] << 8));
    if (pc != 0) {
        *model = SPECTRUM_MODEL_48K;
        return true;
    }

    if (size < 35) {
        return false;
    }

    if (data[34] < 3) {
        *model = SPECTRUM_MODEL_48K;
    } else {
        if (data[34] == 7 || data[34] == 8 || data[34] == 13) {
            return false;
        }
        *model = SPECTRUM_MODEL_128K;
    }
    return true;
}


/* Builds the chips ZX Spectrum machine from the already loaded ROM buffers and
   records the display descriptor used later for framebuffer conversion. */
static void spectrum_init_machine(Spectrum *spec) {
    zx_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (spec->model == SPECTRUM_MODEL_128K) {
        desc.type = ZX_TYPE_128;
    } else {
        desc.type = ZX_TYPE_48K;
    }
    desc.joystick_type = ZX_JOYSTICKTYPE_NONE;
    desc.audio.callback = spec->audio_callback;
    desc.audio.num_samples = spec->audio_num_samples;
    desc.audio.sample_rate = spec->audio_sample_rate;
    desc.audio.beeper_volume = spec->beeper_volume;
    desc.audio.ay_volume = spec->ay_volume;
    if (spec->model == SPECTRUM_MODEL_128K) {
        desc.roms.zx128_0.ptr = spec->rom[0];
        desc.roms.zx128_0.size = sizeof(spec->rom[0]);
        desc.roms.zx128_1.ptr = spec->rom[1];
        desc.roms.zx128_1.size = sizeof(spec->rom[0]);
    } else {
        desc.roms.zx48k.ptr = spec->rom[spec->rom48_index];
        desc.roms.zx48k.size = sizeof(spec->rom[spec->rom48_index]);
    }
    zx_init(&spec->machine, &desc);
    spec->display = zx_display_info(&spec->machine);

    /* Add direct bindings for the two Spectrum modifier keys so the frontend
       can expose them as discrete keys in addition to the auto-shift layers. */
    kbd_register_key(&spec->machine.kbd, ZX_HOST_KEY_CAPS_SHIFT, 0, 0, 0);
    kbd_register_key(&spec->machine.kbd, ZX_HOST_KEY_SYMBOL_SHIFT, 7, 1, 0);

    spec->machine_ready = true;
}

/* Recreates the wrapped machine using the already loaded ROM buffers after the
   requested model changes, for example when loading a snapshot from another
   Spectrum variant. */
static bool spectrum_rebuild_for_model(
    Spectrum *spec,
    SpectrumModel model,
    char *error_buffer,
    size_t error_buffer_size
) {
    if (!spec->rom_loaded[0]) {
        snprintf(error_buffer, error_buffer_size, "No base ROM is loaded.");
        return false;
    }
    if (model == SPECTRUM_MODEL_128K && !spec->rom_loaded[1]) {
        snprintf(
            error_buffer,
            error_buffer_size,
            "This machine needs 128K ROMs, but only a 48K ROM is available."
        );
        return false;
    }

    spec->model = model;
    spec->machine_ready = false;
    spectrum_init_machine(spec);
    return true;
}

/* Resets the wrapper to a known empty state and records which machine type the
   caller wants to create once ROM data becomes available. */
void spectrum_init(Spectrum *spec, SpectrumModel model) {
    memset(spec, 0, sizeof(*spec));
    spec->model = model;
    spec->audio_sample_rate = 44100;
    spec->beeper_volume = 0.35f;
    spec->ay_volume = 0.20f;
    spec->rom48_index = 0;
}

/* Records the audio callback configuration used whenever the wrapped machine
   is created or rebuilt, allowing the frontend to own the actual device I/O. */
void spectrum_configure_audio(
    Spectrum *spec,
    chips_audio_callback_t callback,
    int sample_rate,
    int num_samples,
    float beeper_volume,
    float ay_volume
) {
    spec->audio_callback = callback;
    spec->audio_sample_rate = sample_rate;
    spec->audio_num_samples = num_samples;
    spec->beeper_volume = beeper_volume;
    spec->ay_volume = ay_volume;
}

/* Loads ROM data from disk, validates whether it matches the requested 48K
   or 128K layout, then creates and resets the embedded chips-based machine. */
bool spectrum_load_roms(
    Spectrum *spec,
    const char *rom_path_a,
    const char *rom_path_b,
    char *error_buffer,
    size_t error_buffer_size
) {
    size_t file_size = 0;
    uint8_t combined[0x8000];
    memset(spec->rom_loaded, 0, sizeof(spec->rom_loaded));
    spec->rom48_index = 0;

    {
        if (!spectrum_load_file_exact(rom_path_a, combined, sizeof(combined), &file_size)) {
            snprintf(error_buffer, error_buffer_size, "Could not load ROM file: %s", rom_path_a);
            return false;
        }

        if (file_size == 0x4000) {
            memcpy(spec->rom[0], combined, 0x4000);
            spec->rom_loaded[0] = true;
        } else if (file_size == 0x8000) {
            memcpy(spec->rom[0], combined, 0x4000);
            memcpy(spec->rom[1], combined + 0x4000, 0x4000);
            spec->rom_loaded[0] = true;
            spec->rom_loaded[1] = true;
            spec->rom48_index = 1;
        } else {
            snprintf(error_buffer, error_buffer_size, "ROM file must be 16 KB or 32 KB: %s", rom_path_a);
            return false;
        }
    }

    if (rom_path_b != NULL) {
        size_t second_size = 0;
        if (!spectrum_load_file_exact(rom_path_b, spec->rom[1], sizeof(spec->rom[1]), &second_size)) {
            snprintf(error_buffer, error_buffer_size, "Could not load ROM file: %s", rom_path_b);
            return false;
        }
        if (second_size != 0x4000) {
            snprintf(error_buffer, error_buffer_size, "Second ROM must be exactly 16 KB: %s", rom_path_b);
            return false;
        }
        spec->rom_loaded[1] = true;
        spec->rom48_index = 1;
    }

    if (spec->model == SPECTRUM_MODEL_128K && !spec->rom_loaded[1]) {
        snprintf(
            error_buffer,
            error_buffer_size,
            "128K mode needs either a combined 32 KB ROM or two 16 KB ROMs."
        );
        return false;
    }

    spectrum_init_machine(spec);
    spectrum_reset(spec);
    return true;
}

/* Resets the embedded emulator if it has already been created and synchronizes
   the wrapper framebuffer with the machine's freshly reset display state. */
void spectrum_reset(Spectrum *spec) {
    if (!spec->machine_ready) {
        return;
    }
    zx_reset(&spec->machine);
    spectrum_render_frame(spec);
}

/* Converts the emulator's indexed display output into the 32-bit framebuffer
   consumed by the Win32 frontend for painting the current frame. */
void spectrum_render_frame(Spectrum *spec) {
    if (!spec->machine_ready) {
        memset(spec->framebuffer, 0, sizeof(spec->framebuffer));
        return;
    }

    const uint8_t *src = (const uint8_t *)spec->display.frame.buffer.ptr;
    const uint32_t *palette = (const uint32_t *)spec->display.palette.ptr;
    const int pitch = spec->display.frame.dim.width;

    for (int y = 0; y < ZX_SCREEN_HEIGHT; ++y) {
        const uint8_t *src_row = src + y * pitch;
        uint32_t *dst_row = &spec->framebuffer[y * ZX_SCREEN_WIDTH];
        for (int x = 0; x < ZX_SCREEN_WIDTH; ++x) {
            uint32_t color = palette[src_row[x] & 0x0F];
            dst_row[x] =
                ((color & 0x000000FFu) << 16) |
                (color & 0x0000FF00u) |
                ((color & 0x00FF0000u) >> 16);
        }
    }
}

/* Advances the machine by about one 50 Hz frame and then refreshes the cached
   frontend framebuffer from the emulator's current display buffer. */
void spectrum_run_frame(Spectrum *spec) {
    uint32_t frame_us;

    if (!spec->machine_ready) {
        return;
    }
    if (spec->model == SPECTRUM_MODEL_48K) {
        frame_us = ZX_48K_FRAME_US;
    } else {
        frame_us = ZX_128K_FRAME_US;
    }
    zx_exec(&spec->machine, frame_us);
    spectrum_render_frame(spec);
}

/* Delivers a host key press to the embedded chips keyboard handler when the
   machine is initialized and ready to receive input. */
void spectrum_key_down(Spectrum *spec, int key_code) {
    if (!spec->machine_ready) {
        return;
    }
    zx_key_down(&spec->machine, key_code);
}

/* Delivers a host key release to the embedded chips keyboard handler when the
   machine is initialized and ready to receive input. */
void spectrum_key_up(Spectrum *spec, int key_code) {
    if (!spec->machine_ready) {
        return;
    }
    zx_key_up(&spec->machine, key_code);
}

/* Updates the emulated joystick lines so frontend controller input can feed
   the Kempston port without interfering with keyboard mappings. */
void spectrum_set_joystick_mask(Spectrum *spec, uint8_t mask) {
    if (!spec->machine_ready) {
        return;
    }
    zx_joystick(&spec->machine, mask);
}

/* Loads a `.z80` snapshot file, switches the backend model when necessary,
   then asks the chips ZX implementation to restore the machine state. */
bool spectrum_load_snapshot_z80(
    Spectrum *spec,
    const char *snapshot_path,
    char *error_buffer,
    size_t error_buffer_size
) {
    uint8_t *data = NULL;
    size_t data_size = 0;
    SpectrumModel snapshot_model;
    bool ok = false;

    if (!spectrum_load_file_all(snapshot_path, &data, &data_size)) {
        snprintf(error_buffer, error_buffer_size, "Could not read snapshot file: %s", snapshot_path);
        return false;
    }

    if (!spectrum_detect_snapshot_model(data, data_size, &snapshot_model)) {
        snprintf(error_buffer, error_buffer_size, "Unsupported or corrupt .z80 snapshot: %s", snapshot_path);
        goto cleanup;
    }

    if (snapshot_model != spec->model) {
        if (!spectrum_rebuild_for_model(spec, snapshot_model, error_buffer, error_buffer_size)) {
            goto cleanup;
        }
    }

    if (!zx_quickload(&spec->machine, (chips_range_t){ .ptr = data, .size = data_size })) {
        snprintf(
            error_buffer,
            error_buffer_size,
            "Could not load .z80 snapshot into the current machine: %s",
            snapshot_path
        );
        goto cleanup;
    }

    spectrum_render_frame(spec);
    ok = true;

cleanup:
    free(data);
    return ok;
}
