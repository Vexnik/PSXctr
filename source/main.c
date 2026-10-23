#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "libretro.h"

#define BASE_DIR   "sdmc:/3ds/PSXctr"
#define SYSTEM_DIR BASE_DIR "/system"
#define SAVE_DIR   BASE_DIR "/saves"
#define LOG_PATH   BASE_DIR "/log.txt"
#define ROM_DIR    "sdmc:/roms/psx"

#define EXIT_COMBO (KEY_L | KEY_R | KEY_START | KEY_SELECT)

#define AUDIO_BUFFER_COUNT  3
#define AUDIO_BUFFER_FRAMES 2048
#define AUDIO_SAMPLE_RATE   44100.0f

#define STATS_SECONDS 5
#define TICKS_TO_US(t) ((unsigned)(((unsigned long long)(t) * 1000000ULL) / SYSCLOCK_ARM11))

u32 __stacksize__ = 0x100000;

/* Leave at 0: the core's privileged-memory paths are not set up. */
int __ctr_svchax = 0;

static FILE *logfile;
static volatile int quit_requested;

static ndspWaveBuf audio_wavebufs[AUDIO_BUFFER_COUNT];
static int16_t *audio_buffers[AUDIO_BUFFER_COUNT];
static bool audio_ready;
static bool audio_initialized;

static unsigned long long stat_run, stat_video, stat_audio, stat_wait;
static unsigned stat_frames, stat_audio_dropped;

/* --------------------------------------------------------- */
/* Logging                                                   */
/* --------------------------------------------------------- */

static void log_printf(const char *fmt, ...)
{
    va_list args;

    if (!logfile)
        return;

    va_start(args, fmt);
    vfprintf(logfile, fmt, args);
    va_end(args);

    fflush(logfile);
}

static void log_stage(const char *message)
{
    log_printf("[PSXctr] %s\n", message);
    printf("[PSXctr] %s\n", message);
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list args;

    (void)level;

    if (!logfile)
        return;

    va_start(args, fmt);
    vfprintf(logfile, fmt, args);
    va_end(args);

    fflush(logfile);
}

static void report_stats(unsigned long long elapsed)
{
    unsigned frames = stat_frames;
    unsigned fps10;
    unsigned run_us, video_us, audio_us, wait_us;

    if (frames == 0 || elapsed == 0)
        return;

    fps10 = (unsigned)(((unsigned long long)frames * 10ULL *
                        SYSCLOCK_ARM11) / elapsed);
    run_us = TICKS_TO_US(stat_run / frames);
    video_us = TICKS_TO_US(stat_video / frames);
    audio_us = TICKS_TO_US(stat_audio / frames);
    wait_us = TICKS_TO_US(stat_wait / frames);

    log_printf("[PSXctr] Stats: %u.%u fps | per frame: run %u us "
               "(video %u, audio %u), vblank wait %u us | "
               "audio frames dropped %u\n",
               fps10 / 10, fps10 % 10, run_us, video_us, audio_us,
               wait_us, stat_audio_dropped);
    printf("%u.%u fps\n", fps10 / 10, fps10 % 10);

    stat_run = stat_video = stat_audio = stat_wait = 0;
    stat_frames = 0;
    stat_audio_dropped = 0;
}

/* --------------------------------------------------------- */
/* Core options                                               */
/* --------------------------------------------------------- */

static bool env_cb(unsigned cmd, void *data)
{
    static bool logged_cpu_time;
    static bool logged_drc;

    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (!data)
            return false;

        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        if (!data)
            return false;

        return *(enum retro_pixel_format *)data ==
               RETRO_PIXEL_FORMAT_RGB565;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (!data)
            return false;

        *(const char **)data = SYSTEM_DIR;
        return true;

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        if (!data)
            return false;

        *(const char **)data = SAVE_DIR;
        return true;

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        if (!data)
            return false;

        ((struct retro_log_callback *)data)->log = log_cb;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *variable =
            (struct retro_variable *)data;

        if (!variable || !variable->key)
            return false;

        variable->value = NULL;

        if (strcmp(variable->key,
                   "pcsx_rearmed_3ds_appcputime") == 0) {
            variable->value = "80";

            if (!logged_cpu_time) {
                logged_cpu_time = true;
                log_printf("[PSXctr] Option: %s = %s\n",
                           variable->key, variable->value);
            }

            return true;
        }

        if (strcmp(variable->key, "pcsx_rearmed_drc") == 0) {
            variable->value = "enabled";

            if (!logged_drc) {
                logged_drc = true;
                log_printf("[PSXctr] Option: %s = %s\n",
                           variable->key, variable->value);
            }

            return true;
        }

        return false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (!data)
            return false;

        *(bool *)data = false;
        return true;

    default:
        return false;
    }
}

/* --------------------------------------------------------- */
/* Video output                                               */
/* --------------------------------------------------------- */

static void draw_frame(const void *data,
                       unsigned width,
                       unsigned height,
                       size_t pitch)
{
    enum {
        SCREEN_COLUMNS = 400,
        SCREEN_ROWS = 240,
        GAME_COLUMNS = 320,
        GAME_ROWS = 240,
        X_OFFSET = 40
    };

    u16 fb_width;
    u16 fb_height;
    u16 *framebuffer;
    unsigned x;
    unsigned y;

    if (!data || width == 0 || height == 0 || pitch == 0)
        return;

    framebuffer = (u16 *)gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &fb_width, &fb_height
    );

    /* The framebuffer is rotated in memory: 240 rows by 400 columns. */
    if (!framebuffer ||
        fb_width < SCREEN_ROWS ||
        fb_height < SCREEN_COLUMNS)
        return;

    for (x = 0; x < GAME_COLUMNS; x++) {
        unsigned source_x = x * width / GAME_COLUMNS;
        u16 *column =
            framebuffer + (X_OFFSET + x) * SCREEN_ROWS;

        for (y = 0; y < GAME_ROWS; y++) {
            unsigned source_y = y * height / GAME_ROWS;
            const u16 *source_row = (const u16 *)(
                (const uint8_t *)data +
                (size_t)source_y * pitch
            );

            column[SCREEN_ROWS - 1 - y] = source_row[source_x];
        }
    }
}

static void video_cb(const void *data,
                     unsigned width,
                     unsigned height,
                     size_t pitch)
{
    u64 start = svcGetSystemTick();

    draw_frame(data, width, height, pitch);

    stat_video += svcGetSystemTick() - start;
}

/* --------------------------------------------------------- */
/* Audio output                                               */
/* --------------------------------------------------------- */

static bool audio_init(void)
{
    Result result;
    unsigned i;

    memset(audio_wavebufs, 0, sizeof(audio_wavebufs));
    memset(audio_buffers, 0, sizeof(audio_buffers));

    audio_ready = false;
    audio_initialized = false;

    result = ndspInit();

    if (R_FAILED(result)) {
        log_printf("[PSXctr] Audio: ndspInit failed: 0x%08lX\n",
                   (unsigned long)result);
        return false;
    }

    audio_initialized = true;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(0, AUDIO_SAMPLE_RATE);

    {
        float mix[12] = {0};

        mix[0] = 1.0f;
        mix[1] = 1.0f;

        ndspChnSetMix(0, mix);
    }

    for (i = 0; i < AUDIO_BUFFER_COUNT; i++) {
        audio_buffers[i] = (int16_t *)linearAlloc(
            AUDIO_BUFFER_FRAMES * 2 * sizeof(int16_t)
        );

        if (!audio_buffers[i]) {
            unsigned j;

            log_printf("[PSXctr] Audio: buffer allocation failed\n");

            for (j = 0; j < i; j++) {
                linearFree(audio_buffers[j]);
                audio_buffers[j] = NULL;
            }

            ndspExit();
            audio_initialized = false;
            return false;
        }
    }

    audio_ready = true;

    log_stage("Audio: NDSP initialized, stereo PCM16 at 44100 Hz");
    return true;
}

static size_t audio_queue(const int16_t *data, size_t frames)
{
    size_t consumed = 0;

    if (!audio_ready || !data)
        return frames;

    while (consumed < frames) {
        int slot = -1;
        unsigned i;
        size_t chunk;
        size_t bytes;

        for (i = 0; i < AUDIO_BUFFER_COUNT; i++) {
            if (audio_wavebufs[i].status == NDSP_WBUF_FREE ||
                audio_wavebufs[i].status == NDSP_WBUF_DONE) {
                slot = (int)i;
                break;
            }
        }

        /* All buffers busy: report only what was queued. */
        if (slot < 0)
            break;

        chunk = frames - consumed;

        if (chunk > AUDIO_BUFFER_FRAMES)
            chunk = AUDIO_BUFFER_FRAMES;

        bytes = chunk * 2 * sizeof(int16_t);

        memcpy(audio_buffers[slot],
               data + consumed * 2,
               bytes);

        DSP_FlushDataCache(audio_buffers[slot], bytes);

        audio_wavebufs[slot].data_vaddr = audio_buffers[slot];
        audio_wavebufs[slot].nsamples = (u32)chunk;
        audio_wavebufs[slot].looping = false;
        audio_wavebufs[slot].status = NDSP_WBUF_FREE;

        ndspChnWaveBufAdd(0, &audio_wavebufs[slot]);

        consumed += chunk;
    }

    return consumed;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    u64 start = svcGetSystemTick();
    size_t consumed = audio_queue(data, frames);

    stat_audio += svcGetSystemTick() - start;

    if (consumed < frames)
        stat_audio_dropped += (unsigned)(frames - consumed);

    return consumed;
}

static void audio_cb(int16_t left, int16_t right)
{
    const int16_t sample[2] = { left, right };

    (void)audio_queue(sample, 1);
}

static void audio_shutdown(void)
{
    unsigned i;

    audio_ready = false;

    if (audio_initialized) {
        ndspChnWaveBufClear(0);
        ndspChnReset(0);
    }

    for (i = 0; i < AUDIO_BUFFER_COUNT; i++) {
        if (audio_buffers[i]) {
            linearFree(audio_buffers[i]);
            audio_buffers[i] = NULL;
        }
    }

    if (audio_initialized) {
        ndspExit();
        audio_initialized = false;
    }
}

/* --------------------------------------------------------- */
/* Controller input                                           */
/* --------------------------------------------------------- */

static void input_poll_cb(void)
{
    hidScanInput();

    if ((hidKeysHeld() & EXIT_COMBO) == EXIT_COMBO)
        quit_requested = 1;
}

static int16_t input_state_cb(unsigned port,
                              unsigned device,
                              unsigned index,
                              unsigned id)
{
    u32 held;

    (void)index;

    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;

    held = hidKeysHeld();

    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:
        return !!(held & (KEY_DUP | KEY_CPAD_UP));
    case RETRO_DEVICE_ID_JOYPAD_DOWN:
        return !!(held & (KEY_DDOWN | KEY_CPAD_DOWN));
    case RETRO_DEVICE_ID_JOYPAD_LEFT:
        return !!(held & (KEY_DLEFT | KEY_CPAD_LEFT));
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:
        return !!(held & (KEY_DRIGHT | KEY_CPAD_RIGHT));
    case RETRO_DEVICE_ID_JOYPAD_A:
        return !!(held & KEY_A);
    case RETRO_DEVICE_ID_JOYPAD_B:
        return !!(held & KEY_B);
    case RETRO_DEVICE_ID_JOYPAD_X:
        return !!(held & KEY_X);
    case RETRO_DEVICE_ID_JOYPAD_Y:
        return !!(held & KEY_Y);
    case RETRO_DEVICE_ID_JOYPAD_L:
        return !!(held & KEY_L);
    case RETRO_DEVICE_ID_JOYPAD_R:
        return !!(held & KEY_R);
    case RETRO_DEVICE_ID_JOYPAD_L2:
        return !!(held & KEY_ZL);
    case RETRO_DEVICE_ID_JOYPAD_R2:
        return !!(held & KEY_ZR);
    case RETRO_DEVICE_ID_JOYPAD_START:
        return !!(held & KEY_START);
    case RETRO_DEVICE_ID_JOYPAD_SELECT:
        return !!(held & KEY_SELECT);
    default:
        return 0;
    }
}

/* --------------------------------------------------------- */
/* Game file discovery                                        */
/* --------------------------------------------------------- */

static int has_ext(const char *name, const char *extension)
{
    size_t name_length = strlen(name);
    size_t extension_length = strlen(extension);

    return name_length > extension_length &&
           strcasecmp(name + name_length - extension_length,
                      extension) == 0;
}

static int find_game(char *out, size_t capacity)
{
    DIR *directory = opendir(ROM_DIR);
    struct dirent *entry;

    if (!directory)
        return 0;

    while ((entry = readdir(directory)) != NULL) {
        if (has_ext(entry->d_name, ".cue") ||
            has_ext(entry->d_name, ".chd") ||
            has_ext(entry->d_name, ".pbp") ||
            has_ext(entry->d_name, ".iso")) {
            int written = snprintf(out, capacity, "%s/%s",
                                   ROM_DIR, entry->d_name);

            closedir(directory);

            if (written < 0 || (size_t)written >= capacity)
                return 0;

            return 1;
        }
    }

    closedir(directory);
    return 0;
}

static void wait_for_start(void)
{
    while (aptMainLoop()) {
        hidScanInput();

        if (hidKeysDown() & KEY_START)
            break;

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

/* --------------------------------------------------------- */
/* Main                                                       */
/* --------------------------------------------------------- */

int main(void)
{
    char game_path[512];

    struct retro_game_info game;
    struct retro_system_av_info av;

    u16 fb_width;
    u16 fb_height;
    u8 *framebuffer;

    mkdir(BASE_DIR, 0777);
    mkdir(SYSTEM_DIR, 0777);
    mkdir(SAVE_DIR, 0777);
    logfile = fopen(LOG_PATH, "w");

    log_stage("Startup: log opened");
    log_printf("[PSXctr] Config: __ctr_svchax=%d "
               "(0 means the core uses the interpreter, "
               "not the dynamic recompiler)\n", __ctr_svchax);

    log_stage("Startup: enabling 3DS speedup");
    osSetSpeedupEnable(true);

    log_stage("Startup: initializing graphics");
    gfxInitDefault();

    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    consoleInit(GFX_BOTTOM, NULL);

    framebuffer = gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &fb_width, &fb_height
    );

    if (framebuffer)
        memset(framebuffer, 0,
               (size_t)fb_width * fb_height * sizeof(u16));

    printf("PSXctr\n\n");

    log_stage("Audio: initializing NDSP");

    if (!audio_init())
        log_stage("Audio: unavailable; continuing without sound");

    log_stage("Core: registering callbacks");
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);

    log_stage("Core: entering retro_init");
    retro_init();
    log_stage("Core: retro_init completed");

    log_stage("ROM: scanning sdmc:/roms/psx");

    if (find_game(game_path, sizeof(game_path))) {
        memset(&game, 0, sizeof(game));
        game.path = game_path;

        printf("Loading:\n%s\n\n", game_path);
        log_stage("Core: entering retro_load_game");

        if (retro_load_game(&game)) {
            u64 report_start;

            log_stage("Core: retro_load_game completed successfully");

            retro_get_system_av_info(&av);
            retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

            log_printf("[PSXctr] AV: %ux%u, core reports %u fps x100\n",
                       av.geometry.base_width,
                       av.geometry.base_height,
                       (unsigned)(av.timing.fps * 100.0));

            printf("Exit: hold L+R+START+SELECT\n\n");

            log_stage("Emulation: entering retro_run loop");

            stat_run = stat_video = stat_audio = stat_wait = 0;
            stat_frames = 0;
            stat_audio_dropped = 0;
            report_start = svcGetSystemTick();

            while (aptMainLoop() && !quit_requested) {
                u64 t0 = svcGetSystemTick();
                u64 t1;
                u64 t2;

                retro_run();

                t1 = svcGetSystemTick();

                gfxFlushBuffers();
                gfxSwapBuffers();
                gspWaitForVBlank();

                t2 = svcGetSystemTick();

                stat_run += t1 - t0;
                stat_wait += t2 - t1;
                stat_frames++;

                if (t2 - report_start >=
                    (u64)SYSCLOCK_ARM11 * STATS_SECONDS) {
                    report_stats(t2 - report_start);
                    report_start = t2;
                }
            }

            report_stats(svcGetSystemTick() - report_start);

            log_stage(quit_requested ?
                      "Exit: button combination pressed" :
                      "Exit: system requested shutdown");

            log_stage("Core: unloading game");
            retro_unload_game();
            log_stage("Core: game unloaded");
        } else {
            log_stage("Core: retro_load_game failed");

            printf("Could not load the game.\n");
            printf("See 3ds/PSXctr/log.txt\n");
            printf("\nPress START to exit.\n");

            wait_for_start();
        }
    } else {
        log_stage("ROM: no supported game file found");

        printf("No game found in\n%s\n", ROM_DIR);
        printf("(.cue, .chd, .pbp or .iso)\n");
        printf("\nPress START to exit.\n");

        wait_for_start();
    }

    log_stage("Core: entering retro_deinit");
    retro_deinit();
    log_stage("Core: retro_deinit completed");

    log_stage("Audio: shutting down");
    audio_shutdown();

    log_stage("Shutdown: closing log");

    if (logfile) {
        fclose(logfile);
        logfile = NULL;
    }

    gfxExit();

    return 0;
}
