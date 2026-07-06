#ifndef TAPE_H
#define TAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum TapeStopMode {
    TAPE_STOP_NONE = 0,
    TAPE_STOP_ALWAYS,
    TAPE_STOP_48K_ONLY
} TapeStopMode;

typedef struct TapeSegment {
    uint32_t duration_ticks;
    bool level_high;
    TapeStopMode stop_after;
} TapeSegment;

typedef struct TapeBlock {
    uint8_t *bytes;
    size_t length;
} TapeBlock;

typedef enum TapeAutoloadTarget {
    TAPE_AUTOLOAD_TARGET_UNKNOWN = 0,
    TAPE_AUTOLOAD_TARGET_48_BASIC,
    TAPE_AUTOLOAD_TARGET_128_MENU
} TapeAutoloadTarget;

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
    TapeAutoloadTarget autoload_target;
    bool stop_in_48k_mode;
    bool inserted;
    bool playing;
} TapePlayer;

void tape_init(TapePlayer *player);
void tape_discard(TapePlayer *player);
bool tape_load_file(
    TapePlayer *player,
    const char *path,
    uint32_t tick_hz,
    char *error_buffer,
    size_t error_buffer_size
);
void tape_rewind(TapePlayer *player, uint32_t tick_hz);
void tape_set_stop_mode(TapePlayer *player, bool stop_in_48k_mode);
void tape_start(TapePlayer *player, uint64_t tick_count);
void tape_stop(TapePlayer *player);
bool tape_input_level(TapePlayer *player, uint64_t tick_count);
bool tape_has_tape(const TapePlayer *player);
TapeAutoloadTarget tape_autoload_target(const TapePlayer *player);
bool tape_try_fast_load(TapePlayer *player, void *machine);

#endif
