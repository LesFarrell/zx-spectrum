#include "next_machine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    NEXT_REG_MACHINE_ID = 0x00,
    NEXT_REG_RESET = 0x02,
    NEXT_REG_MACHINE_TYPE = 0x03,
    NEXT_REG_CPU_SPEED = 0x07,
    NEXT_REG_LAYER2_BASE = 0x12,
    NEXT_REG_LAYER2_SHADOW = 0x13,
    NEXT_REG_LAYER_SYSTEM = 0x15,
    NEXT_REG_GLOBAL_TRANSPARENCY = 0x14,
    NEXT_REG_PALETTE_INDEX = 0x40,
    NEXT_REG_PALETTE_VALUE_8 = 0x41,
    NEXT_REG_PALETTE_CONTROL = 0x43,
    NEXT_REG_PALETTE_VALUE_9 = 0x44,
    NEXT_REG_COPPER_DATA = 0x60,
    NEXT_REG_COPPER_CURSOR_LOW = 0x61,
    NEXT_REG_COPPER_CURSOR_HIGH = 0x62,
    NEXT_REG_LAYER2_CONTROL = 0x69,
    NEXT_REG_LAYER2_MODE = 0x70,
    NEXT_REG_ALT_ROM = 0x8C,
    NEXT_REG_ROM_MAPPING = 0x8E,
    NEXT_REG_MMU0 = 0x50,
    NEXT_REG_MMU7 = 0x57,
    NEXT_MACHINE_ID = 0x0A,
    NEXT_MACHINE_TYPE = 0xB3
};

enum
{
    NEXT_BASE_CLOCK_HZ = 3500000,
    NEXT_DEFAULT_SAMPLE_RATE = 44100,
    NEXT_DEFAULT_AUDIO_SAMPLES = 256
};

static void next_machine_render_frame_internal(
    NextMachine *machine, bool advance_copper);
static uint16_t next_classic_palette_rgb(uint8_t index);

static void next_set_error(char *buffer, size_t buffer_size, const char *message)
{
    if (buffer != NULL && buffer_size > 0)
    {
        snprintf(buffer, buffer_size, "%s", message);
    }
}

static void next_z80n_write_reg(void *user_data, uint8_t reg, uint8_t value)
{
    next_machine_write_nextreg((NextMachine *)user_data, reg, value);
}

static uint8_t *next_runtime_ram_page(NextMachine *machine, uint8_t page)
{
    return machine->ram + (size_t)page * NEXT_MMU_SLOT_SIZE;
}

static bool next_ay_held(const NextMachine *machine)
{
    return (machine->nextregs[0x06] & 0x03) == 0x03;
}

static void next_reset_ay(NextMachine *machine)
{
    for (unsigned chip = 0; chip < 3; ++chip)
        ay38910_reset(&machine->ay[chip]);
    machine->audio_ay_dirty = true;
    machine->selected_ay = 0;
    memset(machine->ay_pan, 3, sizeof(machine->ay_pan));
}

/* Volume tables from ayumi (https://github.com/true-grue/ayumi). The YM2149
   has a 32-step DAC; fixed amplitudes and the 16-step envelope use its odd
   entries. */
static const float next_ay_volumes[16] = {
    0.0f, 0.00999465934234f, 0.0144502937362f, 0.0210574502174f,
    0.0307011520562f, 0.0455481803616f, 0.0644998855573f, 0.107362478065f,
    0.126588845655f, 0.20498970016f, 0.292210269322f, 0.372838941024f,
    0.492530708782f, 0.635324635691f, 0.805584802014f, 1.0f};

static const float next_ym_volumes[32] = {
    0.0f, 0.0f, 0.00465400167849f, 0.00772106507973f,
    0.0109559777218f, 0.0139620050355f, 0.0169985503929f, 0.0200198367285f,
    0.024368657969f, 0.029694056611f, 0.0350652323186f, 0.0403906309606f,
    0.0485389486534f, 0.0583352407111f, 0.0680552376593f, 0.0777752346075f,
    0.0925154497597f, 0.111085679408f, 0.129747463188f, 0.148485542077f,
    0.17666895552f, 0.211551079576f, 0.246387426566f, 0.281101701381f,
    0.333730067903f, 0.400427252613f, 0.467383840696f, 0.53443198291f,
    0.635172045472f, 0.75800717174f, 0.879926756695f, 1.0f};

/* Output gains per stereo side. Three AYs at full volume reach about 0.9;
   the soft limiter keeps rare peaks with the beeper and DACs from clipping. */
#define NEXT_AY_GAIN 0.2f
#define NEXT_DAC_GAIN (1.0f / 512.0f)

static void next_init_audio(
    NextMachine *machine,
    int sample_rate,
    float beeper_volume,
    float ay_volume)
{
    beeper_init(&machine->beeper, &(beeper_desc_t){
                                      .tick_hz = NEXT_BASE_CLOCK_HZ,
                                      .sound_hz = sample_rate,
                                      .base_volume = beeper_volume,
                                  });
    for (unsigned chip = 0; chip < 3; ++chip)
    {
        ay38910_init(&machine->ay[chip], &(ay38910_desc_t){
                                             .type = AY38910_TYPE_8912,
                                             .tick_hz = NEXT_BASE_CLOCK_HZ / 2,
                                             .sound_hz = sample_rate,
                                             .magnitude = ay_volume,
                                         });
    }
    machine->audio_cpu_divider = 0;
    machine->ay_clock_divider = 0;
    machine->audio_sample_pos = 0;
    machine->audio_ay_left = 0.0f;
    machine->audio_ay_right = 0.0f;
    machine->audio_ay_count = 0;
    machine->audio_ay_dirty = true;
    memset(machine->audio_dc_in, 0, sizeof(machine->audio_dc_in));
    memset(machine->audio_dc_out, 0, sizeof(machine->audio_dc_out));
    machine->selected_ay = 0;
    memset(machine->ay_pan, 3, sizeof(machine->ay_pan));
    memset(machine->dac, 0x80, sizeof(machine->dac));
}

/* Adds one AY's instantaneous output to the current stereo level. Channels are
   routed ABC or ACB (NR$08 bit 5) with the middle channel split across both
   sides, or all centred in mono mode (NR$09 bits 7:5). The chip's left and
   right enables come from the Turbosound select write on port 0xFFFD. */
static void next_ay_accumulate(NextMachine *machine, unsigned chip, bool ym)
{
    const ay38910_t *ay = &machine->ay[chip];
    float level[3];
    float left;
    float right;

    for (unsigned i = 0; i < 3; ++i)
    {
        const ay38910_tone_t *tone = &ay->tone[i];
        const uint8_t amp = ay->reg[AY38910_REG_AMP_A + i];
        const unsigned step = (amp & 0x10) != 0 ? ay->env.shape_state : (unsigned)(amp & 0x0F);
        const bool audible = ((tone->bit | tone->tone_disable) &
                              ((ay->noise.rng & 1) | tone->noise_disable)) != 0;
        level[i] = audible ? (ym ? next_ym_volumes[step * 2 + 1] : next_ay_volumes[step]) : 0.0f;
    }
    if ((machine->nextregs[0x09] & (0x20 << chip)) != 0)
    {
        left = right = (level[0] + level[1] + level[2]) * 0.5f;
    }
    else if ((machine->nextregs[0x08] & 0x20) != 0)
    {
        left = level[0] + level[2] * 0.5f;
        right = level[1] + level[2] * 0.5f;
    }
    else
    {
        left = level[0] + level[1] * 0.5f;
        right = level[2] + level[1] * 0.5f;
    }
    if ((machine->ay_pan[chip] & 0x02) != 0)
        machine->audio_ay_level_left += left;
    if ((machine->ay_pan[chip] & 0x01) != 0)
        machine->audio_ay_level_right += right;
}

/* Recomputes the summed AY stereo level from all audible chips. */
static void next_ay_update_levels(NextMachine *machine, bool ym, bool turbosound)
{
    machine->audio_ay_level_left = 0.0f;
    machine->audio_ay_level_right = 0.0f;
    for (unsigned chip = 0; chip < 3; ++chip)
    {
        if (turbosound || chip == machine->selected_ay)
            next_ay_accumulate(machine, chip, ym);
    }
    machine->audio_ay_dirty = false;
}

/* Removes DC (beeper level, DAC centre) with a ~3.5 Hz one-pole high-pass. */
static float next_audio_dc_block(NextMachine *machine, unsigned side, float input)
{
    const float output = input - machine->audio_dc_in[side] +
                         0.9995f * machine->audio_dc_out[side];
    machine->audio_dc_in[side] = input;
    machine->audio_dc_out[side] = output;
    return output;
}

/* Leaves normal levels untouched and bends only peaks above 0.8 towards 1.0. */
static float next_audio_soft_limit(float sample)
{
    const float magnitude = fabsf(sample);
    float limited;

    if (magnitude <= 0.8f)
        return sample;
    limited = 0.8f + 0.2f * tanhf((magnitude - 0.8f) / 0.2f);
    return sample < 0.0f ? -limited : limited;
}

/* Runs at 3.5 MHz regardless of CPU turbo: the AYs tick at 1.75 MHz and are
   averaged between output samples, which the beeper paces at the host rate. */
static void next_audio_tick(NextMachine *machine)
{
    const bool ay_held = next_ay_held(machine);
    const bool turbosound = (machine->nextregs[0x08] & 0x02) != 0;

    if (++machine->ay_clock_divider == 2)
    {
        machine->ay_clock_divider = 0;
        if (!ay_held)
        {
            bool changed = machine->audio_ay_dirty;
            for (unsigned chip = 0; chip < 3; ++chip)
            {
                ay38910_tick(&machine->ay[chip]);
                /* Tone, noise and envelope only step every 8 AY ticks. */
                changed |= (machine->ay[chip].tick & 7) == 0;
            }
            if (changed)
            {
                next_ay_update_levels(machine,
                                      (machine->nextregs[0x06] & 0x03) == 0x00,
                                      turbosound);
            }
            machine->audio_ay_left += machine->audio_ay_level_left;
            machine->audio_ay_right += machine->audio_ay_level_right;
            ++machine->audio_ay_count;
        }
    }
    if (beeper_tick(&machine->beeper))
    {
        float left = 0.0f;
        float right = 0.0f;
        const bool beeper_muted =
            (machine->nextregs[0x06] & 0x40) != 0 &&
            (machine->nextregs[0x08] & 0x10) != 0;

        if (!beeper_muted)
        {
            left += machine->beeper.sample;
            right += machine->beeper.sample;
        }
        if (machine->audio_ay_count > 0)
        {
            const float scale = NEXT_AY_GAIN / (float)machine->audio_ay_count;
            left += machine->audio_ay_left * scale;
            right += machine->audio_ay_right * scale;
        }
        machine->audio_ay_left = 0.0f;
        machine->audio_ay_right = 0.0f;
        machine->audio_ay_count = 0;
        if ((machine->nextregs[0x08] & 0x08) != 0)
        {
            /* DACs A and B feed the left channel, C and D the right. */
            left += (float)((int)machine->dac[0] + (int)machine->dac[1] - 256) * NEXT_DAC_GAIN;
            right += (float)((int)machine->dac[2] + (int)machine->dac[3] - 256) * NEXT_DAC_GAIN;
        }
        left = next_audio_soft_limit(next_audio_dc_block(machine, 0, left));
        right = next_audio_soft_limit(next_audio_dc_block(machine, 1, right));
        machine->audio_samples[machine->audio_sample_pos * 2] = left;
        machine->audio_samples[machine->audio_sample_pos * 2 + 1] = right;
        if (++machine->audio_sample_pos == machine->audio_num_samples)
        {
            if (machine->audio_callback.func != NULL)
            {
                machine->audio_callback.func(
                    machine->audio_samples,
                    machine->audio_num_samples,
                    machine->audio_callback.user_data);
            }
            machine->audio_sample_pos = 0;
        }
    }
}

/* Advances the audio by a number of CPU clock cycles at the current speed,
   including cycles the CPU spends stalled by the DMA. */
static void next_audio_advance(NextMachine *machine, uint64_t cycles)
{
    while (cycles-- > 0)
    {
        if (++machine->audio_cpu_divider >= machine->cpu_speed_multiplier)
        {
            machine->audio_cpu_divider = 0;
            next_audio_tick(machine);
        }
    }
}

static const uint8_t *next_runtime_ram_page_const(
    const NextMachine *machine,
    uint8_t page)
{
    return machine->ram + (size_t)page * NEXT_MMU_SLOT_SIZE;
}

/* The Multiface starts hidden: it only answers its ports after an NMI. */
static void next_multiface_reset(NextMachine *machine)
{
    machine->multiface_paged = false;
    machine->multiface_nmi_active = false;
    machine->multiface_invisible = true;
    machine->nmi_request = false;
    machine->last_m1_ed_prefix = false;
    machine->nmi_stackless_retn = false;
    machine->nmi_retn_reads = 0;
    machine->nmi_write_count = 0;
}

/* Called on the first opcode fetch of the NMI handler. The acknowledge
   cycle has just pushed the return address; it is always mirrored to
   NR$C2/C3, and in stackless mode (NR$C0 bit 3) the stack bytes are
   restored so the NMI leaves memory untouched. */
static void next_nmi_acknowledged(NextMachine *machine)
{
    const uint16_t sp = machine->cpu.sp;

    machine->nextregs[0xC2] = next_machine_read_memory(machine, sp);
    machine->nextregs[0xC3] = next_machine_read_memory(machine, (uint16_t)(sp + 1));
    if ((machine->nextregs[0xC0] & 0x08) != 0)
    {
        if (machine->nmi_write_count == 2 &&
            machine->nmi_write_address[0] == (uint16_t)(sp + 1) &&
            machine->nmi_write_address[1] == sp)
        {
            next_machine_write_memory(machine, sp, machine->nmi_write_old_value[1]);
            next_machine_write_memory(machine, (uint16_t)(sp + 1),
                                      machine->nmi_write_old_value[0]);
        }
        machine->nmi_stackless_retn = true;
    }
    machine->nmi_write_count = 0;
}

static void next_soft_reset(NextMachine *machine)
{
    const uint16_t af = machine->cpu.af;
    const uint16_t bc = machine->cpu.bc;
    const uint16_t de = machine->cpu.de;
    const uint16_t hl = machine->cpu.hl;
    const uint16_t af2 = machine->cpu.af2;
    const uint16_t bc2 = machine->cpu.bc2;
    const uint16_t de2 = machine->cpu.de2;
    const uint16_t hl2 = machine->cpu.hl2;
    const uint16_t sp = machine->cpu.sp;
    const uint16_t ix = machine->cpu.ix;
    const uint16_t iy = machine->cpu.iy;

    machine->initial_pc = 0;
    machine->initial_sp = 0;
    machine->int_counter = 0;
    machine->rom_bank = 0;
    machine->port_7ffd = 0;
    machine->port_1ffd = 0;
    machine->port_dffd = 0;
    machine->timex_video_mode = 0;
    machine->mmu_override_mask = 0;
    machine->paging_locked = false;
    machine->divmmc_automap = false;
    machine->divmmc_automap_pending = false;
    next_multiface_reset(machine);
    {
        static const uint8_t reset_mmu[NEXT_MMU_SLOT_COUNT] = {
            0xFF, 0xFF, 10, 11, 4, 5, 0, 1};
        memcpy(machine->mmu, reset_mmu, sizeof(reset_mmu));
        for (unsigned slot = 0; slot < NEXT_MMU_SLOT_COUNT; ++slot)
        {
            machine->nextregs[NEXT_REG_MMU0 + slot] = reset_mmu[slot];
        }
    }
    machine->cpu_pins = z80_reset(&machine->cpu);
    machine->cpu.af = af;
    machine->cpu.bc = bc;
    machine->cpu.de = de;
    machine->cpu.hl = hl;
    machine->cpu.af2 = af2;
    machine->cpu.bc2 = bc2;
    machine->cpu.de2 = de2;
    machine->cpu.hl2 = hl2;
    machine->cpu.sp = sp;
    machine->cpu.ix = ix;
    machine->cpu.iy = iy;
    z80_set_z80n_enabled(&machine->cpu, true);
    z80_set_z80n_nextreg_callback(&machine->cpu,
                                  next_z80n_write_reg, machine);
}

static void next_init_keyboard(NextMachine *machine)
{
    static const char keymap[] =
        " zxcv"
        "asdfg"
        "qwert"
        "12345"
        "09876"
        "poiuy"
        " lkjh"
        "  mnb"
        " ZXCV"
        "ASDFG"
        "QWERT"
        "     "
        "     "
        "POIUY"
        " LKJH"
        "  MNB"
        " : ?/"
        "     "
        "   <>"
        "!@#$%"
        "_)('&"
        "\";   "
        " =+-^"
        "  .,*";

    kbd_init(&machine->keyboard, 1);
    kbd_register_modifier(&machine->keyboard, 0, 0, 0);
    kbd_register_modifier(&machine->keyboard, 1, 7, 1);
    kbd_register_key(&machine->keyboard, 1, 0, 0, 0);
    kbd_register_key(&machine->keyboard, 2, 7, 1, 0);
    for (int layer = 0; layer < 3; ++layer)
    {
        for (int column = 0; column < 8; ++column)
        {
            for (int line = 0; line < 5; ++line)
            {
                const uint8_t key = keymap[layer * 40 + column * 5 + line];
                if (key != ' ')
                {
                    kbd_register_key(&machine->keyboard, key, column, line,
                                     layer > 0 ? 1 << (layer - 1) : 0);
                }
            }
        }
    }
    kbd_register_key(&machine->keyboard, ' ', 7, 0, 0);
    kbd_register_key(&machine->keyboard, 0x0F, 7, 1, 0);
    kbd_register_key(&machine->keyboard, 0x08, 3, 4, 1);
    kbd_register_key(&machine->keyboard, 0x0A, 4, 4, 1);
    kbd_register_key(&machine->keyboard, 0x0B, 4, 3, 1);
    kbd_register_key(&machine->keyboard, 0x09, 4, 2, 1);
    kbd_register_key(&machine->keyboard, 0x07, 3, 0, 1);
    kbd_register_key(&machine->keyboard, 0x0C, 4, 0, 1);
    kbd_register_key(&machine->keyboard, 0x0D, 6, 0, 0);
}

static uint16_t next_palette_from_8bit(uint8_t value)
{
    return (uint16_t)(((uint16_t)value << 1) | ((value & 0x03) != 0));
}

static uint32_t next_palette_to_rgba(uint16_t color)
{
    const uint8_t red = (uint8_t)((color >> 6) & 7);
    const uint8_t green = (uint8_t)((color >> 3) & 7);
    const uint8_t blue = (uint8_t)(color & 7);
    const uint8_t r8 = (uint8_t)((red << 5) | (red << 2) | (red >> 1));
    const uint8_t g8 = (uint8_t)((green << 5) | (green << 2) | (green >> 1));
    const uint8_t b8 = (uint8_t)((blue << 5) | (blue << 2) | (blue >> 1));
    return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

static void next_reset_palettes(NextMachine *machine)
{
    memset(machine->hw_palette_priority, 0,
           sizeof(machine->hw_palette_priority));
    for (unsigned palette = 0; palette < 8; ++palette)
    {
        for (unsigned index = 0; index < 256; ++index)
        {
            machine->hw_palette[palette][index] = next_palette_from_8bit((uint8_t)index);
        }
    }
    /* The ULA palettes start with the Spectrum colours: INKs at 0-15 and
       PAPERs at 16-31. */
    for (unsigned index = 0; index < 32; ++index)
    {
        machine->hw_palette[0][index] = next_classic_palette_rgb((uint8_t)(index & 0x0F));
        machine->hw_palette[4][index] = next_classic_palette_rgb((uint8_t)(index & 0x0F));
    }
    machine->palette_index = 0;
    machine->palette_latch = 0;
    machine->palette_second_write = false;
}

static void next_palette_advance(NextMachine *machine)
{
    if ((machine->nextregs[NEXT_REG_PALETTE_CONTROL] & 0x80) == 0)
    {
        ++machine->palette_index;
        machine->nextregs[NEXT_REG_PALETTE_INDEX] = machine->palette_index;
    }
}

static uint16_t next_read_u16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t next_read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool next_add_size(size_t *value, size_t amount, size_t limit)
{
    if (*value > limit || amount > limit - *value)
        return false;
    *value += amount;
    return true;
}

static bool next_nex_version(const uint8_t *header, NextNexVersion *version)
{
    if (memcmp(header + 4, "V1.0", 4) == 0)
        *version = NEXT_NEX_VERSION_1_0;
    else if (memcmp(header + 4, "V1.1", 4) == 0)
        *version = NEXT_NEX_VERSION_1_1;
    else if (memcmp(header + 4, "V1.2", 4) == 0)
        *version = NEXT_NEX_VERSION_1_2;
    else if (memcmp(header + 4, "V1.3", 4) == 0)
        *version = NEXT_NEX_VERSION_1_3;
    else
        return false;
    return true;
}

static unsigned next_nex_flagged_bank_count(const uint8_t *header)
{
    unsigned count = 0;
    for (unsigned bank = 0; bank < NEXT_NEX_BANK_COUNT; ++bank)
    {
        if (header[18 + bank] != 0)
            ++count;
    }
    return count;
}

static size_t next_nex_screen_size(
    const uint8_t *header,
    NextNexVersion version,
    bool *has_palette,
    bool *has_copper)
{
    const uint8_t flags = header[10];
    const uint8_t flags2 = version == NEXT_NEX_VERSION_1_3 ? header[152] : 0;
    size_t size = 0;

    *has_palette = false;
    *has_copper = version == NEXT_NEX_VERSION_1_3 && header[153] != 0;

    if ((flags & 0x01) != 0)
        size += 49152;
    if ((flags & 0x02) != 0)
        size += 6912;
    if ((flags & 0x04) != 0)
        size += 12288;
    if ((flags & 0x08) != 0)
        size += 12288;
    if ((flags & 0x10) != 0)
        size += 12288;

    if (version == NEXT_NEX_VERSION_1_3 && (flags & 0x40) != 0)
    {
        if (flags2 == 1 || flags2 == 2)
            size += 81920;
    }

    /* A palette block accompanies any loading screen unless bit 7
       explicitly suppresses it. */
    *has_palette = flags != 0 && (flags & 0x80) == 0;
    return size;
}

static void next_copy_timex_loading_screen(
    NextMachine *machine, const uint8_t *data)
{
    /* 12288-byte LoRes/HiRes/HiColour screens carry two 6K bitmap halves:
       the first at bank 5 offset 0x0000 (address 0x4000) and the second at
       offset 0x2000 (address 0x6000), leaving the attribute area between. */
    memcpy(machine->ram + 5 * NEXT_NEX_BANK_SIZE, data, 0x1800);
    memcpy(machine->ram + 5 * NEXT_NEX_BANK_SIZE + 0x2000,
           data + 0x1800, 0x1800);
}

static void next_copy_loading_screens(
    NextMachine *machine,
    const uint8_t *data,
    size_t *offset,
    const uint8_t *header,
    NextNexVersion version)
{
    const uint8_t flags = header[10];
    const uint8_t flags2 = version == NEXT_NEX_VERSION_1_3 ? header[152] : 0;

    if ((flags & 0x01) != 0)
    {
        memcpy(machine->ram + 9 * NEXT_NEX_BANK_SIZE, data + *offset, 49152);
        *offset += 49152;
    }
    if ((flags & 0x02) != 0)
    {
        memcpy(machine->ram + 5 * NEXT_NEX_BANK_SIZE, data + *offset, 6912);
        *offset += 6912;
    }
    if ((flags & 0x04) != 0)
    {
        next_copy_timex_loading_screen(machine, data + *offset);
        *offset += 12288;
    }
    if ((flags & 0x08) != 0)
    {
        next_copy_timex_loading_screen(machine, data + *offset);
        *offset += 12288;
    }
    if ((flags & 0x10) != 0)
    {
        next_copy_timex_loading_screen(machine, data + *offset);
        *offset += 12288;
    }
    if (version == NEXT_NEX_VERSION_1_3 && (flags & 0x40) != 0 &&
        (flags2 == 1 || flags2 == 2))
    {
        memcpy(machine->ram + 9 * NEXT_NEX_BANK_SIZE, data + *offset, 81920);
        *offset += 81920;
    }
}

bool next_machine_init(
    NextMachine *machine,
    char *error_buffer,
    size_t error_buffer_size)
{
    if (machine == NULL)
    {
        next_set_error(error_buffer, error_buffer_size, "Invalid Next machine state.");
        return false;
    }

    memset(machine, 0, sizeof(*machine));
    machine->ram = (uint8_t *)calloc(1, NEXT_RAM_SIZE);
    if (machine->ram == NULL)
    {
        next_set_error(error_buffer, error_buffer_size, "Unable to allocate 2 MB of Next RAM.");
        return false;
    }
    machine->framebuffer = (uint32_t *)calloc(
        NEXT_DISPLAY_MAX_WIDTH * NEXT_DISPLAY_HEIGHT, sizeof(uint32_t));
    if (machine->framebuffer == NULL)
    {
        free(machine->ram);
        machine->ram = NULL;
        next_set_error(error_buffer, error_buffer_size, "Unable to allocate the Next framebuffer.");
        return false;
    }

    machine->initialized = true;
    next_sd_init(&machine->sd_card);
    next_rtc_init(&machine->rtc);
    next_dma_init(&machine->dma);
    next_copper_init(&machine->copper_state);
    next_sprite_init(&machine->sprites);
    machine->cpu_pins = z80_init(&machine->cpu);
    machine->audio_num_samples = NEXT_DEFAULT_AUDIO_SAMPLES;
    next_init_audio(machine, NEXT_DEFAULT_SAMPLE_RATE, 0.35f, 0.14f);
    next_init_keyboard(machine);
    next_machine_reset(machine);
    return true;
}

void next_machine_configure_audio(
    NextMachine *machine,
    chips_audio_callback_t callback,
    int sample_rate,
    int num_samples,
    float beeper_volume,
    float ay_volume)
{
    if (machine == NULL || !machine->initialized)
        return;
    if (sample_rate <= 0)
        sample_rate = NEXT_DEFAULT_SAMPLE_RATE;
    if (num_samples <= 0 || num_samples > NEXT_AUDIO_MAX_SAMPLES)
        num_samples = NEXT_DEFAULT_AUDIO_SAMPLES;
    if (num_samples > NEXT_AUDIO_MAX_SAMPLES / 2)
        num_samples = NEXT_AUDIO_MAX_SAMPLES / 2;
    machine->audio_callback = callback;
    machine->audio_num_samples = num_samples;
    next_init_audio(machine, sample_rate, beeper_volume, ay_volume);
}

void next_machine_discard(NextMachine *machine)
{
    if (machine == NULL)
        return;
    next_sd_close(&machine->sd_card);
    free(machine->ram);
    free(machine->framebuffer);
    memset(machine, 0, sizeof(*machine));
}

bool next_machine_open_sd_image(
    NextMachine *machine,
    const char *path,
    bool writable,
    char *error_buffer,
    size_t error_buffer_size)
{
    if (machine == NULL || !machine->initialized)
    {
        next_set_error(error_buffer, error_buffer_size, "Invalid Next machine state.");
        return false;
    }
    return next_sd_open_image(&machine->sd_card, path, writable,
                              error_buffer, error_buffer_size);
}

void next_machine_reset(NextMachine *machine)
{
    static const uint8_t default_mmu[NEXT_MMU_SLOT_COUNT] = {
        0xFF, 0xFF, 10, 11, 4, 5, 0, 1};

    if (machine == NULL || !machine->initialized)
        return;
    memset(machine->nextregs, 0, sizeof(machine->nextregs));
    memcpy(machine->mmu, default_mmu, sizeof(machine->mmu));
    machine->selected_nextreg = 0;
    machine->nextregs[NEXT_REG_MACHINE_ID] = NEXT_MACHINE_ID;
    machine->nextregs[0x01] = 0x32;
    machine->nextregs[NEXT_REG_MACHINE_TYPE] =
        (uint8_t)((NEXT_MACHINE_TYPE & 0xF8) | 0x02);
    machine->reset_type = 0x04;
    machine->nextregs[0x02] = 0x00;
    machine->rom_bank = 0;
    machine->port_7ffd = 0;
    machine->port_1ffd = 0;
    machine->port_dffd = 0;
    machine->timex_video_mode = 0;
    machine->mmu_override_mask = 0;
    machine->paging_locked = false;
    machine->nextregs[0x05] = 0x01;
    machine->nextregs[0x06] = 0xA0;
    machine->nextregs[0x08] = 0x10;
    machine->nextregs[0x0A] = 0x10;
    machine->nextregs[0x0E] = 0x03;
    machine->nextregs[0x0F] = 0x01;
    machine->nextregs[NEXT_REG_LAYER2_BASE] = 0x08;
    machine->nextregs[0x13] = 0x0B;
    machine->nextregs[NEXT_REG_GLOBAL_TRANSPARENCY] = 0xE3;
    machine->nextregs[0x42] = 0x07;
    machine->nextregs[0x4A] = 0xE3;
    machine->nextregs[0x4B] = 0xE3;
    machine->nextregs[0x4C] = 0x0F;
    machine->nextregs[0x6E] = 0x2C;
    machine->nextregs[0x6F] = 0x0C;
    machine->nextregs[0x98] = 0xFF;
    machine->nextregs[0x99] = 0x01;
    machine->nextregs[0xB8] = 0x83;
    machine->nextregs[0xBB] = 0xCD;
    machine->nextregs[0xC4] = 0x81;
    for (unsigned slot = 0; slot < NEXT_MMU_SLOT_COUNT; ++slot)
    {
        machine->nextregs[NEXT_REG_MMU0 + slot] = machine->mmu[slot];
    }
    machine->initial_pc = 0;
    machine->initial_sp = 0;
    machine->ticks = 0;
    machine->frame_count = 0;
    machine->frame_tstate = 0;
    machine->int_counter = 0;
    machine->border_color = 0;
    machine->layer2_control = 0;
    machine->layer2_map_control = 0;
    machine->layer2_map_offset = 0;
    machine->config_mode = true;
    machine->config_page = 0;
    machine->divmmc_automap = false;
    machine->divmmc_automap_pending = false;
    machine->divmmc_control = 0;
    next_multiface_reset(machine);
    machine->framebuffer_width = 320;
    machine->cpu_speed_multiplier = 1 << (machine->nextregs[NEXT_REG_CPU_SPEED] & 3);
    next_dma_reset(&machine->dma);
    next_copper_reset(&machine->copper_state);
    machine->copper_debt = 0;
    next_sprite_reset(&machine->sprites);
    beeper_reset(&machine->beeper);
    next_reset_ay(machine);
    machine->audio_cpu_divider = 0;
    machine->ay_clock_divider = 0;
    machine->audio_sample_pos = 0;
    memset(machine->dac, 0x80, sizeof(machine->dac));
    next_reset_palettes(machine);
    machine->cpu_pins = z80_reset(&machine->cpu);
    z80_set_z80n_enabled(&machine->cpu, true);
    z80_set_z80n_nextreg_callback(&machine->cpu, next_z80n_write_reg, machine);
    machine->palette_loaded = false;
    machine->copper_loaded = false;
    machine->sprite_clip[0] = 0x00;
    machine->sprite_clip[1] = 0xFF;
    machine->sprite_clip[2] = 0x00;
    machine->sprite_clip[3] = 0xBF;
    machine->sprite_clip_index = 0;
    next_sprite_set_clip(&machine->sprites, 0x00, 0xFF, 0x00, 0xBF);
    machine->layer2_clip[0] = 0x00;
    machine->layer2_clip[1] = 0xFF;
    machine->layer2_clip[2] = 0x00;
    machine->layer2_clip[3] = 0xBF;
    machine->layer2_clip_index = 0;
    machine->ula_clip[0] = 0x00;
    machine->ula_clip[1] = 0xFF;
    machine->ula_clip[2] = 0x00;
    machine->ula_clip[3] = 0xBF;
    machine->ula_clip_index = 0;
    machine->tilemap_scroll_x = 0;
    machine->tilemap_scroll_y = 0;
    machine->tilemap_clip[0] = 0x00;
    machine->tilemap_clip[1] = 0x9F;
    machine->tilemap_clip[2] = 0x00;
    machine->tilemap_clip[3] = 0xFF;
    machine->tilemap_clip_index = 0;
    machine->loader_active = machine->loader_rom_loaded;
}

bool next_machine_load_rom_data(
    NextMachine *machine,
    const uint8_t *rom,
    size_t rom_size,
    const uint8_t *boot_rom,
    size_t boot_rom_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    if (machine == NULL || !machine->initialized || rom == NULL || boot_rom == NULL)
    {
        next_set_error(error_buffer, error_buffer_size, "Invalid Next ROM data.");
        return false;
    }
    if (rom_size != NEXT_ROM_SIZE || boot_rom_size != NEXT_BOOT_ROM_SIZE)
    {
        next_set_error(error_buffer, error_buffer_size,
                       "Next ROMs must be exactly 64 KB and 8 KB.");
        return false;
    }
    memcpy(machine->rom, rom, NEXT_ROM_SIZE);
    memcpy(machine->divmmc_rom, boot_rom, NEXT_BOOT_ROM_SIZE);
    machine->rom_loaded = true;
    machine->divmmc_rom_loaded = true;
    return true;
}

bool next_machine_load_loader_rom_data(
    NextMachine *machine,
    const uint8_t *loader_rom,
    size_t loader_rom_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    if (machine == NULL || !machine->initialized || loader_rom == NULL ||
        loader_rom_size != NEXT_BOOT_ROM_SIZE)
    {
        next_set_error(error_buffer, error_buffer_size,
                       "The Next FPGA loader ROM must be exactly 8 KB.");
        return false;
    }
    memcpy(machine->loader_rom, loader_rom, NEXT_BOOT_ROM_SIZE);
    machine->loader_rom_loaded = true;
    machine->loader_active = true;
    return true;
}

static bool next_layer2_redirect(const NextMachine *machine, uint16_t address,
                                 uint8_t *page, size_t *offset)
{
    const unsigned region = address >> 14;
    const unsigned segment = (machine->layer2_map_control >> 6) & 3;
    unsigned base;
    unsigned bank;

    if (region == 3 || (region != 0 && segment != 3))
        return false;
    base = (machine->layer2_map_control & 0x08) != 0
               ? machine->nextregs[NEXT_REG_LAYER2_SHADOW]
               : machine->nextregs[NEXT_REG_LAYER2_BASE];
    bank = (base & 0x7F) + (segment == 3 ? region : segment) +
           machine->layer2_map_offset;
    if (bank >= 128)
        return false;
    *page = (uint8_t)(bank * 2 + ((address & 0x3FFF) >= NEXT_MMU_SLOT_SIZE));
    *offset = address & (NEXT_MMU_SLOT_SIZE - 1);
    return true;
}

uint8_t next_machine_read_memory(const NextMachine *machine, uint16_t address)
{
    const unsigned slot = address >> 13;
    const uint8_t page = machine->mmu[slot];
    const size_t offset = address & (NEXT_MMU_SLOT_SIZE - 1);

    /* Above 16K only Layer 2's 48K read mapping can override the MMU. */
    if (address >= 0x4000 &&
        (address >= 0xC000 || (machine->layer2_map_control & 0xC4) != 0xC4))
    {
        return next_runtime_ram_page_const(machine, page)[offset];
    }

    if (machine->loader_active && slot == 0 && machine->loader_rom_loaded)
    {
        return machine->loader_rom[offset];
    }
    /* A paged Multiface overrides divMMC and the ROM: ROM at 0x0000-0x1FFF,
       RAM at 0x2000-0x3FFF. */
    if (machine->multiface_paged && address < 0x4000)
    {
        return machine->multiface_mem[address];
    }
    if (!machine->loader_active &&
        ((machine->divmmc_control & 0x80) != 0 || machine->divmmc_automap) &&
        address < 0x4000)
    {
        if (address < 0x2000)
        {
            if ((machine->divmmc_control & 0x40) != 0)
            {
                return machine->divmmc_ram[3 * NEXT_BOOT_ROM_SIZE + offset];
            }
            return machine->divmmc_rom[offset];
        }
        return machine->divmmc_ram[(size_t)(machine->divmmc_control & 0x0F) * NEXT_BOOT_ROM_SIZE + offset];
    }
    if (machine->config_mode && address < 0x4000)
    {
        const size_t config_offset = address & 0x3FFF;
        if (machine->config_page <= 3)
        {
            return machine->rom[(size_t)machine->config_page * 0x4000 + config_offset];
        }
        if (machine->config_page == 4)
        {
            return config_offset < NEXT_BOOT_ROM_SIZE
                       ? machine->divmmc_rom[config_offset]
                       : 0xFF;
        }
        if (machine->config_page == 5)
        {
            return machine->multiface_mem[config_offset];
        }
        if (machine->config_page == 6 || machine->config_page == 7)
        {
            return machine->alt_rom[machine->config_page - 6][config_offset];
        }
        if (machine->config_page >= 8 && machine->config_page <= 0x0F)
        {
            return machine->divmmc_ram[(size_t)(machine->config_page - 8) * 0x4000 + config_offset];
        }
        if (machine->config_page >= 0x10 && machine->config_page <= 0x7F)
        {
            /* SRAM bank 16 is Next RAM bank 0, after the ROMs and divMMC RAM. */
            return machine->ram[(size_t)(machine->config_page - 0x10) * 0x4000 + config_offset];
        }
    }
    if ((machine->layer2_map_control & 0x04) != 0)
    {
        uint8_t layer2_page;
        size_t layer2_offset;
        if (next_layer2_redirect(machine, address, &layer2_page, &layer2_offset))
            return next_runtime_ram_page_const(machine, layer2_page)[layer2_offset];
    }
    if (address < 0x4000 && page == 0xFF &&
        (machine->nextregs[NEXT_REG_ALT_ROM] & 0xC0) == 0x80)
    {
        unsigned bank = machine->rom_bank & 1;
        if ((machine->nextregs[NEXT_REG_ALT_ROM] & 0x20) != 0)
            bank = 1;
        else if ((machine->nextregs[NEXT_REG_ALT_ROM] & 0x10) != 0)
            bank = 0;
        return machine->alt_rom[bank][address];
    }
    if (page == 0xFF && slot < 2 && machine->rom_loaded)
    {
        return machine->rom[(size_t)machine->rom_bank * NEXT_NEX_BANK_SIZE + address];
    }
    return next_runtime_ram_page_const(machine, page)[offset];
}

void next_machine_write_memory(NextMachine *machine, uint16_t address, uint8_t value)
{
    const unsigned slot = address >> 13;
    const uint8_t page = machine->mmu[slot];
    const size_t offset = address & (NEXT_MMU_SLOT_SIZE - 1);

    /* Above 16K only Layer 2's 48K write mapping can override the MMU. */
    if (address >= 0x4000 &&
        (address >= 0xC000 || (machine->layer2_map_control & 0xC1) != 0xC1))
    {
        next_runtime_ram_page(machine, page)[offset] = value;
        return;
    }

    if (machine->multiface_paged && address < 0x4000)
    {
        if (address >= 0x2000)
            machine->multiface_mem[address] = value;
        return;
    }
    if (!machine->loader_active &&
        ((machine->divmmc_control & 0x80) != 0 || machine->divmmc_automap) &&
        address < 0x4000)
    {
        if (address >= 0x2000)
        {
            machine->divmmc_ram[(size_t)(machine->divmmc_control & 0x0F) * NEXT_BOOT_ROM_SIZE + offset] = value;
        }
        return;
    }

    if (machine->config_mode && address < 0x4000)
    {
        const size_t config_offset = address & 0x3FFF;
        if (machine->config_page <= 3)
        {
            machine->rom[(size_t)machine->config_page * 0x4000 + config_offset] = value;
        }
        else if (machine->config_page == 4 && config_offset < NEXT_BOOT_ROM_SIZE)
        {
            machine->divmmc_rom[config_offset] = value;
        }
        else if (machine->config_page == 5)
        {
            machine->multiface_mem[config_offset] = value;
        }
        else if (machine->config_page == 6 || machine->config_page == 7)
        {
            machine->alt_rom[machine->config_page - 6][config_offset] = value;
        }
        else if (machine->config_page >= 8 && machine->config_page <= 0x0F)
        {
            machine->divmmc_ram[(size_t)(machine->config_page - 8) * 0x4000 + config_offset] = value;
        }
        else if (machine->config_page >= 0x10 && machine->config_page <= 0x7F)
        {
            machine->ram[(size_t)(machine->config_page - 0x10) * 0x4000 +
                         config_offset] = value;
        }
        return;
    }

    if ((machine->layer2_map_control & 0x01) != 0)
    {
        uint8_t layer2_page;
        size_t layer2_offset;
        if (next_layer2_redirect(machine, address, &layer2_page, &layer2_offset))
        {
            next_runtime_ram_page(machine, layer2_page)[layer2_offset] = value;
            return;
        }
    }

    if (address < 0x4000 && page == 0xFF &&
        (machine->nextregs[NEXT_REG_ALT_ROM] & 0xC0) == 0xC0)
    {
        unsigned bank = machine->rom_bank & 1;
        if ((machine->nextregs[NEXT_REG_ALT_ROM] & 0x20) != 0)
            bank = 1;
        else if ((machine->nextregs[NEXT_REG_ALT_ROM] & 0x10) != 0)
            bank = 0;
        machine->alt_rom[bank][address] = value;
        return;
    }
    if (page == 0xFF && slot < 2)
        return;
    next_runtime_ram_page(machine, page)[offset] = value;
}

static void next_set_rom_bank(NextMachine *machine, uint8_t bank)
{
    machine->rom_bank = bank;
}

uint8_t next_machine_read_nextreg(const NextMachine *machine, uint8_t reg)
{
    if (reg == 0x08)
    {
        return (uint8_t)((machine->nextregs[0x08] & 0x7F) |
                         (machine->paging_locked ? 0x00 : 0x80));
    }
    if (reg == NEXT_REG_PALETTE_INDEX)
    {
        return machine->palette_index;
    }
    if (reg == NEXT_REG_PALETTE_VALUE_8)
    {
        const unsigned palette =
            (machine->nextregs[NEXT_REG_PALETTE_CONTROL] >> 4) & 7;
        return (uint8_t)(machine->hw_palette[palette][machine->palette_index] >> 1);
    }
    if (reg == NEXT_REG_PALETTE_VALUE_9)
    {
        const unsigned palette =
            (machine->nextregs[NEXT_REG_PALETTE_CONTROL] >> 4) & 7;
        return (uint8_t)(machine->hw_palette[palette][machine->palette_index] & 1);
    }
    if (reg >= NEXT_REG_MMU0 && reg <= NEXT_REG_MMU7)
    {
        return machine->mmu[reg - NEXT_REG_MMU0];
    }
    if (reg == NEXT_REG_LAYER2_CONTROL)
    {
        return (uint8_t)(((machine->layer2_control & 0x02) != 0 ? 0x80 : 0) |
                         ((machine->port_7ffd & 0x08) != 0 ? 0x40 : 0) |
                         (machine->timex_video_mode & 0x3F));
    }
    if (reg == NEXT_REG_ROM_MAPPING)
    {
        return (uint8_t)(((machine->port_dffd & 1) << 7) |
                         ((machine->port_7ffd & 7) << 4) | 0x08 |
                         ((machine->port_1ffd & 1) << 2) |
                         ((machine->port_1ffd & 4) >> 1) |
                         ((machine->port_7ffd & 0x10) >> 4));
    }
    if (reg == NEXT_REG_COPPER_CURSOR_LOW)
    {
        return (uint8_t)(machine->copper_state.write_ptr & 0xFF);
    }
    if (reg == NEXT_REG_COPPER_CURSOR_HIGH)
    {
        return (uint8_t)(((machine->copper_state.write_ptr >> 8) & 0x07) |
                         ((machine->copper_state.mode & 0x03) << 6));
    }
    if (reg == 0x1E || reg == 0x1F)
    {
        const uint32_t speed = machine->nextregs[NEXT_REG_CPU_SPEED] & 3;
        const uint32_t row = machine->frame_tstate / (NEXT_LINE_TSTATES << speed);
        const uint32_t line = (row + NEXT_FRAME_LINES - 32 +
                               machine->nextregs[0x64]) %
                              NEXT_FRAME_LINES;
        return reg == 0x1E ? (uint8_t)(line >> 8) : (uint8_t)(line & 0xFF);
    }
    if (reg == 0x19)
    {
        return machine->sprite_clip[machine->sprite_clip_index & 3];
    }
    if (reg == 0x18)
    {
        return machine->layer2_clip[machine->layer2_clip_index & 3];
    }
    if (reg == 0x1A)
    {
        return machine->ula_clip[machine->ula_clip_index & 3];
    }
    if (reg == 0x1B)
    {
        return machine->tilemap_clip[machine->tilemap_clip_index & 3];
    }
    if (reg == 0x1C)
    {
        return (uint8_t)(((machine->tilemap_clip_index & 3) << 6) |
                         ((machine->ula_clip_index & 3) << 4) |
                         ((machine->sprite_clip_index & 3) << 2) |
                         (machine->layer2_clip_index & 3));
    }
    return machine->nextregs[reg];
}

/* CPU-cycle counters are expressed at the active Z80 clock. Preserve their
   position within the video/audio interval when software changes turbo speed
   part-way through a frame. */
static void next_set_cpu_speed(NextMachine *machine, uint8_t value)
{
    const uint32_t old_multiplier =
        machine->cpu_speed_multiplier > 0
            ? (uint32_t)machine->cpu_speed_multiplier
            : 1u;
    const uint32_t new_multiplier = 1u << (value & 0x03);

    if (new_multiplier != old_multiplier)
    {
        machine->frame_tstate = (uint32_t)((uint64_t)machine->frame_tstate * new_multiplier / old_multiplier);
        if (machine->int_counter > 0)
        {
            machine->int_counter = (int)(((uint64_t)(unsigned)machine->int_counter * new_multiplier +
                                          old_multiplier - 1u) /
                                         old_multiplier);
        }
        machine->audio_cpu_divider = (int)((uint64_t)(unsigned)machine->audio_cpu_divider * new_multiplier /
                                           old_multiplier);
    }
    machine->nextregs[NEXT_REG_CPU_SPEED] = value & 0x03;
    machine->cpu_speed_multiplier = (int)new_multiplier;
}

void next_machine_write_nextreg(NextMachine *machine, uint8_t reg, uint8_t value)
{
    /* The FPGA core-version bytes are read-only.  Nextoid probes NR$00 by
       writing it, however, so preserve that compatibility behaviour while
       protecting the version it subsequently checks. */
    if (reg == 0x00 || reg == 0x01 || reg == 0x0E)
        return;
    if (reg == NEXT_REG_RESET)
    {
        if ((value & 0x02) != 0)
        {
            next_machine_reset(machine);
        }
        else if ((value & 0x01) != 0)
        {
            const uint8_t previous = machine->reset_type;
            /* The reset type is a three-bit shift register: the soft
               reset slides the history one step (100 -> 010 -> 001 ->
               000), which NextZXOS reads back to tell its first
               staging pass from the post-staging pass. */
            machine->reset_type = (uint8_t)(previous >> 1);
            machine->nextregs[reg] =
                (uint8_t)((value & 0x80) | (machine->reset_type & 0x03));
            {
                const uint8_t defaults = machine->nextregs[NEXT_REG_ALT_ROM] & 0x0F;
                machine->nextregs[NEXT_REG_ALT_ROM] =
                    (uint8_t)(defaults | (defaults << 4));
            }
            /* A firmware handoff is a CPU reset.  The configured NextRegs,
               MMU and cleared FPGA-loader mask survive it. */
            next_sd_write_chip_select(&machine->sd_card, 0xFF);
            next_soft_reset(machine);
        }
        else
        {
            /* Bit 3 requests a Multiface NMI and reads back as set while
               the request stands. NextZXOS uses it to hand the final stage
               of a snapshot launch to its Multiface code. Requests are
               ignored while a Multiface NMI is already being serviced. */
            uint8_t nmi_flags = machine->nextregs[reg] & 0x08;
            if ((value & 0x08) == 0)
                nmi_flags = 0;
            else if (!machine->multiface_nmi_active)
            {
                machine->multiface_nmi_active = true;
                machine->multiface_invisible = false;
                machine->nmi_request = true;
                machine->nmi_write_count = 0;
                nmi_flags = 0x08;
            }
            machine->nextregs[reg] =
                (uint8_t)((value & 0x80) | nmi_flags | (machine->reset_type & 0x03));
        }
        return;
    }
    if (reg == NEXT_REG_MACHINE_TYPE)
    {
        const uint8_t type = value & 7;
        machine->loader_active = false;
        if (type == 7)
        {
            machine->config_mode = true;
        }
        else if (type != 0 && machine->config_mode)
        {
            machine->config_mode = false;
        }
        if (type == 0)
        {
            value = (uint8_t)((value & 0xF8) |
                              (machine->nextregs[NEXT_REG_MACHINE_TYPE] & 7));
        }
    }
    machine->nextregs[reg] = value;
    if (reg == 0x06 || reg == 0x08 || reg == 0x09)
        machine->audio_ay_dirty = true;
    if (reg == 0x06)
    {
        if ((value & 0x03) == 0x03)
            next_reset_ay(machine);
    }
    else if (reg == 0x08)
    {
        machine->nextregs[reg] = (uint8_t)(value & 0x7F);
        /* Bit 7 unlocks port 0x7FFD by clearing its lock bit 5. */
        if ((value & 0x80) != 0)
        {
            machine->paging_locked = false;
            machine->port_7ffd &= (uint8_t)~0x20;
        }
        if ((value & 0x08) == 0)
            memset(machine->dac, 0x80, sizeof(machine->dac));
    }
    else if ((machine->nextregs[0x08] & 0x08) != 0)
    {
        if (reg == 0x2C)
            machine->dac[1] = value;
        else if (reg == 0x2D)
        {
            machine->dac[0] = value;
            machine->dac[3] = value;
        }
        else if (reg == 0x2E)
            machine->dac[2] = value;
    }
    if (reg == NEXT_REG_CPU_SPEED)
        next_set_cpu_speed(machine, value);
    if (reg == 0x22 || reg == 0x23 || reg == 0x07)
    {
        const uint8_t nr22 = machine->nextregs[0x22];
        const uint8_t nr23 = machine->nextregs[0x23];
        machine->frame_int_disabled = (nr22 & 0x04) != 0;
        if ((nr22 & 0x02) == 0)
        {
            machine->line_int_offset_tstates = 0;
            machine->line_int_pending = false;
        }
        else
        {
            uint16_t target = (uint16_t)nr23 | (uint16_t)((nr22 & 0x01) << 8);
            uint64_t line_abs = 310;
            if (target != 0)
                line_abs = (uint64_t)target - 1;
            machine->line_int_offset_tstates =
                line_abs * NEXT_LINE_TSTATES *
                (uint64_t)machine->cpu_speed_multiplier;
        }
    }
    if (reg == NEXT_REG_PALETTE_INDEX)
    {
        machine->palette_index = value;
        machine->palette_second_write = false;
    }
    else if (reg == NEXT_REG_PALETTE_CONTROL)
    {
        machine->palette_second_write = false;
    }
    else if (reg == NEXT_REG_PALETTE_VALUE_8)
    {
        const unsigned palette = (machine->nextregs[NEXT_REG_PALETTE_CONTROL] >> 4) & 7;
        machine->hw_palette[palette][machine->palette_index] = next_palette_from_8bit(value);
        machine->hw_palette_priority[palette][machine->palette_index] = 0;
        machine->palette_second_write = false;
        next_palette_advance(machine);
    }
    else if (reg == NEXT_REG_PALETTE_VALUE_9)
    {
        if (!machine->palette_second_write)
        {
            machine->palette_latch = value;
            machine->palette_second_write = true;
        }
        else
        {
            const unsigned palette = (machine->nextregs[NEXT_REG_PALETTE_CONTROL] >> 4) & 7;
            machine->hw_palette[palette][machine->palette_index] =
                (uint16_t)(((uint16_t)machine->palette_latch << 1) | (value & 1));
            machine->hw_palette_priority[palette][machine->palette_index] =
                (uint8_t)((value >> 6) & 3);
            machine->palette_second_write = false;
            next_palette_advance(machine);
        }
    }
    else if (reg == NEXT_REG_LAYER2_CONTROL)
    {
        /* NR$69 aliases port 0x123B bit 1 (Layer 2 display), port 0x7FFD
           bit 3 (ULA shadow screen) and port 0xFF bits 5:0 (Timex modes). */
        machine->layer2_control = (uint8_t)((machine->layer2_control & (uint8_t)~0x02) |
                                            ((value & 0x80) != 0 ? 0x02 : 0));
        machine->port_7ffd = (uint8_t)((machine->port_7ffd & (uint8_t)~0x08) |
                                       ((value & 0x40) != 0 ? 0x08 : 0));
        machine->timex_video_mode = (uint8_t)((machine->timex_video_mode & 0xC0) | (value & 0x3F));
    }
    if (reg >= NEXT_REG_MMU0 && reg <= NEXT_REG_MMU7)
    {
        const unsigned slot = reg - NEXT_REG_MMU0;
        machine->mmu[slot] = value;
        machine->mmu_override_mask |= (uint8_t)(1u << slot);
    }
    if (reg == NEXT_REG_ROM_MAPPING)
    {
        next_set_rom_bank(machine, value & 3);
        machine->port_1ffd = (uint8_t)((machine->port_1ffd & 0xF8) |
                                       ((value & 2) << 1) | ((value & 1) << 1) | ((value & 4) >> 2));
        if ((value & 4) == 0)
        {
            machine->port_7ffd = (uint8_t)((machine->port_7ffd & 0xEF) |
                                           ((value & 1) << 4));
        }
        if ((value & 0x08) != 0)
        {
            const uint8_t bank = (uint8_t)(((value >> 4) & 7) |
                                           ((value & 0x80) != 0 ? 8 : 0));
            machine->port_7ffd = (uint8_t)((machine->port_7ffd & 0xF8) |
                                           (bank & 7));
            machine->port_dffd = (uint8_t)((value >> 7) & 1);
            machine->mmu_override_mask &= 0x3F;
            machine->mmu[6] = (uint8_t)(bank * 2);
            machine->mmu[7] = (uint8_t)(bank * 2 + 1);
            machine->nextregs[NEXT_REG_MMU0 + 6] = machine->mmu[6];
            machine->nextregs[NEXT_REG_MMU0 + 7] = machine->mmu[7];
        }
    }
    if (reg == 0x04 && machine->config_mode)
    {
        machine->config_page = value;
    }
    if (reg == NEXT_REG_COPPER_DATA)
    {
        next_copper_write_data(&machine->copper_state, value);
    }
    else if (reg == NEXT_REG_COPPER_CURSOR_LOW)
    {
        next_copper_set_write_ptr_low(&machine->copper_state, value);
    }
    else if (reg == NEXT_REG_COPPER_CURSOR_HIGH)
    {
        next_copper_set_write_ptr_high_and_mode(&machine->copper_state, value);
    }
    if (reg == 0x15)
    {
        next_sprite_set_enabled(&machine->sprites, (value & 0x01) != 0);
        next_sprite_set_zero_on_top(&machine->sprites, (value & 0x40) != 0);
        next_sprite_set_over_border(&machine->sprites, (value & 0x02) != 0);
        next_sprite_set_border_clip(&machine->sprites, (value & 0x20) != 0);
    }
    else if (reg == 0x34)
    {
        if ((machine->nextregs[0x09] & 0x10) != 0)
        {
            next_sprite_select_slot(&machine->sprites, value);
            next_sprite_select_sprite(&machine->sprites, value);
        }
        else
        {
            next_sprite_select_sprite(&machine->sprites, value);
        }
        machine->nextregs[reg] = (uint8_t)(value & 0x7F);
    }
    else if (reg >= 0x35 && reg <= 0x39)
    {
        next_sprite_apply_attr_byte(&machine->sprites, (int)(reg - 0x35), value);
    }
    else if (reg >= 0x75 && reg <= 0x79)
    {
        next_sprite_apply_attr_byte(&machine->sprites, (int)(reg - 0x75), value);
        next_sprite_select_sprite(
            &machine->sprites,
            (uint8_t)(next_sprite_selected_sprite(&machine->sprites) + 1));
        machine->nextregs[0x34] = next_sprite_selected_sprite(&machine->sprites);
        if ((machine->nextregs[0x09] & 0x10) != 0)
            next_sprite_select_io_sprite(
                &machine->sprites,
                next_sprite_selected_sprite(&machine->sprites));
        machine->nextregs[0x35 + (reg - 0x75)] = value;
    }
    else if (reg == 0x18)
    {
        machine->layer2_clip[machine->layer2_clip_index & 3] = value;
        machine->layer2_clip_index = (uint8_t)((machine->layer2_clip_index + 1) & 3);
    }
    else if (reg == 0x19)
    {
        machine->sprite_clip[machine->sprite_clip_index & 3] = value;
        machine->sprite_clip_index = (uint8_t)((machine->sprite_clip_index + 1) & 3);
        next_sprite_set_clip(&machine->sprites,
                             machine->sprite_clip[0], machine->sprite_clip[1],
                             machine->sprite_clip[2], machine->sprite_clip[3]);
    }
    else if (reg == 0x1A)
    {
        machine->ula_clip[machine->ula_clip_index & 3] = value;
        machine->ula_clip_index = (uint8_t)((machine->ula_clip_index + 1) & 3);
    }
    else if (reg == 0x1C)
    {
        if ((value & 0x01) != 0)
            machine->layer2_clip_index = 0;
        if ((value & 0x02) != 0)
            machine->sprite_clip_index = 0;
        if ((value & 0x04) != 0)
            machine->ula_clip_index = 0;
        if ((value & 0x08) != 0)
            machine->tilemap_clip_index = 0;
    }
    else if (reg == 0x1B)
    {
        machine->tilemap_clip[machine->tilemap_clip_index & 3] = value;
        machine->tilemap_clip_index = (uint8_t)((machine->tilemap_clip_index + 1) & 3);
    }
    else if (reg == 0x2F)
    {
        machine->tilemap_scroll_x =
            (int)((value & 0x03) << 8) | (int)machine->nextregs[0x30];
    }
    else if (reg == 0x30)
    {
        machine->tilemap_scroll_x =
            (int)((machine->nextregs[0x2F] & 0x03) << 8) | (int)value;
    }
    else if (reg == 0x31)
    {
        machine->tilemap_scroll_y = (int)value;
    }
}

static bool next_is_dffd_port(uint16_t port)
{
    return (port & 0xF003) == 0xD001;
}

static bool next_is_ay_select_port(uint16_t port)
{
    return (port & 0xC007) == 0xC005;
}

static bool next_is_ay_data_port(uint16_t port)
{
    return (port & 0xC007) == 0x8005;
}

/* NR$0A bits 7:6 select the Multiface model: +3 pages in on 0x3F and out
   on 0xBF, 128 v87.2 swaps those, and 128 v87.12 / Multiface 1 use
   0x9F / 0x1F. */
static uint8_t next_multiface_enable_port(const NextMachine *machine)
{
    static const uint8_t ports[4] = {0x3F, 0xBF, 0x9F, 0x9F};
    return ports[machine->nextregs[0x0A] >> 6];
}

static uint8_t next_multiface_disable_port(const NextMachine *machine)
{
    static const uint8_t ports[4] = {0xBF, 0x3F, 0x1F, 0x1F};
    return ports[machine->nextregs[0x0A] >> 6];
}

uint8_t next_machine_read_port(NextMachine *machine, uint16_t port)
{
    const uint8_t port_lsb = (uint8_t)port;
    if (port_lsb == next_multiface_disable_port(machine))
    {
        machine->multiface_paged = false;
    }
    else if (port_lsb == next_multiface_enable_port(machine) &&
             !machine->multiface_invisible)
    {
        machine->multiface_paged = true;
        /* The +3 Multiface reads back the paging registers so its code can
           restore them on exit. */
        if ((machine->nextregs[0x0A] >> 6) == 0)
        {
            switch (port >> 12)
            {
            case 0x1:
                return machine->port_1ffd;
            case 0x7:
                return machine->port_7ffd;
            case 0xD:
                return machine->port_dffd;
            default:
                return 0xFF;
            }
        }
    }
    if (port_lsb == 0x6B || port_lsb == 0x0B)
    {
        next_dma_set_z80_mode(&machine->dma, port_lsb == 0x0B);
        return next_dma_read_command(machine);
    }
    if (port == 0x243B)
        return machine->selected_nextreg;
    if (port == 0x253B)
    {
        return next_machine_read_nextreg(machine, machine->selected_nextreg);
    }
    if (port == 0x303B)
        return next_sprite_read_status(&machine->sprites);
    if (port == 0x123B)
        return machine->layer2_control;
    if (port == 0x133B)
    {
        /* No serial peer is attached. Report an empty transmitter and no
           receive/error state so debug-output polling cannot deadlock the
           emulated machine. */
        return 0x10;
    }
    if (port == 0x143B)
        return 0x00;
    if (port == 0x113B)
    {
        /* i2c SDA read-back: the open-drain bus floats high unless the
           RTC slave pulls it low. */
        uint8_t value = 0xFE;
        if (next_rtc_read_sda(&machine->rtc))
            value |= 0x01;
        return value;
    }
    if (port == 0xBFF5)
    {
        static const uint8_t chip_bits[3] = {0xC0, 0x80, 0x40};
        return (uint8_t)(chip_bits[machine->selected_ay] |
                         (machine->ay[machine->selected_ay].addr & 0x1F));
    }
    if (next_is_ay_select_port(port) || next_is_ay_data_port(port))
    {
        const ay38910_t *ay = &machine->ay[machine->selected_ay];
        return ay->addr < AY38910_NUM_REGISTERS ? ay->reg[ay->addr] : 0xFF;
    }
    if ((uint8_t)port == 0xE3)
        return machine->divmmc_control;
    if ((uint8_t)port == 0xEB)
        return next_sd_read_data(&machine->sd_card);
    if ((uint8_t)port == 0x1F)
        return machine->joystick_mask;
    if (port_lsb == 0xFF && (machine->nextregs[0x08] & 0x04) != 0)
        return machine->timex_video_mode;
    if ((port & 1) == 0)
    {
        const uint16_t columns = (uint16_t)(~(port >> 8)) & 0xFF;
        const uint16_t lines = kbd_test_lines(&machine->keyboard, columns);
        return (uint8_t)(0xE0 | ((~lines) & 0x1F));
    }
    return 0xFF;
}

static void next_set_legacy_mmu_page(
    NextMachine *machine, unsigned slot, uint8_t page)
{
    machine->mmu[slot] = page;
    machine->nextregs[NEXT_REG_MMU0 + slot] = page;
    machine->mmu_override_mask &= (uint8_t)~(1u << slot);
}

static void next_apply_normal_paging(NextMachine *machine)
{
    const uint8_t bank = (uint8_t)(((machine->port_dffd & 0x0F) << 3) |
                                   (machine->port_7ffd & 7));
    static const uint8_t fixed_pages[6] = {0xFF, 0xFF, 10, 11, 4, 5};

    for (unsigned slot = 0; slot < 6; ++slot)
        next_set_legacy_mmu_page(machine, slot, fixed_pages[slot]);
    next_set_legacy_mmu_page(machine, 6, (uint8_t)(bank * 2));
    next_set_legacy_mmu_page(machine, 7, (uint8_t)(bank * 2 + 1));
}

static void next_apply_all_ram_paging(NextMachine *machine)
{
    static const uint8_t configurations[4][4] = {
        {0, 1, 2, 3},
        {4, 5, 6, 7},
        {4, 5, 6, 3},
        {4, 7, 6, 3}};
    const unsigned configuration = (machine->port_1ffd >> 1) & 3;

    for (unsigned slot = 0; slot < NEXT_MMU_SLOT_COUNT; ++slot)
    {
        const uint8_t bank = configurations[configuration][slot >> 1];
        next_set_legacy_mmu_page(
            machine, slot, (uint8_t)(bank * 2 + (slot & 1)));
    }
}

void next_machine_write_port(NextMachine *machine, uint16_t port, uint8_t value)
{
    const uint8_t port_lsb = (uint8_t)port;
    /* Writing either Multiface port hides it until the next NMI, so later
       software reading those ports cannot page it in by accident. */
    if (port_lsb == next_multiface_enable_port(machine) ||
        port_lsb == next_multiface_disable_port(machine))
    {
        machine->multiface_invisible = true;
    }
    if (port_lsb == 0x6B || port_lsb == 0x0B)
    {
        next_dma_set_z80_mode(&machine->dma, port_lsb == 0x0B);
        next_dma_write_command(machine, value);
        return;
    }
    if (port == 0x303B)
    {
        next_sprite_select_slot(&machine->sprites, value);
        if ((machine->nextregs[0x09] & 0x10) != 0)
        {
            next_sprite_select_sprite(&machine->sprites, value);
            machine->nextregs[0x34] = (uint8_t)(value & 0x7F);
        }
        return;
    }
    if (port == 0x133B)
    {
        /* Treat transmitted bytes as consumed immediately when no UART peer
           is connected. */
        return;
    }
    if (port_lsb == 0x3B && (port >> 8) == 0x10)
    {
        next_rtc_write_scl(&machine->rtc, (value & 0x01) != 0);
        return;
    }
    if (port_lsb == 0x3B && (port >> 8) == 0x11)
    {
        next_rtc_write_sda(&machine->rtc, (value & 0x01) != 0);
        return;
    }
    if (next_is_ay_select_port(port) && !next_is_dffd_port(port))
    {
        if (next_ay_held(machine))
            return;
        machine->audio_ay_dirty = true;
        if ((value & 0x80) != 0 && (value & 0x1C) == 0x1C)
        {
            if ((machine->nextregs[0x08] & 0x02) != 0)
            {
                const uint8_t selection = value & 3;
                if (selection == 2)
                    machine->selected_ay = 1;
                else if (selection == 1)
                    machine->selected_ay = 2;
                else
                    machine->selected_ay = 0;
                machine->ay_pan[machine->selected_ay] =
                    (uint8_t)((value >> 5) & 3);
            }
        }
        else if ((value & 0xE0) == 0)
        {
            /* Registers 16-31 deselect the AY, so later data writes are ignored. */
            ay38910_set_addr_latch(&machine->ay[machine->selected_ay], value & 0x1F);
        }
        return;
    }
    if (next_is_ay_data_port(port))
    {
        if (next_ay_held(machine))
            return;
        machine->audio_ay_dirty = true;
        ay38910_t *ay = &machine->ay[machine->selected_ay];
        ay38910_set_register(ay, ay->addr, value);
        return;
    }
    if ((machine->nextregs[0x08] & 0x08) != 0)
    {
        switch (port_lsb)
        {
        case 0x1F:
        case 0xF1:
        case 0x3F:
            machine->dac[0] = value;
            return;
        case 0x0F:
        case 0xF3:
            machine->dac[1] = value;
            return;
        case 0x4F:
        case 0xF9:
            machine->dac[2] = value;
            return;
        case 0x5F:
            machine->dac[3] = value;
            return;
        case 0xDF:
        case 0xFB:
            machine->dac[0] = value;
            machine->dac[3] = value;
            return;
        case 0xB3:
            machine->dac[1] = value;
            machine->dac[2] = value;
            return;
        default:
            break;
        }
    }
    if (port_lsb == 0x5B)
    {
        next_sprite_write_pattern_byte(&machine->sprites, value);
        return;
    }
    if (port_lsb == 0x57)
    {
        if (next_sprite_write_attr(&machine->sprites, value) &&
            (machine->nextregs[0x09] & 0x10) != 0)
        {
            const uint8_t selected =
                next_sprite_selected_io_sprite(&machine->sprites);
            next_sprite_select_sprite(&machine->sprites, selected);
            machine->nextregs[0x34] = selected;
        }
        return;
    }
    if (port == 0x243B)
    {
        machine->selected_nextreg = value;
    }
    else if (port == 0x253B)
    {
        next_machine_write_nextreg(machine, machine->selected_nextreg, value);
    }
    else if ((uint8_t)port == 0xE7)
    {
        next_sd_write_chip_select(&machine->sd_card, value);
    }
    else if ((uint8_t)port == 0xE3)
    {
        machine->divmmc_control = value;
    }
    else if ((uint8_t)port == 0xEB)
    {
        next_sd_write_data(&machine->sd_card, value);
    }
    else if (port_lsb == 0xFF)
    {
        machine->timex_video_mode = value;
    }
    else if (port == 0x123B)
    {
        /* With bit 4 set the write only carries the 16K bank offset. */
        if ((value & 0x10) == 0)
        {
            machine->layer2_control = value;
            machine->layer2_map_control = value;
        }
        else
        {
            machine->layer2_map_offset = value & 0x07;
        }
    }
    else if ((port & 0xF002) == 0xD000 && !machine->paging_locked)
    {
        const uint8_t bank = (uint8_t)(((value & 0x0F) << 3) |
                                       (machine->port_7ffd & 7));
        machine->port_dffd = value & 0x0F;
        if ((machine->port_1ffd & 1) == 0)
        {
            next_set_legacy_mmu_page(machine, 6, (uint8_t)(bank * 2));
            next_set_legacy_mmu_page(machine, 7, (uint8_t)(bank * 2 + 1));
        }
    }
    else if ((port & 0xF002) == 0x1000 && !machine->paging_locked)
    {
        const bool was_all_ram = (machine->port_1ffd & 1) != 0;
        machine->port_1ffd = value;
        next_set_rom_bank(machine, (uint8_t)(((value & 4) >> 1) |
                                             ((machine->port_7ffd & 0x10) >> 4)));
        if ((value & 1) != 0)
            next_apply_all_ram_paging(machine);
        else if (was_all_ram)
            next_apply_normal_paging(machine);
    }
    else if ((port & 0xC002) == 0x4000 && !machine->paging_locked)
    {
        const uint8_t bank = (uint8_t)(((machine->port_dffd & 0x0F) << 3) |
                                       (value & 7));
        machine->port_7ffd = value;
        next_set_rom_bank(machine, (uint8_t)(((machine->port_1ffd & 4) >> 1) |
                                             ((value >> 4) & 1)));
        if ((machine->port_1ffd & 1) == 0)
        {
            next_set_legacy_mmu_page(machine, 6, (uint8_t)(bank * 2));
            next_set_legacy_mmu_page(machine, 7, (uint8_t)(bank * 2 + 1));
        }
        machine->paging_locked = (value & 0x20) != 0;
    }
    else if ((port & 1) == 0)
    {
        machine->border_color = value & 7;
        beeper_set(&machine->beeper, (value & 0x10) != 0);
    }
}

/* One CPU clock. Inlined into the frame loop: this is the emulator's hot
   path, so rarely-changing state is tested as cheaply as possible. */
static inline uint64_t next_machine_tick_inline(NextMachine *machine)
{
    uint64_t pins;

    /* DMA state is tested inline: this runs every T-state. */
    if (machine->dma.active_burst)
        next_dma_step(machine, machine->ticks);
    /* The Z80 latches NMI on a rising edge, so a one-tick pulse suffices. */
    if (machine->nmi_request)
    {
        machine->cpu_pins |= Z80_NMI;
        machine->nmi_request = false;
    }
    else if ((machine->cpu_pins & Z80_NMI) != 0)
    {
        machine->cpu_pins &= ~Z80_NMI;
    }
    if (machine->dma.active_burst && machine->dma.hold_cpu)
    {
        /* A continuous DMA transfer owns the bus: the CPU is frozen and
           drives no bus cycles, but time, interrupts and audio carry on. */
        pins = machine->cpu_pins & ~Z80_CTRL_PIN_MASK;
    }
    else
    {
        pins = z80_tick(&machine->cpu, machine->cpu_pins);
    }
    /* Test the enable bit rather than the offset: line 1 has offset 0. */
    if ((machine->nextregs[0x22] & 0x02) != 0 &&
        machine->frame_tstate >= machine->line_int_offset_tstates &&
        machine->frame_tstate <
            machine->line_int_offset_tstates +
                (uint32_t)(32 * machine->cpu_speed_multiplier))
    {
        machine->line_int_pending = true;
    }
    if (machine->line_int_pending)
    {
        if (machine->int_counter == 0)
            machine->int_counter =
                32 * machine->cpu_speed_multiplier;
        if (machine->int_counter > 0 && --machine->int_counter == 0)
        {
            pins &= ~Z80_INT;
            machine->line_int_pending = false;
        }
        else
        {
            pins |= Z80_INT;
        }
    }
    else if (machine->frame_tstate == 0)
    {
        if (!machine->frame_int_disabled)
        {
            pins |= Z80_INT;
            machine->int_counter =
                32 * machine->cpu_speed_multiplier;
        }
        else
        {
            pins &= ~Z80_INT;
        }
    }
    else if (machine->int_counter > 0 && --machine->int_counter == 0)
        pins &= ~Z80_INT;
    /* A paged Multiface owns the bottom 16K, so its own interrupt handler at
       0x0038 must not arm divMMC automap. NextZXOS's snapshot launcher ends
       with RETN to a RET in the 48K ROM, which an armed divMMC would hide. */
    if ((pins & (Z80_M1 | Z80_MREQ | Z80_RD)) == (Z80_M1 | Z80_MREQ | Z80_RD) &&
        !machine->loader_active && !machine->multiface_paged &&
        (machine->nextregs[0x0A] & 0x10) != 0)
    {
        const uint16_t address = Z80_GET_ADDR(pins);
        const bool rom3_eligible = !machine->config_mode && machine->rom_bank == 3;

        if (machine->divmmc_automap_pending)
        {
            machine->divmmc_automap = true;
            machine->divmmc_automap_pending = false;
        }
        if ((address & 7) == 0 && address <= 0x0038)
        {
            const unsigned entry = address >> 3;
            const uint8_t mask = (uint8_t)(1u << entry);
            if ((machine->nextregs[0xB8] & mask) != 0)
            {
                const bool main_entry = (machine->nextregs[0xB9] & mask) != 0;
                if (main_entry || rom3_eligible)
                {
                    if ((machine->nextregs[0xBA] & mask) != 0)
                    {
                        machine->divmmc_automap = true;
                    }
                    else
                    {
                        machine->divmmc_automap_pending = true;
                    }
                }
            }
        }
        else if (rom3_eligible &&
                 (((machine->nextregs[0xBB] & 0x04) != 0 && address == 0x04C6) ||
                  ((machine->nextregs[0xBB] & 0x08) != 0 && address == 0x0562) ||
                  ((machine->nextregs[0xBB] & 0x10) != 0 && address == 0x04D7) ||
                  ((machine->nextregs[0xBB] & 0x20) != 0 && address == 0x056A)))
        {
            machine->divmmc_automap_pending = true;
        }
        else if (rom3_eligible && (machine->nextregs[0xBB] & 0x80) != 0 &&
                 (address & 0xFF00) == 0x3D00)
        {
            machine->divmmc_automap = true;
        }
    }
    if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ))
    {
        Z80_SET_DATA(pins, 0xFF);
    }
    else if (pins & Z80_MREQ)
    {
        const uint16_t address = Z80_GET_ADDR(pins);
        const bool opcode_fetch = (pins & (Z80_M1 | Z80_RD)) == (Z80_M1 | Z80_RD);
        /* The Multiface pages in as the NMI handler's first opcode is
           fetched from 0x0066, so that opcode already comes from its ROM. */
        if (opcode_fetch && address == 0x0066 && machine->multiface_nmi_active &&
            !machine->multiface_paged)
        {
            next_nmi_acknowledged(machine);
            machine->multiface_paged = true;
        }
        if (pins & Z80_RD)
        {
            /* Plain RAM above 16K is read inline; the full decoder handles
               ROM, divMMC, Multiface, config mode and Layer 2 mapping. */
            uint8_t read_value =
                address >= 0x4000 &&
                        (address >= 0xC000 || (machine->layer2_map_control & 0xC4) != 0xC4)
                    ? machine->ram[(size_t)machine->mmu[address >> 13] * NEXT_MMU_SLOT_SIZE +
                                   (address & (NEXT_MMU_SLOT_SIZE - 1))]
                    : next_machine_read_memory(machine, address);
            if (!opcode_fetch && machine->nmi_retn_reads > 0)
            {
                /* A stackless RETN pops the address saved in NR$C2/C3. */
                read_value = machine->nextregs[machine->nmi_retn_reads == 2 ? 0xC2 : 0xC3];
                --machine->nmi_retn_reads;
            }
            Z80_SET_DATA(pins, read_value);
            if (opcode_fetch)
            {
                /* RETN (ED 45 and its mirrors, but not RETI) ends the
                   Multiface NMI and pages it out. */
                const bool retn = machine->last_m1_ed_prefix &&
                                  (read_value & 0xC7) == 0x45 &&
                                  read_value != 0x4D;
                machine->last_m1_ed_prefix =
                    !machine->last_m1_ed_prefix && read_value == 0xED;
                if (retn && machine->nmi_stackless_retn)
                {
                    machine->nmi_stackless_retn = false;
                    machine->nmi_retn_reads = 2;
                }
                if (retn && machine->multiface_nmi_active)
                {
                    machine->multiface_nmi_active = false;
                    machine->multiface_paged = false;
                }
            }
        }
        else if (pins & Z80_WR)
        {
            if (machine->multiface_nmi_active && !machine->multiface_paged)
            {
                /* Remember what the NMI acknowledge overwrites. */
                machine->nmi_write_address[0] = machine->nmi_write_address[1];
                machine->nmi_write_old_value[0] = machine->nmi_write_old_value[1];
                machine->nmi_write_address[1] = address;
                machine->nmi_write_old_value[1] = next_machine_read_memory(machine, address);
                if (machine->nmi_write_count < 2)
                    ++machine->nmi_write_count;
            }
            if (address >= 0x4000 &&
                (address >= 0xC000 || (machine->layer2_map_control & 0xC1) != 0xC1))
            {
                machine->ram[(size_t)machine->mmu[address >> 13] * NEXT_MMU_SLOT_SIZE +
                             (address & (NEXT_MMU_SLOT_SIZE - 1))] = Z80_GET_DATA(pins);
            }
            else
            {
                next_machine_write_memory(machine, address, Z80_GET_DATA(pins));
            }
        }
        if (opcode_fetch && address >= 0x1FF8 && address <= 0x1FFF)
        {
            machine->divmmc_automap = false;
            machine->divmmc_automap_pending = false;
        }
    }
    else if ((pins & Z80_IORQ) != 0 && (pins & Z80_M1) == 0)
    {
        const uint16_t port = Z80_GET_ADDR(pins);
        if (pins & Z80_RD)
        {
            Z80_SET_DATA(pins, next_machine_read_port(machine, port));
        }
        else if (pins & Z80_WR)
        {
            next_machine_write_port(machine, port, Z80_GET_DATA(pins));
        }
    }
    machine->cpu_pins = pins;
    ++machine->ticks;
    {
        const uint32_t frame_ticks =
            NEXT_FRAME_TSTATES * (uint32_t)machine->cpu_speed_multiplier;
        const uint64_t charge =
            machine->dma.pending_charge != 0 ? next_dma_take_charge(&machine->dma) : 0;
        if (charge == 0)
        {
            if (++machine->audio_cpu_divider >= machine->cpu_speed_multiplier)
            {
                machine->audio_cpu_divider = 0;
                next_audio_tick(machine);
            }
        }
        else
        {
            /* Cycles the DMA steals from the CPU still pass for audio. */
            next_audio_advance(machine, 1 + charge);
        }
        machine->ticks += charge;
        machine->frame_tstate += (uint32_t)(charge + 1);
        while (machine->frame_tstate >= frame_ticks)
        {
            machine->frame_tstate -= frame_ticks;
            ++machine->frame_count;
            machine->line_int_pending = false;
            machine->int_counter = 0;
        }
    }
    if (machine->debug.callback.func != NULL)
        machine->debug.callback.func(machine->debug.callback.user_data, pins);
    return pins;
}

/* True once a debugger hook has asked execution to stop. */
static bool next_machine_debug_stopped(const NextMachine *machine)
{
    return machine->debug.stopped != NULL && *machine->debug.stopped;
}

uint64_t next_machine_tick(NextMachine *machine)
{
    if (machine == NULL || !machine->initialized)
        return 0;
    return next_machine_tick_inline(machine);
}

void next_machine_exec_ticks(NextMachine *machine, uint32_t ticks)
{
    if (machine == NULL || !machine->initialized)
        return;
    while (ticks-- > 0 && !next_machine_debug_stopped(machine))
        next_machine_tick_inline(machine);
}

void next_machine_exec_frame(NextMachine *machine)
{
    uint64_t target;

    if (machine == NULL || !machine->initialized)
        return;
    target = machine->frame_count + 1;
    if (machine->debug.callback.func != NULL)
    {
        /* A debugger stop leaves the frame unfinished; the next call
           completes it, so the frame is only rendered (and the Copper
           advanced) once. */
        while (machine->frame_count < target)
        {
            next_machine_tick_inline(machine);
            if (next_machine_debug_stopped(machine))
                return;
        }
    }
    else
    {
        while (machine->frame_count < target)
            next_machine_tick_inline(machine);
    }
    kbd_update(&machine->keyboard, 20000);
    next_machine_render_frame_internal(machine, true);
}

void next_machine_key_down(NextMachine *machine, int key_code)
{
    if (machine != NULL && machine->initialized)
    {
        kbd_key_down(&machine->keyboard, key_code);
    }
}

void next_machine_key_up(NextMachine *machine, int key_code)
{
    if (machine != NULL && machine->initialized)
    {
        kbd_key_up(&machine->keyboard, key_code);
    }
}

void next_machine_set_joystick_mask(NextMachine *machine, uint8_t mask)
{
    if (machine != NULL)
        machine->joystick_mask = mask;
}

static uint8_t next_ula_palette_index(uint8_t attribute, bool ink)
{
    const uint8_t bright = (attribute >> 6) & 1;
    const uint8_t color = ink ? (attribute & 7) : ((attribute >> 3) & 7);
    return (uint8_t)(color + bright * 8);
}

static uint8_t next_timex_hires_attribute(uint8_t video_mode)
{
    const uint8_t ink = (video_mode >> 3) & 7;
    const uint8_t paper = ink ^ 7;
    return (uint8_t)(0x40 | (paper << 3) | ink);
}

static const uint8_t *next_tilemap_bank(
    const NextMachine *machine, uint8_t base, size_t *length)
{
    if ((base & 0x80) != 0)
    {
        *length = NEXT_MMU_SLOT_SIZE;
        return machine->ram + 7 * NEXT_NEX_BANK_SIZE;
    }
    *length = NEXT_NEX_BANK_SIZE;
    return machine->ram + 5 * NEXT_NEX_BANK_SIZE;
}

static int next_render_tilemap_scanline(
    const NextMachine *machine,
    int y,
    uint8_t *indices,
    uint8_t *present,
    uint8_t *below_ula,
    int max_width)
{
    const uint8_t control = machine->nextregs[0x6B];
    const int tiles_per_row = (control & 0x40) != 0 ? 80 : 40;
    const int width = tiles_per_row * 8;
    size_t map_len;
    size_t tiles_len;
    const uint8_t *map_buf;
    const uint8_t *tiles_buf;
    int map_offset_base;
    int tiles_offset_base;
    int xscale;
    int clip_x_start;
    int clip_x_end;
    bool strip_flags;
    int bytes_per_tile;
    int abs_y;
    int tile_y;
    int pixel_row;
    bool textmode;
    bool mode512;
    int x;

    for (x = 0; x < width && x < max_width; ++x)
    {
        indices[x] = 0;
        present[x] = 0;
        below_ula[x] = 0;
    }
    if (y < machine->tilemap_clip[2] || y > machine->tilemap_clip[3])
    {
        return width;
    }
    map_buf = next_tilemap_bank(machine, machine->nextregs[0x6E], &map_len);
    tiles_buf = next_tilemap_bank(machine, machine->nextregs[0x6F], &tiles_len);
    map_offset_base = (int)(machine->nextregs[0x6E] & 0x3F) << 8;
    tiles_offset_base = (int)(machine->nextregs[0x6F] & 0x3F) << 8;
    xscale = tiles_per_row == 80 ? 2 : 1;
    clip_x_start = (int)machine->tilemap_clip[0] * 2 * xscale;
    clip_x_end = ((int)machine->tilemap_clip[1] * 2 + 1) * xscale + (xscale - 1);
    strip_flags = (control & 0x20) != 0;
    bytes_per_tile = strip_flags ? 1 : 2;
    abs_y = y + machine->tilemap_scroll_y;
    tile_y = (abs_y / 8) % 32;
    pixel_row = abs_y % 8;
    textmode = (control & 0x08) != 0;
    mode512 = (control & 0x02) != 0;

    for (x = 0; x < width && x < max_width; ++x)
    {
        int abs_x;
        int tile_x;
        int map_entry;
        uint8_t attr;
        int tile_index;
        int pixel_in_tile;
        if (x < clip_x_start || x > clip_x_end)
            continue;
        abs_x = x + machine->tilemap_scroll_x;
        tile_x = (abs_x / 8) % tiles_per_row;
        map_entry = (map_offset_base + (tile_y * tiles_per_row + tile_x) * bytes_per_tile) % (int)map_len;
        attr = machine->nextregs[0x6C];
        if (!strip_flags)
            attr = map_buf[(map_entry + 1) % (int)map_len];
        tile_index = map_buf[map_entry];
        if (mode512)
            tile_index |= (int)(attr & 0x01) << 8;
        below_ula[x] = (uint8_t)(((attr & 0x01) != 0 || mode512) && (control & 0x01) == 0);
        pixel_in_tile = abs_x % 8;

        if (textmode)
        {
            const int def_addr = (tiles_offset_base + tile_index * 8 + pixel_row) % (int)tiles_len;
            const int bit = (tiles_buf[def_addr] >> (7 - pixel_in_tile)) & 1;
            indices[x] = (uint8_t)((attr & 0xFE) | (uint8_t)bit);
            present[x] = 1;
            continue;
        }

        {
            int tx = pixel_in_tile;
            int ty = pixel_row;
            int palette_offset;
            int tile_data_base;
            uint8_t b;
            uint8_t nibble;
            if (((attr >> 3) & 1) != ((attr >> 1) & 1))
                tx = 7 - tx;
            if ((attr & 0x04) != 0)
                ty = 7 - ty;
            if ((attr & 0x02) != 0)
            {
                const int t = tx;
                tx = ty;
                ty = t;
            }
            palette_offset = (attr >> 4) & 0x0F;
            tile_data_base = (tiles_offset_base + tile_index * 32 + ty * 4) % (int)tiles_len;
            b = tiles_buf[(tile_data_base + tx / 2) % (int)tiles_len];
            nibble = (tx & 1) == 0 ? (uint8_t)((b >> 4) & 0x0F) : (uint8_t)(b & 0x0F);
            indices[x] = (uint8_t)((palette_offset << 4) | nibble);
            present[x] = 1;
        }
    }
    return width;
}

typedef struct NextMixerInput
{
    uint16_t ula_rgb;
    uint16_t tile_rgb;
    uint16_t sprite_rgb;
    uint16_t layer2_rgb;
    bool ula_enabled;
    bool ula_clipped;
    bool ula_classic_palette;
    bool border;
    bool tile_enabled;
    bool tile_pixel_enabled;
    bool tile_textmode;
    bool tile_below;
    bool sprite_enabled;
    bool layer2_enabled;
    bool layer2_priority;
    uint8_t transparency;
    uint8_t fallback;
    uint8_t blend_mode;
    uint8_t priority;
    bool stencil_mode;
} NextMixerInput;

static uint16_t next_fallback_color(uint8_t value)
{
    return next_palette_from_8bit(value);
}

static uint16_t next_mix_pixel(const NextMixerInput *in,
                               bool *classic_ula_output)
{
    const uint8_t transparency = in->transparency;
    const bool ula_mix_transparent =
        (uint8_t)(in->ula_rgb >> 1) == transparency || in->ula_clipped;
    const uint16_t ula_mix_rgb = ula_mix_transparent ? 0 : in->ula_rgb;
    const bool ula_transparent = ula_mix_transparent || !in->ula_enabled;
    const uint16_t ula_rgb = ula_transparent ? 0 : in->ula_rgb;
    const bool tile_transparent = !in->tile_pixel_enabled ||
                                  (in->tile_textmode && (uint8_t)(in->tile_rgb >> 1) == transparency) ||
                                  !in->tile_enabled;
    const uint16_t tile_rgb = tile_transparent ? 0 : in->tile_rgb;
    const bool stencil_transparent = ula_transparent || tile_transparent;
    const uint16_t stencil_rgb = stencil_transparent ? 0 : (uint16_t)(ula_rgb & tile_rgb);
    const uint16_t ulatm_rgb =
        !tile_transparent && (!in->tile_below || ula_transparent)
            ? tile_rgb
            : ula_rgb;
    const bool ulatm_transparent = ula_transparent && tile_transparent;
    const bool sprite_transparent = !in->sprite_enabled;
    const bool layer2_transparent =
        (uint8_t)(in->layer2_rgb >> 1) == transparency || !in->layer2_enabled;
    const bool layer2_priority = in->layer2_priority && !layer2_transparent;
    uint16_t ula_final_rgb;
    bool ula_final_transparent;
    bool ula_final_classic_ula;
    uint16_t mix_rgb = 0;
    uint16_t mix_top_rgb = 0;
    uint16_t mix_bottom_rgb = 0;
    bool mix_transparent = true;
    bool mix_top_transparent = true;
    bool mix_bottom_transparent = true;
    bool mix_top_classic_ula = false;
    bool mix_bottom_classic_ula = false;
    uint16_t out = next_fallback_color(in->fallback);
    const bool ula_border_sprite_mask =
        in->border && tile_transparent && !sprite_transparent;

    *classic_ula_output = false;

    if (in->stencil_mode && in->ula_enabled && in->tile_enabled)
    {
        ula_final_rgb = stencil_rgb;
        ula_final_transparent = stencil_transparent;
        ula_final_classic_ula = false;
    }
    else
    {
        ula_final_rgb = ulatm_rgb;
        ula_final_transparent = ulatm_transparent;
        ula_final_classic_ula = in->ula_classic_palette &&
                                (tile_transparent || (in->tile_below && !ula_transparent));
    }

    switch (in->blend_mode & 3)
    {
    case 0:
        mix_rgb = ula_mix_rgb;
        mix_transparent = ula_mix_transparent;
        mix_top_rgb = tile_rgb;
        mix_top_transparent = tile_transparent || in->tile_below;
        mix_bottom_rgb = tile_rgb;
        mix_bottom_transparent = tile_transparent || !in->tile_below;
        break;
    case 2:
        mix_rgb = ula_final_rgb;
        mix_transparent = ula_final_transparent;
        break;
    case 3:
        mix_rgb = tile_rgb;
        mix_transparent = tile_transparent;
        mix_top_rgb = ula_rgb;
        mix_top_transparent = ula_transparent || !in->tile_below;
        mix_top_classic_ula = in->ula_classic_palette && !ula_transparent;
        mix_bottom_rgb = ula_rgb;
        mix_bottom_transparent = ula_transparent || in->tile_below;
        mix_bottom_classic_ula = in->ula_classic_palette && !ula_transparent;
        break;
    default:
        if (in->tile_below)
        {
            mix_top_rgb = ula_rgb;
            mix_top_transparent = ula_transparent;
            mix_top_classic_ula = in->ula_classic_palette && !ula_transparent;
            mix_bottom_rgb = tile_rgb;
            mix_bottom_transparent = tile_transparent;
        }
        else
        {
            mix_top_rgb = tile_rgb;
            mix_top_transparent = tile_transparent;
            mix_bottom_rgb = ula_rgb;
            mix_bottom_transparent = ula_transparent;
            mix_bottom_classic_ula = in->ula_classic_palette && !ula_transparent;
        }
        break;
    }

    switch (in->priority & 7)
    {
    case 0:
        if (layer2_priority)
            out = in->layer2_rgb;
        else if (!sprite_transparent)
            out = in->sprite_rgb;
        else if (!layer2_transparent)
            out = in->layer2_rgb;
        else if (!ula_final_transparent)
        {
            out = ula_final_rgb;
            *classic_ula_output = ula_final_classic_ula;
        }
        break;
    case 1:
        if (!layer2_transparent)
            out = in->layer2_rgb;
        else if (!sprite_transparent)
            out = in->sprite_rgb;
        else if (!ula_final_transparent)
        {
            out = ula_final_rgb;
            *classic_ula_output = ula_final_classic_ula;
        }
        break;
    case 2:
        if (layer2_priority)
            out = in->layer2_rgb;
        else if (!sprite_transparent)
            out = in->sprite_rgb;
        else if (!ula_final_transparent)
        {
            out = ula_final_rgb;
            *classic_ula_output = ula_final_classic_ula;
        }
        else if (!layer2_transparent)
            out = in->layer2_rgb;
        break;
    case 3:
        if (!layer2_transparent)
            out = in->layer2_rgb;
        else if (!ula_final_transparent && !ula_border_sprite_mask)
        {
            out = ula_final_rgb;
            *classic_ula_output = ula_final_classic_ula;
        }
        else if (!sprite_transparent)
            out = in->sprite_rgb;
        break;
    case 4:
        if (layer2_priority)
            out = in->layer2_rgb;
        else if (!ula_final_transparent && !ula_border_sprite_mask)
        {
            out = ula_final_rgb;
            *classic_ula_output = ula_final_classic_ula;
        }
        else if (!sprite_transparent)
            out = in->sprite_rgb;
        else if (!layer2_transparent)
            out = in->layer2_rgb;
        break;
    case 5:
        if (layer2_priority)
            out = in->layer2_rgb;
        else if (!ula_final_transparent && !ula_border_sprite_mask)
        {
            out = ula_final_rgb;
            *classic_ula_output = ula_final_classic_ula;
        }
        else if (!layer2_transparent)
            out = in->layer2_rgb;
        else if (!sprite_transparent)
            out = in->sprite_rgb;
        break;
    case 6:
    case 7:
    {
        int r = (int)((in->layer2_rgb >> 6) & 7) + (int)((mix_rgb >> 6) & 7);
        int g = (int)((in->layer2_rgb >> 3) & 7) + (int)((mix_rgb >> 3) & 7);
        int b = (int)(in->layer2_rgb & 7) + (int)(mix_rgb & 7);
        if ((in->priority & 7) == 6)
        {
            if (r > 7)
                r = 7;
            if (g > 7)
                g = 7;
            if (b > 7)
                b = 7;
        }
        else if (!mix_transparent)
        {
            r = r <= 4 ? 0 : (r >= 12 ? 7 : (r - 5));
            g = g <= 4 ? 0 : (g >= 12 ? 7 : (g - 5));
            b = b <= 4 ? 0 : (b >= 12 ? 7 : (b - 5));
        }
        const uint16_t blended =
            (uint16_t)(((r & 7) << 6) | ((g & 7) << 3) | (b & 7));
        if (layer2_priority)
            out = blended;
        else if (!mix_top_transparent)
        {
            out = mix_top_rgb;
            *classic_ula_output = mix_top_classic_ula;
        }
        else if (!sprite_transparent)
            out = in->sprite_rgb;
        else if (!mix_bottom_transparent)
        {
            out = mix_bottom_rgb;
            *classic_ula_output = mix_bottom_classic_ula;
        }
        else if (!layer2_transparent)
            out = blended;
        break;
    }
    }
    return out;
}

/* Number of INK bits in a ULANext mask (NR$42), or -1 when the mask is 255
   or not a contiguous run of low bits; PAPER then uses the NR$4A fallback. */
static int next_ulanext_ink_bits(uint8_t mask)
{
    int bits = 0;
    while (bits < 7 && (mask & (1u << bits)) != 0)
        ++bits;
    return mask == (uint8_t)((1u << bits) - 1u) && bits > 0 && bits < 8 ? bits : -1;
}

/* ULANext packs INKs from palette index 0 and PAPERs from index 128. */
static uint8_t next_ula_palette_entry(
    const NextMachine *machine, uint8_t attribute, bool ink, bool *use_fallback)
{
    *use_fallback = false;
    if ((machine->nextregs[NEXT_REG_PALETTE_CONTROL] & 0x01) != 0)
    {
        const uint8_t mask = machine->nextregs[0x42];
        const int ink_bits = next_ulanext_ink_bits(mask);
        if (ink)
            return (uint8_t)(attribute & mask);
        if (ink_bits < 0)
        {
            *use_fallback = true;
            return 0;
        }
        return (uint8_t)(0x80 + (attribute >> ink_bits));
    }
    return next_ula_palette_index(attribute, ink);
}

static uint8_t next_layer2_read(const NextMachine *machine, unsigned base_bank,
                                size_t offset)
{
    const unsigned bank = (base_bank + (unsigned)(offset / NEXT_NEX_BANK_SIZE)) & 0x7F;
    const size_t in_bank = offset & (NEXT_NEX_BANK_SIZE - 1);
    const uint8_t page = (uint8_t)(bank * 2 + (in_bank >= NEXT_MMU_SLOT_SIZE));
    return next_runtime_ram_page_const(machine, page)[in_bank & (NEXT_MMU_SLOT_SIZE - 1)];
}

static bool next_layer2_pixel(const NextMachine *machine, int x, int y,
                              uint8_t *index, bool *priority)
{
    const unsigned mode = (machine->nextregs[NEXT_REG_LAYER2_MODE] >> 4) & 3;
    const unsigned base_bank = machine->nextregs[NEXT_REG_LAYER2_BASE] & 0x7F;
    const int width = mode == 2 ? 640 : (mode == 1 ? 320 : 256);
    const int height = mode == 0 ? 192 : 256;
    int sx;
    int sy;
    int clip_x = x;
    size_t offset;
    uint8_t raw;

    if (mode == 3 || x < 0 || y < 0 || x >= width || y >= height)
        return false;
    if (mode == 1)
        clip_x >>= 1;
    else if (mode == 2)
        clip_x >>= 2;
    if (clip_x < machine->layer2_clip[0] || clip_x > machine->layer2_clip[1] ||
        y < machine->layer2_clip[2] || y > machine->layer2_clip[3])
        return false;

    sx = (x + (int)machine->nextregs[0x16] +
          ((int)(machine->nextregs[0x71] & 1) << 8)) %
         width;
    sy = (y + (int)machine->nextregs[0x17]) % height;
    if (mode == 0)
    {
        offset = (size_t)sy * 256 + (size_t)sx;
        raw = next_layer2_read(machine, base_bank, offset);
    }
    else if (mode == 1)
    {
        offset = (size_t)sx * 256 + (size_t)sy;
        raw = next_layer2_read(machine, base_bank, offset);
    }
    else
    {
        offset = (size_t)(sx >> 1) * 256 + (size_t)sy;
        raw = next_layer2_read(machine, base_bank, offset);
        raw = (sx & 1) == 0 ? (uint8_t)(raw >> 4) : (uint8_t)(raw & 0x0F);
    }
    *index = (uint8_t)(((((raw >> 4) +
                          (machine->nextregs[NEXT_REG_LAYER2_MODE] & 0x0F)) &
                         0x0F)
                        << 4) |
                       (raw & 0x0F));
    {
        const unsigned palette =
            (machine->nextregs[NEXT_REG_PALETTE_CONTROL] & 0x04) ? 5 : 1;
        *priority = (machine->hw_palette_priority[palette][*index] & 2) != 0;
    }
    return true;
}

static uint16_t next_classic_palette_rgb(uint8_t index)
{
    const unsigned level = (index & 8) != 0 ? 7 : 6;
    return (uint16_t)(((index & 2) != 0 ? level << 6 : 0) |
                      ((index & 4) != 0 ? level << 3 : 0) |
                      ((index & 1) != 0 ? level : 0));
}

/* True when a 9-bit ULA palette entry holds the standard Spectrum colour for
   classic index 0-15, at either the 5/7 or 6/7 intensity convention.
   NextZXOS also adds one level of green to bright magenta so that it is not
   the default transparent colour. */
static bool next_ula_entry_is_standard(uint16_t entry, uint8_t index)
{
    const unsigned red = (entry >> 6) & 7;
    const unsigned green = (entry >> 3) & 7;
    const unsigned blue = entry & 7;
    const bool bright = (index & 8) != 0;
    const unsigned levels[3] = {red, green, blue};
    const bool present[3] = {(index & 2) != 0, (index & 4) != 0, (index & 1) != 0};

    for (unsigned channel = 0; channel < 3; ++channel)
    {
        const unsigned level = levels[channel];
        if (!present[channel])
        {
            const bool magenta_green = channel == 1 && bright && (index & 7) == 3 && level == 1;
            if (level != 0 && !magenta_green)
                return false;
        }
        else if (bright ? level != 7 : (level != 5 && level != 6))
        {
            return false;
        }
    }
    return true;
}

static uint32_t next_classic_palette_rgba(uint8_t index)
{
    const uint8_t level = (index & 8) != 0 ? 0xFF : 0xCD;
    return 0xFF000000u |
           ((index & 2) != 0 ? (uint32_t)level << 16 : 0) |
           ((index & 4) != 0 ? (uint32_t)level << 8 : 0) |
           ((index & 1) != 0 ? level : 0);
}

static uint8_t next_ula_screen_read(const NextMachine *machine, bool shadow,
                                    unsigned offset)
{
    if (!shadow)
        return machine->ram[5 * NEXT_NEX_BANK_SIZE + (offset & 0x3FFF)];
    return next_runtime_ram_page_const(
        machine, (uint8_t)(0x0E + ((offset & 0x3FFF) >> 13)))[offset & 0x1FFF];
}

static uint8_t next_lores_index(const NextMachine *machine, int x, int y)
{
    const bool radastan = (machine->nextregs[0x6A] & 0x20) != 0;
    const uint8_t px = (uint8_t)(x + machine->nextregs[0x32]);
    const uint16_t y_pre = (uint16_t)(y + machine->nextregs[0x33]) & 0x1FF;
    uint8_t py = (uint8_t)y_pre;
    uint16_t address;
    uint8_t data;

    if (y_pre >= 192)
        py = (uint8_t)(((((y_pre >> 6) + 1) & 3) << 6) | (y_pre & 0x3F));
    if (radastan)
    {
        address = (uint16_t)(((uint16_t)(py >> 1) << 6) | (px >> 2));
        if ((machine->timex_video_mode & 1) != 0)
            address |= 0x2000;
        data = machine->ram[5 * NEXT_NEX_BANK_SIZE + (address & 0x3FFF)];
        return (uint8_t)(((machine->nextregs[0x6A] & 0x0F) << 4) |
                         ((px & 2) == 0 ? data >> 4 : data & 0x0F));
    }
    address = (uint16_t)(((uint16_t)(py >> 1) << 7) | (px >> 1));
    if (py >= 96)
        address = (uint16_t)((((((address >> 11) + 1) & 7) << 11) |
                              (address & 0x7FF)) &
                             0x3FFF);
    data = machine->ram[5 * NEXT_NEX_BANK_SIZE + address];
    return (uint8_t)((((((data >> 4) + (machine->nextregs[0x6A] & 0x0F)) &
                        0x0F)
                       << 4) |
                      (data & 0x0F)));
}

static void next_render_scanline(NextMachine *machine, int y, int width)
{
    const uint8_t tile_control = machine->nextregs[0x6B];
    const bool tile_enabled = (tile_control & 0x80) != 0;
    const bool tile_textmode = (tile_control & 0x08) != 0;
    const bool ula_enabled = (machine->nextregs[0x68] & 0x80) == 0;
    const bool lores_enabled = (machine->nextregs[0x15] & 0x80) != 0;
    const bool layer2_master_enabled = (machine->layer2_control & 0x02) != 0;
    const unsigned l2_mode =
        (machine->nextregs[NEXT_REG_LAYER2_MODE] >> 4) & 3;
    const bool shadow = (machine->port_7ffd & 0x08) != 0;
    const uint8_t timex_mode = machine->timex_video_mode & 7;
    const bool timex_hires = timex_mode == 6;
    const uint8_t hires_attr = next_timex_hires_attribute(machine->timex_video_mode);
    const unsigned ula_palette =
        (machine->nextregs[NEXT_REG_PALETTE_CONTROL] & 0x02) != 0 ? 4 : 0;
    const unsigned layer2_palette =
        (machine->nextregs[NEXT_REG_PALETTE_CONTROL] & 0x04) != 0 ? 5 : 1;
    const unsigned sprite_palette =
        (machine->nextregs[NEXT_REG_PALETTE_CONTROL] & 0x08) != 0 ? 6 : 2;
    const unsigned tile_palette = (tile_control & 0x10) != 0 ? 7 : 3;
    const bool ula_next = (machine->nextregs[NEXT_REG_PALETTE_CONTROL] & 1) != 0;
    uint8_t tm_indices[NEXT_TILEMAP_MAX_WIDTH] = {0};
    uint8_t tm_present[NEXT_TILEMAP_MAX_WIDTH] = {0};
    uint8_t tm_below[NEXT_TILEMAP_MAX_WIDTH] = {0};
    uint8_t sprite_indices[NEXT_SPRITE_DISPLAY_WIDTH];
    uint8_t sprite_present[NEXT_SPRITE_DISPLAY_WIDTH] = {0};
    int tm_width = 0;
    uint32_t *row = machine->framebuffer + (size_t)y * (size_t)width;

    if (tile_enabled)
        tm_width = next_render_tilemap_scanline(machine, y, tm_indices,
                                                tm_present, tm_below, NEXT_TILEMAP_MAX_WIDTH);
    memset(sprite_indices, 0, sizeof(sprite_indices));
    if (machine->sprites.enabled)
        next_sprite_render_scanline(&machine->sprites, y,
                                    machine->nextregs[0x4B], sprite_indices,
                                    sprite_present, NEXT_SPRITE_DISPLAY_WIDTH);

    for (int fb_x = 0; fb_x < width; ++fb_x)
    {
        const int frame_x = width == 640 ? fb_x >> 1 : fb_x;
        const bool border = frame_x < 32 || frame_x >= 288 || y < 32 || y >= 224;
        int paper_x = frame_x - 32;
        int paper_y = y - 32;
        uint8_t ula_index = machine->border_color & 7;
        bool ula_fallback = false;
        bool ula_standard_colour = false;
        uint8_t l2_index = 0;
        uint8_t tile_index = 0;
        uint8_t sprite_index = 0;
        bool ink = false;
        bool l2_priority = false;
        bool layer2_enabled = false;
        bool tile_pixel_enabled = false;
        bool sprite_enabled = false;
        bool ula_clipped = false;
        bool classic_ula_output = false;
        NextMixerInput input;

        if (!border)
        {
            const int ux = (paper_x + machine->nextregs[0x26]) & 0xFF;
            const int uy = (paper_y + machine->nextregs[0x27]) % 192;
            ula_clipped = paper_x < machine->ula_clip[0] ||
                          paper_x > machine->ula_clip[1] || paper_y < machine->ula_clip[2] ||
                          paper_y > machine->ula_clip[3];
            if (lores_enabled && !ula_clipped)
            {
                ula_index = next_lores_index(machine, paper_x, paper_y);
            }
            else if (timex_hires)
            {
                const int hx = width == 640 ? fb_x - 64 : paper_x * 2;
                const unsigned bitmap_row = (unsigned)((uy & 0xC0) << 5) |
                                            (unsigned)((uy & 7) << 8) | (unsigned)((uy & 0x38) << 2);
                const unsigned column = (unsigned)hx >> 3;
                const uint8_t bits = next_ula_screen_read(machine, shadow,
                                                          ((column & 1) != 0 ? 0x2000 : 0) + bitmap_row + (column >> 1));
                ink = (bits & (0x80 >> (hx & 7))) != 0;
                ula_index = next_ula_palette_entry(machine, hires_attr, ink, &ula_fallback);
            }
            else
            {
                const unsigned screen_base = timex_mode == 1 ? 0x2000 : 0;
                const unsigned bitmap_row = (unsigned)((uy & 0xC0) << 5) |
                                            (unsigned)((uy & 7) << 8) | (unsigned)((uy & 0x38) << 2);
                const unsigned column = (unsigned)ux >> 3;
                const uint8_t bits = next_ula_screen_read(
                    machine, shadow, screen_base + bitmap_row + column);
                const uint8_t attr = timex_mode == 2
                                         ? next_ula_screen_read(machine, shadow, 0x2000 + bitmap_row + column)
                                         : next_ula_screen_read(machine, shadow,
                                                                screen_base + 6144 + (unsigned)(uy >> 3) * 32 + column);
                ink = (bits & (0x80 >> (ux & 7))) != 0;
                /* In ULANext mode bit 7 belongs to INK/PAPER, not FLASH. */
                if (!ula_next && (attr & 0x80) != 0 && (machine->frame_count & 16) != 0)
                    ink = !ink;
                ula_index = next_ula_palette_entry(machine, attr, ink, &ula_fallback);
            }
        }
        if (tile_enabled)
        {
            const int tx = tm_width == 640
                               ? (width == 640 ? fb_x : fb_x * 2)
                               : (width == 640 ? fb_x >> 1 : fb_x);
            if (tx >= 0 && tx < tm_width && tm_present[tx] != 0)
            {
                tile_index = tm_indices[tx];
                tile_pixel_enabled = tile_textmode ||
                                     (tile_index & 0x0F) != (machine->nextregs[0x4C] & 0x0F);
            }
        }
        if (machine->sprites.enabled && frame_x >= 0 && frame_x < NEXT_SPRITE_DISPLAY_WIDTH)
        {
            sprite_index = sprite_indices[frame_x];
            sprite_enabled = sprite_present[frame_x] != 0;
        }
        if (layer2_master_enabled)
        {
            const int lx = l2_mode == 2 ? fb_x : (l2_mode == 1 ? frame_x : paper_x);
            const int ly = l2_mode == 0 ? paper_y : y;
            layer2_enabled = next_layer2_pixel(
                machine, lx, ly, &l2_index, &l2_priority);
        }

        memset(&input, 0, sizeof(input));
        if (ula_next && border && !lores_enabled)
        {
            /* The border takes the PAPER colour of the same value. */
            const int ink_bits = next_ulanext_ink_bits(machine->nextregs[0x42]);
            ula_fallback = ink_bits < 0;
            ula_index = (uint8_t)(0x80 + (ula_fallback ? 0 : (machine->border_color & 7)));
        }
        input.ula_rgb = ula_fallback
                            ? next_fallback_color(machine->nextregs[0x4A])
                            : machine->hw_palette[ula_palette][ula_index];
        if (!ula_fallback && !ula_next && !lores_enabled)
        {
            /* The standard ULA looks up INKs at 0-15 and PAPERs (and the
               border) at 16-31. Transparency follows the real entry, while
               standard colours keep the classic Spectrum RGB on screen. */
            const uint8_t palette_index = (uint8_t)((ink ? 0 : 16) + (ula_index & 0x0F));
            input.ula_rgb = machine->hw_palette[ula_palette][palette_index];
            ula_standard_colour = next_ula_entry_is_standard(input.ula_rgb, ula_index & 0x0F);
        }
        input.tile_rgb = machine->hw_palette[tile_palette][tile_index];
        input.sprite_rgb = machine->hw_palette[sprite_palette][sprite_index];
        input.layer2_rgb = machine->hw_palette[layer2_palette][l2_index];
        input.ula_enabled = ula_enabled;
        input.ula_clipped = ula_clipped;
        input.ula_classic_palette = ula_standard_colour;
        input.border = border;
        input.tile_enabled = tile_enabled;
        input.tile_pixel_enabled = tile_pixel_enabled;
        input.tile_textmode = tile_textmode;
        input.tile_below = tile_enabled && tm_width > 0 &&
                           tm_below[tm_width == 640 ? (width == 640 ? fb_x : fb_x * 2) : (width == 640 ? fb_x >> 1 : fb_x)] != 0;
        input.sprite_enabled = sprite_enabled;
        input.layer2_enabled = layer2_enabled;
        /* Only palette entries with the priority bit set (NR$44) are priority. */
        input.layer2_priority = l2_priority;
        input.transparency = machine->nextregs[NEXT_REG_GLOBAL_TRANSPARENCY];
        input.fallback = machine->nextregs[0x4A];
        input.blend_mode = (machine->nextregs[0x68] >> 5) & 3;
        input.priority = (machine->nextregs[0x15] >> 2) & 7;
        input.stencil_mode = (machine->nextregs[0x68] & 1) != 0;
        if (!tile_enabled && !sprite_enabled && !layer2_enabled &&
            ula_standard_colour && ula_enabled && !ula_clipped &&
            (uint8_t)(input.ula_rgb >> 1) != input.transparency)
        {
            row[fb_x] = next_classic_palette_rgba(ula_index & 0x0F);
        }
        else
        {
            const uint16_t color = next_mix_pixel(&input, &classic_ula_output);
            row[fb_x] = classic_ula_output
                            ? next_classic_palette_rgba(ula_index & 0x0F)
                            : next_palette_to_rgba(color);
        }
    }
}

static void next_step_copper_line(NextMachine *machine, uint16_t scanline)
{
    for (int hc = 0; hc < NEXT_COPPER_COLUMNS_PER_LINE; ++hc)
    {
        const int clocks = NEXT_COPPER_CLOCKS_PER_HCOUNT - machine->copper_debt;
        const int spent = next_copper_step(
            machine, &machine->copper_state, scanline, (uint16_t)hc, clocks);
        machine->copper_debt = spent - clocks;
        if (machine->copper_debt < 0)
            machine->copper_debt = 0;
    }
}

static void next_machine_render_frame_internal(
    NextMachine *machine, bool advance_copper)
{
    const unsigned mode = (machine->nextregs[NEXT_REG_LAYER2_MODE] >> 4) & 3;
    const bool tilemap_80_columns = (machine->nextregs[0x6B] & 0xC0) == 0xC0;
    const bool wide = mode == 2 || (machine->timex_video_mode & 7) == 6 ||
                      tilemap_80_columns;
    const int width = wide ? 640 : 320;
    const bool copper_active = advance_copper &&
                               !next_copper_idle(&machine->copper_state);

    machine->framebuffer_width = width;
    for (int y = 0; y < NEXT_DISPLAY_HEIGHT; ++y)
    {
        if (copper_active && y >= 32 && y < 224)
        {
            next_step_copper_line(machine, (uint16_t)(y - 32));
        }
        next_render_scanline(machine, y, width);
    }
    if (copper_active)
    {
        for (int y = 192; y < NEXT_FRAME_LINES; ++y)
        {
            next_step_copper_line(machine, (uint16_t)y);
        }
    }
}

void next_machine_render_frame(NextMachine *machine)
{
    next_machine_render_frame_internal(machine, false);
}

bool next_machine_load_nex_data(
    NextMachine *machine,
    const uint8_t *data,
    size_t data_size,
    NextNexLoadResult *result,
    char *error_buffer,
    size_t error_buffer_size)
{
    NextNexVersion version;
    bool has_palette;
    bool has_copper;
    size_t screen_size;
    size_t offset = NEXT_NEX_HEADER_SIZE;
    unsigned bank_count;
    unsigned loaded = 0;

    if (machine == NULL || !machine->initialized || machine->ram == NULL ||
        data == NULL || result == NULL)
    {
        next_set_error(error_buffer, error_buffer_size, "Invalid NEX load request.");
        return false;
    }
    if (data_size < NEXT_NEX_HEADER_SIZE || memcmp(data, "Next", 4) != 0)
    {
        next_set_error(error_buffer, error_buffer_size, "Not a valid NEX file.");
        return false;
    }
    if (!next_nex_version(data, &version))
    {
        next_set_error(error_buffer, error_buffer_size, "Unsupported NEX version.");
        return false;
    }
    if (data[8] > 1 || data[9] > NEXT_NEX_BANK_COUNT)
    {
        next_set_error(error_buffer, error_buffer_size, "Invalid NEX memory requirements.");
        return false;
    }

    bank_count = next_nex_flagged_bank_count(data);
    /* Some programs shipped on the official System/Next image have a stale
       byte-9 count but a correct bank bitmap and payload. The distro loader
       follows the bitmap, so do the same for compatibility. */

    screen_size = next_nex_screen_size(data, version, &has_palette, &has_copper);
    if (has_palette && !next_add_size(&offset, NEXT_NEX_PALETTE_SIZE, data_size))
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX palette.");
        return false;
    }
    if (!next_add_size(&offset, screen_size, data_size))
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX loading screen.");
        return false;
    }
    if (has_copper && !next_add_size(&offset, NEXT_COPPER_SIZE, data_size))
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX copper program.");
        return false;
    }

    if (version == NEXT_NEX_VERSION_1_3)
    {
        const uint32_t declared_offset = next_read_u32(data + 144);
        if (declared_offset != 0 && declared_offset != offset)
        {
            next_set_error(error_buffer, error_buffer_size,
                           "NEX bank-data offset does not match its optional blocks.");
            return false;
        }
    }
    if ((size_t)bank_count * NEXT_NEX_BANK_SIZE > data_size - offset)
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX bank data.");
        return false;
    }

    if (data[134] == 0)
    {
        next_machine_reset(machine);
        /* NEX programs are launched by NextZXOS with the upper ROM-select bit
           set. Programs such as Spectron then select the 48K ROM by writing
           bit 4 to port 7FFD and expect physical ROM 3. */
        machine->port_1ffd = 0x04;
        machine->rom_bank = 2;
        machine->loader_active = false;
        machine->config_mode = false;
        machine->nextregs[0x0A] &= (uint8_t)~0x10;
    }
    if (has_palette)
    {
        memcpy(machine->palette, data + NEXT_NEX_HEADER_SIZE, NEXT_NEX_PALETTE_SIZE);
        machine->palette_loaded = true;
        for (unsigned index = 0; index < 256; ++index)
        {
            machine->hw_palette[1][index] = (uint16_t)(((uint16_t)machine->palette[index * 2] << 1) |
                                                       (machine->palette[index * 2 + 1] & 1));
            machine->hw_palette_priority[1][index] =
                (uint8_t)((machine->palette[index * 2 + 1] >> 6) & 3);
        }
    }

    {
        size_t screen_offset = NEXT_NEX_HEADER_SIZE +
                               (has_palette ? NEXT_NEX_PALETTE_SIZE : 0);
        next_copy_loading_screens(machine, data, &screen_offset, data, version);
        if (has_copper)
        {
            memcpy(machine->copper, data + screen_offset, NEXT_COPPER_SIZE);
            next_copper_set_write_ptr_low(&machine->copper_state, 0);
            next_copper_set_write_ptr_high_and_mode(&machine->copper_state, 0);
            for (size_t i = 0; i < NEXT_COPPER_SIZE; ++i)
            {
                next_copper_write_data(&machine->copper_state, data[screen_offset + i]);
            }
            next_copper_set_write_ptr_low(&machine->copper_state, 0);
            next_copper_set_write_ptr_high_and_mode(
                &machine->copper_state,
                (uint8_t)(NEXT_COPPER_START_ON_VBL << 6));
            machine->copper_loaded = true;
        }
    }

    {
        static const uint8_t first_banks[] = {5, 2, 0, 1, 3, 4};
        for (unsigned order = 0; order < NEXT_NEX_BANK_COUNT; ++order)
        {
            const unsigned bank = order < sizeof(first_banks)
                                      ? first_banks[order]
                                      : order;
            uint8_t *dest;
            if (data[18 + bank] == 0)
                continue;
            dest = machine->ram + (size_t)bank * NEXT_NEX_BANK_SIZE;
            memcpy(dest,
                   data + offset + (size_t)loaded * NEXT_NEX_BANK_SIZE,
                   NEXT_NEX_BANK_SIZE);
            ++loaded;
        }
    }

    machine->initial_sp = next_read_u16(data + 12);
    machine->initial_pc = next_read_u16(data + 14);
    machine->mmu[6] = (uint8_t)(data[139] * 2);
    machine->mmu[7] = (uint8_t)(data[139] * 2 + 1);
    machine->nextregs[NEXT_REG_MMU0 + 6] = machine->mmu[6];
    machine->nextregs[NEXT_REG_MMU0 + 7] = machine->mmu[7];
    if ((data[10] & 0x01) != 0)
    {
        machine->nextregs[NEXT_REG_LAYER2_BASE] = 9;
        machine->layer2_control |= 0x02;
    }
    if (version == NEXT_NEX_VERSION_1_3 && (data[10] & 0x40) != 0)
    {
        if (data[152] == 1)
            machine->nextregs[NEXT_REG_LAYER2_MODE] = 0x10 | (data[138] & 0x0F);
        else if (data[152] == 2)
            machine->nextregs[NEXT_REG_LAYER2_MODE] = 0x20 | (data[138] & 0x0F);
        if (data[152] == 1 || data[152] == 2)
        {
            machine->nextregs[NEXT_REG_LAYER2_BASE] = 9;
            machine->layer2_control |= 0x02;
        }
    }
    machine->cpu_pins = z80_reset(&machine->cpu);
    machine->cpu.sp = machine->initial_sp;
    machine->cpu_pins = z80_prefetch(&machine->cpu, machine->initial_pc);

    memset(result, 0, sizeof(*result));
    result->version = version;
    result->stack_pointer = machine->initial_sp;
    result->program_counter = machine->initial_pc;
    result->file_handle_address = next_read_u16(data + 140);
    result->entry_bank = data[139];
    result->border_color = data[11] & 0x07;
    result->banks_loaded = (uint8_t)loaded;
    result->preserve_nextregs = data[134] != 0;
    result->runnable = machine->initial_pc != 0;
    result->appended_data_offset = offset + (size_t)loaded * NEXT_NEX_BANK_SIZE;
    return true;
}

bool next_machine_load_snx_data(
    NextMachine *machine,
    const uint8_t *data,
    size_t data_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    static const size_t header_size = 27;
    static const size_t bank_size = 0x4000;
    static const size_t snx_48k_size = 27 + 3 * 0x4000;
    static const size_t snx_trailer_size = 4;
    static const size_t snx_128k_size =
        snx_48k_size + snx_trailer_size + 5 * 0x4000;
    uint8_t snapshot_mmu[NEXT_MMU_SLOT_COUNT] = {
        0xFF, 0xFF, 10, 11, 4, 5, 0, 1};
    uint8_t paged_page[0x4000];
    uint16_t stack_pointer;
    uint16_t program_counter;
    uint8_t selected_bank = 0;
    bool is_128k;

    /* A 128K snapshot stores the five remaining banks, or six when the paged
       bank is 2 or 5 (that bank then also appears in the 48K section). */
    if (machine == NULL || !machine->initialized || data == NULL ||
        (data_size != snx_48k_size && data_size != snx_128k_size &&
         data_size != snx_128k_size + bank_size))
    {
        next_set_error(error_buffer, error_buffer_size,
                       "Unsupported or corrupt SNX snapshot.");
        return false;
    }
    if (data[25] > 2)
    {
        next_set_error(error_buffer, error_buffer_size,
                       "Invalid interrupt mode in SNX snapshot.");
        return false;
    }
    stack_pointer = next_read_u16(data + 23);
    if (stack_pointer < 0x4000 || stack_pointer == 0xFFFF)
    {
        next_set_error(error_buffer, error_buffer_size,
                       "Invalid SNX program-counter stack address.");
        return false;
    }

    is_128k = data_size != snx_48k_size;
    if (is_128k)
    {
        const uint8_t *trailer = data + snx_48k_size;
        program_counter = next_read_u16(trailer);
        selected_bank = (uint8_t)(trailer[2] & 0x07);
        if (data_size != ((selected_bank == 2 || selected_bank == 5)
                              ? snx_128k_size + bank_size
                              : snx_128k_size))
        {
            next_set_error(error_buffer, error_buffer_size,
                           "128K SNX snapshot has the wrong RAM-bank count.");
            return false;
        }
        snapshot_mmu[6] = (uint8_t)(selected_bank * 2);
        snapshot_mmu[7] = (uint8_t)(selected_bank * 2 + 1);
    }

    /* An SNX is a SNA whose surrounding NextZXOS/divMMC state must stay
       alive.  Replace only the legacy address space and CPU state; the
       SD controller, divMMC RAM and open filesystem context are preserved. */
    memcpy(machine->ram + 5 * NEXT_NEX_BANK_SIZE,
           data + header_size, bank_size);
    memcpy(machine->ram + 4 * NEXT_MMU_SLOT_SIZE,
           data + header_size + bank_size, bank_size);
    memcpy(paged_page, data + header_size + 2 * bank_size, bank_size);

    if (is_128k)
    {
        const uint8_t *trailer = data + snx_48k_size;
        size_t offset = snx_48k_size + snx_trailer_size;

        memcpy(next_runtime_ram_page(machine, (uint8_t)(selected_bank * 2)),
               paged_page, NEXT_MMU_SLOT_SIZE);
        memcpy(next_runtime_ram_page(machine, (uint8_t)(selected_bank * 2 + 1)),
               paged_page + NEXT_MMU_SLOT_SIZE, NEXT_MMU_SLOT_SIZE);
        for (uint8_t bank = 0; bank < 8; ++bank)
        {
            if (bank == 5 || bank == 2 || bank == selected_bank)
                continue;
            memcpy(next_runtime_ram_page(machine, (uint8_t)(bank * 2)),
                   data + offset, NEXT_MMU_SLOT_SIZE);
            memcpy(next_runtime_ram_page(machine, (uint8_t)(bank * 2 + 1)),
                   data + offset + NEXT_MMU_SLOT_SIZE, NEXT_MMU_SLOT_SIZE);
            offset += bank_size;
        }
        machine->port_7ffd = trailer[2];
    }
    else
    {
        memcpy(machine->ram, paged_page, bank_size);
        machine->port_7ffd = 0x10;
    }

    memcpy(machine->mmu, snapshot_mmu, sizeof(snapshot_mmu));
    for (unsigned slot = 0; slot < NEXT_MMU_SLOT_COUNT; ++slot)
        machine->nextregs[NEXT_REG_MMU0 + slot] = snapshot_mmu[slot];

    machine->port_1ffd = 0x04;
    machine->port_dffd = 0;
    machine->paging_locked = false;
    machine->rom_bank = (uint8_t)(((machine->port_1ffd & 0x04) >> 1) |
                                  ((machine->port_7ffd & 0x10) >> 4));
    machine->loader_active = false;
    machine->config_mode = false;
    machine->divmmc_automap = false;
    machine->divmmc_automap_pending = false;
    machine->border_color = data[26] & 7;

    machine->cpu_pins = z80_reset(&machine->cpu);
    machine->cpu.i = data[0];
    machine->cpu.hl2 = next_read_u16(data + 1);
    machine->cpu.de2 = next_read_u16(data + 3);
    machine->cpu.bc2 = next_read_u16(data + 5);
    machine->cpu.af2 = next_read_u16(data + 7);
    machine->cpu.hl = next_read_u16(data + 9);
    machine->cpu.de = next_read_u16(data + 11);
    machine->cpu.bc = next_read_u16(data + 13);
    machine->cpu.iy = next_read_u16(data + 15);
    machine->cpu.ix = next_read_u16(data + 17);
    machine->cpu.iff2 = (data[19] & 0x04) != 0;
    machine->cpu.iff1 = machine->cpu.iff2;
    machine->cpu.r = data[20];
    machine->cpu.af = next_read_u16(data + 21);
    machine->cpu.sp = stack_pointer;
    machine->cpu.im = data[25];
    if (!is_128k)
    {
        program_counter =
            (uint16_t)(next_machine_read_memory(machine, stack_pointer) |
                       ((uint16_t)next_machine_read_memory(
                            machine, (uint16_t)(stack_pointer + 1))
                        << 8));
        machine->cpu.sp = (uint16_t)(stack_pointer + 2);
    }
    machine->initial_pc = program_counter;
    machine->initial_sp = machine->cpu.sp;
    machine->cpu_pins = z80_prefetch(&machine->cpu, program_counter);
    z80_set_z80n_enabled(&machine->cpu, true);
    z80_set_z80n_nextreg_callback(&machine->cpu,
                                  next_z80n_write_reg, machine);
    return true;
}

/* Writes one 16K NEX bank into the unified physical RAM pool. */
static void next_store_pool_bank(
    NextMachine *machine, unsigned bank, const uint8_t *data)
{
    const uint8_t lower = (uint8_t)(bank * 2);
    const uint8_t upper = (uint8_t)(bank * 2 + 1);

    memcpy(next_runtime_ram_page(machine, lower), data, NEXT_MMU_SLOT_SIZE);
    memcpy(next_runtime_ram_page(machine, upper),
           data + NEXT_MMU_SLOT_SIZE, NEXT_MMU_SLOT_SIZE);
}

bool next_machine_load_nex_direct(
    NextMachine *machine,
    const uint8_t *data,
    size_t data_size,
    NextNexLoadResult *result,
    char *error_buffer,
    size_t error_buffer_size)
{
    NextNexVersion version;
    bool has_palette;
    bool has_copper;
    size_t screen_size;
    size_t offset = NEXT_NEX_HEADER_SIZE;
    unsigned bank_count;
    unsigned loaded = 0;

    if (machine == NULL || !machine->initialized || machine->ram == NULL ||
        data == NULL || result == NULL)
    {
        next_set_error(error_buffer, error_buffer_size, "Invalid NEX load request.");
        return false;
    }
    if (data_size < NEXT_NEX_HEADER_SIZE || memcmp(data, "Next", 4) != 0)
    {
        next_set_error(error_buffer, error_buffer_size, "Not a valid NEX file.");
        return false;
    }
    if (!next_nex_version(data, &version))
    {
        next_set_error(error_buffer, error_buffer_size, "Unsupported NEX version.");
        return false;
    }

    bank_count = next_nex_flagged_bank_count(data);
    screen_size = next_nex_screen_size(data, version, &has_palette, &has_copper);
    if (has_palette && !next_add_size(&offset, NEXT_NEX_PALETTE_SIZE, data_size))
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX palette.");
        return false;
    }
    if (!next_add_size(&offset, screen_size, data_size))
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX loading screen.");
        return false;
    }
    if (has_copper && !next_add_size(&offset, NEXT_COPPER_SIZE, data_size))
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX copper program.");
        return false;
    }
    if ((size_t)bank_count * NEXT_NEX_BANK_SIZE > data_size - offset)
    {
        next_set_error(error_buffer, error_buffer_size, "Truncated NEX bank data.");
        return false;
    }

    {
        static const uint8_t first_banks[] = {5, 2, 0, 1, 3, 4};
        for (unsigned order = 0; order < NEXT_NEX_BANK_COUNT; ++order)
        {
            const unsigned bank = order < sizeof(first_banks)
                                      ? first_banks[order]
                                      : order;
            if (data[18 + bank] == 0)
                continue;
            next_store_pool_bank(
                machine, bank,
                data + offset + (size_t)loaded * NEXT_NEX_BANK_SIZE);
            ++loaded;
        }
    }

    machine->initial_sp = next_read_u16(data + 12);
    machine->initial_pc = next_read_u16(data + 14);
    machine->cpu_pins = z80_reset(&machine->cpu);
    machine->cpu.sp = machine->initial_sp;
    machine->cpu_pins = z80_prefetch(&machine->cpu, machine->initial_pc);

    /* The entry bank is paged at 0xC000 through the classic 7FFD port. */
    {
        const uint8_t entry = (uint8_t)(data[139] & 0x07);
        machine->port_7ffd = (uint8_t)((machine->port_7ffd & 0xF8) | entry);
        next_set_legacy_mmu_page(machine, 6, (uint8_t)(entry * 2));
        next_set_legacy_mmu_page(machine, 7, (uint8_t)(entry * 2 + 1));
    }

    memset(result, 0, sizeof(*result));
    result->version = version;
    result->stack_pointer = machine->initial_sp;
    result->program_counter = machine->initial_pc;
    result->file_handle_address = next_read_u16(data + 140);
    result->entry_bank = data[139];
    result->border_color = data[11] & 0x07;
    result->banks_loaded = (uint8_t)loaded;
    result->preserve_nextregs = data[134] != 0;
    result->runnable = machine->initial_pc != 0;
    result->appended_data_offset = offset + (size_t)loaded * NEXT_NEX_BANK_SIZE;
    return true;
}
