/* Split from main.c: debugger implementation. Included by main.c. */

enum {
    APP_DEBUG_DISASSEMBLY_LINE_COUNT = 64
};

static HMENU app_create_debugger_menu(void) {
    HMENU menu_bar = CreateMenu();
    HMENU help_menu = CreatePopupMenu();

    if (menu_bar == NULL || help_menu == NULL) {
        if (help_menu != NULL) {
            DestroyMenu(help_menu);
        }
        if (menu_bar != NULL) {
            DestroyMenu(menu_bar);
        }
        return NULL;
    }

    AppendMenuA(help_menu, MF_STRING, APP_MENU_DEBUG_HELP_SHOW, "Debugger &Help\tF1");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)help_menu, "&Help");
    return menu_bar;
}

/* Appends formatted text into a caller-owned buffer while preserving a valid
   trailing NUL even when the output becomes truncated. */
static void app_append_textf(char *buffer, size_t buffer_size, size_t *used, const char *format, ...) {
    va_list args;
    int written;

    if (*used >= buffer_size) {
        return;
    }

    va_start(args, format);
    written = vsnprintf(buffer + *used, buffer_size - *used, format, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    if ((size_t)written >= buffer_size - *used) {
        *used = buffer_size - 1;
    } else {
        *used += (size_t)written;
    }
}

/* The debugger drives whichever machine is active: the classic Spectrum
   core or the Spectrum Next. These helpers hide which one it is. */
static bool app_debug_is_next(const AppState *app) {
    return app->machine_kind == APP_MACHINE_NEXT;
}

static bool app_debug_ready(const AppState *app) {
    return app_debug_is_next(app) ? app->next.initialized : app->spec.machine_ready;
}

static z80_t *app_debug_cpu(AppState *app) {
    return app_debug_is_next(app) ? &app->next.cpu : &app->spec.machine.cpu;
}

static const z80_t *app_debug_cpu_const(const AppState *app) {
    return app_debug_is_next(app) ? &app->next.cpu : &app->spec.machine.cpu;
}

static uint64_t app_debug_pins(const AppState *app) {
    return app_debug_is_next(app) ? app->next.cpu_pins : app->spec.machine.pins;
}

/* Reads one byte through the active machine's current memory map (128K
   paging or the Next MMU), so debugger views match what the CPU sees. */
static uint8_t app_debug_mem_rd(const AppState *app, uint16_t addr) {
    if (app_debug_is_next(app)) {
        return next_machine_read_memory(&app->next, addr);
    }
    return mem_rd((mem_t *)&app->spec.machine.mem, addr);
}

/* Reads a 16-bit little-endian value from the active memory map. */
static uint16_t app_debug_mem_rd16(const AppState *app, uint16_t addr) {
    return (uint16_t)(app_debug_mem_rd(app, addr) | (app_debug_mem_rd(app, (uint16_t)(addr + 1)) << 8));
}

static void app_debug_mem_wr(AppState *app, uint16_t addr, uint8_t value) {
    if (app_debug_is_next(app)) {
        next_machine_write_memory(&app->next, addr, value);
    } else {
        mem_wr(&app->spec.machine.mem, addr, value);
    }
}

static bool app_debug_at_instruction_boundary(const AppState *app) {
    const uint64_t pins = app_debug_pins(app);
    return ((pins & (Z80_M1 | Z80_MREQ | Z80_RD)) == (Z80_M1 | Z80_MREQ | Z80_RD)) &&
        z80_opdone((z80_t *)app_debug_cpu_const(app));
}

/* The cycle-stepped Z80 overlaps completion of one instruction with M1/T2 of
   the next opcode fetch. At that boundary PC has already been incremented;
   the address bus still carries the debugger-visible address of the next
   instruction. */
static uint16_t app_debug_current_pc(const AppState *app) {
    const uint64_t pins = app_debug_pins(app);
    if (app_debug_at_instruction_boundary(app)) {
        return Z80_GET_ADDR(pins);
    }
    return app_debug_cpu_const(app)->pc;
}

/* Configures the read-only disassembly editor. Margin 0 carries real Z80
   addresses; margin 1 carries independent PC and breakpoint markers. */
static void app_debug_configure_disassembly(HWND editor) {
    const COLORREF background = GetSysColor(COLOR_WINDOW);
    const COLORREF foreground = GetSysColor(COLOR_WINDOWTEXT);

    if (editor == NULL) {
        return;
    }
    SendMessageA(editor, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    SendMessageA(editor, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
    SendMessageA(editor, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Consolas");
    SendMessageA(editor, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    SendMessageA(editor, SCI_STYLESETFORE, STYLE_DEFAULT, foreground);
    SendMessageA(editor, SCI_STYLESETBACK, STYLE_DEFAULT, background);
    SendMessageA(editor, SCI_STYLECLEARALL, 0, 0);
    SendMessageA(editor, SCI_STYLESETFORE, STYLE_LINENUMBER, GetSysColor(COLOR_GRAYTEXT));
    SendMessageA(editor, SCI_STYLESETBACK, STYLE_LINENUMBER, GetSysColor(COLOR_BTNFACE));
    SendMessageA(editor, SCI_SETMARGINTYPEN, 0, SC_MARGIN_TEXT);
    SendMessageA(editor, SCI_SETMARGINWIDTHN, 0,
        SendMessageA(editor, SCI_TEXTWIDTH, STYLE_LINENUMBER, (LPARAM)" FFFF "));
    SendMessageA(editor, SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
    SendMessageA(editor, SCI_SETMARGINMASKN, 1, (1u << 0) | (1u << 1));
    SendMessageA(editor, SCI_SETMARGINWIDTHN, 1, 18);
    SendMessageA(editor, SCI_SETMARGINWIDTHN, 2, 0);
    SendMessageA(editor, SCI_MARKERDEFINE, 0, SC_MARK_SHORTARROW);
    SendMessageA(editor, SCI_MARKERSETFORE, 0, RGB(32, 96, 180));
    SendMessageA(editor, SCI_MARKERSETBACK, 0, RGB(32, 96, 180));
    SendMessageA(editor, SCI_MARKERDEFINE, 1, SC_MARK_CIRCLE);
    SendMessageA(editor, SCI_MARKERSETFORE, 1, RGB(190, 40, 40));
    SendMessageA(editor, SCI_MARKERSETBACK, 1, RGB(190, 40, 40));
    SendMessageA(editor, SCI_MARKERDEFINE, 2, SC_MARK_BACKGROUND);
    SendMessageA(editor, SCI_MARKERSETBACK, 2, RGB(218, 232, 252));
    SendMessageA(editor, SCI_SETCARETLINEVISIBLE, FALSE, 0);
    SendMessageA(editor, SCI_SETREADONLY, TRUE, 0);
}

static void app_debug_set_disassembly(
    AppState *app,
    const char *text,
    const uint16_t *addresses,
    size_t address_count
) {
    HWND editor = app->debug.debugger_text;

    if (editor == NULL) {
        return;
    }
    SendMessageA(editor, SCI_SETREADONLY, FALSE, 0);
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)(text != NULL ? text : ""));
    SendMessageA(editor, SCI_SETREADONLY, TRUE, 0);
    SendMessageA(editor, SCI_MARGINTEXTCLEARALL, 0, 0);
    SendMessageA(editor, SCI_MARKERDELETEALL, (WPARAM)-1, 0);

    for (size_t line = 0; line < address_count; ++line) {
        char address_text[8];
        snprintf(address_text, sizeof(address_text), "%04X", addresses[line]);
        SendMessageA(editor, SCI_MARGINSETTEXT, (WPARAM)line, (LPARAM)address_text);
        SendMessageA(editor, SCI_MARGINSETSTYLE, (WPARAM)line, STYLE_LINENUMBER);
        if (app_debug_ready(app) && addresses[line] == app_debug_current_pc(app)) {
            SendMessageA(editor, SCI_MARKERADD, (WPARAM)line, 0);
            SendMessageA(editor, SCI_MARKERADD, (WPARAM)line, 2);
        }
        if (app_debug_has_breakpoint(app, addresses[line])) {
            SendMessageA(editor, SCI_MARKERADD, (WPARAM)line, 1);
        }
    }
}

static void app_debug_configure_memory(HWND editor) {
    app_debug_configure_disassembly(editor);
    if (editor != NULL) {
        SendMessageA(editor, SCI_SETMARGINWIDTHN, 1, 0);
    }
}

static void app_debug_set_memory(
    HWND editor,
    const char *text,
    const uint16_t *addresses,
    const bool *has_address,
    size_t line_count
) {
    if (editor == NULL) {
        return;
    }
    SendMessageA(editor, SCI_SETREADONLY, FALSE, 0);
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)(text != NULL ? text : ""));
    SendMessageA(editor, SCI_SETREADONLY, TRUE, 0);
    SendMessageA(editor, SCI_MARGINTEXTCLEARALL, 0, 0);

    for (size_t line = 0; line < line_count; ++line) {
        if (addresses != NULL && (has_address == NULL || has_address[line])) {
            char address_text[8];
            snprintf(address_text, sizeof(address_text), "%04X", addresses[line]);
            SendMessageA(editor, SCI_MARGINSETTEXT, (WPARAM)line, (LPARAM)address_text);
            SendMessageA(editor, SCI_MARGINSETSTYLE, (WPARAM)line, STYLE_LINENUMBER);
        }
    }
}


static void app_debug_format_flags(uint8_t flags, char *out, size_t out_size) {
    snprintf(
        out,
        out_size,
        "%c%c%c%c%c%c%c%c",
        (flags & Z80_SF) ? 'S' : '-',
        (flags & Z80_ZF) ? 'Z' : '-',
        (flags & Z80_YF) ? '5' : '-',
        (flags & Z80_HF) ? 'H' : '-',
        (flags & Z80_XF) ? '3' : '-',
        (flags & Z80_VF) ? 'P' : '-',
        (flags & Z80_NF) ? 'N' : '-',
        (flags & Z80_CF) ? 'C' : '-'
    );
}

/* Returns the complete encoded length, including ignored/repeated DD/FD
   prefixes.  Prefix trains are legal on a Z80 and must not desynchronise the
   disassembly or make Step Over stop inside the current instruction. */
static uint16_t app_debug_instruction_length(const AppState *app, uint16_t addr) {
    uint32_t prefix_count = 0;
    uint16_t cursor = addr;
    uint8_t op;
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint8_t p;
    uint8_t q;
    uint16_t base_length = 1;
    bool indexed = false;

    while (prefix_count < 0x10000u) {
        op = app_debug_mem_rd(app, cursor);
        if (op != 0xDD && op != 0xFD) {
            break;
        }
        indexed = true;
        prefix_count++;
        cursor = (uint16_t)(cursor + 1);
    }
    if (prefix_count == 0x10000u) {
        /* A full 64K made entirely of index prefixes has no following opcode.
           Treat one byte as data so the debugger view can still make progress. */
        return 1;
    }

    op = app_debug_mem_rd(app, cursor);
    if (op == 0xCB) {
        base_length = indexed ? 3 : 2;
    } else if (op == 0xED) {
        const uint8_t ed = app_debug_mem_rd(app, (uint16_t)(cursor + 1));
        base_length = ((ed & 0xC7u) == 0x43u) ? 4 : 2;
        if (app_debug_is_next(app)) {
            /* Z80N operands: TEST n, NEXTREG r,A (one byte); ADD rr,nn,
               PUSH nn, NEXTREG r,n (two bytes). */
            if (ed == 0x27 || ed == 0x92) {
                base_length = 3;
            } else if (ed == 0x34 || ed == 0x35 || ed == 0x36 || ed == 0x8A || ed == 0x91) {
                base_length = 4;
            }
        }
    } else {
        x = op >> 6;
        y = (op >> 3) & 7;
        z = op & 7;
        p = y >> 1;
        q = y & 1;
        if (x == 0) {
            if (z == 0 && y >= 2) {
                base_length = 2;
            } else if (z == 1 && q == 0) {
                base_length = 3;
            } else if (z == 2 && y >= 4) {
                base_length = 3;
            } else if (z == 6) {
                base_length = 2;
            }
        } else if (x == 3) {
            if (z == 2 || z == 4 || (z == 5 && q == 1 && p == 0)) {
                base_length = 3;
            } else if (z == 3) {
                if (y == 0) {
                    base_length = 3;
                } else if (y == 1 || y == 2 || y == 3) {
                    base_length = 2;
                }
            } else if (z == 6) {
                base_length = 2;
            }
        }

        if (indexed) {
            const bool indexed_memory =
                (x == 0 && (z == 4 || z == 5 || z == 6) && y == 6) ||
                (x == 1 && op != 0x76 && (y == 6 || z == 6)) ||
                (x == 2 && z == 6);
            if (indexed_memory) {
                base_length++;
            }
        }
    }

    if (prefix_count + base_length > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)(prefix_count + base_length);
}

/* Produces a compact mnemonic for one instruction at the supplied address.
   Unsupported prefixes fall back to byte-oriented output instead of guessing. */
static void app_debug_decode_instruction_partial(
    const AppState *app,
    uint16_t addr,
    char *out,
    size_t out_size,
    uint16_t *instruction_length
) {
    char discard[64];
    static const char *reg8[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    static const char *reg16[] = {"BC", "DE", "HL", "SP"};
    static const char *reg16_push[] = {"BC", "DE", "HL", "AF"};
    static const char *cond[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
    static const char *rot[] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};
    static const char *alu[] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
    const uint8_t op = app_debug_mem_rd(app, addr);
    const uint8_t next = app_debug_mem_rd(app, (uint16_t)(addr + 1));
    const uint8_t next2 = app_debug_mem_rd(app, (uint16_t)(addr + 2));
    const uint16_t nn = (uint16_t)(next | (next2 << 8));
    const int8_t disp = (int8_t)next;
    const uint8_t x = op >> 6;
    const uint8_t y = (op >> 3) & 7;
    const uint8_t z = op & 7;
    const uint8_t p = y >> 1;
    const uint8_t q = y & 1;

    if (out == NULL || out_size == 0) {
        out = discard;
        out_size = sizeof(discard);
    }

    *instruction_length = 1;

    if (op == 0xDD || op == 0xFD) {
        const char *index_reg = (op == 0xDD) ? "IX" : "IY";
        *instruction_length = 2;
        switch (next) {
            case 0x21:
                *instruction_length = 4;
                snprintf(out, out_size, "LD %s,%04Xh", index_reg, app_debug_mem_rd16(app, (uint16_t)(addr + 2)));
                return;
            case 0x22:
                *instruction_length = 4;
                snprintf(out, out_size, "LD (%04Xh),%s", app_debug_mem_rd16(app, (uint16_t)(addr + 2)), index_reg);
                return;
            case 0x2A:
                *instruction_length = 4;
                snprintf(out, out_size, "LD %s,(%04Xh)", index_reg, app_debug_mem_rd16(app, (uint16_t)(addr + 2)));
                return;
            case 0xCB: {
                static const char *cb_rot[] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
                const uint8_t cb_op = app_debug_mem_rd(app, (uint16_t)(addr + 3));
                const int8_t cb_disp = (int8_t)app_debug_mem_rd(app, (uint16_t)(addr + 2));
                const uint8_t cb_x = cb_op >> 6;
                const uint8_t cb_bit = (cb_op >> 3) & 7;
                const uint8_t cb_reg = cb_op & 7;
                *instruction_length = 4;
                if (cb_x == 0) {
                    if (cb_reg == 6) {
                        snprintf(out, out_size, "%s (%s%+d)", cb_rot[cb_bit], index_reg, (int)cb_disp);
                    } else {
                        snprintf(out, out_size, "%s (%s%+d),%s", cb_rot[cb_bit], index_reg, (int)cb_disp, reg8[cb_reg]);
                    }
                } else if (cb_x == 1) {
                    snprintf(out, out_size, "BIT %u,(%s%+d)", cb_bit, index_reg, (int)cb_disp);
                } else {
                    const char *cb_name = (cb_x == 1) ? "BIT" : (cb_x == 2) ? "RES" : "SET";
                    if (cb_reg == 6) {
                        snprintf(out, out_size, "%s %u,(%s%+d)", cb_name, cb_bit, index_reg, (int)cb_disp);
                    } else {
                        snprintf(out, out_size, "%s %u,(%s%+d),%s", cb_name, cb_bit, index_reg, (int)cb_disp, reg8[cb_reg]);
                    }
                }
                return;
            }
            case 0x23:
                snprintf(out, out_size, "INC %s", index_reg);
                return;
            case 0x2B:
                snprintf(out, out_size, "DEC %s", index_reg);
                return;
            case 0x34:
                *instruction_length = 3;
                snprintf(out, out_size, "INC (%s%+d)", index_reg, (int)(int8_t)app_debug_mem_rd(app, (uint16_t)(addr + 2)));
                return;
            case 0x35:
                *instruction_length = 3;
                snprintf(out, out_size, "DEC (%s%+d)", index_reg, (int)(int8_t)app_debug_mem_rd(app, (uint16_t)(addr + 2)));
                return;
            case 0x36:
                *instruction_length = 4;
                snprintf(
                    out,
                    out_size,
                    "LD (%s%+d),%02Xh",
                    index_reg,
                    (int)(int8_t)app_debug_mem_rd(app, (uint16_t)(addr + 2)),
                    app_debug_mem_rd(app, (uint16_t)(addr + 3))
                );
                return;
            case 0xE1:
                snprintf(out, out_size, "POP %s", index_reg);
                return;
            case 0xE3:
                snprintf(out, out_size, "EX (SP),%s", index_reg);
                return;
            case 0xE5:
                snprintf(out, out_size, "PUSH %s", index_reg);
                return;
            case 0xE9:
                snprintf(out, out_size, "JP (%s)", index_reg);
                return;
            case 0xF9:
                snprintf(out, out_size, "LD SP,%s", index_reg);
                return;
            default: {
                /* The rest of the indexed set is the unprefixed instruction
                   with HL swapped out: (HL) becomes (IX+d) and H/L become
                   IXH/IXL. Prefixes that change nothing are shown on their
                   own so the following instruction decodes normally. */
                const uint8_t nx = next >> 6;
                const uint8_t ny = (next >> 3) & 7;
                const uint8_t nz = next & 7;
                char memory[16];
                char half_h[8];
                char half_l[8];
                const char *index8[8];
                snprintf(memory, sizeof(memory), "(%s%+d)", index_reg, (int)(int8_t)next2);
                snprintf(half_h, sizeof(half_h), "%sH", index_reg);
                snprintf(half_l, sizeof(half_l), "%sL", index_reg);
                for (int i = 0; i < 8; ++i) {
                    index8[i] = reg8[i];
                }
                index8[4] = half_h;
                index8[5] = half_l;
                index8[6] = memory;
                if (nx == 1 && next != 0x76) {
                    if (ny == 6 || nz == 6) {
                        *instruction_length = 3;
                        snprintf(out, out_size, "LD %s,%s", (ny == 6) ? memory : reg8[ny], (nz == 6) ? memory : reg8[nz]);
                        return;
                    }
                    if (ny == 4 || ny == 5 || nz == 4 || nz == 5) {
                        snprintf(out, out_size, "LD %s,%s", index8[ny], index8[nz]);
                        return;
                    }
                } else if (nx == 2) {
                    if (nz == 6) {
                        *instruction_length = 3;
                        snprintf(out, out_size, "%s%s", alu[ny], memory);
                        return;
                    }
                    if (nz == 4 || nz == 5) {
                        snprintf(out, out_size, "%s%s", alu[ny], index8[nz]);
                        return;
                    }
                } else if (nx == 0) {
                    if (nz == 1 && (ny & 1) != 0) {
                        snprintf(out, out_size, "ADD %s,%s", index_reg, (ny >> 1) == 2 ? index_reg : reg16[ny >> 1]);
                        return;
                    }
                    if ((nz == 4 || nz == 5) && (ny == 4 || ny == 5)) {
                        snprintf(out, out_size, "%s %s", (nz == 4) ? "INC" : "DEC", index8[ny]);
                        return;
                    }
                    if (nz == 6 && (ny == 4 || ny == 5)) {
                        *instruction_length = 3;
                        snprintf(out, out_size, "LD %s,%02Xh", index8[ny], next2);
                        return;
                    }
                }
                *instruction_length = 1;
                snprintf(out, out_size, "DB %02Xh", op);
                return;
            }
        }
    }

    if (op == 0xED && app_debug_is_next(app)) {
        /* ZX Spectrum Next (Z80N) extended instructions. */
        static const struct {
            uint8_t opcode;
            const char *text;
        } z80n[] = {
            {0x23, "SWAPNIB"}, {0x24, "MIRROR A"}, {0x28, "BSLA DE,B"}, {0x29, "BSRA DE,B"},
            {0x2A, "BSRL DE,B"}, {0x2B, "BSRF DE,B"}, {0x2C, "BRLC DE,B"}, {0x30, "MUL D,E"},
            {0x31, "ADD HL,A"}, {0x32, "ADD DE,A"}, {0x33, "ADD BC,A"}, {0x90, "OUTINB"},
            {0x93, "PIXELDN"}, {0x94, "PIXELAD"}, {0x95, "SETAE"}, {0x98, "JP (C)"},
            {0xA4, "LDIX"}, {0xA5, "LDWS"}, {0xAC, "LDDX"}, {0xB4, "LDIRX"},
            {0xB7, "LDPIRX"}, {0xBC, "LDDRX"}};
        const uint8_t next3 = app_debug_mem_rd(app, (uint16_t)(addr + 3));
        for (size_t i = 0; i < sizeof(z80n) / sizeof(z80n[0]); ++i) {
            if (z80n[i].opcode == next) {
                *instruction_length = 2;
                snprintf(out, out_size, "%s", z80n[i].text);
                return;
            }
        }
        switch (next) {
            case 0x27:
                *instruction_length = 3;
                snprintf(out, out_size, "TEST %02Xh", next2);
                return;
            case 0x34:
            case 0x35:
            case 0x36:
                *instruction_length = 4;
                snprintf(out, out_size, "ADD %s,%04Xh", next == 0x34 ? "HL" : (next == 0x35 ? "DE" : "BC"),
                    (unsigned)(next2 | (next3 << 8)));
                return;
            case 0x8A:
                /* PUSH nn stores its operand big-endian. */
                *instruction_length = 4;
                snprintf(out, out_size, "PUSH %04Xh", (unsigned)((next2 << 8) | next3));
                return;
            case 0x91:
                *instruction_length = 4;
                snprintf(out, out_size, "NEXTREG %02Xh,%02Xh", next2, next3);
                return;
            case 0x92:
                *instruction_length = 3;
                snprintf(out, out_size, "NEXTREG %02Xh,A", next2);
                return;
            default:
                break;
        }
    }

    if (op == 0xED) {
        static const uint8_t im_mode[] = {0, 0, 1, 2, 0, 0, 1, 2};
        static const char *block_ops[4][4] = {
            {"LDI",  "CPI",  "INI",  "OUTI"},
            {"LDD",  "CPD",  "IND",  "OUTD"},
            {"LDIR", "CPIR", "INIR", "OTIR"},
            {"LDDR", "CPDR", "INDR", "OTDR"}
        };
        const uint8_t ed_x = next >> 6;
        const uint8_t ed_y = (next >> 3) & 7;
        const uint8_t ed_z = next & 7;
        const uint8_t ed_p = ed_y >> 1;
        const uint8_t ed_q = ed_y & 1;
        *instruction_length = 2;
        if (ed_x == 1) {
            switch (ed_z) {
                case 0:
                    if (ed_y == 6) snprintf(out, out_size, "IN (C)");
                    else snprintf(out, out_size, "IN %s,(C)", reg8[ed_y]);
                    return;
                case 1:
                    if (ed_y == 6) snprintf(out, out_size, "OUT (C),0");
                    else snprintf(out, out_size, "OUT (C),%s", reg8[ed_y]);
                    return;
                case 2:
                    snprintf(out, out_size, "%s HL,%s", ed_q ? "ADC" : "SBC", reg16[ed_p]);
                    return;
                case 3:
                    *instruction_length = 4;
                    if (ed_q) snprintf(out, out_size, "LD %s,(%04Xh)", reg16[ed_p], app_debug_mem_rd16(app, (uint16_t)(addr + 2)));
                    else snprintf(out, out_size, "LD (%04Xh),%s", app_debug_mem_rd16(app, (uint16_t)(addr + 2)), reg16[ed_p]);
                    return;
                case 4:
                    snprintf(out, out_size, "NEG");
                    return;
                case 5:
                    snprintf(out, out_size, (next == 0x4D) ? "RETI" : "RETN");
                    return;
                case 6:
                    snprintf(out, out_size, "IM %u", (unsigned)im_mode[ed_y]);
                    return;
                case 7:
                    switch (ed_y) {
                        case 0: snprintf(out, out_size, "LD I,A"); return;
                        case 1: snprintf(out, out_size, "LD R,A"); return;
                        case 2: snprintf(out, out_size, "LD A,I"); return;
                        case 3: snprintf(out, out_size, "LD A,R"); return;
                        case 4: snprintf(out, out_size, "RRD"); return;
                        case 5: snprintf(out, out_size, "RLD"); return;
                        default: break;
                    }
                    break;
            }
        } else if (ed_x == 2 && ed_y >= 4 && ed_z <= 3) {
            snprintf(out, out_size, "%s", block_ops[ed_y - 4][ed_z]);
            return;
        }
        snprintf(out, out_size, "DB EDh,%02Xh", next);
        return;
    }

    if (op == 0xCB) {
        static const char *cb_rot[] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
        const uint8_t cb_x = next >> 6;
        const uint8_t cb_y = (next >> 3) & 7;
        const uint8_t cb_z = next & 7;
        *instruction_length = 2;
        if (cb_x == 0) {
            snprintf(out, out_size, "%s %s", cb_rot[cb_y], reg8[cb_z]);
        } else {
            snprintf(out, out_size, "%s %u,%s", cb_x == 1 ? "BIT" : cb_x == 2 ? "RES" : "SET", cb_y, reg8[cb_z]);
        }
        return;
    }

    switch (x) {
        case 0:
            switch (z) {
                case 0:
                    if (y == 0) {
                        snprintf(out, out_size, "NOP");
                    } else if (y == 1) {
                        snprintf(out, out_size, "EX AF,AF'");
                    } else if (y == 2) {
                        *instruction_length = 2;
                        snprintf(out, out_size, "DJNZ %04Xh", (uint16_t)(addr + 2 + disp));
                    } else if (y == 3) {
                        *instruction_length = 2;
                        snprintf(out, out_size, "JR %04Xh", (uint16_t)(addr + 2 + disp));
                    } else {
                        *instruction_length = 2;
                        snprintf(out, out_size, "JR %s,%04Xh", cond[y - 4], (uint16_t)(addr + 2 + disp));
                    }
                    return;
                case 1:
                    if (q == 0) {
                        *instruction_length = 3;
                        snprintf(out, out_size, "LD %s,%04Xh", reg16[p], nn);
                    } else {
                        snprintf(out, out_size, "ADD HL,%s", reg16[p]);
                    }
                    return;
                case 2:
                    *instruction_length = (y >= 4) ? 3 : 1;
                    switch (y) {
                        case 0: snprintf(out, out_size, "LD (BC),A"); return;
                        case 1: snprintf(out, out_size, "LD A,(BC)"); return;
                        case 2: snprintf(out, out_size, "LD (DE),A"); return;
                        case 3: snprintf(out, out_size, "LD A,(DE)"); return;
                        case 4: snprintf(out, out_size, "LD (%04Xh),HL", nn); return;
                        case 5: snprintf(out, out_size, "LD HL,(%04Xh)", nn); return;
                        case 6: snprintf(out, out_size, "LD (%04Xh),A", nn); return;
                        case 7: snprintf(out, out_size, "LD A,(%04Xh)", nn); return;
                    }
                    break;
                case 3:
                    snprintf(out, out_size, "%s %s", (q == 0) ? "INC" : "DEC", reg16[p]);
                    return;
                case 4:
                    snprintf(out, out_size, "INC %s", reg8[y]);
                    return;
                case 5:
                    snprintf(out, out_size, "DEC %s", reg8[y]);
                    return;
                case 6:
                    *instruction_length = 2;
                    snprintf(out, out_size, "LD %s,%02Xh", reg8[y], next);
                    return;
                case 7:
                    snprintf(out, out_size, "%s", rot[y]);
                    return;
            }
            break;
        case 1:
            if (op == 0x76) {
                snprintf(out, out_size, "HALT");
            } else {
                snprintf(out, out_size, "LD %s,%s", reg8[y], reg8[z]);
            }
            return;
        case 2:
            snprintf(out, out_size, "%s%s", alu[y], reg8[z]);
            return;
        case 3:
            switch (z) {
                case 0:
                    snprintf(out, out_size, "RET %s", cond[y]);
                    return;
                case 1:
                    if (q == 0) {
                        snprintf(out, out_size, "POP %s", reg16_push[p]);
                    } else {
                        switch (p) {
                            case 0: snprintf(out, out_size, "RET"); return;
                            case 1: snprintf(out, out_size, "EXX"); return;
                            case 2: snprintf(out, out_size, "JP (HL)"); return;
                            case 3: snprintf(out, out_size, "LD SP,HL"); return;
                        }
                    }
                    return;
                case 2:
                    *instruction_length = 3;
                    snprintf(out, out_size, "JP %s,%04Xh", cond[y], nn);
                    return;
                case 3:
                    switch (y) {
                        case 0:
                            *instruction_length = 3;
                            snprintf(out, out_size, "JP %04Xh", nn);
                            return;
                        case 1:
                            *instruction_length = 2;
                            snprintf(out, out_size, "DB CBh,%02Xh", next);
                            return;
                        case 2:
                            *instruction_length = 2;
                            snprintf(out, out_size, "OUT (%02Xh),A", next);
                            return;
                        case 3:
                            *instruction_length = 2;
                            snprintf(out, out_size, "IN A,(%02Xh)", next);
                            return;
                        case 4:
                            snprintf(out, out_size, "EX (SP),HL");
                            return;
                        case 5:
                            snprintf(out, out_size, "EX DE,HL");
                            return;
                        case 6:
                            snprintf(out, out_size, "DI");
                            return;
                        case 7:
                            snprintf(out, out_size, "EI");
                            return;
                    }
                    break;
                case 4:
                    *instruction_length = 3;
                    snprintf(out, out_size, "CALL %s,%04Xh", cond[y], nn);
                    return;
                case 5:
                    if (q == 0) {
                        snprintf(out, out_size, "PUSH %s", reg16_push[p]);
                    } else if (p == 0) {
                        *instruction_length = 3;
                        snprintf(out, out_size, "CALL %04Xh", nn);
                    } else {
                        snprintf(out, out_size, "DB %02Xh", op);
                    }
                    return;
                case 6:
                    *instruction_length = 2;
                    snprintf(out, out_size, "%s%02Xh", alu[y], next);
                    return;
                case 7:
                    snprintf(out, out_size, "RST %02Xh", y * 8);
                    return;
            }
            break;
    }

    snprintf(out, out_size, "DB %02Xh", op);
}

static void app_debug_decode_instruction(
    const AppState *app,
    uint16_t addr,
    char *out,
    size_t out_size,
    uint16_t *instruction_length
) {
    uint16_t partial_length = 1;
    app_debug_decode_instruction_partial(app, addr, out, out_size, &partial_length);
    *instruction_length = app_debug_instruction_length(app, addr);
    /* A DD/FD prefix that changes nothing is listed on its own line. */
    if (partial_length == 1 && *instruction_length > 1) {
        const uint8_t op = app_debug_mem_rd(app, addr);
        if (op == 0xDD || op == 0xFD) {
            *instruction_length = 1;
        }
    }
}

/* Refreshes the debugger button captions and enabled state so the current
   paused/running state remains visible from the tool window chrome. */
static void app_debug_update_controls(AppState *app) {
    bool has_selected_point = false;
    const bool ready = app_debug_ready(app);
    const BOOL machine_ready = ready ? TRUE : FALSE;
    const BOOL paused_ready = (app->debug.paused && ready) ? TRUE : FALSE;
    HWND controls[] = {
        app->debug.debugger_refresh_button, app->debug.debugger_address_edit, app->debug.debugger_go_button,
        app->debug.debugger_run_to_button, app->debug.debugger_run_at_button,
        app->debug.debugger_breakpoint_toggle_button, app->debug.debugger_watchpoint_toggle_button,
        app->debug.debugger_view_pc_button, app->debug.debugger_view_sp_button,
        app->debug.debugger_page_up_button, app->debug.debugger_page_down_button,
        app->debug.debugger_command_edit, app->debug.debugger_command_button};

    if (app->debug.debugger_hwnd == NULL) {
        return;
    }
    if (app->debug.debugger_points_list != NULL) {
        has_selected_point = SendMessageA(app->debug.debugger_points_list, LB_GETCURSEL, 0, 0) != LB_ERR;
    }
    SetWindowTextA(app->debug.debugger_pause_button, app->debug.paused ? "Run" : "Pause");
    EnableWindow(app->debug.debugger_pause_button, machine_ready);
    EnableWindow(app->debug.debugger_step_button, paused_ready);
    EnableWindow(app->debug.debugger_step_over_button, paused_ready);
    EnableWindow(app->debug.debugger_step_out_button, paused_ready);
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        if (controls[i] != NULL) {
            EnableWindow(controls[i], machine_ready);
        }
    }
    if (app->debug.debugger_remove_selected_button != NULL) {
        EnableWindow(app->debug.debugger_remove_selected_button, has_selected_point ? TRUE : FALSE);
    }
    if (app->debug.debugger_sync_checkbox != NULL) {
        SendMessageA(
            app->debug.debugger_sync_checkbox,
            BM_SETCHECK,
            app->debug.debugger_view_sync_pc ? BST_CHECKED : BST_UNCHECKED,
            0
        );
    }
}

/* Keeps the debugger address widgets synchronized with the current base view
   address and optional follow-PC mode. */
static void app_debug_sync_view_controls(AppState *app) {
    char address_text[16];

    if (app->debug.debugger_address_edit != NULL) {
        snprintf(address_text, sizeof(address_text), "%04Xh", app->debug.debugger_view_address);
        SetWindowTextA(app->debug.debugger_address_edit, address_text);
    }
    app_debug_update_controls(app);
}

/* ---------------------------------------------------------------------------
   Breakpoint conditions

   A small C-like expression language evaluated when a breakpoint matches:
     registers   A F B C D E H L I R AF BC DE HL IX IY SP PC IXH IXL IYH IYL
                 AF' BC' DE' HL'
     flags       ZF CF SF HF PF NF (0 or 1)
     access      ADDR, VALUE (address and data byte of the access that hit)
     functions   PEEK(a), DPEEK(a), NR(r) (Next register, Next only)
     operators   || && | ^ & == = != < > <= >= << >> + - * / % ! ~ - ( )
     numbers     the assembler syntax: 123, $7F, 0x7F, 7Fh, %1010
   --------------------------------------------------------------------------- */

/* Debugger numbers default to hexadecimal ("8000", "FE"); the assembler's
   explicit forms ($FE, 0xFE, 0FEh, %1010) also work and "#123" is decimal. */
static bool app_debug_parse_number(const char *text, int *value) {
    const size_t length = strlen(text);
    char *end = NULL;
    long parsed;

    if (length == 0) {
        return false;
    }
    if (text[0] == '#') {
        if (text[1] == '\0') {
            return false;
        }
        parsed = strtol(text + 1, &end, 10);
    } else if (text[0] == '$' || text[0] == '%' || (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ||
               text[length - 1] == 'h' || text[length - 1] == 'H') {
        return app_parse_number(text, value);
    } else {
        parsed = strtol(text, &end, 16);
    }
    if (end == NULL || *end != '\0') {
        return false;
    }
    *value = (int)parsed;
    return true;
}

typedef struct AppDebugEval {
    const AppState *app;
    const char *cursor;
    uint16_t pc;
    uint16_t access_address;
    uint8_t access_value;
    bool failed;
    char error[96];
} AppDebugEval;

static void app_debug_eval_fail(AppDebugEval *eval, const char *message) {
    if (!eval->failed) {
        eval->failed = true;
        snprintf(eval->error, sizeof(eval->error), "%s", message);
    }
}

static void app_debug_eval_skip_space(AppDebugEval *eval) {
    while (*eval->cursor == ' ' || *eval->cursor == '\t') {
        eval->cursor++;
    }
}

static bool app_debug_eval_accept(AppDebugEval *eval, const char *token) {
    const size_t length = strlen(token);
    app_debug_eval_skip_space(eval);
    if (strncmp(eval->cursor, token, length) == 0) {
        eval->cursor += length;
        return true;
    }
    return false;
}

static long app_debug_eval_expression(AppDebugEval *eval);

/* Looks up a register, flag or access variable by name. */
static bool app_debug_eval_variable(AppDebugEval *eval, const char *name, long *out) {
    const z80_t *cpu = app_debug_cpu_const(eval->app);
    static const struct {
        const char *name;
        int kind;
    } flags[] = {{"ZF", Z80_ZF}, {"CF", Z80_CF}, {"SF", Z80_SF}, {"HF", Z80_HF}, {"PF", Z80_VF}, {"NF", Z80_NF}};
    const struct {
        const char *name;
        long value;
    } registers[] = {
        {"A", cpu->a}, {"F", cpu->f}, {"B", cpu->b}, {"C", cpu->c}, {"D", cpu->d}, {"E", cpu->e},
        {"H", cpu->h}, {"L", cpu->l}, {"I", cpu->i}, {"R", cpu->r},
        {"AF", cpu->af}, {"BC", cpu->bc}, {"DE", cpu->de}, {"HL", cpu->hl},
        {"IX", cpu->ix}, {"IY", cpu->iy}, {"SP", cpu->sp}, {"PC", eval->pc},
        {"IXH", cpu->ix >> 8}, {"IXL", cpu->ix & 0xFF}, {"IYH", cpu->iy >> 8}, {"IYL", cpu->iy & 0xFF},
        {"AF'", cpu->af2}, {"BC'", cpu->bc2}, {"DE'", cpu->de2}, {"HL'", cpu->hl2},
        {"ADDR", eval->access_address}, {"VALUE", eval->access_value}};

    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        if (_stricmp(name, registers[i].name) == 0) {
            *out = registers[i].value;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        if (_stricmp(name, flags[i].name) == 0) {
            *out = (cpu->f & flags[i].kind) != 0 ? 1 : 0;
            return true;
        }
    }
    return false;
}

static long app_debug_eval_primary(AppDebugEval *eval) {
    char token[32];
    size_t length = 0;
    long value = 0;

    app_debug_eval_skip_space(eval);
    if (app_debug_eval_accept(eval, "(")) {
        value = app_debug_eval_expression(eval);
        if (!app_debug_eval_accept(eval, ")")) {
            app_debug_eval_fail(eval, "Missing ')' in condition.");
        }
        return value;
    }
    /* A token is a number (7F, $7F, %1010, 0x7F, 7Fh, #127) or a name. */
    if (*eval->cursor == '$' || *eval->cursor == '%' || *eval->cursor == '#') {
        token[length++] = *eval->cursor++;
    }
    while (length + 1 < sizeof(token) &&
           (isalnum((unsigned char)*eval->cursor) || *eval->cursor == '_' ||
            (*eval->cursor == '\'' && length > 0))) {
        token[length++] = *eval->cursor++;
    }
    token[length] = '\0';
    if (length == 0) {
        app_debug_eval_fail(eval, "Expected a value in condition.");
        return 0;
    }
    if (isdigit((unsigned char)token[0]) || token[0] == '$' || token[0] == '%' || token[0] == '#') {
        int number;
        if (!app_debug_parse_number(token, &number)) {
            app_debug_eval_fail(eval, "Invalid number in condition.");
            return 0;
        }
        return number;
    }
    if (_stricmp(token, "PEEK") == 0 || _stricmp(token, "DPEEK") == 0 || _stricmp(token, "NR") == 0) {
        long argument;
        if (!app_debug_eval_accept(eval, "(")) {
            app_debug_eval_fail(eval, "Expected '(' after function name.");
            return 0;
        }
        argument = app_debug_eval_expression(eval);
        if (!app_debug_eval_accept(eval, ")")) {
            app_debug_eval_fail(eval, "Missing ')' in condition.");
            return 0;
        }
        if (_stricmp(token, "PEEK") == 0) {
            return app_debug_mem_rd(eval->app, (uint16_t)argument);
        }
        if (_stricmp(token, "DPEEK") == 0) {
            return app_debug_mem_rd16(eval->app, (uint16_t)argument);
        }
        return app_debug_is_next(eval->app)
            ? next_machine_read_nextreg(&eval->app->next, (uint8_t)argument)
            : 0;
    }
    if (!app_debug_eval_variable(eval, token, &value)) {
        char message[64];
        int number;
        /* Hex that starts with a letter (FE, C000) when it is not a register. */
        if (app_debug_parse_number(token, &number)) {
            return number;
        }
        snprintf(message, sizeof(message), "Unknown name in condition: %s", token);
        app_debug_eval_fail(eval, message);
    }
    return value;
}

static long app_debug_eval_unary(AppDebugEval *eval) {
    app_debug_eval_skip_space(eval);
    if (app_debug_eval_accept(eval, "!")) {
        return !app_debug_eval_unary(eval);
    }
    if (app_debug_eval_accept(eval, "~")) {
        return ~app_debug_eval_unary(eval);
    }
    if (app_debug_eval_accept(eval, "-")) {
        return -app_debug_eval_unary(eval);
    }
    return app_debug_eval_primary(eval);
}

/* Binary operators by precedence level, lowest first. Longer spellings are
   listed before their prefixes so "<=" is not read as "<". */
static long app_debug_eval_binary(AppDebugEval *eval, int level) {
    static const char *levels[][4] = {
        {"||", NULL}, {"&&", NULL}, {"|", NULL}, {"^", NULL}, {"&", NULL},
        {"==", "!=", "=", NULL}, {"<=", ">=", "<", ">"}, {"<<", ">>", NULL},
        {"+", "-", NULL}, {"*", "/", "%", NULL}};
    const int level_count = (int)(sizeof(levels) / sizeof(levels[0]));
    long left;

    if (level >= level_count) {
        return app_debug_eval_unary(eval);
    }
    left = app_debug_eval_binary(eval, level + 1);
    for (;;) {
        const char *matched = NULL;
        app_debug_eval_skip_space(eval);
        for (int i = 0; i < 4 && levels[level][i] != NULL; ++i) {
            const char *op = levels[level][i];
            const size_t length = strlen(op);
            /* Keep "|" and "&" from swallowing "||" and "&&". */
            if (strncmp(eval->cursor, op, length) == 0 &&
                !(length == 1 && (op[0] == '|' || op[0] == '&') && eval->cursor[1] == op[0]) &&
                !(length == 1 && (op[0] == '<' || op[0] == '>') && eval->cursor[1] == op[0])) {
                matched = op;
                break;
            }
        }
        if (matched == NULL) {
            return left;
        }
        eval->cursor += strlen(matched);
        {
            const long right = app_debug_eval_binary(eval, level + 1);
            if (strcmp(matched, "||") == 0) left = left || right;
            else if (strcmp(matched, "&&") == 0) left = left && right;
            else if (strcmp(matched, "|") == 0) left = left | right;
            else if (strcmp(matched, "^") == 0) left = left ^ right;
            else if (strcmp(matched, "&") == 0) left = left & right;
            else if (strcmp(matched, "==") == 0 || strcmp(matched, "=") == 0) left = left == right;
            else if (strcmp(matched, "!=") == 0) left = left != right;
            else if (strcmp(matched, "<=") == 0) left = left <= right;
            else if (strcmp(matched, ">=") == 0) left = left >= right;
            else if (strcmp(matched, "<") == 0) left = left < right;
            else if (strcmp(matched, ">") == 0) left = left > right;
            else if (strcmp(matched, "<<") == 0) left = (right >= 0 && right < 31) ? left << right : 0;
            else if (strcmp(matched, ">>") == 0) left = (right >= 0 && right < 31) ? left >> right : 0;
            else if (strcmp(matched, "+") == 0) left = left + right;
            else if (strcmp(matched, "-") == 0) left = left - right;
            else if (strcmp(matched, "*") == 0) left = left * right;
            else if (right == 0) {
                app_debug_eval_fail(eval, "Division by zero in condition.");
                left = 0;
            } else if (strcmp(matched, "/") == 0) left = left / right;
            else left = left % right;
        }
    }
}

static long app_debug_eval_expression(AppDebugEval *eval) {
    return app_debug_eval_binary(eval, 0);
}

/* Evaluates a condition string. Returns false (with error set) on syntax
   errors; *result receives the value. */
static bool app_debug_evaluate(
    const AppState *app,
    const char *text,
    uint16_t pc,
    uint16_t access_address,
    uint8_t access_value,
    long *result,
    char *error_buffer,
    size_t error_buffer_size
) {
    AppDebugEval eval;

    memset(&eval, 0, sizeof(eval));
    eval.app = app;
    eval.cursor = text;
    eval.pc = pc;
    eval.access_address = access_address;
    eval.access_value = access_value;
    *result = app_debug_eval_expression(&eval);
    app_debug_eval_skip_space(&eval);
    if (!eval.failed && *eval.cursor != '\0') {
        app_debug_eval_fail(&eval, "Unexpected text in condition.");
    }
    if (eval.failed && error_buffer != NULL && error_buffer_size > 0) {
        snprintf(error_buffer, error_buffer_size, "%s", eval.error);
    }
    return !eval.failed;
}

/* ---------------------------------------------------------------------------
   Breakpoints
   --------------------------------------------------------------------------- */

static const char *app_debug_point_keyword(AppDebugPointKind kind) {
    static const char *keywords[] = {"bp", "br", "bw", "bi", "bo"};
    return keywords[kind];
}

static const char *app_debug_point_label(AppDebugPointKind kind) {
    static const char *labels[] = {"Exec ", "Read ", "Write", "In   ", "Out  "};
    return labels[kind];
}

/* Port breakpoints given as 00-FF match the low byte of the port, so "bo FE"
   catches every ULA write; wider values match the full 16-bit port. */
static bool app_debug_point_covers(const AppDebugPoint *point, uint16_t address) {
    if ((point->kind == APP_DEBUG_POINT_IN || point->kind == APP_DEBUG_POINT_OUT) && point->end <= 0xFF) {
        address &= 0xFF;
    }
    return address >= point->start && address <= point->end;
}

/* Formats a point as the command that recreates it; used for the list, the
   saved settings and hit messages. */
static void app_debug_format_point(const AppDebugPoint *point, char *out, size_t out_size) {
    size_t used = 0;
    const bool port = point->kind == APP_DEBUG_POINT_IN || point->kind == APP_DEBUG_POINT_OUT;

    out[0] = '\0';
    app_append_textf(out, out_size, &used, "%s ", app_debug_point_keyword(point->kind));
    app_append_textf(out, out_size, &used, port && point->end <= 0xFF ? "%02X" : "%04X", point->start);
    if (point->end != point->start) {
        app_append_textf(out, out_size, &used, port && point->end <= 0xFF ? "-%02X" : "-%04X", point->end);
    }
    if (point->value >= 0) {
        app_append_textf(out, out_size, &used, " =%02X", point->value);
    }
    if (point->condition[0] != '\0') {
        app_append_textf(out, out_size, &used, " if %s", point->condition);
    }
    if (point->hit_target > 1) {
        app_append_textf(out, out_size, &used, " hits %u", (unsigned)point->hit_target);
    }
    if (!point->enabled) {
        app_append_textf(out, out_size, &used, " off");
    }
}

static bool app_debug_reserve_points(AppState *app, size_t required_count) {
    AppDebugPoint *points;
    size_t capacity;

    if (required_count <= app->debug.point_capacity) {
        return true;
    }
    capacity = app->debug.point_capacity > 0 ? app->debug.point_capacity : 16;
    while (capacity < required_count) {
        capacity *= 2;
    }
    points = (AppDebugPoint *)realloc(app->debug.points, capacity * sizeof(points[0]));
    if (points == NULL) {
        return false;
    }
    app->debug.points = points;
    app->debug.point_capacity = capacity;
    return true;
}

static bool app_debug_add_point(AppState *app, const AppDebugPoint *point) {
    if (!app_debug_reserve_points(app, app->debug.point_count + 1)) {
        return false;
    }
    app->debug.points[app->debug.point_count++] = *point;
    return true;
}

static void app_debug_remove_point(AppState *app, size_t index) {
    if (index >= app->debug.point_count) {
        return;
    }
    memmove(&app->debug.points[index], &app->debug.points[index + 1],
        (app->debug.point_count - index - 1) * sizeof(app->debug.points[0]));
    app->debug.point_count--;
}

/* True when an enabled plain execution breakpoint covers the address; used
   for the disassembly margin markers. */
static bool app_debug_has_breakpoint(const AppState *app, uint16_t address) {
    for (size_t i = 0; i < app->debug.point_count; ++i) {
        const AppDebugPoint *point = &app->debug.points[i];
        if (point->enabled && point->kind == APP_DEBUG_POINT_EXEC && app_debug_point_covers(point, address)) {
            return true;
        }
    }
    return false;
}

/* Toggles a simple single-address point of the given kind (F9 / Ctrl+W). */
static bool app_debug_toggle_simple_point(AppState *app, AppDebugPointKind kind, uint16_t address) {
    AppDebugPoint point;

    for (size_t i = 0; i < app->debug.point_count; ++i) {
        const AppDebugPoint *existing = &app->debug.points[i];
        if (existing->kind == kind && existing->start == address && existing->end == address) {
            app_debug_remove_point(app, i);
            return true;
        }
    }
    memset(&point, 0, sizeof(point));
    point.kind = kind;
    point.enabled = true;
    point.start = address;
    point.end = address;
    point.value = -1;
    point.hit_target = 1;
    return app_debug_add_point(app, &point);
}

/* Counts a hit when the point's condition holds; returns true once the hit
   count has reached the point's target (then on every later hit). */
static bool app_debug_point_triggers(
    AppState *app,
    AppDebugPoint *point,
    uint16_t pc,
    uint16_t address,
    uint8_t value
) {
    if (point->value >= 0 && point->kind != APP_DEBUG_POINT_EXEC && point->value != value) {
        return false;
    }
    if (point->condition[0] != '\0') {
        long result = 0;
        /* A condition that no longer parses breaks so the user can fix it. */
        if (app_debug_evaluate(app, point->condition, pc, address, value, &result, NULL, 0) && result == 0) {
            return false;
        }
    }
    point->hits++;
    return point->hits >= point->hit_target;
}

/* Returns the index of the first enabled point of a kind that triggers for an
   access, or -1. */
static int app_debug_match_point(
    AppState *app,
    AppDebugPointKind kind,
    uint16_t pc,
    uint16_t address,
    uint8_t value
) {
    for (size_t i = 0; i < app->debug.point_count; ++i) {
        AppDebugPoint *point = &app->debug.points[i];
        if (point->enabled && point->kind == kind && app_debug_point_covers(point, address) &&
            app_debug_point_triggers(app, point, pc, address, value)) {
            return (int)i;
        }
    }
    return -1;
}

static bool app_debug_has_enabled_points(const AppState *app) {
    for (size_t i = 0; i < app->debug.point_count; ++i) {
        if (app->debug.points[i].enabled) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
   Persistence: points are saved as their command text in the settings file.
   --------------------------------------------------------------------------- */

static bool app_debug_parse_point(const char *text, AppDebugPoint *point, char *error_buffer, size_t error_buffer_size);

static void app_debug_save_points(const AppState *app) {
    char path[MAX_PATH];
    char key[32];
    char value[32];

    if (!app_settings_path(path, sizeof(path))) {
        return;
    }
    /* Clear the old section, then write the current list. */
    WritePrivateProfileStringA("debugger_points", NULL, NULL, path);
    snprintf(value, sizeof(value), "%u", (unsigned)app->debug.point_count);
    WritePrivateProfileStringA("debugger_points", "count", value, path);
    for (size_t i = 0; i < app->debug.point_count; ++i) {
        char command[192];
        app_debug_format_point(&app->debug.points[i], command, sizeof(command));
        snprintf(key, sizeof(key), "point%u", (unsigned)(i + 1));
        WritePrivateProfileStringA("debugger_points", key, command, path);
    }
}

static void app_debug_load_points(AppState *app) {
    char path[MAX_PATH];
    char key[32];
    unsigned count;

    if (!app_settings_path(path, sizeof(path))) {
        return;
    }
    count = GetPrivateProfileIntA("debugger_points", "count", 0, path);
    for (unsigned i = 0; i < count && i < 1024; ++i) {
        char command[192];
        AppDebugPoint point;
        snprintf(key, sizeof(key), "point%u", i + 1);
        GetPrivateProfileStringA("debugger_points", key, "", command, (DWORD)sizeof(command), path);
        if (command[0] != '\0' && app_debug_parse_point(command, &point, NULL, 0)) {
            app_debug_add_point(app, &point);
        }
    }
}

/* Rebuilds the points list box. Each line shows its number, an enable mark,
   the point and how often it has been hit. */
static void app_debug_refresh_points_list(AppState *app) {
    int selection;

    if (app == NULL || app->debug.debugger_points_list == NULL) {
        return;
    }
    selection = (int)SendMessageA(app->debug.debugger_points_list, LB_GETCURSEL, 0, 0);
    SendMessageA(app->debug.debugger_points_list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < app->debug.point_count; ++i) {
        const AppDebugPoint *point = &app->debug.points[i];
        char command[192];
        char item[256];
        AppDebugPoint shown = *point;

        shown.enabled = true;
        app_debug_format_point(&shown, command, sizeof(command));
        /* Drop the keyword; the label column already names the kind. */
        snprintf(item, sizeof(item), "%2u %s %s %s  [%u]",
            (unsigned)(i + 1), point->enabled ? "+" : "-", app_debug_point_label(point->kind),
            command + strlen(app_debug_point_keyword(point->kind)) + 1, (unsigned)point->hits);
        SendMessageA(app->debug.debugger_points_list, LB_ADDSTRING, 0, (LPARAM)item);
    }
    if (selection != LB_ERR && app->debug.point_count > 0) {
        if ((size_t)selection >= app->debug.point_count) {
            selection = (int)app->debug.point_count - 1;
        }
        SendMessageA(app->debug.debugger_points_list, LB_SETCURSEL, (WPARAM)selection, 0);
    }
}

static int app_debug_selected_point(const AppState *app) {
    int selection;

    if (app->debug.debugger_points_list == NULL) {
        return -1;
    }
    selection = (int)SendMessageA(app->debug.debugger_points_list, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR || (size_t)selection >= app->debug.point_count) {
        return -1;
    }
    return selection;
}

static void app_debug_free_storage(AppState *app) {
    if (app == NULL) {
        return;
    }
    free(app->debug.points);
    app->debug.points = NULL;
    app->debug.point_count = 0;
    app->debug.point_capacity = 0;
}

/* Updates the debugger's disassembly and memory base address and reflects the
   new state back into the tool-window controls. */
static void app_debug_set_view_address(AppState *app, uint16_t address, bool sync_to_pc) {
    app->debug.debugger_view_address = address;
    app->debug.debugger_view_sync_pc = sync_to_pc;
    app->debug.debugger_view_initialized = true;
    app_debug_sync_view_controls(app);
}

/* Parses the debugger address field using the assembler's number syntax so the
   value can drive either navigation or breakpoint editing. */
static bool app_debug_parse_address_input(AppState *app, uint16_t *out_address, char *error_buffer, size_t error_buffer_size) {
    char text[64];
    int value;

    if (app->debug.debugger_address_edit == NULL) {
        snprintf(error_buffer, error_buffer_size, "Debugger address control is not ready.");
        return false;
    }

    GetWindowTextA(app->debug.debugger_address_edit, text, (int)sizeof(text));
    if (!app_debug_parse_number(text, &value) || value < 0 || value > 0xFFFF) {
        snprintf(error_buffer, error_buffer_size, "Invalid debugger address: %s", text);
        return false;
    }

    *out_address = (uint16_t)value;
    return true;
}

/* Parses the debugger address field using the assembler's number syntax so the
   view can jump to decimal, hex, or binary addresses on demand. */
static bool app_debug_apply_address_input(AppState *app, char *error_buffer, size_t error_buffer_size) {
    uint16_t address;
    if (!app_debug_parse_address_input(app, &address, error_buffer, error_buffer_size)) {
        return false;
    }
    app_debug_set_view_address(app, address, false);
    return true;
}

static bool app_debug_is_step_over_candidate(const AppState *app, uint16_t address) {
    uint32_t prefix_count = 0;
    uint8_t op = app_debug_mem_rd(app, address);

    /* DD/FD are ignored for CALL and RST, including repeated/mixed prefix
       trains.  They are nevertheless part of the same Z80 instruction. */
    while (prefix_count < 0x10000u && (op == 0xDD || op == 0xFD)) {
        prefix_count++;
        address = (uint16_t)(address + 1);
        op = app_debug_mem_rd(app, address);
    }
    if (prefix_count == 0x10000u || op == 0xCB) {
        return false;
    }
    if (op == 0xED) {
        /* Block repeats (LDIR, OTIR, LDIRX...) loop on themselves; stepping
           over them runs the whole block. */
        const uint8_t ed = app_debug_mem_rd(app, (uint16_t)(address + 1));
        return (ed >= 0xB0 && ed <= 0xBB && (ed & 0x04) == 0) ||
               (app_debug_is_next(app) && (ed == 0xB4 || ed == 0xB7 || ed == 0xBC));
    }

    switch (op) {
        case 0xC4:
        case 0xCC:
        case 0xCD:
        case 0xD4:
        case 0xDC:
        case 0xE4:
        case 0xEC:
        case 0xF4:
        case 0xFC:
            return true;
        case 0x10:
            /* DJNZ loops back on itself. */
            return true;
        case 0xCF:
        case 0xEF:
            /* RST 08h (error) and RST 28h (calculator) consume inline bytes
               and never return to pc+1, so single-step them instead. */
            return false;
        default:
            return (op & 0xC7u) == 0xC7u;
    }
}

/* Renders the active machine's display after the debugger moved it. */
static void app_debug_render(AppState *app) {
    if (app_debug_is_next(app)) {
        next_machine_render_frame(&app->next);
    } else {
        spectrum_render_frame(&app->spec);
    }
    if (app->main_hwnd != NULL) {
        InvalidateRect(app->main_hwnd, NULL, FALSE);
    }
}

/* Renders registers, flags, paging state, disassembly, and nearby memory into
   the debugger's read-only text view. */
static void app_debug_refresh_window(AppState *app) {
    char stack_buffer[16384];
    char *buffer = stack_buffer;
    const size_t buffer_size = sizeof(stack_buffer);
    char flags[16];
    char ascii[17];
    size_t used = 0;
    uint16_t addr;
    uint16_t disassembly_addresses[APP_DEBUG_DISASSEMBLY_LINE_COUNT];
    uint16_t memory_addresses[6];
    uint16_t stack_addresses[4];
    const z80_t *cpu;

    if (app->main_hwnd != NULL) {
        app_update_runtime_menu(app->main_hwnd, app);
    }
    buffer[0] = '\0';

    if (app->debug.debugger_text == NULL ||
        app->debug.debugger_registers_text == NULL ||
        app->debug.debugger_memory_text == NULL ||
        app->debug.debugger_stack_text == NULL) {
        return;
    }
    if (!app_debug_ready(app)) {
        app_append_textf(
            buffer,
            buffer_size,
            &used,
            "Machine not ready.\r\nDebugger actions are disabled until the machine is initialized.\r\n"
        );
        app_debug_set_disassembly(app, buffer, NULL, 0);
        SetWindowTextA(app->debug.debugger_registers_text, buffer);
        app_debug_set_memory(app->debug.debugger_memory_text, buffer, NULL, NULL, 0);
        app_debug_set_memory(app->debug.debugger_stack_text, buffer, NULL, NULL, 0);
        app_debug_refresh_points_list(app);
        app_debug_update_controls(app);
        return;
    }

    cpu = app_debug_cpu_const(app);
    if (!app->debug.debugger_view_initialized) {
        app_debug_set_view_address(app, app_debug_current_pc(app), true);
    } else if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = app_debug_current_pc(app);
        app_debug_sync_view_controls(app);
    }
    app_debug_format_flags(cpu->f, flags, sizeof(flags));

    app_append_textf(buffer, buffer_size, &used, "%-7s  Model:%s\r\n",
        app->debug.paused ? "PAUSED" : "RUNNING",
        app_debug_is_next(app) ? "ZX Spectrum Next" : app_model_name(app->spec.model));
    app_append_textf(buffer, buffer_size, &used, "PC:%04X  SP:%04X  IX:%04X  IY:%04X\r\n",
        app_debug_current_pc(app), cpu->sp, cpu->ix, cpu->iy);
    app_append_textf(buffer, buffer_size, &used, "AF:%04X  BC:%04X  DE:%04X  HL:%04X\r\n",
        cpu->af, cpu->bc, cpu->de, cpu->hl);
    app_append_textf(buffer, buffer_size, &used, "AF':%04X BC':%04X DE':%04X HL':%04X\r\n",
        cpu->af2, cpu->bc2, cpu->de2, cpu->hl2);
    app_append_textf(buffer, buffer_size, &used, "I:%02X R:%02X  IM:%u  IFF:%u/%u\r\nFlags:%s\r\n",
        cpu->i, cpu->r, (unsigned)cpu->im, cpu->iff1 ? 1u : 0u, cpu->iff2 ? 1u : 0u, flags);
    if (app_debug_is_next(app)) {
        const NextMachine *next = &app->next;
        static const char *speeds[] = {"3.5", "7", "14", "28"};
        app_append_textf(buffer, buffer_size, &used, "MMU:%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
            next->mmu[0], next->mmu[1], next->mmu[2], next->mmu[3],
            next->mmu[4], next->mmu[5], next->mmu[6], next->mmu[7]);
        app_append_textf(buffer, buffer_size, &used, "CPU:%sMHz ROM:%u 7FFD:%02X 1FFD:%02X\r\n",
            speeds[next->nextregs[0x07] & 3], (unsigned)next->rom_bank, next->port_7ffd, next->port_1ffd);
        app_append_textf(buffer, buffer_size, &used, "Layers NR15:%02X L2:%s divMMC:%s MF:%s\r\n",
            next->nextregs[0x15], (next->layer2_control & 0x02) ? "on" : "off",
            next->divmmc_automap ? "mapped" : "off", next->multiface_paged ? "paged" : "off");
    } else {
        app_append_textf(buffer, buffer_size, &used, "FE:%02X  7FFD:%02X  Bank:%u  Lock:%s\r\n",
            app->spec.machine.last_fe_out, app->spec.machine.last_mem_config,
            (unsigned)app->spec.machine.display_ram_bank,
            app->spec.machine.memory_paging_disabled ? "Yes" : "No");
    }
    if (app->debug.point_hit || app->debug.debugger_run_to_hit) {
        app_append_textf(buffer, buffer_size, &used, "%s\r\n", app->debug.hit_text);
    } else if (app->debug.debugger_run_to_active) {
        app_append_textf(buffer, buffer_size, &used, "Run-to:%04X\r\n", app->debug.debugger_run_to_address);
    } else if (app->debug.step_out_active) {
        app_append_textf(buffer, buffer_size, &used, "Stepping out...\r\n");
    }
    SetWindowTextA(app->debug.debugger_registers_text, buffer);
    used = 0;
    buffer[0] = '\0';

    addr = app->debug.debugger_view_address;
    for (int i = 0; i < APP_DEBUG_DISASSEMBLY_LINE_COUNT; ++i) {
        char mnemonic[128];
        char bytes[32];
        uint16_t len;
        size_t byte_used = 0;
        disassembly_addresses[i] = addr;

        app_debug_decode_instruction(app, addr, mnemonic, sizeof(mnemonic), &len);
        for (uint16_t byte_index = 0; byte_index < len && byte_used + 4 < sizeof(bytes); ++byte_index) {
            byte_used += (size_t)snprintf(bytes + byte_used, sizeof(bytes) - byte_used, "%02X ",
                app_debug_mem_rd(app, (uint16_t)(addr + byte_index)));
        }
        bytes[byte_used] = '\0';
        app_append_textf(buffer, buffer_size, &used, "%-12s %s\r\n", bytes, mnemonic);
        addr = (uint16_t)(addr + len);
    }
    app_debug_set_disassembly(app, buffer, disassembly_addresses, APP_DEBUG_DISASSEMBLY_LINE_COUNT);
    used = 0;
    buffer[0] = '\0';

    for (int row = 0; row < 4; ++row) {
        uint16_t row_addr = (uint16_t)(cpu->sp + (uint16_t)(row * 8));
        stack_addresses[row] = row_addr;
        for (int col = 0; col < 8; ++col) {
            app_append_textf(buffer, buffer_size, &used, " %02X", app_debug_mem_rd(app, (uint16_t)(row_addr + col)));
        }
        app_append_textf(buffer, buffer_size, &used, "\r\n");
    }
    app_debug_set_memory(app->debug.debugger_stack_text, buffer, stack_addresses, NULL, 4);
    used = 0;
    buffer[0] = '\0';

    for (int row = 0; row < 6; ++row) {
        uint16_t row_addr = (uint16_t)(app->debug.debugger_view_address + (uint16_t)(row * 16));
        memory_addresses[row] = row_addr;
        for (int col = 0; col < 16; ++col) {
            const uint8_t value = app_debug_mem_rd(app, (uint16_t)(row_addr + col));
            app_append_textf(buffer, buffer_size, &used, " %02X", value);
            ascii[col] = (value >= 0x20 && value <= 0x7E) ? (char)value : '.';
        }
        ascii[16] = '\0';
        app_append_textf(buffer, buffer_size, &used, "  |%s|\r\n", ascii);
    }
    app_debug_set_memory(app->debug.debugger_memory_text, buffer, memory_addresses, NULL, 6);
    app_debug_refresh_points_list(app);
    app_debug_update_controls(app);
}

/* Records why execution stopped and requests the stop. */
static void app_debug_stop_with(AppState *app, const char *format, ...) {
    va_list args;

    va_start(args, format);
    vsnprintf(app->debug.hit_text, sizeof(app->debug.hit_text), format, args);
    va_end(args);
    app->debug.point_hit = true;
    app->debug.debugger_run_to_active = false;
    app->debug.step_out_active = false;
    app->debug.stop_requested = true;
}

/* Runs on every machine tick while the debugger is armed. Memory and port
   accesses are matched as they happen and reported at the end of the
   instruction; execution breakpoints, run-to, step and step-out are checked
   at each instruction boundary. */
static void app_debug_callback(void *user_data, uint64_t pins) {
    AppState *app = (AppState *)user_data;
    z80_t *cpu;

    if (app == NULL) {
        return;
    }
    /* Spectrum contention holds the Z80 clock while the system timing keeps
       advancing.  The system hook still runs on those outer ticks, but the
       CPU and its bus state have not advanced, so they must not be mistaken
       for another instruction boundary or memory access. */
    if (!app_debug_is_next(app) && app->spec.machine.contention_cycle_active) {
        return;
    }
    cpu = app_debug_cpu(app);
    if (!app->debug.access_pending && app->debug.point_count > 0 && (pins & Z80_M1) == 0) {
        AppDebugPointKind kind = APP_DEBUG_POINT_EXEC;
        if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
            kind = APP_DEBUG_POINT_WRITE;
        } else if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
            kind = APP_DEBUG_POINT_READ;
        } else if ((pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
            kind = APP_DEBUG_POINT_OUT;
        } else if ((pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD)) {
            kind = APP_DEBUG_POINT_IN;
        }
        if (kind != APP_DEBUG_POINT_EXEC) {
            const uint16_t address = Z80_GET_ADDR(pins);
            const uint8_t value = Z80_GET_DATA(pins);
            const int index = app_debug_match_point(app, kind, app->debug.instruction_pc, address, value);
            if (index >= 0) {
                static const char *verbs[] = {"", "Read", "Write", "In", "Out"};
                const bool port = kind == APP_DEBUG_POINT_IN || kind == APP_DEBUG_POINT_OUT;
                app->debug.access_pending = true;
                snprintf(app->debug.pending_text, sizeof(app->debug.pending_text),
                    port ? "#%d %s port %04X = %02X (PC %04X)" : "#%d %s %04X = %02X (PC %04X)",
                    index + 1, verbs[kind], address, value, app->debug.instruction_pc);
            }
        }
    }
    if (!z80_opdone(cpu)) {
        if (app->debug.stepping) {
            app->debug.debugger_step_left_boundary = true;
        }
        return;
    }
    {
        const uint16_t next_pc = Z80_GET_ADDR(pins);
        const bool reached_run_to = app->debug.debugger_run_to_active && app->debug.debugger_run_to_address == next_pc;
        const bool skip_breakpoint = app->debug.debugger_skip_breakpoint_once &&
                                     app->debug.debugger_skip_breakpoint_address == next_pc;

        app->debug.instruction_pc = next_pc;
        if (skip_breakpoint) {
            app->debug.debugger_skip_breakpoint_once = false;
        }
        if (app->debug.access_pending) {
            app->debug.access_pending = false;
            app_debug_stop_with(app, "Hit %s", app->debug.pending_text);
            return;
        }
        if (!skip_breakpoint && app->debug.point_count > 0) {
            const int index = app_debug_match_point(app, APP_DEBUG_POINT_EXEC, next_pc, next_pc, 0);
            if (index >= 0) {
                app_debug_stop_with(app, "Hit #%d Exec %04X (hits %u)", index + 1, next_pc,
                    (unsigned)app->debug.points[index].hits);
                return;
            }
        }
        if (reached_run_to) {
            app->debug.debugger_run_to_active = false;
            app->debug.debugger_run_to_hit = true;
            snprintf(app->debug.hit_text, sizeof(app->debug.hit_text), "Run-to hit: %04X", next_pc);
            app->debug.stop_requested = true;
        }
        if (app->debug.step_out_active) {
            /* The routine has returned once SP rises above its entry value. */
            const uint16_t popped = (uint16_t)(cpu->sp - app->debug.step_out_sp);
            if (popped >= 1 && popped < 0x8000) {
                app_debug_stop_with(app, "Stepped out to %04X", next_pc);
                return;
            }
        }
        if (app->debug.stepping && app->debug.debugger_step_left_boundary) {
            app->debug.stop_requested = true;
        }
    }
}

/* Installs or removes the debugger hook on the active machine. */
static void app_debug_arm(AppState *app, bool armed) {
    chips_debug_t *debug = app_debug_is_next(app) ? &app->next.debug : &app->spec.machine.debug;

    debug->callback.func = armed ? app_debug_callback : NULL;
    debug->callback.user_data = armed ? app : NULL;
    debug->stopped = armed ? &app->debug.stop_requested : NULL;
    if (armed) {
        app->debug.instruction_pc = app_debug_current_pc(app);
    }
}

/* Runs up to one video frame of the active machine with the hook armed;
   returns early when the hook requests a stop. */
static void app_debug_exec_armed_slice(AppState *app) {
    app_debug_arm(app, true);
    if (app_debug_is_next(app)) {
        next_machine_exec_frame(&app->next);
    } else {
        zx_exec_ticks(&app->spec.machine, app_machine_frame_tstates(&app->spec));
    }
    app_debug_arm(app, false);
}

static void app_debug_clear_stop_state(AppState *app) {
    app->debug.stop_requested = false;
    app->debug.point_hit = false;
    app->debug.debugger_run_to_hit = false;
    app->debug.access_pending = false;
    app->debug.debugger_step_left_boundary = false;
}

static void app_debug_resume(AppState *app) {
    if (!app_debug_ready(app)) {
        return;
    }

    app->debug.paused = false;
    app->debug.stepping = false;
    app_debug_clear_stop_state(app);
    /* Resuming on a breakpoint must not stop again before moving. */
    if (app_debug_has_breakpoint(app, app_debug_current_pc(app))) {
        app->debug.debugger_skip_breakpoint_once = true;
        app->debug.debugger_skip_breakpoint_address = app_debug_current_pc(app);
    } else {
        app->debug.debugger_skip_breakpoint_once = false;
    }
}

/* Executes exactly one instruction from the current paused machine state and
   then refreshes both the display and debugger snapshot text. */
static void app_debug_step_instruction(AppState *app) {
    if (!app_debug_ready(app)) {
        return;
    }

    app->debug.paused = true;
    app->debug.stepping = true;
    app_debug_clear_stop_state(app);
    app->debug.debugger_run_to_active = false;
    app->debug.step_out_active = false;
    app_debug_exec_armed_slice(app);
    app->debug.stepping = false;
    app_debug_render(app);
    app_debug_refresh_window(app);
}

static void app_debug_step_over_instruction(AppState *app) {
    uint16_t len;
    uint16_t pc;

    if (!app_debug_ready(app)) {
        return;
    }

    pc = app_debug_current_pc(app);
    app_debug_decode_instruction(app, pc, NULL, 0, &len);
    if (!app_debug_is_step_over_candidate(app, pc)) {
        app_debug_step_instruction(app);
        return;
    }

    app_debug_resume(app);
    app->debug.debugger_run_to_active = true;
    app->debug.debugger_run_to_address = (uint16_t)(pc + len);
    app->debug.debugger_skip_breakpoint_once = true;
    app->debug.debugger_skip_breakpoint_address = pc;
    if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = pc;
        app_debug_sync_view_controls(app);
    }
    app_debug_refresh_window(app);
}

/* Runs until the current routine returns to its caller. */
static void app_debug_step_out(AppState *app) {
    if (!app_debug_ready(app)) {
        return;
    }
    app_debug_resume(app);
    app->debug.step_out_active = true;
    app->debug.step_out_sp = app_debug_cpu(app)->sp;
    app->debug.debugger_run_to_active = false;
    app_debug_refresh_window(app);
}

static void app_debug_navigate_view(AppState *app, int delta) {
    if (app == NULL || !app_debug_ready(app)) {
        return;
    }
    app_debug_set_view_address(app, (uint16_t)(app->debug.debugger_view_address + delta), false);
    app_debug_refresh_window(app);
}

static void app_debug_view_pc(AppState *app) {
    if (app == NULL || !app_debug_ready(app)) {
        return;
    }
    app_debug_set_view_address(app, app_debug_current_pc(app), true);
    app_debug_refresh_window(app);
}

static void app_debug_view_sp(AppState *app) {
    if (app == NULL || !app_debug_ready(app)) {
        return;
    }
    app_debug_set_view_address(app, app_debug_cpu(app)->sp, false);
    app_debug_refresh_window(app);
}

static void app_debug_run_to_address(AppState *app, uint16_t address) {
    if (!app_debug_ready(app)) {
        return;
    }

    if (address == app_debug_current_pc(app)) {
        app->debug.paused = true;
        app_debug_clear_stop_state(app);
        app->debug.debugger_run_to_active = false;
        app->debug.debugger_run_to_hit = true;
        snprintf(app->debug.hit_text, sizeof(app->debug.hit_text), "Run-to hit: %04X", address);
        if (app->debug.debugger_view_sync_pc) {
            app->debug.debugger_view_address = address;
            app_debug_sync_view_controls(app);
        }
        app_debug_refresh_window(app);
        return;
    }

    app_debug_resume(app);
    app->debug.debugger_run_to_active = true;
    app->debug.debugger_run_to_address = address;
    if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = app_debug_current_pc(app);
        app_debug_sync_view_controls(app);
    }
    app_debug_refresh_window(app);
}

/* Runs one frame of the active machine with breakpoints armed when the
   debugger is open, then pauses and refreshes it if anything was hit. */
static void app_debug_run_frame(AppState *app, bool render_frame) {
    const bool debugger_active = app->debug.debugger_hwnd != NULL;
    const bool armed = debugger_active &&
                       (app_debug_has_enabled_points(app) || app->debug.debugger_run_to_active ||
                        app->debug.step_out_active);

    if (!app_debug_ready(app)) {
        return;
    }

    if (debugger_active && app->debug.debugger_skip_breakpoint_once &&
        app->debug.debugger_skip_breakpoint_address == app_debug_current_pc(app)) {
        /* Leaving the breakpoint we stopped on; the hook clears the flag. */
    } else if (debugger_active && app_debug_at_instruction_boundary(app)) {
        const uint16_t pc = app_debug_current_pc(app);
        const int index = app->debug.point_count > 0
            ? app_debug_match_point(app, APP_DEBUG_POINT_EXEC, pc, pc, 0)
            : -1;
        if (index >= 0) {
            snprintf(app->debug.hit_text, sizeof(app->debug.hit_text), "Hit #%d Exec %04X (hits %u)",
                index + 1, pc, (unsigned)app->debug.points[index].hits);
            app->debug.point_hit = true;
            app->debug.debugger_run_to_active = false;
            app->debug.step_out_active = false;
            app->debug.paused = true;
            if (app->debug.debugger_view_sync_pc) {
                app->debug.debugger_view_address = pc;
                app_debug_sync_view_controls(app);
            }
            app_debug_render(app);
            app_debug_refresh_window(app);
            return;
        }
    }

    app_debug_clear_stop_state(app);
    if (armed) {
        app_debug_exec_armed_slice(app);
    } else if (app_debug_is_next(app)) {
        next_machine_exec_frame(&app->next);
    } else {
        zx_exec_ticks(&app->spec.machine, app_machine_frame_tstates(&app->spec));
    }
    if (render_frame && !app_debug_is_next(app)) {
        spectrum_render_frame(&app->spec);
    }

    if (app->debug.point_hit || app->debug.debugger_run_to_hit) {
        app->debug.paused = true;
        if (app->debug.debugger_view_sync_pc) {
            app->debug.debugger_view_address = app_debug_current_pc(app);
            app_debug_sync_view_controls(app);
        }
        app_debug_render(app);
        app_debug_refresh_window(app);
    }
}

/* Jumps execution to an explicit address from the debugger and resumes the
   machine without relying on a BASIC-side entry command. */
static void app_debug_run_from_address(AppState *app, uint16_t address) {
    if (!app_debug_ready(app)) {
        return;
    }

    if (app_debug_is_next(app)) {
        app->next.cpu_pins = z80_prefetch(&app->next.cpu, address);
    } else {
        app->spec.machine.pins = z80_prefetch(&app->spec.machine.cpu, address);
    }
    app_debug_resume(app);
    app->debug.debugger_run_to_active = false;
    app->debug.debugger_skip_breakpoint_once = true;
    app->debug.debugger_skip_breakpoint_address = address;
    if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = address;
        app_debug_sync_view_controls(app);
    }
    app_debug_refresh_window(app);
}

/* Clears any stale stepping hook after resets, model swaps, or snapshot loads
   so later debug actions start from a clean machine configuration. */
static void app_debug_machine_changed(AppState *app) {
    app->debug.stepping = false;
    app_debug_clear_stop_state(app);
    app->debug.debugger_run_to_active = false;
    app->debug.step_out_active = false;
    app->debug.debugger_skip_breakpoint_once = false;
    app->spec.machine.debug.callback.func = NULL;
    app->spec.machine.debug.callback.user_data = NULL;
    app->spec.machine.debug.stopped = NULL;
    app->next.debug.callback.func = NULL;
    app->next.debug.callback.user_data = NULL;
    app->next.debug.stopped = NULL;
    if (!app->debug.debugger_view_initialized) {
        app->debug.debugger_view_sync_pc = true;
        app->debug.debugger_view_initialized = true;
    }
    if (app_debug_ready(app) && app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = app_debug_current_pc(app);
    }
    app_debug_sync_view_controls(app);
    app_debug_refresh_window(app);
    app_assembler_refresh_window(app);
    app_poke_refresh_window(app);
}

/* A frame slice can end part-way through an opcode. Finish only that opcode
   before presenting a manual pause, matching instruction-oriented debuggers
   and ensuring the displayed PC is a real instruction address. */
static void app_debug_pause(AppState *app) {
    if (!app_debug_ready(app)) {
        return;
    }

    app->debug.paused = true;
    app->debug.stepping = false;
    app_debug_clear_stop_state(app);
    app->debug.debugger_run_to_active = false;
    app->debug.step_out_active = false;

    if (app_debug_at_instruction_boundary(app)) {
        return;
    }

    app->debug.stepping = true;
    app->debug.debugger_step_left_boundary = true;
    app_debug_exec_armed_slice(app);
    app->debug.stepping = false;
    app->debug.debugger_step_left_boundary = false;
    app_debug_render(app);
}

/* ---------------------------------------------------------------------------
   Commands typed into the debugger's command box.
   --------------------------------------------------------------------------- */

/* Parses "KIND START[-END] [=VALUE] [if CONDITION] [hits N] [off]". */
static bool app_debug_parse_point(const char *text, AppDebugPoint *point, char *error_buffer, size_t error_buffer_size) {
    static const struct {
        const char *keyword;
        AppDebugPointKind kind;
    } kinds[] = {
        {"bp", APP_DEBUG_POINT_EXEC}, {"bx", APP_DEBUG_POINT_EXEC}, {"br", APP_DEBUG_POINT_READ},
        {"bw", APP_DEBUG_POINT_WRITE}, {"bi", APP_DEBUG_POINT_IN}, {"bo", APP_DEBUG_POINT_OUT}};
    char work[192];
    char keyword[8];
    char *cursor;
    char *if_part;
    char *hits_part;
    char *off_part;
    char range[32];
    int start;
    int end;
    size_t length;
    bool found = false;

    memset(point, 0, sizeof(*point));
    point->enabled = true;
    point->value = -1;
    point->hit_target = 1;
    snprintf(work, sizeof(work), "%s", text);

    cursor = work;
    while (*cursor == ' ') {
        cursor++;
    }
    length = strcspn(cursor, " ");
    if (length == 0 || length >= sizeof(keyword)) {
        snprintf(error_buffer, error_buffer_size, "Unknown command: %s", text);
        return false;
    }
    memcpy(keyword, cursor, length);
    keyword[length] = '\0';
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        if (_stricmp(keyword, kinds[i].keyword) == 0) {
            point->kind = kinds[i].kind;
            found = true;
        }
    }
    if (!found) {
        snprintf(error_buffer, error_buffer_size, "Unknown command: %s", keyword);
        return false;
    }
    cursor += length;

    /* Trailing options, located case-insensitively from the right. */
    {
        char lower[192];
        for (size_t i = 0; i <= strlen(cursor); ++i) {
            lower[i] = (char)tolower((unsigned char)cursor[i]);
        }
        off_part = NULL;
        length = strlen(lower);
        if (length >= 4 && strcmp(lower + length - 4, " off") == 0) {
            off_part = cursor + length - 4;
            point->enabled = false;
            *off_part = '\0';
            lower[length - 4] = '\0';
        }
        hits_part = strstr(lower, " hits ");
        if_part = strstr(lower, " if ");
        if (hits_part != NULL) {
            hits_part = cursor + (hits_part - lower);
        }
        if (if_part != NULL) {
            if_part = cursor + (if_part - lower);
        }
    }
    if (hits_part != NULL) {
        int hits;
        *hits_part = '\0';
        if (!app_parse_number(hits_part + 6, &hits) || hits < 1) {
            snprintf(error_buffer, error_buffer_size, "Invalid hit count: %s", hits_part + 6);
            return false;
        }
        point->hit_target = (uint32_t)hits;
    }
    if (if_part != NULL) {
        *if_part = '\0';
        snprintf(point->condition, sizeof(point->condition), "%s", if_part + 4);
        while (point->condition[0] == ' ') {
            memmove(point->condition, point->condition + 1, strlen(point->condition));
        }
    }

    /* Address range and optional =value. */
    while (*cursor == ' ') {
        cursor++;
    }
    length = strcspn(cursor, " =");
    if (length == 0 || length >= sizeof(range)) {
        snprintf(error_buffer, error_buffer_size, "Expected an address after %s.", keyword);
        return false;
    }
    memcpy(range, cursor, length);
    range[length] = '\0';
    cursor += length;
    {
        char *dash = strchr(range, '-');
        if (dash != NULL) {
            *dash = '\0';
        }
        if (!app_debug_parse_number(range, &start) || start < 0 || start > 0xFFFF) {
            snprintf(error_buffer, error_buffer_size, "Invalid address: %s", range);
            return false;
        }
        end = start;
        if (dash != NULL && (!app_debug_parse_number(dash + 1, &end) || end < start || end > 0xFFFF)) {
            snprintf(error_buffer, error_buffer_size, "Invalid address range end: %s", dash + 1);
            return false;
        }
    }
    point->start = (uint16_t)start;
    point->end = (uint16_t)end;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '=') {
        int value;
        cursor++;
        while (*cursor == ' ') {
            cursor++;
        }
        if (point->kind == APP_DEBUG_POINT_EXEC) {
            snprintf(error_buffer, error_buffer_size, "Execution breakpoints do not take a value; use 'if'.");
            return false;
        }
        if (!app_debug_parse_number(cursor, &value) || value < 0 || value > 0xFF) {
            snprintf(error_buffer, error_buffer_size, "Invalid data value: %s", cursor);
            return false;
        }
        point->value = value;
        cursor += strcspn(cursor, " ");
    }
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor != '\0') {
        snprintf(error_buffer, error_buffer_size, "Unexpected text: %s", cursor);
        return false;
    }
    return true;
}

/* Applies "del", "en", "dis" or "reset" to one numbered point or "all". */
static bool app_debug_point_list_command(AppState *app, const char *verb, const char *argument,
                                         char *error_buffer, size_t error_buffer_size) {
    size_t first = 0;
    size_t last = app->debug.point_count;
    int number;

    if (_stricmp(argument, "all") != 0) {
        if (!app_parse_number(argument, &number) || number < 1 || (size_t)number > app->debug.point_count) {
            snprintf(error_buffer, error_buffer_size, "No breakpoint number %s.", argument);
            return false;
        }
        first = (size_t)number - 1;
        last = first + 1;
    }
    if (_stricmp(verb, "del") == 0) {
        while (last > first) {
            app_debug_remove_point(app, --last);
        }
        return true;
    }
    for (size_t i = first; i < last; ++i) {
        if (_stricmp(verb, "en") == 0) {
            app->debug.points[i].enabled = true;
        } else if (_stricmp(verb, "dis") == 0) {
            app->debug.points[i].enabled = false;
        } else {
            app->debug.points[i].hits = 0;
        }
    }
    return true;
}

/* Sets a register from "NAME=expression". */
static bool app_debug_set_register(AppState *app, const char *name, long value) {
    z80_t *cpu = app_debug_cpu(app);
    const uint16_t word = (uint16_t)value;
    const uint8_t byte = (uint8_t)value;
    static const char *byte_names[] = {"A", "F", "B", "C", "D", "E", "H", "L", "I", "R"};
    uint8_t *bytes[] = {&cpu->a, &cpu->f, &cpu->b, &cpu->c, &cpu->d, &cpu->e, &cpu->h, &cpu->l, &cpu->i, &cpu->r};
    static const char *word_names[] = {"AF", "BC", "DE", "HL", "IX", "IY", "SP", "AF'", "BC'", "DE'", "HL'"};
    uint16_t *words[] = {&cpu->af, &cpu->bc, &cpu->de, &cpu->hl, &cpu->ix, &cpu->iy, &cpu->sp,
                         &cpu->af2, &cpu->bc2, &cpu->de2, &cpu->hl2};

    for (size_t i = 0; i < sizeof(byte_names) / sizeof(byte_names[0]); ++i) {
        if (_stricmp(name, byte_names[i]) == 0) {
            *bytes[i] = byte;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(word_names) / sizeof(word_names[0]); ++i) {
        if (_stricmp(name, word_names[i]) == 0) {
            *words[i] = word;
            return true;
        }
    }
    if (_stricmp(name, "PC") == 0) {
        /* Restart instruction fetch at the new address. */
        if (app_debug_is_next(app)) {
            app->next.cpu_pins = z80_prefetch(&app->next.cpu, word);
        } else {
            app->spec.machine.pins = z80_prefetch(&app->spec.machine.cpu, word);
        }
        return true;
    }
    return false;
}

/* Runs one command from the command box. Returns false with an error. */
static bool app_debug_run_command(AppState *app, const char *text, char *error_buffer, size_t error_buffer_size) {
    char command[192];
    char verb[16];
    char *cursor;
    char *equals;
    size_t length;

    snprintf(command, sizeof(command), "%s", text);
    cursor = command;
    while (*cursor == ' ') {
        cursor++;
    }
    length = strlen(cursor);
    while (length > 0 && cursor[length - 1] == ' ') {
        cursor[--length] = '\0';
    }
    if (length == 0) {
        return true;
    }
    length = strcspn(cursor, " =");
    snprintf(verb, sizeof(verb), "%.*s", (int)(length < sizeof(verb) ? length : sizeof(verb) - 1), cursor);

    if (_stricmp(verb, "bp") == 0 || _stricmp(verb, "bx") == 0 || _stricmp(verb, "br") == 0 ||
        _stricmp(verb, "bw") == 0 || _stricmp(verb, "bi") == 0 || _stricmp(verb, "bo") == 0) {
        AppDebugPoint point;
        if (!app_debug_parse_point(cursor, &point, error_buffer, error_buffer_size)) {
            return false;
        }
        if (point.condition[0] != '\0') {
            long ignored;
            /* Reject conditions that will never parse. */
            if (!app_debug_evaluate(app, point.condition, 0, 0, 0, &ignored, error_buffer, error_buffer_size)) {
                return false;
            }
        }
        if (!app_debug_add_point(app, &point)) {
            snprintf(error_buffer, error_buffer_size, "Out of memory.");
            return false;
        }
        app_debug_save_points(app);
        return true;
    }
    if (_stricmp(verb, "del") == 0 || _stricmp(verb, "en") == 0 ||
        _stricmp(verb, "dis") == 0 || _stricmp(verb, "reset") == 0) {
        char *argument = cursor + length;
        while (*argument == ' ') {
            argument++;
        }
        if (*argument == '\0') {
            snprintf(error_buffer, error_buffer_size, "Usage: %s N or %s all", verb, verb);
            return false;
        }
        if (!app_debug_point_list_command(app, verb, argument, error_buffer, error_buffer_size)) {
            return false;
        }
        app_debug_save_points(app);
        return true;
    }
    if (_stricmp(verb, "poke") == 0) {
        char *argument = cursor + length;
        char *token;
        int address;
        int count = 0;
        token = strtok(argument, " ,");
        if (token == NULL || !app_debug_parse_number(token, &address) || address < 0 || address > 0xFFFF) {
            snprintf(error_buffer, error_buffer_size, "Usage: poke ADDRESS BYTE [BYTE...]");
            return false;
        }
        while ((token = strtok(NULL, " ,")) != NULL) {
            int value;
            if (!app_debug_parse_number(token, &value) || value < -128 || value > 255) {
                snprintf(error_buffer, error_buffer_size, "Invalid byte: %s", token);
                return false;
            }
            app_debug_mem_wr(app, (uint16_t)(address + count), (uint8_t)value);
            ++count;
        }
        if (count == 0) {
            snprintf(error_buffer, error_buffer_size, "Usage: poke ADDRESS BYTE [BYTE...]");
            return false;
        }
        app_debug_render(app);
        return true;
    }
    equals = strchr(cursor, '=');
    if (equals != NULL) {
        char name[8];
        long value;
        length = (size_t)(equals - cursor);
        while (length > 0 && cursor[length - 1] == ' ') {
            length--;
        }
        if (length == 0 || length >= sizeof(name)) {
            snprintf(error_buffer, error_buffer_size, "Unknown register.");
            return false;
        }
        memcpy(name, cursor, length);
        name[length] = '\0';
        if (!app_debug_evaluate(app, equals + 1, app_debug_current_pc(app), 0, 0, &value, error_buffer, error_buffer_size)) {
            return false;
        }
        if (!app_debug_set_register(app, name, value)) {
            snprintf(error_buffer, error_buffer_size, "Unknown register: %s", name);
            return false;
        }
        return true;
    }
    snprintf(error_buffer, error_buffer_size,
        "Unknown command: %s\r\nTry bp/br/bw/bi/bo, del/en/dis/reset, poke, or REG=value. See Help (F1).", verb);
    return false;
}

/* Sizes the debugger controls to the current client area of the tool window. */
static void app_debugger_layout_controls(AppState *app, HWND hwnd) {
    RECT rect;
    int width;
    int height;
    int content_width;
    int content_height;
    int top_height;
    int disassembly_width;
    int right_x;
    int registers_height;
    HWND disassembly_group;
    HWND registers_group;
    HWND memory_group;
    HWND stack_group;
    HWND points_group;
    const int top_row_y = 8;
    const int second_row_y = 38;
    const int command_row_y = 68;
    const int content_y = 100;
    const int gap = 8;
    const int memory_height = 152;

    if (app->debug.debugger_text == NULL) {
        return;
    }
    GetClientRect(hwnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width < 736 || height < 320) {
        return;
    }
    content_width = width - 16;
    content_height = height - content_y - 8;
    top_height = content_height - memory_height - gap;
    if (top_height < 120) {
        top_height = content_height / 2;
    }
    disassembly_width = (content_width * 3) / 5;
    right_x = 8 + disassembly_width + gap;
    registers_height = (top_height * 3) / 5;
    disassembly_group = GetDlgItem(hwnd, APP_CTRL_DEBUG_DISASSEMBLY_GROUP);
    registers_group = GetDlgItem(hwnd, APP_CTRL_DEBUG_REGISTERS_GROUP);
    memory_group = GetDlgItem(hwnd, APP_CTRL_DEBUG_MEMORY_GROUP);
    stack_group = GetDlgItem(hwnd, APP_CTRL_DEBUG_STACK_GROUP);
    points_group = GetDlgItem(hwnd, APP_CTRL_DEBUG_POINTS_GROUP);

    MoveWindow(app->debug.debugger_pause_button, 8, top_row_y, 72, 24, TRUE);
    MoveWindow(app->debug.debugger_step_button, 88, top_row_y, 72, 24, TRUE);
    MoveWindow(app->debug.debugger_step_over_button, 168, top_row_y, 82, 24, TRUE);
    MoveWindow(app->debug.debugger_refresh_button, 258, top_row_y, 72, 24, TRUE);
    MoveWindow(app->debug.debugger_address_edit, 338, top_row_y, 88, 24, TRUE);
    MoveWindow(app->debug.debugger_go_button, 434, top_row_y, 48, 24, TRUE);
    MoveWindow(app->debug.debugger_run_to_button, 490, top_row_y, 64, 24, TRUE);
    MoveWindow(app->debug.debugger_run_at_button, 562, top_row_y, 64, 24, TRUE);
    MoveWindow(app->debug.debugger_sync_checkbox, 634, top_row_y + 2, 84, 20, TRUE);
    MoveWindow(app->debug.debugger_breakpoint_toggle_button, 8, second_row_y, 96, 24, TRUE);
    MoveWindow(app->debug.debugger_watchpoint_toggle_button, 112, second_row_y, 96, 24, TRUE);
    MoveWindow(app->debug.debugger_remove_selected_button, 216, second_row_y, 108, 24, TRUE);
    MoveWindow(app->debug.debugger_view_pc_button, 332, second_row_y, 44, 24, TRUE);
    MoveWindow(app->debug.debugger_view_sp_button, 384, second_row_y, 44, 24, TRUE);
    MoveWindow(app->debug.debugger_page_up_button, 436, second_row_y, 64, 24, TRUE);
    MoveWindow(app->debug.debugger_page_down_button, 508, second_row_y, 64, 24, TRUE);
    MoveWindow(app->debug.debugger_step_out_button, 580, second_row_y, 72, 24, TRUE);
    MoveWindow(app->debug.debugger_command_edit, 8, command_row_y, width - 16 - 64 - 8, 24, TRUE);
    MoveWindow(app->debug.debugger_command_button, width - 8 - 64, command_row_y, 64, 24, TRUE);
    MoveWindow(disassembly_group, 8, content_y, disassembly_width, top_height, TRUE);
    MoveWindow(app->debug.debugger_text, 16, content_y + 20, disassembly_width - 16, top_height - 28, TRUE);
    MoveWindow(registers_group, right_x, content_y, content_width - disassembly_width - gap, registers_height, TRUE);
    MoveWindow(app->debug.debugger_registers_text, right_x + 8, content_y + 20,
        content_width - disassembly_width - gap - 16, registers_height - 28, TRUE);
    MoveWindow(points_group, right_x, content_y + registers_height + gap,
        content_width - disassembly_width - gap, top_height - registers_height - gap, TRUE);
    MoveWindow(app->debug.debugger_points_list, right_x + 8, content_y + registers_height + gap + 20,
        content_width - disassembly_width - gap - 16, top_height - registers_height - gap - 28, TRUE);
    {
        const int memory_width = (content_width * 2) / 3;
        const int stack_x = 8 + memory_width + gap;
        const int bottom_height = content_height - top_height - gap;

        MoveWindow(memory_group, 8, content_y + top_height + gap, memory_width, bottom_height, TRUE);
        MoveWindow(app->debug.debugger_memory_text, 16, content_y + top_height + gap + 20,
            memory_width - 16, bottom_height - 28, TRUE);
        MoveWindow(stack_group, stack_x, content_y + top_height + gap,
            content_width - memory_width - gap, bottom_height, TRUE);
        MoveWindow(app->debug.debugger_stack_text, stack_x + 8, content_y + top_height + gap + 20,
            content_width - memory_width - gap - 16, bottom_height - 28, TRUE);
    }
}


static LRESULT CALLBACK app_debugger_address_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    HWND parent = GetParent(hwnd);
    AppState *app = parent != NULL ? (AppState *)GetWindowLongPtrA(parent, GWLP_USERDATA) : NULL;
    WNDPROC old_proc = (app != NULL && app->debug.debugger_address_edit_wndproc != NULL)
        ? app->debug.debugger_address_edit_wndproc
        : DefWindowProcA;

    if (app != NULL && msg == WM_KEYDOWN) {
        const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if ((UINT)wparam == VK_RETURN) {
            UINT command = APP_CTRL_DEBUG_GO;
            if (ctrl_down) {
                command = APP_CTRL_DEBUG_RUN_TO;
            } else if (shift_down) {
                command = APP_CTRL_DEBUG_RUN_AT;
            }
            SendMessageA(parent, WM_COMMAND, command, 0);
            return 0;
        }
        if ((UINT)wparam == VK_ESCAPE) {
            app_debug_sync_view_controls(app);
            SendMessageA(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        if (!ctrl_down && !shift_down) {
            switch ((UINT)wparam) {
                case VK_UP:
                    app_debug_navigate_view(app, -16);
                    return 0;
                case VK_DOWN:
                    app_debug_navigate_view(app, 16);
                    return 0;
                case VK_PRIOR:
                    app_debug_navigate_view(app, -256);
                    return 0;
                case VK_NEXT:
                    app_debug_navigate_view(app, 256);
                    return 0;
                default:
                    break;
            }
        }
        if (ctrl_down) {
            switch ((UINT)wparam) {
                case VK_HOME:
                    app_debug_view_pc(app);
                    return 0;
                case VK_END:
                    app_debug_view_sp(app);
                    return 0;
                default:
                    break;
            }
        }
    }

    return CallWindowProcA(old_proc, hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK app_debugger_command_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    HWND parent = GetParent(hwnd);
    AppState *app = parent != NULL ? (AppState *)GetWindowLongPtrA(parent, GWLP_USERDATA) : NULL;
    WNDPROC old_proc = (app != NULL && app->debug.debugger_command_edit_wndproc != NULL)
        ? app->debug.debugger_command_edit_wndproc
        : DefWindowProcA;

    if (app != NULL && msg == WM_KEYDOWN && (UINT)wparam == VK_RETURN) {
        SendMessageA(parent, WM_COMMAND, APP_CTRL_DEBUG_COMMAND_RUN, 0);
        return 0;
    }
    if (app != NULL && msg == WM_KEYDOWN && (UINT)wparam == VK_ESCAPE) {
        SetWindowTextA(hwnd, "");
        return 0;
    }
    if (msg == WM_CHAR && (wparam == '\r' || wparam == 27)) {
        return 0;
    }
    return CallWindowProcA(old_proc, hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK app_debugger_points_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    HWND parent = GetParent(hwnd);
    AppState *app = parent != NULL ? (AppState *)GetWindowLongPtrA(parent, GWLP_USERDATA) : NULL;
    WNDPROC old_proc = (app != NULL && app->debug.debugger_points_list_wndproc != NULL)
        ? app->debug.debugger_points_list_wndproc
        : DefWindowProcA;

    if (app != NULL && msg == WM_KEYDOWN && (UINT)wparam == VK_DELETE) {
        SendMessageA(parent, WM_COMMAND, APP_CTRL_DEBUG_REMOVE_SELECTED, 0);
        return 0;
    }

    return CallWindowProcA(old_proc, hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK app_debugger_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *create = (CREATESTRUCTA *)lparam;
            app = (AppState *)create->lpCreateParams;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            if (app != NULL) {
                app->debug.debugger_hwnd = hwnd;
            }
            return TRUE;
        }
        case WM_CREATE: {
            HMENU menu;
            RECT rect;

            if (app == NULL) {
                return -1;
            }
            menu = app_create_debugger_menu();
            if (menu != NULL) {
                SetMenu(hwnd, menu);
                DrawMenuBar(hwnd);
            }
            app->debug.debugger_panel = CreateDialogParamA(
                GetModuleHandleA(NULL),
                MAKEINTRESOURCEA(APP_DIALOG_DEBUGGER),
                hwnd,
                app_debugger_dlgproc,
                (LPARAM)app
            );
            if (app->debug.debugger_panel == NULL) {
                return -1;
            }
            GetClientRect(hwnd, &rect);
            MoveWindow(
                app->debug.debugger_panel,
                0,
                0,
                rect.right - rect.left,
                rect.bottom - rect.top,
                TRUE
            );
            return 0;
        }
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && app != NULL && app->debug.debugger_panel != NULL) {
                MoveWindow(app->debug.debugger_panel, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            limits->ptMinTrackSize.x = 760;
            limits->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            app_set_modal_loop_timer(app, hwnd, true);
            return 0;
        case WM_EXITSIZEMOVE:
            app_set_modal_loop_timer(app, hwnd, false);
            return 0;
        case WM_TIMER:
            if (app != NULL && wparam == APP_TIMER_MODAL_LOOP) {
                app_tick_emulator(app);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (app != NULL &&
                app->debug.debugger_panel != NULL &&
                SendMessageA(app->debug.debugger_panel, WM_COMMAND, wparam, lparam)) {
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (app != NULL) {
                app_set_modal_loop_timer(app, hwnd, false);
                app->debug.debugger_hwnd = NULL;
                app->debug.debugger_panel = NULL;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static INT_PTR CALLBACK app_debugger_dlgproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_INITDIALOG: {
            static const ACCEL debugger_accels[] = {
                {FVIRTKEY, VK_F1, APP_MENU_DEBUG_HELP_SHOW},
                {FVIRTKEY, VK_F5, APP_CTRL_DEBUG_PAUSE},
                {FVIRTKEY, VK_F9, APP_CTRL_DEBUG_BREAKPOINT_TOGGLE},
                {FVIRTKEY, VK_F10, APP_CTRL_DEBUG_STEP_OVER},
                {FVIRTKEY, VK_F11, APP_CTRL_DEBUG_STEP},
                {FSHIFT | FVIRTKEY, VK_F11, APP_CTRL_DEBUG_STEP_OUT},
                {FCONTROL | FVIRTKEY, 'G', APP_CTRL_DEBUG_GO},
                {FCONTROL | FVIRTKEY, 'P', APP_CTRL_DEBUG_VIEW_PC},
                {FCONTROL | FVIRTKEY, 'R', APP_CTRL_DEBUG_RUN_AT},
                {FCONTROL | FVIRTKEY, 'S', APP_CTRL_DEBUG_VIEW_SP},
                {FCONTROL | FVIRTKEY, 'T', APP_CTRL_DEBUG_RUN_TO},
                {FCONTROL | FVIRTKEY, 'W', APP_CTRL_DEBUG_WATCHPOINT_TOGGLE},
                {FVIRTKEY, VK_PRIOR, APP_CTRL_DEBUG_PAGE_UP},
                {FVIRTKEY, VK_NEXT, APP_CTRL_DEBUG_PAGE_DOWN}
            };
            HFONT font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
            app = (AppState *)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            if (app == NULL) {
                return FALSE;
            }
            app->debug.debugger_background_brush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            app->debug.debugger_surface_brush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
            app->debug.debugger_input_brush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
            app->debug.debugger_pause_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_PAUSE);
            app->debug.debugger_step_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_STEP);
            app->debug.debugger_step_over_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_STEP_OVER);
            app->debug.debugger_refresh_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_REFRESH);
            app->debug.debugger_address_edit = GetDlgItem(hwnd, APP_CTRL_DEBUG_ADDRESS);
            app->debug.debugger_go_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_GO);
            app->debug.debugger_sync_checkbox = GetDlgItem(hwnd, APP_CTRL_DEBUG_SYNC);
            app->debug.debugger_breakpoint_toggle_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_BREAKPOINT_TOGGLE);
            app->debug.debugger_watchpoint_toggle_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_WATCHPOINT_TOGGLE);
            app->debug.debugger_remove_selected_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_REMOVE_SELECTED);
            app->debug.debugger_view_pc_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_VIEW_PC);
            app->debug.debugger_view_sp_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_VIEW_SP);
            app->debug.debugger_page_up_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_PAGE_UP);
            app->debug.debugger_page_down_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_PAGE_DOWN);
            app->debug.debugger_run_to_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_RUN_TO);
            app->debug.debugger_run_at_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_RUN_AT);
            app->debug.debugger_text = GetDlgItem(hwnd, APP_CTRL_DEBUG_TEXT);
            app->debug.debugger_registers_text = GetDlgItem(hwnd, APP_CTRL_DEBUG_REGISTERS);
            app->debug.debugger_memory_text = GetDlgItem(hwnd, APP_CTRL_DEBUG_MEMORY);
            app->debug.debugger_stack_text = GetDlgItem(hwnd, APP_CTRL_DEBUG_STACK);
            app->debug.debugger_points_list = GetDlgItem(hwnd, APP_CTRL_DEBUG_POINTS_LIST);
            app->debug.debugger_step_out_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_STEP_OUT);
            app->debug.debugger_command_edit = GetDlgItem(hwnd, APP_CTRL_DEBUG_COMMAND);
            app->debug.debugger_command_button = GetDlgItem(hwnd, APP_CTRL_DEBUG_COMMAND_RUN);
            SendMessageA(app->debug.debugger_command_edit, EM_SETLIMITTEXT, 180, 0);
            SendMessageA(app->debug.debugger_command_edit, EM_SETCUEBANNER, TRUE,
                (LPARAM)L"Command: bp 8000 if A=5 | bw 4000-57FF =0 | bo FE | del 1 | HL=4000 | poke 8000 C9");
            app->debug.debugger_command_edit_wndproc = (WNDPROC)SetWindowLongPtrA(
                app->debug.debugger_command_edit, GWLP_WNDPROC, (LONG_PTR)app_debugger_command_wndproc);
            app->debug.debugger_accel = CreateAcceleratorTableA((LPACCEL)debugger_accels, (int)(sizeof(debugger_accels) / sizeof(debugger_accels[0])));
            SendMessageA(app->debug.debugger_address_edit, EM_SETLIMITTEXT, 15, 0);
            SendMessageA(app->debug.debugger_address_edit, WM_SETFONT, (WPARAM)font, TRUE);
            app_debug_configure_disassembly(app->debug.debugger_text);
            SendMessageA(app->debug.debugger_registers_text, WM_SETFONT, (WPARAM)font, TRUE);
            app_debug_configure_memory(app->debug.debugger_memory_text);
            app_debug_configure_memory(app->debug.debugger_stack_text);
            SendMessageA(app->debug.debugger_points_list, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageA(app->debug.debugger_command_edit, WM_SETFONT, (WPARAM)font, TRUE);
            app->debug.debugger_address_edit_wndproc = (WNDPROC)SetWindowLongPtrA(
                app->debug.debugger_address_edit,
                GWLP_WNDPROC,
                (LONG_PTR)app_debugger_address_wndproc
            );
            app->debug.debugger_points_list_wndproc = (WNDPROC)SetWindowLongPtrA(
                app->debug.debugger_points_list,
                GWLP_WNDPROC,
                (LONG_PTR)app_debugger_points_wndproc
            );
            app_apply_flat_control_style(hwnd);
            app_debugger_layout_controls(app, hwnd);
            app_debug_sync_view_controls(app);
            app_debug_refresh_window(app);
            return FALSE;
        }
        case WM_SIZE:
            if (app != NULL) {
                app_debugger_layout_controls(app, hwnd);
            }
            return TRUE;
        case WM_ENTERSIZEMOVE:
        case WM_EXITSIZEMOVE:
        case WM_TIMER:
            break;
        case WM_ERASEBKGND:
            if (app != NULL && app->debug.debugger_background_brush != NULL) {
                RECT rect;
                GetClientRect(hwnd, &rect);
                FillRect((HDC)wparam, &rect, app->debug.debugger_background_brush);
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, 1);
                return TRUE;
            }
            break;
        case WM_DRAWITEM:
            if (app_draw_flat_button((const DRAWITEMSTRUCT *)lparam)) {
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT:
            if (app != NULL) {
                HDC hdc = (HDC)wparam;
                HWND control = (HWND)lparam;

                if (control == app->debug.debugger_address_edit || control == app->debug.debugger_command_edit) {
                    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                    SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                    SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)(app->debug.debugger_input_brush != NULL
                        ? app->debug.debugger_input_brush
                        : GetSysColorBrush(COLOR_WINDOW)));
                    return TRUE;
                }
            }
            break;
        case WM_CTLCOLORLISTBOX:
            if (app != NULL) {
                HDC hdc = (HDC)wparam;
                HWND control = (HWND)lparam;

                if (control == app->debug.debugger_points_list) {
                    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                    SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                    SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)(app->debug.debugger_input_brush != NULL
                        ? app->debug.debugger_input_brush
                        : GetSysColorBrush(COLOR_WINDOW)));
                    return TRUE;
                }
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (app != NULL) {
                HDC hdc = (HDC)wparam;
                HWND control = (HWND)lparam;

                if (control == app->debug.debugger_text ||
                    control == app->debug.debugger_registers_text ||
                    control == app->debug.debugger_memory_text ||
                    control == app->debug.debugger_stack_text) {
                    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                    SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                    SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)(app->debug.debugger_surface_brush != NULL
                        ? app->debug.debugger_surface_brush
                        : GetSysColorBrush(COLOR_WINDOW)));
                    return TRUE;
                }
                SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
                SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
                SetBkMode(hdc, TRANSPARENT);
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)(app->debug.debugger_background_brush != NULL
                    ? app->debug.debugger_background_brush
                    : GetSysColorBrush(COLOR_BTNFACE)));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (app == NULL) {
                break;
            }
            switch (LOWORD(wparam)) {
                case APP_MENU_DEBUG_HELP_SHOW:
                    app_show_debugger_help(hwnd);
                    return TRUE;
                case APP_CTRL_DEBUG_PAUSE:
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    if (app->debug.paused) {
                        app_debug_resume(app);
                    } else {
                        app_debug_pause(app);
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                case APP_CTRL_DEBUG_STEP:
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    app_debug_step_instruction(app);
                    return TRUE;
                case APP_CTRL_DEBUG_STEP_OVER:
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    app_debug_step_over_instruction(app);
                    return TRUE;
                case APP_CTRL_DEBUG_REFRESH:
                    if (!app_debug_ready(app)) {
                        app_debug_refresh_window(app);
                        return TRUE;
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                case APP_CTRL_DEBUG_GO: {
                    char error[128];
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    if (!app_debug_apply_address_input(app, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_VIEW_PC:
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    app_debug_view_pc(app);
                    return TRUE;
                case APP_CTRL_DEBUG_VIEW_SP:
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    app_debug_view_sp(app);
                    return TRUE;
                case APP_CTRL_DEBUG_PAGE_UP:
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    app_debug_navigate_view(app, -256);
                    return TRUE;
                case APP_CTRL_DEBUG_PAGE_DOWN:
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    app_debug_navigate_view(app, 256);
                    return TRUE;
                case APP_CTRL_DEBUG_RUN_TO: {
                    char error[128];
                    uint16_t address;
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_run_to_address(app, address);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_BREAKPOINT_TOGGLE: {
                    char error[128];
                    uint16_t address;
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    if (!app_debug_toggle_simple_point(app, APP_DEBUG_POINT_EXEC, address)) {
                        MessageBoxA(hwnd, "Could not update the breakpoint list.", "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_save_points(app);
                    app_debug_refresh_window(app);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_WATCHPOINT_TOGGLE: {
                    char error[128];
                    uint16_t address;
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    if (!app_debug_toggle_simple_point(app, APP_DEBUG_POINT_WRITE, address)) {
                        MessageBoxA(hwnd, "Could not update the watchpoint list.", "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_save_points(app);
                    app_debug_refresh_window(app);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_REMOVE_SELECTED: {
                    const int selected = app_debug_selected_point(app);
                    if (selected < 0) {
                        MessageBoxA(hwnd, "Select a breakpoint first.", "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_remove_point(app, (size_t)selected);
                    app_debug_save_points(app);
                    app_debug_refresh_window(app);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_STEP_OUT:
                    if (!app_debug_ready(app) || !app->debug.paused) {
                        return TRUE;
                    }
                    app_debug_step_out(app);
                    return TRUE;
                case APP_CTRL_DEBUG_COMMAND_RUN: {
                    char command[192];
                    char error[256];
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    GetWindowTextA(app->debug.debugger_command_edit, command, (int)sizeof(command));
                    if (!app_debug_run_command(app, command, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        SetFocus(app->debug.debugger_command_edit);
                        return TRUE;
                    }
                    SetWindowTextA(app->debug.debugger_command_edit, "");
                    app_debug_refresh_window(app);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_RUN_AT: {
                    char error[128];
                    uint16_t address;
                    if (!app_debug_ready(app)) {
                        return TRUE;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_run_from_address(app, address);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_SYNC:
                    if (HIWORD(wparam) == BN_CLICKED) {
                        bool sync_to_pc = SendMessageA(app->debug.debugger_sync_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        if (sync_to_pc && app_debug_ready(app)) {
                            app_debug_set_view_address(app, app_debug_current_pc(app), true);
                        } else {
                            app->debug.debugger_view_sync_pc = sync_to_pc;
                            app_debug_sync_view_controls(app);
                        }
                        app_debug_refresh_window(app);
                        return TRUE;
                    }
                    break;
                case APP_CTRL_DEBUG_POINTS_LIST:
                    if (HIWORD(wparam) == LBN_SELCHANGE) {
                        if (!app_debug_ready(app)) {
                            app_debug_update_controls(app);
                            return TRUE;
                        }
                        const int selected = app_debug_selected_point(app);
                        if (selected >= 0 && app->debug.points[selected].kind != APP_DEBUG_POINT_IN &&
                            app->debug.points[selected].kind != APP_DEBUG_POINT_OUT) {
                            app_debug_set_view_address(app, app->debug.points[selected].start, false);
                            app_debug_refresh_window(app);
                        } else {
                            app_debug_update_controls(app);
                        }
                        return TRUE;
                    }
                    if (HIWORD(wparam) == LBN_DBLCLK) {
                        /* Double-click enables or disables the point. */
                        const int selected = app_debug_selected_point(app);
                        if (selected >= 0) {
                            app->debug.points[selected].enabled = !app->debug.points[selected].enabled;
                            app_debug_save_points(app);
                            app_debug_refresh_window(app);
                        }
                        return TRUE;
                    }
                    break;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            return TRUE;
        case WM_DESTROY:
            if (app != NULL) {
                /* Children are destroyed after this; give them back their own procs first. */
                if (app->debug.debugger_address_edit != NULL && app->debug.debugger_address_edit_wndproc != NULL) {
                    SetWindowLongPtrA(app->debug.debugger_address_edit, GWLP_WNDPROC,
                        (LONG_PTR)app->debug.debugger_address_edit_wndproc);
                }
                if (app->debug.debugger_points_list != NULL && app->debug.debugger_points_list_wndproc != NULL) {
                    SetWindowLongPtrA(app->debug.debugger_points_list, GWLP_WNDPROC,
                        (LONG_PTR)app->debug.debugger_points_list_wndproc);
                }
                if (app->debug.debugger_command_edit != NULL && app->debug.debugger_command_edit_wndproc != NULL) {
                    SetWindowLongPtrA(app->debug.debugger_command_edit, GWLP_WNDPROC,
                        (LONG_PTR)app->debug.debugger_command_edit_wndproc);
                }
                if (app->debug.debugger_background_brush != NULL) {
                    DeleteObject(app->debug.debugger_background_brush);
                    app->debug.debugger_background_brush = NULL;
                }
                if (app->debug.debugger_surface_brush != NULL) {
                    DeleteObject(app->debug.debugger_surface_brush);
                    app->debug.debugger_surface_brush = NULL;
                }
                if (app->debug.debugger_input_brush != NULL) {
                    DeleteObject(app->debug.debugger_input_brush);
                    app->debug.debugger_input_brush = NULL;
                }
                app->debug.paused = false;
                app->debug.stepping = false;
                app->debug.stop_requested = false;
                app->debug.debugger_run_to_active = false;
                app->debug.debugger_run_to_hit = false;
                app->debug.debugger_panel = NULL;
                app->debug.debugger_text = NULL;
                app->debug.debugger_registers_text = NULL;
                app->debug.debugger_memory_text = NULL;
                app->debug.debugger_stack_text = NULL;
                app->debug.debugger_pause_button = NULL;
                app->debug.debugger_step_button = NULL;
                app->debug.debugger_step_over_button = NULL;
                app->debug.debugger_refresh_button = NULL;
                app->debug.debugger_address_edit = NULL;
                app->debug.debugger_go_button = NULL;
                app->debug.debugger_sync_checkbox = NULL;
                app->debug.debugger_breakpoint_toggle_button = NULL;
                app->debug.debugger_run_at_button = NULL;
                app->debug.debugger_run_to_button = NULL;
                app->debug.debugger_watchpoint_toggle_button = NULL;
                app->debug.debugger_remove_selected_button = NULL;
                app->debug.debugger_view_pc_button = NULL;
                app->debug.debugger_view_sp_button = NULL;
                app->debug.debugger_page_up_button = NULL;
                app->debug.debugger_page_down_button = NULL;
                app->debug.debugger_points_list = NULL;
                app->debug.debugger_step_out_button = NULL;
                app->debug.debugger_command_edit = NULL;
                app->debug.debugger_command_button = NULL;
                app->debug.debugger_command_edit_wndproc = NULL;
                app->debug.step_out_active = false;
                if (app->debug.debugger_accel != NULL) {
                    DestroyAcceleratorTable(app->debug.debugger_accel);
                    app->debug.debugger_accel = NULL;
                }
                app->debug.debugger_address_edit_wndproc = NULL;
                app->debug.debugger_points_list_wndproc = NULL;
            }
            return TRUE;
        default:
            break;
    }
    return FALSE;
}


static void app_open_debugger_window(AppState *app) {
    if (app->debug.debugger_hwnd != NULL) {
        ShowWindow(app->debug.debugger_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(app->debug.debugger_hwnd);
        app_debug_refresh_window(app);
        return;
    }

    if (app_scintilla_module == NULL) {
        app_scintilla_module = LoadLibraryA("Scintilla.dll");
    }
    if (app_scintilla_module == NULL) {
        MessageBoxA(
            app->main_hwnd,
            "Scintilla.dll could not be loaded. Rebuild the application or place the DLL beside zx-spectrum.exe.",
            "Spectrum Debugger",
            MB_OK | MB_ICONERROR
        );
        return;
    }

    app->debug.debugger_hwnd = CreateWindowExA(
        0,
        "ZXSpecDebuggerWindow",
        "Spectrum Debugger",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        560,
        app->main_hwnd,
        NULL,
        GetModuleHandleA(NULL),
        app
    );
    if (app->debug.debugger_hwnd != NULL) {
        app_apply_window_icons(app->debug.debugger_hwnd);
    }
}


static void app_show_debugger_help(HWND hwnd) {
    MessageBoxA(
        hwnd,
        "Spectrum Debugger\r\n"
        "\r\n"
        "Execution:\r\n"
        "  F5 Run/Pause\r\n"
        "  F11 Step one instruction\r\n"
        "  F10 Step over CALLs, RSTs, DJNZ and block repeats\r\n"
        "  Shift+F11 Step out of the current subroutine\r\n"
        "  Refresh redraws the current register and memory view.\r\n"
        "\r\n"
        "Numbers:\r\n"
        "  Addresses and values are hex by default (8000, FE).\r\n"
        "  $hex, 0xhex, H suffix and %binary also work; #123 is decimal.\r\n"
        "  Hit counts and list numbers are decimal.\r\n"
        "\r\n"
        "Address field:\r\n"
        "  - Enter an address then click Go or press Enter to move the view.\r\n"
        "  - Ctrl+Enter runs until the typed address.\r\n"
        "  - Shift+Enter starts execution from the typed address.\r\n"
        "\r\n"
        "Navigation:\r\n"
        "  Ctrl+P jumps the view to PC.\r\n"
        "  Ctrl+S jumps the view to SP.\r\n"
        "  PgUp/PgDn pages the disassembly and memory view by 256 bytes.\r\n"
        "  Ctrl+Home and Ctrl+End jump directly to PC and SP from the address box.\r\n"
        "\r\n"
        "Breakpoints:\r\n"
        "  F9 toggles an execution breakpoint at the typed address.\r\n"
        "  Ctrl+W toggles a write watchpoint at the typed address.\r\n"
        "  Double-click a list entry to enable or disable it.\r\n"
        "  Delete or Remove Sel removes the selected entry.\r\n"
        "  Breakpoints are saved and restored with the emulator settings.\r\n"
        "\r\n"
        "Commands (type in the command box, press Enter or Do):\r\n"
        "  bp ADDR             break on execution (bx is the same)\r\n"
        "  br/bw RANGE         break on memory read/write\r\n"
        "  bi/bo PORT          break on port in/out (00-FF matches the low byte)\r\n"
        "  RANGE is START or START-END, e.g. bw 4000-57FF\r\n"
        "  Options: =VAL (read/write/port data), if COND, hits N, off\r\n"
        "  e.g. bp 8000 if A=3 && HL>4000 hits 2\r\n"
        "  del N|all, en N|all, dis N|all, reset N|all (hit counts)\r\n"
        "  poke ADDR B1 B2 ...  writes memory\r\n"
        "  REG=expr             sets a register, e.g. HL=PEEK(5C00)+1 or PC=8000\r\n"
        "\r\n"
        "Conditions:\r\n"
        "  Registers A..L, AF..SP, IX, IY, IXH.., I, R, AF', etc.\r\n"
        "  Flags ZF CF SF HF PF NF; ADDR and VALUE for the accessed address/data.\r\n"
        "  PEEK(a), DPEEK(a), NR(reg) (Spectrum Next registers).\r\n"
        "  Operators: + - * / % & | ^ ~ << >> == = != < <= > >= && || !\r\n"
        "\r\n"
        "Spectrum Next:\r\n"
        "  The debugger works on the Next, disassembles Z80N instructions\r\n"
        "  and NR() reads Next registers in conditions.\r\n"
        "\r\n"
        "View markers:\r\n"
        "  A blue highlighted line and arrow mark the current PC.\r\n"
        "  A red circle marks an active breakpoint.\r\n"
        "  Recent breakpoint, run-to, and watch hits are shown in Registers.\r\n"
        "\r\n"
        "Tips:\r\n"
        "  - Sync PC keeps the debugger view following the live program counter.\r\n"
        "  - Run To stops when execution reaches the typed address.\r\n"
        "  - Run @ changes PC first, then resumes execution.\r\n",
        "Debugger Help",
        MB_OK | MB_ICONINFORMATION
    );
}

