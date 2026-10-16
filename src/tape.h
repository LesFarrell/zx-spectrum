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
    /* First timed segment that must play after this block is fast-loaded. */
    size_t resume_segment_index;
} TapeBlock;

typedef enum TapeAutoloadTarget {
    TAPE_AUTOLOAD_TARGET_UNKNOWN = 0,
    TAPE_AUTOLOAD_TARGET_48_BASIC,
    TAPE_AUTOLOAD_TARGET_128_MENU
} TapeAutoloadTarget;

typedef int (*TapeSelectCallback)(
    void *user_data,
    const char *const *descriptions,
    size_t count
);

typedef enum TapeSaveKind {
    TAPE_SAVE_CODE = 0,
    TAPE_SAVE_PROG
} TapeSaveKind;

typedef struct TapeLoadOptions {
    TapeSelectCallback select;
    void *user_data;
} TapeLoadOptions;

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
bool tape_load_file_with_options(
    TapePlayer *player,
    const char *path,
    uint32_t tick_hz,
    char *error_buffer,
    size_t error_buffer_size,
    const TapeLoadOptions *options
);
bool tape_load_data_with_options(
    TapePlayer *player,
    const uint8_t *data,
    size_t data_size,
    const char *name,
    uint32_t tick_hz,
    char *error_buffer,
    size_t error_buffer_size,
    const TapeLoadOptions *options
);
void tape_rewind(TapePlayer *player, uint32_t tick_hz);
void tape_set_stop_mode(TapePlayer *player, bool stop_in_48k_mode);
void tape_start(TapePlayer *player, uint64_t tick_count);
void tape_stop(TapePlayer *player);
bool tape_input_level(TapePlayer *player, uint64_t tick_count);
bool tape_has_tape(const TapePlayer *player);
TapeAutoloadTarget tape_autoload_target(const TapePlayer *player);
bool tape_try_fast_load(TapePlayer *player, void *machine);
/* Copies the 10-character name from the first standard header block. */
bool tape_first_header_name(const TapePlayer *player, char *name, size_t name_size);

/* Writes a standard two-block TAP image (header + data) from in-memory bytes.
   header_param1 and header_param2 are the format-specific words stored after
   the data length (autostart/variable offset for programs, address/32768 for
   CODE blocks). */
bool tape_write_tap(
    const char *path,
    TapeSaveKind kind,
    const char *name,
    uint16_t header_param1,
    uint16_t length,
    uint16_t header_param2,
    const uint8_t *data,
    char *error_buffer,
    size_t error_buffer_size
);

#endif
