#ifndef NEXT_SPRITE_H
#define NEXT_SPRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum
{
    NEXT_SPRITE_MAX = 128,
    NEXT_SPRITE_PATTERN_RAM_SIZE = 16384,
    NEXT_SPRITE_SIZE = 16,
    NEXT_SPRITE_DISPLAY_WIDTH = 320,
    NEXT_SPRITE_LINE_CLOCK_BUDGET = 1700
};

typedef struct NextSpriteAttr
{
    int x;
    int y;
    uint8_t pattern;
    uint8_t palette;
    bool visible;
    bool x_mirror;
    bool y_mirror;
    bool rotate;
    bool extended;
    uint8_t byte4;
} NextSpriteAttr;

typedef struct NextSpriteEngine
{
    NextSpriteAttr attrs[NEXT_SPRITE_MAX];
    uint8_t pattern[NEXT_SPRITE_PATTERN_RAM_SIZE];
    uint8_t sel_sprite;
    uint8_t io_sel_sprite;
    uint16_t sel_patt_addr;
    bool enabled;
    bool collided;
    bool overtime;
    bool zero_on_top;
    bool over_border;
    bool border_clip;
    bool clip_set;
    uint8_t clip_x1;
    uint8_t clip_x2;
    uint8_t clip_y1;
    uint8_t clip_y2;
    int attr_cursor;
    bool attr_extended;
    bool line_covered[NEXT_SPRITE_DISPLAY_WIDTH];
} NextSpriteEngine;

void next_sprite_init(NextSpriteEngine *engine);
void next_sprite_reset(NextSpriteEngine *engine);
void next_sprite_set_enabled(NextSpriteEngine *engine, bool on);
void next_sprite_set_zero_on_top(NextSpriteEngine *engine, bool on);
void next_sprite_set_over_border(NextSpriteEngine *engine, bool on);
void next_sprite_set_border_clip(NextSpriteEngine *engine, bool on);
void next_sprite_set_clip(NextSpriteEngine *engine, uint8_t x1, uint8_t x2, uint8_t y1, uint8_t y2);
void next_sprite_select_sprite(NextSpriteEngine *engine, uint8_t value);
void next_sprite_select_io_sprite(NextSpriteEngine *engine, uint8_t value);
void next_sprite_select_slot(NextSpriteEngine *engine, uint8_t value);
void next_sprite_apply_attr_byte(NextSpriteEngine *engine, int idx, uint8_t value);
bool next_sprite_write_attr(NextSpriteEngine *engine, uint8_t value);
void next_sprite_write_pattern_byte(NextSpriteEngine *engine, uint8_t value);
uint8_t next_sprite_read_status(NextSpriteEngine *engine);
uint8_t next_sprite_selected_sprite(const NextSpriteEngine *engine);
uint8_t next_sprite_selected_io_sprite(const NextSpriteEngine *engine);

/* Renders one display scanline's sprite pixels as palette indices into dst.
   present is set for each non-transparent pixel; both arrays must contain
   NEXT_SPRITE_DISPLAY_WIDTH bytes. */
void next_sprite_render_scanline(NextSpriteEngine *engine, int y,
                                 uint8_t transparent_index,
                                 uint8_t *dst, uint8_t *present, int width);

#endif
