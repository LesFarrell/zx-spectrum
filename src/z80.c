#include "z80.h"

#include <string.h>

enum {
    FLAG_C = 0x01,
    FLAG_N = 0x02,
    FLAG_PV = 0x04,
    FLAG_X = 0x08,
    FLAG_H = 0x10,
    FLAG_Y = 0x20,
    FLAG_Z = 0x40,
    FLAG_S = 0x80
};

typedef enum IndexPrefix {
    PREFIX_NONE = 0,
    PREFIX_IX = 1,
    PREFIX_IY = 2
} IndexPrefix;

static uint8_t z80_read8(Z80 *cpu, uint16_t address) {
    return cpu->read_memory(cpu->context, address);
}

static void z80_write8(Z80 *cpu, uint16_t address, uint8_t value) {
    cpu->write_memory(cpu->context, address, value);
}

static uint16_t z80_read16(Z80 *cpu, uint16_t address) {
    uint16_t lo = z80_read8(cpu, address);
    uint16_t hi = z80_read8(cpu, (uint16_t)(address + 1));
    return (uint16_t)(lo | (hi << 8));
}

static void z80_write16(Z80 *cpu, uint16_t address, uint16_t value) {
    z80_write8(cpu, address, (uint8_t)(value & 0xFF));
    z80_write8(cpu, (uint16_t)(address + 1), (uint8_t)(value >> 8));
}

static uint8_t z80_next_opcode(Z80 *cpu) {
    uint8_t value = z80_read8(cpu, cpu->pc);
    cpu->pc = (uint16_t)(cpu->pc + 1);
    cpu->r = (uint8_t)((cpu->r & 0x80) | ((cpu->r + 1) & 0x7F));
    return value;
}

static uint8_t z80_next_byte(Z80 *cpu) {
    uint8_t value = z80_read8(cpu, cpu->pc);
    cpu->pc = (uint16_t)(cpu->pc + 1);
    return value;
}

static uint16_t z80_next_word(Z80 *cpu) {
    uint16_t lo = z80_next_byte(cpu);
    uint16_t hi = z80_next_byte(cpu);
    return (uint16_t)(lo | (hi << 8));
}

static void z80_push(Z80 *cpu, uint16_t value) {
    cpu->sp = (uint16_t)(cpu->sp - 1);
    z80_write8(cpu, cpu->sp, (uint8_t)(value >> 8));
    cpu->sp = (uint16_t)(cpu->sp - 1);
    z80_write8(cpu, cpu->sp, (uint8_t)(value & 0xFF));
}

static uint16_t z80_pop(Z80 *cpu) {
    uint16_t lo = z80_read8(cpu, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 1);
    uint16_t hi = z80_read8(cpu, cpu->sp);
    cpu->sp = (uint16_t)(cpu->sp + 1);
    return (uint16_t)(lo | (hi << 8));
}

static uint8_t z80_a(const Z80 *cpu) {
    return (uint8_t)(cpu->af >> 8);
}

static void z80_set_a(Z80 *cpu, uint8_t value) {
    cpu->af = (uint16_t)((cpu->af & 0x00FF) | ((uint16_t)value << 8));
}

static uint8_t z80_f(const Z80 *cpu) {
    return (uint8_t)(cpu->af & 0x00FF);
}

static void z80_set_f(Z80 *cpu, uint8_t value) {
    cpu->af = (uint16_t)((cpu->af & 0xFF00) | value);
}

static uint8_t z80_hi(uint16_t value) {
    return (uint8_t)(value >> 8);
}

static uint8_t z80_lo(uint16_t value) {
    return (uint8_t)(value & 0xFF);
}

static void z80_set_hi(uint16_t *reg, uint8_t value) {
    *reg = (uint16_t)((*reg & 0x00FF) | ((uint16_t)value << 8));
}

static void z80_set_lo(uint16_t *reg, uint8_t value) {
    *reg = (uint16_t)((*reg & 0xFF00) | value);
}

static bool z80_parity(uint8_t value) {
    value ^= (uint8_t)(value >> 4);
    value &= 0x0F;
    return ((0x6996u >> value) & 1u) != 0;
}

static uint8_t z80_flags_sz53(uint8_t value) {
    return (uint8_t)(value & (FLAG_S | FLAG_Y | FLAG_X));
}

static uint8_t z80_flags_sz53p(uint8_t value) {
    uint8_t flags = z80_flags_sz53(value);
    if (value == 0) {
        flags |= FLAG_Z;
    }
    if (z80_parity(value)) {
        flags |= FLAG_PV;
    }
    return flags;
}

static uint8_t z80_inc8(Z80 *cpu, uint8_t value) {
    uint8_t result = (uint8_t)(value + 1);
    uint8_t flags = (uint8_t)(z80_f(cpu) & FLAG_C);
    flags |= (uint8_t)(result & (FLAG_S | FLAG_Y | FLAG_X));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if ((value & 0x0F) == 0x0F) {
        flags |= FLAG_H;
    }
    if (value == 0x7F) {
        flags |= FLAG_PV;
    }
    z80_set_f(cpu, flags);
    return result;
}

static uint8_t z80_dec8(Z80 *cpu, uint8_t value) {
    uint8_t result = (uint8_t)(value - 1);
    uint8_t flags = (uint8_t)((z80_f(cpu) & FLAG_C) | FLAG_N);
    flags |= (uint8_t)(result & (FLAG_S | FLAG_Y | FLAG_X));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if ((value & 0x0F) == 0x00) {
        flags |= FLAG_H;
    }
    if (value == 0x80) {
        flags |= FLAG_PV;
    }
    z80_set_f(cpu, flags);
    return result;
}

static uint8_t z80_add8(Z80 *cpu, uint8_t lhs, uint8_t rhs, uint8_t carry) {
    uint16_t sum = (uint16_t)lhs + rhs + carry;
    uint8_t result = (uint8_t)sum;
    uint8_t flags = (uint8_t)(result & (FLAG_S | FLAG_Y | FLAG_X));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (((lhs ^ rhs ^ result) & 0x10) != 0) {
        flags |= FLAG_H;
    }
    if (((~(lhs ^ rhs)) & (lhs ^ result) & 0x80) != 0) {
        flags |= FLAG_PV;
    }
    if ((sum & 0x100) != 0) {
        flags |= FLAG_C;
    }
    z80_set_f(cpu, flags);
    return result;
}

static uint8_t z80_sub8(Z80 *cpu, uint8_t lhs, uint8_t rhs, uint8_t carry) {
    uint16_t diff = (uint16_t)lhs - rhs - carry;
    uint8_t result = (uint8_t)diff;
    uint8_t flags = (uint8_t)(FLAG_N | (result & (FLAG_S | FLAG_Y | FLAG_X)));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (((lhs ^ rhs ^ result) & 0x10) != 0) {
        flags |= FLAG_H;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x80) != 0) {
        flags |= FLAG_PV;
    }
    if ((diff & 0x100) != 0) {
        flags |= FLAG_C;
    }
    z80_set_f(cpu, flags);
    return result;
}

static void z80_cp8(Z80 *cpu, uint8_t lhs, uint8_t rhs) {
    uint16_t diff = (uint16_t)lhs - rhs;
    uint8_t result = (uint8_t)diff;
    uint8_t flags = (uint8_t)(FLAG_N | (result & FLAG_S));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (((lhs ^ rhs ^ result) & 0x10) != 0) {
        flags |= FLAG_H;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x80) != 0) {
        flags |= FLAG_PV;
    }
    if ((diff & 0x100) != 0) {
        flags |= FLAG_C;
    }
    flags |= (uint8_t)(rhs & (FLAG_Y | FLAG_X));
    z80_set_f(cpu, flags);
}

static uint16_t z80_get_hl_prefixed(Z80 *cpu, IndexPrefix prefix) {
    if (prefix == PREFIX_IX) {
        return cpu->ix;
    }
    if (prefix == PREFIX_IY) {
        return cpu->iy;
    }
    return cpu->hl;
}

static void z80_set_hl_prefixed(Z80 *cpu, IndexPrefix prefix, uint16_t value) {
    if (prefix == PREFIX_IX) {
        cpu->ix = value;
    } else if (prefix == PREFIX_IY) {
        cpu->iy = value;
    } else {
        cpu->hl = value;
    }
}

static uint16_t z80_get_rp(Z80 *cpu, IndexPrefix prefix, int p) {
    switch (p & 3) {
        case 0: return cpu->bc;
        case 1: return cpu->de;
        case 2: return z80_get_hl_prefixed(cpu, prefix);
        default: return cpu->sp;
    }
}

static void z80_set_rp(Z80 *cpu, IndexPrefix prefix, int p, uint16_t value) {
    switch (p & 3) {
        case 0: cpu->bc = value; break;
        case 1: cpu->de = value; break;
        case 2: z80_set_hl_prefixed(cpu, prefix, value); break;
        default: cpu->sp = value; break;
    }
}

static uint16_t z80_get_rp2(Z80 *cpu, IndexPrefix prefix, int p) {
    switch (p & 3) {
        case 0: return cpu->bc;
        case 1: return cpu->de;
        case 2: return z80_get_hl_prefixed(cpu, prefix);
        default: return cpu->af;
    }
}

static void z80_set_rp2(Z80 *cpu, IndexPrefix prefix, int p, uint16_t value) {
    switch (p & 3) {
        case 0: cpu->bc = value; break;
        case 1: cpu->de = value; break;
        case 2: z80_set_hl_prefixed(cpu, prefix, value); break;
        default: cpu->af = value; break;
    }
}

static uint16_t z80_indexed_address(Z80 *cpu, IndexPrefix prefix, int8_t displacement) {
    uint16_t base = prefix == PREFIX_IY ? cpu->iy : cpu->ix;
    return (uint16_t)(base + displacement);
}

static uint8_t z80_get_reg8(Z80 *cpu, IndexPrefix prefix, int code, bool has_indexed_addr, uint16_t indexed_addr) {
    switch (code & 7) {
        case 0: return z80_hi(cpu->bc);
        case 1: return z80_lo(cpu->bc);
        case 2: return z80_hi(cpu->de);
        case 3: return z80_lo(cpu->de);
        case 4:
            if (prefix == PREFIX_IX) {
                return z80_hi(cpu->ix);
            }
            if (prefix == PREFIX_IY) {
                return z80_hi(cpu->iy);
            }
            return z80_hi(cpu->hl);
        case 5:
            if (prefix == PREFIX_IX) {
                return z80_lo(cpu->ix);
            }
            if (prefix == PREFIX_IY) {
                return z80_lo(cpu->iy);
            }
            return z80_lo(cpu->hl);
        case 6:
            if (has_indexed_addr) {
                return z80_read8(cpu, indexed_addr);
            }
            return z80_read8(cpu, z80_get_hl_prefixed(cpu, prefix));
        default:
            return z80_a(cpu);
    }
}

static void z80_set_reg8(
    Z80 *cpu,
    IndexPrefix prefix,
    int code,
    bool has_indexed_addr,
    uint16_t indexed_addr,
    uint8_t value
) {
    switch (code & 7) {
        case 0: z80_set_hi(&cpu->bc, value); break;
        case 1: z80_set_lo(&cpu->bc, value); break;
        case 2: z80_set_hi(&cpu->de, value); break;
        case 3: z80_set_lo(&cpu->de, value); break;
        case 4:
            if (prefix == PREFIX_IX) {
                z80_set_hi(&cpu->ix, value);
            } else if (prefix == PREFIX_IY) {
                z80_set_hi(&cpu->iy, value);
            } else {
                z80_set_hi(&cpu->hl, value);
            }
            break;
        case 5:
            if (prefix == PREFIX_IX) {
                z80_set_lo(&cpu->ix, value);
            } else if (prefix == PREFIX_IY) {
                z80_set_lo(&cpu->iy, value);
            } else {
                z80_set_lo(&cpu->hl, value);
            }
            break;
        case 6:
            if (has_indexed_addr) {
                z80_write8(cpu, indexed_addr, value);
            } else {
                z80_write8(cpu, z80_get_hl_prefixed(cpu, prefix), value);
            }
            break;
        default:
            z80_set_a(cpu, value);
            break;
    }
}

static bool z80_condition(Z80 *cpu, int code) {
    uint8_t f = z80_f(cpu);
    switch (code & 7) {
        case 0: return (f & FLAG_Z) == 0;
        case 1: return (f & FLAG_Z) != 0;
        case 2: return (f & FLAG_C) == 0;
        case 3: return (f & FLAG_C) != 0;
        case 4: return (f & FLAG_PV) == 0;
        case 5: return (f & FLAG_PV) != 0;
        case 6: return (f & FLAG_S) == 0;
        default: return (f & FLAG_S) != 0;
    }
}

static void z80_logic_flags(Z80 *cpu, uint8_t value, uint8_t extra) {
    uint8_t flags = (uint8_t)(z80_flags_sz53p(value) | extra);
    z80_set_f(cpu, flags);
}

static void z80_add_hl(Z80 *cpu, IndexPrefix prefix, uint16_t value) {
    uint16_t hl = z80_get_hl_prefixed(cpu, prefix);
    uint32_t sum = (uint32_t)hl + value;
    uint16_t result = (uint16_t)sum;
    uint8_t flags = (uint8_t)(z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV));
    if ((((hl & 0x0FFF) + (value & 0x0FFF)) & 0x1000) != 0) {
        flags |= FLAG_H;
    }
    if ((sum & 0x10000) != 0) {
        flags |= FLAG_C;
    }
    flags |= (uint8_t)((result >> 8) & (FLAG_Y | FLAG_X));
    z80_set_f(cpu, flags);
    z80_set_hl_prefixed(cpu, prefix, result);
}

static uint16_t z80_adc16(Z80 *cpu, uint16_t lhs, uint16_t rhs, uint8_t carry) {
    uint32_t sum = (uint32_t)lhs + rhs + carry;
    uint16_t result = (uint16_t)sum;
    uint8_t flags = (uint8_t)((result >> 8) & (FLAG_S | FLAG_Y | FLAG_X));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if ((((lhs & 0x0FFF) + (rhs & 0x0FFF) + carry) & 0x1000) != 0) {
        flags |= FLAG_H;
    }
    if ((~(lhs ^ rhs) & (lhs ^ result) & 0x8000) != 0) {
        flags |= FLAG_PV;
    }
    if ((sum & 0x10000) != 0) {
        flags |= FLAG_C;
    }
    z80_set_f(cpu, flags);
    return result;
}

static uint16_t z80_sbc16(Z80 *cpu, uint16_t lhs, uint16_t rhs, uint8_t carry) {
    uint32_t diff = (uint32_t)lhs - rhs - carry;
    uint16_t result = (uint16_t)diff;
    uint8_t flags = (uint8_t)(FLAG_N | ((result >> 8) & (FLAG_S | FLAG_Y | FLAG_X)));
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if ((((lhs & 0x0FFF) - (rhs & 0x0FFF) - carry) & 0x1000) != 0) {
        flags |= FLAG_H;
    }
    if (((lhs ^ rhs) & (lhs ^ result) & 0x8000) != 0) {
        flags |= FLAG_PV;
    }
    if ((diff & 0x10000) != 0) {
        flags |= FLAG_C;
    }
    z80_set_f(cpu, flags);
    return result;
}

static uint8_t z80_rot_rlc(uint8_t value, uint8_t *flags) {
    uint8_t result = (uint8_t)((value << 1) | (value >> 7));
    *flags = z80_flags_sz53p(result);
    if ((value & 0x80) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static uint8_t z80_rot_rrc(uint8_t value, uint8_t *flags) {
    uint8_t result = (uint8_t)((value >> 1) | (value << 7));
    *flags = z80_flags_sz53p(result);
    if ((value & 0x01) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static uint8_t z80_rot_rl(Z80 *cpu, uint8_t value, uint8_t *flags) {
    uint8_t carry = (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0);
    uint8_t result = (uint8_t)((value << 1) | carry);
    *flags = z80_flags_sz53p(result);
    if ((value & 0x80) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static uint8_t z80_rot_rr(Z80 *cpu, uint8_t value, uint8_t *flags) {
    uint8_t carry = (uint8_t)(z80_f(cpu) & FLAG_C ? 0x80 : 0);
    uint8_t result = (uint8_t)((value >> 1) | carry);
    *flags = z80_flags_sz53p(result);
    if ((value & 0x01) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static uint8_t z80_shift_sla(uint8_t value, uint8_t *flags) {
    uint8_t result = (uint8_t)(value << 1);
    *flags = z80_flags_sz53p(result);
    if ((value & 0x80) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static uint8_t z80_shift_sra(uint8_t value, uint8_t *flags) {
    uint8_t result = (uint8_t)((value >> 1) | (value & 0x80));
    *flags = z80_flags_sz53p(result);
    if ((value & 0x01) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static uint8_t z80_shift_sll(uint8_t value, uint8_t *flags) {
    uint8_t result = (uint8_t)((value << 1) | 0x01);
    *flags = z80_flags_sz53p(result);
    if ((value & 0x80) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static uint8_t z80_shift_srl(uint8_t value, uint8_t *flags) {
    uint8_t result = (uint8_t)(value >> 1);
    *flags = z80_flags_sz53p(result);
    if ((value & 0x01) != 0) {
        *flags |= FLAG_C;
    }
    return result;
}

static int z80_execute_cb(Z80 *cpu, uint8_t opcode, IndexPrefix prefix, bool indexed, uint16_t indexed_addr) {
    int x = opcode >> 6;
    int y = (opcode >> 3) & 0x07;
    int z = opcode & 0x07;
    uint8_t value = indexed
        ? z80_read8(cpu, indexed_addr)
        : z80_get_reg8(cpu, prefix, z, false, 0);
    uint8_t flags = 0;

    if (x == 0) {
        uint8_t result = value;
        switch (y) {
            case 0: result = z80_rot_rlc(value, &flags); break;
            case 1: result = z80_rot_rrc(value, &flags); break;
            case 2: result = z80_rot_rl(cpu, value, &flags); break;
            case 3: result = z80_rot_rr(cpu, value, &flags); break;
            case 4: result = z80_shift_sla(value, &flags); break;
            case 5: result = z80_shift_sra(value, &flags); break;
            case 6: result = z80_shift_sll(value, &flags); break;
            default: result = z80_shift_srl(value, &flags); break;
        }
        z80_set_f(cpu, flags);
        if (indexed) {
            z80_write8(cpu, indexed_addr, result);
            if (z != 6) {
                z80_set_reg8(cpu, PREFIX_NONE, z, false, 0, result);
            }
            return z == 6 ? 23 : 23;
        }
        z80_set_reg8(cpu, prefix, z, false, 0, result);
        return z == 6 ? 15 : 8;
    }

    if (x == 1) {
        uint8_t mask = (uint8_t)(1u << y);
        uint8_t result_flags = (uint8_t)((z80_f(cpu) & FLAG_C) | FLAG_H);
        if ((value & mask) == 0) {
            result_flags |= FLAG_Z | FLAG_PV;
        }
        if (y == 7 && (value & mask) != 0) {
            result_flags |= FLAG_S;
        }
        if (indexed) {
            result_flags |= (uint8_t)(indexed_addr >> 8) & (FLAG_Y | FLAG_X);
        } else if (z == 6) {
            result_flags |= value & (FLAG_Y | FLAG_X);
        } else {
            result_flags |= value & (FLAG_Y | FLAG_X);
        }
        z80_set_f(cpu, result_flags);
        return indexed ? 20 : (z == 6 ? 12 : 8);
    }

    if (x == 2) {
        value = (uint8_t)(value & ~(1u << y));
    } else {
        value = (uint8_t)(value | (1u << y));
    }

    if (indexed) {
        z80_write8(cpu, indexed_addr, value);
        if (z != 6) {
            z80_set_reg8(cpu, PREFIX_NONE, z, false, 0, value);
        }
        return 23;
    }
    z80_set_reg8(cpu, prefix, z, false, 0, value);
    return z == 6 ? 15 : 8;
}

static void z80_do_ldi(Z80 *cpu, int step) {
    uint8_t value = z80_read8(cpu, cpu->hl);
    z80_write8(cpu, cpu->de, value);
    cpu->hl = (uint16_t)(cpu->hl + step);
    cpu->de = (uint16_t)(cpu->de + step);
    cpu->bc = (uint16_t)(cpu->bc - 1);

    uint8_t sum = (uint8_t)(value + z80_a(cpu));
    uint8_t flags = (uint8_t)(z80_f(cpu) & FLAG_C);
    if (cpu->bc != 0) {
        flags |= FLAG_PV;
    }
    flags |= (uint8_t)(sum & (FLAG_Y | FLAG_X));
    z80_set_f(cpu, flags);
}

static void z80_do_cpi(Z80 *cpu, int step) {
    uint8_t value = z80_read8(cpu, cpu->hl);
    uint8_t a = z80_a(cpu);
    uint8_t result = (uint8_t)(a - value);
    cpu->hl = (uint16_t)(cpu->hl + step);
    cpu->bc = (uint16_t)(cpu->bc - 1);

    uint8_t flags = (uint8_t)((z80_f(cpu) & FLAG_C) | FLAG_N);
    if (result & 0x80) {
        flags |= FLAG_S;
    }
    if (result == 0) {
        flags |= FLAG_Z;
    }
    if (((a ^ value ^ result) & 0x10) != 0) {
        flags |= FLAG_H;
    }
    if (cpu->bc != 0) {
        flags |= FLAG_PV;
    }
    flags |= (uint8_t)(result & (FLAG_Y | FLAG_X));
    z80_set_f(cpu, flags);
}

static int z80_execute_ed(Z80 *cpu, uint8_t opcode) {
    int x = opcode >> 6;
    int y = (opcode >> 3) & 0x07;
    int z = opcode & 0x07;
    int p = y >> 1;
    int q = y & 1;

    if (x == 1) {
        switch (z) {
            case 0: {
                if (q == 0) {
                    uint8_t value = cpu->read_port(cpu->context, cpu->bc);
                    if (y != 6) {
                        z80_set_reg8(cpu, PREFIX_NONE, y, false, 0, value);
                    }
                    z80_set_f(cpu, (uint8_t)((z80_f(cpu) & FLAG_C) | z80_flags_sz53p(value)));
                    return 12;
                }
                cpu->write_port(
                    cpu->context,
                    cpu->bc,
                    y == 6 ? 0 : z80_get_reg8(cpu, PREFIX_NONE, y, false, 0)
                );
                return 12;
            }
            case 1:
                if (q == 0) {
                    uint16_t value = z80_get_rp(cpu, PREFIX_NONE, p);
                    cpu->hl = z80_sbc16(cpu, cpu->hl, value, (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0));
                } else {
                    uint16_t value = z80_get_rp(cpu, PREFIX_NONE, p);
                    cpu->hl = z80_adc16(cpu, cpu->hl, value, (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0));
                }
                return 15;
            case 2: {
                uint16_t address = z80_next_word(cpu);
                if (q == 0) {
                    z80_write16(cpu, address, z80_get_rp(cpu, PREFIX_NONE, p));
                } else {
                    z80_set_rp(cpu, PREFIX_NONE, p, z80_read16(cpu, address));
                }
                return 20;
            }
            case 4: {
                uint8_t value = z80_sub8(cpu, 0, z80_a(cpu), 0);
                z80_set_a(cpu, value);
                return 8;
            }
            case 5:
                cpu->iff1 = cpu->iff2;
                cpu->pc = z80_pop(cpu);
                return 14;
            case 6:
                switch (y) {
                    case 0:
                    case 1: cpu->im = 0; break;
                    case 2:
                    case 3: cpu->im = 1; break;
                    default: cpu->im = 2; break;
                }
                return 8;
            case 7:
                switch (y) {
                    case 0: cpu->i = z80_a(cpu); return 9;
                    case 1: cpu->r = (uint8_t)((cpu->r & 0x80) | (z80_a(cpu) & 0x7F)); return 9;
                    case 2: {
                        uint8_t value = cpu->i;
                        z80_set_a(cpu, value);
                        uint8_t flags = (uint8_t)((z80_f(cpu) & FLAG_C) | z80_flags_sz53(value));
                        if (value == 0) {
                            flags |= FLAG_Z;
                        }
                        if (cpu->iff2) {
                            flags |= FLAG_PV;
                        }
                        z80_set_f(cpu, flags);
                        return 9;
                    }
                    case 3: {
                        uint8_t value = (uint8_t)((cpu->r & 0x7F) | (cpu->r & 0x80));
                        z80_set_a(cpu, value);
                        uint8_t flags = (uint8_t)((z80_f(cpu) & FLAG_C) | z80_flags_sz53(value));
                        if (value == 0) {
                            flags |= FLAG_Z;
                        }
                        if (cpu->iff2) {
                            flags |= FLAG_PV;
                        }
                        z80_set_f(cpu, flags);
                        return 9;
                    }
                    case 4: {
                        uint8_t old_a = z80_a(cpu);
                        uint8_t value = z80_read8(cpu, cpu->hl);
                        uint8_t low = z80_lo(cpu->af);
                        z80_write8(cpu, cpu->hl, (uint8_t)((value << 4) | (old_a & 0x0F)));
                        z80_set_a(cpu, (uint8_t)((old_a & 0xF0) | (value >> 4)));
                        z80_set_f(cpu, (uint8_t)((low & FLAG_C) | z80_flags_sz53p(z80_a(cpu))));
                        return 18;
                    }
                    case 5: {
                        uint8_t old_a = z80_a(cpu);
                        uint8_t value = z80_read8(cpu, cpu->hl);
                        uint8_t low = z80_lo(cpu->af);
                        z80_write8(cpu, cpu->hl, (uint8_t)((value >> 4) | (old_a << 4)));
                        z80_set_a(cpu, (uint8_t)((old_a & 0xF0) | (value & 0x0F)));
                        z80_set_f(cpu, (uint8_t)((low & FLAG_C) | z80_flags_sz53p(z80_a(cpu))));
                        return 18;
                    }
                    default:
                        return 8;
                }
            default:
                break;
        }
    }

    if (x == 2 && z <= 3 && y >= 4) {
        switch (opcode) {
            case 0xA0: z80_do_ldi(cpu, +1); return 16;
            case 0xA1: z80_do_cpi(cpu, +1); return 16;
            case 0xA2: {
                uint8_t value = cpu->read_port(cpu->context, cpu->bc);
                z80_write8(cpu, cpu->hl, value);
                cpu->hl = (uint16_t)(cpu->hl + 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                z80_set_f(cpu, (uint8_t)(z80_flags_sz53((uint8_t)(z80_hi(cpu->bc))) | (z80_hi(cpu->bc) != 0 ? FLAG_PV : 0) | FLAG_N));
                return 16;
            }
            case 0xA3: {
                uint8_t value = z80_read8(cpu, cpu->hl);
                cpu->write_port(cpu->context, cpu->bc, value);
                cpu->hl = (uint16_t)(cpu->hl + 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                z80_set_f(cpu, (uint8_t)(z80_flags_sz53((uint8_t)(z80_hi(cpu->bc))) | (z80_hi(cpu->bc) != 0 ? FLAG_PV : 0) | FLAG_N));
                return 16;
            }
            case 0xA8: z80_do_ldi(cpu, -1); return 16;
            case 0xA9: z80_do_cpi(cpu, -1); return 16;
            case 0xAA: {
                uint8_t value = cpu->read_port(cpu->context, cpu->bc);
                z80_write8(cpu, cpu->hl, value);
                cpu->hl = (uint16_t)(cpu->hl - 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                z80_set_f(cpu, (uint8_t)(z80_flags_sz53((uint8_t)(z80_hi(cpu->bc))) | (z80_hi(cpu->bc) != 0 ? FLAG_PV : 0) | FLAG_N));
                return 16;
            }
            case 0xAB: {
                uint8_t value = z80_read8(cpu, cpu->hl);
                cpu->write_port(cpu->context, cpu->bc, value);
                cpu->hl = (uint16_t)(cpu->hl - 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                z80_set_f(cpu, (uint8_t)(z80_flags_sz53((uint8_t)(z80_hi(cpu->bc))) | (z80_hi(cpu->bc) != 0 ? FLAG_PV : 0) | FLAG_N));
                return 16;
            }
            case 0xB0:
                z80_do_ldi(cpu, +1);
                if (cpu->bc != 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            case 0xB1:
                z80_do_cpi(cpu, +1);
                if (cpu->bc != 0 && (z80_f(cpu) & FLAG_Z) == 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            case 0xB2: {
                uint8_t value = cpu->read_port(cpu->context, cpu->bc);
                z80_write8(cpu, cpu->hl, value);
                cpu->hl = (uint16_t)(cpu->hl + 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                if (z80_hi(cpu->bc) != 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            }
            case 0xB3: {
                uint8_t value = z80_read8(cpu, cpu->hl);
                cpu->write_port(cpu->context, cpu->bc, value);
                cpu->hl = (uint16_t)(cpu->hl + 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                if (z80_hi(cpu->bc) != 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            }
            case 0xB8:
                z80_do_ldi(cpu, -1);
                if (cpu->bc != 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            case 0xB9:
                z80_do_cpi(cpu, -1);
                if (cpu->bc != 0 && (z80_f(cpu) & FLAG_Z) == 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            case 0xBA: {
                uint8_t value = cpu->read_port(cpu->context, cpu->bc);
                z80_write8(cpu, cpu->hl, value);
                cpu->hl = (uint16_t)(cpu->hl - 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                if (z80_hi(cpu->bc) != 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            }
            case 0xBB: {
                uint8_t value = z80_read8(cpu, cpu->hl);
                cpu->write_port(cpu->context, cpu->bc, value);
                cpu->hl = (uint16_t)(cpu->hl - 1);
                z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                if (z80_hi(cpu->bc) != 0) {
                    cpu->pc = (uint16_t)(cpu->pc - 2);
                    return 21;
                }
                return 16;
            }
            default:
                break;
        }
    }

    return 8;
}

static int z80_execute_main(Z80 *cpu, uint8_t opcode, IndexPrefix prefix) {
    int x = opcode >> 6;
    int y = (opcode >> 3) & 0x07;
    int z = opcode & 0x07;
    int p = y >> 1;
    int q = y & 1;

    if (x == 0) {
        switch (z) {
            case 0:
                switch (y) {
                    case 0:
                        return 4;
                    case 1: {
                        uint16_t tmp = cpu->af;
                        cpu->af = cpu->af_alt;
                        cpu->af_alt = tmp;
                        return 4;
                    }
                    case 2: {
                        int8_t disp = (int8_t)z80_next_byte(cpu);
                        z80_set_hi(&cpu->bc, (uint8_t)(z80_hi(cpu->bc) - 1));
                        if (z80_hi(cpu->bc) != 0) {
                            cpu->pc = (uint16_t)(cpu->pc + disp);
                            return 13;
                        }
                        return 8;
                    }
                    case 3: {
                        int8_t disp = (int8_t)z80_next_byte(cpu);
                        cpu->pc = (uint16_t)(cpu->pc + disp);
                        return 12;
                    }
                    default: {
                        int8_t disp = (int8_t)z80_next_byte(cpu);
                        if (z80_condition(cpu, y - 4)) {
                            cpu->pc = (uint16_t)(cpu->pc + disp);
                            return 12;
                        }
                        return 7;
                    }
                }
            case 1:
                if (q == 0) {
                    z80_set_rp(cpu, prefix, p, z80_next_word(cpu));
                    return 10;
                }
                z80_add_hl(cpu, prefix, z80_get_rp(cpu, PREFIX_NONE, p));
                return prefix == PREFIX_NONE ? 11 : 15;
            case 2:
                switch (y) {
                    case 0: z80_write8(cpu, cpu->bc, z80_a(cpu)); return 7;
                    case 1: z80_set_a(cpu, z80_read8(cpu, cpu->bc)); return 7;
                    case 2: z80_write8(cpu, cpu->de, z80_a(cpu)); return 7;
                    case 3: z80_set_a(cpu, z80_read8(cpu, cpu->de)); return 7;
                    case 4: {
                        uint16_t address = z80_next_word(cpu);
                        z80_write16(cpu, address, z80_get_hl_prefixed(cpu, prefix));
                        return prefix == PREFIX_NONE ? 16 : 20;
                    }
                    case 5: {
                        uint16_t address = z80_next_word(cpu);
                        z80_set_hl_prefixed(cpu, prefix, z80_read16(cpu, address));
                        return prefix == PREFIX_NONE ? 16 : 20;
                    }
                    case 6: {
                        uint16_t address = z80_next_word(cpu);
                        z80_write8(cpu, address, z80_a(cpu));
                        return 13;
                    }
                    default: {
                        uint16_t address = z80_next_word(cpu);
                        z80_set_a(cpu, z80_read8(cpu, address));
                        return 13;
                    }
                }
            case 3:
                if (q == 0) {
                    z80_set_rp(cpu, prefix, p, (uint16_t)(z80_get_rp(cpu, prefix, p) + 1));
                } else {
                    z80_set_rp(cpu, prefix, p, (uint16_t)(z80_get_rp(cpu, prefix, p) - 1));
                }
                return prefix == PREFIX_NONE ? 6 : (p == 2 ? 10 : 6);
            case 4: {
                bool indexed = prefix != PREFIX_NONE && y == 6;
                uint16_t addr = 0;
                if (indexed) {
                    addr = z80_indexed_address(cpu, prefix, (int8_t)z80_next_byte(cpu));
                }
                uint8_t value = z80_get_reg8(cpu, prefix, y, indexed, addr);
                value = z80_inc8(cpu, value);
                z80_set_reg8(cpu, prefix, y, indexed, addr, value);
                return indexed ? 23 : (y == 6 ? 11 : 4);
            }
            case 5: {
                bool indexed = prefix != PREFIX_NONE && y == 6;
                uint16_t addr = 0;
                if (indexed) {
                    addr = z80_indexed_address(cpu, prefix, (int8_t)z80_next_byte(cpu));
                }
                uint8_t value = z80_get_reg8(cpu, prefix, y, indexed, addr);
                value = z80_dec8(cpu, value);
                z80_set_reg8(cpu, prefix, y, indexed, addr, value);
                return indexed ? 23 : (y == 6 ? 11 : 4);
            }
            case 6: {
                bool indexed = prefix != PREFIX_NONE && y == 6;
                uint16_t addr = 0;
                if (indexed) {
                    addr = z80_indexed_address(cpu, prefix, (int8_t)z80_next_byte(cpu));
                }
                uint8_t value = z80_next_byte(cpu);
                z80_set_reg8(cpu, prefix, y, indexed, addr, value);
                return indexed ? 19 : (y == 6 ? 10 : 7);
            }
            default:
                switch (y) {
                    case 0: {
                        uint8_t a = z80_a(cpu);
                        uint8_t result = (uint8_t)((a << 1) | (a >> 7));
                        z80_set_a(cpu, result);
                        z80_set_f(cpu, (uint8_t)((z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV)) | (result & (FLAG_Y | FLAG_X)) | ((a >> 7) & FLAG_C)));
                        return 4;
                    }
                    case 1: {
                        uint8_t a = z80_a(cpu);
                        uint8_t result = (uint8_t)((a >> 1) | (a << 7));
                        z80_set_a(cpu, result);
                        z80_set_f(cpu, (uint8_t)((z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV)) | (result & (FLAG_Y | FLAG_X)) | (a & FLAG_C)));
                        return 4;
                    }
                    case 2: {
                        uint8_t a = z80_a(cpu);
                        uint8_t carry = (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0);
                        uint8_t result = (uint8_t)((a << 1) | carry);
                        z80_set_a(cpu, result);
                        z80_set_f(cpu, (uint8_t)((z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV)) | (result & (FLAG_Y | FLAG_X)) | ((a >> 7) & FLAG_C)));
                        return 4;
                    }
                    case 3: {
                        uint8_t a = z80_a(cpu);
                        uint8_t carry = (uint8_t)(z80_f(cpu) & FLAG_C ? 0x80 : 0);
                        uint8_t result = (uint8_t)((a >> 1) | carry);
                        z80_set_a(cpu, result);
                        z80_set_f(cpu, (uint8_t)((z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV)) | (result & (FLAG_Y | FLAG_X)) | (a & FLAG_C)));
                        return 4;
                    }
                    case 4: {
                        uint8_t old_a = z80_a(cpu);
                        uint8_t adjust = 0;
                        uint8_t carry = (uint8_t)(z80_f(cpu) & FLAG_C);
                        if ((z80_f(cpu) & FLAG_H) != 0 || ((z80_f(cpu) & FLAG_N) == 0 && (old_a & 0x0F) > 9)) {
                            adjust |= 0x06;
                        }
                        if (carry != 0 || ((z80_f(cpu) & FLAG_N) == 0 && old_a > 0x99)) {
                            adjust |= 0x60;
                            carry = FLAG_C;
                        }
                        uint8_t result = (z80_f(cpu) & FLAG_N) != 0 ? (uint8_t)(old_a - adjust) : (uint8_t)(old_a + adjust);
                        z80_set_a(cpu, result);
                        uint8_t flags = (uint8_t)((z80_f(cpu) & FLAG_N) | carry | (result & (FLAG_S | FLAG_Y | FLAG_X)));
                        if (result == 0) {
                            flags |= FLAG_Z;
                        }
                        if (z80_parity(result)) {
                            flags |= FLAG_PV;
                        }
                        if (((old_a ^ result) & 0x10) != 0) {
                            flags |= FLAG_H;
                        }
                        z80_set_f(cpu, flags);
                        return 4;
                    }
                    case 5: {
                        uint8_t value = (uint8_t)~z80_a(cpu);
                        z80_set_a(cpu, value);
                        z80_set_f(cpu, (uint8_t)((z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C)) | FLAG_H | FLAG_N | (value & (FLAG_Y | FLAG_X))));
                        return 4;
                    }
                    case 6:
                        z80_set_f(cpu, (uint8_t)((z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV)) | FLAG_C | (z80_a(cpu) & (FLAG_Y | FLAG_X))));
                        return 4;
                    default: {
                        uint8_t carry = (uint8_t)(z80_f(cpu) & FLAG_C);
                        z80_set_f(cpu, (uint8_t)((z80_f(cpu) & (FLAG_S | FLAG_Z | FLAG_PV)) | (z80_a(cpu) & (FLAG_Y | FLAG_X)) | (carry ? FLAG_H : 0) | (carry ? 0 : FLAG_C)));
                        return 4;
                    }
                }
        }
    }

    if (x == 1) {
        if (y == 6 && z == 6) {
            cpu->halted = true;
            return 4;
        }

        bool indexed = prefix != PREFIX_NONE && (y == 6 || z == 6);
        uint16_t addr = 0;
        if (indexed) {
            addr = z80_indexed_address(cpu, prefix, (int8_t)z80_next_byte(cpu));
        }
        uint8_t value = z80_get_reg8(cpu, prefix, z, indexed, addr);
        z80_set_reg8(cpu, prefix, y, indexed, addr, value);
        return indexed ? 19 : ((y == 6 || z == 6) ? 7 : 4);
    }

    if (x == 2) {
        bool indexed = prefix != PREFIX_NONE && z == 6;
        uint16_t addr = 0;
        if (indexed) {
            addr = z80_indexed_address(cpu, prefix, (int8_t)z80_next_byte(cpu));
        }
        uint8_t value = z80_get_reg8(cpu, prefix, z, indexed, addr);
        uint8_t a = z80_a(cpu);
        switch (y) {
            case 0: z80_set_a(cpu, z80_add8(cpu, a, value, 0)); break;
            case 1: z80_set_a(cpu, z80_add8(cpu, a, value, (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0))); break;
            case 2: z80_set_a(cpu, z80_sub8(cpu, a, value, 0)); break;
            case 3: z80_set_a(cpu, z80_sub8(cpu, a, value, (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0))); break;
            case 4: z80_set_a(cpu, (uint8_t)(a & value)); z80_logic_flags(cpu, z80_a(cpu), FLAG_H); break;
            case 5: z80_set_a(cpu, (uint8_t)(a ^ value)); z80_logic_flags(cpu, z80_a(cpu), 0); break;
            case 6: z80_set_a(cpu, (uint8_t)(a | value)); z80_logic_flags(cpu, z80_a(cpu), 0); break;
            default: z80_cp8(cpu, a, value); break;
        }
        return indexed ? 19 : (z == 6 ? 7 : 4);
    }

    switch (z) {
        case 0:
            if (z80_condition(cpu, y)) {
                cpu->pc = z80_pop(cpu);
                return 11;
            }
            return 5;
        case 1:
            if (q == 0) {
                z80_set_rp2(cpu, prefix, p, z80_pop(cpu));
                return p == 2 && prefix != PREFIX_NONE ? 14 : 10;
            }
            switch (p) {
                case 0:
                    cpu->pc = z80_pop(cpu);
                    return 10;
                case 1: {
                    uint16_t tmp = cpu->bc;
                    cpu->bc = cpu->bc_alt;
                    cpu->bc_alt = tmp;
                    tmp = cpu->de;
                    cpu->de = cpu->de_alt;
                    cpu->de_alt = tmp;
                    tmp = cpu->hl;
                    cpu->hl = cpu->hl_alt;
                    cpu->hl_alt = tmp;
                    return 4;
                }
                case 2:
                    cpu->pc = z80_get_hl_prefixed(cpu, prefix);
                    return prefix == PREFIX_NONE ? 4 : 8;
                default:
                    cpu->sp = z80_get_hl_prefixed(cpu, prefix);
                    return prefix == PREFIX_NONE ? 6 : 10;
            }
        case 2: {
            uint16_t address = z80_next_word(cpu);
            if (z80_condition(cpu, y)) {
                cpu->pc = address;
            }
            return 10;
        }
        case 3:
            switch (y) {
                case 0:
                    cpu->pc = z80_next_word(cpu);
                    return 10;
                case 1:
                    return z80_execute_cb(cpu, z80_next_opcode(cpu), PREFIX_NONE, false, 0);
                case 2: {
                    uint8_t port = z80_next_byte(cpu);
                    cpu->write_port(cpu->context, (uint16_t)((z80_a(cpu) << 8) | port), z80_a(cpu));
                    return 11;
                }
                case 3: {
                    uint8_t port = z80_next_byte(cpu);
                    z80_set_a(cpu, cpu->read_port(cpu->context, (uint16_t)((z80_a(cpu) << 8) | port)));
                    return 11;
                }
                case 4: {
                    uint16_t address = cpu->sp;
                    uint16_t value = z80_read16(cpu, address);
                    z80_write16(cpu, address, z80_get_hl_prefixed(cpu, prefix));
                    z80_set_hl_prefixed(cpu, prefix, value);
                    return prefix == PREFIX_NONE ? 19 : 23;
                }
                case 5: {
                    uint16_t tmp = cpu->de;
                    cpu->de = z80_get_hl_prefixed(cpu, prefix);
                    z80_set_hl_prefixed(cpu, prefix, tmp);
                    return 4;
                }
                case 6:
                    cpu->iff1 = 0;
                    cpu->iff2 = 0;
                    return 4;
                default:
                    cpu->ei_delay = 2;
                    return 4;
            }
        case 4: {
            uint16_t address = z80_next_word(cpu);
            if (z80_condition(cpu, y)) {
                z80_push(cpu, cpu->pc);
                cpu->pc = address;
                return 17;
            }
            return 10;
        }
        case 5:
            if (q == 0) {
                z80_push(cpu, z80_get_rp2(cpu, prefix, p));
                return p == 2 && prefix != PREFIX_NONE ? 15 : 11;
            }
            switch (p) {
                case 0: {
                    uint16_t address = z80_next_word(cpu);
                    z80_push(cpu, cpu->pc);
                    cpu->pc = address;
                    return 17;
                }
                case 1:
                {
                    uint8_t next = z80_next_opcode(cpu);
                    if (next == 0xCB) {
                        int8_t disp = (int8_t)z80_next_byte(cpu);
                        uint8_t cb = z80_next_opcode(cpu);
                        return z80_execute_cb(cpu, cb, PREFIX_IX, true, z80_indexed_address(cpu, PREFIX_IX, disp)) + 4;
                    }
                    if (next == 0xED) {
                        return z80_execute_ed(cpu, z80_next_opcode(cpu)) + 4;
                    }
                    return z80_execute_main(cpu, next, PREFIX_IX) + 4;
                }
                case 2:
                    return z80_execute_ed(cpu, z80_next_opcode(cpu));
                default:
                {
                    uint8_t next = z80_next_opcode(cpu);
                    if (next == 0xCB) {
                        int8_t disp = (int8_t)z80_next_byte(cpu);
                        uint8_t cb = z80_next_opcode(cpu);
                        return z80_execute_cb(cpu, cb, PREFIX_IY, true, z80_indexed_address(cpu, PREFIX_IY, disp)) + 4;
                    }
                    if (next == 0xED) {
                        return z80_execute_ed(cpu, z80_next_opcode(cpu)) + 4;
                    }
                    return z80_execute_main(cpu, next, PREFIX_IY) + 4;
                }
            }
        case 6: {
            uint8_t value = z80_next_byte(cpu);
            uint8_t a = z80_a(cpu);
            switch (y) {
                case 0: z80_set_a(cpu, z80_add8(cpu, a, value, 0)); break;
                case 1: z80_set_a(cpu, z80_add8(cpu, a, value, (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0))); break;
                case 2: z80_set_a(cpu, z80_sub8(cpu, a, value, 0)); break;
                case 3: z80_set_a(cpu, z80_sub8(cpu, a, value, (uint8_t)(z80_f(cpu) & FLAG_C ? 1 : 0))); break;
                case 4: z80_set_a(cpu, (uint8_t)(a & value)); z80_logic_flags(cpu, z80_a(cpu), FLAG_H); break;
                case 5: z80_set_a(cpu, (uint8_t)(a ^ value)); z80_logic_flags(cpu, z80_a(cpu), 0); break;
                case 6: z80_set_a(cpu, (uint8_t)(a | value)); z80_logic_flags(cpu, z80_a(cpu), 0); break;
                default: z80_cp8(cpu, a, value); break;
            }
            return 7;
        }
        default:
            z80_push(cpu, cpu->pc);
            cpu->pc = (uint16_t)(y * 8);
            return 11;
    }
}

static int z80_service_interrupt(Z80 *cpu) {
    if (!cpu->pending_irq || cpu->iff1 == 0 || cpu->ei_delay > 0) {
        return 0;
    }

    cpu->pending_irq = false;
    cpu->halted = false;
    cpu->iff1 = 0;
    cpu->iff2 = 0;

    if (cpu->im == 2) {
        uint16_t vector = (uint16_t)((cpu->i << 8) | 0xFF);
        z80_push(cpu, cpu->pc);
        cpu->pc = z80_read16(cpu, vector);
        cpu->cycles += 19;
        return 19;
    }

    z80_push(cpu, cpu->pc);
    cpu->pc = 0x0038;
    cpu->cycles += 13;
    return 13;
}

void z80_init(
    Z80 *cpu,
    void *context,
    z80_read_memory_fn read_memory,
    z80_write_memory_fn write_memory,
    z80_read_port_fn read_port,
    z80_write_port_fn write_port
) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->context = context;
    cpu->read_memory = read_memory;
    cpu->write_memory = write_memory;
    cpu->read_port = read_port;
    cpu->write_port = write_port;
}

void z80_reset(Z80 *cpu) {
    cpu->af = 0xFFFF;
    cpu->bc = 0;
    cpu->de = 0;
    cpu->hl = 0;
    cpu->af_alt = 0xFFFF;
    cpu->bc_alt = 0;
    cpu->de_alt = 0;
    cpu->hl_alt = 0;
    cpu->ix = 0;
    cpu->iy = 0;
    cpu->sp = 0xFFFF;
    cpu->pc = 0x0000;
    cpu->i = 0;
    cpu->r = 0;
    cpu->iff1 = 0;
    cpu->iff2 = 0;
    cpu->im = 0;
    cpu->halted = false;
    cpu->pending_irq = false;
    cpu->ei_delay = 0;
    cpu->cycles = 0;
}

int z80_step(Z80 *cpu) {
    int irq_cycles = z80_service_interrupt(cpu);
    if (irq_cycles != 0) {
        return irq_cycles;
    }

    if (cpu->halted) {
        cpu->cycles += 4;
        if (cpu->ei_delay > 0) {
            cpu->ei_delay -= 1;
            if (cpu->ei_delay == 0) {
                cpu->iff1 = 1;
                cpu->iff2 = 1;
            }
        }
        return 4;
    }

    uint8_t opcode = z80_next_opcode(cpu);
    int cycles = z80_execute_main(cpu, opcode, PREFIX_NONE);
    cpu->cycles += (uint64_t)cycles;

    if (cpu->ei_delay > 0) {
        cpu->ei_delay -= 1;
        if (cpu->ei_delay == 0) {
            cpu->iff1 = 1;
            cpu->iff2 = 1;
        }
    }

    return cycles;
}

void z80_request_interrupt(Z80 *cpu) {
    cpu->pending_irq = true;
}
