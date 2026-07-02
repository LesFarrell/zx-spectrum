#ifndef Z80_H
#define Z80_H

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t (*z80_read_memory_fn)(void *context, uint16_t address);
typedef void (*z80_write_memory_fn)(void *context, uint16_t address, uint8_t value);
typedef uint8_t (*z80_read_port_fn)(void *context, uint16_t port);
typedef void (*z80_write_port_fn)(void *context, uint16_t port, uint8_t value);

typedef struct Z80 {
    uint16_t af;
    uint16_t bc;
    uint16_t de;
    uint16_t hl;

    uint16_t af_alt;
    uint16_t bc_alt;
    uint16_t de_alt;
    uint16_t hl_alt;

    uint16_t ix;
    uint16_t iy;
    uint16_t sp;
    uint16_t pc;

    uint8_t i;
    uint8_t r;

    uint8_t iff1;
    uint8_t iff2;
    uint8_t im;

    bool halted;
    bool pending_irq;

    int ei_delay;
    uint64_t cycles;

    void *context;
    z80_read_memory_fn read_memory;
    z80_write_memory_fn write_memory;
    z80_read_port_fn read_port;
    z80_write_port_fn write_port;
} Z80;

void z80_init(
    Z80 *cpu,
    void *context,
    z80_read_memory_fn read_memory,
    z80_write_memory_fn write_memory,
    z80_read_port_fn read_port,
    z80_write_port_fn write_port
);

void z80_reset(Z80 *cpu);
int z80_step(Z80 *cpu);
void z80_request_interrupt(Z80 *cpu);

#endif
