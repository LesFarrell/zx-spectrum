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
#include <Scintilla.h>

typedef struct KeyMap {
    UINT vk;
    int code;
} KeyMap;

typedef struct AppState AppState;
typedef struct RomSet RomSet;
typedef struct RomSelection RomSelection;
typedef struct AssemblerBinaryOutput AssemblerBinaryOutput;

/* Shared because both the debugger and assembler host Scintilla controls. */
static HMODULE app_scintilla_module;

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
    APP_MENU_TOOLS_POKE = 1303,
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
    APP_CTRL_DEBUG_REGISTERS = 2019,
    APP_CTRL_DEBUG_MEMORY = 2020,
    APP_CTRL_DEBUG_DISASSEMBLY_GROUP = 2021,
    APP_CTRL_DEBUG_REGISTERS_GROUP = 2022,
    APP_CTRL_DEBUG_MEMORY_GROUP = 2023,
    APP_CTRL_DEBUG_POINTS_GROUP = 2024,
    APP_CTRL_DEBUG_STACK = 2025,
    APP_CTRL_DEBUG_STACK_GROUP = 2026,
    APP_CTRL_ASM_SOURCE = 2102,
    APP_CTRL_ASM_APPLY = 2103,
    APP_CTRL_ASM_STATUS = 2104,
    APP_CTRL_ASM_HELP = 2105,
    APP_CTRL_ASM_LOAD = 2106,
    APP_CTRL_ASM_SAVE = 2107,
    APP_CTRL_POKE_ADDRESS = 2110,
    APP_CTRL_POKE_VALUES = 2111,
    APP_CTRL_POKE_APPLY = 2112,
    APP_CTRL_POKE_STATUS = 2113,
    APP_CTRL_POKE_ADDRESS_LABEL = 2114,
    APP_CTRL_POKE_VALUES_LABEL = 2115,
    APP_MENU_ASM_FILE_NEW = 2200,
    APP_MENU_ASM_FILE_LOAD = 2201,
    APP_MENU_ASM_FILE_RELOAD = 2202,
    APP_MENU_ASM_FILE_SAVE = 2203,
    APP_MENU_ASM_FILE_SAVE_AS = 2204,
    APP_MENU_ASM_FILE_EXPORT_TAP = 2205,
    APP_MENU_ASM_EDIT_UNDO = 2210,
    APP_MENU_ASM_EDIT_CUT = 2211,
    APP_MENU_ASM_EDIT_COPY = 2212,
    APP_MENU_ASM_EDIT_PASTE = 2213,
    APP_MENU_ASM_EDIT_DELETE = 2214,
    APP_MENU_ASM_EDIT_SELECT_ALL = 2215,
    APP_MENU_ASM_EDIT_FONT = 2216,
    APP_MENU_ASM_EDIT_FORMAT = 2217,
    APP_MENU_ASM_EDIT_UPPERCASE = 2218,
    APP_MENU_ASM_EDIT_FIND = 2219,
    APP_MENU_ASM_EDIT_REPLACE = 2220,
    APP_MENU_ASM_BUILD_ASSEMBLE = 2221,
    APP_MENU_ASM_BUILD_ASSEMBLE_RUN = 2222,
    APP_MENU_ASM_HELP_SHOW = 2231,
    APP_MENU_DEBUG_HELP_SHOW = 2241,
    APP_TIMER_MODAL_LOOP = 3001,
    APP_TIMER_ASM_FILE_WATCH = 3002,
    APP_DIALOG_DEBUGGER = 4001,
    APP_DIALOG_ASSEMBLER = 4002,
    APP_DIALOG_POKE = 4003,
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

typedef enum AppSnapshotFormat {
    APP_SNAPSHOT_FORMAT_Z80,
    APP_SNAPSHOT_FORMAT_SNA,
    APP_SNAPSHOT_FORMAT_SZX
} AppSnapshotFormat;

typedef struct AppSnapshotLoadJob {
    HWND hwnd;
    char path[MAX_PATH];
    uint8_t *data;
    size_t size;
    SpectrumModel model;
    char error[256];
    AppSnapshotFormat format;
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
    HWND debugger_panel;
    HWND debugger_text;
    HWND debugger_registers_text;
    HWND debugger_memory_text;
    HWND debugger_stack_text;
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
    HBRUSH debugger_background_brush;
    HBRUSH debugger_surface_brush;
    HBRUSH debugger_input_brush;
    HACCEL debugger_accel;
    WNDPROC debugger_address_edit_wndproc;
    WNDPROC debugger_points_list_wndproc;
    HWND assembler_hwnd;
    HWND assembler_panel;
    HWND assembler_source;
    HWND assembler_status;
    HWND poke_hwnd;
    HWND poke_panel;
    HWND poke_address_edit;
    HWND poke_values_edit;
    HWND poke_apply_button;
    HWND poke_status;
    HBRUSH poke_background_brush;
    HBRUSH poke_input_brush;
    HBRUSH assembler_source_brush;
    HFONT assembler_font;
    WNDPROC assembler_source_wndproc;
    HWND assembler_find_dialog;
    FINDREPLACEA assembler_find_replace;
    char assembler_find_text[256];
    char assembler_replace_text[256];
    bool assembler_find_dialog_is_replace;
    bool assembler_dirty;
    bool assembler_ignore_change;
    bool assembler_suppress_next_char;
    bool assembler_has_file_write_time;
    bool assembler_file_change_prompt_active;
    size_t assembler_error_line;
    FILETIME assembler_file_write_time;
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

typedef struct AssemblerConstant {
    char name[64];
    int value;
} AssemblerConstant;

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
    bool write_to_machine;
    AssemblerBinaryOutput *output;
    AssemblerLabel labels[512];
    size_t label_count;
    AssemblerConstant constants[512];
    size_t constant_count;
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

struct AssemblerBinaryOutput {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    uint16_t start_address;
    bool has_start_address;
};

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
static HMENU app_create_debugger_menu(void);
static void app_show_debugger_help(HWND hwnd);
static HMENU app_create_assembler_menu(void);
static void app_save_model(SpectrumModel model);
static void app_save_tape_autoload(bool enabled);
static void app_update_model_menu(HWND hwnd, SpectrumModel model);
static void app_audio_callback(const float *samples, int num_samples, void *user_data);
static void app_debug_refresh_window(AppState *app);
static void app_assembler_refresh_window(AppState *app);
static void app_poke_refresh_window(AppState *app);
static void app_debug_machine_changed(AppState *app);
static bool app_register_tool_window_classes(HINSTANCE instance);
static void app_open_debugger_window(AppState *app);
static void app_open_assembler_window(AppState *app);
static void app_open_poke_window(AppState *app);
static void app_show_assembler_help(HWND hwnd);
static void app_poke_set_status(AppState *app, const char *text);
static void app_poke_layout_controls(AppState *app, HWND hwnd);
static void app_poke_apply_values(HWND hwnd, AppState *app);
static void app_assembler_set_status(AppState *app, const char *text);
static void app_assembler_update_title(AppState *app);
static void app_assembler_set_current_path(AppState *app, const char *path);
static bool app_assembler_get_file_write_time(const char *path, FILETIME *out_write_time);
static void app_assembler_sync_file_tracking(AppState *app, const char *path);
static bool app_assembler_load_last_path(char *out_path, size_t out_size);
static void app_assembler_save_last_path(const char *path);
static bool app_assembler_load_saved_font(LOGFONTA *font);
static void app_assembler_save_font(const LOGFONTA *font);
static HFONT app_create_assembler_font_from_logfont(const LOGFONTA *font);
static void app_assembler_apply_font(AppState *app, const LOGFONTA *font);
static void app_assembler_choose_font(HWND hwnd, AppState *app);
static bool app_assembler_load_source_path(const char *path, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_confirm_replace_source(
    HWND hwnd,
    AppState *app,
    const char *prompt,
    const char *cancel_status,
    char *status_buffer,
    size_t status_buffer_size
);
static bool app_assembler_load_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_reload_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_write_source_to_path(AppState *app, const char *path, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_save_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_save_source_as(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_export_tap(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static bool app_assembler_new_source(HWND hwnd, AppState *app, char *status_buffer, size_t status_buffer_size);
static void app_assembler_apply_source(HWND hwnd, AppState *app, bool run_after_assembly);
static bool app_assembler_confirm_close(HWND hwnd, AppState *app);
static void app_assembler_poll_external_change(HWND hwnd, AppState *app);
static void app_assembler_free_prepared_source(AssemblerPreparedSource *prepared);
static bool app_assembler_extract_path_operand(
    const char *directive_name,
    const char *operand,
    char *out_path,
    size_t out_size,
    char *error_buffer,
    size_t error_buffer_size
);
static bool app_assembler_resolve_source_path(
    const char *including_path,
    const char *operand,
    const char *directive_name,
    char *out_path,
    size_t out_size,
    char *error_buffer,
    size_t error_buffer_size
);
static bool app_assembler_prepare_source(
    AppState *app,
    const char *source,
    AssemblerPreparedSource *prepared,
    char *status_buffer,
    size_t status_buffer_size
);
static bool app_assemble_source(
    AppState *app,
    uint16_t start_address,
    const AssemblerPreparedSource *source,
    bool write_to_machine,
    AssemblerBinaryOutput *output,
    char *status_buffer,
    size_t status_buffer_size
);
static void app_assembler_free_binary_output(AssemblerBinaryOutput *output);
static bool app_assembler_write_tap_file(
    const char *path,
    uint16_t load_address,
    const uint8_t *data,
    size_t length,
    char *error_buffer,
    size_t error_buffer_size
);
static void app_assembler_format_location_error(
    const AssemblerSourceLocation *location,
    size_t fallback_line,
    const char *message,
    char *out_error,
    size_t out_error_size
);
static bool app_assembler_find_source_org(const char *source, uint16_t *out_address);
static bool app_parse_value(const AssemblerContext *ctx, const char *text, AssemblerValue *out_value);
static bool app_assembler_handle_edit_command(AppState *app, UINT command_id);
static bool app_assembler_format_source_text(const char *source, char **formatted_output);
static void app_assembler_format_source(HWND hwnd, AppState *app);
static bool app_assembler_uppercase_source_text(const char *source, char **uppercase_output);
static void app_assembler_uppercase_source(HWND hwnd, AppState *app);
static void app_assembler_clear_error_marker(AppState *app);
static void app_assembler_focus_line(AppState *app, size_t line_number);
static void app_assembler_sync_error_from_status(AppState *app, const char *status_text);
static void app_assembler_update_line_numbers(AppState *app);
static LRESULT app_assembler_handle_find_message(AppState *app, FINDREPLACEA *find_replace);
static LRESULT CALLBACK app_debugger_address_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_debugger_points_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_poke_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static INT_PTR CALLBACK app_poke_dlgproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_assembler_source_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_debugger_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static INT_PTR CALLBACK app_debugger_dlgproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static INT_PTR CALLBACK app_assembler_dlgproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK app_assembler_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
static HICON app_get_window_icon(bool small);
static void app_apply_window_icons(HWND hwnd);
static bool app_settings_path(char *out_path, size_t out_size);

#define APP_ICON_RESOURCE_ID 1

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
    AppendMenuA(tools_menu, MF_STRING, APP_MENU_TOOLS_POKE, "&Poke...\tCtrl+Shift+P");
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

static bool app_draw_flat_button(const DRAWITEMSTRUCT *draw) {
    char text[256];
    RECT rect;
    COLORREF background_color = GetSysColor(COLOR_WINDOW);
    COLORREF border_color = GetSysColor(COLOR_BTNSHADOW);
    COLORREF text_color = GetSysColor(COLOR_BTNTEXT);
    HBRUSH background_brush;
    HBRUSH border_brush;
    HFONT font;
    HGDIOBJ old_font = NULL;

    if (draw == NULL || draw->CtlType != ODT_BUTTON || draw->hwndItem == NULL) {
        return false;
    }

    if ((draw->itemState & ODS_DISABLED) != 0) {
        background_color = GetSysColor(COLOR_BTNFACE);
        border_color = GetSysColor(COLOR_3DLIGHT);
        text_color = GetSysColor(COLOR_GRAYTEXT);
    } else if ((draw->itemState & ODS_SELECTED) != 0) {
        background_color = GetSysColor(COLOR_3DLIGHT);
        border_color = GetSysColor(COLOR_HIGHLIGHT);
    } else if ((draw->itemState & ODS_FOCUS) != 0) {
        border_color = GetSysColor(COLOR_HIGHLIGHT);
    }

    background_brush = CreateSolidBrush(background_color);
    border_brush = CreateSolidBrush(border_color);
    if (background_brush == NULL || border_brush == NULL) {
        if (background_brush != NULL) {
            DeleteObject(background_brush);
        }
        if (border_brush != NULL) {
            DeleteObject(border_brush);
        }
        return false;
    }

    rect = draw->rcItem;
    FillRect(draw->hDC, &rect, background_brush);
    FrameRect(draw->hDC, &rect, border_brush);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, text_color);
    font = (HFONT)SendMessageA(draw->hwndItem, WM_GETFONT, 0, 0);
    if (font != NULL) {
        old_font = SelectObject(draw->hDC, font);
    }
    GetWindowTextA(draw->hwndItem, text, (int)sizeof(text));
    DrawTextA(draw->hDC, text, -1, &rect, DT_CENTER | DT_END_ELLIPSIS | DT_SINGLELINE | DT_VCENTER);
    if (old_font != NULL) {
        SelectObject(draw->hDC, old_font);
    }
    DeleteObject(border_brush);
    DeleteObject(background_brush);
    return true;
}

static void app_draw_flat_edit_border(HWND control) {
    RECT rect;
    HDC hdc;
    HBRUSH border_brush;
    COLORREF border_color;

    if (control == NULL) {
        return;
    }

    if (!IsWindowEnabled(control)) {
        border_color = GetSysColor(COLOR_3DLIGHT);
    } else if (GetFocus() == control) {
        border_color = GetSysColor(COLOR_HIGHLIGHT);
    } else {
        border_color = GetSysColor(COLOR_BTNSHADOW);
    }

    hdc = GetDC(control);
    if (hdc == NULL) {
        return;
    }
    border_brush = CreateSolidBrush(border_color);
    if (border_brush != NULL) {
        GetClientRect(control, &rect);
        FrameRect(hdc, &rect, border_brush);
        DeleteObject(border_brush);
    }
    ReleaseDC(control, hdc);
}

static void app_fill_flat_edit_background(HWND control, HDC hdc) {
    RECT rect;
    HBRUSH background_brush;

    if (control == NULL || hdc == NULL) {
        return;
    }
    background_brush = GetSysColorBrush(IsWindowEnabled(control) ? COLOR_WINDOW : COLOR_BTNFACE);
    GetClientRect(control, &rect);
    FillRect(hdc, &rect, background_brush);
}

static void app_fill_flat_edit_padding(HWND control) {
    RECT client_rect;
    RECT format_rect;
    RECT padding_rect;
    HBRUSH background_brush;
    HDC hdc;

    if (control == NULL) {
        return;
    }
    GetClientRect(control, &client_rect);
    SendMessageA(control, EM_GETRECT, 0, (LPARAM)&format_rect);
    format_rect.left = max(format_rect.left, client_rect.left);
    format_rect.top = max(format_rect.top, client_rect.top);
    format_rect.right = min(format_rect.right, client_rect.right);
    format_rect.bottom = min(format_rect.bottom, client_rect.bottom);
    if (format_rect.left > format_rect.right || format_rect.top > format_rect.bottom) {
        return;
    }

    hdc = GetDC(control);
    if (hdc == NULL) {
        return;
    }
    background_brush = GetSysColorBrush(IsWindowEnabled(control) ? COLOR_WINDOW : COLOR_BTNFACE);

    SetRect(&padding_rect, client_rect.left, client_rect.top, client_rect.right, format_rect.top);
    FillRect(hdc, &padding_rect, background_brush);
    SetRect(&padding_rect, client_rect.left, format_rect.bottom, client_rect.right, client_rect.bottom);
    FillRect(hdc, &padding_rect, background_brush);
    SetRect(&padding_rect, client_rect.left, format_rect.top, format_rect.left, format_rect.bottom);
    FillRect(hdc, &padding_rect, background_brush);
    SetRect(&padding_rect, format_rect.right, format_rect.top, client_rect.right, format_rect.bottom);
    FillRect(hdc, &padding_rect, background_brush);
    ReleaseDC(control, hdc);
}

static LRESULT CALLBACK app_flat_edit_subclass_proc(
    HWND hwnd,
    UINT msg,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data
) {
    LRESULT result;
    (void)reference_data;

    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, app_flat_edit_subclass_proc, subclass_id);
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    if (msg == WM_ERASEBKGND) {
        app_fill_flat_edit_background(hwnd, (HDC)wparam);
        return 1;
    }

    if (msg == WM_PAINT) {
        HDC hdc = GetDC(hwnd);
        if (hdc != NULL) {
            app_fill_flat_edit_background(hwnd, hdc);
            ReleaseDC(hwnd, hdc);
        }
    }

    result = DefSubclassProc(hwnd, msg, wparam, lparam);
    if (msg == WM_PAINT) {
        app_fill_flat_edit_padding(hwnd);
        app_draw_flat_edit_border(hwnd);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE) {
        RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
    }
    return result;
}

static void app_make_control_flat(HWND control) {
    char class_name[64];
    LONG_PTR style;
    LONG_PTR extended_style;

    if (control == NULL) {
        return;
    }

    style = GetWindowLongPtrA(control, GWL_STYLE);
    if (GetClassNameA(control, class_name, (int)sizeof(class_name)) > 0) {
        if (_stricmp(class_name, "Button") == 0) {
            LONG_PTR button_type = style & BS_TYPEMASK;
            if (button_type == BS_PUSHBUTTON || button_type == BS_DEFPUSHBUTTON) {
                style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW | BS_FLAT;
            } else {
                style |= BS_FLAT;
            }
        } else if (_stricmp(class_name, "Edit") == 0 &&
                   SetWindowSubclass(control, app_flat_edit_subclass_proc, 1, 0)) {
            style &= ~WS_BORDER;
            SendMessageA(
                control,
                EM_SETMARGINS,
                EC_LEFTMARGIN | EC_RIGHTMARGIN,
                MAKELPARAM(4, 4)
            );
        } else if (_stricmp(class_name, STATUSCLASSNAMEA) == 0) {
            style |= CCS_NODIVIDER;
            style &= ~SBARS_SIZEGRIP;
        }
        SetWindowLongPtrA(control, GWL_STYLE, style);
    }

    extended_style = GetWindowLongPtrA(control, GWL_EXSTYLE);
    extended_style &= ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    SetWindowLongPtrA(control, GWL_EXSTYLE, extended_style);
    SetWindowPos(
        control,
        NULL,
        0,
        0,
        0,
        0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
    );
    RedrawWindow(
        control,
        NULL,
        NULL,
        RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN
    );
}

static BOOL CALLBACK app_make_child_control_flat(HWND control, LPARAM unused) {
    (void)unused;
    app_make_control_flat(control);
    return TRUE;
}

static void app_apply_flat_control_style(HWND parent) {
    EnumChildWindows(parent, app_make_child_control_flat, 0);
}

/* Keep the UI modules in separate files while preserving the existing single
   translation unit and static helper visibility. */
#include "debugger.c"
#include "assembler_ui.c"
#include "assembler_core.c"
#include "poke.c"


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
   loading larger snapshot files does not stall painting or input. */
static DWORD WINAPI app_snapshot_load_worker(void *param) {
    AppSnapshotLoadJob *job = (AppSnapshotLoadJob *)param;
    const char *extension = strrchr(job->path, '.');

    if (extension != NULL && _stricmp(extension, ".sna") == 0) {
        job->format = APP_SNAPSHOT_FORMAT_SNA;
    } else if (extension != NULL && _stricmp(extension, ".szx") == 0) {
        job->format = APP_SNAPSHOT_FORMAT_SZX;
    } else {
        job->format = APP_SNAPSHOT_FORMAT_Z80;
    }

    job->ok = app_read_file_all(
        job->path,
        &job->data,
        &job->size,
        job->error,
        sizeof(job->error),
        "snapshot"
    );
    if (job->ok) {
        bool detected;
        const char *format_name;
        if (job->format == APP_SNAPSHOT_FORMAT_SNA) {
            detected = spectrum_detect_snapshot_sna_model_data(job->data, job->size, &job->model);
            format_name = "sna";
        } else if (job->format == APP_SNAPSHOT_FORMAT_SZX) {
            detected = spectrum_detect_snapshot_szx_model_data(job->data, job->size, &job->model);
            format_name = "szx";
        } else {
            detected = spectrum_detect_snapshot_model_data(job->data, job->size, &job->model);
            format_name = "z80";
        }
        if (!detected) {
            snprintf(
                job->error,
                sizeof(job->error),
                "Unsupported or corrupt .%s snapshot: %s",
                format_name,
                job->path);
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
   machine only if the decoded payload targets a different model. */
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
    bool loaded;
    if (job->format == APP_SNAPSHOT_FORMAT_SNA) {
        loaded = spectrum_load_snapshot_sna_data(
            &app->spec, job->data, job->size, error_buffer, error_buffer_size);
    } else if (job->format == APP_SNAPSHOT_FORMAT_SZX) {
        loaded = spectrum_load_snapshot_szx_data(
            &app->spec, job->data, job->size, error_buffer, error_buffer_size);
    } else {
        loaded = spectrum_load_snapshot_z80_data(
            &app->spec, job->data, job->size, error_buffer, error_buffer_size);
    }
    if (!loaded) {
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
                        case 'P':
                            app_open_poke_window(app);
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
                            "ZX Spectrum Files (*.tap;*.tzx;*.z80;*.sna;*.szx)\0*.tap;*.tzx;*.z80;*.sna;*.szx\0"
                            "Tape Images (*.tap;*.tzx)\0*.tap;*.tzx\0"
                            "ZX Spectrum Snapshots (*.z80;*.sna;*.szx)\0*.z80;*.sna;*.szx\0"
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

                            if (extension != NULL &&
                                (_stricmp(extension, ".z80") == 0 || _stricmp(extension, ".sna") == 0 ||
                                 _stricmp(extension, ".szx") == 0)) {
                                if (!app_load_snapshot_file(hwnd, app, path, error_buffer, sizeof(error_buffer))) {
                                    app_show_error(error_buffer);
                                }
                                return 0;
                            }

                            app_show_error("Unsupported file type. Open a .tap, .tzx, .z80, .sna, or .szx file.");
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
                    case APP_MENU_TOOLS_POKE:
                        app_open_poke_window(app);
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
    app_apply_window_icons(hwnd);
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
            if (app.debug.assembler_find_dialog != NULL &&
                IsDialogMessageA(app.debug.assembler_find_dialog, &msg)) {
                continue;
            }
            if (app.debug.debugger_hwnd != NULL &&
                app.debug.debugger_accel != NULL &&
                (msg.hwnd == app.debug.debugger_hwnd || IsChild(app.debug.debugger_hwnd, msg.hwnd)) &&
                TranslateAcceleratorA(app.debug.debugger_hwnd, app.debug.debugger_accel, &msg)) {
                continue;
            }
            if (app.debug.debugger_hwnd != NULL &&
                app.debug.debugger_panel != NULL &&
                (msg.hwnd == app.debug.debugger_panel || IsChild(app.debug.debugger_panel, msg.hwnd)) &&
                msg.message == WM_KEYDOWN &&
                msg.wParam == VK_TAB &&
                IsDialogMessageA(app.debug.debugger_panel, &msg)) {
                continue;
            }
            if (app.debug.poke_hwnd != NULL &&
                app.debug.poke_panel != NULL &&
                (msg.hwnd == app.debug.poke_panel || IsChild(app.debug.poke_panel, msg.hwnd)) &&
                msg.message == WM_KEYDOWN &&
                msg.wParam == VK_TAB &&
                IsDialogMessageA(app.debug.poke_panel, &msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        app_tick_emulator(&app);
        /* Yield to Windows between timing slices, but wake immediately when
           input or window work arrives instead of polling at full speed. */
        MsgWaitForMultipleObjectsEx(0, NULL, 2, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    app_controller_shutdown(&app);
    app_audio_shutdown(&app);
    tape_discard(&app.tape);
    app_debug_free_storage(&app);
    return 0;
}
