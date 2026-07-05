#ifndef TAPE_H
#define TAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TapeSegment {
    uint32_t duration_ticks;
    bool level_high;
} TapeSegment;

typedef struct TapeBlock {
    uint8_t *bytes;
    size_t length;
} TapeBlock;

typedef struct TapePlayer {
    TapeSegment *segments;
    size_t segment_count;
    size_t segment_capacity;
    TapeBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    size_t next_block_index;
    size_t play_index;
    uint64_t segment_end_tick;
    uint32_t tick_hz;
    bool inserted;
    bool playing;
} TapePlayer;

void tape_init(TapePlayer *player);
void tape_discard(TapePlayer *player);
bool tape_load_file(
    TapePlayer *player,
    const char *path,
    uint32_t tick_hz,
    bool stop_in_48k_mode,
    char *error_buffer,
    size_t error_buffer_size
);
void tape_rewind(TapePlayer *player, uint32_t tick_hz);
void tape_start(TapePlayer *player, uint64_t tick_count);
void tape_stop(TapePlayer *player);
bool tape_input_level(TapePlayer *player, uint64_t tick_count);
bool tape_has_tape(const TapePlayer *player);
bool tape_try_fast_load(TapePlayer *player, void *machine);

#endif
