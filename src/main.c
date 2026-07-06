#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <xinput.h>

#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "tape.h"

typedef struct KeyMap {
    UINT vk;
    int code;
} KeyMap;

typedef struct AppState AppState;
typedef struct RomSet RomSet;
typedef struct RomSelection RomSelection;

enum {
    ZX_KEY_CAPS_SHIFT = 1,
    ZX_KEY_SYMBOL_SHIFT = 2,
    APP_AUTOTYPE_CAPACITY = 4096,
    APP_AUTOTYPE_HOLD_FRAMES = 10,
    APP_AUTOTYPE_COOLDOWN_FRAMES = 6,
    APP_AUTOTYPE_BOOT_DELAY_FRAMES = 150,
    APP_AUDIO_SAMPLE_RATE = 44100,
    APP_AUDIO_CALLBACK_SAMPLES = 128,
    APP_AUDIO_BUFFER_SAMPLES = 1024,
    APP_AUDIO_BUFFER_COUNT = 4,
    APP_AUDIO_RING_SAMPLES = 16384,
    APP_MENU_FILE_OPEN_SNAPSHOT = 1001,
    APP_MENU_FILE_RESET = 1002,
    APP_MENU_FILE_EXIT = 1003,
    APP_MENU_FILE_PLAY_TAPE = 1004,
    APP_MENU_FILE_STOP_TAPE = 1005,
    APP_MENU_FILE_AUTOLOAD_TAPES = 1006,
    APP_MENU_TOOLS_ASSEMBLER = 1301,
    APP_MENU_TOOLS_DEBUGGER = 1302,
    APP_MENU_MACHINE_48K = 1201,
    APP_MENU_MACHINE_128K = 1202,
    APP_CTRL_DEBUG_TEXT = 2001,
    APP_CTRL_DEBUG_PAUSE = 2002,
    APP_CTRL_DEBUG_STEP = 2003,
    APP_CTRL_DEBUG_STEP_OVER = 2004,
    APP_CTRL_DEBUG_REFRESH = 2005,
    APP_CTRL_DEBUG_ADDRESS = 2006,
    APP_CTRL_DEBUG_GO = 2007,
    APP_CTRL_DEBUG_SYNC = 2008,
    APP_CTRL_DEBUG_BREAKPOINT_TOGGLE = 2009,
    APP_CTRL_DEBUG_RUN_AT = 2010,
    APP_CTRL_DEBUG_RUN_TO = 2011,
    APP_CTRL_DEBUG_WATCHPOINT_TOGGLE = 2012,
    APP_CTRL_DEBUG_REMOVE_SELECTED = 2013,
    APP_CTRL_DEBUG_POINTS_LIST = 2014,
    APP_CTRL_DEBUG_VIEW_PC = 2015,
    APP_CTRL_DEBUG_VIEW_SP = 2016,
    APP_CTRL_DEBUG_PAGE_UP = 2017,
    APP_CTRL_DEBUG_PAGE_DOWN = 2018,
    APP_CTRL_ASM_SOURCE = 2102,
    APP_CTRL_ASM_APPLY = 2103,
    APP_CTRL_ASM_STATUS = 2104,
    APP_CTRL_ASM_HELP = 2105,
    APP_CTRL_ASM_LOAD = 2106,
    APP_CTRL_ASM_SAVE = 2107,
    APP_CTRL_ASM_LINES = 2108,
    APP_MENU_ASM_FILE_NEW = 2200,
    APP_MENU_ASM_FILE_LOAD = 2201,
    APP_MENU_ASM_FILE_SAVE = 2202,
    APP_MENU_ASM_FILE_SAVE_AS = 2203,
    APP_MENU_ASM_EDIT_UNDO = 2210,
    APP_MENU_ASM_EDIT_CUT = 2211,
    APP_MENU_ASM_EDIT_COPY = 2212,
    APP_MENU_ASM_EDIT_PASTE = 2213,
    APP_MENU_ASM_EDIT_DELETE = 2214,
    APP_MENU_ASM_EDIT_SELECT_ALL = 2215,
    APP_MENU_ASM_BUILD_ASSEMBLE = 2221,
    APP_MENU_ASM_HELP_SHOW = 2231,
    APP_TIMER_MODAL_LOOP = 3001,
    APP_MSG_TAPE_LOAD_COMPLETE = WM_APP + 1,
    APP_MSG_SNAPSHOT_LOAD_COMPLETE = WM_APP + 2
};

typedef struct AudioBuffer {
    WAVEHDR header;
    int16_t samples[APP_AUDIO_BUFFER_SAMPLES];
} AudioBuffer;

typedef struct AudioState {
    HWAVEOUT device;
    AudioBuffer buffers[APP_AUDIO_BUFFER_COUNT];
    int16_t ring[APP_AUDIO_RING_SAMPLES];
    size_t ring_read;
    size_t ring_write;
    size_t ring_count;
    bool enabled;
} AudioState;

typedef DWORD (WINAPI *AppXInputGetStateFn)(DWORD user_index, XINPUT_STATE *state);

typedef struct AutoTypeState {
    char buffer[APP_AUTOTYPE_CAPACITY];
    size_t length;
    size_t position;
    int cooldown_frames;
    int hold_frames;
    int active_key_code;
} AutoTypeState;

typedef struct AppTapeLoadJob {
    HWND hwnd;
    char path[MAX_PATH];
    uint32_t tick_hz;
    TapePlayer tape;
    char error[256];
    bool ok;
} AppTapeLoadJob;

typedef struct AppSnapshotLoadJob {
    HWND hwnd;
    char path[MAX_PATH];
    uint8_t *data;
    size_t size;
    SpectrumModel model;
    char error[256];
    bool ok;
} AppSnapshotLoadJob;

struct RomSet {
    char rom_a[MAX_PATH];
    char rom_b[MAX_PATH];
    bool has_rom_a;
    bool has_rom_b;
};

typedef struct ControllerState {
    HMODULE xinput_module;
    AppXInputGetStateFn xinput_get_state;
    DWORD active_user_index;
    uint8_t joystick_mask;
    bool available;
    bool connected;
} ControllerState;

typedef struct DebugState {
    HWND debugger_hwnd;
    HWND debugger_text;
    HWND debugger_pause_button;
    HWND debugger_step_button;
    HWND debugger_step_over_button;
    HWND debugger_refresh_button;
    HWND debugger_address_edit;
    HWND debugger_go_button;
    HWND debugger_sync_checkbox;
    HWND debugger_breakpoint_toggle_button;
    HWND debugger_run_at_button;
    HWND debugger_run_to_button;
    HWND debugger_watchpoint_toggle_button;
    HWND debugger_remove_selected_button;
    HWND debugger_view_pc_button;
    HWND debugger_view_sp_button;
    HWND debugger_page_up_button;
    HWND debugger_page_down_button;
    HWND debugger_points_list;
    HACCEL debugger_accel;
    WNDPROC debugger_address_edit_wndproc;
    WNDPROC debugger_points_list_wndproc;
    HWND assembler_hwnd;
    HWND assembler_line_numbers;
    HWND assembler_source;
    HWND assembler_status;
    HWND assembler_apply_button;
    HWND assembler_help_button;
    HWND assembler_load_button;
    HWND assembler_save_button;
    HFONT assembler_font;
    WNDPROC assembler_source_wndproc;
    bool assembler_dirty;
    bool assembler_ignore_change;
    size_t assembler_error_line;
    char assembler_current_path[MAX_PATH];
    bool paused;
    bool stepping;
    bool stop_requested;
    bool breakpoint_hit;
    bool watchpoint_hit;
    bool debugger_view_sync_pc;
    bool debugger_view_initialized;
    bool debugger_run_to_active;
    bool debugger_run_to_hit;
    bool debugger_watchpoint_pending;
    bool debugger_skip_breakpoint_once;
    uint16_t debugger_view_address;
    uint16_t last_breakpoint_address;
    uint16_t debugger_run_to_address;
    uint16_t last_run_to_address;
    uint16_t debugger_skip_breakpoint_address;
    uint16_t last_watchpoint_address;
    uint16_t pending_watchpoint_address;
    uint16_t *breakpoints;
    size_t breakpoint_count;
    size_t breakpoint_capacity;
    uint16_t *watchpoints;
    size_t watchpoint_count;
    size_t watchpoint_capacity;
    uint8_t last_watchpoint_value;
    uint8_t pending_watchpoint_value;
    uint64_t last_pins;
} DebugState;

typedef struct AssemblerLabel {
    char name[64];
    uint16_t address;
} AssemblerLabel;

typedef struct AssemblerValue {
    int value;
    bool resolved;
    bool numeric_literal;
} AssemblerValue;

typedef struct AssemblerContext {
    AppState *app;
    uint16_t start_address;
    uint16_t address;
    size_t total_written;
    int pass;
    AssemblerLabel labels[512];
    size_t label_count;
} AssemblerContext;

/* Stores the selected machine type and the resolved ROM paths after command
   line parsing and fallback autodetection have been applied. */
struct RomSelection {
    SpectrumModel model;
    RomSet rom_48k;
    RomSet rom_128k;
};

struct AppState {
    Spectrum spec;
    RomSelection roms;
    BITMAPINFO bitmap_info;
    HWND main_hwnd;
    LARGE_INTEGER perf_freq;
    LARGE_INTEGER last_tick;
    double emulation_accumulator_ms;
    AudioState audio;
    ControllerState controller;
    AutoTypeState auto_type;
    TapePlayer tape;
    DebugState debug;
    int active_key_codes[256];
    bool tape_autoload_enabled;
    bool tape_autoplay_pending;
    bool tape_load_in_progress;
    bool snapshot_load_in_progress;
    bool running;
    bool modal_loop_timer_active;
};

typedef struct AssemblerSourceLocation {
    char path[MAX_PATH];
    size_t line_number;
} AssemblerSourceLocation;

typedef struct AssemblerPreparedSource {
    char *text;
    size_t text_length;
    size_t text_capacity;
    AssemblerSourceLocation *locations;
    size_t line_count;
    size_t line_capacity;
} AssemblerPreparedSource;

static const KeyMap SPECIAL_KEY_MAP[] = {
    {VK_SHIFT, ZX_KEY_CAPS_SHIFT},
    {VK_CONTROL, ZX_KEY_SYMBOL_SHIFT},
    {VK_RETURN, 0x0D},
    {VK_BACK, 0x0C},
    {VK_LEFT, 0x08},
    {VK_RIGHT, 0x09},
    {VK_DOWN, 0x0A},
    {VK_UP, 0x0B},
};

static void app_show_error(const char *message);
static bool app_start_autotype(AppState *app, const WCHAR *text);
static bool app_tape_input_callback(void *user_data, uint64_t tick_count);
static bool app_tape_fast_load_trap(void *user_data, void *machine);
static DWORD WINAPI app_tape_load_worker(void *param);
static DWORD WINAPI app_snapshot_load_worker(void *param);
static bool app_parent_dir(char *path);
static bool app_file_exists(const char *path);
static bool app_join_path(char *out, size_t out_size, const char *left, const char *right);
static bool app_read_file_all(
    const char *path,
    uint8_t **buffer,
    size_t *size,
    char *error_buffer,
    size_t error_buffer_size,
    const char *kind
);
static RomSet *app_rom_set_for_model(RomSelection *selection, SpectrumModel model);
static const RomSet *app_rom_set_for_model_const(const RomSelection *selection, SpectrumModel model);
static bool app_rom_set_is_available(const RomSet *set);
static void app_rom_set_set_paths(RomSet *set, const char *rom_a, const char *rom_b);
static bool app_load_model_roms(
    AppState *app,
    SpectrumModel model,
    const RomSet *set,
    char *error_buffer,
    size_t error_buffer_size
);
static bool app_parse_number(const char *text, int *value);
static double app_model_frame_duration_ms(SpectrumModel model);
static void app_tick_emulator(AppState *app);
static bool app_load_tape_file(HWND hwnd, AppState *app, const char *path, char *error_buffer, size_t error_buffer_size);
static bool app_apply_loaded_tape(HWND hwnd, AppState *app, TapePlayer *loaded_tape, char *error_buffer, size_t error_buffer_size);
static bool app_load_snapshot_file(HWND hwnd, AppState *app, const char *path, char *error_buffer, size_t error_buffer_size);
static bool app_apply_loaded_snapshot(HWND hwnd, AppState *app, AppSnapshotLoadJob *job, char *error_buffer, size_t error_buffer_size);
static void app_play_tape(AppState *app);
static void app_stop_tape(AppState *app);
static void app_sync_tape_stop_mode(AppState *app);
static void app_set_modal_loop_timer(AppState *app, HWND hwnd, bool enabled);
static void app_controller_init(AppState *app);
static void app_controller_shutdown(AppState *app);
static void app_controller_poll(AppState *app);
static bool app_debug_has_breakpoint(const AppState *app, uint16_t address);
static bool app_debug_reserve_breakpoints(AppState *app, size_t required_count);
static bool app_debug_has_watchpoint(const AppState *app, uint16_t address);
static bool app_debug_reserve_watchpoints(AppState *app, size_t required_count);
static bool app_debug_toggle_breakpoint(AppState *app, uint16_t address, bool *enabled);
static bool app_debug_toggle_watchpoint(AppState *app, uint16_t address, bool *enabled);
static void app_debug_refresh_points_list(AppState *app);
static bool app_debug_remove_selected_point(AppState *app);
static void app_debug_free_storage(AppState *app);
static void app_debug_append_point_summary(char *buffer, size_t buffer_size, size_t *used, const AppState *app);
static bool app_debug_is_step_over_candidate(const Spectrum *spec, uint16_t address);
static void app_debug_resume(AppState *app);
static void app_debug_step_over_instruction(AppState *app);
static void app_debug_navigate_view(AppState *app, int delta);
static void app_debug_view_pc(AppState *app);
static void app_debug_view_sp(AppState *app);
static void app_debug_run_to_address(AppState *app, uint16_t address);
static void app_debug_run_frame(AppState *app);
static void app_debug_run_from_address(AppState *app, uint16_t address);
static HMENU app_create_assembler_menu(void);
static void app_save_model(SpectrumModel model);
static void app_save_tape_autoload(bool enabled);
static void app_update_model_menu(HWND hwnd, SpectrumModel model);
static void app_audio_callback(const float *samples, int num_samples, void *user_data);
static void app_debug_refresh_window(AppState *app);
static void app_assembler_refresh_window(AppState *app);
static void app_debug_machine_changed(AppState *app);
static bool app_register_tool_window_classes(HINSTANCE instance);
static void app_open_debugger_window(AppState *app);
static void app_open_assembler_window(AppState *app);
static void app_show_assembler_help(HWND hwnd);
static void app_assembler_set_status(AppState *app, const char *text);
static void app_assembler_update_title(AppState *app);
static void app_assembler_set_current_path(AppState *app, const char *path);
static bool app_assembler_load_last_path(char *out_path, size_t out_size);
static void app_assembler_save_last_path(const char *path);
static bool app_assembler_load_source_path(const char *path, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_load_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_write_source_to_path(AppState *app, const char *path, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_save_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_save_source_as(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_new_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static void app_assembler_apply_source(HWND hwnd, AppState *app);
static bool app_assembler_confirm_close(HWND hwnd, AppState *app);
static void app_assembler_free_prepared_source(AssemblerPreparedSource *prepared);
static bool app_assembler_prepare_source(
    AppState *app,
    const char *source,
    AssemblerPreparedSource *prepared,
    char *status_buffer,
    size_t status_buffer_size
);
static void app_assembler_format_location_error(
    const AssemblerSourceLocation *location,
    size_t fallback_line,
    const char *message,
    char *out_error,
    size_t out_error_size
);
static bool app_assembler_find_source_org(const char *source, uint16_t *out_address);
static HFONT app_create_monospace_font(void);
static bool app_assembler_handle_edit_command(AppState *app, UINT command_id);
static void app_assembler_clear_error_marker(AppState *app);
static void app_assembler_focus_line(AppState *app, size_t line_number);
static void app_assembler_sync_error_from_status(AppState *app, const char *status_text);
static void app_assembler_update_line_numbers(AppState *app);
static LRESULT CALLBACK app_debugger_address_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_debugger_points_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_assembler_gutter_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_assembler_source_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

/* Creates the application's menu bar so snapshot loading and other emulator
   actions are available from the standard Windows chrome. */
static HMENU app_create_menu(void) {
    HMENU menu_bar = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU tape_menu = CreatePopupMenu();
    HMENU machine_menu = CreatePopupMenu();
    HMENU tools_menu = CreatePopupMenu();

    if (menu_bar == NULL || file_menu == NULL || tape_menu == NULL || machine_menu == NULL || tools_menu == NULL) {
        if (tools_menu != NULL) {
            DestroyMenu(tools_menu);
        }
        if (machine_menu != NULL) {
            DestroyMenu(machine_menu);
        }
        if (tape_menu != NULL) {
            DestroyMenu(tape_menu);
        }
        if (file_menu != NULL) {
            DestroyMenu(file_menu);
        }
        if (menu_bar != NULL) {
            DestroyMenu(menu_bar);
        }
        return NULL;
    }

    AppendMenuA(file_menu, MF_STRING, APP_MENU_FILE_OPEN_SNAPSHOT, "&Open Tape/Snapshot...\tCtrl+O");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_FILE_RESET, "&Reset\tCtrl+R");
    AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file_menu, MF_STRING, APP_MENU_FILE_EXIT, "E&xit\tAlt+F4");
    AppendMenuA(tape_menu, MF_STRING, APP_MENU_FILE_PLAY_TAPE, "&Play Tape\tF3");
    AppendMenuA(tape_menu, MF_STRING, APP_MENU_FILE_STOP_TAPE, "S&top Tape\tF4");
    AppendMenuA(tape_menu, MF_STRING, APP_MENU_FILE_AUTOLOAD_TAPES, "&Auto-load Tapes On Open");
    AppendMenuA(machine_menu, MF_STRING, APP_MENU_MACHINE_48K, "&48K\tCtrl+1");
    AppendMenuA(machine_menu, MF_STRING, APP_MENU_MACHINE_128K, "&128K\tCtrl+2");
    AppendMenuA(tools_menu, MF_STRING, APP_MENU_TOOLS_ASSEMBLER, "&Assembler...\tCtrl+Shift+A");
    AppendMenuA(tools_menu, MF_STRING, APP_MENU_TOOLS_DEBUGGER, "&Debugger...\tCtrl+Shift+D");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)file_menu, "&File");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)tape_menu, "&Tape");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)machine_menu, "&Machine");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)tools_menu, "&Tools");
    return menu_bar;
}

static const char *app_model_name(SpectrumModel model) {
    switch (model) {
        case SPECTRUM_MODEL_128K: return "128K";
        case SPECTRUM_MODEL_48K:
        default:
            return "48K";
    }
}

static UINT app_model_menu_id(SpectrumModel model) {
    switch (model) {
        case SPECTRUM_MODEL_128K: return APP_MENU_MACHINE_128K;
        case SPECTRUM_MODEL_48K:
        default:
            return APP_MENU_MACHINE_48K;
    }
}

static const char *app_model_settings_value(SpectrumModel model) {
    switch (model) {
        case SPECTRUM_MODEL_128K: return "128";
        case SPECTRUM_MODEL_48K:
        default:
            return "48";
    }
}

static const char *app_tape_autoload_settings_value(bool enabled) {
    return enabled ? "1" : "0";
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
    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_SAVE, "&Save\tCtrl+S");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_ASM_FILE_SAVE_AS, "Save &As...\tCtrl+Shift+S");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_UNDO, "Undo\tCtrl+Z");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_CUT, "Cut\tCtrl+X");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_COPY, "Copy\tCtrl+C");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_PASTE, "Paste\tCtrl+V");
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_DELETE, "Delete\tDel");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(edit_menu, MF_STRING, APP_MENU_ASM_EDIT_SELECT_ALL, "Select All\tCtrl+A");
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

/* Creates a readable Windows monospace UI font for the assembler editor and
   falls back to the stock fixed font if the requested face is unavailable. */
static HFONT app_create_monospace_font(void) {
    return CreateFontA(
        -16,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN,
        "Consolas"
    );
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

/* Trims leading and trailing ASCII whitespace from a mutable string and
   returns the first non-space character inside the same buffer. */
static char *app_trim_inplace(char *text) {
    char *start = text;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return start;
}

/* Removes assembler comments introduced by ';' from a source line unless the
   marker appears inside a single- or double-quoted string literal. */
static void app_strip_comment(char *text) {
    bool in_single = false;
    bool in_double = false;

    for (char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*cursor == '"' && !in_single) {
            in_double = !in_double;
        } else if (*cursor == ';' && !in_single && !in_double) {
            *cursor = '\0';
            return;
        }
    }
}

/* Returns true when two ASCII strings match irrespective of case. */
static bool app_equals_ignore_case(const char *lhs, const char *rhs) {
    return _stricmp(lhs, rhs) == 0;
}

/* Copies the next token into an upper-case destination buffer and returns the
   number of source characters that were consumed from the input line. */
static size_t app_read_upper_token(const char *text, char *token, size_t token_size) {
    size_t length = 0;

    while (
        text[length] != '\0' &&
        !isspace((unsigned char)text[length]) &&
        text[length] != ',' &&
        text[length] != '('
    ) {
        if (length + 1 < token_size) {
            token[length] = (char)toupper((unsigned char)text[length]);
        }
        length++;
    }

    if (token_size > 0) {
        size_t copy_length = (length < token_size - 1) ? length : (token_size - 1);
        token[copy_length] = '\0';
    }
    return length;
}

/* Reads a byte from the currently mapped Spectrum address space so the
   debugger can inspect RAM, ROM, and paged banks through one view. */
static uint8_t app_debug_mem_rd(const Spectrum *spec, uint16_t addr) {
    return mem_rd((mem_t *)&spec->machine.mem, addr);
}

/* Reads a 16-bit little-endian value from the current Spectrum memory map. */
static uint16_t app_debug_mem_rd16(const Spectrum *spec, uint16_t addr) {
    return mem_rd16((mem_t *)&spec->machine.mem, addr);
}

/* Formats the Z80 flag register into the conventional `SZ5H3PNC` layout so
   debugger snapshots show active and inactive flag bits compactly. */
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

    if (app->debug.debugger_text == NULL) {
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
        SetWindowTextA(app->debug.debugger_text, buffer);
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
        "State: %s    Model: %s\r\n",
        app->debug.paused ? "Paused" : "Running",
        app_model_name(app->spec.model)
    );
    if (app->debug.debugger_run_to_active) {
        app_append_textf(
            buffer,
            buffer_size,
            &used,
            "Run-to target: %04X\r\n",
            app->debug.debugger_run_to_address
        );
    }
    if (app->debug.breakpoint_hit) {
        app_append_textf(
            buffer,
            buffer_size,
            &used,
            "Breakpoint hit at %04X\r\n",
            app->debug.last_breakpoint_address
        );
    }
    if (app->debug.debugger_run_to_hit) {
        app_append_textf(
            buffer,
            buffer_size,
            &used,
            "Run-to hit at %04X\r\n",
            app->debug.last_run_to_address
        );
    }
    if (app->debug.watchpoint_hit) {
        app_append_textf(
            buffer,
            buffer_size,
            &used,
            "Write watch hit at %04X <= %02X\r\n",
            app->debug.last_watchpoint_address,
            app->debug.last_watchpoint_value
        );
    }
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "PC:%04X  SP:%04X  IR:%02X%02X  IM:%u  IFF1:%u  IFF2:%u\r\n",
        cpu->pc,
        cpu->sp,
        cpu->i,
        cpu->r,
        (unsigned)cpu->im,
        cpu->iff1 ? 1u : 0u,
        cpu->iff2 ? 1u : 0u
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "AF:%04X  BC:%04X  DE:%04X  HL:%04X  IX:%04X  IY:%04X\r\n",
        cpu->af,
        cpu->bc,
        cpu->de,
        cpu->hl,
        cpu->ix,
        cpu->iy
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "AF':%04X BC':%04X DE':%04X HL':%04X  Flags:%s\r\n",
        cpu->af2,
        cpu->bc2,
        cpu->de2,
        cpu->hl2,
        flags
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "FE:%02X  7FFD:%02X  DisplayBank:%u  PagingLocked:%u\r\n\r\n",
        app->spec.machine.last_fe_out,
        app->spec.machine.last_mem_config,
        (unsigned)app->spec.machine.display_ram_bank,
        app->spec.machine.memory_paging_disabled ? 1u : 0u
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "Shortcuts: F5 Run/Pause  F9 Toggle BP  F10 Step Over  F11 Step  Ctrl+W Toggle WP  Ctrl+T Run To\r\n"
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "View: Ctrl+P PC  Ctrl+S SP  PgUp/PgDn page  Address Up/Down +/-10h  Ctrl+Home PC  Ctrl+End SP\r\n"
    );
    app_append_textf(
        buffer,
        buffer_size,
        &used,
        "Address: Enter Go  Ctrl+Enter Run To  Shift+Enter Run @  Markers: > PC  * BP\r\n\r\n"
    );
    app_debug_append_point_summary(buffer, buffer_size, &used, app);
    app_append_textf(buffer, buffer_size, &used, "\r\n\r\n");

    app_append_textf(buffer, buffer_size, &used, "Disassembly @ %04X\r\n", app->debug.debugger_view_address);
    addr = app->debug.debugger_view_address;
    for (int i = 0; i < 14; ++i) {
        char mnemonic[128];
        char bytes[32];
        uint8_t len;
        size_t byte_used = 0;
        const char pc_marker = (addr == cpu->pc) ? '>' : ' ';
        const char bp_marker = app_debug_has_breakpoint(app, addr) ? '*' : ' ';

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
            "%c%c %04X: %-12s %s\r\n",
            pc_marker,
            bp_marker,
            addr,
            bytes,
            mnemonic
        );
        addr = (uint16_t)(addr + len);
    }

    app_append_textf(buffer, buffer_size, &used, "\r\nStack @ %04X\r\n", cpu->sp);
    for (int row = 0; row < 4; ++row) {
        uint16_t row_addr = (uint16_t)(cpu->sp + (uint16_t)(row * 8));
        app_append_textf(buffer, buffer_size, &used, "%04X:", row_addr);
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

    app_append_textf(buffer, buffer_size, &used, "\r\nMemory @ %04X\r\n", app->debug.debugger_view_address);
    for (int row = 0; row < 6; ++row) {
        uint16_t row_addr = (uint16_t)(app->debug.debugger_view_address + (uint16_t)(row * 16));
        app_append_textf(buffer, buffer_size, &used, "%04X:", row_addr);
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

    SetWindowTextA(app->debug.debugger_text, buffer);
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
static void app_debug_run_frame(AppState *app) {
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
    spectrum_render_frame(&app->spec);

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

    app->spec.machine.cpu.pc = address;
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
}

/* Splits a comma-separated operand list while respecting quoted strings so
   assembler directives can accept text literals and normal arguments. */
static bool app_read_operand(char **text, char *operand, size_t operand_size) {
    char *cursor;
    char *start;
    bool in_single = false;
    bool in_double = false;

    if (text == NULL || *text == NULL) {
        return false;
    }

    start = *text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (*start == '\0') {
        *text = start;
        return false;
    }

    cursor = start;
    while (*cursor != '\0') {
        if (*cursor == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*cursor == '"' && !in_single) {
            in_double = !in_double;
        } else if (*cursor == ',' && !in_single && !in_double) {
            break;
        }
        cursor++;
    }

    {
        size_t length = (size_t)(cursor - start);
        size_t copy_length = (length < operand_size - 1) ? length : (operand_size - 1);
        memcpy(operand, start, copy_length);
        operand[copy_length] = '\0';
    }

    {
        char *trimmed = app_trim_inplace(operand);
        if (trimmed != operand) {
            memmove(operand, trimmed, strlen(trimmed) + 1);
        }
    }

    *text = (*cursor == ',') ? (cursor + 1) : cursor;
    return true;
}

/* Strict operand reader for assembly syntax checks. Returns 1 when an operand
   was read, 0 when no more operands are present, and -1 on malformed lists. */
static int app_read_operand_strict(
    char **text,
    char *operand,
    size_t operand_size,
    char *error_buffer,
    size_t error_buffer_size
) {
    char *cursor;
    char *start;
    bool in_single = false;
    bool in_double = false;

    if (text == NULL || *text == NULL) {
        return 0;
    }

    start = *text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (*start == '\0') {
        *text = start;
        return 0;
    }
    if (*start == ',') {
        snprintf(error_buffer, error_buffer_size, "Missing operand before comma.");
        return -1;
    }

    cursor = start;
    while (*cursor != '\0') {
        if (*cursor == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*cursor == '"' && !in_single) {
            in_double = !in_double;
        } else if (*cursor == ',' && !in_single && !in_double) {
            break;
        }
        cursor++;
    }

    {
        size_t length = (size_t)(cursor - start);
        size_t copy_length = (length < operand_size - 1) ? length : (operand_size - 1);
        memcpy(operand, start, copy_length);
        operand[copy_length] = '\0';
    }
    {
        char *trimmed = app_trim_inplace(operand);
        if (trimmed != operand) {
            memmove(operand, trimmed, strlen(trimmed) + 1);
        }
    }
    if (operand[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "Missing operand.");
        return -1;
    }

    if (*cursor == ',') {
        char *next = cursor + 1;
        while (*next != '\0' && isspace((unsigned char)*next)) {
            next++;
        }
        if (*next == '\0') {
            snprintf(error_buffer, error_buffer_size, "Missing operand after comma.");
            return -1;
        }
        if (*next == ',') {
            snprintf(error_buffer, error_buffer_size, "Missing operand between commas.");
            return -1;
        }
        *text = next;
    } else {
        *text = cursor;
    }
    return 1;
}

/* Reports a clear syntax error when an instruction receives the wrong number
   of operands for the specific form supported by the mini assembler. */
static bool app_require_operand_count(
    const char *mnemonic,
    int actual_count,
    int min_count,
    int max_count,
    char *error_buffer,
    size_t error_buffer_size
) {
    if (actual_count < min_count || actual_count > max_count) {
        if (min_count == max_count) {
            snprintf(
                error_buffer,
                error_buffer_size,
                "%s expects %d operand%s.",
                mnemonic,
                min_count,
                min_count == 1 ? "" : "s"
            );
        } else {
            snprintf(
                error_buffer,
                error_buffer_size,
                "%s expects %d or %d operands.",
                mnemonic,
                min_count,
                max_count
            );
        }
        return false;
    }
    return true;
}

/* Parses a simple assembler numeric literal in decimal, hex, binary, or as a
   quoted character literal such as `'A'`. */
static bool app_parse_number(const char *text, int *value) {
    char token[128];
    char *end = NULL;
    long parsed;
    bool negative = false;
    int base = 10;
    size_t length;

    snprintf(token, sizeof(token), "%s", text);
    {
        char *trimmed = app_trim_inplace(token);
        if (trimmed != token) {
            memmove(token, trimmed, strlen(trimmed) + 1);
        }
    }

    length = strlen(token);
    if (length == 0) {
        return false;
    }

    if (token[0] == '-' || token[0] == '+') {
        negative = token[0] == '-';
        memmove(token, token + 1, strlen(token));
        length = strlen(token);
        if (length == 0) {
            return false;
        }
    }

    if ((token[0] == '\'' || token[0] == '"') && length >= 3 && token[length - 1] == token[0]) {
        if (length != 3) {
            return false;
        }
        *value = negative ? -(unsigned char)token[1] : (unsigned char)token[1];
        return true;
    }

    if (token[0] == '$') {
        memmove(token, token + 1, strlen(token));
        base = 16;
    } else if (length > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        memmove(token, token + 2, strlen(token) - 1);
        base = 16;
    } else if (token[0] == '%') {
        memmove(token, token + 1, strlen(token));
        base = 2;
    } else if (length > 1 && (token[length - 1] == 'h' || token[length - 1] == 'H')) {
        token[length - 1] = '\0';
        base = 16;
    } else if (length > 1 && (token[length - 1] == 'b' || token[length - 1] == 'B')) {
        token[length - 1] = '\0';
        base = 2;
    }

    parsed = strtol(token, &end, base);
    if (end == NULL || *end != '\0') {
        return false;
    }
    if (negative) {
        parsed = -parsed;
    }
    *value = (int)parsed;
    return true;
}

/* Returns true when the character is valid at the start of a label name. */
static bool app_is_label_start_char(char ch) {
    return isalpha((unsigned char)ch) || ch == '_' || ch == '.' || ch == '?';
}

/* Returns true when the character is valid after the first label character. */
static bool app_is_label_char(char ch) {
    return isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == '?' || ch == '$';
}

/* Looks up a previously defined label by name and returns its table index. */
static int app_find_label_index(const AssemblerContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->label_count; ++i) {
        if (_stricmp(ctx->labels[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Defines a label at the current assembly address during pass 1 and verifies
   that pass 2 sees the same symbol layout. */
static bool app_define_label(
    AssemblerContext *ctx,
    const char *name,
    uint16_t address,
    char *error_buffer,
    size_t error_buffer_size
) {
    int existing_index = app_find_label_index(ctx, name);

    if (ctx->pass == 1) {
        if (existing_index >= 0) {
            snprintf(error_buffer, error_buffer_size, "Duplicate label: %s", name);
            return false;
        }
        if (ctx->label_count >= sizeof(ctx->labels) / sizeof(ctx->labels[0])) {
            snprintf(error_buffer, error_buffer_size, "Too many labels in assembler source.");
            return false;
        }
        snprintf(ctx->labels[ctx->label_count].name, sizeof(ctx->labels[ctx->label_count].name), "%s", name);
        ctx->labels[ctx->label_count].address = address;
        ctx->label_count++;
        return true;
    }

    if (existing_index < 0) {
        snprintf(error_buffer, error_buffer_size, "Unknown label during second pass: %s", name);
        return false;
    }
    if (ctx->labels[existing_index].address != address) {
        snprintf(error_buffer, error_buffer_size, "Label address changed between passes: %s", name);
        return false;
    }
    return true;
}

/* Consumes one leading `label:` definition from the current line if present. */
static bool app_parse_leading_label(char **text, char *label, size_t label_size) {
    char *cursor;
    char *name_end;
    size_t length;

    if (text == NULL || *text == NULL) {
        return false;
    }

    cursor = *text;
    if (!app_is_label_start_char(*cursor)) {
        return false;
    }

    name_end = cursor + 1;
    while (app_is_label_char(*name_end)) {
        name_end++;
    }
    length = (size_t)(name_end - cursor);

    while (*name_end != '\0' && isspace((unsigned char)*name_end)) {
        name_end++;
    }
    if (*name_end != ':') {
        return false;
    }

    if (label_size > 0) {
        size_t copy_length = (length < label_size - 1) ? length : (label_size - 1);
        memcpy(label, cursor, copy_length);
        label[copy_length] = '\0';
    }

    *text = name_end + 1;
    return true;
}

/* Scans assembler source for the first explicit ORG directive so assembly can
   start from source-defined addresses without separate UI state. */
static bool app_assembler_find_source_org(const char *source, uint16_t *out_address) {
    char *mutable_source;
    char *cursor;
    char *line;

    mutable_source = _strdup(source);
    if (mutable_source == NULL) {
        return false;
    }

    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        char mnemonic[32];
        char operand[128];
        char label[64];
        char *text;
        size_t mnemonic_chars;
        int org_value;

        line = cursor;
        if (line_end != NULL) {
            *line_end = '\0';
            cursor = line_end + 1;
        } else {
            cursor = NULL;
        }

        {
            size_t line_length = strlen(line);
            if (line_length > 0 && line[line_length - 1] == '\r') {
                line[line_length - 1] = '\0';
            }
        }

        app_strip_comment(line);
        text = app_trim_inplace(line);
        if (*text == '\0') {
            continue;
        }
        while (app_parse_leading_label(&text, label, sizeof(label))) {
            text = app_trim_inplace(text);
            if (*text == '\0') {
                break;
            }
        }
        if (*text == '\0') {
            continue;
        }

        mnemonic_chars = app_read_upper_token(text, mnemonic, sizeof(mnemonic));
        text += mnemonic_chars;
        text = app_trim_inplace(text);
        if (!app_equals_ignore_case(mnemonic, "ORG")) {
            continue;
        }
        operand[0] = '\0';
        if (*text != '\0') {
            char *operand_cursor = text;
            app_read_operand(&operand_cursor, operand, sizeof(operand));
        }
        if (app_parse_number(operand, &org_value) && org_value >= 0 && org_value <= 0xFFFF) {
            *out_address = (uint16_t)org_value;
            free(mutable_source);
            return true;
        }
    }

    free(mutable_source);
    return false;
}

/* Resolves an assembler value token as either a numeric literal or a label.
   During pass 1 forward label references are accepted as unresolved zeros. */
static bool app_parse_value(
    const AssemblerContext *ctx,
    const char *text,
    AssemblerValue *out_value
) {
    char token[128];
    int numeric_value;
    int label_index;

    snprintf(token, sizeof(token), "%s", text);
    {
        char *trimmed = app_trim_inplace(token);
        if (trimmed != token) {
            memmove(token, trimmed, strlen(trimmed) + 1);
        }
    }
    if (token[0] == '\0') {
        return false;
    }

    if (app_parse_number(token, &numeric_value)) {
        out_value->value = numeric_value;
        out_value->resolved = true;
        out_value->numeric_literal = true;
        return true;
    }

    if (!app_is_label_start_char(token[0])) {
        return false;
    }
    for (size_t i = 1; token[i] != '\0'; ++i) {
        if (!app_is_label_char(token[i])) {
            return false;
        }
    }

    label_index = app_find_label_index(ctx, token);
    if (label_index >= 0) {
        out_value->value = ctx->labels[label_index].address;
        out_value->resolved = true;
        out_value->numeric_literal = false;
        return true;
    }

    if (ctx->pass == 1) {
        out_value->value = 0;
        out_value->resolved = false;
        out_value->numeric_literal = false;
        return true;
    }
    return false;
}

/* Returns the 8-bit register code used by most primary Z80 opcodes, including
   `(HL)` for memory-indirect forms. */
static int app_parse_reg8(const char *text) {
    static const char *names[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    if (app_equals_ignore_case(text, "(HL)")) {
        return 6;
    }
    for (int i = 0; i < 8; ++i) {
        if (app_equals_ignore_case(text, names[i])) {
            return i;
        }
    }
    return -1;
}

/* Returns the standard BC/DE/HL/SP register-pair code used by many 16-bit
   Z80 instructions. */
static int app_parse_reg16(const char *text) {
    static const char *names[] = {"BC", "DE", "HL", "SP"};
    for (int i = 0; i < 4; ++i) {
        if (app_equals_ignore_case(text, names[i])) {
            return i;
        }
    }
    return -1;
}

/* Returns the BC/DE/HL/AF register-pair code used by PUSH and POP. */
static int app_parse_reg16_push(const char *text) {
    static const char *names[] = {"BC", "DE", "HL", "AF"};
    for (int i = 0; i < 4; ++i) {
        if (app_equals_ignore_case(text, names[i])) {
            return i;
        }
    }
    return -1;
}

/* Maps a condition mnemonic such as `NZ` or `C` to its Z80 condition code. */
static int app_parse_condition(const char *text) {
    static const char *names[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
    for (int i = 0; i < 8; ++i) {
        if (app_equals_ignore_case(text, names[i])) {
            return i;
        }
    }
    return -1;
}

/* Parses a `(nn)` absolute memory operand and returns the enclosed address. */
static bool app_parse_indirect_address(
    const AssemblerContext *ctx,
    const char *text,
    AssemblerValue *value
) {
    char token[128];
    char *inner;
    size_t length;

    snprintf(token, sizeof(token), "%s", text);
    {
        char *trimmed = app_trim_inplace(token);
        if (trimmed != token) {
            memmove(token, trimmed, strlen(trimmed) + 1);
        }
    }

    length = strlen(token);
    if (length < 3 || token[0] != '(' || token[length - 1] != ')') {
        return false;
    }
    token[length - 1] = '\0';
    inner = app_trim_inplace(token + 1);

    /* `(nn)` is the absolute-memory form. Register-indirect operands such as
       `(HL)` and `(BC)` must fall through to their dedicated instruction
       encodings instead of being treated as forward label references. */
    if (
        app_equals_ignore_case(inner, "HL") ||
        app_equals_ignore_case(inner, "BC") ||
        app_equals_ignore_case(inner, "DE") ||
        app_equals_ignore_case(inner, "SP") ||
        app_equals_ignore_case(inner, "IX") ||
        app_equals_ignore_case(inner, "IY")
    ) {
        return false;
    }

    return app_parse_value(ctx, inner, value);
}

/* Emits bytes into the active Spectrum address space while rejecting writes to
   ROM or beyond the 16-bit address range. */
static bool app_assemble_write_bytes(
    AssemblerContext *ctx,
    const uint8_t *bytes,
    size_t length,
    char *error_buffer,
    size_t error_buffer_size
) {
    if (length == 0) {
        return true;
    }
    if (ctx->address < 0x4000) {
        snprintf(error_buffer, error_buffer_size, "Assembler writes are limited to RAM at 4000h-FFFFh.");
        return false;
    }
    if ((size_t)ctx->address + length > 0x10000u) {
        snprintf(error_buffer, error_buffer_size, "Assembled bytes run past FFFFh.");
        return false;
    }
    if (ctx->pass == 2) {
        mem_write_range(&ctx->app->spec.machine.mem, ctx->address, bytes, (uint32_t)length);
    }
    ctx->address = (uint16_t)(ctx->address + (uint16_t)length);
    return true;
}

/* Parses a `DB` item, allowing both numeric literals and quoted ASCII
   strings, and appends the resulting bytes to the output buffer. */
static bool app_assemble_db_item(
    const AssemblerContext *ctx,
    const char *operand,
    uint8_t *bytes,
    size_t *length,
    size_t max_length,
    char *error_buffer,
    size_t error_buffer_size
) {
    size_t operand_length = strlen(operand);
    AssemblerValue value;

    if (operand_length >= 2 && (operand[0] == '\'' || operand[0] == '"') && operand[operand_length - 1] == operand[0]) {
        for (size_t i = 1; i + 1 < operand_length; ++i) {
            if (*length >= max_length) {
                snprintf(error_buffer, error_buffer_size, "Line expands beyond the assembler output buffer.");
                return false;
            }
            bytes[(*length)++] = (uint8_t)operand[i];
        }
        return true;
    }

    if (!app_parse_value(ctx, operand, &value)) {
        snprintf(error_buffer, error_buffer_size, "Invalid DB value: %s", operand);
        return false;
    }
    if (ctx->pass == 2 && (value.value < -128 || value.value > 255)) {
        snprintf(error_buffer, error_buffer_size, "Invalid DB value: %s", operand);
        return false;
    }
    if (*length >= max_length) {
        snprintf(error_buffer, error_buffer_size, "Line expands beyond the assembler output buffer.");
        return false;
    }
    bytes[(*length)++] = (uint8_t)value.value;
    return true;
}

/* Encodes one assembler line from the mini-assembler supported by the UI. */
static bool app_assemble_line(
    AssemblerContext *ctx,
    char *line,
    char *error_buffer,
    size_t error_buffer_size
) {
    AssemblerValue value;
    char mnemonic[32];
    char label[64];
    char lhs[128];
    char rhs[128];
    char extra[128];
    char *operands;
    uint8_t bytes[256];
    size_t length = 0;
    size_t mnemonic_chars;
    int operand_count = 0;
    int operand_result;

    app_strip_comment(line);
    operands = app_trim_inplace(line);
    if (*operands == '\0') {
        return true;
    }

    while (app_parse_leading_label(&operands, label, sizeof(label))) {
        if (!app_define_label(ctx, label, ctx->address, error_buffer, error_buffer_size)) {
            return false;
        }
        operands = app_trim_inplace(operands);
        if (*operands == '\0') {
            return true;
        }
    }

    mnemonic_chars = app_read_upper_token(operands, mnemonic, sizeof(mnemonic));
    operands += mnemonic_chars;
    operands = app_trim_inplace(operands);
    lhs[0] = '\0';
    rhs[0] = '\0';
    extra[0] = '\0';
    if (*operands != '\0' && !app_equals_ignore_case(mnemonic, "DB") && !app_equals_ignore_case(mnemonic, "DW")) {
        char *operand_cursor = operands;
        operand_result = app_read_operand_strict(&operand_cursor, lhs, sizeof(lhs), error_buffer, error_buffer_size);
        if (operand_result < 0) {
            return false;
        }
        if (operand_result > 0) {
            operand_count = 1;
            operand_result = app_read_operand_strict(&operand_cursor, rhs, sizeof(rhs), error_buffer, error_buffer_size);
            if (operand_result < 0) {
                return false;
            }
            if (operand_result > 0) {
                operand_count = 2;
                operand_result = app_read_operand_strict(&operand_cursor, extra, sizeof(extra), error_buffer, error_buffer_size);
                if (operand_result < 0) {
                    return false;
                }
                if (operand_result > 0) {
                    snprintf(error_buffer, error_buffer_size, "Too many operands for %s.", mnemonic);
                    return false;
                }
            }
        }
    }

    if (app_equals_ignore_case(mnemonic, "ORG")) {
        int new_address;
        if (!app_require_operand_count("ORG", operand_count, 1, 1, error_buffer, error_buffer_size)) {
            return false;
        }
        if (!app_parse_number(lhs, &new_address) || new_address < 0 || new_address > 0xFFFF) {
            snprintf(error_buffer, error_buffer_size, "Invalid ORG address: %s", lhs);
            return false;
        }
        ctx->address = (uint16_t)new_address;
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "DB")) {
        char *cursor = operands;
        char operand[256];
        operand_count = 0;
        while ((operand_result = app_read_operand_strict(&cursor, operand, sizeof(operand), error_buffer, error_buffer_size)) > 0) {
            operand_count++;
            if (!app_assemble_db_item(ctx, operand, bytes, &length, sizeof(bytes), error_buffer, error_buffer_size)) {
                return false;
            }
        }
        if (operand_result < 0) {
            return false;
        }
        if (operand_count == 0) {
            snprintf(error_buffer, error_buffer_size, "DB expects at least 1 operand.");
            return false;
        }
        if (!app_assemble_write_bytes(ctx, bytes, length, error_buffer, error_buffer_size)) {
            return false;
        }
        ctx->total_written += length;
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "DW")) {
        char *cursor = operands;
        char operand[256];
        operand_count = 0;
        while ((operand_result = app_read_operand_strict(&cursor, operand, sizeof(operand), error_buffer, error_buffer_size)) > 0) {
            operand_count++;
            if (!app_parse_value(ctx, operand, &value)) {
                snprintf(error_buffer, error_buffer_size, "Invalid DW value: %s", operand);
                return false;
            }
            if (ctx->pass == 2 && (value.value < -32768 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Invalid DW value: %s", operand);
                return false;
            }
            if (length + 2 > sizeof(bytes)) {
                snprintf(error_buffer, error_buffer_size, "Line expands beyond the assembler output buffer.");
                return false;
            }
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        if (operand_result < 0) {
            return false;
        }
        if (operand_count == 0) {
            snprintf(error_buffer, error_buffer_size, "DW expects at least 1 operand.");
            return false;
        }
        if (!app_assemble_write_bytes(ctx, bytes, length, error_buffer, error_buffer_size)) {
            return false;
        }
        ctx->total_written += length;
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "NOP")) {
        if (!app_require_operand_count("NOP", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x00;
    }
    else if (app_equals_ignore_case(mnemonic, "HALT")) {
        if (!app_require_operand_count("HALT", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x76;
    }
    else if (app_equals_ignore_case(mnemonic, "DI")) {
        if (!app_require_operand_count("DI", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0xF3;
    }
    else if (app_equals_ignore_case(mnemonic, "EI")) {
        if (!app_require_operand_count("EI", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0xFB;
    }
    else if (app_equals_ignore_case(mnemonic, "SCF")) {
        if (!app_require_operand_count("SCF", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x37;
    }
    else if (app_equals_ignore_case(mnemonic, "CCF")) {
        if (!app_require_operand_count("CCF", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x3F;
    }
    else if (app_equals_ignore_case(mnemonic, "CPL")) {
        if (!app_require_operand_count("CPL", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x2F;
    }
    else if (app_equals_ignore_case(mnemonic, "DAA")) {
        if (!app_require_operand_count("DAA", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x27;
    }
    else if (app_equals_ignore_case(mnemonic, "RLCA")) {
        if (!app_require_operand_count("RLCA", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x07;
    }
    else if (app_equals_ignore_case(mnemonic, "RRCA")) {
        if (!app_require_operand_count("RRCA", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x0F;
    }
    else if (app_equals_ignore_case(mnemonic, "RLA")) {
        if (!app_require_operand_count("RLA", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x17;
    }
    else if (app_equals_ignore_case(mnemonic, "RRA")) {
        if (!app_require_operand_count("RRA", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0x1F;
    }
    else if (app_equals_ignore_case(mnemonic, "EXX")) {
        if (!app_require_operand_count("EXX", operand_count, 0, 0, error_buffer, error_buffer_size)) {
            return false;
        }
        bytes[length++] = 0xD9;
    }
    else if (app_equals_ignore_case(mnemonic, "RET")) {
        if (!app_require_operand_count("RET", operand_count, 0, 1, error_buffer, error_buffer_size)) {
            return false;
        }
        if (lhs[0] == '\0') {
            bytes[length++] = 0xC9;
        } else {
            int cc = app_parse_condition(lhs);
            if (cc < 0) {
                snprintf(error_buffer, error_buffer_size, "Unsupported RET condition: %s", lhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0xC0 + (cc << 3));
        }
    } else if (app_equals_ignore_case(mnemonic, "RST")) {
        int rst_value;
        if (!app_require_operand_count("RST", operand_count, 1, 1, error_buffer, error_buffer_size)) {
            return false;
        }
        if (!app_parse_number(lhs, &rst_value) || rst_value < 0 || rst_value > 0x38 || (rst_value & 0x07) != 0) {
            snprintf(error_buffer, error_buffer_size, "RST expects 00h,08h,...,38h.");
            return false;
        }
        bytes[length++] = (uint8_t)(0xC7 + rst_value);
    } else if (app_equals_ignore_case(mnemonic, "DJNZ")) {
        int disp;
        if (!app_require_operand_count("DJNZ", operand_count, 1, 1, error_buffer, error_buffer_size)) {
            return false;
        }
        if (!app_parse_value(ctx, lhs, &value)) {
            snprintf(error_buffer, error_buffer_size, "Invalid DJNZ target: %s", lhs);
            return false;
        }
        disp = (value.numeric_literal && value.value >= -128 && value.value <= 127)
            ? value.value
            : (value.value - ((int)ctx->address + 2));
        if (ctx->pass == 2 && (disp < -128 || disp > 127)) {
            snprintf(error_buffer, error_buffer_size, "DJNZ target is out of range.");
            return false;
        }
        bytes[length++] = 0x10;
        bytes[length++] = (uint8_t)(int8_t)disp;
    } else if (app_equals_ignore_case(mnemonic, "JR")) {
        int cc = -1;
        int disp;
        if (!app_require_operand_count("JR", operand_count, 1, 2, error_buffer, error_buffer_size)) {
            return false;
        }
        if (rhs[0] != '\0') {
            cc = app_parse_condition(lhs);
            if (cc < 0 || cc > 3) {
                snprintf(error_buffer, error_buffer_size, "JR only supports NZ, Z, NC, or C.");
                return false;
            }
            if (!app_parse_value(ctx, rhs, &value)) {
                snprintf(error_buffer, error_buffer_size, "Invalid JR target: %s", rhs);
                return false;
            }
        } else {
            if (!app_parse_value(ctx, lhs, &value)) {
                snprintf(error_buffer, error_buffer_size, "Invalid JR target: %s", lhs);
                return false;
            }
        }
        disp = (value.numeric_literal && value.value >= -128 && value.value <= 127)
            ? value.value
            : (value.value - ((int)ctx->address + 2));
        if (ctx->pass == 2 && (disp < -128 || disp > 127)) {
            snprintf(error_buffer, error_buffer_size, "JR target is out of range.");
            return false;
        }
        bytes[length++] = (uint8_t)((cc >= 0) ? (0x20 + (cc << 3)) : 0x18);
        bytes[length++] = (uint8_t)(int8_t)disp;
    } else if (app_equals_ignore_case(mnemonic, "JP")) {
        if (!app_require_operand_count("JP", operand_count, 1, 2, error_buffer, error_buffer_size)) {
            return false;
        }
        if (rhs[0] == '\0') {
            if (app_equals_ignore_case(lhs, "(HL)")) {
                bytes[length++] = 0xE9;
            } else if (app_equals_ignore_case(lhs, "(IX)")) {
                bytes[length++] = 0xDD;
                bytes[length++] = 0xE9;
            } else if (app_equals_ignore_case(lhs, "(IY)")) {
                bytes[length++] = 0xFD;
                bytes[length++] = 0xE9;
            } else {
                if (!app_parse_value(ctx, lhs, &value)) {
                    snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", lhs);
                    return false;
                }
                if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                    snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", lhs);
                    return false;
                }
                bytes[length++] = 0xC3;
                bytes[length++] = (uint8_t)value.value;
                bytes[length++] = (uint8_t)(value.value >> 8);
            }
        } else {
            int cc = app_parse_condition(lhs);
            if (cc < 0) {
                snprintf(error_buffer, error_buffer_size, "Unsupported JP condition: %s", lhs);
                return false;
            }
            if (!app_parse_value(ctx, rhs, &value)) {
                snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", rhs);
                return false;
            }
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0xC2 + (cc << 3));
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
    } else if (app_equals_ignore_case(mnemonic, "CALL")) {
        if (!app_require_operand_count("CALL", operand_count, 1, 2, error_buffer, error_buffer_size)) {
            return false;
        }
        if (rhs[0] == '\0') {
            if (!app_parse_value(ctx, lhs, &value)) {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", lhs);
                return false;
            }
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", lhs);
                return false;
            }
            bytes[length++] = 0xCD;
        } else {
            int cc = app_parse_condition(lhs);
            if (cc < 0) {
                snprintf(error_buffer, error_buffer_size, "Unsupported CALL condition: %s", lhs);
                return false;
            }
            if (!app_parse_value(ctx, rhs, &value)) {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", rhs);
                return false;
            }
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0xC4 + (cc << 3));
        }
        bytes[length++] = (uint8_t)value.value;
        bytes[length++] = (uint8_t)(value.value >> 8);
    } else if (app_equals_ignore_case(mnemonic, "OUT")) {
        if (!app_require_operand_count("OUT", operand_count, 2, 2, error_buffer, error_buffer_size)) {
            return false;
        }
        if (!app_equals_ignore_case(rhs, "A") || !app_parse_indirect_address(ctx, lhs, &value)) {
            snprintf(error_buffer, error_buffer_size, "Only OUT (n),A is supported.");
            return false;
        }
        if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFF)) {
            snprintf(error_buffer, error_buffer_size, "Only OUT (n),A is supported.");
            return false;
        }
        bytes[length++] = 0xD3;
        bytes[length++] = (uint8_t)value.value;
    } else if (app_equals_ignore_case(mnemonic, "IN")) {
        if (!app_require_operand_count("IN", operand_count, 2, 2, error_buffer, error_buffer_size)) {
            return false;
        }
        if (!app_equals_ignore_case(lhs, "A") || !app_parse_indirect_address(ctx, rhs, &value)) {
            snprintf(error_buffer, error_buffer_size, "Only IN A,(n) is supported.");
            return false;
        }
        if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFF)) {
            snprintf(error_buffer, error_buffer_size, "Only IN A,(n) is supported.");
            return false;
        }
        bytes[length++] = 0xDB;
        bytes[length++] = (uint8_t)value.value;
    } else if (app_equals_ignore_case(mnemonic, "PUSH") || app_equals_ignore_case(mnemonic, "POP")) {
        int rr = app_parse_reg16_push(lhs);
        uint8_t base = app_equals_ignore_case(mnemonic, "PUSH") ? 0xC5 : 0xC1;
        if (!app_require_operand_count(mnemonic, operand_count, 1, 1, error_buffer, error_buffer_size)) {
            return false;
        }
        if (rr >= 0) {
            bytes[length++] = (uint8_t)(base + (rr << 4));
        } else if (app_equals_ignore_case(lhs, "IX")) {
            bytes[length++] = 0xDD;
            bytes[length++] = (uint8_t)(app_equals_ignore_case(mnemonic, "PUSH") ? 0xE5 : 0xE1);
        } else if (app_equals_ignore_case(lhs, "IY")) {
            bytes[length++] = 0xFD;
            bytes[length++] = (uint8_t)(app_equals_ignore_case(mnemonic, "PUSH") ? 0xE5 : 0xE1);
        } else {
            snprintf(error_buffer, error_buffer_size, "Unsupported register for %s: %s", mnemonic, lhs);
            return false;
        }
    } else if (app_equals_ignore_case(mnemonic, "INC") || app_equals_ignore_case(mnemonic, "DEC")) {
        uint8_t op8 = app_equals_ignore_case(mnemonic, "INC") ? 0x04 : 0x05;
        uint8_t op16 = app_equals_ignore_case(mnemonic, "INC") ? 0x03 : 0x0B;
        int r = app_parse_reg8(lhs);
        int rr = app_parse_reg16(lhs);
        if (!app_require_operand_count(mnemonic, operand_count, 1, 1, error_buffer, error_buffer_size)) {
            return false;
        }
        if (r >= 0) {
            bytes[length++] = (uint8_t)(op8 + (r << 3));
        } else if (rr >= 0) {
            bytes[length++] = (uint8_t)(op16 + (rr << 4));
        } else if (app_equals_ignore_case(lhs, "IX")) {
            bytes[length++] = 0xDD;
            bytes[length++] = app_equals_ignore_case(mnemonic, "INC") ? 0x23 : 0x2B;
        } else if (app_equals_ignore_case(lhs, "IY")) {
            bytes[length++] = 0xFD;
            bytes[length++] = app_equals_ignore_case(mnemonic, "INC") ? 0x23 : 0x2B;
        } else {
            snprintf(error_buffer, error_buffer_size, "Unsupported %s operand: %s", mnemonic, lhs);
            return false;
        }
    } else if (app_equals_ignore_case(mnemonic, "EX")) {
        if (!app_require_operand_count("EX", operand_count, 2, 2, error_buffer, error_buffer_size)) {
            return false;
        }
        if (app_equals_ignore_case(lhs, "DE") && app_equals_ignore_case(rhs, "HL")) {
            bytes[length++] = 0xEB;
        } else if (app_equals_ignore_case(lhs, "AF") && app_equals_ignore_case(rhs, "AF'")) {
            bytes[length++] = 0x08;
        } else if (app_equals_ignore_case(lhs, "(SP)") && app_equals_ignore_case(rhs, "HL")) {
            bytes[length++] = 0xE3;
        } else if (app_equals_ignore_case(lhs, "(SP)") && app_equals_ignore_case(rhs, "IX")) {
            bytes[length++] = 0xDD;
            bytes[length++] = 0xE3;
        } else if (app_equals_ignore_case(lhs, "(SP)") && app_equals_ignore_case(rhs, "IY")) {
            bytes[length++] = 0xFD;
            bytes[length++] = 0xE3;
        } else {
            snprintf(error_buffer, error_buffer_size, "Unsupported EX form.");
            return false;
        }
    } else if (app_equals_ignore_case(mnemonic, "ADD")) {
        int rr = app_parse_reg16(rhs);
        int r = app_parse_reg8(rhs);
        if (!app_require_operand_count("ADD", operand_count, 2, 2, error_buffer, error_buffer_size)) {
            return false;
        }
        if (app_equals_ignore_case(lhs, "HL") && rr >= 0) {
            bytes[length++] = (uint8_t)(0x09 + (rr << 4));
        } else if (app_equals_ignore_case(lhs, "IX") && rr >= 0) {
            bytes[length++] = 0xDD;
            bytes[length++] = (uint8_t)(0x09 + (rr << 4));
        } else if (app_equals_ignore_case(lhs, "IY") && rr >= 0) {
            bytes[length++] = 0xFD;
            bytes[length++] = (uint8_t)(0x09 + (rr << 4));
        } else if (app_equals_ignore_case(lhs, "A") && r >= 0) {
            bytes[length++] = (uint8_t)(0x80 + r);
        } else if (app_equals_ignore_case(lhs, "A") && app_parse_value(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < -128 || value.value > 255)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported ADD form.");
                return false;
            }
            bytes[length++] = 0xC6;
            bytes[length++] = (uint8_t)value.value;
        } else {
            snprintf(error_buffer, error_buffer_size, "Unsupported ADD form.");
            return false;
        }
    } else if (
        app_equals_ignore_case(mnemonic, "SUB") ||
        app_equals_ignore_case(mnemonic, "AND") ||
        app_equals_ignore_case(mnemonic, "OR") ||
        app_equals_ignore_case(mnemonic, "XOR") ||
        app_equals_ignore_case(mnemonic, "CP")
    ) {
        uint8_t reg_base;
        uint8_t imm_base;
        int r = app_parse_reg8(lhs);

        if (app_equals_ignore_case(mnemonic, "SUB")) { reg_base = 0x90; imm_base = 0xD6; }
        else if (app_equals_ignore_case(mnemonic, "AND")) { reg_base = 0xA0; imm_base = 0xE6; }
        else if (app_equals_ignore_case(mnemonic, "XOR")) { reg_base = 0xA8; imm_base = 0xEE; }
        else if (app_equals_ignore_case(mnemonic, "OR")) { reg_base = 0xB0; imm_base = 0xF6; }
        else { reg_base = 0xB8; imm_base = 0xFE; }

        if (!app_require_operand_count(mnemonic, operand_count, 1, 1, error_buffer, error_buffer_size)) {
            return false;
        }
        if (r >= 0) {
            bytes[length++] = (uint8_t)(reg_base + r);
        } else if (app_parse_value(ctx, lhs, &value)) {
            if (ctx->pass == 2 && (value.value < -128 || value.value > 255)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported %s operand: %s", mnemonic, lhs);
                return false;
            }
            bytes[length++] = imm_base;
            bytes[length++] = (uint8_t)value.value;
        } else {
            snprintf(error_buffer, error_buffer_size, "Unsupported %s operand: %s", mnemonic, lhs);
            return false;
        }
    } else if (app_equals_ignore_case(mnemonic, "LD")) {
        int rr = app_parse_reg16(lhs);
        int dst8 = app_parse_reg8(lhs);
        int src8 = app_parse_reg8(rhs);
        if (!app_require_operand_count("LD", operand_count, 2, 2, error_buffer, error_buffer_size)) {
            return false;
        }

        if (rr >= 0 && app_parse_value(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0x01 + (rr << 4));
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "IX") && app_parse_value(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xDD;
            bytes[length++] = 0x21;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "IY") && app_parse_value(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xFD;
            bytes[length++] = 0x21;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "SP") && app_equals_ignore_case(rhs, "HL")) {
            bytes[length++] = 0xF9;
        } else if (app_equals_ignore_case(lhs, "SP") && app_equals_ignore_case(rhs, "IX")) {
            bytes[length++] = 0xDD;
            bytes[length++] = 0xF9;
        } else if (app_equals_ignore_case(lhs, "SP") && app_equals_ignore_case(rhs, "IY")) {
            bytes[length++] = 0xFD;
            bytes[length++] = 0xF9;
        } else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "A")) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x32;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "A") && app_parse_indirect_address(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x3A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "HL")) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x22;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "HL") && app_parse_indirect_address(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x2A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "IX")) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xDD;
            bytes[length++] = 0x22;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "IX") && app_parse_indirect_address(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xDD;
            bytes[length++] = 0x2A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "IY")) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xFD;
            bytes[length++] = 0x22;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "IY") && app_parse_indirect_address(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xFD;
            bytes[length++] = 0x2A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        } else if (app_equals_ignore_case(lhs, "(BC)") && app_equals_ignore_case(rhs, "A")) {
            bytes[length++] = 0x02;
        } else if (app_equals_ignore_case(lhs, "(DE)") && app_equals_ignore_case(rhs, "A")) {
            bytes[length++] = 0x12;
        } else if (app_equals_ignore_case(lhs, "A") && app_equals_ignore_case(rhs, "(BC)")) {
            bytes[length++] = 0x0A;
        } else if (app_equals_ignore_case(lhs, "A") && app_equals_ignore_case(rhs, "(DE)")) {
            bytes[length++] = 0x1A;
        } else if (dst8 >= 0 && src8 >= 0) {
            bytes[length++] = (uint8_t)(0x40 + (dst8 << 3) + src8);
        } else if (dst8 >= 0 && app_parse_value(ctx, rhs, &value)) {
            if (ctx->pass == 2 && (value.value < -128 || value.value > 255)) {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0x06 + (dst8 << 3));
            bytes[length++] = (uint8_t)value.value;
        } else {
            snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
            return false;
        }
    } else {
        snprintf(error_buffer, error_buffer_size, "Unsupported mnemonic: %s", mnemonic);
        return false;
    }

    if (!app_assemble_write_bytes(ctx, bytes, length, error_buffer, error_buffer_size)) {
        return false;
    }
    ctx->total_written += length;
    return true;
}

/* Assembles a block of source text line by line and reports the first error
   with its line number so the small UI remains practical to use. */
static bool app_assemble_source(
    AppState *app,
    uint16_t start_address,
    const AssemblerPreparedSource *source,
    char *status_buffer,
    size_t status_buffer_size
) {
    AssemblerContext ctx;
    char *mutable_source;
    char *line;
    char *cursor;
    size_t line_number;
    bool success;

    memset(&ctx, 0, sizeof(ctx));
    ctx.app = app;
    ctx.start_address = start_address;
    ctx.address = start_address;
    ctx.pass = 1;

    mutable_source = _strdup(source->text);
    if (mutable_source == NULL) {
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }

    success = true;
    line_number = 0;
    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        line = cursor;
        if (line_end != NULL) {
            *line_end = '\0';
            cursor = line_end + 1;
        } else {
            cursor = NULL;
        }
        size_t line_length = strlen(line);
        if (line_length > 0 && line[line_length - 1] == '\r') {
            line[line_length - 1] = '\0';
        }
        line_number++;
        if (!app_assemble_line(&ctx, line, status_buffer, status_buffer_size)) {
            char final_error[512];
            app_assembler_format_location_error(
                line_number <= source->line_count ? &source->locations[line_number - 1] : NULL,
                line_number,
                status_buffer,
                final_error,
                sizeof(final_error)
            );
            snprintf(status_buffer, status_buffer_size, "%s", final_error);
            success = false;
            break;
        }
    }
    free(mutable_source);

    if (!success) {
        return false;
    }

    ctx.address = start_address;
    ctx.total_written = 0;
    ctx.pass = 2;

    mutable_source = _strdup(source->text);
    if (mutable_source == NULL) {
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }

    line_number = 0;
    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        line = cursor;
        if (line_end != NULL) {
            *line_end = '\0';
            cursor = line_end + 1;
        } else {
            cursor = NULL;
        }
        {
            size_t line_length = strlen(line);
            if (line_length > 0 && line[line_length - 1] == '\r') {
                line[line_length - 1] = '\0';
            }
        }
        line_number++;
        if (!app_assemble_line(&ctx, line, status_buffer, status_buffer_size)) {
            char final_error[512];
            app_assembler_format_location_error(
                line_number <= source->line_count ? &source->locations[line_number - 1] : NULL,
                line_number,
                status_buffer,
                final_error,
                sizeof(final_error)
            );
            snprintf(status_buffer, status_buffer_size, "%s", final_error);
            free(mutable_source);
            return false;
        }
    }
    free(mutable_source);

    snprintf(
        status_buffer,
        status_buffer_size,
        "Assembled %zu byte%s with %zu label%s to RAM starting at %04Xh. Next address: %04Xh.",
        ctx.total_written,
        ctx.total_written == 1 ? "" : "s",
        ctx.label_count,
        ctx.label_count == 1 ? "" : "s",
        start_address,
        ctx.address
    );
    return true;
}

/* Seeds the assembler with the current PC and a brief syntax reminder. */
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
            background_brush = CreateSolidBrush(RGB(248, 248, 248));
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
static void app_debugger_layout_controls(AppState *app, HWND hwnd) {
    RECT rect;
    int width;
    int height;
    const int side_panel_width = 180;
    const int top_row_y = 8;
    const int second_row_y = 38;
    const int content_y = 72;

    if (app->debug.debugger_text == NULL) {
        return;
    }
    GetClientRect(hwnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

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
    MoveWindow(
        app->debug.debugger_text,
        8,
        content_y,
        width - side_panel_width - 24,
        height - content_y - 8,
        TRUE
    );
    MoveWindow(
        app->debug.debugger_points_list,
        width - side_panel_width - 8,
        content_y,
        side_panel_width,
        height - content_y - 8,
        TRUE
    );
}

/* Sizes the assembler controls to the current client area of the tool window. */
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

/* Hosts the modeless debugger window with pause/run/step controls and a live
   register plus memory snapshot view. */
static LRESULT CALLBACK app_debugger_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *create = (CREATESTRUCTA *)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
            return TRUE;
        }
        case WM_CREATE: {
            static const ACCEL debugger_accels[] = {
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
            app->debug.debugger_pause_button = CreateWindowExA(0, "BUTTON", "Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)APP_CTRL_DEBUG_PAUSE, NULL, NULL);
            app->debug.debugger_step_button = CreateWindowExA(0, "BUTTON", "Step", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)APP_CTRL_DEBUG_STEP, NULL, NULL);
            app->debug.debugger_step_over_button = CreateWindowExA(0, "BUTTON", "Step Over", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)APP_CTRL_DEBUG_STEP_OVER, NULL, NULL);
            app->debug.debugger_refresh_button = CreateWindowExA(0, "BUTTON", "Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)APP_CTRL_DEBUG_REFRESH, NULL, NULL);
            app->debug.debugger_address_edit = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                "EDIT",
                "",
                WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | ES_UPPERCASE,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_ADDRESS,
                NULL,
                NULL
            );
            app->debug.debugger_go_button = CreateWindowExA(0, "BUTTON", "Go", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)APP_CTRL_DEBUG_GO, NULL, NULL);
            app->debug.debugger_sync_checkbox = CreateWindowExA(0, "BUTTON", "Sync PC", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)APP_CTRL_DEBUG_SYNC, NULL, NULL);
            app->debug.debugger_breakpoint_toggle_button = CreateWindowExA(
                0,
                "BUTTON",
                "Toggle BP",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_BREAKPOINT_TOGGLE,
                NULL,
                NULL
            );
            app->debug.debugger_watchpoint_toggle_button = CreateWindowExA(
                0,
                "BUTTON",
                "Toggle WP",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_WATCHPOINT_TOGGLE,
                NULL,
                NULL
            );
            app->debug.debugger_remove_selected_button = CreateWindowExA(
                0,
                "BUTTON",
                "Remove Sel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_REMOVE_SELECTED,
                NULL,
                NULL
            );
            app->debug.debugger_view_pc_button = CreateWindowExA(
                0,
                "BUTTON",
                "PC",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_VIEW_PC,
                NULL,
                NULL
            );
            app->debug.debugger_view_sp_button = CreateWindowExA(
                0,
                "BUTTON",
                "SP",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_VIEW_SP,
                NULL,
                NULL
            );
            app->debug.debugger_page_up_button = CreateWindowExA(
                0,
                "BUTTON",
                "Page -",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_PAGE_UP,
                NULL,
                NULL
            );
            app->debug.debugger_page_down_button = CreateWindowExA(
                0,
                "BUTTON",
                "Page +",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_PAGE_DOWN,
                NULL,
                NULL
            );
            app->debug.debugger_run_to_button = CreateWindowExA(
                0,
                "BUTTON",
                "Run To",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_RUN_TO,
                NULL,
                NULL
            );
            app->debug.debugger_run_at_button = CreateWindowExA(
                0,
                "BUTTON",
                "Run @",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_RUN_AT,
                NULL,
                NULL
            );
            app->debug.debugger_text = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                "EDIT",
                "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_TEXT,
                NULL,
                NULL
            );
            app->debug.debugger_points_list = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                "LISTBOX",
                "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                0,
                0,
                0,
                0,
                hwnd,
                (HMENU)(INT_PTR)APP_CTRL_DEBUG_POINTS_LIST,
                NULL,
                NULL
            );
            app->debug.debugger_accel = CreateAcceleratorTableA((LPACCEL)debugger_accels, (int)(sizeof(debugger_accels) / sizeof(debugger_accels[0])));
            SendMessageA(app->debug.debugger_address_edit, EM_SETLIMITTEXT, 15, 0);
            SendMessageA(app->debug.debugger_address_edit, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageA(app->debug.debugger_text, WM_SETFONT, (WPARAM)font, TRUE);
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
            app_debugger_layout_controls(app, hwnd);
            app_debug_sync_view_controls(app);
            app_debug_refresh_window(app);
            return 0;
        }
        case WM_SIZE:
            if (app != NULL) {
                app_debugger_layout_controls(app, hwnd);
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
            if (app == NULL) {
                break;
            }
            switch (LOWORD(wparam)) {
                case APP_CTRL_DEBUG_PAUSE:
                    if (!app->spec.machine_ready) {
                        return 0;
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
                    return 0;
                case APP_CTRL_DEBUG_STEP:
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    app_debug_step_instruction(app);
                    return 0;
                case APP_CTRL_DEBUG_STEP_OVER:
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    app_debug_step_over_instruction(app);
                    return 0;
                case APP_CTRL_DEBUG_REFRESH:
                    if (!app->spec.machine_ready) {
                        app_debug_refresh_window(app);
                        return 0;
                    }
                    app_debug_refresh_window(app);
                    return 0;
                case APP_CTRL_DEBUG_GO: {
                    char error[128];
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    if (!app_debug_apply_address_input(app, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    app_debug_refresh_window(app);
                    return 0;
                }
                case APP_CTRL_DEBUG_VIEW_PC:
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    app_debug_view_pc(app);
                    return 0;
                case APP_CTRL_DEBUG_VIEW_SP:
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    app_debug_view_sp(app);
                    return 0;
                case APP_CTRL_DEBUG_PAGE_UP:
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    app_debug_navigate_view(app, -256);
                    return 0;
                case APP_CTRL_DEBUG_PAGE_DOWN:
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    app_debug_navigate_view(app, 256);
                    return 0;
                case APP_CTRL_DEBUG_RUN_TO: {
                    char error[128];
                    uint16_t address;
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    app_debug_run_to_address(app, address);
                    return 0;
                }
                case APP_CTRL_DEBUG_BREAKPOINT_TOGGLE: {
                    char error[128];
                    uint16_t address;
                    bool enabled;
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    if (!app_debug_toggle_breakpoint(app, address, &enabled)) {
                        MessageBoxA(hwnd, "Could not update the breakpoint list.", "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    app_debug_refresh_window(app);
                    return 0;
                }
                case APP_CTRL_DEBUG_WATCHPOINT_TOGGLE: {
                    char error[128];
                    uint16_t address;
                    bool enabled;
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    if (!app_debug_toggle_watchpoint(app, address, &enabled)) {
                        MessageBoxA(hwnd, "Could not update the watchpoint list.", "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    app_debug_refresh_window(app);
                    return 0;
                }
                case APP_CTRL_DEBUG_REMOVE_SELECTED:
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    if (!app_debug_remove_selected_point(app)) {
                        MessageBoxA(hwnd, "Select a breakpoint or watchpoint first.", "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    app_debug_refresh_window(app);
                    return 0;
                case APP_CTRL_DEBUG_RUN_AT: {
                    char error[128];
                    uint16_t address;
                    if (!app->spec.machine_ready) {
                        return 0;
                    }
                    if (!app_debug_parse_address_input(app, &address, error, sizeof(error))) {
                        MessageBoxA(hwnd, error, "Debugger", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    app_debug_run_from_address(app, address);
                    return 0;
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
                        return 0;
                    }
                    break;
                case APP_CTRL_DEBUG_POINTS_LIST:
                    if (HIWORD(wparam) == LBN_SELCHANGE) {
                        if (!app->spec.machine_ready) {
                            app_debug_update_controls(app);
                            return 0;
                        }
                        uint16_t address = 0;
                        if (app_debug_get_selected_point(app, NULL, &address)) {
                            app_debug_set_view_address(app, address, false);
                            app_debug_refresh_window(app);
                        } else {
                            app_debug_update_controls(app);
                        }
                        return 0;
                    }
                    if (HIWORD(wparam) == LBN_DBLCLK) {
                        if (!app->spec.machine_ready) {
                            return 0;
                        }
                        if (app_debug_remove_selected_point(app)) {
                            app_debug_refresh_window(app);
                        }
                        return 0;
                    }
                    break;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (app != NULL) {
                app_set_modal_loop_timer(app, hwnd, false);
                app->debug.paused = false;
                app->debug.stepping = false;
                app->debug.stop_requested = false;
                app->debug.debugger_run_to_active = false;
                app->debug.debugger_run_to_hit = false;
                app->debug.debugger_hwnd = NULL;
                app->debug.debugger_text = NULL;
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
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* Hosts the modeless mini-assembler window used to patch Spectrum RAM from a
   small block of Z80 source code. */
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
            app->debug.assembler_font = app_create_monospace_font();
            font = app->debug.assembler_font != NULL
                ? app->debug.assembler_font
                : (HFONT)GetStockObject(ANSI_FIXED_FONT);
            menu = app_create_assembler_menu();
            if (menu != NULL) {
                SetMenu(hwnd, menu);
                DrawMenuBar(hwnd);
            }
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
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
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
                "Mini assembler with labels and Load/Save. ORG in the source sets the assembly address; otherwise the current PC is used."
            );
            app_assembler_refresh_window(app);
            return 0;
        }
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
                app->debug.assembler_error_line = 0;
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

/* Creates or focuses the debugger tool window from the Tools menu. */
static void app_open_debugger_window(AppState *app) {
    if (app->debug.debugger_hwnd != NULL) {
        ShowWindow(app->debug.debugger_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(app->debug.debugger_hwnd);
        app_debug_refresh_window(app);
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
}

/* Creates or focuses the assembler tool window from the Tools menu. */
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
    app_assembler_update_title(app);
    if (app_assembler_load_last_path(path, sizeof(path))) {
        if (app_assembler_load_source_path(path, app, status, sizeof(status))) {
            app_assembler_set_status(app, status);
        } else {
            app_assembler_set_status(app, status);
        }
    }
}

/* Shows a concise summary of the built-in assembler's supported directives,
   instruction subset, and RAM-only patching behavior. */
static void app_show_assembler_help(HWND hwnd) {
    MessageBoxA(
        hwnd,
        "Built-in Spectrum assembler\r\n"
        "\r\n"
        "Labels:\r\n"
        "  start:\r\n"
        "  loop: JR NZ,loop\r\n"
        "\r\n"
        "Directives:\r\n"
        "  ORG address\r\n"
        "  DB value[, value...]\r\n"
        "  DW value[, value...]\r\n"
        "  INCLUDE \"file.asm\"\r\n"
        "\r\n"
        "Editor:\r\n"
        "  Ctrl+N New\r\n"
        "  Ctrl+O Load\r\n"
        "  Ctrl+S Save\r\n"
        "  Ctrl+Shift+S Save As\r\n"
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
        "  - Quoted text works in DB, for example DB \"HELLO\",13.\r\n"
        "  - INCLUDE expands in place, so it can appear mid-file.\r\n"
        "  - INCLUDE paths are resolved relative to the current source file.\r\n"
        "  - Labels are supported.\r\n"
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
    } else {
        app->debug.assembler_current_path[0] = '\0';
        app_assembler_save_last_path("");
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

/* Opens a file picker for assembler source and loads the selected text file
   into the current assembler document. */
static bool app_assembler_load_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size) {
    OPENFILENAMEA ofn;
    char path[MAX_PATH];

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
    if (app_assemble_source(app, effective_address, &prepared_source, status, sizeof(status))) {
        InvalidateRect(app->main_hwnd, NULL, FALSE);
        app_debug_refresh_window(app);
    } else {
        app_assembler_sync_error_from_status(app, status);
    }
    app_assembler_free_prepared_source(&prepared_source);
    app_assembler_set_status(app, status);
    free(source);
}

/* Appends converted 16-bit PCM samples into the frontend's audio ring buffer,
   discarding the oldest data first if playback falls behind. */
static void app_audio_push_samples(AppState *app, const float *samples, int num_samples) {
    AudioState *audio = &app->audio;

    if (!audio->enabled) {
        return;
    }

    for (int i = 0; i < num_samples; ++i) {
        float sample = samples[i];
        int value;

        if (sample < -1.0f) {
            sample = -1.0f;
        } else if (sample > 1.0f) {
            sample = 1.0f;
        }
        value = (int)(sample * 32767.0f);
        if (audio->ring_count == APP_AUDIO_RING_SAMPLES) {
            audio->ring_read = (audio->ring_read + 1) % APP_AUDIO_RING_SAMPLES;
            audio->ring_count--;
        }
        audio->ring[audio->ring_write] = (int16_t)value;
        audio->ring_write = (audio->ring_write + 1) % APP_AUDIO_RING_SAMPLES;
        audio->ring_count++;
    }
}

/* Receives floating-point beeper samples from the emulator core and stores
   them in the frontend ring buffer for later waveOut submission. */
static void app_audio_callback(const float *samples, int num_samples, void *user_data) {
    AppState *app = (AppState *)user_data;
    if (app == NULL) {
        return;
    }
    app_audio_push_samples(app, samples, num_samples);
}

/* Refills any available waveOut buffers from the ring buffer, padding with
   silence when the emulator has not generated enough audio yet. */
static void app_audio_service(AppState *app) {
    AudioState *audio = &app->audio;

    if (!audio->enabled) {
        return;
    }

    for (size_t i = 0; i < APP_AUDIO_BUFFER_COUNT; ++i) {
        AudioBuffer *buffer = &audio->buffers[i];

        if ((buffer->header.dwFlags & WHDR_INQUEUE) != 0) {
            continue;
        }

        for (size_t sample_index = 0; sample_index < APP_AUDIO_BUFFER_SAMPLES; ++sample_index) {
            if (audio->ring_count > 0) {
                buffer->samples[sample_index] = audio->ring[audio->ring_read];
                audio->ring_read = (audio->ring_read + 1) % APP_AUDIO_RING_SAMPLES;
                audio->ring_count--;
            } else {
                buffer->samples[sample_index] = 0;
            }
        }

        buffer->header.lpData = (LPSTR)buffer->samples;
        buffer->header.dwBufferLength = (DWORD)sizeof(buffer->samples);
        buffer->header.dwBytesRecorded = 0;
        buffer->header.dwUser = 0;
        buffer->header.dwFlags &= ~WHDR_DONE;
        buffer->header.dwLoops = 0;
        waveOutWrite(audio->device, &buffer->header, sizeof(buffer->header));
    }
}

/* Drops any queued frontend audio so resets, model switches, and snapshot
   loads restart sound output from the new machine state immediately. */
static void app_audio_flush(AppState *app) {
    AudioState *audio = &app->audio;

    audio->ring_read = 0;
    audio->ring_write = 0;
    audio->ring_count = 0;
    if (audio->device != NULL) {
        waveOutReset(audio->device);
    }
}

/* Opens a simple mono PCM waveOut device for Spectrum beeper playback and
   prepares a small pool of reusable output buffers. */
static bool app_audio_init(AppState *app) {
    AudioState *audio = &app->audio;
    WAVEFORMATEX format;
    MMRESULT result;

    ZeroMemory(&format, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = APP_AUDIO_SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (WORD)(format.nChannels * (format.wBitsPerSample / 8));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    result = waveOutOpen(&audio->device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        audio->device = NULL;
        audio->enabled = false;
        return false;
    }

    for (size_t i = 0; i < APP_AUDIO_BUFFER_COUNT; ++i) {
        ZeroMemory(&audio->buffers[i].header, sizeof(audio->buffers[i].header));
        audio->buffers[i].header.lpData = (LPSTR)audio->buffers[i].samples;
        audio->buffers[i].header.dwBufferLength = (DWORD)sizeof(audio->buffers[i].samples);
        result = waveOutPrepareHeader(audio->device, &audio->buffers[i].header, sizeof(audio->buffers[i].header));
        if (result != MMSYSERR_NOERROR) {
            waveOutReset(audio->device);
            for (size_t j = 0; j < i; ++j) {
                waveOutUnprepareHeader(audio->device, &audio->buffers[j].header, sizeof(audio->buffers[j].header));
            }
            waveOutClose(audio->device);
            audio->device = NULL;
            audio->enabled = false;
            return false;
        }
    }

    audio->enabled = true;
    app_audio_service(app);
    return true;
}

/* Drains, unprepares, and closes the waveOut device owned by the frontend. */
static void app_audio_shutdown(AppState *app) {
    AudioState *audio = &app->audio;

    if (audio->device == NULL) {
        audio->enabled = false;
        return;
    }

    waveOutReset(audio->device);
    for (size_t i = 0; i < APP_AUDIO_BUFFER_COUNT; ++i) {
        waveOutUnprepareHeader(audio->device, &audio->buffers[i].header, sizeof(audio->buffers[i].header));
    }
    waveOutClose(audio->device);
    audio->device = NULL;
    audio->enabled = false;
}

/* Normalizes printable ASCII for Spectrum text entry so host uppercase letters
   still press the base Spectrum key while punctuation is preserved as-is. */
static int app_normalize_text_char(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return 'a' + (ch - 'A');
    }
    return ch;
}

/* Returns a fixed Spectrum key code for non-text keys whose host bindings
   should not depend on the active keyboard layout. */
static int app_lookup_special_key(UINT vk) {
    for (size_t i = 0; i < sizeof(SPECIAL_KEY_MAP) / sizeof(SPECIAL_KEY_MAP[0]); ++i) {
        if (SPECIAL_KEY_MAP[i].vk == vk) {
            return SPECIAL_KEY_MAP[i].code;
        }
    }
    return 0;
}

/* Resolves plain alpha-numeric host keys to their Spectrum counterparts when
   Windows does not translate them into a printable ASCII character. */
static int app_lookup_fallback_key(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return 'a' + (int)(vk - 'A');
    }
    if (vk >= '0' && vk <= '9') {
        return (int)vk;
    }
    if (vk == VK_SPACE) {
        return ' ';
    }
    return 0;
}

/* Uses the active Windows keyboard layout to translate a key press into the
   printable ASCII character the user actually typed, such as `"` on UK Shift+2. */
static int app_translate_text_key(UINT vk, LPARAM lparam) {
    BYTE keyboard_state[256];
    WCHAR translated[4];
    HKL layout;
    UINT scan_code;
    int count;

    if (!GetKeyboardState(keyboard_state)) {
        return 0;
    }

    layout = GetKeyboardLayout(0);
    scan_code = (UINT)((lparam >> 16) & 0xFF);
    count = ToUnicodeEx(vk, scan_code, keyboard_state, translated, 4, 0, layout);
    if (count == 1 && translated[0] > 0 && translated[0] <= 0x7F) {
        return app_normalize_text_char((int)translated[0]);
    }
    return 0;
}

/* Maps a Win32 key-down event to the Spectrum key code that should remain held
   until the corresponding host key-up arrives. */
static int app_resolve_key_down(WPARAM wparam, LPARAM lparam) {
    UINT vk = (UINT)wparam;
    int special = app_lookup_special_key(vk);
    int translated;

    if (special != 0) {
        return special;
    }

    if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) {
        translated = app_translate_text_key(vk, lparam);
        if (translated != 0) {
            return translated;
        }
    }

    return app_lookup_fallback_key(vk);
}

/* Converts internal auto-type text into the limited ASCII subset needed by
   the tape autoload command queue. */
static int app_autotype_translate_char(WCHAR ch) {
    if (ch == L'\r' || ch == L'\n') {
        return 0x0D;
    }
    if (ch == L'\t') {
        return ' ';
    }
    if (ch >= 0x20 && ch <= 0x7E) {
        return app_normalize_text_char((int)ch);
    }
    return 0;
}

/* Clears any pending auto-type command and releases any injected key that is
   still being held across frames. */
static void app_autotype_clear(AppState *app) {
    if (app->auto_type.active_key_code != 0) {
        spectrum_key_up(&app->spec, app->auto_type.active_key_code);
    }
    app->auto_type.length = 0;
    app->auto_type.position = 0;
    app->auto_type.cooldown_frames = 0;
    app->auto_type.hold_frames = 0;
    app->auto_type.active_key_code = 0;
    app->tape_autoplay_pending = false;
}

/* Releases every currently held Spectrum key, for example after the emulator
   window loses focus while the user is still holding host keys down. */
static void app_release_all_keys(AppState *app) {
    for (size_t i = 0; i < sizeof(app->active_key_codes) / sizeof(app->active_key_codes[0]); ++i) {
        if (app->active_key_codes[i] != 0) {
            spectrum_key_up(&app->spec, app->active_key_codes[i]);
            app->active_key_codes[i] = 0;
        }
    }
}

/* Samples the inserted tape image at the current emulated t-state so ULA
   reads can see the waveform on EAR bit 6. */
static bool app_tape_input_callback(void *user_data, uint64_t tick_count) {
    AppState *app = (AppState *)user_data;
    return tape_input_level(&app->tape, tick_count);
}

/* Fast-traps the standard ROM tape loader when a TAP/TZX block can be
   injected directly instead of being edge-timed through the EAR bit. */
static bool app_tape_fast_load_trap(void *user_data, void *machine) {
    AppState *app = (AppState *)user_data;
    return tape_try_fast_load(&app->tape, (zx_t *)machine);
}

/* Queues a short ROM-facing command sequence, such as the tape autoload
   keystrokes, for paced injection over later emulator frames. */
static bool app_start_autotype(AppState *app, const WCHAR *text) {
    bool queued = false;

    app_release_all_keys(app);
    app_autotype_clear(app);

    for (size_t i = 0; text[i] != L'\0'; ++i) {
        int key_code;
        if (text[i] == L'\r' && text[i + 1] == L'\n') {
            continue;
        }

        key_code = app_autotype_translate_char(text[i]);
        if (key_code == 0) {
            continue;
        }
        if (app->auto_type.length >= sizeof(app->auto_type.buffer) - 1) {
            break;
        }

        app->auto_type.buffer[app->auto_type.length++] = (char)key_code;
        queued = true;
    }

    return queued;
}

/* Runs tape file I/O and decode work away from the window thread so opening
   larger TAP/TZX files does not stall input or trigger the busy cursor. */
static DWORD WINAPI app_tape_load_worker(void *param) {
    AppTapeLoadJob *job = (AppTapeLoadJob *)param;

    tape_init(&job->tape);
    job->ok = tape_load_file(
        &job->tape,
        job->path,
        job->tick_hz,
        job->error,
        sizeof(job->error)
    );
    if (!PostMessageA(job->hwnd, APP_MSG_TAPE_LOAD_COMPLETE, 0, (LPARAM)job)) {
        if (job->ok) {
            tape_discard(&job->tape);
        }
        free(job);
    }
    return 0;
}

/* Runs snapshot file I/O and model detection away from the window thread so
   loading larger `.z80` files does not stall painting or input processing. */
static DWORD WINAPI app_snapshot_load_worker(void *param) {
    AppSnapshotLoadJob *job = (AppSnapshotLoadJob *)param;

    job->ok = app_read_file_all(
        job->path,
        &job->data,
        &job->size,
        job->error,
        sizeof(job->error),
        "snapshot"
    );
    if (job->ok) {
        if (!spectrum_detect_snapshot_model_data(job->data, job->size, &job->model)) {
            snprintf(job->error, sizeof(job->error), "Unsupported or corrupt .z80 snapshot: %s", job->path);
            job->ok = false;
        }
    }
    if (!PostMessageA(job->hwnd, APP_MSG_SNAPSHOT_LOAD_COMPLETE, 0, (LPARAM)job)) {
        free(job->data);
        free(job);
    }
    return 0;
}

/* Starts an asynchronous tape decode job and returns immediately so the UI
   thread can keep pumping messages while file loading happens in the background. */
static bool app_load_tape_file(
    HWND hwnd,
    AppState *app,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
) {
    AppTapeLoadJob *job;
    HANDLE thread_handle;

    if (app->tape_load_in_progress) {
        snprintf(error_buffer, error_buffer_size, "A tape is already loading.");
        return false;
    }

    job = (AppTapeLoadJob *)calloc(1, sizeof(*job));
    if (job == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while starting tape load.");
        return false;
    }

    job->hwnd = hwnd;
    job->tick_hz = (uint32_t)app->spec.machine.freq_hz;
    snprintf(job->path, sizeof(job->path), "%s", path);

    thread_handle = CreateThread(NULL, 0, app_tape_load_worker, job, 0, NULL);
    if (thread_handle == NULL) {
        free(job);
        snprintf(error_buffer, error_buffer_size, "Could not create tape loading thread.");
        return false;
    }

    CloseHandle(thread_handle);
    app->tape_load_in_progress = true;
    return true;
}

/* Starts an asynchronous snapshot file read/validation job and returns
   immediately so the UI thread stays responsive while disk work completes. */
static bool app_load_snapshot_file(
    HWND hwnd,
    AppState *app,
    const char *path,
    char *error_buffer,
    size_t error_buffer_size
) {
    AppSnapshotLoadJob *job;
    HANDLE thread_handle;

    if (app->snapshot_load_in_progress) {
        snprintf(error_buffer, error_buffer_size, "A snapshot is already loading.");
        return false;
    }

    job = (AppSnapshotLoadJob *)calloc(1, sizeof(*job));
    if (job == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory while starting snapshot load.");
        return false;
    }
    job->hwnd = hwnd;
    snprintf(job->path, sizeof(job->path), "%s", path);

    thread_handle = CreateThread(NULL, 0, app_snapshot_load_worker, job, 0, NULL);
    if (thread_handle == NULL) {
        free(job);
        snprintf(error_buffer, error_buffer_size, "Could not create snapshot loading thread.");
        return false;
    }

    CloseHandle(thread_handle);
    app->snapshot_load_in_progress = true;
    return true;
}

/* Inserts an already decoded TAP/TZX image, then either auto-starts a standard
   ROM load or leaves the tape inserted for manual control depending on user
   preference. This runs on the main thread after background file decode ends. */
static bool app_apply_loaded_tape(
    HWND hwnd,
    AppState *app,
    TapePlayer *loaded_tape,
    char *error_buffer,
    size_t error_buffer_size
) {
    const TapeAutoloadTarget autoload_target = tape_autoload_target(loaded_tape);
    SpectrumModel autoload_model = app->spec.model;

    tape_discard(&app->tape);
    app->tape = *loaded_tape;
    tape_stop(&app->tape);

    if (app->tape_autoload_enabled) {
        if (autoload_target == TAPE_AUTOLOAD_TARGET_128_MENU) {
            autoload_model = SPECTRUM_MODEL_128K;
        } else if (autoload_target == TAPE_AUTOLOAD_TARGET_48_BASIC) {
            autoload_model = SPECTRUM_MODEL_48K;
        }

        app_autotype_clear(app);
        app_release_all_keys(app);
        app_audio_flush(app);
        if (autoload_model != app->spec.model) {
            const RomSet *set = app_rom_set_for_model_const(&app->roms, autoload_model);
            if (!app_load_model_roms(app, autoload_model, set, error_buffer, error_buffer_size)) {
                return false;
            }
            app_update_title(hwnd, &app->spec);
            app_update_model_menu(hwnd, app->spec.model);
        }
        spectrum_reset(&app->spec);
        app_sync_tape_stop_mode(app);
        tape_rewind(&app->tape, (uint32_t)app->spec.machine.freq_hz);
        if (autoload_model == SPECTRUM_MODEL_128K) {
            app->tape_autoplay_pending = app_start_autotype(app, L"\r");
        } else {
            app->tape_autoplay_pending = app_start_autotype(app, L"j\"\"\r");
        }
        if (app->tape_autoplay_pending) {
            app->auto_type.cooldown_frames = APP_AUTOTYPE_BOOT_DELAY_FRAMES;
        }
        if (!app->tape_autoplay_pending) {
            tape_start(&app->tape, app->spec.machine.tick_count);
        }
    } else {
        app_sync_tape_stop_mode(app);
        tape_rewind(&app->tape, (uint32_t)app->spec.machine.freq_hz);
    }

    app_debug_machine_changed(app);
    InvalidateRect(hwnd, NULL, FALSE);
    return true;
}

/* Applies a background-loaded snapshot on the main thread, rebuilding the
   machine only if the decoded `.z80` payload targets a different model. */
static bool app_apply_loaded_snapshot(
    HWND hwnd,
    AppState *app,
    AppSnapshotLoadJob *job,
    char *error_buffer,
    size_t error_buffer_size
) {
    const bool model_changed = job->model != app->spec.model;
    const Spectrum previous_spec = app->spec;

    app_autotype_clear(app);
    app_release_all_keys(app);
    app_audio_flush(app);
    tape_stop(&app->tape);

    if (model_changed) {
        const RomSet *set = app_rom_set_for_model_const(&app->roms, job->model);
        if (!app_load_model_roms(app, job->model, set, error_buffer, error_buffer_size)) {
            return false;
        }
    }
    if (!spectrum_load_snapshot_z80_data(&app->spec, job->data, job->size, error_buffer, error_buffer_size)) {
        if (model_changed) {
            app->spec = previous_spec;
            app_sync_tape_stop_mode(app);
        }
        return false;
    }

    app_audio_flush(app);
    app_sync_tape_stop_mode(app);
    if (model_changed) {
        tape_rewind(&app->tape, (uint32_t)app->spec.machine.freq_hz);
    }
    app_update_title(hwnd, &app->spec);
    app_update_model_menu(hwnd, app->spec.model);
    app_debug_machine_changed(app);
    InvalidateRect(hwnd, NULL, FALSE);
    return true;
}

/* Advances the queued text-entry path once per frame, tapping a single
   Spectrum key with a short gap between characters for ROM scanning safety. */
static void app_run_autotype(AppState *app) {
    if (app->auto_type.active_key_code != 0) {
        if (app->auto_type.hold_frames > 0) {
            app->auto_type.hold_frames--;
            return;
        }
        spectrum_key_up(&app->spec, app->auto_type.active_key_code);
        app->auto_type.active_key_code = 0;
        app->auto_type.cooldown_frames = APP_AUTOTYPE_COOLDOWN_FRAMES;
        if (app->auto_type.position >= app->auto_type.length) {
            app->auto_type.length = 0;
            app->auto_type.position = 0;
            if (app->tape_autoplay_pending) {
                app->tape_autoplay_pending = false;
                tape_start(&app->tape, app->spec.machine.tick_count);
            }
        }
        return;
    }
    if (app->auto_type.cooldown_frames > 0) {
        app->auto_type.cooldown_frames--;
        return;
    }
    if (app->auto_type.position >= app->auto_type.length) {
        return;
    }

    app->auto_type.active_key_code = (unsigned char)app->auto_type.buffer[app->auto_type.position++];
    spectrum_key_down(&app->spec, app->auto_type.active_key_code);
    app->auto_type.hold_frames = APP_AUTOTYPE_HOLD_FRAMES;
}

/* Starts the inserted tape from the current position, rewinding only after
   the decoded waveform has already reached the end. */
static void app_play_tape(AppState *app) {
    if (!tape_has_tape(&app->tape)) {
        return;
    }
    app->tape_autoplay_pending = false;
    app_sync_tape_stop_mode(app);
    tape_start(&app->tape, app->spec.machine.tick_count);
}

/* Stops the current tape playback position without ejecting the inserted tape. */
static void app_stop_tape(AppState *app) {
    app->tape_autoplay_pending = false;
    tape_stop(&app->tape);
}

static void app_sync_tape_stop_mode(AppState *app) {
    tape_set_stop_mode(&app->tape, app->spec.model == SPECTRUM_MODEL_48K);
}

/* Switches the running machine between 48K and 128K by reloading the ROM
   bundle assigned to that model, then persists the user's choice. */
static void app_switch_model(HWND hwnd, AppState *app, SpectrumModel model) {
    char error_buffer[256];
    const RomSet *set = app_rom_set_for_model_const(&app->roms, model);

    if (app->spec.model == model) {
        app_save_model(model);
        app_update_model_menu(hwnd, model);
        app_audio_flush(app);
        return;
    }

    app_autotype_clear(app);
    app_release_all_keys(app);
    app_audio_flush(app);
    tape_stop(&app->tape);
    if (!app_load_model_roms(app, model, set, error_buffer, sizeof(error_buffer))) {
        app_show_error(error_buffer);
        app_update_model_menu(hwnd, app->spec.model);
        return;
    }
    app_sync_tape_stop_mode(app);
    tape_rewind(&app->tape, (uint32_t)app->spec.machine.freq_hz);

    app_save_model(model);
    app_update_title(hwnd, &app->spec);
    app_update_model_menu(hwnd, app->spec.model);
    app_debug_machine_changed(app);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Tracks which Spectrum key code each host virtual key resolved to so that
   translated text keys and held game controls both release correctly. */
static void app_handle_key_down(AppState *app, WPARAM wparam, LPARAM lparam) {
    UINT vk = (UINT)wparam;
    int key_code;

    if (vk >= sizeof(app->active_key_codes) / sizeof(app->active_key_codes[0])) {
        return;
    }

    key_code = app_resolve_key_down(wparam, lparam);
    if (key_code == 0) {
        return;
    }

    if (app->active_key_codes[vk] != 0 && app->active_key_codes[vk] != key_code) {
        spectrum_key_up(&app->spec, app->active_key_codes[vk]);
    }

    app->active_key_codes[vk] = key_code;
    spectrum_key_down(&app->spec, key_code);
}

/* Releases the exact Spectrum key code associated with the host virtual key
   that has just been released by Windows. */
static void app_handle_key_up(AppState *app, WPARAM wparam) {
    UINT vk = (UINT)wparam;
    int key_code;

    if (vk >= sizeof(app->active_key_codes) / sizeof(app->active_key_codes[0])) {
        return;
    }

    key_code = app->active_key_codes[vk];
    if (key_code == 0) {
        return;
    }

    spectrum_key_up(&app->spec, key_code);
    app->active_key_codes[vk] = 0;
}

/* Returns the real PAL frame duration for the selected Spectrum model so the
   frontend can step 48K and 128K machines at their native rates. */
static double app_model_frame_duration_ms(SpectrumModel model) {
    if (model == SPECTRUM_MODEL_128K) {
        return (311.0 * 228.0 * 1000.0) / 3546894.0;
    }
    return (312.0 * 224.0 * 1000.0) / 3500000.0;
}

/* Runs one timing slice of the emulator and audio service. This is shared by
   the normal idle loop and the timer path used during modal move/size loops. */
static void app_tick_emulator(AppState *app) {
    LARGE_INTEGER now;
    double elapsed_ms;
    double frame_ms;
    bool rendered = false;
    int frames_run = 0;

    if (app == NULL || !app->running) {
        return;
    }

    QueryPerformanceCounter(&now);
    elapsed_ms = (double)(now.QuadPart - app->last_tick.QuadPart) * 1000.0 / (double)app->perf_freq.QuadPart;
    app->last_tick = now;
    frame_ms = app_model_frame_duration_ms(app->spec.model);
    app->emulation_accumulator_ms += elapsed_ms;
    if (app->emulation_accumulator_ms > frame_ms * 8.0) {
        app->emulation_accumulator_ms = frame_ms * 8.0;
    }

    app_controller_poll(app);

    if (!app->debug.paused) {
        while (app->emulation_accumulator_ms >= frame_ms && frames_run < 8) {
            app_run_autotype(app);
            app_debug_run_frame(app);
            app->emulation_accumulator_ms -= frame_ms;
            rendered = true;
            frames_run++;
            if (app->debug.paused) {
                app->emulation_accumulator_ms = 0.0;
                break;
            }
        }
        if (rendered) {
            InvalidateRect(app->main_hwnd, NULL, FALSE);
        }
    } else {
        app->emulation_accumulator_ms = 0.0;
    }
    app_audio_service(app);
}

/* Keeps the emulator ticking while Windows is inside a modal move/size loop
   for any of the app's top-level windows. */
static void app_set_modal_loop_timer(AppState *app, HWND hwnd, bool enabled) {
    if (app == NULL) {
        return;
    }
    if (enabled) {
        if (!app->modal_loop_timer_active) {
            SetTimer(hwnd, APP_TIMER_MODAL_LOOP, 20, NULL);
            app->modal_loop_timer_active = true;
        }
    } else if (app->modal_loop_timer_active) {
        KillTimer(hwnd, APP_TIMER_MODAL_LOOP);
        app->modal_loop_timer_active = false;
    }
}

/* Acts as the Win32 window procedure, routing keyboard input, paint events,
   and shutdown messages between the OS and the emulator application state. */
static LRESULT CALLBACK app_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *create = (CREATESTRUCTA *)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
            return TRUE;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (app != NULL) {
                const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool alt_down = (GetKeyState(VK_MENU) & 0x8000) != 0;

                if (ctrl_down && shift_down) {
                    switch ((UINT)wparam) {
                        case 'A':
                            app_open_assembler_window(app);
                            return 0;
                        case 'D':
                            app_open_debugger_window(app);
                            return 0;
                        default:
                            break;
                    }
                }
                if (ctrl_down && !shift_down && !alt_down) {
                    switch ((UINT)wparam) {
                        case 'O':
                            SendMessageA(hwnd, WM_COMMAND, APP_MENU_FILE_OPEN_SNAPSHOT, 0);
                            return 0;
                        case 'R':
                            SendMessageA(hwnd, WM_COMMAND, APP_MENU_FILE_RESET, 0);
                            return 0;
                        case '1':
                        case VK_NUMPAD1:
                            SendMessageA(hwnd, WM_COMMAND, APP_MENU_MACHINE_48K, 0);
                            return 0;
                        case '2':
                        case VK_NUMPAD2:
                            SendMessageA(hwnd, WM_COMMAND, APP_MENU_MACHINE_128K, 0);
                            return 0;
                        default:
                            break;
                    }
                }
                if (alt_down && (UINT)wparam == VK_F4) {
                    SendMessageA(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                if (!ctrl_down && !shift_down && !alt_down) {
                    switch ((UINT)wparam) {
                        case VK_F3:
                            SendMessageA(hwnd, WM_COMMAND, APP_MENU_FILE_PLAY_TAPE, 0);
                            return 0;
                        case VK_F4:
                            SendMessageA(hwnd, WM_COMMAND, APP_MENU_FILE_STOP_TAPE, 0);
                            return 0;
                        default:
                            break;
                    }
                }
                app_handle_key_down(app, wparam, lparam);
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (app != NULL) {
                app_handle_key_up(app, wparam);
            }
            return 0;
        case WM_KILLFOCUS:
            if (app != NULL) {
                app_release_all_keys(app);
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
        case APP_MSG_TAPE_LOAD_COMPLETE:
            if (app != NULL) {
                AppTapeLoadJob *job = (AppTapeLoadJob *)lparam;
                app->tape_load_in_progress = false;
                if (job != NULL) {
                    if (!job->ok) {
                        app_show_error(job->error);
                    } else if (!app_apply_loaded_tape(hwnd, app, &job->tape, job->error, sizeof(job->error))) {
                        app_show_error(job->error);
                    } else {
                        tape_init(&job->tape);
                    }
                    tape_discard(&job->tape);
                    free(job);
                }
                return 0;
            }
            break;
        case APP_MSG_SNAPSHOT_LOAD_COMPLETE:
            if (app != NULL) {
                AppSnapshotLoadJob *job = (AppSnapshotLoadJob *)lparam;
                app->snapshot_load_in_progress = false;
                if (job != NULL) {
                    if (!job->ok) {
                        app_show_error(job->error);
                    } else if (!app_apply_loaded_snapshot(hwnd, app, job, job->error, sizeof(job->error))) {
                        app_show_error(job->error);
                    }
                    free(job->data);
                    free(job);
                }
                return 0;
            }
            break;
        case WM_COMMAND:
            if (app != NULL) {
                switch (LOWORD(wparam)) {
                    case APP_MENU_FILE_OPEN_SNAPSHOT: {
                        OPENFILENAMEA ofn;
                        char path[MAX_PATH];
                        char error_buffer[256];
                        const char *extension;

                        ZeroMemory(&ofn, sizeof(ofn));
                        ZeroMemory(path, sizeof(path));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hwnd;
                        ofn.lpstrFilter =
                            "ZX Spectrum Files (*.tap;*.tzx;*.z80)\0*.tap;*.tzx;*.z80\0"
                            "Tape Images (*.tap;*.tzx)\0*.tap;*.tzx\0"
                            "ZX Spectrum Snapshots (*.z80)\0*.z80\0"
                            "All Files (*.*)\0*.*\0";
                        ofn.lpstrFile = path;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                        ofn.lpstrDefExt = "tap";

                        if (GetOpenFileNameA(&ofn)) {
                            extension = strrchr(path, '.');
                            if (extension != NULL &&
                                (_stricmp(extension, ".tap") == 0 || _stricmp(extension, ".tzx") == 0)) {
                                if (!app_load_tape_file(hwnd, app, path, error_buffer, sizeof(error_buffer))) {
                                    app_show_error(error_buffer);
                                }
                                return 0;
                            }

                            if (extension != NULL && _stricmp(extension, ".z80") == 0) {
                                if (!app_load_snapshot_file(hwnd, app, path, error_buffer, sizeof(error_buffer))) {
                                    app_show_error(error_buffer);
                                }
                                return 0;
                            }

                            app_show_error("Unsupported file type. Open a .tap, .tzx, or .z80 file.");
                        }
                        return 0;
                    }
                    case APP_MENU_FILE_PLAY_TAPE:
                        app_play_tape(app);
                        return 0;
                    case APP_MENU_FILE_STOP_TAPE:
                        app_stop_tape(app);
                        return 0;
                    case APP_MENU_FILE_AUTOLOAD_TAPES:
                        app->tape_autoload_enabled = !app->tape_autoload_enabled;
                        app_save_tape_autoload(app->tape_autoload_enabled);
                        app_update_tape_menu(hwnd, app->tape_autoload_enabled);
                        return 0;
                    case APP_MENU_FILE_RESET:
                        app_autotype_clear(app);
                        app_release_all_keys(app);
                        app_audio_flush(app);
                        tape_stop(&app->tape);
                        spectrum_reset(&app->spec);
                        tape_rewind(&app->tape, (uint32_t)app->spec.machine.freq_hz);
                        app_debug_machine_changed(app);
                        InvalidateRect(hwnd, NULL, FALSE);
                        return 0;
                    case APP_MENU_MACHINE_48K:
                        app_switch_model(hwnd, app, SPECTRUM_MODEL_48K);
                        return 0;
                    case APP_MENU_MACHINE_128K:
                        app_switch_model(hwnd, app, SPECTRUM_MODEL_128K);
                        return 0;
                    case APP_MENU_TOOLS_ASSEMBLER:
                        app_open_assembler_window(app);
                        return 0;
                    case APP_MENU_TOOLS_DEBUGGER:
                        app_open_debugger_window(app);
                        return 0;
                    case APP_MENU_FILE_EXIT:
                        SendMessageA(hwnd, WM_CLOSE, 0, 0);
                        return 0;
                    default:
                        break;
                }
            }
            break;
        case WM_PAINT:
            if (app != NULL) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rect;
                GetClientRect(hwnd, &rect);
                StretchDIBits(
                    hdc,
                    0,
                    0,
                    rect.right - rect.left,
                    rect.bottom - rect.top,
                    0,
                    0,
                    ZX_SCREEN_WIDTH,
                    ZX_SCREEN_HEIGHT,
                    app->spec.framebuffer,
                    &app->bitmap_info,
                    DIB_RGB_COLORS,
                    SRCCOPY
                );
                EndPaint(hwnd, &ps);
            }
            return 0;
        case WM_CLOSE:
            if (app != NULL && app->debug.assembler_hwnd != NULL) {
                HWND assembler_hwnd = app->debug.assembler_hwnd;
                SendMessageA(assembler_hwnd, WM_CLOSE, 0, 0);
                if (app->debug.assembler_hwnd == assembler_hwnd) {
                    return 0;
                }
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (app != NULL) {
                app_set_modal_loop_timer(app, hwnd, false);
                if (app->debug.debugger_hwnd != NULL) {
                    DestroyWindow(app->debug.debugger_hwnd);
                }
                if (app->debug.assembler_hwnd != NULL) {
                    DestroyWindow(app->debug.assembler_hwnd);
                }
                app->running = false;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wparam, lparam);
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

/* Parses optional `--48` or `--128` flags plus explicit ROM path arguments,
   returning false only when too many positional ROM paths are provided. */
static bool app_parse_args(
    int argc,
    char **argv,
    SpectrumModel *model,
    bool *model_explicit,
    const char **rom_a,
    const char **rom_b
) {
    *model = SPECTRUM_MODEL_48K;
    *model_explicit = false;
    *rom_a = NULL;
    *rom_b = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--128") == 0) {
            *model = SPECTRUM_MODEL_128K;
            *model_explicit = true;
            continue;
        }
        if (strcmp(argv[i], "--48") == 0) {
            *model = SPECTRUM_MODEL_48K;
            *model_explicit = true;
            continue;
        }
        if (*rom_a == NULL) {
            *rom_a = argv[i];
        } else if (*rom_b == NULL) {
            *rom_b = argv[i];
        } else {
            return false;
        }
    }

    return true;
}

/* Prints the supported command line forms and the default ROM autodetection
   behavior used when the emulator is started without arguments. */
static void app_print_usage(void) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  zxspecemu --48  <48k.rom>\n");
    fprintf(stderr, "  zxspecemu --128 <128k-0.rom> <128k-1.rom>\n");
    fprintf(stderr, "  zxspecemu --128 <128k-combined-32k.rom>\n");
    fprintf(stderr, "  zxspecemu\n");
    fprintf(stderr, "\nAutodetects .\\128.rom first, then .\\48.rom if present.\n");
}

/* Resolves the persisted settings file path, preferring `%APPDATA%` so the
   user's selected model survives rebuilds and different working folders. */
static bool app_settings_path(char *out_path, size_t out_size) {
    char appdata[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        int written = snprintf(out_path, out_size, "%s\\zxspecemu.ini", appdata);
        return written > 0 && (size_t)written < out_size;
    }

    if (GetModuleFileNameA(NULL, appdata, MAX_PATH) == 0 || appdata[0] == '\0') {
        return false;
    }
    if (!app_parent_dir(appdata)) {
        return false;
    }

    {
        int written = snprintf(out_path, out_size, "%s\\zxspecemu.ini", appdata);
        return written > 0 && (size_t)written < out_size;
    }
}

/* Reads the user's preferred startup machine model from the persisted INI
   file, returning false when no valid selection has been stored yet. */
static bool app_load_saved_model(SpectrumModel *model) {
    char path[MAX_PATH];
    char value[16];

    if (!app_settings_path(path, sizeof(path))) {
        return false;
    }

    GetPrivateProfileStringA("emulator", "model", "", value, sizeof(value), path);
    if (strcmp(value, "128") == 0) {
        *model = SPECTRUM_MODEL_128K;
        return true;
    }
    if (strcmp(value, "48") == 0) {
        *model = SPECTRUM_MODEL_48K;
        return true;
    }
    return false;
}

/* Reads the persisted tape auto-load setting, defaulting to enabled when
   the user has not explicitly chosen otherwise yet. */
static bool app_load_saved_tape_autoload(bool *enabled) {
    char path[MAX_PATH];
    char value[16];

    if (!app_settings_path(path, sizeof(path))) {
        return false;
    }

    GetPrivateProfileStringA("emulator", "tape_autoload", "", value, sizeof(value), path);
    if (strcmp(value, "1") == 0) {
        *enabled = true;
        return true;
    }
    if (strcmp(value, "0") == 0) {
        *enabled = false;
        return true;
    }
    return false;
}

/* Persists the user's menu-selected machine preference so future launches
   start in the same 48K or 128K mode when possible. */
static void app_save_model(SpectrumModel model) {
    char path[MAX_PATH];
    if (!app_settings_path(path, sizeof(path))) {
        return;
    }
    WritePrivateProfileStringA(
        "emulator",
        "model",
        app_model_settings_value(model),
        path
    );
}

/* Persists whether opening a tape should immediately begin the standard ROM
   loading flow or simply insert the image for manual control. */
static void app_save_tape_autoload(bool enabled) {
    char path[MAX_PATH];
    if (!app_settings_path(path, sizeof(path))) {
        return;
    }
    WritePrivateProfileStringA(
        "emulator",
        "tape_autoload",
        app_tape_autoload_settings_value(enabled),
        path
    );
}

/* Loads the persisted last-used assembler source path from the settings file. */
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

/* Returns true only when the supplied path exists and resolves to a file
   rather than a directory. */
static bool app_file_exists(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/* Joins two path fragments with a single backslash and reports whether the
   resulting string fit inside the caller-provided output buffer. */
static bool app_join_path(char *out, size_t out_size, const char *left, const char *right) {
    int written = snprintf(out, out_size, "%s\\%s", left, right);
    return written > 0 && (size_t)written < out_size;
}

/* Mutates a path string in place so it becomes its parent directory and
   returns false if there is no directory separator to trim. */
static bool app_parent_dir(char *path) {
    char *slash = strrchr(path, '\\');
    if (slash == NULL) {
        return false;
    }
    *slash = '\0';
    return true;
}

/* Releases any heap storage attached to a prepared assembler source bundle. */
static void app_assembler_free_prepared_source(AssemblerPreparedSource *prepared) {
    if (prepared == NULL) {
        return;
    }
    free(prepared->text);
    free(prepared->locations);
    memset(prepared, 0, sizeof(*prepared));
}

/* Returns true when the supplied path is already absolute on Windows. */
static bool app_path_is_absolute(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (path[0] == '\\' || path[0] == '/') {
        return true;
    }
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

/* Converts a raw include operand into a plain path, accepting either quoted or
   unquoted forms so `INCLUDE file.asm` and `INCLUDE "file.asm"` both work. */
static bool app_assembler_extract_include_path(
    const char *operand,
    char *out_path,
    size_t out_size,
    char *error_buffer,
    size_t error_buffer_size
) {
    char token[512];
    char *trimmed;
    size_t length;

    snprintf(token, sizeof(token), "%s", operand);
    trimmed = app_trim_inplace(token);
    if (trimmed != token) {
        memmove(token, trimmed, strlen(trimmed) + 1);
    }
    length = strlen(token);
    if (length == 0) {
        snprintf(error_buffer, error_buffer_size, "INCLUDE expects a file path.");
        return false;
    }
    if ((token[0] == '"' || token[0] == '\'') && length >= 2 && token[length - 1] == token[0]) {
        token[length - 1] = '\0';
        memmove(token, token + 1, strlen(token));
    }
    if (token[0] == '\0') {
        snprintf(error_buffer, error_buffer_size, "INCLUDE expects a file path.");
        return false;
    }
    snprintf(out_path, out_size, "%s", token);
    return true;
}

/* Resolves an include path relative to the file that referenced it when one is
   available, otherwise against the current process working directory. */
static bool app_assembler_resolve_include_path(
    const char *including_path,
    const char *operand,
    char *out_path,
    size_t out_size,
    char *error_buffer,
    size_t error_buffer_size
) {
    char relative_path[MAX_PATH];
    char candidate[MAX_PATH];
    DWORD resolved_length;

    if (!app_assembler_extract_include_path(operand, relative_path, sizeof(relative_path), error_buffer, error_buffer_size)) {
        return false;
    }

    if (app_path_is_absolute(relative_path)) {
        snprintf(candidate, sizeof(candidate), "%s", relative_path);
    } else if (including_path != NULL && including_path[0] != '\0') {
        char parent[MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", including_path);
        if (!app_parent_dir(parent) || !app_join_path(candidate, sizeof(candidate), parent, relative_path)) {
            snprintf(error_buffer, error_buffer_size, "INCLUDE path is too long: %s", relative_path);
            return false;
        }
    } else {
        snprintf(candidate, sizeof(candidate), "%s", relative_path);
    }

    resolved_length = GetFullPathNameA(candidate, (DWORD)out_size, out_path, NULL);
    if (resolved_length == 0 || resolved_length >= out_size) {
        snprintf(error_buffer, error_buffer_size, "Could not resolve include path: %s", relative_path);
        return false;
    }
    if (!app_file_exists(out_path)) {
        snprintf(error_buffer, error_buffer_size, "Included file not found: %s", out_path);
        return false;
    }
    return true;
}

/* Reads a text file and normalizes all line endings to bare LF characters so
   assembler line handling works consistently across source files. */
static bool app_read_text_file_normalized(
    const char *path,
    char **out_text,
    char *error_buffer,
    size_t error_buffer_size
) {
    FILE *file;
    long file_size;
    char *raw_data;
    char *normalized;
    size_t normalized_length = 0;

    *out_text = NULL;
    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error_buffer, error_buffer_size, "Could not open include file: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read include file: %s", path);
        return false;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read include file: %s", path);
        return false;
    }
    rewind(file);

    raw_data = (char *)malloc((size_t)file_size + 1);
    if (raw_data == NULL) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Out of memory.");
        return false;
    }
    if (file_size > 0 && fread(raw_data, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        free(raw_data);
        snprintf(error_buffer, error_buffer_size, "Could not read include file: %s", path);
        return false;
    }
    fclose(file);
    raw_data[file_size] = '\0';

    normalized = (char *)malloc((size_t)file_size + 1);
    if (normalized == NULL) {
        free(raw_data);
        snprintf(error_buffer, error_buffer_size, "Out of memory.");
        return false;
    }

    for (long i = 0; i < file_size; ++i) {
        unsigned char ch = (unsigned char)raw_data[i];
        if (ch == '\r') {
            if (i + 1 < file_size && raw_data[i + 1] == '\n') {
                i++;
            }
            normalized[normalized_length++] = '\n';
        } else {
            normalized[normalized_length++] = (char)ch;
        }
    }
    normalized[normalized_length] = '\0';

    if (normalized_length >= 3 &&
        (unsigned char)normalized[0] == 0xEF &&
        (unsigned char)normalized[1] == 0xBB &&
        (unsigned char)normalized[2] == 0xBF) {
        memmove(normalized, normalized + 3, normalized_length - 2);
    }

    free(raw_data);
    *out_text = normalized;
    return true;
}

/* Extends the prepared-source buffers and records one logical source line plus
   its originating file and line number. */
static bool app_assembler_append_prepared_line(
    AssemblerPreparedSource *prepared,
    const char *line,
    const char *source_path,
    size_t source_line,
    char *error_buffer,
    size_t error_buffer_size
) {
    size_t line_length = strlen(line);

    if (prepared->text_length + line_length + 2 > prepared->text_capacity) {
        size_t new_capacity = prepared->text_capacity == 0 ? 1024 : prepared->text_capacity * 2;
        while (new_capacity < prepared->text_length + line_length + 2) {
            new_capacity *= 2;
        }
        char *new_text = (char *)realloc(prepared->text, new_capacity);
        if (new_text == NULL) {
            snprintf(error_buffer, error_buffer_size, "Out of memory.");
            return false;
        }
        prepared->text = new_text;
        prepared->text_capacity = new_capacity;
    }
    if (prepared->line_count + 1 > prepared->line_capacity) {
        size_t new_capacity = prepared->line_capacity == 0 ? 64 : prepared->line_capacity * 2;
        AssemblerSourceLocation *new_locations =
            (AssemblerSourceLocation *)realloc(prepared->locations, new_capacity * sizeof(AssemblerSourceLocation));
        if (new_locations == NULL) {
            snprintf(error_buffer, error_buffer_size, "Out of memory.");
            return false;
        }
        prepared->locations = new_locations;
        prepared->line_capacity = new_capacity;
    }

    memcpy(prepared->text + prepared->text_length, line, line_length);
    prepared->text_length += line_length;
    prepared->text[prepared->text_length++] = '\n';
    prepared->text[prepared->text_length] = '\0';

    prepared->locations[prepared->line_count].path[0] = '\0';
    if (source_path != NULL) {
        snprintf(prepared->locations[prepared->line_count].path, MAX_PATH, "%s", source_path);
    }
    prepared->locations[prepared->line_count].line_number = source_line;
    prepared->line_count++;
    return true;
}

/* Formats assembler diagnostics with file-and-line context when expanded
   source originated from one or more INCLUDEd files. */
static void app_assembler_format_location_error(
    const AssemblerSourceLocation *location,
    size_t fallback_line,
    const char *message,
    char *out_error,
    size_t out_error_size
) {
    size_t line_number = fallback_line;

    if (location != NULL && location->line_number != 0) {
        line_number = location->line_number;
    }
    if (location != NULL && location->path[0] != '\0') {
        snprintf(out_error, out_error_size, "%s line %zu: %s", location->path, line_number, message);
    } else {
        snprintf(out_error, out_error_size, "Line %zu: %s", line_number, message);
    }
}

/* Recursively expands INCLUDE directives into a flat source buffer while
   preserving source locations for later error reporting. */
static bool app_assembler_expand_source(
    const char *source,
    const char *source_path,
    AssemblerPreparedSource *prepared,
    char include_stack[][MAX_PATH],
    size_t include_depth,
    char *error_buffer,
    size_t error_buffer_size
) {
    char *mutable_source;
    char *cursor;
    size_t line_number = 0;

    if (include_depth >= 32) {
        snprintf(error_buffer, error_buffer_size, "INCLUDE nesting is too deep.");
        return false;
    }

    mutable_source = _strdup(source);
    if (mutable_source == NULL) {
        snprintf(error_buffer, error_buffer_size, "Out of memory.");
        return false;
    }

    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        char *line = cursor;
        char *analysis_line;

        if (line_end != NULL) {
            *line_end = '\0';
            cursor = line_end + 1;
        } else {
            cursor = NULL;
        }

        {
            size_t line_length = strlen(line);
            if (line_length > 0 && line[line_length - 1] == '\r') {
                line[line_length - 1] = '\0';
            }
        }
        line_number++;

        analysis_line = _strdup(line);
        if (analysis_line == NULL) {
            free(mutable_source);
            snprintf(error_buffer, error_buffer_size, "Out of memory.");
            return false;
        }

        app_strip_comment(analysis_line);
        {
            char mnemonic[32];
            char lhs[256];
            char rhs[256];
            char label[64];
            char *text = app_trim_inplace(analysis_line);
            size_t mnemonic_chars;
            int operand_result;
            int operand_count = 0;

            while (app_parse_leading_label(&text, label, sizeof(label))) {
                text = app_trim_inplace(text);
                if (*text == '\0') {
                    break;
                }
            }

            if (*text != '\0') {
                mnemonic_chars = app_read_upper_token(text, mnemonic, sizeof(mnemonic));
                text += mnemonic_chars;
                text = app_trim_inplace(text);
                lhs[0] = '\0';
                rhs[0] = '\0';

                if (*text != '\0') {
                    char *operand_cursor = text;
                    operand_result = app_read_operand_strict(&operand_cursor, lhs, sizeof(lhs), error_buffer, error_buffer_size);
                    if (operand_result < 0) {
                        AssemblerSourceLocation location = {{0}, line_number};
                        char final_error[512];
                        if (source_path != NULL) {
                            snprintf(location.path, sizeof(location.path), "%s", source_path);
                        }
                        app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    if (operand_result > 0) {
                        operand_count = 1;
                        operand_result = app_read_operand_strict(&operand_cursor, rhs, sizeof(rhs), error_buffer, error_buffer_size);
                        if (operand_result < 0) {
                            AssemblerSourceLocation location = {{0}, line_number};
                            char final_error[512];
                            if (source_path != NULL) {
                                snprintf(location.path, sizeof(location.path), "%s", source_path);
                            }
                            app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                            snprintf(error_buffer, error_buffer_size, "%s", final_error);
                            free(analysis_line);
                            free(mutable_source);
                            return false;
                        }
                        if (operand_result > 0) {
                            operand_count = 2;
                        }
                    }
                }

                if (app_equals_ignore_case(mnemonic, "INCLUDE")) {
                    char include_path[MAX_PATH];
                    char *include_source;
                    AssemblerSourceLocation location = {{0}, line_number};

                    if (source_path != NULL) {
                        snprintf(location.path, sizeof(location.path), "%s", source_path);
                    }
                    if (operand_count != 1) {
                        char final_error[512];
                        app_assembler_format_location_error(
                            &location,
                            line_number,
                            "INCLUDE expects 1 operand.",
                            final_error,
                            sizeof(final_error)
                        );
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    if (!app_assembler_resolve_include_path(
                            source_path,
                            lhs,
                            include_path,
                            sizeof(include_path),
                            error_buffer,
                            error_buffer_size)) {
                        char final_error[512];
                        app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    for (size_t i = 0; i < include_depth; ++i) {
                        if (_stricmp(include_stack[i], include_path) == 0) {
                            char circular_error[512];
                            snprintf(circular_error, sizeof(circular_error), "Circular INCLUDE detected: %s", include_path);
                            app_assembler_format_location_error(
                                &location,
                                line_number,
                                circular_error,
                                error_buffer,
                                error_buffer_size
                            );
                            free(analysis_line);
                            free(mutable_source);
                            return false;
                        }
                    }
                    snprintf(include_stack[include_depth], MAX_PATH, "%s", include_path);
                    if (!app_read_text_file_normalized(include_path, &include_source, error_buffer, error_buffer_size)) {
                        char final_error[512];
                        app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    if (!app_assembler_expand_source(
                            include_source,
                            include_path,
                            prepared,
                            include_stack,
                            include_depth + 1,
                            error_buffer,
                            error_buffer_size)) {
                        free(include_source);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    free(include_source);
                    free(analysis_line);
                    continue;
                }
            }
        }

        free(analysis_line);
        if (!app_assembler_append_prepared_line(
                prepared,
                line,
                source_path,
                line_number,
                error_buffer,
                error_buffer_size)) {
            free(mutable_source);
            return false;
        }
    }

    free(mutable_source);
    return true;
}

/* Builds the final source buffer used by ORG scanning and the two assembly
   passes, expanding any nested INCLUDE directives first. */
static bool app_assembler_prepare_source(
    AppState *app,
    const char *source,
    AssemblerPreparedSource *prepared,
    char *status_buffer,
    size_t status_buffer_size
) {
    char include_stack[32][MAX_PATH];
    const char *root_path = NULL;
    size_t include_depth = 0;

    memset(prepared, 0, sizeof(*prepared));
    if (app != NULL && app->debug.assembler_current_path[0] != '\0') {
        root_path = app->debug.assembler_current_path;
        snprintf(include_stack[0], MAX_PATH, "%s", root_path);
        include_depth = 1;
    }
    if (!app_assembler_expand_source(
            source,
            root_path,
            prepared,
            include_stack,
            include_depth,
            status_buffer,
            status_buffer_size)) {
        app_assembler_free_prepared_source(prepared);
        return false;
    }
    if (prepared->text == NULL) {
        prepared->text = _strdup("");
        if (prepared->text == NULL) {
            snprintf(status_buffer, status_buffer_size, "Out of memory.");
            return false;
        }
        prepared->text_capacity = 1;
    }
    return true;
}

/* Searches for a ROM file in the current working directory, next to the EXE,
   and then one directory above the EXE to support the repo's layout. */
static bool app_find_rom(char *out_path, size_t out_size, const char *filename) {
    char module_path[MAX_PATH];
    char module_dir[MAX_PATH];
    char parent_dir[MAX_PATH];

    if (app_file_exists(filename)) {
        snprintf(out_path, out_size, "%s", filename);
        return true;
    }

    if (GetModuleFileNameA(NULL, module_path, MAX_PATH) == 0 || module_path[0] == '\0') {
        return false;
    }

    snprintf(module_dir, sizeof(module_dir), "%s", module_path);
    if (!app_parent_dir(module_dir)) {
        return false;
    }

    if (app_join_path(out_path, out_size, module_dir, filename) && app_file_exists(out_path)) {
        return true;
    }

    snprintf(parent_dir, sizeof(parent_dir), "%s", module_dir);
    if (app_parent_dir(parent_dir)) {
        if (app_join_path(out_path, out_size, parent_dir, filename) && app_file_exists(out_path)) {
            return true;
        }
    }

    return false;
}

/* Returns the ROM bundle tracked for one Spectrum model inside the startup
   selection catalog and later machine-switch state. */
static RomSet *app_rom_set_for_model(RomSelection *selection, SpectrumModel model) {
    switch (model) {
        case SPECTRUM_MODEL_128K:
            return &selection->rom_128k;
        case SPECTRUM_MODEL_48K:
        default:
            return &selection->rom_48k;
    }
}

/* Const-qualified companion used by code paths that only need to inspect the
   resolved ROM catalog without mutating it. */
static const RomSet *app_rom_set_for_model_const(const RomSelection *selection, SpectrumModel model) {
    switch (model) {
        case SPECTRUM_MODEL_128K:
            return &selection->rom_128k;
        case SPECTRUM_MODEL_48K:
        default:
            return &selection->rom_48k;
    }
}

/* Treats any ROM set with a primary image path as a loadable machine target. */
static bool app_rom_set_is_available(const RomSet *set) {
    return set != NULL && set->has_rom_a;
}

/* Stores one or two ROM paths into a model-specific bundle, clearing the
   record first so stale secondary paths cannot leak between models. */
static void app_rom_set_set_paths(RomSet *set, const char *rom_a, const char *rom_b) {
    ZeroMemory(set, sizeof(*set));
    if (rom_a != NULL && rom_a[0] != '\0') {
        snprintf(set->rom_a, sizeof(set->rom_a), "%s", rom_a);
        set->has_rom_a = true;
    }
    if (rom_b != NULL && rom_b[0] != '\0') {
        snprintf(set->rom_b, sizeof(set->rom_b), "%s", rom_b);
        set->has_rom_b = true;
    }
}

/* Rebuilds the wrapped Spectrum from the ROM files assigned to the requested
   model so each machine variant boots from its own supplied images. */
static bool app_load_model_roms(
    AppState *app,
    SpectrumModel model,
    const RomSet *set,
    char *error_buffer,
    size_t error_buffer_size
) {
    Spectrum previous_spec = app->spec;
    chips_audio_callback_t audio_callback;
    int sample_rate = app->spec.audio_sample_rate > 0 ? app->spec.audio_sample_rate : APP_AUDIO_SAMPLE_RATE;
    int num_samples = app->spec.audio_num_samples > 0 ? app->spec.audio_num_samples : APP_AUDIO_CALLBACK_SAMPLES;
    float beeper_volume = app->spec.beeper_volume > 0.0f ? app->spec.beeper_volume : 0.35f;
    float ay_volume = app->spec.ay_volume > 0.0f ? app->spec.ay_volume : 0.20f;

    if (!app_rom_set_is_available(set)) {
        snprintf(error_buffer, error_buffer_size, "No ROM configured for %s mode.", app_model_name(model));
        return false;
    }

    ZeroMemory(&audio_callback, sizeof(audio_callback));
    audio_callback.func = app_audio_callback;
    audio_callback.user_data = app;

    spectrum_init(&app->spec, model);
    spectrum_configure_audio(
        &app->spec,
        audio_callback,
        sample_rate,
        num_samples,
        beeper_volume,
        ay_volume
    );
    spectrum_configure_tape_input(&app->spec, app_tape_input_callback, app);
    spectrum_configure_tape_load_trap(&app->spec, app_tape_fast_load_trap, app);
    if (!spectrum_load_roms(
            &app->spec,
            set->rom_a,
            set->has_rom_b ? set->rom_b : NULL,
            error_buffer,
            error_buffer_size)) {
        app->spec = previous_spec;
        return false;
    }

    spectrum_set_joystick_mask(&app->spec, app->controller.joystick_mask);
    return true;
}

/* Loads any available XInput runtime lazily so Xbox-compatible controllers can
   drive the Kempston port without adding a hard DLL dependency. */
static void app_controller_init(AppState *app) {
    static const char *const dll_names[] = {
        "xinput1_4.dll",
        "xinput9_1_0.dll",
        "xinput1_3.dll"
    };
    ControllerState *controller = &app->controller;

    for (size_t i = 0; i < sizeof(dll_names) / sizeof(dll_names[0]); ++i) {
        FARPROC proc;

        controller->xinput_module = LoadLibraryA(dll_names[i]);
        if (controller->xinput_module != NULL) {
            proc = GetProcAddress(controller->xinput_module, "XInputGetState");
            controller->xinput_get_state = NULL;
            if (proc != NULL) {
                memcpy(&controller->xinput_get_state, &proc, sizeof(proc));
            }
            if (controller->xinput_get_state != NULL) {
                controller->available = true;
                controller->active_user_index = 0;
                return;
            }
            FreeLibrary(controller->xinput_module);
            controller->xinput_module = NULL;
        }
    }
}

/* Releases the optional controller runtime on shutdown and clears the cached
   joystick state so later runs always start from a clean baseline. */
static void app_controller_shutdown(AppState *app) {
    ControllerState *controller = &app->controller;

    controller->joystick_mask = 0;
    controller->connected = false;
    controller->available = false;
    controller->xinput_get_state = NULL;
    if (controller->xinput_module != NULL) {
        FreeLibrary(controller->xinput_module);
        controller->xinput_module = NULL;
    }
}

/* Converts the current XInput state into a Kempston joystick bitmask and
   pushes it into the emulated machine each frame. */
static void app_controller_poll(AppState *app) {
    ControllerState *controller = &app->controller;
    XINPUT_STATE state;
    DWORD result;
    uint8_t mask = 0;
    SHORT thumb_x;
    SHORT thumb_y;

    if (!controller->available || controller->xinput_get_state == NULL) {
        return;
    }

    ZeroMemory(&state, sizeof(state));
    result = controller->xinput_get_state(controller->active_user_index, &state);
    if (result != ERROR_SUCCESS) {
        controller->connected = false;
        for (DWORD user_index = 0; user_index < XUSER_MAX_COUNT; ++user_index) {
            ZeroMemory(&state, sizeof(state));
            result = controller->xinput_get_state(user_index, &state);
            if (result == ERROR_SUCCESS) {
                controller->active_user_index = user_index;
                controller->connected = true;
                break;
            }
        }
        if (!controller->connected) {
            if (controller->joystick_mask != 0) {
                controller->joystick_mask = 0;
                spectrum_set_joystick_mask(&app->spec, 0);
            }
            return;
        }
    } else {
        controller->connected = true;
    }

    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0) {
        mask |= ZX_JOYSTICK_LEFT;
    }
    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0) {
        mask |= ZX_JOYSTICK_RIGHT;
    }
    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0) {
        mask |= ZX_JOYSTICK_DOWN;
    }
    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0) {
        mask |= ZX_JOYSTICK_UP;
    }

    thumb_x = state.Gamepad.sThumbLX;
    thumb_y = state.Gamepad.sThumbLY;
    if (thumb_x <= -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        mask |= ZX_JOYSTICK_LEFT;
    } else if (thumb_x >= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        mask |= ZX_JOYSTICK_RIGHT;
    }
    if (thumb_y <= -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        mask |= ZX_JOYSTICK_DOWN;
    } else if (thumb_y >= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        mask |= ZX_JOYSTICK_UP;
    }

    if ((state.Gamepad.wButtons & (
            XINPUT_GAMEPAD_A |
            XINPUT_GAMEPAD_B |
            XINPUT_GAMEPAD_X |
            XINPUT_GAMEPAD_Y |
            XINPUT_GAMEPAD_LEFT_SHOULDER |
            XINPUT_GAMEPAD_RIGHT_SHOULDER)) != 0 ||
        state.Gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
        state.Gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        mask |= ZX_JOYSTICK_BTN;
    }

    if (mask != controller->joystick_mask) {
        controller->joystick_mask = mask;
        spectrum_set_joystick_mask(&app->spec, mask);
    }
}

/* Reads an entire file into a heap buffer so background loader workers can
   validate and apply media without blocking the window thread on file I/O. */
static bool app_read_file_all(
    const char *path,
    uint8_t **buffer,
    size_t *size,
    char *error_buffer,
    size_t error_buffer_size,
    const char *kind
) {
    FILE *file = fopen(path, "rb");
    uint8_t *data = NULL;
    long file_size;

    if (file == NULL) {
        snprintf(error_buffer, error_buffer_size, "Could not read %s file: %s", kind, path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not size %s file: %s", kind, path);
        return false;
    }
    file_size = ftell(file);
    if (file_size <= 0) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "%s file is empty: %s", kind, path);
        return false;
    }
    rewind(file);

    data = (uint8_t *)malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Out of memory while reading %s file: %s", kind, path);
        return false;
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read %s file: %s", kind, path);
        return false;
    }
    fclose(file);

    *buffer = data;
    *size = (size_t)file_size;
    return true;
}

/* Combines command line parsing with ROM autodetection so startup always ends
   with a complete machine selection and at least one resolved ROM path. */
static bool app_resolve_roms(
    int argc,
    char **argv,
    bool has_saved_model,
    SpectrumModel saved_model,
    RomSelection *selection
) {
    SpectrumModel model;
    bool model_explicit;
    const char *rom_a = NULL;
    const char *rom_b = NULL;
    RomSet *target_set;
    char rom_path[MAX_PATH];

    if (!app_parse_args(argc, argv, &model, &model_explicit, &rom_a, &rom_b)) {
        return false;
    }
    if (!model_explicit && rom_a == NULL && has_saved_model) {
        model = saved_model;
    }

    ZeroMemory(selection, sizeof(*selection));
    selection->model = model;
    if (rom_a != NULL) {
        target_set = app_rom_set_for_model(selection, model);
        app_rom_set_set_paths(target_set, rom_a, rom_b);
        selection->model = model;
        return true;
    }

    if (app_find_rom(rom_path, sizeof(rom_path), "48.rom")) {
        app_rom_set_set_paths(&selection->rom_48k, rom_path, NULL);
    } else if (app_find_rom(rom_path, sizeof(rom_path), "128.rom")) {
        app_rom_set_set_paths(&selection->rom_48k, rom_path, NULL);
    }
    if (app_find_rom(rom_path, sizeof(rom_path), "128.rom")) {
        app_rom_set_set_paths(&selection->rom_128k, rom_path, NULL);
    }

    if (model_explicit) {
        return app_rom_set_is_available(app_rom_set_for_model_const(selection, model));
    }

    if (has_saved_model && app_rom_set_is_available(app_rom_set_for_model_const(selection, saved_model))) {
        selection->model = saved_model;
        return true;
    }

    if (app_rom_set_is_available(&selection->rom_128k)) {
        selection->model = SPECTRUM_MODEL_128K;
        return true;
    }
    if (app_rom_set_is_available(&selection->rom_48k)) {
        selection->model = SPECTRUM_MODEL_48K;
        return true;
    }
    return false;
}

/* Displays a blocking Windows error dialog for startup failures that would
   otherwise be easy to miss when the EXE is launched from Explorer. */
static void app_show_error(const char *message) {
    MessageBoxA(NULL, message, "ZX Spectrum Emulator", MB_OK | MB_ICONERROR);
}

/* Initializes the frontend, resolves ROM files, creates the window, and then
   drives the emulator in a simple 50 Hz event-and-render loop. */
int main(int argc, char **argv) {
    RomSelection selection;
    SpectrumModel saved_model = SPECTRUM_MODEL_48K;
    bool saved_tape_autoload = true;
    bool has_saved_model;
    bool has_saved_tape_autoload;
    INITCOMMONCONTROLSEX common_controls;
    ZeroMemory(&selection, sizeof(selection));
    has_saved_model = app_load_saved_model(&saved_model);
    has_saved_tape_autoload = app_load_saved_tape_autoload(&saved_tape_autoload);
    if (!app_resolve_roms(argc, argv, has_saved_model, saved_model, &selection)) {
        app_print_usage();
        app_show_error("No compatible ROM found. Place 48.rom and/or 128.rom next to the EXE or in the repo.");
        return 1;
    }

    AppState app;
    ZeroMemory(&app, sizeof(app));
    app.roms = selection;
    app.tape_autoload_enabled = has_saved_tape_autoload ? saved_tape_autoload : true;
    tape_init(&app.tape);
    app_audio_init(&app);
    app_controller_init(&app);

    char error_buffer[256];
    if (!app_load_model_roms(
            &app,
            selection.model,
            app_rom_set_for_model_const(&app.roms, selection.model),
            error_buffer,
            sizeof(error_buffer))) {
        fprintf(stderr, "%s\n", error_buffer);
        app_show_error(error_buffer);
        app_controller_shutdown(&app);
        app_audio_shutdown(&app);
        tape_discard(&app.tape);
        return 1;
    }
    app_sync_tape_stop_mode(&app);

    app.bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    app.bitmap_info.bmiHeader.biWidth = ZX_SCREEN_WIDTH;
    app.bitmap_info.bmiHeader.biHeight = -ZX_SCREEN_HEIGHT;
    app.bitmap_info.bmiHeader.biPlanes = 1;
    app.bitmap_info.bmiHeader.biBitCount = 32;
    app.bitmap_info.bmiHeader.biCompression = BI_RGB;

    ZeroMemory(&common_controls, sizeof(common_controls));
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&common_controls);

    QueryPerformanceFrequency(&app.perf_freq);
    QueryPerformanceCounter(&app.last_tick);

    HINSTANCE instance = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = app_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = "ZXSpecEmuWindow";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (RegisterClassA(&wc) == 0) {
        fprintf(stderr, "RegisterClass failed.\n");
        app_show_error("RegisterClass failed.");
        app_controller_shutdown(&app);
        app_audio_shutdown(&app);
        tape_discard(&app.tape);
        return 1;
    }
    if (!app_register_tool_window_classes(instance)) {
        fprintf(stderr, "Tool window registration failed.\n");
        app_show_error("Tool window registration failed.");
        app_controller_shutdown(&app);
        app_audio_shutdown(&app);
        tape_discard(&app.tape);
        return 1;
    }

    HMENU menu = app_create_menu();
    if (menu == NULL) {
        fprintf(stderr, "CreateMenu failed.\n");
        app_show_error("CreateMenu failed.");
        app_controller_shutdown(&app);
        app_audio_shutdown(&app);
        tape_discard(&app.tape);
        return 1;
    }

    RECT desired = {0, 0, ZX_SCREEN_WIDTH * 2, ZX_SCREEN_HEIGHT * 2};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, TRUE);
    HWND hwnd = CreateWindowExA(
        0,
        wc.lpszClassName,
        "ZX Spectrum Emulator",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        desired.right - desired.left,
        desired.bottom - desired.top,
        NULL,
        menu,
        instance,
        &app
    );
    if (hwnd == NULL) {
        fprintf(stderr, "CreateWindowEx failed.\n");
        app_show_error("CreateWindowEx failed.");
        app_controller_shutdown(&app);
        app_audio_shutdown(&app);
        tape_discard(&app.tape);
        return 1;
    }

    app.main_hwnd = hwnd;
    app_update_title(hwnd, &app.spec);
    app_update_model_menu(hwnd, app.spec.model);
    app_update_tape_menu(hwnd, app.tape_autoload_enabled);
    app.running = true;
    app_debug_machine_changed(&app);

    while (app.running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (app.debug.debugger_hwnd != NULL &&
                app.debug.debugger_accel != NULL &&
                (msg.hwnd == app.debug.debugger_hwnd || IsChild(app.debug.debugger_hwnd, msg.hwnd)) &&
                TranslateAcceleratorA(app.debug.debugger_hwnd, app.debug.debugger_accel, &msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        app_tick_emulator(&app);
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (((double)(now.QuadPart - app.last_tick.QuadPart) * 1000.0 / (double)app.perf_freq.QuadPart) < 20.0) {
                Sleep(1);
            }
        }
    }

    app_controller_shutdown(&app);
    app_audio_shutdown(&app);
    tape_discard(&app.tape);
    app_debug_free_storage(&app);
    return 0;
}
