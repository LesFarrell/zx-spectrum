#define CHIPS_IMPL
#include "chips/z80.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static unsigned run_instruction(const uint8_t *program, size_t size, z80_t *cpu)
{
    uint8_t memory[65536] = {0};
    uint64_t pins = z80_init(cpu);
    unsigned ticks = 0;
    bool armed = false;
    memcpy(memory, program, size);
    pins = z80_prefetch(cpu, 0);
    while (ticks < 100) {
        if ((pins & Z80_MREQ) != 0 && (pins & Z80_RD) != 0)
            Z80_SET_DATA(pins, memory[Z80_GET_ADDR(pins)]);
        pins = z80_tick(cpu, pins);
        ++ticks;
        if (!z80_opdone(cpu))
            armed = true;
        else if (armed)
            break;
    }
    CHECK(ticks < 100);
    /* z80_prefetch() has already issued the first M1/T1 cycle. */
    return ticks > 0 ? ticks - 1 : 0;
}

static void expect(const uint8_t *program, size_t size, unsigned expected)
{
    z80_t cpu;
    const unsigned actual = run_instruction(program, size, &cpu);
    if (actual != expected)
        fprintf(stderr, "timing mismatch opcode %02X: got %u expected %u\n", program[0], actual, expected);
    CHECK(actual == expected);
}

int main(void)
{
    static const uint8_t nop[] = { 0x00 };
    static const uint8_t ld_bc[] = { 0x01, 0x34, 0x12 };
    static const uint8_t ld_mem_a[] = { 0x32, 0x00, 0x80 };
    static const uint8_t ld_a_mem[] = { 0x3A, 0x00, 0x80 };
    static const uint8_t jr[] = { 0x18, 0x00 };
    static const uint8_t jp[] = { 0xC3, 0x00, 0x00 };
    static const uint8_t call[] = { 0xCD, 0x00, 0x00 };
    static const uint8_t ret[] = { 0xC9 };
    static const uint8_t cb_reg[] = { 0xCB, 0x00 };
    static const uint8_t cb_mem[] = { 0xCB, 0x46 };
    static const uint8_t ed_ldi[] = { 0xED, 0xA0 };
    static const uint8_t dd_ld[] = { 0xDD, 0x21, 0x34, 0x12 };
    static const uint8_t dd_mem[] = { 0xDD, 0x36, 0x01, 0x7F };

    expect(nop, sizeof(nop), 4);
    expect(ld_bc, sizeof(ld_bc), 10);
    expect(ld_mem_a, sizeof(ld_mem_a), 13);
    expect(ld_a_mem, sizeof(ld_a_mem), 13);
    expect(jr, sizeof(jr), 12);
    expect(jp, sizeof(jp), 10);
    expect(call, sizeof(call), 17);
    expect(ret, sizeof(ret), 10);
    expect(cb_reg, sizeof(cb_reg), 8);
    expect(cb_mem, sizeof(cb_mem), 12);
    expect(ed_ldi, sizeof(ed_ldi), 16);
    expect(dd_ld, sizeof(dd_ld), 14);
    expect(dd_mem, sizeof(dd_mem), 19);

    if (failures != 0) {
        fprintf(stderr, "%d Z80 timing checks failed.\n", failures);
        return 1;
    }
    puts("All Z80 timing checks passed.");
    return 0;
}
