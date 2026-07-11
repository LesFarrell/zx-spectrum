/* Split from main.c: assembler UI implementation. Included by main.c. */

static LRESULT app_scintilla_send(HWND editor, UINT message, WPARAM wparam, LPARAM lparam) {
    return SendMessageA(editor, message, wparam, lparam);
}

static UINT app_assembler_find_message;

static int app_assembler_editor_text_length(HWND editor) {
    return (int)app_scintilla_send(editor, SCI_GETTEXTLENGTH, 0, 0);
}

static void app_assembler_editor_get_text(HWND editor, char *text, int capacity) {
    if (editor == NULL || text == NULL || capacity <= 0) {
        return;
    }
    app_scintilla_send(editor, SCI_GETTEXT, (WPARAM)capacity, (LPARAM)text);
}

static void app_assembler_editor_set_text(HWND editor, const char *text) {
    if (editor != NULL) {
        app_scintilla_send(editor, SCI_SETTEXT, 0, (LPARAM)(text != NULL ? text : ""));
    }
}

static void app_assembler_apply_scintilla_font(HWND editor, const LOGFONTA *font) {
    HDC hdc;
    int dpi;
    int point_size_hundredths;

    if (editor == NULL || font == NULL) {
        return;
    }
    hdc = GetDC(editor);
    dpi = hdc != NULL ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc != NULL) {
        ReleaseDC(editor, hdc);
    }
    if (dpi <= 0) {
        dpi = 96;
    }
    point_size_hundredths = MulDiv(font->lfHeight < 0 ? -font->lfHeight : font->lfHeight, 7200, dpi);
    if (point_size_hundredths <= 0) {
        point_size_hundredths = 1000;
    }
    app_scintilla_send(editor, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)font->lfFaceName);
    app_scintilla_send(editor, SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, point_size_hundredths);
    app_scintilla_send(editor, SCI_STYLESETBOLD, STYLE_DEFAULT, font->lfWeight >= FW_BOLD);
    app_scintilla_send(editor, SCI_STYLESETITALIC, STYLE_DEFAULT, font->lfItalic != 0);
    app_scintilla_send(editor, SCI_STYLESETUNDERLINE, STYLE_DEFAULT, font->lfUnderline != 0);
    app_scintilla_send(editor, SCI_STYLECLEARALL, 0, 0);
}

static void app_assembler_configure_scintilla(HWND editor, const LOGFONTA *font) {
    if (editor == NULL) {
        return;
    }
    app_scintilla_send(editor, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    app_scintilla_send(editor, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    app_scintilla_send(editor, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
    app_scintilla_send(editor, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
    app_scintilla_send(editor, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    app_scintilla_send(editor, SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
    app_scintilla_send(editor, SCI_SETMARGINMASKN, 1, 1u << 0);
    app_scintilla_send(editor, SCI_SETMARGINWIDTHN, 1, 14);
    app_scintilla_send(editor, SCI_SETMARGINWIDTHN, 2, 0);
    app_scintilla_send(editor, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(32, 32, 32));
    app_scintilla_send(editor, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(244, 240, 228));
    app_assembler_apply_scintilla_font(editor, font);
    app_scintilla_send(editor, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(96, 96, 96));
    app_scintilla_send(editor, SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(235, 230, 214));
    app_scintilla_send(editor, SCI_SETCARETLINEBACK, RGB(232, 226, 204), 0);
    app_scintilla_send(editor, SCI_SETCARETLINEVISIBLE, TRUE, 0);
    app_scintilla_send(editor, SCI_MARKERDEFINE, 0, SC_MARK_CIRCLE);
    app_scintilla_send(editor, SCI_MARKERSETFORE, 0, RGB(220, 40, 40));
    app_scintilla_send(editor, SCI_MARKERSETBACK, 0, RGB(220, 40, 40));
}

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
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_FIND, "&Find...\tCtrl+F");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_REPLACE, "&Replace...\tCtrl+H");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_FONT, "Font...\tCtrl+Shift+F");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_FORMAT, "Format Source\tCtrl+Shift+L");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_UPPERCASE, "Uppercase Source\tCtrl+Shift+U");
    AppendMenuA(build_menu, MF_STRING, APP_MENU_ASM_BUILD_ASSEMBLE, "&Assemble\tCtrl+B");
    AppendMenuA(build_menu, MF_STRING, APP_MENU_ASM_BUILD_ASSEMBLE_RUN, "Assemble and &Run\tCtrl+F5");
    AppendMenuA(help_menu, MF_STRING, APP_MENU_ASM_HELP_SHOW, "Assembler &Help\tF1");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)file_menu, "&File");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)edit_menu, "&Edit");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)build_menu, "&Build");
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

/* Clears the current assembler error marker. */
static void app_assembler_clear_error_marker(AppState *app) {
    if (app == NULL) {
        return;
    }
    app->debug.assembler_error_line = 0;
    if (app->debug.assembler_source != NULL) {
        app_scintilla_send(app->debug.assembler_source, SCI_MARKERDELETEALL, 0, 0);
    }
}

/* Moves the assembler caret to the requested 1-based source line and scrolls
   the edit control so the target becomes visible immediately after errors. */
static void app_assembler_focus_line(AppState *app, size_t line_number) {
    LRESULT start_index;

    if (app == NULL || app->debug.assembler_source == NULL || line_number == 0) {
        return;
    }
    start_index = app_scintilla_send(app->debug.assembler_source, SCI_POSITIONFROMLINE, (WPARAM)(line_number - 1), 0);
    if (start_index < 0) {
        return;
    }
    app_scintilla_send(app->debug.assembler_source, SCI_SETSEL, (WPARAM)start_index, (LPARAM)start_index);
    app_scintilla_send(app->debug.assembler_source, SCI_SCROLLCARET, 0, 0);
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
    if (app->debug.assembler_source != NULL) {
        app_scintilla_send(app->debug.assembler_source, SCI_MARKERDELETEALL, 0, 0);
        app_scintilla_send(app->debug.assembler_source, SCI_MARKERADD, (WPARAM)(parsed_line - 1), 0);
    }
    app_assembler_focus_line(app, app->debug.assembler_error_line);
}

/* Keeps Scintilla's line-number margin wide enough for the current document. */
static void app_assembler_update_line_numbers(AppState *app) {
    int line_count;
    int digits = 1;
    char sample[32] = "_9";

    if (app == NULL || app->debug.assembler_source == NULL) {
        return;
    }
    line_count = (int)app_scintilla_send(app->debug.assembler_source, SCI_GETLINECOUNT, 0, 0);
    while (line_count >= 10 && digits < 20) {
        line_count /= 10;
        ++digits;
    }
    sample[0] = '_';
    for (int i = 0; i < digits; ++i) {
        sample[i + 1] = '9';
    }
    sample[digits + 1] = '\0';
    app_scintilla_send(
        app->debug.assembler_source,
        SCI_SETMARGINWIDTHN,
        0,
        app_scintilla_send(app->debug.assembler_source, SCI_TEXTWIDTH, STYLE_LINENUMBER, (LPARAM)sample)
    );
}

/* Sizes the debugger controls to the current client area of the tool window. */

static void app_assembler_layout_controls(AppState *app, HWND hwnd) {
    RECT rect;
    RECT status_rect;
    HDWP positions;
    int width;
    int height;
    int source_height;
    int source_width;
    const int edge_padding = 0;
    int status_height = 0;

    if (app->debug.assembler_source == NULL) {
        return;
    }
    GetClientRect(hwnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    if (app->debug.assembler_status != NULL) {
        GetWindowRect(app->debug.assembler_status, &status_rect);
        status_height = status_rect.bottom - status_rect.top;
    }
    source_width = width - (edge_padding * 2);
    if (source_width < 0) {
        source_width = 0;
    }
    source_height = height - status_height - (edge_padding * 2);
    if (source_height < 0) {
        source_height = 0;
    }
    positions = BeginDeferWindowPos(app->debug.assembler_status != NULL ? 2 : 1);
    if (positions != NULL) {
        positions = DeferWindowPos(
            positions,
            app->debug.assembler_source,
            NULL,
            edge_padding,
            edge_padding,
            source_width,
            source_height,
            SWP_NOACTIVATE | SWP_NOZORDER
        );
    }
    if (positions != NULL && app->debug.assembler_status != NULL) {
        positions = DeferWindowPos(
            positions,
            app->debug.assembler_status,
            NULL,
            0,
            height - status_height,
            width,
            status_height,
            SWP_NOACTIVATE | SWP_NOZORDER
        );
    }
    if (positions != NULL) {
        EndDeferWindowPos(positions);
    } else {
        MoveWindow(
            app->debug.assembler_source,
            edge_padding,
            edge_padding,
            source_width,
            source_height,
            TRUE
        );
        if (app->debug.assembler_status != NULL) {
            MoveWindow(
                app->debug.assembler_status,
                0,
                height - status_height,
                width,
                status_height,
                TRUE
            );
        }
    }
    app_assembler_update_line_numbers(app);
}

static bool app_assembler_create_controls_from_resource(AppState *app, HWND hwnd) {
    if (app_scintilla_module == NULL) {
        app_scintilla_module = LoadLibraryA("Scintilla.dll");
    }
    if (app_scintilla_module == NULL) {
        MessageBoxA(
            hwnd,
            "Scintilla.dll could not be loaded. Rebuild the application or place the DLL beside zx-spectrum.exe.",
            "Spectrum Assembler",
            MB_OK | MB_ICONERROR
        );
        return false;
    }
    app->debug.assembler_panel = CreateDialogParamA(
        GetModuleHandleA(NULL),
        MAKEINTRESOURCEA(APP_DIALOG_ASSEMBLER),
        hwnd,
        app_assembler_dlgproc,
        (LPARAM)app
    );
    return app->debug.assembler_panel != NULL;
}

static LRESULT CALLBACK app_assembler_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    if (app != NULL && app_assembler_find_message != 0 && msg == app_assembler_find_message) {
        return app_assembler_handle_find_message(app, (FINDREPLACEA *)lparam);
    }

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *create = (CREATESTRUCTA *)lparam;
            app = (AppState *)create->lpCreateParams;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            if (app != NULL) {
                app->debug.assembler_hwnd = hwnd;
            }
            return TRUE;
        }
        case WM_CREATE: {
            HMENU menu;
            RECT rect;

            if (app == NULL) {
                return -1;
            }
            menu = app_create_assembler_menu();
            if (menu != NULL) {
                SetMenu(hwnd, menu);
                DrawMenuBar(hwnd);
            }
            if (!app_assembler_create_controls_from_resource(app, hwnd)) {
                return -1;
            }
            GetClientRect(hwnd, &rect);
            MoveWindow(
                app->debug.assembler_panel,
                0,
                0,
                rect.right - rect.left,
                rect.bottom - rect.top,
                TRUE
            );
            return 0;
        }
        case WM_SIZE:
            if (app != NULL && app->debug.assembler_panel != NULL) {
                MoveWindow(app->debug.assembler_panel, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
                app_assembler_layout_controls(app, app->debug.assembler_panel);
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
        case WM_COMMAND:
            if (app != NULL &&
                app->debug.assembler_panel != NULL &&
                SendMessageA(app->debug.assembler_panel, WM_COMMAND, wparam, lparam)) {
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
                if (app->debug.assembler_find_dialog != NULL) {
                    DestroyWindow(app->debug.assembler_find_dialog);
                    app->debug.assembler_find_dialog = NULL;
                }
                app->debug.assembler_hwnd = NULL;
                app->debug.assembler_panel = NULL;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
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
            if (app_scintilla_send(source, SCI_CANUNDO, 0, 0)) {
                app_scintilla_send(source, SCI_UNDO, 0, 0);
            }
            return true;
        case APP_MENU_ASM_EDIT_CUT:
            app_scintilla_send(source, SCI_CUT, 0, 0);
            return true;
        case APP_MENU_ASM_EDIT_COPY:
            app_scintilla_send(source, SCI_COPY, 0, 0);
            return true;
        case APP_MENU_ASM_EDIT_PASTE:
            app_scintilla_send(source, SCI_PASTE, 0, 0);
            return true;
        case APP_MENU_ASM_EDIT_DELETE:
            app_scintilla_send(source, SCI_REPLACESEL, 0, (LPARAM)"");
            return true;
        case APP_MENU_ASM_EDIT_SELECT_ALL:
            app_scintilla_send(source, SCI_SELECTALL, 0, 0);
            return true;
        default:
            return false;
    }
}

static int app_assembler_find_flags(const FINDREPLACEA *find_replace) {
    int flags = SCFIND_NONE;

    if ((find_replace->Flags & FR_MATCHCASE) != 0) {
        flags |= SCFIND_MATCHCASE;
    }
    if ((find_replace->Flags & FR_WHOLEWORD) != 0) {
        flags |= SCFIND_WHOLEWORD;
    }
    return flags;
}

/* Selects the next match from the current selection, wrapping once at the
   end (or start) of the document. */
static bool app_assembler_find_next(AppState *app, const FINDREPLACEA *find_replace) {
    HWND source;
    const char *needle;
    LRESULT document_length;
    LRESULT selection_start;
    LRESULT selection_end;
    LRESULT match;
    bool down;
    bool wrapped = false;

    if (app == NULL || app->debug.assembler_source == NULL || find_replace == NULL) {
        return false;
    }
    source = app->debug.assembler_source;
    needle = find_replace->lpstrFindWhat;
    if (needle == NULL || needle[0] == '\0') {
        app_assembler_set_status(app, "Enter text to find.");
        return false;
    }

    document_length = app_scintilla_send(source, SCI_GETLENGTH, 0, 0);
    selection_start = app_scintilla_send(source, SCI_GETSELECTIONSTART, 0, 0);
    selection_end = app_scintilla_send(source, SCI_GETSELECTIONEND, 0, 0);
    down = (find_replace->Flags & FR_DOWN) != 0;
    app_scintilla_send(source, SCI_SETSEARCHFLAGS, app_assembler_find_flags(find_replace), 0);

    app_scintilla_send(source, SCI_SETTARGETSTART, down ? selection_end : selection_start, 0);
    app_scintilla_send(source, SCI_SETTARGETEND, down ? document_length : 0, 0);
    match = app_scintilla_send(source, SCI_SEARCHINTARGET, strlen(needle), (LPARAM)needle);
    if (match < 0) {
        app_scintilla_send(source, SCI_SETTARGETSTART, down ? 0 : document_length, 0);
        app_scintilla_send(source, SCI_SETTARGETEND, down ? selection_end : selection_start, 0);
        match = app_scintilla_send(source, SCI_SEARCHINTARGET, strlen(needle), (LPARAM)needle);
        wrapped = match >= 0;
    }
    if (match < 0) {
        char status[320];
        snprintf(status, sizeof(status), "Cannot find \"%s\".", needle);
        app_assembler_set_status(app, status);
        MessageBeep(MB_ICONINFORMATION);
        return false;
    }

    app_scintilla_send(
        source,
        SCI_SETSEL,
        app_scintilla_send(source, SCI_GETTARGETSTART, 0, 0),
        app_scintilla_send(source, SCI_GETTARGETEND, 0, 0)
    );
    app_scintilla_send(source, SCI_SCROLLCARET, 0, 0);
    app_assembler_set_status(app, wrapped ? "Match found (search wrapped)." : "Match found.");
    return true;
}

static bool app_assembler_selection_matches(AppState *app, const FINDREPLACEA *find_replace) {
    HWND source = app->debug.assembler_source;
    LRESULT selection_start = app_scintilla_send(source, SCI_GETSELECTIONSTART, 0, 0);
    LRESULT selection_end = app_scintilla_send(source, SCI_GETSELECTIONEND, 0, 0);
    LRESULT match;

    if (selection_start == selection_end || find_replace->lpstrFindWhat[0] == '\0') {
        return false;
    }
    app_scintilla_send(source, SCI_SETSEARCHFLAGS, app_assembler_find_flags(find_replace), 0);
    app_scintilla_send(source, SCI_SETTARGETSTART, selection_start, 0);
    app_scintilla_send(source, SCI_SETTARGETEND, selection_end, 0);
    match = app_scintilla_send(
        source,
        SCI_SEARCHINTARGET,
        strlen(find_replace->lpstrFindWhat),
        (LPARAM)find_replace->lpstrFindWhat
    );
    return match == selection_start &&
        app_scintilla_send(source, SCI_GETTARGETEND, 0, 0) == selection_end;
}

static void app_assembler_replace_selection(AppState *app, FINDREPLACEA *find_replace) {
    HWND source = app->debug.assembler_source;
    LRESULT selection_start;
    size_t replacement_length;

    if (!app_assembler_selection_matches(app, find_replace)) {
        app_assembler_find_next(app, find_replace);
        return;
    }
    selection_start = app_scintilla_send(source, SCI_GETSELECTIONSTART, 0, 0);
    replacement_length = strlen(find_replace->lpstrReplaceWith);
    app_scintilla_send(source, SCI_REPLACETARGET, replacement_length, (LPARAM)find_replace->lpstrReplaceWith);
    app_scintilla_send(source, SCI_SETSEL, selection_start, selection_start + (LRESULT)replacement_length);
    app_assembler_find_next(app, find_replace);
}

static void app_assembler_replace_all(AppState *app, FINDREPLACEA *find_replace) {
    HWND source = app->debug.assembler_source;
    const char *needle = find_replace->lpstrFindWhat;
    const char *replacement = find_replace->lpstrReplaceWith;
    LRESULT document_length;
    LRESULT search_start = 0;
    LRESULT match;
    size_t needle_length;
    size_t replacement_length;
    unsigned int replacements = 0;
    char status[128];

    if (needle == NULL || needle[0] == '\0') {
        app_assembler_set_status(app, "Enter text to find.");
        return;
    }
    needle_length = strlen(needle);
    replacement_length = strlen(replacement);
    document_length = app_scintilla_send(source, SCI_GETLENGTH, 0, 0);
    app_scintilla_send(source, SCI_SETSEARCHFLAGS, app_assembler_find_flags(find_replace), 0);
    app_scintilla_send(source, SCI_BEGINUNDOACTION, 0, 0);
    while (search_start <= document_length) {
        LRESULT match_end;

        app_scintilla_send(source, SCI_SETTARGETSTART, search_start, 0);
        app_scintilla_send(source, SCI_SETTARGETEND, document_length, 0);
        match = app_scintilla_send(source, SCI_SEARCHINTARGET, needle_length, (LPARAM)needle);
        if (match < 0) {
            break;
        }
        match_end = app_scintilla_send(source, SCI_GETTARGETEND, 0, 0);
        app_scintilla_send(source, SCI_REPLACETARGET, replacement_length, (LPARAM)replacement);
        document_length += (LRESULT)replacement_length - (match_end - match);
        search_start = match + (LRESULT)replacement_length;
        ++replacements;
    }
    app_scintilla_send(source, SCI_ENDUNDOACTION, 0, 0);
    snprintf(status, sizeof(status), "%u replacement%s made.", replacements, replacements == 1 ? "" : "s");
    app_assembler_set_status(app, status);
    if (replacements == 0) {
        MessageBeep(MB_ICONINFORMATION);
    }
}

static void app_assembler_seed_find_text(AppState *app) {
    HWND source = app->debug.assembler_source;
    LRESULT selection_start = app_scintilla_send(source, SCI_GETSELECTIONSTART, 0, 0);
    LRESULT selection_end = app_scintilla_send(source, SCI_GETSELECTIONEND, 0, 0);
    LRESULT selection_length = selection_end - selection_start;

    if (selection_length > 0 && selection_length < (LRESULT)sizeof(app->debug.assembler_find_text)) {
        app_scintilla_send(source, SCI_GETSELTEXT, 0, (LPARAM)app->debug.assembler_find_text);
        if (strchr(app->debug.assembler_find_text, '\r') != NULL ||
            strchr(app->debug.assembler_find_text, '\n') != NULL) {
            app->debug.assembler_find_text[0] = '\0';
        }
    }
}

static void app_assembler_show_find_dialog(HWND owner, AppState *app, bool replace) {
    FINDREPLACEA *find_replace;
    HWND dialog;

    if (app == NULL || app->debug.assembler_source == NULL) {
        return;
    }
    if (app_assembler_find_message == 0) {
        app_assembler_find_message = RegisterWindowMessageA(FINDMSGSTRINGA);
        if (app_assembler_find_message == 0) {
            app_assembler_set_status(app, "Could not initialise the search dialog.");
            return;
        }
    }
    if (app->debug.assembler_find_dialog != NULL) {
        if (app->debug.assembler_find_dialog_is_replace == replace) {
            SetForegroundWindow(app->debug.assembler_find_dialog);
            return;
        }
        DestroyWindow(app->debug.assembler_find_dialog);
        app->debug.assembler_find_dialog = NULL;
    }
    app_assembler_seed_find_text(app);
    ZeroMemory(&app->debug.assembler_find_replace, sizeof(app->debug.assembler_find_replace));
    find_replace = &app->debug.assembler_find_replace;
    find_replace->lStructSize = sizeof(*find_replace);
    find_replace->hwndOwner = owner;
    find_replace->Flags = FR_DOWN;
    find_replace->lpstrFindWhat = app->debug.assembler_find_text;
    find_replace->wFindWhatLen = sizeof(app->debug.assembler_find_text);
    find_replace->lpstrReplaceWith = app->debug.assembler_replace_text;
    find_replace->wReplaceWithLen = sizeof(app->debug.assembler_replace_text);
    dialog = replace ? ReplaceTextA(find_replace) : FindTextA(find_replace);
    if (dialog == NULL) {
        app_assembler_set_status(app, "Could not open the search dialog.");
        return;
    }
    app->debug.assembler_find_dialog = dialog;
    app->debug.assembler_find_dialog_is_replace = replace;
}

static LRESULT app_assembler_handle_find_message(AppState *app, FINDREPLACEA *find_replace) {
    if ((find_replace->Flags & FR_DIALOGTERM) != 0) {
        app->debug.assembler_find_dialog = NULL;
        SetFocus(app->debug.assembler_source);
    } else if ((find_replace->Flags & FR_FINDNEXT) != 0) {
        app_assembler_find_next(app, find_replace);
    } else if ((find_replace->Flags & FR_REPLACE) != 0) {
        app_assembler_replace_selection(app, find_replace);
    } else if ((find_replace->Flags & FR_REPLACEALL) != 0) {
        app_assembler_replace_all(app, find_replace);
    }
    return 0;
}

/* Adds assembler-specific shortcuts to the Scintilla editor. */
static LRESULT CALLBACK app_assembler_source_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    HWND parent = GetParent(hwnd);
    AppState *app = parent != NULL ? (AppState *)GetWindowLongPtrA(parent, GWLP_USERDATA) : NULL;
    HWND command_target = (app != NULL && app->debug.assembler_hwnd != NULL) ? app->debug.assembler_hwnd : parent;
    WNDPROC old_proc = (app != NULL) ? app->debug.assembler_source_wndproc : DefWindowProcA;
    LRESULT result;

    if (app != NULL && msg == WM_CHAR && app->debug.assembler_suppress_next_char) {
        app->debug.assembler_suppress_next_char = false;
        if (wparam > 0 && wparam < 32) {
            return 0;
        }
    }

    if (app != NULL && msg == WM_KEYDOWN) {
        const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if ((UINT)wparam == VK_F1) {
            SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_HELP_SHOW, 0);
            return 0;
        }
        if ((UINT)wparam == VK_F5 && ctrl_down) {
            app->debug.assembler_suppress_next_char = true;
            SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_BUILD_ASSEMBLE_RUN, 0);
            return 0;
        }
        if ((UINT)wparam == VK_F5) {
            SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_FILE_RELOAD, 0);
            return 0;
        }

        if (ctrl_down && !shift_down) {
            switch ((UINT)wparam) {
                case 'N':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_FILE_NEW, 0);
                    return 0;
                case 'O':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_FILE_LOAD, 0);
                    return 0;
                case 'S':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_FILE_SAVE, 0);
                    return 0;
                case 'R':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_FILE_RELOAD, 0);
                    return 0;
                case 'B':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_BUILD_ASSEMBLE, 0);
                    return 0;
                case 'F':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_EDIT_FIND, 0);
                    return 0;
                case 'H':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_EDIT_REPLACE, 0);
                    return 0;
                default:
                    break;
            }
        }
        if (ctrl_down && shift_down) {
            switch ((UINT)wparam) {
                case 'S':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_FILE_SAVE_AS, 0);
                    return 0;
                case 'F':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_EDIT_FONT, 0);
                    return 0;
                case 'L':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_EDIT_FORMAT, 0);
                    return 0;
                case 'U':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_EDIT_UPPERCASE, 0);
                    return 0;
                case 'T':
                    app->debug.assembler_suppress_next_char = true;
                    SendMessageA(command_target, WM_COMMAND, APP_MENU_ASM_FILE_EXPORT_TAP, 0);
                    return 0;
                default:
                    break;
            }
        }
    }

    result = CallWindowProcA(old_proc, hwnd, msg, wparam, lparam);

    return result;
}


static INT_PTR CALLBACK app_assembler_dlgproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_INITDIALOG: {
            LOGFONTA assembler_font;

            app = (AppState *)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            if (app == NULL) {
                return FALSE;
            }
            app->debug.assembler_source_brush = CreateSolidBrush(RGB(244, 240, 228));
            if (app_assembler_load_saved_font(&assembler_font)) {
                app->debug.assembler_font = app_create_assembler_font_from_logfont(&assembler_font);
            } else {
                app_default_assembler_logfont(&assembler_font);
                app->debug.assembler_font = app_create_assembler_font_from_logfont(&assembler_font);
            }
            SetTimer(hwnd, APP_TIMER_ASM_FILE_WATCH, 1000, NULL);
            app->debug.assembler_status = GetDlgItem(hwnd, APP_CTRL_ASM_STATUS);
            app->debug.assembler_source = GetDlgItem(hwnd, APP_CTRL_ASM_SOURCE);
            app_assembler_configure_scintilla(app->debug.assembler_source, &assembler_font);
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
            return FALSE;
        }
        case WM_CTLCOLORDLG:
            if (app != NULL && app->debug.assembler_source_brush != NULL) {
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)app->debug.assembler_source_brush);
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (app != NULL && app->debug.assembler_source_brush != NULL) {
                HDC hdc = (HDC)wparam;
                SetBkColor(hdc, RGB(244, 240, 228));
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)app->debug.assembler_source_brush);
                return TRUE;
            }
            break;
        case WM_CTLCOLORSCROLLBAR:
            if (app != NULL && app->debug.assembler_source_brush != NULL) {
                SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, (LONG_PTR)app->debug.assembler_source_brush);
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            if (app != NULL && app->debug.assembler_source_brush != NULL) {
                RECT background_rect;
                HDC hdc = (HDC)wparam;
                GetClientRect(hwnd, &background_rect);
                FillRect(hdc, &background_rect, app->debug.assembler_source_brush);
                return TRUE;
            }
            break;
        case WM_SIZE:
            if (app != NULL) {
                app_assembler_layout_controls(app, hwnd);
            }
            return TRUE;
        case WM_TIMER:
            if (app != NULL && wparam == APP_TIMER_ASM_FILE_WATCH) {
                app_assembler_poll_external_change(hwnd, app);
                return TRUE;
            }
            break;
        case WM_NOTIFY:
            if (app != NULL && lparam != 0) {
                SCNotification *notification = (SCNotification *)lparam;
                if (notification->nmhdr.idFrom == APP_CTRL_ASM_SOURCE &&
                    notification->nmhdr.code == SCN_MODIFIED &&
                    (notification->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) != 0) {
                    if (!app->debug.assembler_ignore_change) {
                        app->debug.assembler_dirty = true;
                        app_assembler_clear_error_marker(app);
                        app_assembler_set_status(app, "");
                    }
                    app_assembler_update_line_numbers(app);
                    return TRUE;
                }
            }
            break;
        case WM_COMMAND:
            if (app == NULL) {
                break;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_HELP || LOWORD(wparam) == APP_MENU_ASM_HELP_SHOW) {
                app_show_assembler_help(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_EDIT_FONT) {
                app_assembler_choose_font(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_EDIT_FIND || LOWORD(wparam) == APP_MENU_ASM_EDIT_REPLACE) {
                app_assembler_show_find_dialog(
                    app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd,
                    app,
                    LOWORD(wparam) == APP_MENU_ASM_EDIT_REPLACE
                );
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_EDIT_FORMAT) {
                app_assembler_format_source(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_EDIT_UPPERCASE) {
                app_assembler_uppercase_source(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_NEW) {
                char status[512];
                app_assembler_new_source(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_LOAD || LOWORD(wparam) == APP_MENU_ASM_FILE_LOAD) {
                char status[512];
                app_assembler_load_source(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_RELOAD) {
                char status[512];
                app_assembler_reload_source(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_SAVE || LOWORD(wparam) == APP_MENU_ASM_FILE_SAVE) {
                char status[512];
                app_assembler_save_source(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_SAVE_AS) {
                char status[512];
                app_assembler_save_source_as(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app, status, sizeof(status));
                app_assembler_set_status(app, status);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_FILE_EXPORT_TAP) {
                char status[512];
                if (!app_assembler_export_tap(app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd, app, status, sizeof(status))) {
                    app_assembler_sync_error_from_status(app, status);
                }
                app_assembler_set_status(app, status);
                return TRUE;
            }
            if (LOWORD(wparam) == APP_CTRL_ASM_APPLY || LOWORD(wparam) == APP_MENU_ASM_BUILD_ASSEMBLE) {
                app_assembler_apply_source(
                    app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd,
                    app,
                    false
                );
                return TRUE;
            }
            if (LOWORD(wparam) == APP_MENU_ASM_BUILD_ASSEMBLE_RUN) {
                app_assembler_apply_source(
                    app->debug.assembler_hwnd != NULL ? app->debug.assembler_hwnd : hwnd,
                    app,
                    true
                );
                return TRUE;
            }
            if (app_assembler_handle_edit_command(app, LOWORD(wparam))) {
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (app != NULL) {
                KillTimer(hwnd, APP_TIMER_ASM_FILE_WATCH);
                if (app->debug.assembler_source_brush != NULL) {
                    DeleteObject(app->debug.assembler_source_brush);
                    app->debug.assembler_source_brush = NULL;
                }
                if (app->debug.assembler_font != NULL) {
                    DeleteObject(app->debug.assembler_font);
                    app->debug.assembler_font = NULL;
                }
                app->debug.assembler_panel = NULL;
                app->debug.assembler_source = NULL;
                app->debug.assembler_status = NULL;
                app->debug.assembler_source_wndproc = NULL;
                app->debug.assembler_dirty = false;
                app->debug.assembler_ignore_change = false;
                app->debug.assembler_suppress_next_char = false;
                app->debug.assembler_has_file_write_time = false;
                app->debug.assembler_file_change_prompt_active = false;
                app->debug.assembler_error_line = 0;
                ZeroMemory(&app->debug.assembler_file_write_time, sizeof(app->debug.assembler_file_write_time));
            }
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

/* Registers the custom assembler frame and line-number gutter classes. */
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
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        700,
        460,
        app->main_hwnd,
        NULL,
        GetModuleHandleA(NULL),
        app
    );
    if (app->debug.assembler_hwnd == NULL) {
        return;
    }
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
        "  Ctrl+F5 Assemble and run from ORG\r\n"
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
    app_assembler_editor_set_text(app->debug.assembler_source, normalized);
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
    int source_length = app_assembler_editor_text_length(app->debug.assembler_source);
    char *source;
    FILE *file;

    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }
    app_assembler_editor_get_text(app->debug.assembler_source, source, source_length + 1);

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

    source_length = app_assembler_editor_text_length(app->debug.assembler_source);
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
    app_assembler_editor_get_text(app->debug.assembler_source, source, source_length + 1);
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
    app_assembler_editor_set_text(app->debug.assembler_source, "");
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
static void app_assembler_apply_source(HWND hwnd, AppState *app, bool run_after_assembly) {
    char status[512];
    uint16_t effective_address;
    uint16_t org_address;
    int source_length = app_assembler_editor_text_length(app->debug.assembler_source);
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
    app_assembler_editor_get_text(app->debug.assembler_source, source, source_length + 1);
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
        if (run_after_assembly) {
            size_t used = strlen(status);
            app_debug_set_view_address(app, effective_address, true);
            app_debug_run_from_address(app, effective_address);
            snprintf(
                status + used,
                sizeof(status) - used,
                " Running from %04Xh.",
                effective_address
            );
        }
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

    if (app->debug.assembler_source != NULL) {
        app_assembler_apply_scintilla_font(app->debug.assembler_source, font);
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

    source_length = app_assembler_editor_text_length(app->debug.assembler_source);
    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        app_assembler_set_status(app, "Out of memory.");
        return;
    }
    app_assembler_editor_get_text(app->debug.assembler_source, source, source_length + 1);

    if (!app_assembler_format_source_text(source, &formatted)) {
        free(source);
        app_assembler_set_status(app, "Could not format assembler source.");
        return;
    }

    app->debug.assembler_ignore_change = true;
    app_assembler_editor_set_text(app->debug.assembler_source, formatted);
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

    source_length = app_assembler_editor_text_length(app->debug.assembler_source);
    source = (char *)malloc((size_t)source_length + 1);
    if (source == NULL) {
        app_assembler_set_status(app, "Out of memory.");
        return;
    }
    app_assembler_editor_get_text(app->debug.assembler_source, source, source_length + 1);

    if (!app_assembler_uppercase_source_text(source, &uppercase_text)) {
        free(source);
        app_assembler_set_status(app, "Could not convert assembler source to upper case.");
        return;
    }

    app->debug.assembler_ignore_change = true;
    app_assembler_editor_set_text(app->debug.assembler_source, uppercase_text);
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
