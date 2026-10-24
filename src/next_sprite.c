#include "next_sprite.h"

#include <string.h>

typedef struct NextSpriteAnchor
{
    int x;
    int y;
    uint8_t paloff;
    int pattern;
    bool vis;
    bool h;
    bool rel_type;
    bool rotate;
    bool xmirror;
    bool ymirror;
    unsigned xscale_bits;
    unsigned yscale_bits;
} NextSpriteAnchor;

void next_sprite_init(NextSpriteEngine *engine)
{
    memset(engine, 0, sizeof(*engine));
}

void next_sprite_reset(NextSpriteEngine *engine)
{
    memset(engine->attrs, 0, sizeof(engine->attrs));
    engine->sel_sprite = 0;
    engine->io_sel_sprite = 0;
    engine->sel_patt_addr = 0;
    engine->enabled = false;
    engine->collided = false;
    engine->overtime = false;
    engine->zero_on_top = false;
    engine->over_border = false;
    engine->border_clip = false;
    engine->clip_set = false;
    engine->attr_cursor = 0;
    engine->attr_extended = false;
}

void next_sprite_set_enabled(NextSpriteEngine *engine, bool on) { engine->enabled = on; }

void next_sprite_set_zero_on_top(NextSpriteEngine *engine, bool on) { engine->zero_on_top = on; }

void next_sprite_set_over_border(NextSpriteEngine *engine, bool on) { engine->over_border = on; }

void next_sprite_set_border_clip(NextSpriteEngine *engine, bool on) { engine->border_clip = on; }

void next_sprite_set_clip(NextSpriteEngine *engine, uint8_t x1, uint8_t x2, uint8_t y1, uint8_t y2)
{
    engine->clip_x1 = x1;
    engine->clip_x2 = x2;
    engine->clip_y1 = y1;
    engine->clip_y2 = y2;
    engine->clip_set = true;
}

void next_sprite_select_sprite(NextSpriteEngine *engine, uint8_t value)
{
    engine->sel_sprite = (uint8_t)(value & 0x7F);
}

void next_sprite_select_io_sprite(NextSpriteEngine *engine, uint8_t value)
{
    engine->io_sel_sprite = (uint8_t)(value & 0x7F);
    engine->attr_cursor = 0;
    engine->attr_extended = false;
}

void next_sprite_select_slot(NextSpriteEngine *engine, uint8_t value)
{
    next_sprite_select_io_sprite(engine, value);
    {
        uint16_t addr = (uint16_t)((value & 0x3F) * 256);
        if ((value & 0x80) != 0)
            addr = (uint16_t)(addr + 128);
        engine->sel_patt_addr = addr;
    }
}

static void next_sprite_apply_attr(NextSpriteAttr *sprite, int idx, uint8_t value)
{
    switch (idx)
    {
    case 0:
        sprite->x = (sprite->x & 0x100) | (int)value;
        break;
    case 1:
        sprite->y = (int)value;
        break;
    case 2:
        sprite->palette = (uint8_t)((value >> 4) & 0x0F);
        if ((value & 0x01) != 0)
            sprite->x |= 0x100;
        else
            sprite->x &= ~0x100;
        sprite->x_mirror = (value & 0x08) != 0;
        sprite->y_mirror = (value & 0x04) != 0;
        sprite->rotate = (value & 0x02) != 0;
        break;
    case 3:
        sprite->visible = (value & 0x80) != 0;
        sprite->extended = (value & 0x40) != 0;
        sprite->pattern = (uint8_t)(value & 0x3F);
        break;
    case 4:
        sprite->byte4 = value;
        break;
    default:
        break;
    }
}

void next_sprite_apply_attr_byte(NextSpriteEngine *engine, int idx, uint8_t value)
{
    next_sprite_apply_attr(&engine->attrs[engine->sel_sprite], idx, value);
}

bool next_sprite_write_attr(NextSpriteEngine *engine, uint8_t value)
{
    bool advanced = false;
    next_sprite_apply_attr(&engine->attrs[engine->io_sel_sprite],
                           engine->attr_cursor, value);
    if (engine->attr_cursor == 3)
        engine->attr_extended = (value & 0x40) != 0;
    engine->attr_cursor++;
    {
        const int count = engine->attr_extended ? 5 : 4;
        if (engine->attr_cursor >= count)
        {
            engine->io_sel_sprite = (uint8_t)((engine->io_sel_sprite + 1) & 0x7F);
            engine->attr_cursor = 0;
            engine->attr_extended = false;
            advanced = true;
        }
    }
    return advanced;
}

void next_sprite_write_pattern_byte(NextSpriteEngine *engine, uint8_t value)
{
    engine->pattern[engine->sel_patt_addr] = value;
    engine->sel_patt_addr = (uint16_t)((engine->sel_patt_addr + 1) %
                                       NEXT_SPRITE_PATTERN_RAM_SIZE);
}

uint8_t next_sprite_read_status(NextSpriteEngine *engine)
{
    uint8_t status = 0;
    if (engine->collided)
        status |= 0x01;
    if (engine->overtime)
        status |= 0x02;
    engine->collided = false;
    engine->overtime = false;
    return status;
}

uint8_t next_sprite_selected_sprite(const NextSpriteEngine *engine)
{
    return engine->sel_sprite;
}

uint8_t next_sprite_selected_io_sprite(const NextSpriteEngine *engine)
{
    return engine->io_sel_sprite;
}

static void next_sprite_effective_clip(
    const NextSpriteEngine *engine, int *xs, int *xe, int *ys, int *ye)
{
    const int x1 = engine->clip_x1;
    const int x2 = engine->clip_x2;
    const int y1 = engine->clip_y1;
    const int y2 = engine->clip_y2;
    if (engine->over_border)
    {
        if (!engine->border_clip)
        {
            *xs = 0;
            *xe = 319;
            *ys = 0;
            *ye = 255;
            return;
        }
        *xs = x1 * 2;
        *xe = x2 * 2 + 1;
        *ys = y1;
        *ye = y2;
        return;
    }
    {
        const int sx1 = (((x1 >> 5) + 1) << 5) | (x1 & 0x1F);
        const int sx2 = (((x2 >> 5) + 1) << 5) | (x2 & 0x1F);
        const int sy1 = (((y1 >> 5) + 1) << 5) | (y1 & 0x1F);
        const int sy2 = (((y2 >> 5) + 1) << 5) | (y2 & 0x1F);
        *xs = sx1;
        *xe = sx2;
        *ys = sy1;
        *ye = sy2;
    }
}

static void next_sprite_cover(NextSpriteEngine *engine, int sx)
{
    if (engine->line_covered[sx])
        engine->collided = true;
    else
        engine->line_covered[sx] = true;
}

static void next_sprite_latch_anchor(NextSpriteAnchor *anc, const NextSpriteAttr *raw)
{
    anc->x = raw->x;
    {
        int y = raw->y;
        int pat = raw->pattern & 0x3F;
        if (raw->extended)
        {
            if ((raw->byte4 & 0x01) != 0)
                y |= 0x100;
            if ((raw->byte4 & 0x40) != 0)
                pat |= 0x40;
        }
        anc->y = y;
        anc->pattern = pat;
    }
    anc->paloff = raw->palette;
    anc->vis = raw->visible;
    anc->h = raw->extended && (raw->byte4 & 0x80) != 0;
    anc->rel_type = raw->extended && (raw->byte4 & 0x20) != 0;
    if (anc->rel_type)
    {
        anc->rotate = raw->rotate;
        anc->xmirror = raw->x_mirror;
        anc->ymirror = raw->y_mirror;
        anc->xscale_bits = (raw->byte4 >> 3) & 0x03;
        anc->yscale_bits = (raw->byte4 >> 1) & 0x03;
    }
    else
    {
        anc->rotate = false;
        anc->xmirror = false;
        anc->ymirror = false;
        anc->xscale_bits = 0;
        anc->yscale_bits = 0;
    }
}

static NextSpriteAttr next_sprite_resolve_relative(
    const NextSpriteAttr *raw, const NextSpriteAnchor *anc)
{
    int rx0 = (int)(int8_t)(uint8_t)raw->x;
    int ry0 = (int)(int8_t)(uint8_t)raw->y;
    int rx1;
    int ry1;
    NextSpriteAttr eff;
    uint8_t b4 = 0;
    int eff_x;
    int eff_y;

    if (anc->rotate)
    {
        const int t = rx0;
        rx0 = ry0;
        ry0 = t;
    }
    rx1 = rx0;
    ry1 = ry0;
    if (anc->rotate != anc->xmirror)
        rx1 = -rx0;
    if (anc->ymirror)
        ry1 = -ry0;
    eff_x = (anc->x + (rx1 << anc->xscale_bits)) & 0x1FF;
    eff_y = (anc->y + (ry1 << anc->yscale_bits)) & 0x1FF;

    memset(&eff, 0, sizeof(eff));
    eff.extended = true;
    eff.visible = anc->vis && raw->visible;
    eff.x = (int)eff_x;
    eff.y = (int)(eff_y & 0xFF);
    if (eff_y & 0x100)
        b4 |= 0x01;
    if (anc->h)
        b4 |= 0x80;

    if (raw->x & 0x100)
        eff.palette = (uint8_t)((anc->paloff + raw->palette) & 0x0F);
    else
        eff.palette = raw->palette;

    {
        int pat = raw->pattern & 0x3F;
        if ((raw->byte4 & 0x01) != 0)
            pat = (pat + (anc->pattern & 0x3F)) & 0x3F;
        if (anc->h && (raw->byte4 & 0x20) != 0)
            pat |= 0x40;
        eff.pattern = (uint8_t)(pat & 0x3F);
        if (pat & 0x40)
            b4 |= 0x40;
    }

    if (!anc->rel_type)
    {
        eff.x_mirror = raw->x_mirror;
        eff.y_mirror = raw->y_mirror;
        eff.rotate = raw->rotate;
        b4 = (uint8_t)(b4 | (raw->byte4 & 0x1E));
    }
    else
    {
        bool xm = raw->x_mirror;
        bool ym = raw->y_mirror;
        if (anc->rotate)
        {
            xm = raw->y_mirror != raw->rotate;
            ym = raw->x_mirror != raw->rotate;
        }
        eff.x_mirror = anc->xmirror != xm;
        eff.y_mirror = anc->ymirror != ym;
        eff.rotate = anc->rotate != raw->rotate;
        b4 = (uint8_t)(b4 | (uint8_t)(anc->xscale_bits << 3) |
                       (uint8_t)(anc->yscale_bits << 1));
    }
    eff.byte4 = b4;
    return eff;
}

void next_sprite_render_scanline(NextSpriteEngine *engine, int y,
                                 uint8_t transparent_index,
                                 uint8_t *dst, uint8_t *present, int width)
{
    int clip_xs = 0;
    int clip_xe = width - 1;
    int clocks_left = NEXT_SPRITE_LINE_CLOCK_BUDGET;
    bool line_exhausted = false;
    int i;
    NextSpriteAnchor anchor;

    if (!engine->enabled)
        return;

    if (engine->clip_set)
    {
        int xs, xe, ys, ye;
        next_sprite_effective_clip(engine, &xs, &xe, &ys, &ye);
        if (y < ys || y > ye || (!engine->over_border && y >= 224))
            return;
        clip_xs = xs;
        clip_xe = xe;
    }

    memset(engine->line_covered, 0, sizeof(engine->line_covered));

    memset(&anchor, 0, sizeof(anchor));
    for (i = 0; i < NEXT_SPRITE_MAX; ++i)
    {
        const NextSpriteAttr *raw = &engine->attrs[i];
        const NextSpriteAttr *a = raw;
        NextSpriteAttr eff;
        int scale_x = 1;
        int scale_y = 1;
        int slot;
        int sy;
        bool eight_bit = true;
        bool n6 = false;
        int size_y;
        int dy;
        int row;
        int base;
        int span;
        bool x_mirror_eff;
        int size_x;
        int dx;

        if (raw->extended && (raw->byte4 & 0xC0) == 0x40)
        {
            eff = next_sprite_resolve_relative(raw, &anchor);
            a = &eff;
        }
        else
        {
            next_sprite_latch_anchor(&anchor, raw);
        }
        if (!a->visible)
            continue;

        slot = a->pattern & 0x3F;
        sy = a->y;
        if (a->extended)
        {
            scale_x = 1 << ((a->byte4 >> 3) & 0x03);
            scale_y = 1 << ((a->byte4 >> 1) & 0x03);
            n6 = (a->byte4 & 0x40) != 0;
            if ((a->byte4 & 0x01) != 0)
                sy |= 0x100;
            eight_bit = (a->byte4 & 0x80) == 0;
        }

        size_y = NEXT_SPRITE_SIZE * scale_y;
        dy = (y - sy + 512) % 512;
        if (dy >= size_y)
            continue;
        if (clocks_left <= 0)
        {
            engine->overtime = true;
            break;
        }
        --clocks_left;
        row = dy / scale_y;
        base = slot * 256;
        span = 256;
        if (!eight_bit)
        {
            span = 128;
            if (n6)
                base += 128;
        }
        if (base + span > NEXT_SPRITE_PATTERN_RAM_SIZE)
            continue;

        x_mirror_eff = a->x_mirror != a->rotate;
        size_x = NEXT_SPRITE_SIZE * scale_x;
        for (dx = 0; dx < size_x; ++dx)
        {
            const int sx = (a->x + dx) & 0x1FF;
            int col;
            int tc, tr;
            if (clocks_left <= 0)
            {
                engine->overtime = true;
                line_exhausted = true;
                break;
            }
            --clocks_left;
            if (sx < 0 || sx >= width)
                continue;
            if (engine->clip_set && (sx < clip_xs || sx > clip_xe))
                continue;
            col = dx / scale_x;
            tc = col;
            tr = row;
            if (x_mirror_eff)
                tc = (NEXT_SPRITE_SIZE - 1) - tc;
            if (a->y_mirror)
                tr = (NEXT_SPRITE_SIZE - 1) - tr;
            if (a->rotate)
            {
                const int t = tc;
                tc = tr;
                tr = t;
            }
            if (eight_bit)
            {
                const uint8_t idx = engine->pattern[base + tr * 16 + tc];
                if (idx == transparent_index)
                    continue;
                if (!engine->zero_on_top || !engine->line_covered[sx])
                {
                    dst[sx] = (uint8_t)((idx + (a->palette << 4)) & 0xFF);
                    present[sx] = 1;
                }
                next_sprite_cover(engine, sx);
            }
            else
            {
                const uint8_t pix = engine->pattern[base + tr * 8 + (tc >> 1)];
                uint8_t idx;
                if ((tc & 1) == 0)
                    idx = (uint8_t)(pix >> 4);
                else
                    idx = (uint8_t)(pix & 0x0F);
                if (idx == (transparent_index & 0x0F))
                    continue;
                if (!engine->zero_on_top || !engine->line_covered[sx])
                {
                    dst[sx] = (uint8_t)((a->palette << 4) | idx);
                    present[sx] = 1;
                }
                next_sprite_cover(engine, sx);
            }
        }
        if (line_exhausted)
            break;
    }
}
