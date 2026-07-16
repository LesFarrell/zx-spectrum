#include "tape.h"

#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/mem.h"
#include "chips/kbd.h"
#include "chips/clk.h"
#include "systems/zx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TAPE_TZX_HEADER_SIZE = 10,
    TAPE_TZX_LOOP_DEPTH = 16,
    TAPE_TZX_MAX_BLOCK_EXECUTIONS = 1000000,
    TAPE_PILOT_PULSE_TICKS = 2168,
    TAPE_SYNC1_PULSE_TICKS = 667,
    TAPE_SYNC2_PULSE_TICKS = 735,
    TAPE_ZERO_PULSE_TICKS = 855,
    TAPE_ONE_PULSE_TICKS = 1710,
    TAPE_HEADER_PILOT_PULSES = 8063,
    TAPE_DATA_PILOT_PULSES = 3223,
    TAPE_TAP_BLOCK_PAUSE_MS = 1000,
    TAPE_DECODE_BASE_WORK = 65536,
    TAPE_DECODE_WORK_PER_BYTE = 512,
    TAPE_MAX_DECODE_WORK = 8 * 1024 * 1024,
    TAPE_MAX_SEGMENTS = 8 * 1024 * 1024
};

typedef struct TapeBuilder {
    TapePlayer *player;
    bool current_level;
    size_t work_remaining;
} TapeBuilder;

typedef struct TapeTzxLoop {
    size_t body_block_index;
    uint16_t remaining;
} TapeTzxLoop;

typedef struct TapeTzxDirectory {
    size_t *offsets;
    size_t count;
    size_t capacity;
} TapeTzxDirectory;

typedef struct TapeTzxCall {
    const uint8_t *offsets;
    size_t call_block_index;
    size_t return_block_index;
    uint16_t count;
    uint16_t next;
    bool active;
} TapeTzxCall;

static bool tape_is_standard_header_block(const TapeBlock *block, uint8_t *type_out) {
    if (block == NULL || block->length != 19 || block->bytes == NULL || block->bytes[0] != 0x00) {
        return false;
    }
    if (type_out != NULL) {
        *type_out = block->bytes[1];
    }
    return true;
}

static bool tape_program_uses_128_basic_tokens(const TapeBlock *block) {
    if (block == NULL || block->length < 3 || block->bytes == NULL || block->bytes[0] != 0xFF) {
        return false;
    }

    for (size_t i = 1; i + 1 < block->length; ++i) {
        if (block->bytes[i] == 0xA3 || block->bytes[i] == 0xA4) {
            return true;
        }
    }
    return false;
}

/* Classifies the likely ROM-side autoload path from the first indexed blocks.
   Ambiguous tapes fall back to 48 BASIC because most TAP/TZX program images
   use a standard 48K loader even when opened from a 128K-capable frontend. */
static TapeAutoloadTarget tape_detect_autoload_target(const TapePlayer *player) {
    uint8_t first_type;

    if (player == NULL) {
        return TAPE_AUTOLOAD_TARGET_UNKNOWN;
    }
    if (player->autoload_target == TAPE_AUTOLOAD_TARGET_128_MENU) {
        return TAPE_AUTOLOAD_TARGET_128_MENU;
    }
    if (player->block_count == 0) {
        return TAPE_AUTOLOAD_TARGET_UNKNOWN;
    }
    if (!tape_is_standard_header_block(&player->blocks[0], &first_type)) {
        return TAPE_AUTOLOAD_TARGET_48_BASIC;
    }
    if (first_type != 0x00) {
        return TAPE_AUTOLOAD_TARGET_48_BASIC;
    }
    if (player->block_count > 1 && tape_program_uses_128_basic_tokens(&player->blocks[1])) {
        return TAPE_AUTOLOAD_TARGET_128_MENU;
    }
    return TAPE_AUTOLOAD_TARGET_48_BASIC;
}

static bool tape_reserve_blocks(TapePlayer *player, size_t additional_count) {
    TapeBlock *blocks;
    size_t required;
    size_t new_capacity;

    if (additional_count > SIZE_MAX - player->block_count) {
        return false;
    }
    required = player->block_count + additional_count;
    if (required <= player->block_capacity) {
        return true;
    }

    new_capacity = (player->block_capacity > 0) ? player->block_capacity : 64;
    while (new_capacity < required) {
        if (new_capacity > (SIZE_MAX / 2)) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    blocks = (TapeBlock *)realloc(player->blocks, new_capacity * sizeof(TapeBlock));
    if (blocks == NULL) {
        return false;
    }
    player->blocks = blocks;
    player->block_capacity = new_capacity;
    return true;
}

static bool tape_append_fast_block(TapePlayer *player, const uint8_t *data, size_t length) {
    TapeBlock *block;

    if (!tape_reserve_blocks(player, 1)) {
        return false;
    }
    block = &player->blocks[player->block_count++];
    block->bytes = (uint8_t *)malloc(length);
    if (block->bytes == NULL) {
        player->block_count--;
        return false;
    }
    memcpy(block->bytes, data, length);
    block->length = length;
    return true;
}

static void tape_set_error(char *buffer, size_t buffer_size, const char *message, const char *path) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }
    if (path != NULL && path[0] != '\0') {
        snprintf(buffer, buffer_size, "%s: %s", message, path);
    } else {
        snprintf(buffer, buffer_size, "%s", message);
    }
}

static uint16_t tape_read_u16(const uint8_t *ptr) {
    return (uint16_t)(ptr[0] | (ptr[1] << 8));
}

static uint32_t tape_read_u24(const uint8_t *ptr) {
    return (uint32_t)(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16));
}

static uint32_t tape_read_u32(const uint8_t *ptr) {
    return (uint32_t)(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
}

static uint8_t tape_cp_flags(uint8_t left, uint8_t right) {
    const uint8_t result = (uint8_t)(left - right);
    uint8_t flags = Z80_NF | (result & (Z80_SF | Z80_YF | Z80_XF));

    if (result == 0) {
        flags |= Z80_ZF;
    }
    if (((left ^ right ^ result) & 0x10u) != 0) {
        flags |= Z80_HF;
    }
    if (((left ^ right) & (left ^ result) & 0x80u) != 0) {
        flags |= Z80_PF;
    }
    if (left < right) {
        flags |= Z80_CF;
    }
    return flags;
}

static bool tape_has_bytes(size_t offset, size_t count, size_t total) {
    return offset <= total && count <= (total - offset);
}

static uint32_t tape_ms_to_ticks(uint32_t tick_hz, uint16_t duration_ms) {
    if (duration_ms == 0 || tick_hz == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)duration_ms * (uint64_t)tick_hz + 999u) / 1000u);
}

static bool tape_tstates_to_ticks(uint32_t tick_hz, uint32_t tstates, uint32_t *ticks_out) {
    uint64_t ticks;
    if (tstates == 0 || ticks_out == NULL) {
        return false;
    }
    ticks = ((uint64_t)tstates * tick_hz + 1749999u) / 3500000u;
    if (ticks == 0) {
        ticks = 1;
    }
    if (ticks > UINT32_MAX) {
        return false;
    }
    *ticks_out = (uint32_t)ticks;
    return true;
}

static bool tape_reserve_segments(TapePlayer *player, size_t additional_count) {
    TapeSegment *segments;
    size_t required;
    size_t new_capacity;

    if (additional_count > SIZE_MAX - player->segment_count) {
        return false;
    }
    required = player->segment_count + additional_count;
    if (required > TAPE_MAX_SEGMENTS) {
        return false;
    }
    if (required <= player->segment_capacity) {
        return true;
    }

    new_capacity = (player->segment_capacity > 0) ? player->segment_capacity : 1024;
    while (new_capacity < required) {
        if (new_capacity > (SIZE_MAX / 2)) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2;
    }

    segments = (TapeSegment *)realloc(player->segments, new_capacity * sizeof(TapeSegment));
    if (segments == NULL) {
        return false;
    }
    player->segments = segments;
    player->segment_capacity = new_capacity;
    return true;
}

/* Limits decoded waveform work relative to the source size while retaining a
   generous allowance for short standard blocks and pilot tones. */
static size_t tape_decode_work_budget(size_t source_size) {
    if (source_size >= (TAPE_MAX_DECODE_WORK - TAPE_DECODE_BASE_WORK) /
            TAPE_DECODE_WORK_PER_BYTE) {
        return TAPE_MAX_DECODE_WORK;
    }
    return TAPE_DECODE_BASE_WORK + (source_size * TAPE_DECODE_WORK_PER_BYTE);
}

static bool tape_consume_work(TapeBuilder *builder, size_t amount) {
    if (amount > builder->work_remaining) {
        return false;
    }
    builder->work_remaining -= amount;
    return true;
}

static bool tape_append_level(TapePlayer *player, uint32_t duration_ticks, bool level_high) {
    TapeSegment *segment;

    if (duration_ticks == 0) {
        return true;
    }
    if (player->segment_count > 0) {
        segment = &player->segments[player->segment_count - 1];
        if (segment->level_high == level_high && segment->stop_after == TAPE_STOP_NONE) {
            if (segment->duration_ticks > UINT32_MAX - duration_ticks) {
                return false;
            }
            segment->duration_ticks += duration_ticks;
            return true;
        }
    }
    if (!tape_reserve_segments(player, 1)) {
        return false;
    }
    segment = &player->segments[player->segment_count++];
    segment->duration_ticks = duration_ticks;
    segment->level_high = level_high;
    segment->stop_after = TAPE_STOP_NONE;
    return true;
}

static bool tape_mark_stop(TapePlayer *player, TapeStopMode stop_mode) {
    TapeSegment *segment;

    if (player->segment_count > 0) {
        segment = &player->segments[player->segment_count - 1];
        if (segment->duration_ticks == 0) {
            if (stop_mode == TAPE_STOP_ALWAYS || segment->stop_after == TAPE_STOP_NONE) {
                segment->stop_after = stop_mode;
            }
            return true;
        }
    }
    if (!tape_reserve_segments(player, 1)) {
        return false;
    }
    segment = &player->segments[player->segment_count++];
    segment->duration_ticks = 0;
    segment->level_high = false;
    segment->stop_after = stop_mode;
    return true;
}

static bool tape_append_pulse(TapeBuilder *builder, uint32_t duration_ticks) {
    uint32_t machine_ticks;
    if (!tape_consume_work(builder, 1) ||
        !tape_tstates_to_ticks(builder->player->tick_hz, duration_ticks, &machine_ticks)) {
        return false;
    }
    builder->current_level = !builder->current_level;
    return tape_append_level(builder->player, machine_ticks, builder->current_level);
}

static bool tape_append_pause(TapeBuilder *builder, uint16_t duration_ms) {
    uint32_t duration_ticks;
    uint32_t finishing_ticks;

    if (duration_ms == 0) {
        return true;
    }
    if (!tape_consume_work(builder, 2)) {
        return false;
    }

    duration_ticks = tape_ms_to_ticks(builder->player->tick_hz, duration_ms);
    finishing_ticks = tape_ms_to_ticks(builder->player->tick_hz, 1);
    if (finishing_ticks > duration_ticks) {
        finishing_ticks = duration_ticks;
    }

    /* TZX pauses first finish the preceding pulse at the opposite level for
       at least 1 ms, then hold EAR low for the rest of the pause. */
    builder->current_level = !builder->current_level;
    if (!tape_append_level(builder->player, finishing_ticks, builder->current_level)) {
        return false;
    }
    builder->current_level = false;
    return tape_append_level(builder->player, duration_ticks - finishing_ticks, false);
}

static bool tape_append_direct_samples(
    TapeBuilder *builder,
    const uint8_t *data,
    size_t length,
    uint8_t used_bits_last,
    uint16_t sample_ticks
) {
    if (length == 0) {
        return true;
    }
    if (sample_ticks == 0 || used_bits_last == 0 || used_bits_last > 8) {
        return false;
    }

    for (size_t index = 0; index < length; ++index) {
        const int bits_in_byte = (index + 1 == length) ? used_bits_last : 8;
        for (int bit = 7; bit >= 8 - bits_in_byte; --bit) {
            uint32_t machine_ticks;
            builder->current_level = (data[index] & (1u << bit)) != 0;
            if (!tape_consume_work(builder, 1) ||
                !tape_tstates_to_ticks(builder->player->tick_hz, sample_ticks, &machine_ticks) ||
                !tape_append_level(builder->player, machine_ticks, builder->current_level)) {
                return false;
            }
        }
    }
    return true;
}

static bool tape_append_generalized_symbol(
    TapeBuilder *builder,
    const uint8_t *definition,
    uint8_t max_pulses
) {
    const uint8_t polarity = definition[0] & 0x03u;

    if (polarity == 0) {
        builder->current_level = !builder->current_level;
    } else if (polarity == 2) {
        builder->current_level = false;
    } else if (polarity == 3) {
        builder->current_level = true;
    }

    for (uint8_t pulse_index = 0; pulse_index < max_pulses; ++pulse_index) {
        const uint16_t pulse_ticks = tape_read_u16(&definition[1u + ((size_t)pulse_index * 2u)]);
        uint32_t machine_ticks;
        if (pulse_ticks == 0) {
            break;
        }
        if (pulse_index > 0) {
            builder->current_level = !builder->current_level;
        }
        if (!tape_consume_work(builder, 1) ||
            !tape_tstates_to_ticks(builder->player->tick_hz, pulse_ticks, &machine_ticks) ||
            !tape_append_level(builder->player, machine_ticks, builder->current_level)) {
            return false;
        }
    }
    return true;
}

static bool tape_append_csw_rle(
    TapeBuilder *builder,
    const uint8_t *data,
    size_t length,
    uint32_t sample_rate,
    uint32_t expected_pulses
) {
    size_t offset = 0;
    uint32_t pulse_count = 0;

    if (sample_rate == 0) {
        return false;
    }
    while (offset < length) {
        uint32_t samples = data[offset++];
        uint64_t scaled_ticks;

        if (samples == 0) {
            if (!tape_has_bytes(offset, 4, length)) {
                return false;
            }
            samples = tape_read_u32(&data[offset]);
            offset += 4;
        }
        if (samples == 0) {
            return false;
        }
        scaled_ticks = ((uint64_t)samples * 3500000u + (sample_rate / 2u)) / sample_rate;
        if (scaled_ticks == 0) {
            scaled_ticks = 1;
        }
        if (scaled_ticks > UINT32_MAX || !tape_append_pulse(builder, (uint32_t)scaled_ticks)) {
            return false;
        }
        pulse_count++;
    }
    return pulse_count == expected_pulses;
}

static bool tape_append_data_bits(
    TapeBuilder *builder,
    const uint8_t *data,
    size_t length,
    uint8_t used_bits_last,
    uint16_t zero_pulse_ticks,
    uint16_t one_pulse_ticks
) {
    size_t index;

    if (length == 0) {
        return true;
    }

    for (index = 0; index < length; ++index) {
        int bits_in_byte = 8;
        if (index + 1 == length && used_bits_last > 0 && used_bits_last < 8) {
            bits_in_byte = used_bits_last;
        }

        for (int bit = 7; bit >= (8 - bits_in_byte); --bit) {
            const uint16_t pulse_ticks = (data[index] & (1u << bit)) ? one_pulse_ticks : zero_pulse_ticks;
            if (!tape_append_pulse(builder, pulse_ticks) || !tape_append_pulse(builder, pulse_ticks)) {
                return false;
            }
        }
    }
    return true;
}

static bool tape_append_standard_block(
    TapeBuilder *builder,
    const uint8_t *data,
    size_t length,
    uint16_t pause_ms
) {
    uint16_t pilot_count;

    pilot_count = (length > 0 && data[0] < 0x80) ? TAPE_HEADER_PILOT_PULSES : TAPE_DATA_PILOT_PULSES;
    for (uint16_t pulse_index = 0; pulse_index < pilot_count; ++pulse_index) {
        if (!tape_append_pulse(builder, TAPE_PILOT_PULSE_TICKS)) {
            return false;
        }
    }
    if (!tape_append_pulse(builder, TAPE_SYNC1_PULSE_TICKS) ||
        !tape_append_pulse(builder, TAPE_SYNC2_PULSE_TICKS)) {
        return false;
    }
    if (!tape_append_data_bits(builder, data, length, 8, TAPE_ZERO_PULSE_TICKS, TAPE_ONE_PULSE_TICKS)) {
        return false;
    }
    if (pause_ms > 0) {
        return tape_append_pause(builder, pause_ms);
    }
    return true;
}

static bool tape_append_turbo_block(
    TapeBuilder *builder,
    const uint8_t *data,
    size_t length,
    uint16_t pilot_pulse_ticks,
    uint16_t sync1_pulse_ticks,
    uint16_t sync2_pulse_ticks,
    uint16_t zero_pulse_ticks,
    uint16_t one_pulse_ticks,
    uint16_t pilot_count,
    uint8_t used_bits_last,
    uint16_t pause_ms
) {
    for (uint16_t pulse_index = 0; pulse_index < pilot_count; ++pulse_index) {
        if (!tape_append_pulse(builder, pilot_pulse_ticks)) {
            return false;
        }
    }
    if (!tape_append_pulse(builder, sync1_pulse_ticks) ||
        !tape_append_pulse(builder, sync2_pulse_ticks)) {
        return false;
    }
    if (!tape_append_data_bits(builder, data, length, used_bits_last, zero_pulse_ticks, one_pulse_ticks)) {
        return false;
    }
    if (pause_ms > 0) {
        return tape_append_pause(builder, pause_ms);
    }
    return true;
}

static bool tape_parse_tap(
    TapePlayer *player,
    const uint8_t *data,
    size_t size,
    char *error_buffer,
    size_t error_buffer_size,
    const char *path
) {
    TapeBuilder builder;
    size_t offset = 0;

    builder.player = player;
    builder.current_level = false;
    builder.work_remaining = tape_decode_work_budget(size);

    while (offset < size) {
        uint16_t block_length;

        if (!tape_has_bytes(offset, 2, size)) {
            tape_set_error(error_buffer, error_buffer_size, "Corrupt TAP block header", path);
            return false;
        }
        block_length = tape_read_u16(&data[offset]);
        offset += 2;
        if (!tape_has_bytes(offset, block_length, size)) {
            tape_set_error(error_buffer, error_buffer_size, "Truncated TAP block", path);
            return false;
        }
        if (!tape_append_standard_block(&builder, &data[offset], block_length, TAPE_TAP_BLOCK_PAUSE_MS)) {
            tape_set_error(error_buffer, error_buffer_size, "Out of memory while decoding TAP file", path);
            return false;
        }
        if (!tape_append_fast_block(player, &data[offset], block_length)) {
            tape_set_error(error_buffer, error_buffer_size, "Out of memory while indexing TAP blocks", path);
            return false;
        }
        offset += block_length;
    }
    return true;
}

static bool tape_tzx_directory_append(TapeTzxDirectory *directory, size_t offset) {
    size_t new_capacity;
    size_t *new_offsets;

    if (directory->count < directory->capacity) {
        directory->offsets[directory->count++] = offset;
        return true;
    }
    new_capacity = directory->capacity > 0 ? directory->capacity * 2u : 128u;
    if (new_capacity < directory->capacity || new_capacity > SIZE_MAX / sizeof(size_t)) {
        return false;
    }
    new_offsets = (size_t *)realloc(directory->offsets, new_capacity * sizeof(size_t));
    if (new_offsets == NULL) {
        return false;
    }
    directory->offsets = new_offsets;
    directory->capacity = new_capacity;
    directory->offsets[directory->count++] = offset;
    return true;
}

/* Returns the byte offset of the next block while validating this block's
   complete encoded extent. This first pass makes relative block control flow
   safe to execute during the decoding pass. */
static bool tape_tzx_next_block_offset(
    const uint8_t *data,
    size_t size,
    size_t block_offset,
    size_t *next_offset
) {
    size_t offset;
    size_t body_length;
    uint8_t block_id;

    if (!tape_has_bytes(block_offset, 1, size)) {
        return false;
    }
    block_id = data[block_offset];
    offset = block_offset + 1u;

    switch (block_id) {
        case 0x10:
            if (!tape_has_bytes(offset, 4, size)) return false;
            body_length = 4u + tape_read_u16(&data[offset + 2u]);
            break;
        case 0x11:
            if (!tape_has_bytes(offset, 18, size)) return false;
            body_length = 18u + tape_read_u24(&data[offset + 15u]);
            break;
        case 0x12:
            body_length = 4;
            break;
        case 0x13:
            if (!tape_has_bytes(offset, 1, size)) return false;
            body_length = 1u + ((size_t)data[offset] * 2u);
            break;
        case 0x14:
            if (!tape_has_bytes(offset, 10, size)) return false;
            body_length = 10u + tape_read_u24(&data[offset + 7u]);
            break;
        case 0x15:
            if (!tape_has_bytes(offset, 8, size)) return false;
            body_length = 8u + tape_read_u24(&data[offset + 5u]);
            break;
        case 0x18:
        case 0x19:
        case 0x2B:
            if (!tape_has_bytes(offset, 4, size)) return false;
            body_length = 4u + tape_read_u32(&data[offset]);
            break;
        case 0x20:
        case 0x23:
        case 0x24:
            body_length = 2;
            break;
        case 0x21:
        case 0x30:
            if (!tape_has_bytes(offset, 1, size)) return false;
            body_length = 1u + data[offset];
            break;
        case 0x22:
        case 0x25:
        case 0x27:
            body_length = 0;
            break;
        case 0x26:
            if (!tape_has_bytes(offset, 2, size)) return false;
            body_length = 2u + ((size_t)tape_read_u16(&data[offset]) * 2u);
            break;
        case 0x28:
        case 0x32:
            if (!tape_has_bytes(offset, 2, size)) return false;
            body_length = 2u + tape_read_u16(&data[offset]);
            break;
        case 0x2A:
            body_length = 4;
            break;
        case 0x31:
            if (!tape_has_bytes(offset, 2, size)) return false;
            body_length = 2u + data[offset + 1u];
            break;
        case 0x33:
            if (!tape_has_bytes(offset, 1, size)) return false;
            body_length = 1u + ((size_t)data[offset] * 3u);
            break;
        case 0x35:
            if (!tape_has_bytes(offset, 20, size)) return false;
            body_length = 20u + tape_read_u32(&data[offset + 16u]);
            break;
        case 0x5A:
            body_length = 9;
            break;
        default:
            return false;
    }

    if (!tape_has_bytes(offset, body_length, size)) {
        return false;
    }
    *next_offset = offset + body_length;
    return true;
}

static bool tape_build_tzx_directory(
    TapeTzxDirectory *directory,
    const uint8_t *data,
    size_t size
) {
    size_t offset = TAPE_TZX_HEADER_SIZE;

    memset(directory, 0, sizeof(*directory));
    while (offset < size) {
        size_t next_offset;
        if (!tape_tzx_directory_append(directory, offset) ||
            !tape_tzx_next_block_offset(data, size, offset, &next_offset) ||
            next_offset <= offset) {
            free(directory->offsets);
            memset(directory, 0, sizeof(*directory));
            return false;
        }
        offset = next_offset;
    }
    return offset == size;
}

static bool tape_tzx_relative_target(
    size_t block_index,
    int16_t relative,
    size_t block_count,
    size_t *target
) {
    const int64_t destination = (int64_t)block_index + relative;
    if (destination < 0 || destination >= (int64_t)block_count) {
        return false;
    }
    *target = (size_t)destination;
    return true;
}

static unsigned tape_tzx_symbol_bits(uint16_t alphabet_size) {
    unsigned bits = 0;
    uint16_t values = 1;
    while (values < alphabet_size) {
        values <<= 1;
        bits++;
    }
    return bits;
}

static bool tape_append_generalized_data(
    TapeBuilder *builder,
    const uint8_t *payload,
    size_t payload_length
) {
    uint16_t pause_ms;
    uint32_t pilot_records;
    uint8_t pilot_pulses;
    uint16_t pilot_alphabet_size;
    uint32_t data_symbols;
    uint8_t data_pulses;
    uint16_t data_alphabet_size;
    size_t offset = 14;
    size_t pilot_definition_size;
    size_t data_definition_size;

    if (payload_length < 14) {
        return false;
    }
    pause_ms = tape_read_u16(&payload[0]);
    pilot_records = tape_read_u32(&payload[2]);
    pilot_pulses = payload[6];
    pilot_alphabet_size = payload[7] == 0 ? 256u : payload[7];
    data_symbols = tape_read_u32(&payload[8]);
    data_pulses = payload[12];
    data_alphabet_size = payload[13] == 0 ? 256u : payload[13];

    pilot_definition_size = 1u + ((size_t)pilot_pulses * 2u);
    data_definition_size = 1u + ((size_t)data_pulses * 2u);

    if (pilot_records > 0) {
        const size_t definitions_length = pilot_definition_size * pilot_alphabet_size;
        const uint8_t *definitions;
        if (!tape_has_bytes(offset, definitions_length, payload_length)) return false;
        definitions = &payload[offset];
        offset += definitions_length;
        if (!tape_has_bytes(offset, (size_t)pilot_records * 3u, payload_length)) return false;
        for (uint32_t record = 0; record < pilot_records; ++record) {
            const uint8_t symbol = payload[offset];
            const uint16_t repetitions = tape_read_u16(&payload[offset + 1u]);
            if (symbol >= pilot_alphabet_size) return false;
            for (uint16_t repeat = 0; repeat < repetitions; ++repeat) {
                if (!tape_append_generalized_symbol(
                        builder,
                        &definitions[(size_t)symbol * pilot_definition_size],
                        pilot_pulses)) return false;
            }
            offset += 3;
        }
    }

    if (data_symbols > 0) {
        const size_t definitions_length = data_definition_size * data_alphabet_size;
        const uint8_t *definitions;
        const unsigned symbol_bits = tape_tzx_symbol_bits(data_alphabet_size);
        const uint64_t stream_bits = (uint64_t)symbol_bits * data_symbols;
        const uint64_t stream_bytes = (stream_bits + 7u) / 8u;
        uint64_t bit_offset = 0;

        if (!tape_has_bytes(offset, definitions_length, payload_length)) return false;
        definitions = &payload[offset];
        offset += definitions_length;
        if (stream_bytes > SIZE_MAX || !tape_has_bytes(offset, (size_t)stream_bytes, payload_length)) return false;

        for (uint32_t record = 0; record < data_symbols; ++record) {
            uint16_t symbol = 0;
            for (unsigned bit = 0; bit < symbol_bits; ++bit) {
                const size_t byte_index = offset + (size_t)(bit_offset / 8u);
                symbol = (uint16_t)((symbol << 1u) |
                    ((payload[byte_index] >> (7u - (bit_offset % 8u))) & 1u));
                bit_offset++;
            }
            if (symbol >= data_alphabet_size ||
                !tape_append_generalized_symbol(
                    builder,
                    &definitions[(size_t)symbol * data_definition_size],
                    data_pulses)) return false;
        }
        offset += (size_t)stream_bytes;
    }

    if (offset != payload_length) {
        return false;
    }
    return pause_ms == 0 || tape_append_pause(builder, pause_ms);
}

static bool tape_parse_tzx(
    TapePlayer *player,
    const uint8_t *data,
    size_t size,
    char *error_buffer,
    size_t error_buffer_size,
    const char *path
) {
    TapeBuilder builder;
    TapeTzxDirectory directory;
    TapeTzxLoop loop_stack[TAPE_TZX_LOOP_DEPTH];
    TapeTzxCall call = {0};
    int loop_depth = 0;
    size_t block_index = 0;
    size_t executions = 0;
    const char *failure_message = NULL;
    bool ok = false;

    builder.player = player;
    builder.current_level = false;
    builder.work_remaining = tape_decode_work_budget(size);

    if (size < TAPE_TZX_HEADER_SIZE || memcmp(data, "ZXTape!\x1A", 8) != 0) {
        tape_set_error(error_buffer, error_buffer_size, "Invalid TZX header", path);
        return false;
    }

    if (!tape_build_tzx_directory(&directory, data, size)) {
        tape_set_error(
            error_buffer,
            error_buffer_size,
            "Invalid, truncated, or unsupported TZX block",
            path);
        return false;
    }

    while (block_index < directory.count) {
        const size_t current_block_index = block_index;
        size_t next_block_index = block_index + 1u;
        size_t offset = directory.offsets[block_index] + 1u;
        const uint8_t block_id = data[directory.offsets[block_index]];

        if (++executions > TAPE_TZX_MAX_BLOCK_EXECUTIONS) {
            failure_message = "TZX control flow does not terminate";
            goto finished;
        }

        switch (block_id) {
            case 0x10: {
                const uint16_t pause_ms = tape_read_u16(&data[offset]);
                const uint16_t block_length = tape_read_u16(&data[offset + 2]);
                offset += 4;
                if (!tape_append_standard_block(&builder, &data[offset], block_length, pause_ms)) {
                    failure_message = "Could not decode TZX standard data block";
                    goto finished;
                }
                if (!tape_append_fast_block(player, &data[offset], block_length)) {
                    failure_message = "Out of memory while indexing TZX blocks";
                    goto finished;
                }
                break;
            }

            case 0x11: {
                const uint16_t pilot_pulse_ticks = tape_read_u16(&data[offset]);
                const uint16_t sync1_pulse_ticks = tape_read_u16(&data[offset + 2]);
                const uint16_t sync2_pulse_ticks = tape_read_u16(&data[offset + 4]);
                const uint16_t zero_pulse_ticks = tape_read_u16(&data[offset + 6]);
                const uint16_t one_pulse_ticks = tape_read_u16(&data[offset + 8]);
                const uint16_t pilot_count = tape_read_u16(&data[offset + 10]);
                const uint8_t used_bits_last = data[offset + 12];
                const uint16_t pause_ms = tape_read_u16(&data[offset + 13]);
                const uint32_t block_length = tape_read_u24(&data[offset + 15]);
                offset += 18;
                if (!tape_append_turbo_block(
                        &builder,
                        &data[offset],
                        block_length,
                        pilot_pulse_ticks,
                        sync1_pulse_ticks,
                        sync2_pulse_ticks,
                        zero_pulse_ticks,
                        one_pulse_ticks,
                        pilot_count,
                        used_bits_last,
                        pause_ms)) {
                    failure_message = "Could not decode TZX turbo data block";
                    goto finished;
                }
                break;
            }

            case 0x12: {
                const uint16_t pulse_ticks = tape_read_u16(&data[offset]);
                const uint16_t pulse_count = tape_read_u16(&data[offset + 2]);
                for (uint16_t pulse_index = 0; pulse_index < pulse_count; ++pulse_index) {
                    if (!tape_append_pulse(&builder, pulse_ticks)) {
                        failure_message = "Could not decode TZX pure tone block";
                        goto finished;
                    }
                }
                break;
            }

            case 0x13: {
                const uint8_t pulse_count = data[offset++];
                for (uint8_t pulse_index = 0; pulse_index < pulse_count; ++pulse_index) {
                    if (!tape_append_pulse(&builder, tape_read_u16(&data[offset + (pulse_index * 2)]))) {
                        failure_message = "Could not decode TZX pulse sequence block";
                        goto finished;
                    }
                }
                break;
            }

            case 0x14: {
                const uint16_t zero_pulse_ticks = tape_read_u16(&data[offset]);
                const uint16_t one_pulse_ticks = tape_read_u16(&data[offset + 2]);
                const uint8_t used_bits_last = data[offset + 4];
                const uint16_t pause_ms = tape_read_u16(&data[offset + 5]);
                const uint32_t block_length = tape_read_u24(&data[offset + 7]);
                offset += 10;
                if (!tape_append_data_bits(
                        &builder,
                        &data[offset],
                        block_length,
                        used_bits_last,
                        zero_pulse_ticks,
                        one_pulse_ticks) ||
                    (pause_ms > 0 && !tape_append_pause(&builder, pause_ms))) {
                    failure_message = "Could not decode TZX pure data block";
                    goto finished;
                }
                break;
            }

            case 0x15: {
                const uint16_t sample_ticks = tape_read_u16(&data[offset]);
                const uint16_t pause_ms = tape_read_u16(&data[offset + 2]);
                const uint8_t used_bits_last = data[offset + 4];
                const uint32_t block_length = tape_read_u24(&data[offset + 5]);
                offset += 8;
                if (!tape_append_direct_samples(
                        &builder,
                        &data[offset],
                        block_length,
                        used_bits_last,
                        sample_ticks) ||
                    (pause_ms > 0 && !tape_append_pause(&builder, pause_ms))) {
                    failure_message = "Invalid or too large TZX direct recording block";
                    goto finished;
                }
                break;
            }

            case 0x18: {
                const uint32_t block_length = tape_read_u32(&data[offset]);
                const uint16_t pause_ms = tape_read_u16(&data[offset + 4]);
                const uint32_t sample_rate = tape_read_u24(&data[offset + 6]);
                const uint8_t compression = data[offset + 9];
                const uint32_t pulse_count = tape_read_u32(&data[offset + 10]);
                if (block_length < 10) {
                    failure_message = "Invalid TZX CSW recording block";
                    goto finished;
                }
                if (compression != 1) {
                    failure_message = "Z-RLE compressed TZX CSW blocks are not supported";
                    goto finished;
                }
                if (!tape_append_csw_rle(
                        &builder,
                        &data[offset + 14],
                        block_length - 10u,
                        sample_rate,
                        pulse_count) ||
                    (pause_ms > 0 && !tape_append_pause(&builder, pause_ms))) {
                    failure_message = "Invalid or too large TZX CSW recording block";
                    goto finished;
                }
                break;
            }

            case 0x19: {
                const uint32_t block_length = tape_read_u32(&data[offset]);
                if (!tape_append_generalized_data(&builder, &data[offset + 4], block_length)) {
                    failure_message = "Invalid or too large TZX generalized data block";
                    goto finished;
                }
                break;
            }

            case 0x20: {
                const uint16_t pause_ms = tape_read_u16(&data[offset]);
                if (pause_ms == 0) {
                    if (!tape_mark_stop(player, TAPE_STOP_ALWAYS)) {
                        failure_message = "TZX waveform exceeds the decode limit";
                        goto finished;
                    }
                    break;
                }
                if (!tape_append_pause(&builder, pause_ms)) {
                    failure_message = "Could not decode TZX pause block";
                    goto finished;
                }
                break;
            }

            case 0x21:
            case 0x22:
                break;

            case 0x23: {
                const int16_t relative = (int16_t)tape_read_u16(&data[offset]);
                if (!tape_tzx_relative_target(
                        current_block_index,
                        relative,
                        directory.count,
                        &next_block_index)) {
                    failure_message = "TZX jump target is outside the tape";
                    goto finished;
                }
                break;
            }

            case 0x24: {
                const uint16_t repeat_count = tape_read_u16(&data[offset]);
                if (loop_depth >= TAPE_TZX_LOOP_DEPTH) {
                    failure_message = "TZX loop nesting too deep";
                    goto finished;
                }
                if (repeat_count == 0 || next_block_index >= directory.count) {
                    failure_message = "Invalid TZX loop start block";
                    goto finished;
                }
                loop_stack[loop_depth].body_block_index = next_block_index;
                loop_stack[loop_depth].remaining = repeat_count;
                loop_depth++;
                break;
            }

            case 0x25:
                if (loop_depth <= 0) {
                    failure_message = "TZX loop end without loop start";
                    goto finished;
                }
                if (loop_stack[loop_depth - 1].remaining > 1) {
                    loop_stack[loop_depth - 1].remaining--;
                    next_block_index = loop_stack[loop_depth - 1].body_block_index;
                } else {
                    loop_depth--;
                }
                break;

            case 0x26: {
                const uint16_t call_count = tape_read_u16(&data[offset]);
                if (call.active) {
                    failure_message = "Nested TZX call sequences are not valid";
                    goto finished;
                }
                if (call_count == 0) {
                    break;
                }
                call.offsets = &data[offset + 2];
                call.call_block_index = current_block_index;
                call.return_block_index = next_block_index;
                call.count = call_count;
                call.next = 1;
                call.active = true;
                if (!tape_tzx_relative_target(
                        current_block_index,
                        (int16_t)tape_read_u16(call.offsets),
                        directory.count,
                        &next_block_index)) {
                    failure_message = "TZX call target is outside the tape";
                    goto finished;
                }
                break;
            }

            case 0x27:
                if (!call.active) {
                    failure_message = "TZX return without a call sequence";
                    goto finished;
                }
                if (call.next < call.count) {
                    const int16_t relative =
                        (int16_t)tape_read_u16(&call.offsets[(size_t)call.next * 2u]);
                    call.next++;
                    if (!tape_tzx_relative_target(
                            call.call_block_index,
                            relative,
                            directory.count,
                            &next_block_index)) {
                        failure_message = "TZX call target is outside the tape";
                        goto finished;
                    }
                } else {
                    next_block_index = call.return_block_index;
                    call.active = false;
                }
                break;

            case 0x28: {
                const uint16_t select_length = tape_read_u16(&data[offset]);
                uint8_t selection_count;
                if (select_length < 1u) {
                    failure_message = "Invalid TZX select block";
                    goto finished;
                }
                selection_count = data[offset + 2];
                if (selection_count > 0) {
                    uint8_t description_length;
                    if (select_length < 4u) {
                        failure_message = "Invalid TZX select block";
                        goto finished;
                    }
                    description_length = data[offset + 5];
                    if (
                        (size_t)description_length + 4u > select_length ||
                        !tape_tzx_relative_target(
                            current_block_index,
                            (int16_t)tape_read_u16(&data[offset + 3]),
                            directory.count,
                            &next_block_index)) {
                        failure_message = "Invalid TZX select block";
                        goto finished;
                    }
                }
                break;
            }

            case 0x2A:
                player->autoload_target = TAPE_AUTOLOAD_TARGET_128_MENU;
                if (!tape_mark_stop(player, TAPE_STOP_48K_ONLY)) {
                    failure_message = "TZX waveform exceeds the decode limit";
                    goto finished;
                }
                break;

            case 0x2B: {
                const uint32_t payload_length = tape_read_u32(&data[offset]);
                if (payload_length != 1) {
                    failure_message = "Invalid TZX signal level block";
                    goto finished;
                }
                builder.current_level = (data[offset + 4] != 0);
                break;
            }

            case 0x30:
            case 0x31:
            case 0x32:
            case 0x33:
            case 0x35:
            case 0x5A:
                break;

            default: {
                failure_message = "Unsupported TZX block";
                goto finished;
            }
        }
        block_index = next_block_index;
    }

    if (loop_depth != 0) {
        failure_message = "TZX loop start without loop end";
        goto finished;
    }
    if (call.active) {
        failure_message = "TZX call sequence without return";
        goto finished;
    }
    ok = true;

finished:
    free(directory.offsets);
    if (!ok) {
        tape_set_error(
            error_buffer,
            error_buffer_size,
            failure_message != NULL ? failure_message : "Could not decode TZX file",
            path);
    }
    return ok;
}

void tape_init(TapePlayer *player) {
    memset(player, 0, sizeof(*player));
}

void tape_discard(TapePlayer *player) {
    if (player == NULL) {
        return;
    }
    for (size_t i = 0; i < player->block_count; ++i) {
        free(player->blocks[i].bytes);
    }
    free(player->blocks);
    free(player->segments);
    tape_init(player);
}

bool tape_load_file(
    TapePlayer *player,
    const char *path,
    uint32_t tick_hz,
    char *error_buffer,
    size_t error_buffer_size
) {
    FILE *file;
    uint8_t *data = NULL;
    long file_size;
    bool ok = false;
    const char *extension;

    if (player == NULL || path == NULL) {
        tape_set_error(error_buffer, error_buffer_size, "Invalid tape load request", path);
        return false;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        tape_set_error(error_buffer, error_buffer_size, "Could not open tape file", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        tape_set_error(error_buffer, error_buffer_size, "Could not size tape file", path);
        return false;
    }
    file_size = ftell(file);
    if (file_size <= 0) {
        fclose(file);
        tape_set_error(error_buffer, error_buffer_size, "Tape file is empty", path);
        return false;
    }
    rewind(file);

    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        tape_set_error(error_buffer, error_buffer_size, "Out of memory while loading tape", path);
        return false;
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        free(data);
        tape_set_error(error_buffer, error_buffer_size, "Could not read tape file", path);
        return false;
    }
    fclose(file);

    tape_discard(player);
    player->tick_hz = tick_hz;

    extension = strrchr(path, '.');
    if (extension != NULL && _stricmp(extension, ".tap") == 0) {
        ok = tape_parse_tap(player, data, (size_t)file_size, error_buffer, error_buffer_size, path);
    } else if (extension != NULL && _stricmp(extension, ".tzx") == 0) {
        ok = tape_parse_tzx(
            player,
            data,
            (size_t)file_size,
            error_buffer,
            error_buffer_size,
            path);
    } else {
        tape_set_error(error_buffer, error_buffer_size, "Unsupported tape extension", path);
    }

    free(data);

    if (!ok) {
        tape_discard(player);
        return false;
    }

    player->autoload_target = tape_detect_autoload_target(player);
    player->inserted = player->segment_count > 0;
    player->playing = false;
    player->next_block_index = 0;
    player->play_index = 0;
    player->segment_end_tick = 0;
    return player->inserted;
}

void tape_rewind(TapePlayer *player, uint32_t tick_hz) {
    if (player == NULL) {
        return;
    }
    player->tick_hz = tick_hz;
    player->next_block_index = 0;
    player->play_index = 0;
    player->segment_end_tick = 0;
    player->playing = false;
}

void tape_set_stop_mode(TapePlayer *player, bool stop_in_48k_mode) {
    if (player == NULL) {
        return;
    }
    player->stop_in_48k_mode = stop_in_48k_mode;
}

void tape_start(TapePlayer *player, uint64_t tick_count) {
    if (player == NULL || !player->inserted || player->segment_count == 0) {
        return;
    }
    if (player->play_index >= player->segment_count) {
        player->play_index = 0;
    }
    player->playing = true;
    player->segment_end_tick = tick_count + player->segments[player->play_index].duration_ticks;
}

void tape_stop(TapePlayer *player) {
    if (player == NULL) {
        return;
    }
    player->playing = false;
}

bool tape_input_level(TapePlayer *player, uint64_t tick_count) {
    if (player == NULL || !player->inserted || !player->playing || player->segment_count == 0) {
        return false;
    }

    while (tick_count >= player->segment_end_tick) {
        const TapeSegment *segment = &player->segments[player->play_index];
        const bool should_stop =
            segment->stop_after == TAPE_STOP_ALWAYS ||
            (segment->stop_after == TAPE_STOP_48K_ONLY && player->stop_in_48k_mode);

        player->play_index++;
        if (should_stop) {
            player->playing = false;
            return false;
        }
        if (player->play_index >= player->segment_count) {
            player->playing = false;
            return false;
        }
        player->segment_end_tick += player->segments[player->play_index].duration_ticks;
    }
    return player->segments[player->play_index].level_high;
}

bool tape_has_tape(const TapePlayer *player) {
    return player != NULL && player->inserted && player->segment_count > 0;
}

TapeAutoloadTarget tape_autoload_target(const TapePlayer *player) {
    if (player == NULL) {
        return TAPE_AUTOLOAD_TARGET_UNKNOWN;
    }
    return player->autoload_target;
}

bool tape_try_fast_load(TapePlayer *player, void *machine_ptr) {
    TapeBlock *block;
    uint8_t expected_flag;
    bool is_load;
    uint8_t parity;
    uint16_t requested_length;
    uint16_t start_addr;
    uint16_t bytes_processed = 0;
    zx_t *machine = (zx_t *)machine_ptr;

    if (player == NULL || machine == NULL || player->next_block_index >= player->block_count) {
        return false;
    }

    block = &player->blocks[player->next_block_index];
    expected_flag = (uint8_t)(machine->cpu.af2 >> 8);
    is_load = (machine->cpu.af2 & Z80_CF) != 0;
    requested_length = machine->cpu.de;
    start_addr = machine->cpu.ix;

    if (block->length != ((size_t)requested_length + 2u) || block->length < 2) {
        return false;
    }

    player->next_block_index++;
    player->playing = false;

    parity = block->bytes[0];
    machine->cpu.a = 0;
    machine->cpu.l = parity;

    if (requested_length == 0) {
        machine->cpu.af2 = 0x0001;
        machine->cpu.b = 0xB0;
        machine->cpu.f = tape_cp_flags(parity, 1);
        goto finished;
    }

    machine->cpu.af2 = 0x0145;

    if (parity != expected_flag) {
        machine->cpu.f &= (uint8_t)~Z80_CF;
        goto finished;
    }

    machine->cpu.l = block->bytes[block->length - 1];

    if (is_load) {
        for (uint16_t i = 0; i < requested_length; ++i) {
            const uint8_t value = block->bytes[1u + i];
            parity ^= value;
            mem_wr(&machine->mem, (uint16_t)(start_addr + i), value);
            bytes_processed++;
        }
    } else {
        for (uint16_t i = 0; i < requested_length; ++i) {
            const uint8_t value = block->bytes[1u + i];
            parity ^= value;
            bytes_processed++;
            if (mem_rd(&machine->mem, (uint16_t)(start_addr + i)) != value) {
                machine->cpu.l = value;
                machine->cpu.f &= (uint8_t)~Z80_CF;
                goto finished;
            }
        }
    }

    parity ^= block->bytes[1u + requested_length];
    machine->cpu.a = parity;
    machine->cpu.f = tape_cp_flags(parity, 1);
    machine->cpu.b = 0xB0;

finished:
    machine->cpu.c = 1;
    machine->cpu.h = parity;
    machine->cpu.de = (uint16_t)(requested_length - bytes_processed);
    machine->cpu.ix = (uint16_t)(start_addr + bytes_processed);
    machine->cpu.pc = 0x05E2;
    return true;
}
