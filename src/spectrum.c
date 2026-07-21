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
#include "szx_inflate.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

enum
{
    ZX_HOST_KEY_CAPS_SHIFT = 1,
    ZX_HOST_KEY_SYMBOL_SHIFT = 2,
    SNA_HEADER_SIZE = 27,
    SNA_RAM_BANK_SIZE = 0x4000,
    SNA_48K_SIZE = SNA_HEADER_SIZE + (3 * SNA_RAM_BANK_SIZE),
    SNA_128K_BASE_SIZE = SNA_48K_SIZE + 4,
    SNA_128K_SIZE = SNA_128K_BASE_SIZE + (5 * SNA_RAM_BANK_SIZE),
    SNA_128K_DUPLICATE_BANK_SIZE = SNA_128K_BASE_SIZE + (6 * SNA_RAM_BANK_SIZE),
    Z80_V3_HEADER_SIZE = 87,
    Z80_PAGE_HEADER_SIZE = 3,
    SCR_SCREEN_SIZE = 6912
};

static uint16_t spectrum_read_u16(const uint8_t *data)
{
    return (uint16_t)(data[0] | (data[1] << 8));
}

static void spectrum_write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static uint32_t spectrum_read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static bool spectrum_snapshot_model_compatible(
    SpectrumModel snapshot_model,
    SpectrumModel current_model)
{
    return snapshot_model == current_model ||
        (snapshot_model == SPECTRUM_MODEL_128K &&
         current_model == SPECTRUM_MODEL_PLUS2);
}

/* Reads a ROM file into the provided buffer, rejects files larger than the
   expected maximum size, and pads short reads with 0xFF bytes. */
static bool spectrum_load_file_exact(const char *path, uint8_t *buffer, size_t max_size, size_t *actual_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0)
    {
        fclose(file);
        return false;
    }
    rewind(file);

    if ((size_t)size > max_size)
    {
        fclose(file);
        return false;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size)
    {
        fclose(file);
        return false;
    }
    fclose(file);

    if ((size_t)size < max_size)
    {
        memset(buffer + size, 0xFF, max_size - (size_t)size);
    }
    if (actual_size != NULL)
    {
        *actual_size = (size_t)size;
    }
    return true;
}

/* Extracts the target machine model from a `.z80` snapshot header so the
   wrapper can rebuild the emulator as 48K or 128K before loading the state. */
bool spectrum_detect_snapshot_model_data(
    const uint8_t *data,
    size_t size,
    SpectrumModel *model)
{
    uint16_t additional_length;
    uint8_t hardware_mode;
    uint8_t flags;

    if (data == NULL || model == NULL || size < 30)
    {
        return false;
    }

    uint16_t pc = (uint16_t)(data[6] | (data[7] << 8));
    if (pc != 0)
    {
        *model = SPECTRUM_MODEL_48K;
        return true;
    }

    if (size < 32)
    {
        return false;
    }
    additional_length = spectrum_read_u16(&data[30]);
    if (additional_length < 23 || additional_length > size - 32)
    {
        return false;
    }
    hardware_mode = data[34];
    flags = data[37];

    if (hardware_mode == 7 || hardware_mode == 8)
        *model = SPECTRUM_MODEL_PLUS3;
    else if (hardware_mode == 12)
        *model = SPECTRUM_MODEL_PLUS2;
    else if (hardware_mode == 13)
        *model = SPECTRUM_MODEL_PLUS2A;
    else if (additional_length == 23)
    {
        if (hardware_mode <= 1) *model = SPECTRUM_MODEL_48K;
        else if (hardware_mode == 3 || hardware_mode == 4)
            *model = SPECTRUM_MODEL_128K;
        else return false;
    }
    else if (additional_length >= 54)
    {
        if (hardware_mode == 0 || hardware_mode == 1 || hardware_mode == 3)
            *model = SPECTRUM_MODEL_48K;
        else if (hardware_mode >= 4 && hardware_mode <= 6)
            *model = SPECTRUM_MODEL_128K;
        else return false;
    }
    else return false;

    if (flags & 0x80)
    {
        if (*model == SPECTRUM_MODEL_128K) *model = SPECTRUM_MODEL_PLUS2;
        else if (*model == SPECTRUM_MODEL_PLUS3) *model = SPECTRUM_MODEL_PLUS2A;
    }
    return true;
}

bool spectrum_detect_snapshot_sna_model_data(
    const uint8_t *data,
    size_t size,
    SpectrumModel *model)
{
    if (data == NULL || model == NULL)
    {
        return false;
    }
    if (size == SNA_48K_SIZE)
    {
        *model = SPECTRUM_MODEL_48K;
        return true;
    }
    if (size == SNA_128K_SIZE || size == SNA_128K_DUPLICATE_BANK_SIZE)
    {
        *model = SPECTRUM_MODEL_128K;
        return true;
    }
    return false;
}

/* Builds the chips ZX Spectrum machine from the already loaded ROM buffers and
   records the display descriptor used later for framebuffer conversion. */
static void spectrum_init_machine(Spectrum *spec)
{
    zx_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (spec->model == SPECTRUM_MODEL_PLUS3)
    {
        desc.type = ZX_TYPE_PLUS3;
    }
    else if (spec->model == SPECTRUM_MODEL_PLUS2A)
    {
        desc.type = ZX_TYPE_PLUS2A;
    }
    else if (spec->model == SPECTRUM_MODEL_128K ||
             spec->model == SPECTRUM_MODEL_PLUS2)
    {
        desc.type = ZX_TYPE_128;
    }
    else
    {
        desc.type = ZX_TYPE_48K;
    }
    desc.joystick_type = spec->joystick_type;
    desc.audio.callback = spec->audio_callback;
    desc.audio.num_samples = spec->audio_num_samples;
    desc.audio.sample_rate = spec->audio_sample_rate;
    desc.audio.beeper_volume = spec->beeper_volume;
    desc.audio.ay_volume = spec->ay_volume;
    desc.tape.callback = spec->tape_callback;
    desc.tape.load_trap = spec->tape_load_trap;
    desc.tape.user_data = spec->tape_user_data;
    desc.disk.ready = spec->disk_ready;
    desc.disk.read_sector = spec->disk_read_sector;
    desc.disk.write_sector = spec->disk_write_sector;
    desc.disk.sector_id = spec->disk_sector_id;
    desc.disk.user_data = spec->disk_user_data;
    desc.interface1.enabled =
        spec->interface1_rom_loaded &&
        spec->model != SPECTRUM_MODEL_PLUS2A &&
        spec->model != SPECTRUM_MODEL_PLUS3;
    desc.interface1.rom.ptr = spec->interface1_rom;
    desc.interface1.rom.size = sizeof(spec->interface1_rom);
    desc.interface1.ready = spec->microdrive_ready;
    desc.interface1.write_protected = spec->microdrive_write_protected;
    desc.interface1.length = spec->microdrive_length;
    desc.interface1.read = spec->microdrive_read;
    desc.interface1.write = spec->microdrive_write;
    desc.interface1.user_data = spec->microdrive_user_data;
    desc.expansion.fuller_audio = spec->fuller_audio_enabled;
    desc.expansion.specdrum = spec->specdrum_enabled;
    desc.expansion.covox = spec->covox_enabled;
    /* Make the ROM available to the core even when the attachment is
       currently disabled, so an SZX MFCE block can enable it on restore. */
    desc.expansion.multiface_enabled = spec->multiface_rom_loaded;
    desc.expansion.multiface_rom.ptr = spec->multiface_rom;
    desc.expansion.multiface_rom.size = spec->multiface_rom_loaded
        ? sizeof(spec->multiface_rom)
        : 0;
    if (spec->model == SPECTRUM_MODEL_PLUS2A ||
        spec->model == SPECTRUM_MODEL_PLUS3)
    {
        desc.roms.zxplus3_0.ptr = spec->rom[0];
        desc.roms.zxplus3_0.size = sizeof(spec->rom[0]);
        desc.roms.zxplus3_1.ptr = spec->rom[1];
        desc.roms.zxplus3_1.size = sizeof(spec->rom[1]);
        desc.roms.zxplus3_2.ptr = spec->rom[2];
        desc.roms.zxplus3_2.size = sizeof(spec->rom[2]);
        desc.roms.zxplus3_3.ptr = spec->rom[3];
        desc.roms.zxplus3_3.size = sizeof(spec->rom[3]);
    }
    else if (spec->model == SPECTRUM_MODEL_128K ||
             spec->model == SPECTRUM_MODEL_PLUS2)
    {
        desc.roms.zx128_0.ptr = spec->rom[0];
        desc.roms.zx128_0.size = sizeof(spec->rom[0]);
        desc.roms.zx128_1.ptr = spec->rom[1];
        desc.roms.zx128_1.size = sizeof(spec->rom[0]);
    }
    else
    {
        desc.roms.zx48k.ptr = spec->rom[spec->rom48_index];
        desc.roms.zx48k.size = sizeof(spec->rom[spec->rom48_index]);
    }
    zx_init(&spec->machine, &desc);
    zx_set_multiface_enabled(
        &spec->machine,
        spec->multiface_enabled && spec->multiface_rom_loaded);
    zx_set_kempston_mouse_enabled(&spec->machine, spec->kempston_mouse_enabled);
    zx_mouse(&spec->machine, spec->mouse_x, spec->mouse_y, spec->mouse_buttons);
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
    size_t error_buffer_size)
{
    if (!spec->rom_loaded[0])
    {
        snprintf(error_buffer, error_buffer_size, "No base ROM is loaded.");
        return false;
    }
    if ((model == SPECTRUM_MODEL_128K || model == SPECTRUM_MODEL_PLUS2) &&
        !spec->rom_loaded[1])
    {
        snprintf(
            error_buffer,
            error_buffer_size,
            "This machine needs 128K ROMs, but only a 48K ROM is available.");
        return false;
    }
    if ((model == SPECTRUM_MODEL_PLUS2A || model == SPECTRUM_MODEL_PLUS3) &&
        (!spec->rom_loaded[1] || !spec->rom_loaded[2] || !spec->rom_loaded[3]))
    {
        snprintf(
            error_buffer,
            error_buffer_size,
            "This machine needs four +2A/+3 ROM banks, but they are not all available.");
        return false;
    }

    spec->model = model;
    spec->machine_ready = false;
    spectrum_init_machine(spec);
    return true;
}

/* Resets the wrapper to a known empty state and records which machine type the
   caller wants to create once ROM data becomes available. */
void spectrum_init(Spectrum *spec, SpectrumModel model)
{
    memset(spec, 0, sizeof(*spec));
    spec->model = model;
    spec->audio_sample_rate = 44100;
    spec->beeper_volume = 0.35f;
    spec->ay_volume = 0.20f;
    spec->joystick_type = ZX_JOYSTICKTYPE_KEMPSTON;
    spec->mouse_x = 128;
    spec->mouse_y = 96;
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
    float ay_volume)
{
    spec->audio_callback = callback;
    spec->audio_sample_rate = sample_rate;
    spec->audio_num_samples = num_samples;
    spec->beeper_volume = beeper_volume;
    spec->ay_volume = ay_volume;
}

/* Records the tape input source so machine rebuilds preserve the callback. */
void spectrum_configure_tape_input(
    Spectrum *spec,
    zx_tape_input_callback_t callback,
    void *user_data)
{
    spec->tape_callback = callback;
    spec->tape_user_data = user_data;
    if (spec->machine_ready)
    {
        zx_set_tape_input(&spec->machine, callback, user_data);
    }
}

/* Records the fast-load trap source so rebuilds preserve ROM loader traps. */
void spectrum_configure_tape_load_trap(
    Spectrum *spec,
    zx_tape_load_trap_callback_t callback,
    void *user_data)
{
    spec->tape_load_trap = callback;
    spec->tape_user_data = user_data;
    if (spec->machine_ready)
    {
        zx_set_tape_load_trap(&spec->machine, callback, user_data);
    }
}

void spectrum_configure_disk(
    Spectrum *spec,
    zx_disk_ready_callback_t ready,
    zx_disk_read_sector_callback_t read_sector,
    zx_disk_write_sector_callback_t write_sector,
    zx_disk_sector_id_callback_t sector_id,
    void *user_data)
{
    spec->disk_ready = ready;
    spec->disk_read_sector = read_sector;
    spec->disk_write_sector = write_sector;
    spec->disk_sector_id = sector_id;
    spec->disk_user_data = user_data;
    if (spec->machine_ready)
    {
        zx_set_disk_callbacks(
            &spec->machine,
            ready,
            read_sector,
            write_sector,
            sector_id,
            user_data);
    }
}

void spectrum_configure_interface1(
    Spectrum *spec,
    zx_microdrive_ready_callback_t ready,
    zx_microdrive_write_protected_callback_t write_protected,
    zx_microdrive_length_callback_t length,
    zx_microdrive_read_callback_t read,
    zx_microdrive_write_callback_t write,
    void *user_data)
{
    spec->microdrive_ready = ready;
    spec->microdrive_write_protected = write_protected;
    spec->microdrive_length = length;
    spec->microdrive_read = read;
    spec->microdrive_write = write;
    spec->microdrive_user_data = user_data;
}

void spectrum_notify_disk_changed(Spectrum *spec)
{
    if (spec->machine_ready)
    {
        zx_notify_disk_changed(&spec->machine);
    }
}

bool spectrum_load_interface1_rom(
    Spectrum *spec,
    const char *rom_path,
    char *error_buffer,
    size_t error_buffer_size)
{
    size_t file_size = 0;

    spec->interface1_rom_loaded = false;
    if (rom_path == NULL || rom_path[0] == '\0') {
        return true;
    }
    if (!spectrum_load_file_exact(
            rom_path,
            spec->interface1_rom,
            sizeof(spec->interface1_rom),
            &file_size)) {
        snprintf(error_buffer, error_buffer_size, "Could not load Interface 1 ROM: %s", rom_path);
        return false;
    }
    if (file_size != sizeof(spec->interface1_rom)) {
        snprintf(error_buffer, error_buffer_size, "Interface 1 ROM must be exactly 8 KB: %s", rom_path);
        return false;
    }
    spec->interface1_rom_loaded = true;
    return true;
}

bool spectrum_load_multiface_rom(
    Spectrum *spec,
    const char *rom_path,
    char *error_buffer,
    size_t error_buffer_size)
{
    size_t file_size = 0;

    spec->multiface_rom_loaded = false;
    if (rom_path == NULL || rom_path[0] == '\0') {
        return true;
    }
    if (!spectrum_load_file_exact(
            rom_path,
            spec->multiface_rom,
            sizeof(spec->multiface_rom),
            &file_size)) {
        snprintf(error_buffer, error_buffer_size, "Could not load Multiface ROM: %s", rom_path);
        return false;
    }
    if (file_size != sizeof(spec->multiface_rom)) {
        snprintf(error_buffer, error_buffer_size, "Multiface ROM must be exactly 8 KB: %s", rom_path);
        return false;
    }
    spec->multiface_rom_loaded = true;
    return true;
}

/* Loads ROM data from disk, validates whether it matches the requested 48K,
   128K, or +3 layout, then creates and resets the embedded chips-based machine. */
bool spectrum_load_roms(
    Spectrum *spec,
    const char *rom_path_a,
    const char *rom_path_b,
    char *error_buffer,
    size_t error_buffer_size)
{
    size_t file_size = 0;
    uint8_t combined[0x10000];
    memset(spec->rom_loaded, 0, sizeof(spec->rom_loaded));
    spec->rom48_index = 0;

    {
        if (!spectrum_load_file_exact(rom_path_a, combined, sizeof(combined), &file_size))
        {
            snprintf(error_buffer, error_buffer_size, "Could not load ROM file: %s", rom_path_a);
            return false;
        }

        if (file_size == 0x4000)
        {
            memcpy(spec->rom[0], combined, 0x4000);
            spec->rom_loaded[0] = true;
        }
        else if (file_size == 0x8000)
        {
            memcpy(spec->rom[0], combined, 0x4000);
            memcpy(spec->rom[1], combined + 0x4000, 0x4000);
            spec->rom_loaded[0] = true;
            spec->rom_loaded[1] = true;
            spec->rom48_index = 1;
        }
        else if (file_size == 0x10000 &&
                 (spec->model == SPECTRUM_MODEL_PLUS2A ||
                  spec->model == SPECTRUM_MODEL_PLUS3))
        {
            for (size_t bank = 0; bank < 4; ++bank)
            {
                memcpy(spec->rom[bank], combined + (bank * 0x4000), 0x4000);
                spec->rom_loaded[bank] = true;
            }
        }
        else
        {
            snprintf(
                error_buffer,
                error_buffer_size,
                (spec->model == SPECTRUM_MODEL_PLUS2A ||
                 spec->model == SPECTRUM_MODEL_PLUS3)
                    ? "+2A/+3 mode needs one combined 64 KB ROM: %s"
                    : "ROM file must be 16 KB or 32 KB: %s",
                rom_path_a);
            return false;
        }
    }

    if (rom_path_b != NULL)
    {
        size_t second_size = 0;
        if (!spectrum_load_file_exact(rom_path_b, spec->rom[1], sizeof(spec->rom[1]), &second_size))
        {
            snprintf(error_buffer, error_buffer_size, "Could not load ROM file: %s", rom_path_b);
            return false;
        }
        if (second_size != 0x4000)
        {
            snprintf(error_buffer, error_buffer_size, "Second ROM must be exactly 16 KB: %s", rom_path_b);
            return false;
        }
        spec->rom_loaded[1] = true;
        spec->rom48_index = 1;
    }

    if ((spec->model == SPECTRUM_MODEL_128K ||
         spec->model == SPECTRUM_MODEL_PLUS2) &&
        !spec->rom_loaded[1])
    {
        snprintf(
            error_buffer,
            error_buffer_size,
            "128K/+2 mode needs either a combined 32 KB ROM or two 16 KB ROMs.");
        return false;
    }
    if ((spec->model == SPECTRUM_MODEL_PLUS2A ||
         spec->model == SPECTRUM_MODEL_PLUS3) &&
        (!spec->rom_loaded[0] || !spec->rom_loaded[1] ||
         !spec->rom_loaded[2] || !spec->rom_loaded[3]))
    {
        snprintf(error_buffer, error_buffer_size, "+2A/+3 mode needs one combined 64 KB ROM.");
        return false;
    }

    spectrum_init_machine(spec);
    spectrum_reset(spec);
    return true;
}

/* Resets the embedded emulator if it has already been created and synchronizes
   the wrapper framebuffer with the machine's freshly reset display state. */
void spectrum_reset(Spectrum *spec)
{
    if (!spec->machine_ready)
    {
        return;
    }
    zx_reset(&spec->machine);
    spectrum_render_frame(spec);
}

/* Converts the emulator's indexed display output into the 32-bit framebuffer
   consumed by the Win32 frontend for painting the current frame. */
void spectrum_render_frame(Spectrum *spec)
{
    if (!spec->machine_ready)
    {
        memset(spec->framebuffer, 0, sizeof(spec->framebuffer));
        return;
    }

    const uint8_t *src = (const uint8_t *)spec->display.frame.buffer.ptr;
    const int pitch = spec->display.frame.dim.width;

    for (int y = 0; y < ZX_SCREEN_HEIGHT; ++y)
    {
        const uint8_t *src_row = src + y * pitch;
        uint32_t *dst_row = &spec->framebuffer[y * ZX_SCREEN_WIDTH];
        for (int x = 0; x < ZX_SCREEN_WIDTH; ++x)
        {
            uint32_t color = zx_display_color(&spec->machine, y, src_row[x]);
            dst_row[x] =
                ((color & 0x000000FFu) << 16) |
                (color & 0x0000FF00u) |
                ((color & 0x00FF0000u) >> 16);
        }
    }
}

/* Delivers a host key press to the embedded chips keyboard handler when the
   machine is initialized and ready to receive input. */
void spectrum_key_down(Spectrum *spec, int key_code)
{
    if (!spec->machine_ready)
    {
        return;
    }
    zx_key_down(&spec->machine, key_code);
}

/* Delivers a host key release to the embedded chips keyboard handler when the
   machine is initialized and ready to receive input. */
void spectrum_key_up(Spectrum *spec, int key_code)
{
    if (!spec->machine_ready)
    {
        return;
    }
    zx_key_up(&spec->machine, key_code);
}

void spectrum_set_joystick_type(Spectrum *spec, zx_joystick_type_t type)
{
    spec->joystick_type = type;
    if (spec->machine_ready)
    {
        zx_set_joystick_type(&spec->machine, type);
    }
}

/* Updates the emulated joystick lines so frontend controller input can feed
   the selected interface without interfering with normal keyboard input. */
void spectrum_set_joystick_mask(Spectrum *spec, uint8_t mask)
{
    if (!spec->machine_ready)
    {
        return;
    }
    zx_joystick(&spec->machine, mask);
}

void spectrum_set_kempston_mouse_enabled(Spectrum *spec, bool enabled)
{
    spec->kempston_mouse_enabled = enabled;
    if (spec->machine_ready)
    {
        zx_set_kempston_mouse_enabled(&spec->machine, enabled);
    }
}

void spectrum_set_mouse(Spectrum *spec, uint8_t x, uint8_t y, uint8_t buttons)
{
    spec->mouse_x = x;
    spec->mouse_y = y;
    spec->mouse_buttons = buttons & 0x07u;
    if (spec->machine_ready)
    {
        zx_mouse(&spec->machine, spec->mouse_x, spec->mouse_y, spec->mouse_buttons);
    }
}

void spectrum_set_expansion_audio(
    Spectrum *spec,
    bool fuller_audio,
    bool specdrum,
    bool covox)
{
    spec->fuller_audio_enabled = fuller_audio;
    spec->specdrum_enabled = specdrum;
    spec->covox_enabled = covox;
    if (spec->machine_ready) {
        zx_set_expansion_audio(&spec->machine, fuller_audio, specdrum, covox);
    }
}

void spectrum_set_multiface_enabled(Spectrum *spec, bool enabled)
{
    spec->multiface_enabled = enabled &&
        (spec->multiface_rom_loaded ||
         (spec->machine_ready && spec->machine.multiface_16k_ram_mode));
    if (spec->machine_ready) {
        zx_set_multiface_enabled(&spec->machine, spec->multiface_enabled);
    }
}

void spectrum_multiface_nmi(Spectrum *spec)
{
    if (spec != NULL && spec->machine_ready && spec->multiface_enabled) {
        zx_multiface_nmi(&spec->machine);
    }
}

bool spectrum_load_screen_scr_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    if (spec == NULL || !spec->machine_ready || data == NULL)
    {
        snprintf(error_buffer, error_buffer_size, "No running machine or SCR data is available.");
        return false;
    }
    if (data_size != SCR_SCREEN_SIZE || spec->machine.display_ram_bank >= 8)
    {
        snprintf(error_buffer, error_buffer_size, "SCR images must be exactly 6912 bytes.");
        return false;
    }
    memcpy(
        spec->machine.ram[spec->machine.display_ram_bank],
        data,
        SCR_SCREEN_SIZE);
    spectrum_render_frame(spec);
    return true;
}

bool spectrum_save_screen_scr_file(
    const Spectrum *spec,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file;
    if (spec == NULL || !spec->machine_ready || path == NULL || path[0] == '\0' ||
        spec->machine.display_ram_bank >= 8)
    {
        snprintf(error_buffer, error_buffer_size, "No visible Spectrum screen is available to save.");
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL)
    {
        snprintf(error_buffer, error_buffer_size, "Could not create SCR file: %s", path);
        return false;
    }
    const bool wrote_all = fwrite(
        spec->machine.ram[spec->machine.display_ram_bank],
        1,
        SCR_SCREEN_SIZE,
        file) == SCR_SCREEN_SIZE;
    const int close_result = fclose(file);
    if (!wrote_all || close_result != 0)
    {
        snprintf(error_buffer, error_buffer_size, "Could not save the complete SCR file: %s", path);
        return false;
    }
    return true;
}

static void spectrum_store_sna_registers(
    const zx_t *machine,
    uint8_t *header,
    uint16_t stack_pointer)
{
    header[0] = machine->cpu.i;
    spectrum_write_u16(&header[1], machine->cpu.hl2);
    spectrum_write_u16(&header[3], machine->cpu.de2);
    spectrum_write_u16(&header[5], machine->cpu.bc2);
    spectrum_write_u16(&header[7], machine->cpu.af2);
    spectrum_write_u16(&header[9], machine->cpu.hl);
    spectrum_write_u16(&header[11], machine->cpu.de);
    spectrum_write_u16(&header[13], machine->cpu.bc);
    spectrum_write_u16(&header[15], machine->cpu.iy);
    spectrum_write_u16(&header[17], machine->cpu.ix);
    header[19] = machine->cpu.iff2 ? 0x04 : 0;
    header[20] = machine->cpu.r;
    spectrum_write_u16(&header[21], machine->cpu.af);
    spectrum_write_u16(&header[23], stack_pointer);
    header[25] = machine->cpu.im;
    header[26] = machine->border_color & 0x07u;
}

static uint8_t spectrum_z80_hardware_mode(SpectrumModel model)
{
    switch (model)
    {
        case SPECTRUM_MODEL_128K: return 4;
        case SPECTRUM_MODEL_PLUS2: return 12;
        case SPECTRUM_MODEL_PLUS2A: return 13;
        case SPECTRUM_MODEL_PLUS3: return 7;
        case SPECTRUM_MODEL_48K:
        default: return 0;
    }
}

static void spectrum_store_z80_registers(
    const zx_t *machine,
    uint8_t *header,
    uint16_t program_counter)
{
    header[0] = machine->cpu.a;
    header[1] = machine->cpu.f;
    header[2] = machine->cpu.c;
    header[3] = machine->cpu.b;
    header[4] = machine->cpu.l;
    header[5] = machine->cpu.h;
    spectrum_write_u16(header + 8, machine->cpu.sp);
    header[10] = machine->cpu.i;
    header[11] = machine->cpu.r & 0x7Fu;
    header[12] = (uint8_t)(((machine->cpu.r >> 7) & 1u) |
        ((machine->border_color & 7u) << 1));
    header[13] = machine->cpu.e;
    header[14] = machine->cpu.d;
    header[15] = (uint8_t)machine->cpu.bc2;
    header[16] = (uint8_t)(machine->cpu.bc2 >> 8);
    header[17] = (uint8_t)machine->cpu.de2;
    header[18] = (uint8_t)(machine->cpu.de2 >> 8);
    header[19] = (uint8_t)machine->cpu.hl2;
    header[20] = (uint8_t)(machine->cpu.hl2 >> 8);
    header[21] = (uint8_t)(machine->cpu.af2 >> 8);
    header[22] = (uint8_t)machine->cpu.af2;
    spectrum_write_u16(header + 23, machine->cpu.iy);
    spectrum_write_u16(header + 25, machine->cpu.ix);
    header[27] = machine->cpu.iff1 ? 1 : 0;
    header[28] = machine->cpu.iff2 ? 1 : 0;
    header[29] = machine->cpu.im & 3u;

    spectrum_write_u16(header + 30, 55);
    spectrum_write_u16(header + 32, program_counter);
}

bool spectrum_save_snapshot_z80_file(
    Spectrum *spec,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    zx_t *restore_state;
    zx_t *machine;
    uint32_t restore_version;
    uint16_t program_counter;
    const unsigned page_count = spec != NULL && spec->model == SPECTRUM_MODEL_48K ? 3u : 8u;
    const size_t data_size = Z80_V3_HEADER_SIZE +
        ((size_t)page_count * (Z80_PAGE_HEADER_SIZE + SNA_RAM_BANK_SIZE));
    uint8_t *data;
    size_t offset = Z80_V3_HEADER_SIZE;
    FILE *file;
    bool ok;

    if (spec == NULL || !spec->machine_ready || path == NULL || path[0] == '\0')
    {
        snprintf(error_buffer, error_buffer_size, "No running machine or snapshot path is available.");
        return false;
    }
    restore_state = (zx_t *)_aligned_malloc(sizeof(*restore_state), _Alignof(zx_t));
    data = (uint8_t *)calloc(1, data_size);
    if (restore_state == NULL || data == NULL)
    {
        _aligned_free(restore_state);
        free(data);
        snprintf(error_buffer, error_buffer_size, "Out of memory while creating the Z80 snapshot.");
        return false;
    }

    restore_version = zx_save_snapshot(&spec->machine, restore_state);
    machine = &spec->machine;
    for (unsigned ticks = 0; ticks < 128 && !z80_opdone(&machine->cpu); ++ticks)
    {
        machine->pins = _zx_tick(machine, machine->pins);
    }
    program_counter = z80_opdone(&machine->cpu)
        ? Z80_GET_ADDR(machine->pins)
        : machine->cpu.pc;
    spectrum_store_z80_registers(machine, data, program_counter);
    data[34] = spectrum_z80_hardware_mode(spec->model);
    data[35] = machine->last_mem_config;
    data[38] = machine->ay.addr & 0x0Fu;
    memcpy(data + 39, machine->ay.reg, AY38910_NUM_REGISTERS);
    data[86] = (spec->model == SPECTRUM_MODEL_PLUS2A ||
                spec->model == SPECTRUM_MODEL_PLUS3)
        ? machine->last_plus3_mem_config
        : 0;

    if (spec->model == SPECTRUM_MODEL_48K)
    {
        static const uint8_t page_ids[3] = {8, 4, 5};
        for (unsigned page = 0; page < 3; ++page)
        {
            data[offset] = 0xFF;
            data[offset + 1] = 0xFF;
            data[offset + 2] = page_ids[page];
            memcpy(data + offset + 3, machine->ram[page], SNA_RAM_BANK_SIZE);
            offset += Z80_PAGE_HEADER_SIZE + SNA_RAM_BANK_SIZE;
        }
    }
    else
    {
        for (uint8_t page = 0; page < 8; ++page)
        {
            data[offset] = 0xFF;
            data[offset + 1] = 0xFF;
            data[offset + 2] = (uint8_t)(page + 3u);
            memcpy(data + offset + 3, machine->ram[page], SNA_RAM_BANK_SIZE);
            offset += Z80_PAGE_HEADER_SIZE + SNA_RAM_BANK_SIZE;
        }
    }
    (void)zx_load_snapshot(&spec->machine, restore_version, restore_state);
    _aligned_free(restore_state);

    file = fopen(path, "wb");
    if (file == NULL)
    {
        free(data);
        snprintf(error_buffer, error_buffer_size, "Could not open the Z80 snapshot for writing.");
        return false;
    }
    ok = fwrite(data, 1, data_size, file) == data_size;
    if (fclose(file) != 0) ok = false;
    free(data);
    if (!ok)
    {
        snprintf(error_buffer, error_buffer_size, "Could not save the complete Z80 snapshot.");
        return false;
    }
    return true;
}

bool spectrum_save_snapshot_sna_file(
    Spectrum *spec,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size)
{
    zx_t *machine;
    zx_t *restore_state;
    uint32_t restore_version;
    uint16_t program_counter;
    uint8_t *data;
    size_t data_size;
    size_t offset;
    uint8_t paged_bank;
    FILE *file;
    bool ok;

    if (spec == NULL || !spec->machine_ready || path == NULL || path[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "No running machine or snapshot path is available.");
        return false;
    }
    if (spec->model == SPECTRUM_MODEL_PLUS2A ||
        spec->model == SPECTRUM_MODEL_PLUS3) {
        snprintf(
            error_buffer,
            error_buffer_size,
            "SNA saving currently supports 48K, 128K, and +2 machines.");
        return false;
    }

    restore_state = (zx_t *)_aligned_malloc(sizeof(*restore_state), _Alignof(zx_t));
    if (restore_state == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while preparing the snapshot.");
        return false;
    }
    restore_version = zx_save_snapshot(&spec->machine, restore_state);
    machine = &spec->machine;
    for (unsigned ticks = 0; ticks < 128 && !z80_opdone(&machine->cpu); ++ticks) {
        machine->pins = _zx_tick(machine, machine->pins);
    }
    program_counter = z80_opdone(&machine->cpu)
        ? Z80_GET_ADDR(machine->pins)
        : machine->cpu.pc;
    paged_bank = machine->last_mem_config & 0x07u;
    data_size = spec->model == SPECTRUM_MODEL_48K
        ? SNA_48K_SIZE
        : ((paged_bank == 2 || paged_bank == 5)
            ? SNA_128K_DUPLICATE_BANK_SIZE
            : SNA_128K_SIZE);
    data = (uint8_t *)calloc(1, data_size);
    if (data == NULL) {
        (void)zx_load_snapshot(&spec->machine, restore_version, restore_state);
        _aligned_free(restore_state);
        snprintf(error_buffer, error_buffer_size, "Out of memory while creating the snapshot.");
        return false;
    }

    if (spec->model == SPECTRUM_MODEL_48K) {
        uint16_t saved_sp;
        size_t stack_offset;

        if (machine->cpu.sp < 0x4002u) {
            free(data);
            (void)zx_load_snapshot(&spec->machine, restore_version, restore_state);
            _aligned_free(restore_state);
            snprintf(
                error_buffer,
                error_buffer_size,
                "The current stack is in ROM and cannot be represented by a 48K SNA file.");
            return false;
        }
        saved_sp = (uint16_t)(machine->cpu.sp - 2u);
        spectrum_store_sna_registers(machine, data, saved_sp);
        memcpy(&data[SNA_HEADER_SIZE], machine->ram[0], SNA_RAM_BANK_SIZE);
        memcpy(&data[SNA_HEADER_SIZE + SNA_RAM_BANK_SIZE], machine->ram[1], SNA_RAM_BANK_SIZE);
        memcpy(&data[SNA_HEADER_SIZE + (2 * SNA_RAM_BANK_SIZE)], machine->ram[2], SNA_RAM_BANK_SIZE);
        stack_offset = SNA_HEADER_SIZE + (size_t)(saved_sp - 0x4000u);
        spectrum_write_u16(&data[stack_offset], program_counter);
    }
    else {
        spectrum_store_sna_registers(machine, data, machine->cpu.sp);
        memcpy(&data[SNA_HEADER_SIZE], machine->ram[5], SNA_RAM_BANK_SIZE);
        memcpy(&data[SNA_HEADER_SIZE + SNA_RAM_BANK_SIZE], machine->ram[2], SNA_RAM_BANK_SIZE);
        memcpy(&data[SNA_HEADER_SIZE + (2 * SNA_RAM_BANK_SIZE)], machine->ram[paged_bank], SNA_RAM_BANK_SIZE);
        spectrum_write_u16(&data[SNA_48K_SIZE], program_counter);
        data[SNA_48K_SIZE + 2u] = machine->last_mem_config;
        data[SNA_48K_SIZE + 3u] = 0;
        offset = SNA_128K_BASE_SIZE;
        for (uint8_t bank = 0; bank < 8; ++bank) {
            if (bank == 5 || bank == 2 || bank == paged_bank) {
                continue;
            }
            memcpy(&data[offset], machine->ram[bank], SNA_RAM_BANK_SIZE);
            offset += SNA_RAM_BANK_SIZE;
        }
    }
    (void)zx_load_snapshot(&spec->machine, restore_version, restore_state);
    _aligned_free(restore_state);

    file = fopen(path, "wb");
    if (file == NULL) {
        free(data);
        snprintf(error_buffer, error_buffer_size, "Could not open the snapshot file for writing.");
        return false;
    }
    ok = fwrite(data, 1, data_size, file) == data_size;
    if (fclose(file) != 0) {
        ok = false;
    }
    free(data);
    if (!ok) {
        snprintf(error_buffer, error_buffer_size, "Could not save the complete snapshot file.");
        return false;
    }
    return true;
}

bool spectrum_save_runtime_state(
    Spectrum *spec,
    zx_t *state,
    uint32_t *version)
{
    if (spec == NULL || state == NULL || version == NULL || !spec->machine_ready) {
        return false;
    }
    *version = zx_save_snapshot(&spec->machine, state);
    return true;
}

bool spectrum_load_runtime_state(
    Spectrum *spec,
    SpectrumModel model,
    zx_t *state,
    uint32_t version)
{
    if (spec == NULL || state == NULL || !spec->machine_ready || spec->model != model ||
        !zx_load_snapshot(&spec->machine, version, state)) {
        return false;
    }
    spectrum_render_frame(spec);
    return true;
}

static void spectrum_restore_sna_registers(zx_t *machine, const uint8_t *header)
{
    z80_reset(&machine->cpu);
    machine->cpu.i = header[0];
    machine->cpu.hl2 = spectrum_read_u16(&header[1]);
    machine->cpu.de2 = spectrum_read_u16(&header[3]);
    machine->cpu.bc2 = spectrum_read_u16(&header[5]);
    machine->cpu.af2 = spectrum_read_u16(&header[7]);
    machine->cpu.hl = spectrum_read_u16(&header[9]);
    machine->cpu.de = spectrum_read_u16(&header[11]);
    machine->cpu.bc = spectrum_read_u16(&header[13]);
    machine->cpu.iy = spectrum_read_u16(&header[15]);
    machine->cpu.ix = spectrum_read_u16(&header[17]);
    machine->cpu.iff2 = (header[19] & 0x04u) != 0;
    machine->cpu.iff1 = machine->cpu.iff2;
    machine->cpu.r = header[20];
    machine->cpu.af = spectrum_read_u16(&header[21]);
    machine->cpu.sp = spectrum_read_u16(&header[23]);
    machine->cpu.im = header[25];
    machine->border_color = header[26] & 0x07u;
    machine->last_fe_out = machine->border_color;
    beeper_set(&machine->beeper, false);
}

bool spectrum_load_snapshot_sna_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    SpectrumModel snapshot_model;
    uint8_t paged_bank = 0;
    uint8_t port_7ffd = 0;
    size_t expected_size = SNA_48K_SIZE;

    if (spec == NULL || data == NULL ||
        !spectrum_detect_snapshot_sna_model_data(data, data_size, &snapshot_model))
    {
        snprintf(error_buffer, error_buffer_size, "Unsupported or corrupt .sna snapshot.");
        return false;
    }
    if (data[25] > 2)
    {
        snprintf(error_buffer, error_buffer_size, "Invalid interrupt mode in .sna snapshot.");
        return false;
    }

    if (snapshot_model == SPECTRUM_MODEL_48K)
    {
        const uint16_t stack_pointer = spectrum_read_u16(&data[23]);
        if (stack_pointer < 0x4000u || stack_pointer == 0xFFFFu)
        {
            snprintf(error_buffer, error_buffer_size, "Invalid 48K .sna program-counter stack address.");
            return false;
        }
    }
    else
    {
        port_7ffd = data[SNA_48K_SIZE + 2u];
        paged_bank = port_7ffd & 0x07u;
        expected_size = (paged_bank == 2 || paged_bank == 5)
            ? SNA_128K_DUPLICATE_BANK_SIZE
            : SNA_128K_SIZE;
        if (data_size != expected_size)
        {
            snprintf(error_buffer, error_buffer_size, "128K .sna snapshot has the wrong RAM-bank count.");
            return false;
        }
        if (data[SNA_48K_SIZE + 3u] != 0)
        {
            snprintf(error_buffer, error_buffer_size, "TR-DOS-paged .sna snapshots are not supported.");
            return false;
        }
    }

    if (!spectrum_snapshot_model_compatible(snapshot_model, spec->model))
    {
        if (!spectrum_rebuild_for_model(spec, snapshot_model, error_buffer, error_buffer_size))
        {
            return false;
        }
    }
    _zx_ulaplus_reset(&spec->machine);

    if (snapshot_model == SPECTRUM_MODEL_48K)
    {
        uint16_t program_counter;
        memcpy(spec->machine.ram[0], &data[SNA_HEADER_SIZE], SNA_RAM_BANK_SIZE);
        memcpy(spec->machine.ram[1], &data[SNA_HEADER_SIZE + SNA_RAM_BANK_SIZE], SNA_RAM_BANK_SIZE);
        memcpy(spec->machine.ram[2], &data[SNA_HEADER_SIZE + (2 * SNA_RAM_BANK_SIZE)], SNA_RAM_BANK_SIZE);
        spectrum_restore_sna_registers(&spec->machine, data);

        program_counter = (uint16_t)(
            mem_rd(&spec->machine.mem, spec->machine.cpu.sp) |
            (mem_rd(&spec->machine.mem, (uint16_t)(spec->machine.cpu.sp + 1u)) << 8));
        spec->machine.cpu.sp = (uint16_t)(spec->machine.cpu.sp + 2u);
        spec->machine.pins = z80_prefetch(&spec->machine.cpu, program_counter);
    }
    else
    {
        size_t remaining_offset = SNA_128K_BASE_SIZE;
        memcpy(spec->machine.ram[5], &data[SNA_HEADER_SIZE], SNA_RAM_BANK_SIZE);
        memcpy(spec->machine.ram[2], &data[SNA_HEADER_SIZE + SNA_RAM_BANK_SIZE], SNA_RAM_BANK_SIZE);
        memcpy(spec->machine.ram[paged_bank], &data[SNA_HEADER_SIZE + (2 * SNA_RAM_BANK_SIZE)], SNA_RAM_BANK_SIZE);

        for (uint8_t bank = 0; bank < 8; ++bank)
        {
            if (bank == 5 || bank == 2 || bank == paged_bank)
            {
                continue;
            }
            memcpy(spec->machine.ram[bank], &data[remaining_offset], SNA_RAM_BANK_SIZE);
            remaining_offset += SNA_RAM_BANK_SIZE;
        }
        if (remaining_offset != expected_size)
        {
            snprintf(error_buffer, error_buffer_size, "Could not map all RAM banks from .sna snapshot.");
            return false;
        }

        spectrum_restore_sna_registers(&spec->machine, data);
        ay38910_reset(&spec->machine.ay);
        spec->machine.memory_paging_disabled = false;
        _zx_update_memory_map_zx128(&spec->machine, port_7ffd);
        spec->machine.pins = z80_prefetch(
            &spec->machine.cpu,
            spectrum_read_u16(&data[SNA_48K_SIZE]));
    }

    spectrum_render_frame(spec);
    return true;
}

enum {
    SZX_HEADER_SIZE = 8,
    SZX_BLOCK_HEADER_SIZE = 8,
    SZX_Z80R_SIZE = 37,
    SZX_SPCR_SIZE = 8,
    SZX_AY_SIZE = 18,
    SZX_PLTT_SIZE = 67,
    SZX_PAGE_SIZE = 0x4000,
    SZX_RAMP_COMPRESSED = 1,
    SZX_PLTT_ENABLED = 1,
    SZX_MF_PAGED = 0x01,
    SZX_MF_COMPRESSED = 0x02,
    SZX_MF_SOFTWARE_LOCKOUT = 0x04,
    SZX_MF_RED_BUTTON_DISABLED = 0x08,
    SZX_MF_DISABLED = 0x10,
    SZX_MF_16K_RAM = 0x20
};

typedef struct SpectrumSzxState {
    SpectrumModel model;
    uint8_t z80[SZX_Z80R_SIZE];
    uint8_t spcr[SZX_SPCR_SIZE];
    uint8_t ay[SZX_AY_SIZE];
    uint8_t ulaplus_palette[ZX_ULAPLUS_PALETTE_SIZE];
    uint8_t ulaplus_register;
    uint8_t specdrum_value;
    uint8_t covox_value;
    uint8_t multiface_flags;
    uint8_t multiface_ram[ZX_MULTIFACE_RAM_SIZE];
    uint8_t *pages[8];
    uint8_t page_mask;
    bool has_z80;
    bool has_spcr;
    bool has_ay;
    bool has_ulaplus;
    bool ulaplus_enabled;
    bool has_specdrum;
    bool has_covox;
    bool has_multiface;
} SpectrumSzxState;

bool spectrum_detect_snapshot_szx_model_data(
    const uint8_t *data,
    size_t size,
    SpectrumModel *model)
{
    if (data == NULL || model == NULL || size < SZX_HEADER_SIZE ||
        memcmp(data, "ZXST", 4) != 0 || data[4] != 1)
    {
        return false;
    }
    switch (data[6])
    {
        case 1: *model = SPECTRUM_MODEL_48K; return true;
        case 2: *model = SPECTRUM_MODEL_128K; return true;
        case 3: *model = SPECTRUM_MODEL_PLUS2; return true;
        case 4: *model = SPECTRUM_MODEL_PLUS2A; return true;
        case 5: *model = SPECTRUM_MODEL_PLUS3; return true;
        default: return false;
    }
}

static void spectrum_discard_szx_state(SpectrumSzxState *state)
{
    for (unsigned page = 0; page < 8; ++page)
    {
        free(state->pages[page]);
        state->pages[page] = NULL;
    }
}

static bool spectrum_parse_szx(
    const uint8_t *data,
    size_t data_size,
    SpectrumSzxState *state,
    char *error_buffer,
    size_t error_buffer_size)
{
    size_t offset = SZX_HEADER_SIZE;
    SpectrumModel model;
    memset(state, 0, sizeof(*state));

    if (!spectrum_detect_snapshot_szx_model_data(data, data_size, &model))
    {
        snprintf(error_buffer, error_buffer_size,
            "Unsupported SZX version or machine.");
        return false;
    }
    state->model = model;

    while (offset < data_size)
    {
        const uint8_t *header;
        const uint8_t *payload;
        uint32_t payload_size;
        if (data_size - offset < SZX_BLOCK_HEADER_SIZE)
        {
            snprintf(error_buffer, error_buffer_size, "Truncated SZX block header.");
            goto fail;
        }
        header = data + offset;
        payload_size = spectrum_read_u32(header + 4);
        offset += SZX_BLOCK_HEADER_SIZE;
        if (payload_size > data_size - offset)
        {
            snprintf(error_buffer, error_buffer_size, "Truncated SZX block payload.");
            goto fail;
        }
        payload = data + offset;

        if (memcmp(header, "Z80R", 4) == 0)
        {
            if (state->has_z80 || payload_size != SZX_Z80R_SIZE)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX Z80R block.");
                goto fail;
            }
            memcpy(state->z80, payload, sizeof(state->z80));
            state->has_z80 = true;
        }
        else if (memcmp(header, "SPCR", 4) == 0)
        {
            if (state->has_spcr || payload_size != SZX_SPCR_SIZE)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX SPCR block.");
                goto fail;
            }
            memcpy(state->spcr, payload, sizeof(state->spcr));
            state->has_spcr = true;
        }
        else if (memcmp(header, "AY\0\0", 4) == 0)
        {
            if (state->has_ay || payload_size != SZX_AY_SIZE)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX AY block.");
                goto fail;
            }
            memcpy(state->ay, payload, sizeof(state->ay));
            state->has_ay = true;
        }
        else if (memcmp(header, "PLTT", 4) == 0)
        {
            if (state->has_ulaplus || payload_size != SZX_PLTT_SIZE ||
                (payload[0] & ~SZX_PLTT_ENABLED) != 0)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX PLTT block.");
                goto fail;
            }
            state->ulaplus_enabled = (payload[0] & SZX_PLTT_ENABLED) != 0;
            state->ulaplus_register = payload[1];
            memcpy(
                state->ulaplus_palette,
                payload + 2,
                sizeof(state->ulaplus_palette));
            state->has_ulaplus = true;
        }
        else if (memcmp(header, "DRUM", 4) == 0)
        {
            if (state->has_specdrum || payload_size != 1)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX DRUM block.");
                goto fail;
            }
            state->specdrum_value = payload[0];
            state->has_specdrum = true;
        }
        else if (memcmp(header, "COVX", 4) == 0)
        {
            if (state->has_covox || payload_size != 4)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX COVX block.");
                goto fail;
            }
            state->covox_value = payload[0];
            state->has_covox = true;
        }
        else if (memcmp(header, "MFCE", 4) == 0)
        {
            uint8_t flags;
            size_t ram_size;
            if (state->has_multiface || payload_size < 2)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX MFCE block.");
                goto fail;
            }
            flags = payload[1];
            ram_size = (flags & SZX_MF_16K_RAM) != 0
                ? ZX_MULTIFACE_RAM_SIZE
                : ZX_MULTIFACE_ROM_SIZE;
            if ((flags & SZX_MF_COMPRESSED) != 0)
            {
                if (!szx_inflate_zlib(
                        payload + 2,
                        payload_size - 2,
                        state->multiface_ram,
                        ram_size))
                {
                    snprintf(error_buffer, error_buffer_size, "Could not decompress SZX Multiface RAM.");
                    goto fail;
                }
            }
            else if (payload_size == 2 + ram_size)
            {
                memcpy(state->multiface_ram, payload + 2, ram_size);
            }
            else
            {
                snprintf(error_buffer, error_buffer_size, "Uncompressed SZX Multiface RAM has the wrong size.");
                goto fail;
            }
            state->multiface_flags = flags;
            state->has_multiface = true;
        }
        else if (memcmp(header, "RAMP", 4) == 0)
        {
            uint16_t flags;
            uint8_t page;
            if (payload_size < 3)
            {
                snprintf(error_buffer, error_buffer_size, "Invalid SZX RAMP block.");
                goto fail;
            }
            flags = spectrum_read_u16(payload);
            page = payload[2];
            if (page >= 8 || (flags & ~SZX_RAMP_COMPRESSED) != 0 ||
                state->pages[page] != NULL ||
                (state->model == SPECTRUM_MODEL_48K && page != 0 && page != 2 && page != 5))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid or duplicate SZX RAM page %u.", page);
                goto fail;
            }
            state->pages[page] = (uint8_t *)malloc(SZX_PAGE_SIZE);
            if (state->pages[page] == NULL)
            {
                snprintf(error_buffer, error_buffer_size, "Out of memory while decoding SZX RAM.");
                goto fail;
            }
            if ((flags & SZX_RAMP_COMPRESSED) != 0)
            {
                if (!szx_inflate_zlib(
                        payload + 3, payload_size - 3,
                        state->pages[page], SZX_PAGE_SIZE))
                {
                    snprintf(error_buffer, error_buffer_size,
                        "Could not decompress SZX RAM page %u.", page);
                    goto fail;
                }
            }
            else
            {
                if (payload_size != 3 + SZX_PAGE_SIZE)
                {
                    snprintf(error_buffer, error_buffer_size,
                        "Uncompressed SZX RAM page %u is not 16 KB.", page);
                    goto fail;
                }
                memcpy(state->pages[page], payload + 3, SZX_PAGE_SIZE);
            }
            state->page_mask |= (uint8_t)(1u << page);
        }
        /* ZX-State is extensible: blocks for unsupported peripherals can be
           skipped without losing the core machine state. */
        offset += payload_size;
    }

    if (!state->has_z80 || !state->has_spcr ||
        (state->model == SPECTRUM_MODEL_48K && state->page_mask != 0x25u) ||
        (state->model != SPECTRUM_MODEL_48K && state->page_mask != 0xFFu))
    {
        snprintf(error_buffer, error_buffer_size, "SZX snapshot is missing required machine-state blocks.");
        goto fail;
    }
    if (state->z80[28] > 2)
    {
        snprintf(error_buffer, error_buffer_size, "Invalid interrupt mode in SZX Z80R block.");
        goto fail;
    }
    {
        const uint32_t frame_tstates = state->model == SPECTRUM_MODEL_48K
            ? 312u * 224u
            : 311u * 228u;
        if (spectrum_read_u32(state->z80 + 29) >= frame_tstates)
        {
            snprintf(error_buffer, error_buffer_size, "Invalid frame timing in SZX Z80R block.");
            goto fail;
        }
    }
    return true;

fail:
    spectrum_discard_szx_state(state);
    return false;
}

static void spectrum_restore_szx_cpu(zx_t *machine, const uint8_t *state)
{
    const uint16_t program_counter = spectrum_read_u16(state + 22);
    const uint8_t flags = state[34];
    z80_reset(&machine->cpu);
    machine->cpu.af = spectrum_read_u16(state);
    machine->cpu.bc = spectrum_read_u16(state + 2);
    machine->cpu.de = spectrum_read_u16(state + 4);
    machine->cpu.hl = spectrum_read_u16(state + 6);
    machine->cpu.af2 = spectrum_read_u16(state + 8);
    machine->cpu.bc2 = spectrum_read_u16(state + 10);
    machine->cpu.de2 = spectrum_read_u16(state + 12);
    machine->cpu.hl2 = spectrum_read_u16(state + 14);
    machine->cpu.ix = spectrum_read_u16(state + 16);
    machine->cpu.iy = spectrum_read_u16(state + 18);
    machine->cpu.sp = spectrum_read_u16(state + 20);
    machine->cpu.i = state[24];
    machine->cpu.r = state[25];
    machine->cpu.iff1 = state[26] != 0;
    machine->cpu.iff2 = state[27] != 0;
    machine->cpu.im = state[28];
    machine->cpu.wz = spectrum_read_u16(state + 35);
    machine->cpu.suppress_int = (flags & 0x01u) != 0;
    machine->pins = z80_prefetch(&machine->cpu, program_counter);
    if ((flags & 0x02u) != 0)
    {
        /* chips keeps PC on the HALT opcode while the bus repeats its fetch. */
        machine->cpu.pc--;
        machine->pins |= Z80_HALT;
    }
}

static void spectrum_restore_szx_timing(zx_t *machine, const uint8_t *state)
{
    const uint32_t frame_tstate = spectrum_read_u32(state + 29);
    const uint8_t interrupt_cycles = state[33];
    const uint32_t line_position = frame_tstate % (uint32_t)machine->scanline_period;

    machine->tick_count = frame_tstate;
    machine->frame_tstate = frame_tstate;
    machine->scanline_y = (int)(frame_tstate / (uint32_t)machine->scanline_period);
    machine->scanline_counter = machine->scanline_period - (int)line_position;
    machine->int_counter = interrupt_cycles != 0 ? (int)interrupt_cycles - 1 : 0;
    if (interrupt_cycles != 0)
    {
        machine->pins |= Z80_INT;
        machine->cpu.int_bits |= Z80_INT;
    }
    else
    {
        machine->pins &= ~Z80_INT;
        machine->cpu.int_bits &= ~Z80_INT;
    }
}

bool spectrum_load_snapshot_szx_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    SpectrumSzxState state;
    if (spec == NULL || data == NULL ||
        !spectrum_parse_szx(data, data_size, &state, error_buffer, error_buffer_size))
    {
        return false;
    }
    if (!spectrum_snapshot_model_compatible(state.model, spec->model) &&
        !spectrum_rebuild_for_model(spec, state.model, error_buffer, error_buffer_size))
    {
        spectrum_discard_szx_state(&state);
        return false;
    }

    if (state.model == SPECTRUM_MODEL_48K)
    {
        memcpy(spec->machine.ram[0], state.pages[5], SZX_PAGE_SIZE);
        memcpy(spec->machine.ram[1], state.pages[2], SZX_PAGE_SIZE);
        memcpy(spec->machine.ram[2], state.pages[0], SZX_PAGE_SIZE);
    }
    else
    {
        for (unsigned page = 0; page < 8; ++page)
        {
            memcpy(spec->machine.ram[page], state.pages[page], SZX_PAGE_SIZE);
        }
        spec->machine.memory_paging_disabled = false;
        if (state.model == SPECTRUM_MODEL_PLUS2A ||
            state.model == SPECTRUM_MODEL_PLUS3)
        {
            spec->machine.last_mem_config = state.spcr[1];
            spec->machine.last_plus3_mem_config = state.spcr[2];
            spec->machine.display_ram_bank = (state.spcr[1] & 0x08u) ? 7 : 5;
            _zx_update_memory_map_plus3(&spec->machine);
            spec->machine.memory_paging_disabled = (state.spcr[1] & 0x20u) != 0;
        }
        else
        {
            _zx_update_memory_map_zx128(&spec->machine, state.spcr[1]);
        }
    }
    ay38910_reset(&spec->machine.ay);
    if (state.has_ay)
    {
        for (uint8_t reg = 0; reg < AY38910_NUM_REGISTERS; ++reg)
        {
            ay38910_set_register(&spec->machine.ay, reg, state.ay[2 + reg]);
        }
        ay38910_set_addr_latch(&spec->machine.ay, state.ay[1] & 0x0Fu);
    }
    _zx_ulaplus_reset(&spec->machine);
    if (state.has_ulaplus)
    {
        spec->machine.ulaplus_register = state.ulaplus_register;
        spec->machine.ulaplus_enabled = state.ulaplus_enabled;
        spec->machine.ulaplus_mode = state.ulaplus_enabled ? 1 : 0;
        memcpy(
            spec->machine.ulaplus_palette,
            state.ulaplus_palette,
            sizeof(spec->machine.ulaplus_palette));
        _zx_ulaplus_begin_scanline(&spec->machine);
    }
    spectrum_set_expansion_audio(
        spec,
        state.model == SPECTRUM_MODEL_48K && state.has_ay &&
            (state.ay[0] & 0x01u) != 0,
        state.has_specdrum,
        state.has_covox);
    spec->machine.specdrum_value = state.specdrum_value;
    spec->machine.covox_value = state.covox_value;
    spec->machine.multiface_software_lockout = false;
    spec->machine.multiface_red_button_disabled = false;
    spec->machine.multiface_16k_ram_mode = state.has_multiface &&
        (state.multiface_flags & SZX_MF_16K_RAM) != 0;
    spectrum_set_multiface_enabled(
        spec,
        state.has_multiface &&
            (state.multiface_flags & SZX_MF_DISABLED) == 0);
    if (state.has_multiface && spec->machine.multiface_enabled)
    {
        memcpy(
            spec->machine.multiface_ram,
            state.multiface_ram,
            sizeof(state.multiface_ram));
        spec->machine.multiface_software_lockout =
            (state.multiface_flags & SZX_MF_SOFTWARE_LOCKOUT) != 0;
        spec->machine.multiface_red_button_disabled =
            (state.multiface_flags & SZX_MF_RED_BUTTON_DISABLED) != 0;
        if ((state.multiface_flags & SZX_MF_PAGED) != 0)
        {
            _zx_multiface_page(&spec->machine);
        }
    }
    spectrum_restore_szx_cpu(&spec->machine, state.z80);
    spectrum_restore_szx_timing(&spec->machine, state.z80);
    spec->machine.border_color = state.spcr[0] & 0x07u;
    spec->machine.last_fe_out = state.spcr[3];
    beeper_set(&spec->machine.beeper, (state.spcr[3] & 0x10u) != 0);
    spectrum_discard_szx_state(&state);
    spectrum_render_frame(spec);
    return true;
}

/* Loads a `.z80` snapshot file, switches the backend model when necessary,
   then asks the chips ZX implementation to restore the machine state. */
bool spectrum_load_snapshot_z80_data(
    Spectrum *spec,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    Spectrum previous_spec = *spec;
    SpectrumModel snapshot_model;

    if (!spectrum_detect_snapshot_model_data(data, data_size, &snapshot_model))
    {
        snprintf(error_buffer, error_buffer_size, "Unsupported or corrupt .z80 snapshot.");
        return false;
    }

    if (!spectrum_snapshot_model_compatible(snapshot_model, spec->model))
    {
        if (!spectrum_rebuild_for_model(spec, snapshot_model, error_buffer, error_buffer_size))
        {
            return false;
        }
    }

    if (!zx_quickload(&spec->machine, (chips_range_t){.ptr = (void *)data, .size = data_size}))
    {
        *spec = previous_spec;
        snprintf(
            error_buffer,
            error_buffer_size,
            "Could not load .z80 snapshot into the current machine.");
        return false;
    }

    _zx_ulaplus_reset(&spec->machine);
    spectrum_render_frame(spec);
    return true;
}
