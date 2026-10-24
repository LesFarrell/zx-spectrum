#include "next_dma.h"

#include "next_machine.h"

enum {
    NEXT_DMA_RESET_CYCLE_LEN = 3
};

static uint8_t next_dma_cycle_len(uint8_t value) {
    switch (value & 0x03) {
    case 0x01:
        return 3;
    case 0x02:
        return 2;
    default:
        return 4;
    }
}

static uint8_t next_dma_addr_mode(uint8_t value) {
    if (value & 0x20) {
        return NEXT_DMA_ADDR_FIXED;
    }
    if (value & 0x10) {
        return NEXT_DMA_ADDR_INCREMENT;
    }
    return NEXT_DMA_ADDR_DECREMENT;
}

static uint16_t next_dma_step_addr(uint16_t address, uint8_t mode) {
    switch (mode) {
    case NEXT_DMA_ADDR_INCREMENT:
        return (uint16_t)(address + 1);
    case NEXT_DMA_ADDR_DECREMENT:
        return (uint16_t)(address - 1);
    default:
        return address;
    }
}

static void next_dma_push(NextDma *dma, uint8_t op) {
    if (dma->pending_count < NEXT_DMA_PENDING_CAPACITY) {
        dma->pending[dma->pending_count++] = op;
    }
}

static uint16_t next_dma_counter_init(const NextDma *dma) {
    return dma->z80_mode ? 0xFFFF : 0;
}

static int next_dma_first_read_reg(const NextDma *dma) {
    int reg;
    for (reg = 0; reg < 7; ++reg) {
        if (dma->read_mask & (1u << reg)) {
            return reg;
        }
    }
    return 0;
}

static int next_dma_next_read_reg(const NextDma *dma, int cur) {
    int i;
    for (i = 1; i <= 7; ++i) {
        int reg = (cur + i) % 7;
        if (dma->read_mask & (1u << reg)) {
            return reg;
        }
    }
    return 0;
}

void next_dma_init(NextDma *dma) {
    for (size_t i = 0; i < sizeof(*dma); ++i) {
        ((uint8_t *)dma)[i] = 0;
    }
    dma->a_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
    dma->b_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
    dma->read_mask = 0x7F;
}

void next_dma_reset(NextDma *dma) {
    dma->wedged = false;
    dma->pending_count = 0;
    dma->active_burst = false;
    dma->hold_cpu = false;
    dma->in_transfer = false;
    dma->remaining = 0;
    dma->next_due = 0;
    dma->counter = 0;
    dma->a_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
    dma->b_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
    dma->prescaler = 0;
    dma->mode = NEXT_DMA_MODE_CONTINUOUS;
    dma->auto_restart = false;
    dma->read_mask = 0x7F;
    dma->read_reg = 0;
    dma->end_of_block = false;
    dma->at_least_one = false;
}

void next_dma_set_z80_mode(NextDma *dma, bool on) {
    dma->z80_mode = on;
}

static uint8_t next_dma_endpoint_read(
    NextMachine *machine,
    bool is_io,
    uint16_t address
) {
    if (is_io) {
        return next_machine_read_port(machine, address);
    }
    return next_machine_read_memory(machine, address);
}

static void next_dma_endpoint_write(
    NextMachine *machine,
    bool is_io,
    uint16_t address,
    uint8_t value
) {
    if (is_io) {
        next_machine_write_port(machine, address, value);
        return;
    }
    next_machine_write_memory(machine, address, value);
}

static uint8_t next_dma_port_read(NextDma *dma, NextMachine *machine, bool a) {
    if (a) {
        uint8_t v = next_dma_endpoint_read(machine, dma->a_is_io, dma->cur_a);
        dma->cur_a = next_dma_step_addr(dma->cur_a, dma->a_mode);
        return v;
    }
    {
        uint8_t v = next_dma_endpoint_read(machine, dma->b_is_io, dma->cur_b);
        dma->cur_b = next_dma_step_addr(dma->cur_b, dma->b_mode);
        return v;
    }
}

static void next_dma_port_write(
    NextDma *dma,
    NextMachine *machine,
    bool a,
    uint8_t value
) {
    if (a) {
        next_dma_endpoint_write(machine, dma->a_is_io, dma->cur_a, value);
        dma->cur_a = next_dma_step_addr(dma->cur_a, dma->a_mode);
        return;
    }
    next_dma_endpoint_write(machine, dma->b_is_io, dma->cur_b, value);
    dma->cur_b = next_dma_step_addr(dma->cur_b, dma->b_mode);
}

/* byteCycles: read cycle length + write cycle length, source/dest per
   direction. */
static uint64_t next_dma_byte_cycles_for(const NextDma *dma) {
    uint8_t src_cyc = dma->a_cycle_len;
    uint8_t dst_cyc = dma->b_cycle_len;
    if (!dma->a_to_b) {
        src_cyc = dma->b_cycle_len;
        dst_cyc = dma->a_cycle_len;
    }
    return (uint64_t)src_cyc + (uint64_t)dst_cyc;
}

static uint64_t next_dma_prescaler_tstates(const NextMachine *machine, const NextDma *dma) {
    uint64_t mul = 1;
    int speed = machine->cpu_speed_multiplier;
    if (speed > 0) {
        mul = (uint64_t)speed;
    }
    return 4ull * (uint64_t)dma->prescaler * mul;
}

static uint64_t next_dma_per_byte_cycles(const NextMachine *machine, const NextDma *dma) {
    uint64_t per = next_dma_byte_cycles_for(dma);
    uint64_t p = next_dma_prescaler_tstates(machine, dma);
    if (p > per) {
        per = p;
    }
    return per;
}

static void next_dma_finish_block(NextDma *dma) {
    dma->end_of_block = true;
    if (dma->auto_restart) {
        dma->cur_a = dma->port_a_start;
        dma->cur_b = dma->port_b_start;
        dma->counter = next_dma_counter_init(dma);
    } else {
        dma->at_least_one = false;
    }
}

static void next_dma_run_block(NextMachine *machine, NextDma *dma) {
    bool src_is_a;
    if (dma->bus_delay) {
        dma->stalled = true;
        return;
    }
    dma->stalled = false;
    src_is_a = dma->a_to_b;
    dma->in_transfer = true;
    while (dma->remaining > 0) {
        uint8_t b = next_dma_port_read(dma, machine, src_is_a);
        dma->counter++;
        dma->remaining--;
        next_dma_port_write(dma, machine, !src_is_a, b);
        if (!dma->in_transfer) {
            return;
        }
        dma->at_least_one = true;
        if (dma->bus_delay && dma->remaining > 0) {
            dma->in_transfer = false;
            dma->stalled = true;
            return;
        }
    }
    dma->in_transfer = false;
    dma->pending_charge += dma->last_duration;
    next_dma_finish_block(dma);
}

/* Bytes left in the block from the current counter position. */
static int next_dma_block_bytes(const NextDma *dma) {
    int moved = 0;
    uint16_t c;
    for (c = dma->counter;;) {
        moved++;
        c++;
        if (c >= dma->block_len) {
            break;
        }
    }
    return moved;
}

static void next_dma_trigger(NextMachine *machine, NextDma *dma) {
    int moved;
    if (dma->in_transfer || dma->active_burst || dma->stalled) {
        return;
    }
    moved = next_dma_block_bytes(dma);
    dma->last_duration = (uint64_t)(unsigned)moved * next_dma_per_byte_cycles(machine, dma);

    /* A prescaler paces each byte (typically DAC sample playback). Burst
       mode returns the bus to the CPU between bytes; continuous mode keeps
       it, so the CPU is held until the block completes. */
    if (dma->prescaler != 0) {
        dma->active_burst = true;
        dma->hold_cpu = dma->mode == NEXT_DMA_MODE_CONTINUOUS;
        dma->remaining = moved;
        dma->next_due = machine->ticks;
        return;
    }

    dma->remaining = moved;
    next_dma_run_block(machine, dma);
}

void next_dma_step(NextMachine *machine, uint64_t now) {
    NextDma *dma = &machine->dma;
    uint64_t per;
    bool src_is_a;
    if (!dma->active_burst) {
        return;
    }
    if (dma->bus_delay) {
        dma->next_due = now;
        return;
    }
    per = next_dma_per_byte_cycles(machine, dma);
    src_is_a = dma->a_to_b;
    dma->in_transfer = true;
    while (dma->remaining > 0 && now >= dma->next_due) {
        uint8_t b = next_dma_port_read(dma, machine, src_is_a);
        dma->counter++;
        dma->remaining--;
        next_dma_port_write(dma, machine, !src_is_a, b);
        if (!dma->in_transfer) {
            return;
        }
        dma->at_least_one = true;
        if (!dma->hold_cpu) {
            /* A held CPU is not running, so the bus time is already spent. */
            dma->pending_charge += next_dma_byte_cycles_for(dma);
        }
        dma->next_due += per;
    }
    dma->in_transfer = false;
    if (dma->remaining == 0) {
        next_dma_finish_block(dma);
        if (dma->auto_restart && !dma->hold_cpu) {
            /* Burst transfers with auto-restart loop, e.g. a looping
               sample buffer. */
            dma->remaining = next_dma_block_bytes(dma);
        } else {
            dma->active_burst = false;
            dma->hold_cpu = false;
        }
    }
}

bool next_dma_holds_cpu(const NextDma *dma) {
    return dma->active_burst && dma->hold_cpu;
}

uint64_t next_dma_take_charge(NextDma *dma) {
    uint64_t charge = dma->pending_charge;
    dma->pending_charge = 0;
    return charge;
}

static void next_dma_apply_pending(NextDma *dma, uint8_t op, uint8_t value) {
    switch (op) {
    case NEXT_DMA_OP_PORT_A_LOW:
        dma->port_a_start = (uint16_t)((dma->port_a_start & 0xFF00u) | value);
        break;
    case NEXT_DMA_OP_PORT_A_HIGH:
        dma->port_a_start = (uint16_t)((dma->port_a_start & 0x00FFu) | ((uint16_t)value << 8));
        break;
    case NEXT_DMA_OP_BLOCK_LEN_LOW:
        dma->block_len = (uint16_t)((dma->block_len & 0xFF00u) | value);
        break;
    case NEXT_DMA_OP_BLOCK_LEN_HIGH:
        dma->block_len = (uint16_t)((dma->block_len & 0x00FFu) | ((uint16_t)value << 8));
        break;
    case NEXT_DMA_OP_PORT_B_LOW:
        dma->port_b_start = (uint16_t)((dma->port_b_start & 0xFF00u) | value);
        break;
    case NEXT_DMA_OP_PORT_B_HIGH:
        dma->port_b_start = (uint16_t)((dma->port_b_start & 0x00FFu) | ((uint16_t)value << 8));
        break;
    case NEXT_DMA_OP_A_TIMING:
        dma->a_cycle_len = next_dma_cycle_len(value);
        break;
    case NEXT_DMA_OP_B_TIMING:
        dma->b_cycle_len = next_dma_cycle_len(value);
        if (value & 0x20) {
            next_dma_push(dma, NEXT_DMA_OP_PRESCALER);
        }
        break;
    case NEXT_DMA_OP_PRESCALER:
        dma->prescaler = value;
        break;
    case NEXT_DMA_OP_READ_MASK:
        dma->read_mask = (uint8_t)(value & 0x7F);
        dma->read_reg = next_dma_first_read_reg(dma);
        break;
    default:
        break;
    }
}

static void next_dma_command(NextMachine *machine, NextDma *dma, uint8_t value) {
    switch (value) {
    case 0xC3:
        dma->in_transfer = false;
        dma->active_burst = false;
        dma->hold_cpu = false;
        dma->remaining = 0;
        dma->stalled = false;
        dma->end_of_block = false;
        dma->at_least_one = false;
        dma->a_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
        dma->b_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
        dma->prescaler = 0;
        dma->auto_restart = false;
        break;
    case 0xCF:
        dma->cur_a = dma->port_a_start;
        dma->cur_b = dma->port_b_start;
        dma->counter = next_dma_counter_init(dma);
        dma->end_of_block = false;
        break;
    case 0xD3:
        dma->counter = next_dma_counter_init(dma);
        dma->end_of_block = false;
        break;
    case 0x87:
        next_dma_trigger(machine, dma);
        break;
    case 0xC7:
        dma->a_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
        break;
    case 0xCB:
        dma->b_cycle_len = NEXT_DMA_RESET_CYCLE_LEN;
        break;
    case 0xBB:
        dma->pending_count = 0;
        next_dma_push(dma, NEXT_DMA_OP_READ_MASK);
        break;
    case 0xA7:
        dma->read_reg = next_dma_first_read_reg(dma);
        break;
    case 0xBF:
        dma->read_reg = 0;
        break;
    case 0x8B:
        dma->end_of_block = false;
        dma->at_least_one = false;
        break;
    case 0x83:
        dma->active_burst = false;
        dma->hold_cpu = false;
        dma->remaining = 0;
        dma->stalled = false;
        dma->in_transfer = false;
        dma->at_least_one = false;
        break;
    default:
        break;
    }
}

static void next_dma_decode_base(NextMachine *machine, NextDma *dma, uint8_t value) {
    if ((value & 0x80) == 0) {
        if ((value & 0x03) != 0) {
            dma->a_to_b = (value & 0x04) != 0;
            dma->pending_count = 0;
            if (value & 0x08) {
                next_dma_push(dma, NEXT_DMA_OP_PORT_A_LOW);
            }
            if (value & 0x10) {
                next_dma_push(dma, NEXT_DMA_OP_PORT_A_HIGH);
            }
            if (value & 0x20) {
                next_dma_push(dma, NEXT_DMA_OP_BLOCK_LEN_LOW);
            }
            if (value & 0x40) {
                next_dma_push(dma, NEXT_DMA_OP_BLOCK_LEN_HIGH);
            }
        } else if ((value & 0x07) == 0x04) {
            dma->a_mode = next_dma_addr_mode(value);
            dma->a_is_io = (value & 0x08) != 0;
            if (value & 0x40) {
                dma->pending_count = 0;
                next_dma_push(dma, NEXT_DMA_OP_A_TIMING);
            }
        } else if ((value & 0x07) == 0x00) {
            dma->b_mode = next_dma_addr_mode(value);
            dma->b_is_io = (value & 0x08) != 0;
            if (value & 0x40) {
                dma->pending_count = 0;
                next_dma_push(dma, NEXT_DMA_OP_B_TIMING);
            }
        }
        return;
    }
    switch (value & 0x03) {
    case 0x00:
        dma->pending_count = 0;
        if (value & 0x08) {
            next_dma_push(dma, NEXT_DMA_OP_IGNORE);
        }
        if (value & 0x10) {
            next_dma_push(dma, NEXT_DMA_OP_IGNORE);
        }
        if (value & 0x40) {
            next_dma_trigger(machine, dma);
        }
        break;
    case 0x01: {
        unsigned bits = (value >> 5) & 0x03u;
        dma->mode = (bits == 0x02) ? NEXT_DMA_MODE_BURST : NEXT_DMA_MODE_CONTINUOUS;
        dma->pending_count = 0;
        if (value & 0x04) {
            next_dma_push(dma, NEXT_DMA_OP_PORT_B_LOW);
        }
        if (value & 0x08) {
            next_dma_push(dma, NEXT_DMA_OP_PORT_B_HIGH);
        }
        if (dma->pending_count == 0 && (value & 0x10)) {
            dma->wedged = true;
        }
        break;
    }
    case 0x02:
        dma->auto_restart = (value & 0x20) != 0;
        break;
    default:
        next_dma_command(machine, dma, value);
        break;
    }
}

void next_dma_write_command(NextMachine *machine, uint8_t value) {
    NextDma *dma = &machine->dma;
    if (dma->wedged) {
        return;
    }
    if (dma->pending_count > 0) {
        uint8_t op = dma->pending[0];
        unsigned i;
        for (i = 1; i < dma->pending_count; ++i) {
            dma->pending[i - 1] = dma->pending[i];
        }
        dma->pending_count--;
        next_dma_apply_pending(dma, op, value);
        return;
    }
    next_dma_decode_base(machine, dma, value);
}

static uint8_t next_dma_status_byte(const NextDma *dma) {
    uint8_t s = 0x1A;
    if (!dma->end_of_block) {
        s |= 0x20;
    }
    if (dma->at_least_one) {
        s |= 0x01;
    }
    return s;
}

static uint8_t next_dma_reg_value(const NextDma *dma, int reg) {
    switch (reg) {
    case 1:
        return (uint8_t)dma->counter;
    case 2:
        return (uint8_t)(dma->counter >> 8);
    case 3:
        return (uint8_t)dma->cur_a;
    case 4:
        return (uint8_t)(dma->cur_a >> 8);
    case 5:
        return (uint8_t)dma->cur_b;
    case 6:
        return (uint8_t)(dma->cur_b >> 8);
    default:
        return next_dma_status_byte(dma);
    }
}

uint8_t next_dma_read_command(NextMachine *machine) {
    NextDma *dma = &machine->dma;
    uint8_t v = next_dma_reg_value(dma, dma->read_reg);
    dma->read_reg = next_dma_next_read_reg(dma, dma->read_reg);
    return v;
}
