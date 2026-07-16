/* Split from main.c: poke tool implementation. Included by main.c. */

static LRESULT CALLBACK app_poke_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *create = (CREATESTRUCTA *)lparam;
            app = (AppState *)create->lpCreateParams;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            if (app != NULL) {
                app->debug.poke_hwnd = hwnd;
            }
            return TRUE;
        }
        case WM_CREATE: {
            RECT rect;

            if (app == NULL) {
                return -1;
            }
            app->debug.poke_panel = CreateDialogParamA(
                GetModuleHandleA(NULL),
                MAKEINTRESOURCEA(APP_DIALOG_POKE),
                hwnd,
                app_poke_dlgproc,
                (LPARAM)app
            );
            if (app->debug.poke_panel == NULL) {
                return -1;
            }
            GetClientRect(hwnd, &rect);
            MoveWindow(
                app->debug.poke_panel,
                0,
                0,
                rect.right - rect.left,
                rect.bottom - rect.top,
                TRUE
            );
            return 0;
        }
        case WM_SIZE:
            if (app != NULL && app->debug.poke_panel != NULL) {
                MoveWindow(app->debug.poke_panel, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
            }
            return 0;
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
                app->debug.poke_hwnd = NULL;
                app->debug.poke_panel = NULL;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static INT_PTR CALLBACK app_poke_dlgproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_INITDIALOG: {
            app = (AppState *)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            if (app == NULL) {
                return FALSE;
            }
            app->debug.poke_background_brush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            app->debug.poke_input_brush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
            app->debug.poke_address_edit = GetDlgItem(hwnd, APP_CTRL_POKE_ADDRESS);
            app->debug.poke_values_edit = GetDlgItem(hwnd, APP_CTRL_POKE_VALUES);
            app->debug.poke_apply_button = GetDlgItem(hwnd, APP_CTRL_POKE_APPLY);
            app->debug.poke_status = GetDlgItem(hwnd, APP_CTRL_POKE_STATUS);

            SendMessageA(app->debug.poke_address_edit, EM_SETLIMITTEXT, 15, 0);
            SendMessageA(app->debug.poke_values_edit, EM_SETLIMITTEXT, 2047, 0);
            app_apply_flat_control_style(hwnd);
            app_poke_layout_controls(app, hwnd);
            app_poke_refresh_window(app);
            return FALSE;
        }
        case WM_SIZE:
            if (app != NULL) {
                app_poke_layout_controls(app, hwnd);
            }
            return TRUE;
        case WM_ENTERSIZEMOVE:
        case WM_EXITSIZEMOVE:
        case WM_TIMER:
            break;
        case WM_ERASEBKGND:
            if (app != NULL && app->debug.poke_background_brush != NULL) {
                RECT rect;
                GetClientRect(hwnd, &rect);
                FillRect((HDC)wparam, &rect, app->debug.poke_background_brush);
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, 1);
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT:
            if (app != NULL &&
                (((HWND)lparam == app->debug.poke_address_edit) ||
                 ((HWND)lparam == app->debug.poke_values_edit))) {
                HDC hdc = (HDC)wparam;
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)(app->debug.poke_input_brush != NULL
                    ? app->debug.poke_input_brush
                    : GetSysColorBrush(COLOR_WINDOW)));
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (app != NULL) {
                HDC hdc = (HDC)wparam;
                HWND control = (HWND)lparam;
                if ((control == app->debug.poke_address_edit || control == app->debug.poke_values_edit) &&
                    IsWindowEnabled(control)) {
                    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                    SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
                    SetBkMode(hdc, OPAQUE);
                    SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)(app->debug.poke_input_brush != NULL
                        ? app->debug.poke_input_brush
                        : GetSysColorBrush(COLOR_WINDOW)));
                    return TRUE;
                }
                SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
                SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
                SetBkMode(hdc, TRANSPARENT);
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)(app->debug.poke_background_brush != NULL
                    ? app->debug.poke_background_brush
                    : GetSysColorBrush(COLOR_BTNFACE)));
                return TRUE;
            }
            break;
        case WM_DRAWITEM:
            if (app_draw_flat_button((const DRAWITEMSTRUCT *)lparam)) {
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (app == NULL) {
                break;
            }
            if (LOWORD(wparam) == APP_CTRL_POKE_APPLY) {
                app_poke_apply_values(hwnd, app);
                return TRUE;
            }
            if ((LOWORD(wparam) == APP_CTRL_POKE_ADDRESS || LOWORD(wparam) == APP_CTRL_POKE_VALUES) &&
                HIWORD(wparam) == EN_UPDATE) {
                app_poke_set_status(app, "");
                return TRUE;
            }
            break;
        case WM_CLOSE:
            return TRUE;
        case WM_DESTROY:
            if (app != NULL) {
                if (app->debug.poke_background_brush != NULL) {
                    DeleteObject(app->debug.poke_background_brush);
                    app->debug.poke_background_brush = NULL;
                }
                if (app->debug.poke_input_brush != NULL) {
                    DeleteObject(app->debug.poke_input_brush);
                    app->debug.poke_input_brush = NULL;
                }
                app->debug.poke_panel = NULL;
                app->debug.poke_address_edit = NULL;
                app->debug.poke_values_edit = NULL;
                app->debug.poke_apply_button = NULL;
                app->debug.poke_status = NULL;
            }
            return TRUE;
        default:
            break;
    }
    return FALSE;
}


static void app_open_poke_window(AppState *app) {
    if (app->debug.poke_hwnd != NULL) {
        ShowWindow(app->debug.poke_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(app->debug.poke_hwnd);
        app_poke_refresh_window(app);
        return;
    }

    app->debug.poke_hwnd = CreateWindowExA(
        WS_EX_CONTROLPARENT,
        "ZXSpecPokeWindow",
        "Spectrum Poke",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        230,
        175,
        app->main_hwnd,
        NULL,
        GetModuleHandleA(NULL),
        app
    );
    if (app->debug.poke_hwnd != NULL) {
        app_apply_window_icons(app->debug.poke_hwnd);
    }
}


static void app_poke_set_status(AppState *app, const char *text) {
    if (app == NULL || app->debug.poke_hwnd == NULL) {
        return;
    }

    SetWindowTextA(app->debug.poke_hwnd, "Spectrum Poke");
    if (app->debug.poke_status != NULL) {
        SetWindowTextA(app->debug.poke_status, text != NULL ? text : "");
    }
}

/* Lays out the compact poke controls inside their tool window. */
static void app_poke_layout_controls(AppState *app, HWND hwnd) {
    RECT rect;
    int width;
    const int edge = 10;
    const int label_h = 16;
    const int edit_h = 18;
    const int row_gap = 28;
    const int button_h = 24;
    const int button_w = 96;
    const int status_y = 66;
    const int status_h = 34;

    if (app == NULL || hwnd == NULL || app->debug.poke_address_edit == NULL || app->debug.poke_values_edit == NULL) {
        return;
    }

    GetClientRect(hwnd, &rect);
    width = rect.right - rect.left;

    MoveWindow(GetDlgItem(hwnd, APP_CTRL_POKE_ADDRESS_LABEL), edge, edge, 80, label_h, TRUE);
    MoveWindow(app->debug.poke_address_edit, edge + 80, edge - 2, width - (edge * 2) - 80, edit_h, TRUE);
    MoveWindow(GetDlgItem(hwnd, APP_CTRL_POKE_VALUES_LABEL), edge, edge + row_gap, 80, label_h, TRUE);
    MoveWindow(app->debug.poke_values_edit, edge + 80, edge + row_gap - 2, width - (edge * 2) - 80, edit_h, TRUE);
    if (app->debug.poke_status != NULL) {
        MoveWindow(app->debug.poke_status, edge, status_y, width - (edge * 2), status_h, TRUE);
    }
    MoveWindow(
        app->debug.poke_apply_button,
        width - edge - button_w,
        rect.bottom - edge - button_h,
        button_w,
        button_h,
        TRUE
    );
}

/* Refreshes enabled state and default status text for the poke tool. */
static void app_poke_refresh_window(AppState *app) {
    BOOL ready;
    char address_text[16];

    if (app == NULL || app->debug.poke_hwnd == NULL) {
        return;
    }

    ready = app->spec.machine_ready ? TRUE : FALSE;
    if (app->debug.poke_address_edit != NULL) {
        EnableWindow(app->debug.poke_address_edit, ready);
        if (ready && GetWindowTextLengthA(app->debug.poke_address_edit) == 0) {
            snprintf(address_text, sizeof(address_text), "%04Xh", app->spec.machine.cpu.pc);
            SetWindowTextA(app->debug.poke_address_edit, address_text);
        }
    }
    if (app->debug.poke_values_edit != NULL) {
        EnableWindow(app->debug.poke_values_edit, ready);
    }
    if (app->debug.poke_apply_button != NULL) {
        EnableWindow(app->debug.poke_apply_button, ready);
    }
    app_poke_set_status(app, ready
        ? "Enter a RAM address and one or more comma-separated byte values."
        : "Poke is unavailable until a machine is ready.");
}

/* Parses the poke fields and writes one or more bytes into Spectrum RAM. */
static void app_poke_apply_values(HWND hwnd, AppState *app) {
    char address_text[64];
    int values_length;
    char *values_text;
    char *cursor;
    char operand[128];
    uint8_t *bytes = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int address_value;
    int operand_result;
    uint16_t address;
    char status[512];

    if (app == NULL || !app->spec.machine_ready) {
        app_poke_set_status(app, "Poke is unavailable until a machine is ready.");
        return;
    }
    if (app->debug.poke_address_edit == NULL || app->debug.poke_values_edit == NULL) {
        app_poke_set_status(app, "Poke controls are not ready.");
        return;
    }

    GetWindowTextA(app->debug.poke_address_edit, address_text, (int)sizeof(address_text));
    if (!app_parse_number(address_text, &address_value) || address_value < 0x4000 || address_value > 0xFFFF) {
        snprintf(status, sizeof(status), "Invalid RAM address: %s", address_text);
        app_poke_set_status(app, status);
        SetFocus(app->debug.poke_address_edit);
        return;
    }
    address = (uint16_t)address_value;

    values_length = GetWindowTextLengthA(app->debug.poke_values_edit);
    if (values_length <= 0) {
        app_poke_set_status(app, "Enter one or more byte values first.");
        SetFocus(app->debug.poke_values_edit);
        return;
    }

    values_text = (char *)malloc((size_t)values_length + 1);
    if (values_text == NULL) {
        app_poke_set_status(app, "Out of memory.");
        return;
    }
    GetWindowTextA(app->debug.poke_values_edit, values_text, values_length + 1);
    cursor = values_text;

    while ((operand_result = app_read_operand_strict(&cursor, operand, sizeof(operand), status, sizeof(status))) > 0) {
        int byte_value;
        uint8_t *new_bytes;

        if (!app_parse_number(operand, &byte_value) || byte_value < 0 || byte_value > 0xFF) {
            snprintf(status, sizeof(status), "Invalid byte value: %s", operand);
            free(bytes);
            free(values_text);
            app_poke_set_status(app, status);
            SetFocus(app->debug.poke_values_edit);
            return;
        }
        if (count + 1 > capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
            new_bytes = (uint8_t *)realloc(bytes, new_capacity);
            if (new_bytes == NULL) {
                free(bytes);
                free(values_text);
                app_poke_set_status(app, "Out of memory.");
                return;
            }
            bytes = new_bytes;
            capacity = new_capacity;
        }
        bytes[count++] = (uint8_t)byte_value;
    }

    if (operand_result < 0) {
        free(bytes);
        free(values_text);
        app_poke_set_status(app, status);
        SetFocus(app->debug.poke_values_edit);
        return;
    }
    if (count == 0) {
        free(values_text);
        app_poke_set_status(app, "Enter one or more byte values first.");
        SetFocus(app->debug.poke_values_edit);
        return;
    }
    if ((size_t)address + count > 0x10000u) {
        free(bytes);
        free(values_text);
        app_poke_set_status(app, "Poke values run past FFFFh.");
        SetFocus(app->debug.poke_values_edit);
        return;
    }

    mem_write_range(&app->spec.machine.mem, address, bytes, (uint32_t)count);
    InvalidateRect(app->main_hwnd, NULL, FALSE);
    app_debug_refresh_window(app);
    snprintf(
        status,
        sizeof(status),
        "Poked %zu byte%s at %04Xh.",
        count,
        count == 1 ? "" : "s",
        address
    );
    app_poke_set_status(app, status);
    SetFocus(app->debug.poke_values_edit);

    free(bytes);
    free(values_text);
    (void)hwnd;
}

