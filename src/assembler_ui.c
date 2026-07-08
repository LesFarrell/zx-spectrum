/* Split from main.c: assembler UI implementation. Included by main.c. */

static void app_default_assembler_logfont(LOGFONTA *font) {
    ZeroMemory(font, sizeof(*font));
    font->lfHeight = -16;
    font->lfWeight = FW_NORMAL;
    font->lfCharSet = DEFAULT_CHARSET;
    font->lfOutPrecision = OUT_DEFAULT_PRECIS;
    font->lfClipPrecision = CLIP_DEFAULT_PRECIS;
    font->lfQuality = CLEARTYPE_QUALITY;
    font->lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    snprintf(font->lfFaceName, sizeof(font->lfFaceName), "Consolas");
}

/* Creates a simple document-style menu for the assembler window with file,
   edit, build, and help actions. */
static HMENU app_create_assembler_menu(void) {
    HMENU menu_bar = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU edit_menu = CreatePopupMenu();
    HMENU build_menu = CreatePopupMenu();
    HMENU help_menu = CreatePopupMenu();

    if (menu_bar == NULL || file_menu == NULL || edit_menu == NULL || build_menu == NULL || help_menu == NULL) {
        if (help_menu != NULL) {
            DestroyMenu(help_menu);
        }
        if (build_menu != NULL) {
            DestroyMenu(build_menu);
        }
        if (edit_menu != NULL) {
            DestroyMenu(edit_menu);
        }
        if (file_menu != NULL) {
            DestroyMenu(file_menu);
        }
        if (menu_bar != NULL) {
            DestroyMenu(menu_bar);
        }
        return NULL;
    }

    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_NEW, "&New\tCtrl+N");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_LOAD, "&Load...\tCtrl+O");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_RELOAD, "&Reload\tCtrl+R");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_SAVE, "&Save\tCtrl+S");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_SAVE_AS, "Save &As...\tCtrl+Shift+S");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_EXPORT_TAP, "Export &TAP...\tCtrl+Shift+T");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_UNDO, "Undo\tCtrl+Z");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_CUT, "Cut\tCtrl+X");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_COPY, "Copy\tCtrl+C");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_PASTE, "Paste\tCtrl+V");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_DELETE, "Delete\tDel");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_SELECT_ALL, "Select All\tCtrl+A");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_FONT, "Font...\tCtrl+Shift+F");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_FORMAT, "Format Source\tCtrl+Shift+L");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_UPPERCASE, "Uppercase Source\tCtrl+Shift+U");
    AppendMenuA(build_menu, MF_STRING, APP_MENU_ASM_BUILD_ASSEMBLE, "&Assemble\tCtrl+B");
    AppendMenuA(help_menu, MF_STRING, APP_MENU_ASM_HELP_SHOW, "Assembler &Help\tF1");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)file_menu, "File");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)edit_menu, "Edit");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)build_menu, "Build");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)help_menu, "&Help");
    return menu_bar;
}

/* Updates the window caption so the running machine type is visible from the
   title bar without inspecting the current ROM selection. */
static void app_update_title(HWND hwnd, const Spectrum *spec) {
    char title[128];
    snprintf(
        title,
        sizeof(title),
        "ZX Spectrum Emulator (%s)",
        app_model_name(spec->model)
    );
    SetWindowTextA(hwnd, title);
}

/* Marks the active machine selection in the menu bar so the current backend
   model stays visible even after snapshots or menu-driven rebuilds. */
static void app_update_model_menu(HWND hwnd, SpectrumModel model) {
    HMENU menu = GetMenu(hwnd);
    if (menu == NULL) {
        return;
    }
    CheckMenuRadioItem(
        menu,
        APP_MENU_MACHINE_48K,
        APP_MENU_MACHINE_128K,
        app_model_menu_id(model),
        MF_BYCOMMAND
    );
}

/* Reflects the current tape auto-load setting in the Tape menu. */
static void app_update_tape_menu(HWND hwnd, bool enabled) {
    HMENU menu = GetMenu(hwnd);
    if (menu == NULL) {
        return;
    }
    CheckMenuItem(
        menu,
        APP_MENU_FILE_AUTOLOAD_TAPES,
        MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED)
    );
}

/* Creates the default assembler editor font used when the user has not chosen
   a persisted replacement yet. */
static HFONT app_create_monospace_font(void) {
    LOGFONTA font;
    app_default_assembler_logfont(&font);
    return CreateFontIndirectA(&font);
}

/* Appends formatted text into a caller-owned buffer while preserving a valid
   trailing NUL even when the output becomes truncated. */

static void app_assembler_refresh_window(AppState *app) {
    if (app->debug.assembler_hwnd == NULL) {
        return;
    }
    app_assembler_update_line_numbers(app);
}

/* Prompts to save the current assembler source when there are unsaved edits
   and returns false when the close should be cancelled. */
static bool app_assembler_confirm_close(HWND hwnd, AppState *app) {
    char status[512];
    int response;

    if (app == NULL || !app->debug.assembler_dirty) {
        return true;
    }

    response = MessageBoxA(
        hwnd,
        "Save changes to the current assembler source before closing?",
        "Spectrum Assembler",
        MB_YESNOCANCEL | MB_ICONWARNING
    );
    if (response == IDCANCEL) {
        return false;
    }
    if (response == IDYES) {
        if (!app_assembler_save_source(hwnd, app, status, sizeof(status))) {
            if (app->debug.assembler_status != NULL) {
                app_assembler_set_status(app, status);
            }
            return false;
        }
        if (app->debug.assembler_status != NULL) {
            app_assembler_set_status(app, status);
        }
    }
    return true;
}

/* Polls the current assembler file for external timestamp changes and offers
   to reload it once per observed on-disk update. */
static void app_assembler_poll_external_change(HWND hwnd, AppState *app) {
    FILETIME write_time;
    int reload_result;
    char status[512];

    if (app == NULL ||
        app->debug.assembler_file_change_prompt_active ||
        app->debug.assembler_current_path[0] == '\0') {
        return;
    }
    if (!app_assembler_get_file_write_time(app->debug.assembler_current_path, &write_time)) {
        return;
    }
    if (!app->debug.assembler_has_file_write_time) {
        app->debug.assembler_file_write_time = write_time;
        app->debug.assembler_has_file_write_time = true;
        return;
    }
    if (CompareFileTime(&write_time, &app->debug.assembler_file_write_time) == 0) {
        return;
    }

    app->debug.assembler_file_write_time = write_time;
    app->debug.assembler_has_file_write_time = true;
    app->debug.assembler_file_change_prompt_active = true;
    reload_result = MessageBoxA(
        hwnd,
        app->debug.assembler_dirty
            ? "The assembler file changed on disk.\r\n\r\nReload it now and discard unsaved editor changes?"
            : "The assembler file changed on disk.\r\n\r\nReload it now?",
        "Assembler File Changed",
        MB_YESNO | (app->debug.assembler_dirty ? MB_ICONWARNING : MB_ICONINFORMATION)
    );
    app->debug.assembler_file_change_prompt_active = false;

    if (reload_result == IDYES) {
        app_assembler_load_source_path(app->debug.assembler_current_path, app, status, sizeof(status));
        app_assembler_set_status(app, status);
        return;
    }

    app_assembler_set_status(app, "Assembler file changed on disk. Use File -> Reload when ready.");
}

/* Clears the current assembler error marker and repaints the gutter when
   stale diagnostics should no longer be shown to the user. */
static void app_assembler_clear_error_marker(AppState *app) {
    if (app == NULL) {
        return;
    }
    app->debug.assembler_error_line = 0;
    if (app->debug.assembler_line_numbers != NULL) {
        InvalidateRect(app->debug.assembler_line_numbers, NULL, TRUE);
    }
}

/* Moves the assembler caret to the requested 1-based source line and scrolls
   the edit control so the target becomes visible immediately after errors. */
static void app_assembler_focus_line(AppState *app, size_t line_number) {
    LRESULT start_index;

    if (app == NULL || app->debug.assembler_source == NULL || line_number == 0) {
        return;
    }
    start_index = SendMessageA(app->debug.assembler_source, EM_LINEINDEX, (WPARAM)(line_number - 1), 0);
    if (start_index < 0) {
        return;
    }
    SendMessageA(app->debug.assembler_source, EM_SETSEL, (WPARAM)start_index, (LPARAM)start_index);
    SendMessageA(app->debug.assembler_source, EM_SCROLLCARET, 0, 0);
    SetFocus(app->debug.assembler_source);
}

/* Parses a formatted assembler diagnostic and, when it refers to the current
   editor buffer, moves the caret there and paints one gutter error marker. */
static void app_assembler_sync_error_from_status(AppState *app, const char *status_text) {
    const char *line_text = NULL;
    unsigned long long parsed_line;
    char *end = NULL;

    if (app == NULL || status_text == NULL || *status_text == '\0') {
        return;
    }
    if (strncmp(status_text, "Line ", 5) == 0) {
        line_text = status_text + 5;
    } else if (app->debug.assembler_current_path[0] != '\0') {
        size_t path_length = strlen(app->debug.assembler_current_path);
        if (_strnicmp(status_text, app->debug.assembler_current_path, path_length) == 0 &&
            _strnicmp(status_text + path_length, " line ", 6) == 0) {
            line_text = status_text + path_length + 6;
        }
    }
    if (line_text == NULL) {
        return;
    }
    parsed_line = strtoull(line_text, &end, 10);
    if (parsed_line == 0 || end == line_text || end == NULL || *end != ':') {
        return;
    }
    app->debug.assembler_error_line = (size_t)parsed_line;
    if (app->debug.assembler_line_numbers != NULL) {
        InvalidateRect(app->debug.assembler_line_numbers, NULL, TRUE);
    }
    app_assembler_focus_line(app, app->debug.assembler_error_line);
}

/* Repaints the assembler gutter after source edits or scroll activity so line
   numbers and the current error marker stay visually synchronized. */
static void app_assembler_update_line_numbers(AppState *app) {
    if (app == NULL || app->debug.assembler_line_numbers == NULL) {
        return;
    }
    InvalidateRect(app->debug.assembler_line_numbers, NULL, TRUE);
}

/* Paints the assembler gutter with right-aligned line numbers plus one red
   dot beside the active error line when the last compile failed locally. */
static LRESULT CALLBACK app_assembler_gutter_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(GetParent(hwnd), GWLP_USERDATA);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            RECT rect;
            RECT source_rect;
            HDC hdc;
            HGDIOBJ old_font;
            HBRUSH background_brush;
            HBRUSH error_brush;
            HPEN error_pen;
            HGDIOBJ old_brush;
            HGDIOBJ old_pen;
            TEXTMETRICA metrics;
            int first_line;
            int line_count;
            int char_height;
            int visible_lines;

            BeginPaint(hwnd, &ps);
            GetClientRect(hwnd, &rect);
            hdc = ps.hdc;
            background_brush = CreateSolidBrush(RGB(244, 240, 228));
            FillRect(hdc, &rect, background_brush);
            DeleteObject(background_brush);
            if (app == NULL || app->debug.assembler_source == NULL) {
                EndPaint(hwnd, &ps);
                return 0;
            }

            old_font = SelectObject(hdc, app->debug.assembler_font != NULL ? app->debug.assembler_font : GetStockObject(ANSI_FIXED_FONT));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(96, 96, 96));
            GetTextMetricsA(hdc, &metrics);
            char_height = metrics.tmHeight + metrics.tmExternalLeading;
            if (char_height <= 0) {
                char_height = 16;
            }

            first_line = (int)SendMessageA(app->debug.assembler_source, EM_GETFIRSTVISIBLELINE, 0, 0);
            line_count = (int)SendMessageA(app->debug.assembler_source, EM_GETLINECOUNT, 0, 0);
            if (line_count < 1) {
                line_count = 1;
            }
            GetClientRect(app->debug.assembler_source, &source_rect);
            visible_lines = (source_rect.bottom - source_rect.top) / char_height + 2;
            if (visible_lines < 1) {
                visible_lines = 1;
            }

            error_brush = CreateSolidBrush(RGB(220, 40, 40));
            error_pen = CreatePen(PS_SOLID, 1, RGB(220, 40, 40));
            old_brush = SelectObject(hdc, error_brush);
            old_pen = SelectObject(hdc, error_pen);

            for (int i = 0; i < visible_lines; ++i) {
                char number_buffer[16];
                RECT line_rect = {18, i * char_height, rect.right - 6, (i + 1) * char_height};
                int line_number = first_line + i + 1;

                if (line_number > line_count) {
                    break;
                }
                snprintf(number_buffer, sizeof(number_buffer), "%d", line_number);
                DrawTextA(hdc, number_buffer, -1, &line_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                if (app->debug.assembler_error_line == (size_t)line_number) {
                    int dot_center_y = i * char_height + char_height / 2;
                    Ellipse(hdc, 5, dot_center_y - 4, 13, dot_center_y + 4);
                }
            }

            SelectObject(hdc, old_pen);
            SelectObject(hdc, old_brush);
            DeleteObject(error_pen);
            DeleteObject(error_brush);
            SelectObject(hdc, old_font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (app != NULL && app->debug.assembler_source != NULL) {
                SetFocus(app->debug.assembler_source);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* Sizes the debugger controls to the current client area of the tool window. */

static void app_assembler_layout_controls(AppState *app, HWND hwnd) {
    RECT rect;
    RECT status_rect;
    int width;
    int height;
    const int gutter_width = 56;
    const int edge_padding = 0;
    int status_height = 0;

    if (app->debug.assembler_source == NULL) {
        return;
    }
    GetClientRect(hwnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    if (app->debug.assembler_status != NULL) {
        SendMessageA(app->debug.assembler_status, WM_SIZE, 0, 0);
        GetWindowRect(app->debug.assembler_status, &status_rect);
        status_height = status_rect.bottom - status_rect.top;
    }
    MoveWindow(
        app->debug.assembler_line_numbers,
        edge_padding,
        edge_padding,
        gutter_width,
        height - status_height - (edge_padding * 2),
        TRUE
    );
    MoveWindow(
        app->debug.assembler_source,
        edge_padding + gutter_width,
        edge_padding,
        width - gutter_width - (edge_padding * 2),
        height - status_height - (edge_padding * 2),
        TRUE
    );
    app_assembler_update_line_numbers(app);
}

/* Routes standard edit commands to the assembler source box so menu actions
   and keyboard shortcuts behave like a normal multiline text editor. */
static bool app_assembler_handle_edit_command(AppState *app, UINT command_id) {
    HWND source;

    if (app == NULL || app->debug.assembler_source == NULL) {
        return false;
    }
    source = app->debug.assembler_source;

    switch (command_id) {
        case APP_MENU_ASM_EDIT_UNDO:
            if ((BOOL)SendMessageA(source, EM_CANUNDO, 0, 0)) {
                SendMessageA(source, WM_UNDO, 0, 0);
            }
            return true;
        case APP_MENU_ASM_EDIT_CUT:
            SendMessageA(source, WM_CUT, 0, 0);
            return true;
        case APP_MENU_ASM_EDIT_COPY:
            SendMessageA(source, WM_COPY, 0, 0);
            return true;
        case APP_MENU_ASM_EDIT_PASTE:
            SendMessageA(source, WM_PASTE, 0, 0);
            return true;
        case APP_MENU_ASM_EDIT_DELETE:
            SendMessageA(source, WM_CLEAR, 0, 0);
            return true;
        case APP_MENU_ASM_EDIT_SELECT_ALL:
            SendMessageA(source, EM_SETSEL, 0, -1);
            return true;
        default:
            return false;
    }
}

/* Keeps the assembler line-number gutter synchronized after scroll and text
   navigation messages handled by the source edit control. */
static LRESULT CALLBACK app_assembler_source_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    HWND parent = GetParent(hwnd);
    AppState *app = parent != NULL ? (AppState *)GetWindowLongPtrA(parent, GWLP_USERDATA) : NULL;
    WNDPROC old_proc = (app != NULL) ? app->debug.assembler_source_wndproc : DefWindowProcA;
    LRESULT result;

    if (app != NULL && msg == WM_KEYDOWN) {
        const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if ((UINT)wparam == VK_F1) {
            SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_HELP_SHOW, 0);
            return 0;
        }
        if ((UINT)wparam == VK_F5) {
            SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_FILE_RELOAD, 0);
            return 0;
        }

        if (ctrl_down && !shift_down) {
            switch ((UINT)wparam) {
                case 'A':
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_SELECT_ALL);
                    app_assembler_update_line_numbers(app);
                    return 0;
                case 'Z':
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_UNDO);
                    app_assembler_update_line_numbers(app);
                    return 0;
                case 'X':
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_CUT);
                    app_assembler_update_line_numbers(app);
                    return 0;
                case 'C':
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_COPY);
                    app_assembler_update_line_numbers(app);
                    return 0;
                case 'V':
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_PASTE);
                    app_assembler_update_line_numbers(app);
                    return 0;
                case 'N':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_FILE_NEW, 0);
                    return 0;
                case 'O':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_FILE_LOAD, 0);
                    return 0;
                case 'S':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_FILE_SAVE, 0);
                    return 0;
                case 'R':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_FILE_RELOAD, 0);
                    return 0;
                case 'B':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_BUILD_ASSEMBLE, 0);
                    return 0;
                case VK_INSERT:
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_COPY);
                    return 0;
                default:
                    break;
            }
        }
        if (ctrl_down && shift_down) {
            switch ((UINT)wparam) {
                case 'S':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_FILE_SAVE_AS, 0);
                    return 0;
                case 'F':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_EDIT_FONT, 0);
                    return 0;
                case 'L':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_EDIT_FORMAT, 0);
                    return 0;
                case 'U':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_EDIT_UPPERCASE, 0);
                    return 0;
                case 'T':
                    SendMessageA(parent, WM_COMMAND, APP_MENU_ASM_FILE_EXPORT_TAP, 0);
                    return 0;
                default:
                    break;
            }
        }
        if (shift_down && !ctrl_down) {
            switch ((UINT)wparam) {
                case VK_DELETE:
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_CUT);
                    app_assembler_update_line_numbers(app);
                    return 0;
                case VK_INSERT:
                    app_assembler_handle_edit_command(app, APP_MENU_ASM_EDIT_PASTE);
                    app_assembler_update_line_numbers(app);
                    return 0;
                default:
                    break;
            }
        }
    }

    result = CallWindowProcA(old_proc, hwnd, msg, wparam, lparam);

    if (app != NULL) {
        switch (msg) {
            case WM_VSCROLL:
            case WM_MOUSEWHEEL:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_CHAR:
            case WM_LBUTTONUP:
            case WM_SIZE:
            case EM_SCROLL:
            case EM_LINESCROLL:
            case EM_SCROLLCARET:
                app_assembler_update_line_numbers(app);
                break;
            default:
                break;
        }
    }
    return result;
}


static LRESULT CALLBACK app_assembler_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *create = (CREATESTRUCTA *)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
            return TRUE;
        }
        case WM_CREATE: {
            HFONT font;
            HMENU menu;
            LOGFONTA assembler_font;
            app->debug.assembler_source_brush = CreateSolidBrush(RGB(244, 240, 228));
            if (app_assembler_load_saved_font(&assembler_font)) {
                app->debug.assembler_font = app_create_assembler_font_from_logfont(&assembler_font);
            } else {
                app->debug.assembler_font = app_create_monospace_font();
            }
            font = app->debug.assembler_font != NULL
                ? app->debug.assembler_font
                : (HFONT)GetStockObject(ANSI_FIXED_FONT);
            menu = app_create_assembler_menu();
            if (menu != NULL) {
                SetMenu(hwnd, menu);
                DrawMenuBar(hwnd);
            }
            SetTimer(hwnd, APP_TIMER_ASM_FILE_WATCH, 1000, NULL);
            app->debug.assembler_status = CreateWindowExA(
                0,
                STATUSCLASSNAMEA,
                "",
                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_ASM_STATUS,
                NULL,
                NULL
            );
            app->debug.assembler_line_numbers = CreateWindowExA(
                0,
                "ZXSpecAssemblerGutter",
                "",
                WS_CHILD | WS_VISIBLE | WS_BORDER,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_ASM_LINES,
                NULL,
                NULL
            );
            app->debug.assembler_source = CreateWindowExA(
                0,
                "EDIT",
                "",
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                    ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_ASM_SOURCE,
                NULL,
                NULL
            );
            SendMessageA(app->debug.assembler_line_numbers, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageA(app->debug.assembler_source, WM_SETFONT, (WPARAM)font, TRUE);
            app->debug.assembler_source_wndproc = (WNDPROC)SetWindowLongPtrA(
                app->debug.assembler_source,
                GWLP_WNDPROC,
                (LONG_PTR)app_assembler_source_wndproc
            );
            app_assembler_layout_controls(app, hwnd);
            app_assembler_set_status(
                app,
                "Mini assembler with labels, EQU constants, DEFS/DS, INCBIN, TAP export, and Load/Save. ORG in the source sets the assembly address; otherwise the current PC is used."
            );
            app_assembler_refresh_window(app);
            return 0;
        }
        case WM_CTLCOLOREDIT:
            if (app != NULL && (HWND)lparam == app->debug.assembler_source) {
                HDC hdc = (HDC)wparam;
                SetTextColor(hdc, RGB(32, 32, 32));
                SetBkColor(hdc, RGB(244, 240, 228));
                return (LRESULT)(app->debug.assembler_source_brush != NULL
                    ? app->debug.assembler_source_brush
                    : GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        case WM_SIZE:
            if (app != NULL) {
                app_assembler_layout_controls(app, hwnd);
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
            if (app != NULL && wparam == APP_TIMER_ASM_FILE_WATCH) {
                app_assembler_poll_external_change(hwnd, app);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (app == NULL) {
                break;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_SOURCE && HIWORD(wparam) == EN_CHANGE) {
                if (!app->debug.assembler_ignore_change) {
                    app->debug.assembler_dirty = true;
                    app_assembler_clear_error_marker(app);
                    app_assembler_set_status(app, "");
                }
                app_assembler_update_line_numbers(app);
                return 0;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_HELP || LOWORD(wparam) == APP_MENU_ASM_HELP_SHOW) {
                app_show_assembler_help(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_EDIT_FONT) {
                app_assembler_choose_font(hwnd, app);
                return 0;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_EDIT_FORMAT) {
                app_assembler_format_source(hwnd, app);
                return 0;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_EDIT_UPPERCASE) {
                app_assembler_uppercase_source(hwnd, app);
                return 0;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_NEW) {
                char status[512];
                app_assembler_new_source(hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return 0;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_LOAD || LOWORD(wparam) == APP_MENU_ASM_FILE_LOAD) {
                char status[512];
                app_assembler_load_source(hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return 0;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_RELOAD) {
                char status[512];
                app_assembler_reload_source(hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return 0;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_SAVE || LOWORD(wparam) == APP_MENU_ASM_FILE_SAVE) {
                char status[512];
                app_assembler_save_source(hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return 0;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_SAVE_AS) {
                char status[512];
                app_assembler_save_source_as(hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return 0;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_EXPORT_TAP) {
                char status[512];
                if (!app_assembler_export_tap(hwnd, app, status, sizeof(status))) {
                    app_assembler_sync_error_from_status(app, status);
                }
                app_assembler_set_status(app, status);
                return 0;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_APPLY || LOWORD(wparam) == APP_MENU_ASM_BUILD_ASSEMBLE) {
                app_assembler_apply_source(hwnd, app);
                return 0;
            }
            if (app_assembler_handle_edit_command(app, LOWORD(wparam))) {
                return 0;
            }
            break;
        case WM_CLOSE:
            if (!app_assembler_confirm_close(hwnd, app)) {
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (app != NULL) {
                app_set_modal_loop_timer(app, hwnd, false);
                KillTimer(hwnd, APP_TIMER_ASM_FILE_WATCH);
                if (app->debug.assembler_source_brush != NULL) {
                    DeleteObject(app->debug.assembler_source_brush);
                    app->debug.assembler_source_brush = NULL;
                }
                if (app->debug.assembler_font != NULL) {
                    DeleteObject(app->debug.assembler_font);
                    app->debug.assembler_font = NULL;
                }
                app->debug.assembler_hwnd = NULL;
                app->debug.assembler_line_numbers = NULL;
                app->debug.assembler_source = NULL;
                app->debug.assembler_status = NULL;
                app->debug.assembler_apply_button = NULL;
                app->debug.assembler_help_button = NULL;
                app->debug.assembler_load_button = NULL;
                app->debug.assembler_save_button = NULL;
                app->debug.assembler_source_wndproc = NULL;
                app->debug.assembler_dirty = false;
                app->debug.assembler_ignore_change = false;
                app->debug.assembler_has_file_write_time = false;
                app->debug.assembler_file_change_prompt_active = false;
                app->debug.assembler_error_line = 0;
                ZeroMemory(&app->debug.assembler_file_write_time, sizeof(app->debug.assembler_file_write_time));
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* Registers the small debugger and assembler tool window classes once during
   startup so they can be created on demand from the Tools menu. */
static bool app_register_tool_window_classes(HINSTANCE instance) {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = app_debugger_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = "ZXSpecDebuggerWindow";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = app_poke_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = "ZXSpecPokeWindow";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = app_assembler_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = "ZXSpecAssemblerWindow";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = app_assembler_gutter_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = "ZXSpecAssemblerGutter";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    return true;
}

/* Loads the executable's icon resource at the requested system size and caches
   it so each top-level window can reuse the same handles. */
static HICON app_get_window_icon(bool small) {
    static HICON large_icon = NULL;
    static HICON small_icon = NULL;
    HICON *target = small ? &small_icon : &large_icon;
    int width = GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON);
    int height = GetSystemMetrics(small ? SM_CYSMICON : SM_CYICON);

    if (*target == NULL) {
        *target = (HICON)LoadImageA(
            GetModuleHandleA(NULL),
            MAKEINTRESOURCEA(APP_ICON_RESOURCE_ID),
            IMAGE_ICON,
            width,
            height,
            LR_DEFAULTCOLOR
        );
    }

    return *target;
}

/* Applies the app's large and small icons so caption bars and task switching
   use the bundled emulator icon on each top-level window. */
static void app_apply_window_icons(HWND hwnd) {
    HICON large_icon;
    HICON small_icon;

    if (hwnd == NULL) {
        return;
    }

    large_icon = app_get_window_icon(false);
    small_icon = app_get_window_icon(true);
    if (large_icon != NULL) {
        SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)large_icon);
    }
    if (small_icon != NULL) {
        SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    }
}

/* Creates or focuses the debugger tool window from the Tools menu. */

static void app_open_assembler_window(AppState *app) {
    char path[MAX_PATH];
    char status[512];

    if (app->debug.assembler_hwnd != NULL) {
        ShowWindow(app->debug.assembler_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(app->debug.assembler_hwnd);
        app_assembler_refresh_window(app);
        return;
    }

    app->debug.assembler_hwnd = CreateWindowExA(
        0,
        "ZXSpecAssemblerWindow",
        "Spectrum Assembler",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        700,
        460,
        app->main_hwnd,
        NULL,
        GetModuleHandleA(NULL),
        app
    );
    app_apply_window_icons(app->debug.assembler_hwnd);
    app_assembler_update_title(app);
    if (app_assembler_load_last_path(path, sizeof(path))) {
        if (app_assembler_load_source_path(path, app, status, sizeof(status))) {
            app_assembler_set_status(app, status);
        } else {
            app_assembler_set_status(app, status);
        }
    }
}

/* Summarizes the debugger controls, shortcuts, and common workflows in one
   modal dialog so the tool can be used without reading the source. */

static void app_show_assembler_help(HWND hwnd) {
    MessageBoxA(
        hwnd,
        "Built-in Spectrum assembler\r\n"
        "\r\n"
        "Labels:\r\n"
        "  start:\r\n"
        "  loop: JR NZ,loop\r\n"
        "\r\n"
        "Constants:\r\n"
        "  SCREEN EQU 4000h\r\n"
        "  ATTRS  EQU 5800h\r\n"
        "\r\n"
        "Directives:\r\n"
        "  ORG address\r\n"
        "  DB value[, value...]\r\n"
        "  DW value[, value...]\r\n"
        "  DS count[, fill]\r\n"
        "  DEFS count[, fill]\r\n"
        "  INCBIN \"file.bin\"\r\n"
        "  INCLUDE \"file.asm\"\r\n"
        "\r\n"
        "Editor:\r\n"
        "  Ctrl+N New\r\n"
        "  Ctrl+O Load\r\n"
        "  Ctrl+R or F5 Reload current file\r\n"
        "  Ctrl+S Save\r\n"
        "  Ctrl+Shift+S Save As\r\n"
        "  Ctrl+Shift+T Export TAP\r\n"
        "  Ctrl+Shift+F Choose Font\r\n"
        "  Ctrl+Shift+L Format Source\r\n"
        "  Ctrl+Shift+U Uppercase Source\r\n"
        "  Ctrl+A Select All\r\n"
        "  Ctrl+B Assemble\r\n"
        "  F1 Assembler Help\r\n"
        "  Ctrl+Z/X/C/V standard edit shortcuts\r\n"
        "\r\n"
        "Compile errors:\r\n"
        "  - The caret jumps to the failing line.\r\n"
        "  - A red dot marks the failing line in the gutter.\r\n"
        "  - Error markers clear on edit, load, and compile.\r\n"
        "\r\n"
        "Address selection:\r\n"
        "  - If the source contains ORG, that address is used.\r\n"
        "  - Otherwise assembly starts at the current PC.\r\n"
        "\r\n"
        "Common instructions supported:\r\n"
        "  LD, JP, JR, DJNZ, CALL, RET, INC, DEC\r\n"
        "  ADD, SUB, AND, OR, XOR, CP\r\n"
        "  PUSH, POP, IN, OUT, EX, DI, EI, RST\r\n"
        "\r\n"
        "Notes:\r\n"
        "  - Numbers may be decimal, $hex, 0xhex, hex with H suffix, or %binary.\r\n"
        "  - Constants use NAME EQU value and can reference earlier labels or constants.\r\n"
        "  - Quoted text works in DB, for example DB \"HELLO\",13.\r\n"
        "  - DS is an alias for DEFS.\r\n"
        "  - DEFS emits count fill bytes into RAM; omitted fill defaults to 0.\r\n"
        "  - INCBIN copies one raw binary file into RAM at the current address.\r\n"
        "  - INCLUDE expands in place, so it can appear mid-file.\r\n"
        "  - INCLUDE paths are resolved relative to the current source file.\r\n"
        "  - Export TAP writes one standard Spectrum CODE block.\r\n"
        "  - Export TAP requires one contiguous output range without ORG jumps.\r\n"
        "  - The current file is watched for external edits and reload prompts automatically.\r\n"
        "  - Labels and constants share one symbol namespace.\r\n"
        "  - Macros are not supported.\r\n"
        "  - Writes are limited to RAM at 4000h-FFFFh.\r\n",
        "Assembler Help",
        MB_OK | MB_ICONINFORMATION
    );
}

/* Writes one message into the assembler's bottom status bar when it exists. */
static void app_assembler_set_status(AppState *app, const char *text) {
    if (app == NULL || app->debug.assembler_status == NULL) {
        return;
    }
    SendMessageA(app->debug.assembler_status, SB_SETTEXTA, 0, (LPARAM)text);
}

/* Reads the last-write timestamp for one source file so the assembler can
   detect when an external editor has changed the file on disk. */
static bool app_assembler_get_file_write_time(const char *path, FILETIME *out_write_time) {
    WIN32_FILE_ATTRIBUTE_DATA attributes;

    if (path == NULL || path[0] == '\0' || out_write_time == NULL) {
        return false;
    }
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) {
        return false;
    }
    *out_write_time = attributes.ftLastWriteTime;
    return true;
}

/* Updates the assembler's external-change tracking after loading, saving, or
   switching the current source path. */
static void app_assembler_sync_file_tracking(AppState *app, const char *path) {
    FILETIME write_time;

    if (app == NULL) {
        return;
    }
    if (app_assembler_get_file_write_time(path, &write_time)) {
        app->debug.assembler_file_write_time = write_time;
        app->debug.assembler_has_file_write_time = true;
        return;
    }
    ZeroMemory(&app->debug.assembler_file_write_time, sizeof(app->debug.assembler_file_write_time));
    app->debug.assembler_has_file_write_time = false;
}

/* Reflects the current assembler document path in the window title bar. */
static void app_assembler_update_title(AppState *app) {
    char title[MAX_PATH + 32];

    if (app == NULL || app->debug.assembler_hwnd == NULL) {
        return;
    }
    if (app->debug.assembler_current_path[0] != '\0') {
        snprintf(title, sizeof(title), "Spectrum Assembler - %s", app->debug.assembler_current_path);
    } else {
        snprintf(title, sizeof(title), "Spectrum Assembler");
    }
    SetWindowTextA(app->debug.assembler_hwnd, title);
}

/* Stores the active assembler document path, persists it for the next launch,
   and updates the assembler window title when it is open. */
static void app_assembler_set_current_path(AppState *app, const char *path) {
    if (app == NULL) {
        return;
    }
    if (path != NULL) {
        snprintf(app->debug.assembler_current_path, sizeof(app->debug.assembler_current_path), "%s", path);
        app_assembler_save_last_path(path);
        app_assembler_sync_file_tracking(app, path);
    } else {
        app->debug.assembler_current_path[0] = '\0';
        app_assembler_save_last_path("");
        app_assembler_sync_file_tracking(app, NULL);
    }
    app_assembler_update_title(app);
}

/* Loads a plain-text source file into the assembler edit control and
   normalizes bare LF line endings for the standard Win32 multiline edit box. */
static bool app_assembler_load_source_path(
    const char *path,
    AppState *app,
    char *status_buffer,
    size_t status_buffer_size
) {
    FILE *file;
    long file_size;
    char *data;
    char *normalized;
    size_t extra_cr = 0;
    size_t normalized_index = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(status_buffer, status_buffer_size, "Could not open file: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(status_buffer, status_buffer_size, "Could not read file: %s", path);
        return false;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        snprintf(status_buffer, status_buffer_size, "Could not read file: %s", path);
        return false;
    }
    rewind(file);

    data = (char *)malloc((size_t)file_size + 1);
    if (data == NULL) {
        fclose(file);
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }
    if (file_size > 0 && fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        free(data);
        snprintf(status_buffer, status_buffer_size, "Could not read file: %s", path);
        return false;
    }
    fclose(file);
    data[file_size] = '\0';

    for (long i = 0; i < file_size; ++i) {
        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) {
            extra_cr++;
        }
    }

    normalized = (char *)malloc((size_t)file_size + extra_cr + 1);
    if (normalized == NULL) {
        free(data);
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }
    for (long i = 0; i < file_size; ++i) {
        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) {
            normalized[normalized_index++] = '\r';
        }
        normalized[normalized_index++] = data[i];
    }
    normalized[normalized_index] = '\0';

    app->debug.assembler_ignore_change = true;
    SetWindowTextA(app->debug.assembler_source, normalized);
    app->debug.assembler_ignore_change = false;
    app->debug.assembler_dirty = false;
    app_assembler_clear_error_marker(app);
    app_assembler_set_current_path(app, path);
    snprintf(status_buffer, status_buffer_size, "Loaded assembler source from %s", path);
    free(normalized);
    free(data);
    return true;
}

/* Confirms whether a disk-backed source operation may replace unsaved editor
   contents. */
static bool app_assembler_confirm_replace_source(
    HWND hwnd,
    AppState *app,
    const char *prompt,
    const char *cancel_status,
    char *status_buffer,
    size_t status_buffer_size
) {
    int result;

    if (app == NULL || !app->debug.assembler_dirty) {
        return true;
    }

    result = MessageBoxA(hwnd, prompt, "Assembler", MB_YESNO | MB_ICONWARNING);
    if (result != IDYES) {
        snprintf(status_buffer, status_buffer_size, "%s", cancel_status);
        return false;
    }
    return true;
}

/* Opens a file picker for assembler source and loads the selected text file
   into the current assembler document. */
static bool app_assembler_load_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size) {
    OPENFILENAMEA ofn;
    char path[MAX_PATH];

    if (!app_assembler_confirm_replace_source(
        hwnd,
        app,
        "Discard unsaved assembler changes and load another file?",
        "Load cancelled.",
        status_buffer,
        status_buffer_size
    )) {
        return false;
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ZeroMemory(path, sizeof(path));
    if (app->debug.assembler_current_path[0] != '\0') {
        snprintf(path, sizeof(path), "%s", app->debug.assembler_current_path);
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        "Assembler Source (*.asm;*.txt)\0*.asm;*.txt\0"
        "All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = "asm";

    if (!GetOpenFileNameA(&ofn)) {
        snprintf(status_buffer, status_buffer_size, "Load cancelled.");
        return false;
    }
    return app_assembler_load_source_path(path, app, status_buffer, status_buffer_size);
}

/* Reloads the current assembler file from disk so external edits can replace
   the in-editor buffer without browsing for the file again. */
static bool app_assembler_reload_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size) {
    if (app == NULL) {
        snprintf(status_buffer, status_buffer_size, "No assembler is open.");
        return false;
    }
    if (app->debug.assembler_current_path[0] == '\0') {
        snprintf(status_buffer, status_buffer_size, "No current assembler file to reload.");
        return false;
    }
    if (!app_assembler_confirm_replace_source(
        hwnd,
        app,
        "Discard unsaved assembler changes and reload from disk?",
        "Reload cancelled.",
        status_buffer,
        status_buffer_size
    )) {
        return false;
    }
    return app_assembler_load_source_path(
        app->debug.assembler_current_path,
        app,
        status_buffer,
        status_buffer_size
    );
}

/* Writes the current assembler source to one concrete path and updates the
   active document identity after the write succeeds. */
static bool app_assembler_write_source_to_path(
    AppState *app,
    const char *path,
    char *status_buffer,
    size_t status_buffer_size
) {
    int source_length = GetWindowTextLengthA(app->debug.assembler_source);
    char *source;
    FILE *file;

    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }
    GetWindowTextA(app->debug.assembler_source, source, source_length + 1);

    file = fopen(path, "wb");
    if (file == NULL) {
        free(source);
        snprintf(status_buffer, status_buffer_size, "Could not write file: %s", path);
        return false;
    }
    if (source_length > 0 && fwrite(source, 1, (size_t)source_length, file) != (size_t)source_length) {
        fclose(file);
        free(source);
        snprintf(status_buffer, status_buffer_size, "Could not write file: %s", path);
        return false;
    }
    fclose(file);
    free(source);
    app->debug.assembler_dirty = false;
    app_assembler_set_current_path(app, path);
    snprintf(status_buffer, status_buffer_size, "Saved assembler source to %s", path);
    return true;
}

/* Saves to the current path when one exists, otherwise it falls back to the
   Save As flow so unnamed documents still prompt for a filename. */
static bool app_assembler_save_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size) {
    (void)hwnd;

    if (app->debug.assembler_current_path[0] != '\0') {
        return app_assembler_write_source_to_path(
            app,
            app->debug.assembler_current_path,
            status_buffer,
            status_buffer_size
        );
    }
    return app_assembler_save_source_as(hwnd, app, status_buffer, status_buffer_size);
}

/* Prompts for a filename and then writes the current assembler source to the
   chosen path, even if the document already has a current filename. */
static bool app_assembler_save_source_as(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size) {
    OPENFILENAMEA ofn;
    char path[MAX_PATH];

    ZeroMemory(&ofn, sizeof(ofn));
    ZeroMemory(path, sizeof(path));
    if (app->debug.assembler_current_path[0] != '\0') {
        snprintf(path, sizeof(path), "%s", app->debug.assembler_current_path);
    } else {
        snprintf(path, sizeof(path), "source.asm");
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        "Assembler Source (*.asm;*.txt)\0*.asm;*.txt\0"
        "All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "asm";

    if (!GetSaveFileNameA(&ofn)) {
        snprintf(status_buffer, status_buffer_size, "Save As cancelled.");
        return false;
    }
    return app_assembler_write_source_to_path(app, path, status_buffer, status_buffer_size);
}

/* Assembles the current source into one contiguous CODE block and saves it as
   a standard Spectrum TAP image without altering live RAM. */
static bool app_assembler_export_tap(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size) {
    OPENFILENAMEA ofn;
    AssemblerPreparedSource prepared_source;
    AssemblerBinaryOutput output;
    uint16_t effective_address;
    uint16_t org_address;
    int source_length;
    char *source;
    char path[MAX_PATH];

    if (app == NULL || app->debug.assembler_source == NULL) {
        snprintf(status_buffer, status_buffer_size, "No assembler is open.");
        return false;
    }

    source_length = GetWindowTextLengthA(app->debug.assembler_source);
    if (source_length <= 0) {
        snprintf(status_buffer, status_buffer_size, "Enter one or more assembler lines first.");
        return false;
    }

    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }
    memset(&output, 0, sizeof(output));
    GetWindowTextA(app->debug.assembler_source, source, source_length + 1);
    if (!app_assembler_prepare_source(app, source, &prepared_source, status_buffer, status_buffer_size)) {
        free(source);
        return false;
    }

    if (app_assembler_find_source_org(prepared_source.text, &org_address)) {
        effective_address = org_address;
    } else {
        effective_address = app->spec.machine.cpu.pc;
    }

    if (!app_assemble_source(
            app,
            effective_address,
            &prepared_source,
            false,
            &output,
            status_buffer,
            status_buffer_size)) {
        app_assembler_free_prepared_source(&prepared_source);
        app_assembler_free_binary_output(&output);
        free(source);
        return false;
    }
    if (!output.has_start_address || output.length == 0) {
        snprintf(status_buffer, status_buffer_size, "TAP export requires at least 1 assembled byte.");
        app_assembler_free_prepared_source(&prepared_source);
        app_assembler_free_binary_output(&output);
        free(source);
        return false;
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ZeroMemory(path, sizeof(path));
    if (app->debug.assembler_current_path[0] != '\0') {
        char *dot;
        char *slash;

        snprintf(path, sizeof(path), "%s", app->debug.assembler_current_path);
        slash = strrchr(path, '\\');
        dot = strrchr(path, '.');
        if (dot != NULL && (slash == NULL || dot > slash)) {
            *dot = '\0';
        }
        snprintf(path + strlen(path), sizeof(path) - strlen(path), ".tap");
    } else {
        snprintf(path, sizeof(path), "program.tap");
    }

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        "Spectrum Tape Image (*.tap)\0*.tap\0"
        "All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "tap";

    if (!GetSaveFileNameA(&ofn)) {
        snprintf(status_buffer, status_buffer_size, "Export TAP cancelled.");
        app_assembler_free_prepared_source(&prepared_source);
        app_assembler_free_binary_output(&output);
        free(source);
        return false;
    }

    if (!app_assembler_write_tap_file(
            path,
            output.start_address,
            output.bytes,
            output.length,
            status_buffer,
            status_buffer_size)) {
        app_assembler_free_prepared_source(&prepared_source);
        app_assembler_free_binary_output(&output);
        free(source);
        return false;
    }

    snprintf(
        status_buffer,
        status_buffer_size,
        "Exported %zu byte%s to TAP %s at %04Xh.",
        output.length,
        output.length == 1 ? "" : "s",
        path,
        output.start_address
    );
    app_assembler_free_prepared_source(&prepared_source);
    app_assembler_free_binary_output(&output);
    free(source);
    return true;
}

/* Clears the current assembler document after offering to save unsaved edits. */
static bool app_assembler_new_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size) {
    if (!app_assembler_confirm_close(hwnd, app)) {
        snprintf(status_buffer, status_buffer_size, "New cancelled.");
        return false;
    }

    app->debug.assembler_ignore_change = true;
    SetWindowTextA(app->debug.assembler_source, "");
    app->debug.assembler_ignore_change = false;
    app->debug.assembler_dirty = false;
    app_assembler_clear_error_marker(app);
    app_assembler_set_current_path(app, NULL);
    app_assembler_update_line_numbers(app);
    snprintf(status_buffer, status_buffer_size, "Started a new assembler source.");
    return true;
}

/* Assembles the current source buffer into Spectrum RAM using the source ORG
   when present and otherwise the live program counter as the entry address. */
static void app_assembler_apply_source(HWND hwnd, AppState *app) {
    char status[512];
    uint16_t effective_address;
    uint16_t org_address;
    int source_length = GetWindowTextLengthA(app->debug.assembler_source);
    AssemblerPreparedSource prepared_source;
    char *source;
    (void)hwnd;

    app_assembler_clear_error_marker(app);
    if (source_length <= 0) {
        app_assembler_set_status(app, "Enter one or more assembler lines first.");
        return;
    }

    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        app_assembler_set_status(app, "Out of memory.");
        return;
    }
    GetWindowTextA(app->debug.assembler_source, source, source_length + 1);
    if (!app_assembler_prepare_source(app, source, &prepared_source, status, sizeof(status))) {
        app_assembler_sync_error_from_status(app, status);
        app_assembler_set_status(app, status);
        free(source);
        return;
    }
    if (app_assembler_find_source_org(prepared_source.text, &org_address)) {
        effective_address = org_address;
    } else {
        effective_address = app->spec.machine.cpu.pc;
    }
    if (app_assemble_source(app, effective_address, &prepared_source, true, NULL, status, sizeof(status))) {
        InvalidateRect(app->main_hwnd, NULL, FALSE);
        app_debug_refresh_window(app);
    } else {
        app_assembler_sync_error_from_status(app, status);
    }
    app_assembler_free_prepared_source(&prepared_source);
    app_assembler_set_status(app, status);
    free(source);
}

static bool app_assembler_load_last_path(char *out_path, size_t out_size) {
    char path[MAX_PATH];

    if (out_size == 0) {
        return false;
    }
    out_path[0] = '\0';
    if (!app_settings_path(path, sizeof(path))) {
        return false;
    }
    GetPrivateProfileStringA("assembler", "last_file", "", out_path, (DWORD)out_size, path);
    return out_path[0] != '\0';
}

/* Persists the last-used assembler source path so the document can be restored
   automatically when the assembler window opens again. */
static void app_assembler_save_last_path(const char *path_value) {
    char path[MAX_PATH];

    if (!app_settings_path(path, sizeof(path))) {
        return;
    }
    WritePrivateProfileStringA("assembler", "last_file", path_value, path);
}

static bool app_assembler_load_saved_font(LOGFONTA *font) {
    char path[MAX_PATH];
    char value[64];
    char face_name[LF_FACESIZE];
    bool loaded = false;

    app_default_assembler_logfont(font);
    if (!app_settings_path(path, sizeof(path))) {
        return false;
    }

    GetPrivateProfileStringA("assembler", "font_face", "", face_name, sizeof(face_name), path);
    if (face_name[0] != '\0') {
        snprintf(font->lfFaceName, sizeof(font->lfFaceName), "%s", face_name);
        loaded = true;
    }
    GetPrivateProfileStringA("assembler", "font_height", "", value, sizeof(value), path);
    if (value[0] != '\0') {
        font->lfHeight = atoi(value);
        loaded = true;
    }
    GetPrivateProfileStringA("assembler", "font_weight", "", value, sizeof(value), path);
    if (value[0] != '\0') {
        font->lfWeight = atoi(value);
        loaded = true;
    }
    GetPrivateProfileStringA("assembler", "font_italic", "", value, sizeof(value), path);
    if (value[0] != '\0') {
        font->lfItalic = (BYTE)(atoi(value) != 0 ? TRUE : FALSE);
        loaded = true;
    }
    GetPrivateProfileStringA("assembler", "font_charset", "", value, sizeof(value), path);
    if (value[0] != '\0') {
        font->lfCharSet = (BYTE)atoi(value);
        loaded = true;
    }

    return loaded;
}

static void app_assembler_save_font(const LOGFONTA *font) {
    char path[MAX_PATH];
    char value[64];

    if (!app_settings_path(path, sizeof(path))) {
        return;
    }

    WritePrivateProfileStringA("assembler", "font_face", font->lfFaceName, path);
    snprintf(value, sizeof(value), "%ld", font->lfHeight);
    WritePrivateProfileStringA("assembler", "font_height", value, path);
    snprintf(value, sizeof(value), "%ld", font->lfWeight);
    WritePrivateProfileStringA("assembler", "font_weight", value, path);
    snprintf(value, sizeof(value), "%u", font->lfItalic ? 1u : 0u);
    WritePrivateProfileStringA("assembler", "font_italic", value, path);
    snprintf(value, sizeof(value), "%u", (unsigned)font->lfCharSet);
    WritePrivateProfileStringA("assembler", "font_charset", value, path);
}

static HFONT app_create_assembler_font_from_logfont(const LOGFONTA *font) {
    return CreateFontIndirectA(font);
}

static void app_assembler_apply_font(AppState *app, const LOGFONTA *font) {
    HFONT new_font;

    if (app == NULL || font == NULL) {
        return;
    }

    new_font = app_create_assembler_font_from_logfont(font);
    if (new_font == NULL) {
        return;
    }

    if (app->debug.assembler_font != NULL) {
        DeleteObject(app->debug.assembler_font);
    }
    app->debug.assembler_font = new_font;

    if (app->debug.assembler_line_numbers != NULL) {
        SendMessageA(app->debug.assembler_line_numbers, WM_SETFONT, (WPARAM)new_font, TRUE);
        InvalidateRect(app->debug.assembler_line_numbers, NULL, TRUE);
    }
    if (app->debug.assembler_source != NULL) {
        SendMessageA(app->debug.assembler_source, WM_SETFONT, (WPARAM)new_font, TRUE);
        InvalidateRect(app->debug.assembler_source, NULL, TRUE);
    }
    app_assembler_update_line_numbers(app);
}

static void app_assembler_choose_font(HWND hwnd, AppState *app) {
    CHOOSEFONTA choose_font;
    LOGFONTA font;

    if (app == NULL) {
        return;
    }

    if (app->debug.assembler_font != NULL) {
        GetObjectA(app->debug.assembler_font, sizeof(font), &font);
    } else if (!app_assembler_load_saved_font(&font)) {
        app_default_assembler_logfont(&font);
    }

    ZeroMemory(&choose_font, sizeof(choose_font));
    choose_font.lStructSize = sizeof(choose_font);
    choose_font.hwndOwner = hwnd;
    choose_font.lpLogFont = &font;
    choose_font.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST | CF_FIXEDPITCHONLY;

    if (!ChooseFontA(&choose_font)) {
        return;
    }

    app_assembler_apply_font(app, &font);
    app_assembler_save_font(&font);
    app_assembler_set_status(app, "Assembler font updated.");
}

/* Finds the first assembler comment marker in a text range while respecting
   single- and double-quoted string literals. */
static const char *app_find_comment_start(const char *start, const char *end) {
    bool in_single = false;
    bool in_double = false;

    for (const char *cursor = start; cursor < end; ++cursor) {
        if (*cursor == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*cursor == '"' && !in_single) {
            in_double = !in_double;
        } else if (*cursor == ';' && !in_single && !in_double) {
            return cursor;
        }
    }

    return NULL;
}

/* Appends a comment while normalizing its prefix to `; ` when text follows. */
static bool app_append_formatted_comment(
    char **formatted,
    size_t *capacity,
    size_t *used,
    const char *comment_start,
    const char *comment_end
) {
    const char *comment_text_start;
    size_t required;
    char *resized;

    if (
        formatted == NULL || *formatted == NULL ||
        capacity == NULL || used == NULL ||
        comment_start == NULL || comment_end == NULL ||
        comment_start >= comment_end || *comment_start != ';'
    ) {
        return false;
    }

    comment_text_start = comment_start + 1;
    while (comment_text_start < comment_end && isspace((unsigned char)*comment_text_start)) {
        comment_text_start++;
    }

    required = 1;
    if (comment_text_start < comment_end) {
        required += 1 + (size_t)(comment_end - comment_text_start);
    }
    if (*used + required + 1 >= *capacity) {
        *capacity = *capacity + required + 256;
        resized = (char *)realloc(*formatted, *capacity);
        if (resized == NULL) {
            return false;
        }
        *formatted = resized;
    }

    (*formatted)[(*used)++] = ';';
    if (comment_text_start < comment_end) {
        (*formatted)[(*used)++] = ' ';
        memcpy(*formatted + *used, comment_text_start, (size_t)(comment_end - comment_text_start));
        *used += (size_t)(comment_end - comment_text_start);
    }

    return true;
}


static void app_assembler_format_source(HWND hwnd, AppState *app) {
    int source_length;
    char *source;
    char *formatted;

    if (app == NULL || app->debug.assembler_source == NULL) {
        return;
    }

    source_length = GetWindowTextLengthA(app->debug.assembler_source);
    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        app_assembler_set_status(app, "Out of memory.");
        return;
    }
    GetWindowTextA(app->debug.assembler_source, source, source_length + 1);

    if (!app_assembler_format_source_text(source, &formatted)) {
        free(source);
        app_assembler_set_status(app, "Could not format assembler source.");
        return;
    }

    app->debug.assembler_ignore_change = true;
    SetWindowTextA(app->debug.assembler_source, formatted);
    app->debug.assembler_ignore_change = false;
    app->debug.assembler_dirty = true;
    app_assembler_clear_error_marker(app);
    app_assembler_update_line_numbers(app);
    app_assembler_set_status(app, "Assembler source formatted.");
    SetFocus(app->debug.assembler_source);

    free(formatted);
    free(source);
    (void)hwnd;
}


static void app_assembler_uppercase_source(HWND hwnd, AppState *app) {
    int source_length;
    char *source;
    char *uppercase_text;

    if (app == NULL || app->debug.assembler_source == NULL) {
        return;
    }

    source_length = GetWindowTextLengthA(app->debug.assembler_source);
    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        app_assembler_set_status(app, "Out of memory.");
        return;
    }
    GetWindowTextA(app->debug.assembler_source, source, source_length + 1);

    if (!app_assembler_uppercase_source_text(source, &uppercase_text)) {
        free(source);
        app_assembler_set_status(app, "Could not convert assembler source to upper case.");
        return;
    }

    app->debug.assembler_ignore_change = true;
    SetWindowTextA(app->debug.assembler_source, uppercase_text);
    app->debug.assembler_ignore_change = false;
    app->debug.assembler_dirty = true;
    app_assembler_clear_error_marker(app);
    app_assembler_update_line_numbers(app);
    app_assembler_set_status(app, "Assembler source converted to upper case.");
    SetFocus(app->debug.assembler_source);

    free(uppercase_text);
    free(source);
    (void)hwnd;
}

/* Returns true only when the supplied path exists and resolves to a file
   rather than a directory. */