#include "tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TAPE_TZX_HEADER_SIZE = 10,
    TAPE_TZX_LOOP_DEPTH = 16,
    TAPE_PILOT_PULSE_TICKS = 2168,
    TAPE_SYNC1_PULSE_TICKS = 667,
    TAPE_SYNC2_PULSE_TICKS = 735,
    TAPE_ZERO_PULSE_TICKS = 855,
    TAPE_ONE_PULSE_TICKS = 1710,
    TAPE_HEADER_PILOT_PULSES = 8063,
    TAPE_DATA_PILOT_PULSES = 3223,
    TAPE_TAP_BLOCK_PAUSE_MS = 1000
};

typedef struct TapeBuilder {
    TapePlayer *player;
    bool current_level;
} TapeBuilder;

typedef struct TapeTzxLoop {
    size_t body_offset;
    uint16_t remaining;
} TapeTzxLoop;

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

static bool tape_has_bytes(size_t offset, size_t count, size_t total) {
    return offset <= total && count <= (total - offset);
}

static uint32_t tape_ms_to_ticks(uint32_t tick_hz, uint16_t duration_ms) {
    if (duration_ms == 0 || tick_hz == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)duration_ms * (uint64_t)tick_hz + 999u) / 1000u);
}

static bool tape_reserve_segments(TapePlayer *player, size_t additional_count) {
    TapeSegment *segments;
    size_t required;
    size_t new_capacity;

    if (additional_count > SIZE_MAX - player->segment_count) {
        return false;
    }
    required = player->segment_count + additional_count;
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

static bool tape_append_level(TapePlayer *player, uint32_t duration_ticks, bool level_high) {
    TapeSegment *segment;

    if (duration_ticks == 0) {
        return true;
    }
    if (player->segment_count > 0) {
        segment = &player->segments[player->segment_count - 1];
        if (segment->level_high == level_high) {
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
    return true;
}

static bool tape_append_pulse(TapeBuilder *builder, uint32_t duration_ticks) {
    builder->current_level = !builder->current_level;
    return tape_append_level(builder->player, duration_ticks, builder->current_level);
}

static bool tape_append_pause(TapeBuilder *builder, uint16_t duration_ms) {
    builder->current_level = false;
    return tape_append_level(
        builder->player,
        tape_ms_to_ticks(builder->player->tick_hz, duration_ms),
        false);
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
    return tape_append_pause(builder, pause_ms);
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
    return tape_append_pause(builder, pause_ms);
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
        offset += block_length;
    }
    return true;
}

static bool tape_parse_tzx(
    TapePlayer *player,
    const uint8_t *data,
    size_t size,
    bool stop_in_48k_mode,
    char *error_buffer,
    size_t error_buffer_size,
    const char *path
) {
    TapeBuilder builder;
    TapeTzxLoop loop_stack[TAPE_TZX_LOOP_DEPTH];
    int loop_depth = 0;
    size_t offset = TAPE_TZX_HEADER_SIZE;

    builder.player = player;
    builder.current_level = false;

    if (size < TAPE_TZX_HEADER_SIZE || memcmp(data, "ZXTape!\x1A", 8) != 0) {
        tape_set_error(error_buffer, error_buffer_size, "Invalid TZX header", path);
        return false;
    }

    while (offset < size) {
        const uint8_t block_id = data[offset++];

        switch (block_id) {
            case 0x10: {
                uint16_t pause_ms;
                uint16_t block_length;
                if (!tape_has_bytes(offset, 4, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX standard block", path);
                    return false;
                }
                pause_ms = tape_read_u16(&data[offset]);
                block_length = tape_read_u16(&data[offset + 2]);
                offset += 4;
                if (!tape_has_bytes(offset, block_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX standard block data", path);
                    return false;
                }
                if (!tape_append_standard_block(&builder, &data[offset], block_length, pause_ms)) {
                    tape_set_error(error_buffer, error_buffer_size, "Out of memory while decoding TZX data", path);
                    return false;
                }
                offset += block_length;
                if (pause_ms == 0) {
                    return true;
                }
                break;
            }

            case 0x11: {
                uint16_t pilot_pulse_ticks;
                uint16_t sync1_pulse_ticks;
                uint16_t sync2_pulse_ticks;
                uint16_t zero_pulse_ticks;
                uint16_t one_pulse_ticks;
                uint16_t pilot_count;
                uint8_t used_bits_last;
                uint16_t pause_ms;
                uint32_t block_length;

                if (!tape_has_bytes(offset, 18, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX turbo block", path);
                    return false;
                }
                pilot_pulse_ticks = tape_read_u16(&data[offset]);
                sync1_pulse_ticks = tape_read_u16(&data[offset + 2]);
                sync2_pulse_ticks = tape_read_u16(&data[offset + 4]);
                zero_pulse_ticks = tape_read_u16(&data[offset + 6]);
                one_pulse_ticks = tape_read_u16(&data[offset + 8]);
                pilot_count = tape_read_u16(&data[offset + 10]);
                used_bits_last = data[offset + 12];
                pause_ms = tape_read_u16(&data[offset + 13]);
                block_length = tape_read_u24(&data[offset + 15]);
                offset += 18;
                if (!tape_has_bytes(offset, block_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX turbo block data", path);
                    return false;
                }
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
                    tape_set_error(error_buffer, error_buffer_size, "Out of memory while decoding TZX data", path);
                    return false;
                }
                offset += block_length;
                if (pause_ms == 0) {
                    return true;
                }
                break;
            }

            case 0x12: {
                uint16_t pulse_ticks;
                uint16_t pulse_count;
                if (!tape_has_bytes(offset, 4, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX pure tone block", path);
                    return false;
                }
                pulse_ticks = tape_read_u16(&data[offset]);
                pulse_count = tape_read_u16(&data[offset + 2]);
                offset += 4;
                for (uint16_t pulse_index = 0; pulse_index < pulse_count; ++pulse_index) {
                    if (!tape_append_pulse(&builder, pulse_ticks)) {
                        tape_set_error(error_buffer, error_buffer_size, "Out of memory while decoding TZX data", path);
                        return false;
                    }
                }
                break;
            }

            case 0x13: {
                uint8_t pulse_count;
                if (!tape_has_bytes(offset, 1, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX pulse sequence block", path);
                    return false;
                }
                pulse_count = data[offset++];
                if (!tape_has_bytes(offset, (size_t)pulse_count * 2, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX pulse sequence data", path);
                    return false;
                }
                for (uint8_t pulse_index = 0; pulse_index < pulse_count; ++pulse_index) {
                    if (!tape_append_pulse(&builder, tape_read_u16(&data[offset + (pulse_index * 2)]))) {
                        tape_set_error(error_buffer, error_buffer_size, "Out of memory while decoding TZX data", path);
                        return false;
                    }
                }
                offset += (size_t)pulse_count * 2;
                break;
            }

            case 0x14: {
                uint16_t zero_pulse_ticks;
                uint16_t one_pulse_ticks;
                uint8_t used_bits_last;
                uint16_t pause_ms;
                uint32_t block_length;
                if (!tape_has_bytes(offset, 10, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX pure data block", path);
                    return false;
                }
                zero_pulse_ticks = tape_read_u16(&data[offset]);
                one_pulse_ticks = tape_read_u16(&data[offset + 2]);
                used_bits_last = data[offset + 4];
                pause_ms = tape_read_u16(&data[offset + 5]);
                block_length = tape_read_u24(&data[offset + 7]);
                offset += 10;
                if (!tape_has_bytes(offset, block_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX pure data payload", path);
                    return false;
                }
                if (!tape_append_data_bits(
                        &builder,
                        &data[offset],
                        block_length,
                        used_bits_last,
                        zero_pulse_ticks,
                        one_pulse_ticks) ||
                    !tape_append_pause(&builder, pause_ms)) {
                    tape_set_error(error_buffer, error_buffer_size, "Out of memory while decoding TZX data", path);
                    return false;
                }
                offset += block_length;
                if (pause_ms == 0) {
                    return true;
                }
                break;
            }

            case 0x20: {
                uint16_t pause_ms;
                if (!tape_has_bytes(offset, 2, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX pause block", path);
                    return false;
                }
                pause_ms = tape_read_u16(&data[offset]);
                offset += 2;
                if (pause_ms == 0) {
                    return true;
                }
                if (!tape_append_pause(&builder, pause_ms)) {
                    tape_set_error(error_buffer, error_buffer_size, "Out of memory while decoding TZX data", path);
                    return false;
                }
                break;
            }

            case 0x21: {
                uint8_t text_length;
                if (!tape_has_bytes(offset, 1, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX group start block", path);
                    return false;
                }
                text_length = data[offset++];
                if (!tape_has_bytes(offset, text_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX group name", path);
                    return false;
                }
                offset += text_length;
                break;
            }

            case 0x22:
                break;

            case 0x24: {
                uint16_t repeat_count;
                if (!tape_has_bytes(offset, 2, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX loop start block", path);
                    return false;
                }
                if (loop_depth >= TAPE_TZX_LOOP_DEPTH) {
                    tape_set_error(error_buffer, error_buffer_size, "TZX loop nesting too deep", path);
                    return false;
                }
                repeat_count = tape_read_u16(&data[offset]);
                offset += 2;
                loop_stack[loop_depth].body_offset = offset;
                loop_stack[loop_depth].remaining = repeat_count;
                loop_depth++;
                break;
            }

            case 0x25:
                if (loop_depth <= 0) {
                    tape_set_error(error_buffer, error_buffer_size, "TZX loop end without loop start", path);
                    return false;
                }
                if (loop_stack[loop_depth - 1].remaining > 1) {
                    loop_stack[loop_depth - 1].remaining--;
                    offset = loop_stack[loop_depth - 1].body_offset;
                } else {
                    loop_depth--;
                }
                break;

            case 0x2A:
                if (!tape_has_bytes(offset, 4, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX stop-tape block", path);
                    return false;
                }
                offset += 4;
                if (stop_in_48k_mode) {
                    return true;
                }
                break;

            case 0x2B: {
                uint32_t payload_length;
                if (!tape_has_bytes(offset, 5, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX signal level block", path);
                    return false;
                }
                payload_length = tape_read_u32(&data[offset]);
                if (payload_length != 1) {
                    tape_set_error(error_buffer, error_buffer_size, "Unsupported TZX signal level block", path);
                    return false;
                }
                builder.current_level = (data[offset + 4] != 0);
                offset += 5;
                break;
            }

            case 0x30: {
                uint8_t text_length;
                if (!tape_has_bytes(offset, 1, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX text description block", path);
                    return false;
                }
                text_length = data[offset++];
                if (!tape_has_bytes(offset, text_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX text description", path);
                    return false;
                }
                offset += text_length;
                break;
            }

            case 0x31: {
                uint8_t text_length;
                if (!tape_has_bytes(offset, 2, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX message block", path);
                    return false;
                }
                text_length = data[offset + 1];
                offset += 2;
                if (!tape_has_bytes(offset, text_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX message text", path);
                    return false;
                }
                offset += text_length;
                break;
            }

            case 0x32: {
                uint16_t archive_length;
                if (!tape_has_bytes(offset, 2, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX archive block", path);
                    return false;
                }
                archive_length = tape_read_u16(&data[offset]);
                offset += 2;
                if (!tape_has_bytes(offset, archive_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX archive data", path);
                    return false;
                }
                offset += archive_length;
                break;
            }

            case 0x33: {
                uint8_t info_count;
                if (!tape_has_bytes(offset, 1, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX hardware info block", path);
                    return false;
                }
                info_count = data[offset++];
                if (!tape_has_bytes(offset, (size_t)info_count * 3, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX hardware info data", path);
                    return false;
                }
                offset += (size_t)info_count * 3;
                break;
            }

            case 0x35: {
                uint32_t custom_length;
                if (!tape_has_bytes(offset, 20, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX custom info block", path);
                    return false;
                }
                custom_length = tape_read_u32(&data[offset + 16]);
                offset += 20;
                if (!tape_has_bytes(offset, custom_length, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX custom info data", path);
                    return false;
                }
                offset += custom_length;
                break;
            }

            case 0x5A:
                if (!tape_has_bytes(offset, 9, size)) {
                    tape_set_error(error_buffer, error_buffer_size, "Truncated TZX glue block", path);
                    return false;
                }
                offset += 9;
                break;

            default: {
                char message[96];
                snprintf(message, sizeof(message), "Unsupported TZX block 0x%02X", block_id);
                tape_set_error(error_buffer, error_buffer_size, message, path);
                return false;
            }
        }
    }
    return true;
}

void tape_init(TapePlayer *player) {
    memset(player, 0, sizeof(*player));
}

void tape_discard(TapePlayer *player) {
    if (player == NULL) {
        return;
    }
    free(player->segments);
    tape_init(player);
}

bool tape_load_file(
    TapePlayer *player,
    const char *path,
    uint32_t tick_hz,
    bool stop_in_48k_mode,
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
            stop_in_48k_mode,
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

    player->inserted = player->segment_count > 0;
    player->playing = false;
    player->play_index = 0;
    player->segment_end_tick = 0;
    return player->inserted;
}

void tape_rewind(TapePlayer *player, uint32_t tick_hz) {
    if (player == NULL) {
        return;
    }
    player->tick_hz = tick_hz;
    player->play_index = 0;
    player->segment_end_tick = 0;
    player->playing = false;
}

void tape_start(TapePlayer *player, uint64_t tick_count) {
    if (player == NULL || !player->inserted || player->segment_count == 0) {
        return;
    }
    player->play_index = 0;
    player->playing = true;
    player->segment_end_tick = tick_count + player->segments[0].duration_ticks;
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
        player->play_index++;
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
