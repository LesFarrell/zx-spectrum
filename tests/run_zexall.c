#define CHIPS_IMPL
#include "chips/z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int done;

static int load_file(const char *path, uint8_t *memory)
{
    FILE *file = fopen(path, "rb");
    long size;
    if (file == NULL) return 0;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 0xFF00 || fread(memory + 0x100, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static uint64_t run_zexall(const char *path)
{
    uint8_t memory[65536] = {0};
    z80_t cpu;
    uint64_t pins;
    uint64_t ticks = 0;

    if (!load_file(path, memory)) {
        fprintf(stderr, "Unable to load %s\n", path);
        return 0;
    }
    /* Match the CP/M I/O traps used by the reference z80emu runner. */
    memory[0] = 0xD3;
    memory[1] = 0x00;
    memory[5] = 0xDB;
    memory[6] = 0x00;
    memory[7] = 0xC9;
    done = 0;
    pins = z80_init(&cpu);
    cpu.sp = 0xF000;
    pins = z80_prefetch(&cpu, 0x0100);
    while (!done && ticks < 100000000000ULL) {
        pins = z80_tick(&cpu, pins);
        ++ticks;
        if ((pins & Z80_MREQ) != 0) {
            const uint16_t address = Z80_GET_ADDR(pins);
            if ((pins & Z80_RD) != 0) {
                Z80_SET_DATA(pins, memory[address]);
            }
            else if ((pins & Z80_WR) != 0) {
                memory[address] = Z80_GET_DATA(pins);
            }
        }
        if ((pins & Z80_IORQ) != 0 && (pins & Z80_WR) != 0 &&
            (Z80_GET_ADDR(pins) & 0x00FFu) == 0) {
            done = 1;
        }
        else if ((pins & Z80_IORQ) != 0 && (pins & Z80_RD) != 0 &&
            (Z80_GET_ADDR(pins) & 0x00FFu) == 0) {
            if (cpu.c == 2) putchar(cpu.e);
            else if (cpu.c == 9) {
                uint16_t address = cpu.de;
                unsigned length = 0;
                while (memory[address] != '$' && length++ < 256) putchar(memory[address++]);
            }
            Z80_SET_DATA(pins, 0);
        }
    }
    if (!done) {
        fprintf(stderr, "ZEXALL timeout after %llu clocks: PC=%04X SP=%04X AF=%02X%02X BC=%04X DE=%04X HL=%04X\n",
            ticks, cpu.pc, cpu.sp, cpu.a, cpu.f, cpu.bc, cpu.de, cpu.hl);
        fprintf(stderr, "Bytes at PC:");
        for (unsigned i = 0; i < 16; ++i)
            fprintf(stderr, " %02X", memory[(uint16_t)(cpu.pc + i)]);
        fprintf(stderr, " | stack:");
        for (unsigned i = 0; i < 8; ++i)
            fprintf(stderr, " %02X", memory[(uint16_t)(cpu.sp + i)]);
        fputc('\n', stderr);
    }
    return done ? ticks : 0;
}

int main(int argc, char **argv)
{
    uint64_t clocks;
    if (argc != 2) return 2;
    clocks = run_zexall(argv[1]);
    if (clocks == 0) return 1;
    fprintf(stderr, "\nZEXALL completed in %llu clocks.\n", clocks);
    return 0;
}
