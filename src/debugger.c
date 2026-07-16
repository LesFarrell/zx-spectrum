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

/* Reads one byte from the current Spectrum memory map, including any active
   128K bank paging, so debugger views match the running machine state. */
static uint8_t app_debug_mem_rd(const Spectrum *spec, uint16_t addr) {
    return mem_rd((mem_t *)&spec->machine.mem, addr);
}

/* Reads a 16-bit little-endian value from the current Spectrum memory map. */
static uint16_t app_debug_mem_rd16(const Spectrum *spec, uint16_t addr) {
    return mem_rd16((mem_t *)&spec->machine.mem, addr);
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
        if (app->spec.machine_ready && addresses[line] == app->spec.machine.cpu.pc) {
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

/* Produces a compact mnemonic for one instruction at the supplied address.
   Unsupported prefixes fall back to byte-oriented output instead of guessing. */
static void app_debug_decode_instruction(
    const Spectrum *spec,
    uint16_t addr,
    char *out,
    size_t out_size,
    uint8_t *instruction_length
) {
    char discard[64];
    static const char *reg8[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    static const char *reg16[] = {"BC", "DE", "HL", "SP"};
    static const char *reg16_push[] = {"BC", "DE", "HL", "AF"};
    static const char *cond[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
    static const char *rot[] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};
    static const char *alu[] = {"ADD A", "ADC A", "SUB", "SBC A", "AND", "XOR", "OR", "CP"};
    const uint8_t op = app_debug_mem_rd(spec, addr);
    const uint8_t next = app_debug_mem_rd(spec, (uint16_t)(addr + 1));
    const uint8_t next2 = app_debug_mem_rd(spec, (uint16_t)(addr + 2));
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
                snprintf(out, out_size, "LD %s,%04Xh", index_reg, app_debug_mem_rd16(spec, (uint16_t)(addr + 2)));
                return;
            case 0x22:
                *instruction_length = 4;
                snprintf(out, out_size, "LD (%04Xh),%s", app_debug_mem_rd16(spec, (uint16_t)(addr + 2)), index_reg);
                return;
            case 0x2A:
                *instruction_length = 4;
                snprintf(out, out_size, "LD %s,(%04Xh)", index_reg, app_debug_mem_rd16(spec, (uint16_t)(addr + 2)));
                return;
            case 0x23:
                snprintf(out, out_size, "INC %s", index_reg);
                return;
            case 0x2B:
                snprintf(out, out_size, "DEC %s", index_reg);
                return;
            case 0x34:
                *instruction_length = 3;
                snprintf(out, out_size, "INC (%s%+d)", index_reg, (int)(int8_t)app_debug_mem_rd(spec, (uint16_t)(addr + 2)));
                return;
            case 0x35:
                *instruction_length = 3;
                snprintf(out, out_size, "DEC (%s%+d)", index_reg, (int)(int8_t)app_debug_mem_rd(spec, (uint16_t)(addr + 2)));
                return;
            case 0x36:
                *instruction_length = 4;
                snprintf(
                    out,
                    out_size,
                    "LD (%s%+d),%02Xh",
                    index_reg,
                    (int)(int8_t)app_debug_mem_rd(spec, (uint16_t)(addr + 2)),
                    app_debug_mem_rd(spec, (uint16_t)(addr + 3))
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
            default:
                snprintf(out, out_size, "DB %02Xh,%02Xh", op, next);
                return;
        }
    }

    if (op == 0xED) {
        *instruction_length = 2;
        switch (next) {
            case 0x44:
            case 0x4C:
            case 0x54:
            case 0x5C:
            case 0x64:
            case 0x6C:
            case 0x74:
            case 0x7C:
                snprintf(out, out_size, "NEG");
                return;
            case 0x45:
            case 0x4D:
            case 0x55:
            case 0x5D:
            case 0x65:
            case 0x6D:
            case 0x75:
            case 0x7D:
                snprintf(out, out_size, (next == 0x4D) ? "RETI" : "RETN");
                return;
            default:
                snprintf(out, out_size, "DB EDh,%02Xh", next);
                return;
        }
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
            snprintf(out, out_size, "%s,%s", alu[y], reg8[z]);
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
                    snprintf(out, out_size, "%s,%02Xh", alu[y], next);
                    return;
                case 7:
                    snprintf(out, out_size, "RST %02Xh", y * 8);
                    return;
            }
            break;
    }

    snprintf(out, out_size, "DB %02Xh", op);
}

/* Refreshes the debugger button captions and enabled state so the current
   paused/running state remains visible from the tool window chrome. */
static void app_debug_update_controls(AppState *app) {
    bool has_selected_point = false;
    const BOOL machine_ready = app->spec.machine_ready ? TRUE : FALSE;

    if (app->debug.debugger_hwnd == NULL) {
        return;
    }
    if (app->debug.debugger_points_list != NULL) {
        has_selected_point = SendMessageA(app->debug.debugger_points_list, LB_GETCURSEL, 0, 0) != LB_ERR;
    }
    SetWindowTextA(app->debug.debugger_pause_button, app->debug.paused ? "Run" : "Pause");
    EnableWindow(app->debug.debugger_pause_button, machine_ready);
    EnableWindow(app->debug.debugger_step_button, (app->debug.paused && app->spec.machine_ready) ? TRUE : FALSE);
    EnableWindow(app->debug.debugger_step_over_button, (app->debug.paused && app->spec.machine_ready) ? TRUE : FALSE);
    if (app->debug.debugger_refresh_button != NULL) {
        EnableWindow(app->debug.debugger_refresh_button, machine_ready);
    }
    if (app->debug.debugger_address_edit != NULL) {
        EnableWindow(app->debug.debugger_address_edit, machine_ready);
    }
    if (app->debug.debugger_go_button != NULL) {
        EnableWindow(app->debug.debugger_go_button, machine_ready);
    }
    if (app->debug.debugger_run_to_button != NULL) {
        EnableWindow(app->debug.debugger_run_to_button, machine_ready);
    }
    if (app->debug.debugger_run_at_button != NULL) {
        EnableWindow(app->debug.debugger_run_at_button, machine_ready);
    }
    if (app->debug.debugger_breakpoint_toggle_button != NULL) {
        EnableWindow(app->debug.debugger_breakpoint_toggle_button, machine_ready);
    }
    if (app->debug.debugger_watchpoint_toggle_button != NULL) {
        EnableWindow(app->debug.debugger_watchpoint_toggle_button, machine_ready);
    }
    if (app->debug.debugger_remove_selected_button != NULL) {
        EnableWindow(app->debug.debugger_remove_selected_button, (has_selected_point && app->spec.machine_ready) ? TRUE : FALSE);
    }
    if (app->debug.debugger_view_pc_button != NULL) {
        EnableWindow(app->debug.debugger_view_pc_button, machine_ready);
    }
    if (app->debug.debugger_view_sp_button != NULL) {
        EnableWindow(app->debug.debugger_view_sp_button, machine_ready);
    }
    if (app->debug.debugger_page_up_button != NULL) {
        EnableWindow(app->debug.debugger_page_up_button, machine_ready);
    }
    if (app->debug.debugger_page_down_button != NULL) {
        EnableWindow(app->debug.debugger_page_down_button, machine_ready);
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

/* Returns true when the supplied address is currently in the active debugger
   breakpoint list. */
static bool app_debug_has_breakpoint(const AppState *app, uint16_t address) {
    for (size_t i = 0; i < app->debug.breakpoint_count; ++i) {
        if (app->debug.breakpoints[i] == address) {
            return true;
        }
    }
    return false;
}

static bool app_debug_reserve_breakpoints(AppState *app, size_t required_count) {
    uint16_t *new_breakpoints;
    size_t new_capacity;

    if (required_count <= app->debug.breakpoint_capacity) {
        return true;
    }

    new_capacity = app->debug.breakpoint_capacity > 0 ? app->debug.breakpoint_capacity : 16;
    while (new_capacity < required_count) {
        if (new_capacity > (SIZE_MAX / 2)) {
            new_capacity = required_count;
            break;
        }
        new_capacity *= 2;
    }

    new_breakpoints = (uint16_t *)realloc(app->debug.breakpoints, new_capacity * sizeof(app->debug.breakpoints[0]));
    if (new_breakpoints == NULL) {
        return false;
    }

    app->debug.breakpoints = new_breakpoints;
    app->debug.breakpoint_capacity = new_capacity;
    return true;
}

static bool app_debug_has_watchpoint(const AppState *app, uint16_t address) {
    for (size_t i = 0; i < app->debug.watchpoint_count; ++i) {
        if (app->debug.watchpoints[i] == address) {
            return true;
        }
    }
    return false;
}

static bool app_debug_reserve_watchpoints(AppState *app, size_t required_count) {
    uint16_t *new_watchpoints;
    size_t new_capacity;

    if (required_count <= app->debug.watchpoint_capacity) {
        return true;
    }

    new_capacity = app->debug.watchpoint_capacity > 0 ? app->debug.watchpoint_capacity : 16;
    while (new_capacity < required_count) {
        if (new_capacity > (SIZE_MAX / 2)) {
            new_capacity = required_count;
            break;
        }
        new_capacity *= 2;
    }

    new_watchpoints = (uint16_t *)realloc(app->debug.watchpoints, new_capacity * sizeof(app->debug.watchpoints[0]));
    if (new_watchpoints == NULL) {
        return false;
    }

    app->debug.watchpoints = new_watchpoints;
    app->debug.watchpoint_capacity = new_capacity;
    return true;
}

static DWORD_PTR app_debug_make_point_item_data(bool watchpoint, uint16_t address) {
    return (DWORD_PTR)((watchpoint ? 0x10000u : 0u) | address);
}

static bool app_debug_get_selected_point(AppState *app, bool *watchpoint, uint16_t *address) {
    int selection;
    LRESULT item_data;

    if (app == NULL || app->debug.debugger_points_list == NULL) {
        return false;
    }

    selection = (int)SendMessageA(app->debug.debugger_points_list, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) {
        return false;
    }

    item_data = SendMessageA(app->debug.debugger_points_list, LB_GETITEMDATA, (WPARAM)selection, 0);
    if (item_data == LB_ERR) {
        return false;
    }

    if (watchpoint != NULL) {
        *watchpoint = (((DWORD_PTR)item_data >> 16) & 0x1u) != 0;
    }
    if (address != NULL) {
        *address = (uint16_t)((DWORD_PTR)item_data & 0xFFFFu);
    }
    return true;
}

/* Adds or removes one debugger breakpoint at an absolute 16-bit address. */
static bool app_debug_toggle_breakpoint(AppState *app, uint16_t address, bool *enabled) {
    for (size_t i = 0; i < app->debug.breakpoint_count; ++i) {
        if (app->debug.breakpoints[i] == address) {
            memmove(
                &app->debug.breakpoints[i],
                &app->debug.breakpoints[i + 1],
                (app->debug.breakpoint_count - i - 1) * sizeof(app->debug.breakpoints[0])
            );
            app->debug.breakpoint_count--;
            if (enabled != NULL) {
                *enabled = false;
            }
            return true;
        }
    }

    if (!app_debug_reserve_breakpoints(app, app->debug.breakpoint_count + 1)) {
        return false;
    }

    app->debug.breakpoints[app->debug.breakpoint_count++] = address;
    if (enabled != NULL) {
        *enabled = true;
    }
    return true;
}

static bool app_debug_toggle_watchpoint(AppState *app, uint16_t address, bool *enabled) {
    for (size_t i = 0; i < app->debug.watchpoint_count; ++i) {
        if (app->debug.watchpoints[i] == address) {
            memmove(
                &app->debug.watchpoints[i],
                &app->debug.watchpoints[i + 1],
                (app->debug.watchpoint_count - i - 1) * sizeof(app->debug.watchpoints[0])
            );
            app->debug.watchpoint_count--;
            if (enabled != NULL) {
                *enabled = false;
            }
            return true;
        }
    }

    if (!app_debug_reserve_watchpoints(app, app->debug.watchpoint_count + 1)) {
        return false;
    }

    app->debug.watchpoints[app->debug.watchpoint_count++] = address;
    if (enabled != NULL) {
        *enabled = true;
    }
    return true;
}

static void app_debug_refresh_points_list(AppState *app) {
    bool had_selection = false;
    bool selected_watchpoint = false;
    uint16_t selected_address = 0;
    DWORD_PTR selected_data = 0;
    int restore_index = LB_ERR;

    if (app == NULL || app->debug.debugger_points_list == NULL) {
        return;
    }

    had_selection = app_debug_get_selected_point(app, &selected_watchpoint, &selected_address);
    if (had_selection) {
        selected_data = app_debug_make_point_item_data(selected_watchpoint, selected_address);
    }
    SendMessageA(app->debug.debugger_points_list, LB_RESETCONTENT, 0, 0);

    for (size_t i = 0; i < app->debug.breakpoint_count; ++i) {
        char item[32];
        snprintf(item, sizeof(item), "Exec  %04X", app->debug.breakpoints[i]);
        const LRESULT index = SendMessageA(app->debug.debugger_points_list, LB_ADDSTRING, 0, (LPARAM)item);
        if (index != LB_ERR && index != LB_ERRSPACE) {
            SendMessageA(
                app->debug.debugger_points_list,
                LB_SETITEMDATA,
                (WPARAM)index,
                (LPARAM)app_debug_make_point_item_data(false, app->debug.breakpoints[i])
            );
            if (had_selection && selected_data == app_debug_make_point_item_data(false, app->debug.breakpoints[i])) {
                restore_index = (int)index;
            }
        }
    }
    for (size_t i = 0; i < app->debug.watchpoint_count; ++i) {
        char item[32];
        snprintf(item, sizeof(item), "Write %04X", app->debug.watchpoints[i]);
        const LRESULT index = SendMessageA(app->debug.debugger_points_list, LB_ADDSTRING, 0, (LPARAM)item);
        if (index != LB_ERR && index != LB_ERRSPACE) {
            SendMessageA(
                app->debug.debugger_points_list,
                LB_SETITEMDATA,
                (WPARAM)index,
                (LPARAM)app_debug_make_point_item_data(true, app->debug.watchpoints[i])
            );
            if (had_selection && selected_data == app_debug_make_point_item_data(true, app->debug.watchpoints[i])) {
                restore_index = (int)index;
            }
        }
    }

    if (restore_index != LB_ERR) {
        SendMessageA(app->debug.debugger_points_list, LB_SETCURSEL, (WPARAM)restore_index, 0);
    } else if (had_selection) {
        const size_t total_count = app->debug.breakpoint_count + app->debug.watchpoint_count;
        if (total_count > 0) {
            SendMessageA(app->debug.debugger_points_list, LB_SETCURSEL, (WPARAM)(total_count - 1), 0);
        }
    }
}

static bool app_debug_remove_selected_point(AppState *app) {
    bool watchpoint = false;
    uint16_t address = 0;

    if (!app_debug_get_selected_point(app, &watchpoint, &address)) {
        return false;
    }

    if (watchpoint) {
        for (size_t i = 0; i < app->debug.watchpoint_count; ++i) {
            if (app->debug.watchpoints[i] == address) {
                memmove(
                    &app->debug.watchpoints[i],
                    &app->debug.watchpoints[i + 1],
                    (app->debug.watchpoint_count - i - 1) * sizeof(app->debug.watchpoints[0])
                );
                app->debug.watchpoint_count--;
                return true;
            }
        }
    } else {
        for (size_t i = 0; i < app->debug.breakpoint_count; ++i) {
            if (app->debug.breakpoints[i] == address) {
                memmove(
                    &app->debug.breakpoints[i],
                    &app->debug.breakpoints[i + 1],
                    (app->debug.breakpoint_count - i - 1) * sizeof(app->debug.breakpoints[0])
                );
                app->debug.breakpoint_count--;
                return true;
            }
        }
    }

    return false;
}

static void app_debug_free_storage(AppState *app) {
    if (app == NULL) {
        return;
    }
    free(app->debug.breakpoints);
    app->debug.breakpoints = NULL;
    app->debug.breakpoint_count = 0;
    app->debug.breakpoint_capacity = 0;
    free(app->debug.watchpoints);
    app->debug.watchpoints = NULL;
    app->debug.watchpoint_count = 0;
    app->debug.watchpoint_capacity = 0;
}

static void app_debug_append_point_summary(char *buffer, size_t buffer_size, size_t *used, const AppState *app) {
    app_append_textf(buffer, buffer_size, used, "Exec breakpoints:");
    if (app->debug.breakpoint_count == 0) {
        app_append_textf(buffer, buffer_size, used, " none");
    } else {
        for (size_t i = 0; i < app->debug.breakpoint_count; ++i) {
            app_append_textf(buffer, buffer_size, used, " %04X", app->debug.breakpoints[i]);
        }
    }

    app_append_textf(buffer, buffer_size, used, "\r\nWrite watchpoints:");
    if (app->debug.watchpoint_count == 0) {
        app_append_textf(buffer, buffer_size, used, " none");
    } else {
        for (size_t i = 0; i < app->debug.watchpoint_count; ++i) {
            app_append_textf(buffer, buffer_size, used, " %04X", app->debug.watchpoints[i]);
        }
    }
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
    if (!app_parse_number(text, &value) || value < 0 || value > 0xFFFF) {
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

static bool app_debug_is_step_over_candidate(const Spectrum *spec, uint16_t address) {
    const uint8_t op = app_debug_mem_rd(spec, address);

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
        default:
            return (op & 0xC7u) == 0xC7u;
    }
}

/* Renders registers, flags, paging state, disassembly, and nearby memory into
   the debugger's read-only text view. */
static void app_debug_refresh_window(AppState *app) {
    char stack_buffer[8192];
    char *buffer = stack_buffer;
    size_t buffer_size = sizeof(stack_buffer);
    char flags[16];
    char ascii[17];
    size_t used = 0;
    uint16_t addr;
    uint16_t disassembly_addresses[APP_DEBUG_DISASSEMBLY_LINE_COUNT];
    uint16_t memory_addresses[6];
    uint16_t stack_addresses[4];
    z80_t *cpu;
    const size_t total_points = app->debug.breakpoint_count + app->debug.watchpoint_count;

    if (total_points > 0 && total_points <= (SIZE_MAX - 12288u) / 24u) {
        buffer_size = 12288u + (total_points * 24u);
        buffer = (char *)malloc(buffer_size);
        if (buffer == NULL) {
            buffer = stack_buffer;
            buffer_size = sizeof(stack_buffer);
        }
    }
    buffer[0] = '\0';

    if (app->debug.debugger_text == NULL ||
        app->debug.debugger_registers_text == NULL ||
        app->debug.debugger_memory_text == NULL ||
        app->debug.debugger_stack_text == NULL) {
        if (buffer != stack_buffer) {
            free(buffer);
        }
        return;
    }
    if (!app->spec.machine_ready) {
        app_append_textf(
            buffer,
            buffer_size,
            &used,
            "Machine not ready.\r\nDebugger actions are disabled until the machine is initialized.\r\n\r\n"
        );
        app_debug_append_point_summary(buffer, buffer_size, &used, app);
        app_debug_set_disassembly(app, buffer, NULL, 0);
        SetWindowTextA(app->debug.debugger_registers_text, buffer);
        app_debug_set_memory(app->debug.debugger_memory_text, buffer, NULL, NULL, 0);
        app_debug_set_memory(app->debug.debugger_stack_text, buffer, NULL, NULL, 0);
        app_debug_refresh_points_list(app);
        app_debug_update_controls(app);
        if (buffer != stack_buffer) {
            free(buffer);
        }
        return;
    }

    cpu = &app->spec.machine.cpu;
    if (!app->debug.debugger_view_initialized) {
        app_debug_set_view_address(app, cpu->pc, true);
    } else if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = cpu->pc;
        app_debug_sync_view_controls(app);
    }
    app_debug_format_flags(cpu->f, flags, sizeof(flags));

    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "%-7s  Model:%s\r\n",
        app->debug.paused ? "PAUSED" : "RUNNING",
        app_model_name(app->spec.model)
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "PC:%04X  SP:%04X  IX:%04X  IY:%04X\r\n",
        cpu->pc,
        cpu->sp,
        cpu->ix,
        cpu->iy
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "AF:%04X  BC:%04X  DE:%04X  HL:%04X\r\n",
        cpu->af,
        cpu->bc,
        cpu->de,
        cpu->hl
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "AF':%04X BC':%04X DE':%04X HL':%04X\r\n",
        cpu->af2,
        cpu->bc2,
        cpu->de2,
        cpu->hl2
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "I:%02X R:%02X  IM:%u  IFF:%u/%u  FE:%02X\r\n"
        "Flags:%s\r\n"
        "Page:7FFD:%02X  Bank:%u  Lock:%s\r\n",
        cpu->i,
        cpu->r,
        (unsigned)cpu->im,
        cpu->iff1 ? 1u : 0u,
        cpu->iff2 ? 1u : 0u,
        app->spec.machine.last_fe_out,
        flags,
        app->spec.machine.last_mem_config,
        (unsigned)app->spec.machine.display_ram_bank,
        app->spec.machine.memory_paging_disabled ? "Yes" : "No"
    );
    if (app->debug.watchpoint_hit) {
        app_append_textf(buffer, buffer_size, &used, "Watch:%04X <= %02X\r\n",
            app->debug.last_watchpoint_address, app->debug.last_watchpoint_value);
    } else if (app->debug.breakpoint_hit) {
        app_append_textf(buffer, buffer_size, &used, "Breakpoint:%04X\r\n", app->debug.last_breakpoint_address);
    } else if (app->debug.debugger_run_to_hit) {
        app_append_textf(buffer, buffer_size, &used, "Run-to hit:%04X\r\n", app->debug.last_run_to_address);
    } else if (app->debug.debugger_run_to_active) {
        app_append_textf(buffer, buffer_size, &used, "Run-to:%04X\r\n", app->debug.debugger_run_to_address);
    }
    SetWindowTextA(app->debug.debugger_registers_text, buffer);
    used = 0;
    buffer[0] = '\0';

    addr = app->debug.debugger_view_address;
    for (int i = 0; i < APP_DEBUG_DISASSEMBLY_LINE_COUNT; ++i) {
        char mnemonic[128];
        char bytes[32];
        uint8_t len;
        size_t byte_used = 0;
        disassembly_addresses[i] = addr;

        app_debug_decode_instruction(&app->spec, addr, mnemonic, sizeof(mnemonic), &len);
        for (uint8_t byte_index = 0; byte_index < len; ++byte_index) {
            byte_used += (size_t)snprintf(
                bytes + byte_used,
                sizeof(bytes) - byte_used,
                "%02X ",
                app_debug_mem_rd(&app->spec, (uint16_t)(addr + byte_index))
            );
            if (byte_used >= sizeof(bytes)) {
                byte_used = sizeof(bytes) - 1;
                break;
            }
        }
        bytes[byte_used] = '\0';
        app_append_textf(
            buffer,
            buffer_size,
            &used,
            "%-12s %s\r\n",
            bytes,
            mnemonic
        );
        addr = (uint16_t)(addr + len);
    }

    app_debug_set_disassembly(
        app,
        buffer,
        disassembly_addresses,
        APP_DEBUG_DISASSEMBLY_LINE_COUNT
    );
    used = 0;
    buffer[0] = '\0';

    for (int row = 0; row < 4; ++row) {
        uint16_t row_addr = (uint16_t)(cpu->sp + (uint16_t)(row * 8));
        stack_addresses[row] = row_addr;
        for (int col = 0; col < 8; ++col) {
            app_append_textf(
                buffer,
                buffer_size,
                &used,
                " %02X",
                app_debug_mem_rd(&app->spec, (uint16_t)(row_addr + col))
            );
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
            const uint8_t value = app_debug_mem_rd(&app->spec, (uint16_t)(row_addr + col));
            app_append_textf(
                buffer,
                buffer_size,
                &used,
                " %02X",
                value
            );
            ascii[col] = (value >= 0x20 && value <= 0x7E) ? (char)value : '.';
        }
        ascii[16] = '\0';
        app_append_textf(buffer, buffer_size, &used, "  |%s|\r\n", ascii);
    }

    app_debug_set_memory(
        app->debug.debugger_memory_text,
        buffer,
        memory_addresses,
        NULL,
        6
    );
    app_debug_refresh_points_list(app);
    app_debug_update_controls(app);
    if (buffer != stack_buffer) {
        free(buffer);
    }
}

/* Stops the temporary stepping run after exactly one completed Z80
   instruction and remembers the last pin state for future debugging work. */
static void app_debug_callback(void *user_data, uint64_t pins) {
    AppState *app = (AppState *)user_data;

    if (app == NULL) {
        return;
    }
    app->debug.last_pins = pins;
    if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
        const uint16_t addr = Z80_GET_ADDR(pins);
        if (app_debug_has_watchpoint(app, addr)) {
            app->debug.debugger_watchpoint_pending = true;
            app->debug.pending_watchpoint_address = addr;
            app->debug.pending_watchpoint_value = Z80_GET_DATA(pins);
        }
    }
    if (z80_opdone(&app->spec.machine.cpu)) {
        uint16_t next_pc = app->spec.machine.cpu.pc;
        bool reached_run_to = app->debug.debugger_run_to_active && app->debug.debugger_run_to_address == next_pc;
        bool skip_breakpoint = app->debug.debugger_skip_breakpoint_once && app->debug.debugger_skip_breakpoint_address == next_pc;
        if (skip_breakpoint) {
            app->debug.debugger_skip_breakpoint_once = false;
        }
        if (app->debug.debugger_watchpoint_pending) {
            app->debug.watchpoint_hit = true;
            app->debug.last_watchpoint_address = app->debug.pending_watchpoint_address;
            app->debug.last_watchpoint_value = app->debug.pending_watchpoint_value;
            app->debug.debugger_watchpoint_pending = false;
            app->debug.debugger_run_to_active = false;
            app->debug.stop_requested = true;
            return;
        }
        if (!skip_breakpoint && app_debug_has_breakpoint(app, next_pc)) {
            app->debug.breakpoint_hit = true;
            app->debug.last_breakpoint_address = next_pc;
            app->debug.debugger_run_to_active = false;
            app->debug.stop_requested = true;
        }
        if (reached_run_to) {
            app->debug.debugger_run_to_active = false;
            app->debug.debugger_run_to_hit = true;
            app->debug.last_run_to_address = next_pc;
            app->debug.stop_requested = true;
        }
        if (app->debug.stepping) {
            app->debug.stop_requested = true;
        }
    }
}

static void app_debug_resume(AppState *app) {
    if (!app->spec.machine_ready) {
        return;
    }

    app->debug.paused = false;
    app->debug.stepping = false;
    app->debug.stop_requested = false;
    app->debug.breakpoint_hit = false;
    app->debug.watchpoint_hit = false;
    app->debug.debugger_run_to_hit = false;
    app->debug.debugger_watchpoint_pending = false;
    if (app_debug_has_breakpoint(app, app->spec.machine.cpu.pc)) {
        app->debug.debugger_skip_breakpoint_once = true;
        app->debug.debugger_skip_breakpoint_address = app->spec.machine.cpu.pc;
    } else {
        app->debug.debugger_skip_breakpoint_once = false;
    }
}

/* Executes exactly one instruction from the current paused machine state and
   then refreshes both the display and debugger snapshot text. */
static void app_debug_step_instruction(AppState *app) {
    uint32_t step_us;

    if (!app->spec.machine_ready) {
        return;
    }

    app->debug.paused = true;
    app->debug.stepping = true;
    app->debug.stop_requested = false;
    app->debug.breakpoint_hit = false;
    app->debug.watchpoint_hit = false;
    app->debug.debugger_run_to_active = false;
    app->debug.debugger_run_to_hit = false;
    app->debug.debugger_watchpoint_pending = false;
    app->spec.machine.debug.callback.func = app_debug_callback;
    app->spec.machine.debug.callback.user_data = app;
    app->spec.machine.debug.stopped = &app->debug.stop_requested;
    step_us = (uint32_t)(app_model_frame_duration_ms(app->spec.model) * 1000.0 + 0.5);
    zx_exec(&app->spec.machine, step_us);
    app->spec.machine.debug.callback.func = NULL;
    app->spec.machine.debug.callback.user_data = NULL;
    app->spec.machine.debug.stopped = NULL;
    app->debug.stepping = false;
    spectrum_render_frame(&app->spec);
    InvalidateRect(app->main_hwnd, NULL, FALSE);
    app_debug_refresh_window(app);
}

static void app_debug_step_over_instruction(AppState *app) {
    uint8_t len;
    uint16_t pc;

    if (!app->spec.machine_ready) {
        return;
    }

    pc = app->spec.machine.cpu.pc;
    app_debug_decode_instruction(&app->spec, pc, NULL, 0, &len);
    if (!app_debug_is_step_over_candidate(&app->spec, pc)) {
        app_debug_step_instruction(app);
        return;
    }

    app_debug_resume(app);
    app->debug.debugger_run_to_active = true;
    app->debug.debugger_run_to_address = (uint16_t)(pc + len);
    app->debug.debugger_run_to_hit = false;
    app->debug.debugger_skip_breakpoint_once = true;
    app->debug.debugger_skip_breakpoint_address = pc;
    if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = pc;
        app_debug_sync_view_controls(app);
    }
    app_debug_refresh_window(app);
}

static void app_debug_navigate_view(AppState *app, int delta) {
    uint16_t address;

    if (app == NULL || !app->spec.machine_ready) {
        return;
    }

    address = (uint16_t)(app->debug.debugger_view_address + delta);
    app_debug_set_view_address(app, address, false);
    app_debug_refresh_window(app);
}

static void app_debug_view_pc(AppState *app) {
    if (app == NULL || !app->spec.machine_ready) {
        return;
    }
    app_debug_set_view_address(app, app->spec.machine.cpu.pc, true);
    app_debug_refresh_window(app);
}

static void app_debug_view_sp(AppState *app) {
    if (app == NULL || !app->spec.machine_ready) {
        return;
    }
    app_debug_set_view_address(app, app->spec.machine.cpu.sp, false);
    app_debug_refresh_window(app);
}

static void app_debug_run_to_address(AppState *app, uint16_t address) {
    if (!app->spec.machine_ready) {
        return;
    }

    if (address == app->spec.machine.cpu.pc) {
        app->debug.paused = true;
        app->debug.breakpoint_hit = false;
        app->debug.watchpoint_hit = false;
        app->debug.debugger_run_to_active = false;
        app->debug.debugger_run_to_hit = true;
        app->debug.last_run_to_address = address;
        if (app->debug.debugger_view_sync_pc) {
            app->debug.debugger_view_address = address;
            app_debug_sync_view_controls(app);
        }
        InvalidateRect(app->main_hwnd, NULL, FALSE);
        app_debug_refresh_window(app);
        return;
    }

    app_debug_resume(app);
    app->debug.debugger_run_to_active = true;
    app->debug.debugger_run_to_address = address;
    app->debug.last_run_to_address = address;
    if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = app->spec.machine.cpu.pc;
        app_debug_sync_view_controls(app);
    }
    InvalidateRect(app->main_hwnd, NULL, FALSE);
    app_debug_refresh_window(app);
}

/* Runs one model-accurate frame slice with debugger breakpoints armed, then
   pauses and refreshes the debugger if any enabled address was reached. */
static void app_debug_run_frame(AppState *app, bool render_frame) {
    uint32_t frame_us;
    const bool debugger_active = app->debug.debugger_hwnd != NULL;

    if (!app->spec.machine_ready) {
        return;
    }

    if (debugger_active &&
        (
        app->debug.debugger_skip_breakpoint_once &&
        app->debug.debugger_skip_breakpoint_address == app->spec.machine.cpu.pc
    )) {
        app->debug.debugger_skip_breakpoint_once = false;
    } else if (debugger_active && app_debug_has_breakpoint(app, app->spec.machine.cpu.pc)) {
        app->debug.breakpoint_hit = true;
        app->debug.last_breakpoint_address = app->spec.machine.cpu.pc;
        app->debug.debugger_run_to_active = false;
        app->debug.debugger_watchpoint_pending = false;
        app->debug.paused = true;
        if (app->debug.debugger_view_sync_pc) {
            app->debug.debugger_view_address = app->debug.last_breakpoint_address;
            app_debug_sync_view_controls(app);
        }
        spectrum_render_frame(&app->spec);
        InvalidateRect(app->main_hwnd, NULL, FALSE);
        app_debug_refresh_window(app);
        return;
    }

    app->debug.stop_requested = false;
    app->debug.breakpoint_hit = false;
    app->debug.watchpoint_hit = false;
    app->debug.debugger_run_to_hit = false;
    if (debugger_active &&
        (app->debug.breakpoint_count > 0 || app->debug.watchpoint_count > 0 || app->debug.debugger_run_to_active)) {
        app->spec.machine.debug.callback.func = app_debug_callback;
        app->spec.machine.debug.callback.user_data = app;
        app->spec.machine.debug.stopped = &app->debug.stop_requested;
    }
    frame_us = (uint32_t)(app_model_frame_duration_ms(app->spec.model) * 1000.0 + 0.5);
    zx_exec(&app->spec.machine, frame_us);
    if (debugger_active &&
        (app->debug.breakpoint_count > 0 ||
         app->debug.watchpoint_count > 0 ||
         app->debug.debugger_run_to_active ||
         app->debug.debugger_run_to_hit)) {
        app->spec.machine.debug.callback.func = NULL;
        app->spec.machine.debug.callback.user_data = NULL;
        app->spec.machine.debug.stopped = NULL;
    }
    if (render_frame) {
        spectrum_render_frame(&app->spec);
    }

    if (app->debug.breakpoint_hit || app->debug.watchpoint_hit || app->debug.debugger_run_to_hit) {
        app->debug.paused = true;
        if (app->debug.debugger_view_sync_pc) {
            app->debug.debugger_view_address = app->spec.machine.cpu.pc;
            app_debug_sync_view_controls(app);
        }
        InvalidateRect(app->main_hwnd, NULL, FALSE);
        app_debug_refresh_window(app);
    }
}

/* Jumps execution to an explicit address from the debugger and resumes the
   machine without relying on a BASIC-side entry command. */
static void app_debug_run_from_address(AppState *app, uint16_t address) {
    if (!app->spec.machine_ready) {
        return;
    }

    app->spec.machine.pins = z80_prefetch(&app->spec.machine.cpu, address);
    app_debug_resume(app);
    app->debug.debugger_run_to_active = false;
    app->debug.debugger_skip_breakpoint_once = true;
    app->debug.debugger_skip_breakpoint_address = address;
    if (app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = address;
        app_debug_sync_view_controls(app);
    }
    InvalidateRect(app->main_hwnd, NULL, FALSE);
    app_debug_refresh_window(app);
}

/* Clears any stale stepping hook after resets, model swaps, or snapshot loads
   so later debug actions start from a clean machine configuration. */
static void app_debug_machine_changed(AppState *app) {
    app->debug.stop_requested = false;
    app->debug.stepping = false;
    app->debug.breakpoint_hit = false;
    app->debug.watchpoint_hit = false;
    app->debug.debugger_run_to_active = false;
    app->debug.debugger_run_to_hit = false;
    app->debug.debugger_watchpoint_pending = false;
    app->debug.debugger_skip_breakpoint_once = false;
    app->spec.machine.debug.callback.func = NULL;
    app->spec.machine.debug.callback.user_data = NULL;
    app->spec.machine.debug.stopped = NULL;
    if (!app->debug.debugger_view_initialized) {
        app->debug.debugger_view_sync_pc = true;
        app->debug.debugger_view_initialized = true;
    }
    if (app->spec.machine_ready && app->debug.debugger_view_sync_pc) {
        app->debug.debugger_view_address = app->spec.machine.cpu.pc;
    }
    app_debug_sync_view_controls(app);
    app_debug_refresh_window(app);
    app_assembler_refresh_window(app);
    app_poke_refresh_window(app);
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
    const int content_y = 72;
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
            app->debug.debugger_accel = CreateAcceleratorTableA((LPACCEL)debugger_accels, (int)(sizeof(debugger_accels) / sizeof(debugger_accels[0])));
            SendMessageA(app->debug.debugger_address_edit, EM_SETLIMITTEXT, 15, 0);
            SendMessageA(app->debug.debugger_address_edit, WM_SETFONT, (WPARAM)font, TRUE);
            app_debug_configure_disassembly(app->debug.debugger_text);
            SendMessageA(app->debug.debugger_registers_text, WM_SETFONT, (WPARAM)font, TRUE);
            app_debug_configure_memory(app->debug.debugger_memory_text);
            app_debug_configure_memory(app->debug.debugger_stack_text);
            SendMessageA(app->debug.debugger_points_list, WM_SETFONT, (WPARAM)font, TRUE);
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

                if (control == app->debug.debugger_address_edit) {
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
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    if (app->debug.paused) {
                        app_debug_resume(app);
                    } else {
                        app->debug.paused = true;
                        app->debug.debugger_run_to_active = false;
                        app->debug.debugger_run_to_hit = false;
                        app->debug.breakpoint_hit = false;
                        app->debug.watchpoint_hit = false;
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                case APP_CTRL_DEBUG_STEP:
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    app_debug_step_instruction(app);
                    return TRUE;
                case APP_CTRL_DEBUG_STEP_OVER:
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    app_debug_step_over_instruction(app);
                    return TRUE;
                case APP_CTRL_DEBUG_REFRESH:
                    if (!app->spec.machine_ready) {
                        app_debug_refresh_window(app);
                        return TRUE;
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                case APP_CTRL_DEBUG_GO: {
                    char error[128];
                    if (!app->spec.machine_ready) {
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
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    app_debug_view_pc(app);
                    return TRUE;
                case APP_CTRL_DEBUG_VIEW_SP:
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    app_debug_view_sp(app);
                    return TRUE;
                case APP_CTRL_DEBUG_PAGE_UP:
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    app_debug_navigate_view(app, -256);
                    return TRUE;
                case APP_CTRL_DEBUG_PAGE_DOWN:
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    app_debug_navigate_view(app, 256);
                    return TRUE;
                case APP_CTRL_DEBUG_RUN_TO: {
                    char error[128];
                    uint16_t address;
                    if (!app->spec.machine_ready) {
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
                    bool enabled;
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    if (!app_debug_toggle_breakpoint(app, address, &enabled)) {
                        MessageBoxA(hwnd, "Could not update the breakpoint list.", "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_WATCHPOINT_TOGGLE: {
                    char error[128];
                    uint16_t address;
                    bool enabled;
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    if (!app_debug_toggle_watchpoint(app, address, &enabled)) {
                        MessageBoxA(hwnd, "Could not update the watchpoint list.", "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                }
                case APP_CTRL_DEBUG_REMOVE_SELECTED:
                    if (!app->spec.machine_ready) {
                        return TRUE;
                    }
                    if (!app_debug_remove_selected_point(app)) {
                        MessageBoxA(hwnd, "Select a breakpoint or watchpoint first.", "Debugger", MB_OK | MB_ICONERROR);
                        return TRUE;
                    }
                    app_debug_refresh_window(app);
                    return TRUE;
                case APP_CTRL_DEBUG_RUN_AT: {
                    char error[128];
                    uint16_t address;
                    if (!app->spec.machine_ready) {
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
                        if (sync_to_pc && app->spec.machine_ready) {
                            app_debug_set_view_address(app, app->spec.machine.cpu.pc, true);
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
                        if (!app->spec.machine_ready) {
                            app_debug_update_controls(app);
                            return TRUE;
                        }
                        uint16_t address = 0;
                        if (app_debug_get_selected_point(app, NULL, &address)) {
                            app_debug_set_view_address(app, address, false);
                            app_debug_refresh_window(app);
                        } else {
                            app_debug_update_controls(app);
                        }
                        return TRUE;
                    }
                    if (HIWORD(wparam) == LBN_DBLCLK) {
                        if (!app->spec.machine_ready) {
                            return TRUE;
                        }
                        if (app_debug_remove_selected_point(app)) {
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
        520,
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
        "  F10 Step over CALLs\r\n"
        "  Refresh redraws the current register and memory view.\r\n"
        "\r\n"
        "Address field:\r\n"
        "  - Enter a number then click Go or press Enter to move the view.\r\n"
        "  - Ctrl+Enter runs until the typed address.\r\n"
        "  - Shift+Enter starts execution from the typed address.\r\n"
        "  - Numbers use the assembler syntax: decimal, $hex, 0xhex, H suffix, or %binary.\r\n"
        "\r\n"
        "Navigation:\r\n"
        "  Ctrl+P jumps the view to PC.\r\n"
        "  Ctrl+S jumps the view to SP.\r\n"
        "  PgUp/PgDn pages the disassembly and memory view by 256 bytes.\r\n"
        "  Ctrl+Home and Ctrl+End jump directly to PC and SP from the address box.\r\n"
        "\r\n"
        "Breakpoints and watchpoints:\r\n"
        "  F9 toggles a breakpoint at the typed address.\r\n"
        "  Ctrl+W toggles a write watchpoint at the typed address.\r\n"
        "  The list on the right shows all active breakpoints and watchpoints.\r\n"
        "  Double-click a list entry to remove it, or use Remove Sel.\r\n"
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

