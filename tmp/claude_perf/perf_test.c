/* Temporary accuracy/speed harness. Includes the frontend. Run from bin\. */
#define main app_real_main
#include "../../src/main.c"
#undef main
#include <dbghelp.h>

/* Fixed clock for the Next RTC (see build.sh). */
__time64_t claude_fixed_time(__time64_t *out)
{
    const __time64_t value = 1790000000;
    if (out != NULL)
        *out = value;
    return value;
}

static uint64_t hash_state;
static void hash_bytes(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < size; ++i)
    {
        hash_state ^= bytes[i];
        hash_state *= 0x100000001B3ull;
    }
}

static void hash_audio(AppState *app)
{
    AudioState *audio = &app->audio;
    while (audio->ring_count > 0)
    {
        hash_bytes(&audio->ring[audio->ring_read * 2], 4);
        audio->ring_read = (audio->ring_read + 1) % APP_AUDIO_RING_SAMPLES;
        audio->ring_count--;
    }
}

static void hash_cpu(const z80_t *cpu)
{
    const uint16_t regs[] = {cpu->af, cpu->bc, cpu->de, cpu->hl, cpu->ix, cpu->iy, cpu->sp, cpu->pc,
                             cpu->af2, cpu->bc2, cpu->de2, cpu->hl2, cpu->i, cpu->r, cpu->im, cpu->iff1};
    hash_bytes(regs, sizeof regs);
}

/* ---- sampling profiler ------------------------------------------------- */
#define PROF_MAX 200000
static volatile bool prof_on;
static HANDLE prof_target;
static DWORD64 prof_samples[PROF_MAX];
static volatile LONG prof_count;

static DWORD WINAPI prof_thread(void *unused)
{
    (void)unused;
    while (prof_on)
    {
        CONTEXT ctx;
        ZeroMemory(&ctx, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(prof_target) != (DWORD)-1)
        {
            if (GetThreadContext(prof_target, &ctx) && prof_count < PROF_MAX)
                prof_samples[prof_count++] = ctx.Rip;
            ResumeThread(prof_target);
        }
        Sleep(1);
    }
    return 0;
}

typedef struct { char name[128]; int count; } ProfEntry;
static int prof_cmp(const void *a, const void *b) { return ((const ProfEntry *)b)->count - ((const ProfEntry *)a)->count; }

static void prof_report(const char *title)
{
    static ProfEntry entries[4096];
    int entry_count = 0;
    HANDLE process = GetCurrentProcess();
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    for (LONG i = 0; i < prof_count; ++i)
    {
        const char *name = "?";
        DWORD64 displacement = 0;
        ZeroMemory(buffer, sizeof buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        if (SymFromAddr(process, prof_samples[i], &displacement, symbol))
            name = symbol->Name;
        int j = 0;
        for (; j < entry_count; ++j)
            if (strcmp(entries[j].name, name) == 0) break;
        if (j == entry_count && entry_count < 4096)
        {
            snprintf(entries[entry_count].name, sizeof entries[0].name, "%s", name);
            entries[entry_count++].count = 0;
        }
        if (j < 4096) entries[j].count++;
    }
    if (getenv("CLAUDE_LINES"))
    {
        static ProfEntry lines[4096];
        int line_count = 0;
        for (LONG i = 0; i < prof_count; ++i)
        {
            DWORD64 displacement = 0;
            DWORD line_displacement = 0;
            IMAGEHLP_LINE64 line;
            char key[128];
            ZeroMemory(buffer, sizeof buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            if (!SymFromAddr(process, prof_samples[i], &displacement, symbol) || strcmp(symbol->Name, getenv("CLAUDE_LINES")) != 0)
                continue;
            ZeroMemory(&line, sizeof line);
            line.SizeOfStruct = sizeof line;
            if (SymGetLineFromAddr64(process, prof_samples[i], &line_displacement, &line))
            {
                const char *file = strrchr(line.FileName, 92);
                snprintf(key, sizeof key, "%s:%lu", file ? file + 1 : line.FileName, (unsigned long)line.LineNumber);
            }
            else
                snprintf(key, sizeof key, "+0x%llx", (unsigned long long)displacement);
            int j = 0;
            for (; j < line_count; ++j)
                if (strcmp(lines[j].name, key) == 0) break;
            if (j == line_count && line_count < 4096) { snprintf(lines[line_count].name, sizeof lines[0].name, "%s", key); lines[line_count++].count = 0; }
            if (j < 4096) lines[j].count++;
        }
        qsort(lines, (size_t)line_count, sizeof lines[0], prof_cmp);
        for (int i = 0; i < line_count && i < 40; ++i)
            printf("      %5.1f%%  %s\n", 100.0 * lines[i].count / (prof_count ? prof_count : 1), lines[i].name);
    }
    qsort(entries, (size_t)entry_count, sizeof entries[0], prof_cmp);
    printf("  profile %s (%ld samples)\n", title, (long)prof_count);
    for (int i = 0; i < entry_count && i < 25; ++i)
        printf("    %5.1f%%  %s\n", 100.0 * entries[i].count / (prof_count ? prof_count : 1), entries[i].name);
    prof_count = 0;
}

/* ---- scenarios --------------------------------------------------------- */
static AppState app;
static RomSelection roms;

static void setup_classic(SpectrumModel model)
{
    char err[256];
    AppState fresh;
    tape_discard(&app.tape);
    dsk_discard(&app.disk);
    ZeroMemory(&fresh, sizeof fresh);
    app = fresh;
    app.roms = roms;
    app.machine_kind = APP_MACHINE_CLASSIC;
    app.tape_autoload_enabled = true;
    app.fast_tape_loading_enabled = true;
    app.speed_multiplier = 1;
    app.achieved_speed = 1.0;
    app.joystick_type = ZX_JOYSTICKTYPE_KEMPSTON;
    app.audio.enabled = true;
    app.integer_scaling = true;
    app.crt_framebuffer = (uint32_t *)malloc((size_t)ZX_SCREEN_MAX_WIDTH * ZX_SCREEN_HEIGHT * 4);
    tape_init(&app.tape);
    dsk_init(&app.disk);
    microdrive_bank_init(&app.microdrives);
    if (!app_load_model_roms(&app, model, app_rom_set_for_model_const(&app.roms, model), err, sizeof err))
    {
        printf("ROM load failed: %s\n", err);
        exit(1);
    }
    (void)app_rewind_init(&app);
    app_rewind_capture(&app);
}

static void load_tape(const char *path, bool assume_128k)
{
    char err[256];
    TapePlayer loaded;
    tape_init(&loaded);
    if (!tape_load_file_with_options(&loaded, path, (uint32_t)app.spec.machine.freq_hz, err, sizeof err, NULL) ||
        !app_apply_loaded_tape(NULL, &app, &loaded, path, assume_128k, err, sizeof err))
    {
        printf("tape load failed: %s\n", err);
        exit(1);
    }
}

/* One frame exactly as app_tick_emulator runs it. */
static void classic_frame(void)
{
    const bool warp = app.fast_tape_loading_enabled && (app.tape_autoplay_pending || app.tape.playing);
    app.tape_warp_active = warp;
    app_run_autotype(&app);
    app_debug_run_frame(&app, !warp);
    if (warp)
    {
        app_rewind_capture(&app);
        (void)app_update_tape_loader_activity(&app);
    }
    else
    {
        (void)app_update_tape_loader_activity(&app);
        app_rewind_capture(&app);
    }
    hash_audio(&app);
}

static void setup_next(const char *nex_path)
{
    char err[256];
    uint8_t *rom = NULL, *divmmc = NULL, *loader = NULL;
    size_t rom_size, divmmc_size, loader_size;
    chips_audio_callback_t cb;
    const char *dir = "nextzxos\\sn-emulator-24.11\\";
    char path[MAX_PATH];

    setup_classic(SPECTRUM_MODEL_48K);
    if (!app.next.initialized && !next_machine_init(&app.next, err, sizeof err)) { puts(err); exit(1); }
    ZeroMemory(&cb, sizeof cb);
    cb.func = app_audio_callback_stereo;
    cb.user_data = &app;
    next_machine_configure_audio(&app.next, cb, APP_AUDIO_SAMPLE_RATE, APP_AUDIO_CALLBACK_SAMPLES, 0.35f, 0.14f);
    snprintf(path, sizeof path, "%senNextZX.rom", dir);
    app_read_file_all(path, &rom, &rom_size, err, sizeof err, "rom");
    snprintf(path, sizeof path, "%senNxtmmc.rom", dir);
    app_read_file_all(path, &divmmc, &divmmc_size, err, sizeof err, "divmmc");
    app_read_file_all("nextzxos\\tbblue_loader.rom", &loader, &loader_size, err, sizeof err, "loader");
    snprintf(path, sizeof path, "%scspect-next-1gb-fixed.img", dir);
    if (!next_machine_load_rom_data(&app.next, rom, rom_size, divmmc, divmmc_size, err, sizeof err) ||
        !next_machine_load_loader_rom_data(&app.next, loader, loader_size, err, sizeof err) ||
        !next_machine_open_sd_image(&app.next, path, false, err, sizeof err))
    {
        printf("next setup failed: %s\n", err);
        exit(1);
    }
    free(rom); free(divmmc); free(loader);
    next_machine_reset(&app.next);
    if (nex_path != NULL)
    {
        uint8_t *data; size_t size; NextNexLoadResult result;
        if (!app_read_file_all(nex_path, &data, &size, err, sizeof err, "nex") ||
            !next_machine_load_nex_data(&app.next, data, size, &result, err, sizeof err))
        {
            printf("nex failed: %s\n", err);
            exit(1);
        }
        free(data);
        app.next.loader_active = false;
    }
    next_machine_render_frame(&app.next);
    app.machine_kind = APP_MACHINE_NEXT;
}

static uint64_t trace[2][40000];
typedef struct { uint16_t pc, af, bc, de, hl, sp; uint64_t ticks; } Step;
static Step steps[2][400000];
static int step_count[2];
static int step_run;
static int step_frame = -1;
static int cur_frame;
static void step_hook(void *user, uint64_t pins)
{
    (void)user;
    if (cur_frame != step_frame || !z80_opdone(&app.next.cpu)) return;
    if (step_count[step_run] < 400000)
    {
        const z80_t *c = &app.next.cpu;
        Step st = {c->pc, c->af, c->bc, c->de, c->hl, c->sp, app.next.ticks};
        (void)pins;
        steps[step_run][step_count[step_run]++] = st;
    }
}
static int trace_run;
static void next_frame(void)
{
    next_machine_exec_frame(&app.next);
    app_run_next_nexload(&app);
    hash_audio(&app);
}

typedef struct
{
    const char *name;
    int kind; /* 0 classic boot, 1 classic tape, 2 classic disk, 3 next */
    SpectrumModel model;
    const char *media;
    bool assume_128k;
    int frames;
    bool crt;
} Scenario;

static const Scenario scenarios[] = {
    {"48K boot", 0, SPECTRUM_MODEL_48K, NULL, false, 600, false},
    {"128K boot", 0, SPECTRUM_MODEL_128K, NULL, false, 600, false},
    {"+2A boot", 0, SPECTRUM_MODEL_PLUS2A, NULL, false, 600, false},
    {"+3 boot + CRT", 0, SPECTRUM_MODEL_PLUS3, NULL, false, 600, true},
    {"48K Manic Miner.tap", 1, SPECTRUM_MODEL_48K, "..\\games\\Manic Miner.tap", false, 1500, false},
    {"48K Manic Miner.tzx", 1, SPECTRUM_MODEL_48K, "..\\games\\Manic Miner.tzx", false, 1500, false},
    {"128K Rainbow Islands.tzx", 1, SPECTRUM_MODEL_128K, "..\\games\\Rainbow Islands - Side 1.tzx", true, 30000, false},
    {"48K darkfus.tzx", 1, SPECTRUM_MODEL_48K, "..\\games\\darkfus.tzx", false, 30000, false},
    {"+3 Gauntlet.dsk", 2, SPECTRUM_MODEL_PLUS3, "..\\games\\Gauntlet.dsk", false, 3000, false},
    {"Next boot", 3, SPECTRUM_MODEL_48K, NULL, false, 1500, false},
    {"Next Bloaters.nex", 3, SPECTRUM_MODEL_48K, "nextzxos\\nex-corpus\\games\\Next\\AngryBloaters\\Bloaters.nex", false, 1500, false},
    {"Next hott.nex", 3, SPECTRUM_MODEL_48K, "nextzxos\\nex-corpus\\games\\Next\\Halls of The Things\\hott.nex", false, 1500, true},
    {"Next short Bloaters", 3, SPECTRUM_MODEL_48K, "nextzxos\\nex-corpus\\games\\Next\\AngryBloaters\\Bloaters.nex", false, 300, false},
    {"Next lom.nex", 3, SPECTRUM_MODEL_48K, "nextzxos\\nex-corpus\\games\\Next\\LordsOfMidnight\\lom.nex", false, 1500, false},
};

static void run_scenario(const Scenario *s, double *ms, uint64_t *hash, int *frames_out)
{
    LARGE_INTEGER f, t0, t1;
    int frames = 0;
    int idle_frames = 0;
    char err[256];
    hash_state = 0xCBF29CE484222325ull;
    if (s->kind == 3)
    {
        setup_next(s->media);
        if (getenv("CLAUDE_FRAME"))
        {
            step_frame = atoi(getenv("CLAUDE_FRAME"));
            app.next.debug.callback.func = step_hook;
        }
    }
    else
        setup_classic(s->model);
    app.crt_scanlines = s->crt;
    if (s->kind == 1)
        load_tape(s->media, s->assume_128k);
    if (s->kind == 2 && !app_load_disk_file(NULL, &app, s->media, err, sizeof err))
    {
        printf("disk failed: %s\n", err);
        exit(1);
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (frames = 0; frames < s->frames; ++frames)
    {
        cur_frame = frames;
        if (s->kind == 3)
            next_frame();
        else
        {
            classic_frame();
            /* Tape scenarios stop 1500 frames after the tape finishes. */
            if (s->kind == 1 && !app.tape.playing && !app.tape_autoplay_pending && ++idle_frames >= 1500)
                break;
        }
        if (frames < 40000)
        {
            uint64_t saved = hash_state;
            if (s->kind == 3) { hash_cpu(&app.next.cpu); hash_bytes(&app.next.ticks, 8); }
            trace[trace_run & 1][frames] = hash_state;
            hash_state = saved;
        }
        if (s->crt)
        {
            const uint32_t *pixels = app_crt_apply_effects(&app);
            if (frames % 25 == 0)
                hash_bytes(pixels, 4u * (size_t)(s->kind == 3 ? app.next.framebuffer_width : spectrum_framebuffer_width(&app.spec)) * ZX_SCREEN_HEIGHT);
        }
        if (frames % 25 == 24)
        {
            if (s->kind == 3)
                hash_bytes(app.next.framebuffer, 4u * (size_t)app.next.framebuffer_width * ZX_SCREEN_HEIGHT);
            else
            {
                spectrum_render_frame(&app.spec);
                hash_bytes(app.spec.framebuffer, 4u * (size_t)spectrum_framebuffer_width(&app.spec) * ZX_SCREEN_HEIGHT);
            }
        }
    }
    QueryPerformanceCounter(&t1);
    if (s->kind == 3)
    {
        hash_bytes(app.next.ram, NEXT_RAM_SIZE);
        hash_cpu(&app.next.cpu);
        hash_bytes(app.next.nextregs, sizeof app.next.nextregs);
    }
    else
    {
        hash_bytes(app.spec.machine.ram, sizeof app.spec.machine.ram);
        hash_cpu(&app.spec.machine.cpu);
        hash_bytes(&app.tape.playing, sizeof app.tape.playing);
    }
    *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    *hash = hash_state;
    *frames_out = frames;
}

int main(int argc, char **argv)
{
    char tempdir[MAX_PATH];
    char *fake_argv[] = {"perf", NULL};
    const char *only = NULL;
    bool profile = false;
    int repeats = 3;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--profile") == 0) profile = true;
        else if (strcmp(argv[i], "--once") == 0) repeats = 1;
        else only = argv[i];
    }
    GetTempPathA(sizeof tempdir, tempdir);
    strcat(tempdir, "claude_perf_appdata");
    CreateDirectoryA(tempdir, NULL);
    SetEnvironmentVariableA("APPDATA", tempdir);
    {
        const char *media = NULL;
        if (!app_resolve_roms(1, fake_argv, false, SPECTRUM_MODEL_48K, &roms, &media)) { puts("no roms"); return 1; }
    }
    if (profile)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        {
            char exe_dir[MAX_PATH];
            GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
            *strrchr(exe_dir, '\\') = 0;
            if (!SymInitialize(GetCurrentProcess(), exe_dir, TRUE))
                printf("SymInitialize failed %lu", GetLastError());
        }
        timeBeginPeriod(1);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i)
    {
        const Scenario *s = &scenarios[i];
        double best = 1e30;
        uint64_t first_hash = 0;
        int frames = 0;
        bool stable = true;
        HANDLE thread = NULL;
        if (only != NULL && strstr(s->name, only) == NULL) continue;
        if (profile)
        {
            prof_on = true;
            thread = CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
        }
        for (int r = 0; r < repeats; ++r)
        {
            double ms; uint64_t hash;
            trace_run = r;
            step_run = r & 1;
            step_count[step_run] = 0;
            run_scenario(s, &ms, &hash, &frames);
            if (getenv("CLAUDE_TRACE")) printf("  run %d hash %016llx\n", r, (unsigned long long)hash);
            if (r >= 1)
                for (int k = 0; k < frames && k < 40000; ++k)
                    if (trace[0][k] != trace[1][k]) { printf("  first divergence at frame %d (pc %04X)\n", k, app.next.cpu.pc); break; }
            if (r == 1 && step_frame >= 0)
            {
                int n = step_count[0] < step_count[1] ? step_count[0] : step_count[1];
                for (int k = 0; k < n; ++k)
                {
                    if (memcmp(&steps[0][k], &steps[1][k], sizeof(Step)) != 0)
                    {
                        printf("  first differing step %d of %d/%d\n", k, step_count[0], step_count[1]);
                        for (int m = (k > 12 ? k - 12 : 0); m <= k + 2 && m < n; ++m)
                            for (int q = 0; q < 2; ++q)
                                printf("   %c%d pc %04X af %04X bc %04X de %04X hl %04X sp %04X t %llu\n", q ? 'B' : 'A', m,
                                       steps[q][m].pc, steps[q][m].af, steps[q][m].bc, steps[q][m].de, steps[q][m].hl, steps[q][m].sp,
                                       (unsigned long long)steps[q][m].ticks);
                        break;
                    }
                }
            }
            if (ms < best) best = ms;
            if (r == 0) first_hash = hash; else if (hash != first_hash) stable = false;
        }
        if (profile)
        {
            prof_on = false;
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        printf("%-28s frames %6d  %8.1f ms  %7.1f us/frame  %6.1fx realtime  hash %016llx%s\n",
               s->name, frames, best, best * 1000.0 / frames, (frames * 20.0) / best,
               (unsigned long long)first_hash, stable ? "" : "  NONDETERMINISTIC");
        fflush(stdout);
        if (profile) prof_report(s->name);
    }
    return 0;
}
