#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spectrum.h"

typedef struct KeyMap {
    UINT vk;
    int code;
} KeyMap;

enum {
    ZX_KEY_CAPS_SHIFT = 1,
    ZX_KEY_SYMBOL_SHIFT = 2,
    APP_AUTOTYPE_CAPACITY = 4096,
    APP_AUDIO_SAMPLE_RATE = 44100,
    APP_AUDIO_CALLBACK_SAMPLES = 128,
    APP_AUDIO_BUFFER_SAMPLES = 1024,
    APP_AUDIO_BUFFER_COUNT = 4,
    APP_AUDIO_RING_SAMPLES = 16384,
    APP_MENU_FILE_OPEN_SNAPSHOT = 1001,
    APP_MENU_FILE_RESET = 1002,
    APP_MENU_FILE_EXIT = 1003,
    APP_MENU_MACHINE_48K = 1201,
    APP_MENU_MACHINE_128K = 1202,
    APP_MENU_INPUT_PASTE_TEXT = 1101
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

typedef struct AutoTypeState {
    char buffer[APP_AUTOTYPE_CAPACITY];
    size_t length;
    size_t position;
    int cooldown_frames;
} AutoTypeState;

typedef struct AppState {
    Spectrum spec;
    BITMAPINFO bitmap_info;
    LARGE_INTEGER perf_freq;
    LARGE_INTEGER last_tick;
    AudioState audio;
    AutoTypeState auto_type;
    int active_key_codes[256];
    bool running;
} AppState;

/* Stores the selected machine type and the resolved ROM paths after command
   line parsing and fallback autodetection have been applied. */
typedef struct RomSelection {
    SpectrumModel model;
    char rom_a[MAX_PATH];
    char rom_b[MAX_PATH];
    bool has_rom_a;
    bool has_rom_b;
} RomSelection;

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
static bool app_parent_dir(char *path);
static void app_save_model(SpectrumModel model);
static void app_update_model_menu(HWND hwnd, SpectrumModel model);
static void app_audio_callback(const float *samples, int num_samples, void *user_data);

/* Creates the application's menu bar so snapshot loading and other emulator
   actions are available from the standard Windows chrome. */
static HMENU app_create_menu(void) {
    HMENU menu_bar = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU machine_menu = CreatePopupMenu();
    HMENU input_menu = CreatePopupMenu();

    if (menu_bar == NULL || file_menu == NULL || machine_menu == NULL || input_menu == NULL) {
        if (input_menu != NULL) {
            DestroyMenu(input_menu);
        }
        if (machine_menu != NULL) {
            DestroyMenu(machine_menu);
        }
        if (file_menu != NULL) {
            DestroyMenu(file_menu);
        }
        if (menu_bar != NULL) {
            DestroyMenu(menu_bar);
        }
        return NULL;
    }

    AppendMenuA(file_menu, MF_STRING, APP_MENU_FILE_OPEN_SNAPSHOT, "Open .z80 Snapshot...");
    AppendMenuA(file_menu, MF_STRING, APP_MENU_FILE_RESET, "Reset");
    AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file_menu, MF_STRING, APP_MENU_FILE_EXIT, "Exit");
    AppendMenuA(machine_menu, MF_STRING, APP_MENU_MACHINE_48K, "48K");
    AppendMenuA(machine_menu, MF_STRING, APP_MENU_MACHINE_128K, "128K");
    AppendMenuA(input_menu, MF_STRING, APP_MENU_INPUT_PASTE_TEXT, "Paste Text\tCtrl+V");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)file_menu, "File");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)machine_menu, "Machine");
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)input_menu, "Input");
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
        spec->model == SPECTRUM_MODEL_128K ? "128K" : "48K"
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
        model == SPECTRUM_MODEL_128K ? APP_MENU_MACHINE_128K : APP_MENU_MACHINE_48K,
        MF_BYCOMMAND
    );
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

/* Converts pasted Unicode text into the limited ASCII subset currently
   supported by the emulator's Spectrum keyboard mapping. */
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

/* Clears any queued translated text so a new paste starts from a known state. */
static void app_autotype_clear(AppState *app) {
    app->auto_type.length = 0;
    app->auto_type.position = 0;
    app->auto_type.cooldown_frames = 0;
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

/* Starts a queued text-entry sequence that feeds printable characters into the
   Spectrum one key tap at a time on later emulator frames. */
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

/* Pulls Unicode text from the clipboard and hands it to the queued translated
   text path so BASIC commands can be entered without physical key combos. */
static bool app_paste_text(HWND hwnd, AppState *app) {
    HANDLE clipboard_data;
    const WCHAR *text;
    bool queued = false;

    if (!OpenClipboard(hwnd)) {
        return false;
    }

    clipboard_data = GetClipboardData(CF_UNICODETEXT);
    if (clipboard_data != NULL) {
        text = (const WCHAR *)GlobalLock(clipboard_data);
        if (text != NULL) {
            queued = app_start_autotype(app, text);
            GlobalUnlock(clipboard_data);
        }
    }

    CloseClipboard();
    return queued;
}

/* Advances the queued text-entry path once per frame, tapping a single
   Spectrum key with a short gap between characters for ROM scanning safety. */
static void app_run_autotype(AppState *app) {
    int key_code;

    if (app->auto_type.position >= app->auto_type.length) {
        return;
    }
    if (app->auto_type.cooldown_frames > 0) {
        app->auto_type.cooldown_frames--;
        return;
    }

    key_code = (unsigned char)app->auto_type.buffer[app->auto_type.position++];
    spectrum_key_down(&app->spec, key_code);
    spectrum_key_up(&app->spec, key_code);
    app->auto_type.cooldown_frames = 1;

    if (app->auto_type.position >= app->auto_type.length) {
        app->auto_type.length = 0;
        app->auto_type.position = 0;
    }
}

/* Switches the running machine between 48K and 128K using the ROM data that
   was loaded at startup, then persists the user's choice for future launches. */
static void app_switch_model(HWND hwnd, AppState *app, SpectrumModel model) {
    char error_buffer[256];

    if (app->spec.model == model) {
        app_save_model(model);
        app_update_model_menu(hwnd, model);
        app_audio_flush(app);
        return;
    }

    app_autotype_clear(app);
    app_release_all_keys(app);
    app_audio_flush(app);
    if (!spectrum_set_model(&app->spec, model, error_buffer, sizeof(error_buffer))) {
        app_show_error(error_buffer);
        app_update_model_menu(hwnd, app->spec.model);
        return;
    }

    app_save_model(model);
    app_update_title(hwnd, &app->spec);
    app_update_model_menu(hwnd, app->spec.model);
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
                if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (UINT)wparam == 'V') {
                    app_paste_text(hwnd, app);
                    return 0;
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
        case WM_COMMAND:
            if (app != NULL) {
                switch (LOWORD(wparam)) {
                    case APP_MENU_FILE_OPEN_SNAPSHOT: {
                        OPENFILENAMEA ofn;
                        char path[MAX_PATH];
                        char error_buffer[256];

                        ZeroMemory(&ofn, sizeof(ofn));
                        ZeroMemory(path, sizeof(path));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hwnd;
                        ofn.lpstrFilter =
                            "ZX Spectrum Snapshots (*.z80)\0*.z80\0"
                            "All Files (*.*)\0*.*\0";
                        ofn.lpstrFile = path;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                        ofn.lpstrDefExt = "z80";

                        if (GetOpenFileNameA(&ofn)) {
                            if (!spectrum_load_snapshot_z80(&app->spec, path, error_buffer, sizeof(error_buffer))) {
                                app_show_error(error_buffer);
                            } else {
                                app_audio_flush(app);
                                app_update_title(hwnd, &app->spec);
                                app_update_model_menu(hwnd, app->spec.model);
                                InvalidateRect(hwnd, NULL, FALSE);
                            }
                        }
                        return 0;
                    }
                    case APP_MENU_INPUT_PASTE_TEXT:
                        app_paste_text(hwnd, app);
                        return 0;
                    case APP_MENU_FILE_RESET:
                        app_autotype_clear(app);
                        app_release_all_keys(app);
                        app_audio_flush(app);
                        spectrum_reset(&app->spec);
                        InvalidateRect(hwnd, NULL, FALSE);
                        return 0;
                    case APP_MENU_MACHINE_48K:
                        app_switch_model(hwnd, app, SPECTRUM_MODEL_48K);
                        return 0;
                    case APP_MENU_MACHINE_128K:
                        app_switch_model(hwnd, app, SPECTRUM_MODEL_128K);
                        return 0;
                    case APP_MENU_FILE_EXIT:
                        DestroyWindow(hwnd);
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
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (app != NULL) {
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
        model == SPECTRUM_MODEL_128K ? "128" : "48",
        path
    );
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

    if (!app_parse_args(argc, argv, &model, &model_explicit, &rom_a, &rom_b)) {
        return false;
    }
    if (!model_explicit && rom_a == NULL && has_saved_model) {
        model = saved_model;
    }

    selection->model = model;
    selection->has_rom_a = false;
    selection->has_rom_b = false;

    if (rom_a != NULL) {
        snprintf(selection->rom_a, sizeof(selection->rom_a), "%s", rom_a);
        selection->has_rom_a = true;
    }
    if (rom_b != NULL) {
        snprintf(selection->rom_b, sizeof(selection->rom_b), "%s", rom_b);
        selection->has_rom_b = true;
    }

    if (selection->has_rom_a) {
        return true;
    }

    if (model_explicit && selection->model == SPECTRUM_MODEL_128K) {
        if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "128.rom")) {
            selection->has_rom_a = true;
            return true;
        }
        return false;
    }

    if (model_explicit && selection->model == SPECTRUM_MODEL_48K) {
        if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "128.rom")) {
            selection->has_rom_a = true;
            return true;
        }
        if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "48.rom")) {
            selection->has_rom_a = true;
            return true;
        }
        return false;
    }

    if (has_saved_model && saved_model == SPECTRUM_MODEL_128K) {
        if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "128.rom")) {
            selection->model = SPECTRUM_MODEL_128K;
            selection->has_rom_a = true;
            return true;
        }
        if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "48.rom")) {
            selection->model = SPECTRUM_MODEL_48K;
            selection->has_rom_a = true;
            return true;
        }
        return false;
    }

    if (has_saved_model && saved_model == SPECTRUM_MODEL_48K) {
        if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "128.rom")) {
            selection->model = SPECTRUM_MODEL_48K;
            selection->has_rom_a = true;
            return true;
        }
        if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "48.rom")) {
            selection->model = SPECTRUM_MODEL_48K;
            selection->has_rom_a = true;
            return true;
        }
        return false;
    }

    if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "128.rom")) {
        selection->model = SPECTRUM_MODEL_128K;
        selection->has_rom_a = true;
        return true;
    }
    if (app_find_rom(selection->rom_a, sizeof(selection->rom_a), "48.rom")) {
        selection->model = SPECTRUM_MODEL_48K;
        selection->has_rom_a = true;
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
    bool has_saved_model;
    chips_audio_callback_t audio_callback;
    ZeroMemory(&selection, sizeof(selection));
    has_saved_model = app_load_saved_model(&saved_model);
    if (!app_resolve_roms(argc, argv, has_saved_model, saved_model, &selection)) {
        app_print_usage();
        app_show_error("No ROM found. The emulator looks for 128.rom first, then 48.rom.");
        return 1;
    }

    AppState app;
    ZeroMemory(&app, sizeof(app));
    app_audio_init(&app);
    audio_callback.func = app_audio_callback;
    audio_callback.user_data = &app;
    spectrum_init(&app.spec, selection.model);
    spectrum_configure_audio(
        &app.spec,
        audio_callback,
        APP_AUDIO_SAMPLE_RATE,
        APP_AUDIO_CALLBACK_SAMPLES,
        0.35f,
        0.20f
    );

    char error_buffer[256];
    if (!spectrum_load_roms(
        &app.spec,
        selection.rom_a,
        selection.has_rom_b ? selection.rom_b : NULL,
        error_buffer,
        sizeof(error_buffer)
    )) {
        fprintf(stderr, "%s\n", error_buffer);
        app_show_error(error_buffer);
        app_audio_shutdown(&app);
        return 1;
    }
    spectrum_reset(&app.spec);

    app.bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    app.bitmap_info.bmiHeader.biWidth = ZX_SCREEN_WIDTH;
    app.bitmap_info.bmiHeader.biHeight = -ZX_SCREEN_HEIGHT;
    app.bitmap_info.bmiHeader.biPlanes = 1;
    app.bitmap_info.bmiHeader.biBitCount = 32;
    app.bitmap_info.bmiHeader.biCompression = BI_RGB;

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
        app_audio_shutdown(&app);
        return 1;
    }

    HMENU menu = app_create_menu();
    if (menu == NULL) {
        fprintf(stderr, "CreateMenu failed.\n");
        app_show_error("CreateMenu failed.");
        app_audio_shutdown(&app);
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
        app_audio_shutdown(&app);
        return 1;
    }

    app_update_title(hwnd, &app.spec);
    app_update_model_menu(hwnd, app.spec.model);
    app.running = true;

    while (app.running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed_ms =
            (double)(now.QuadPart - app.last_tick.QuadPart) * 1000.0 / (double)app.perf_freq.QuadPart;
        if (elapsed_ms >= 20.0) {
            app_run_autotype(&app);
            spectrum_run_frame(&app.spec);
            app_audio_service(&app);
            InvalidateRect(hwnd, NULL, FALSE);
            app.last_tick = now;
        } else {
            app_audio_service(&app);
            Sleep(1);
        }
    }

    app_audio_shutdown(&app);
    return 0;
}
