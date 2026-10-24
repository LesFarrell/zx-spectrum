#include "next_copper.h"

#include "next_machine.h"

NextCopperInstruction next_copper_decode(uint16_t word)
{
    NextCopperInstruction inst;
    inst.op = NEXT_COPPER_OP_NOOP;
    inst.reg = 0;
    inst.value = 0;
    inst.y = 0;
    inst.x = 0;
    if ((word & 0x8000u) == 0)
    {
        const uint8_t reg = (uint8_t)((word >> 8) & 0x7F);
        const uint8_t value = (uint8_t)(word & 0xFF);
        if (word == 0)
        {
            return inst;
        }
        inst.op = NEXT_COPPER_OP_MOVE;
        inst.reg = reg;
        inst.value = value;
        return inst;
    }
    inst.op = NEXT_COPPER_OP_WAIT;
    inst.x = (uint8_t)((word >> 9) & 0x3F);
    inst.y = (uint16_t)(word & 0x01FF);
    return inst;
}

static uint16_t next_copper_wait_threshold(uint8_t x)
{
    return (uint16_t)(((int)x * 8 + NEXT_COPPER_HCOUNT_ORIGIN) & 0x1FF);
}

void next_copper_init(NextCopper *copper)
{
    for (size_t i = 0; i < sizeof(*copper); ++i)
    {
        ((uint8_t *)copper)[i] = 0;
    }
    copper->stopped = true;
}

void next_copper_reset(NextCopper *copper)
{
    copper->write_ptr = 0;
    copper->mode = NEXT_COPPER_START_STOP;
    copper->restart_pending = false;
    copper->pc = 0;
    copper->stopped = true;
    copper->last_scanline = 0;
    copper->wrote = false;
}

void next_copper_write_data(NextCopper *copper, uint8_t value)
{
    const uint16_t word = (uint16_t)(copper->write_ptr >> 1);
    if ((copper->write_ptr & 1) == 0)
    {
        copper->program[word] = (uint16_t)((copper->program[word] & 0x00FF) |
                                           ((uint16_t)value << 8));
    }
    else
    {
        copper->program[word] = (uint16_t)((copper->program[word] & 0xFF00) |
                                           (uint16_t)value);
    }
    copper->write_ptr = (uint16_t)((copper->write_ptr + 1) & (NEXT_COPPER_RAM_BYTES - 1));
}

void next_copper_set_write_ptr_low(NextCopper *copper, uint8_t value)
{
    copper->write_ptr = (uint16_t)((copper->write_ptr & 0x700) | value);
}

void next_copper_set_write_ptr_high_and_mode(NextCopper *copper, uint8_t value)
{
    copper->write_ptr = (uint16_t)((copper->write_ptr & 0xFF) |
                                   ((uint16_t)(value & 0x07) << 8));
    {
        const uint8_t new_mode = (uint8_t)((value >> 6) & 0x03);
        if (new_mode != copper->mode &&
            (new_mode == NEXT_COPPER_START_FROM_ZERO ||
             new_mode == NEXT_COPPER_START_ON_VBL))
        {
            copper->restart_pending = true;
        }
        copper->mode = new_mode;
        copper->stopped = new_mode == NEXT_COPPER_START_STOP;
    }
}

bool next_copper_idle(const NextCopper *copper)
{
    return copper->stopped && !copper->restart_pending;
}

int next_copper_step(
    NextMachine *machine,
    NextCopper *copper,
    uint16_t scanline,
    uint16_t hcount,
    int max_clocks)
{
    int spent = 0;
    copper->wrote = false;
    if (copper->mode == NEXT_COPPER_START_ON_VBL && scanline < copper->last_scanline)
    {
        copper->pc = 0;
    }
    copper->last_scanline = scanline;
    while (spent < max_clocks && copper->pc < NEXT_COPPER_MAX_INSTRUCTIONS)
    {
        if (copper->restart_pending)
        {
            copper->restart_pending = false;
            copper->pc = 0;
            spent++;
            continue;
        }
        if (copper->stopped)
        {
            return spent;
        }
        {
            const NextCopperInstruction inst =
                next_copper_decode(copper->program[copper->pc]);
            switch (inst.op)
            {
            case NEXT_COPPER_OP_MOVE:
                copper->wrote = true;
                next_machine_write_nextreg(machine, inst.reg, inst.value);
                copper->pc++;
                spent += 2;
                break;
            case NEXT_COPPER_OP_WAIT:
                if (scanline == inst.y && hcount >= next_copper_wait_threshold(inst.x))
                {
                    copper->pc++;
                    spent++;
                    continue;
                }
                return spent + 1;
            default:
                copper->pc++;
                spent++;
                break;
            }
        }
    }
    if (copper->pc >= NEXT_COPPER_MAX_INSTRUCTIONS)
    {
        copper->pc = 0;
    }
    return spent;
}
