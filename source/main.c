
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

u32 __stacksize__ = 0x100000;

/*
 * Keep this disabled unless the matching 3DS privileged-memory
 * initialization is implemented and verified.
 *
 * Setting this to 1 by itself can cause memory-management failures.
 */
int __ctr_svchax = 0;

static FILE *logfile;
static volatile int quit_requested;

/* Audio buffers must reside in linear memory for NDSP. */
static ndspWaveBuf audio_wavebufs[AUDIO_BUFFER_COUNT];
static int16_t *audio_buffers[AUDIO_BUFFER_COUNT];
static bool audio_ready;
static bool audio_initialized;

/* --------------------------------------------------------- */
/* Logging                                                   */
/* --------------------------------------------------------- */

static void log_stage(const char *message)
{
    if (logfile) {
        fprintf(logfile, "[PSXctr] %s\n", message);
        fflush(logfile);
    }

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

/* --------------------------------------------------------- */
/* Core options                                               */
/* --------------------------------------------------------- */

/*
 * This frontend currently exposes a small, fixed set of core
 * variables. The strings must remain valid after this callback
 * returns, so they are static string literals.
 *
 * IMPORTANT:
 * Requesting the DRC option does not override the core's separate
 * __ctr_svchax safety check. With __ctr_svchax == 0, this pinned
 * core still selects the interpreter on 3DS.
 */
static bool env_cb(unsigned cmd, void *data)
{
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

        /*
         * Give the emulator more of the system CPU-time budget.
         * This may help scheduling and audio responsiveness, but
         * cannot replace the dynamic recompiler.
         */
        if (strcmp(variable->key,
                   "pcsx_rearmed_3ds_appcputime") == 0) {
            variable->value = "80";
            return true;
        }

        /*
         * Request the normal DRC option value. The pinned core
         * separately checks __ctr_svchax and overrides this when
         * privileged access is unavailable.
         */
        if (strcmp(variable->key, "pcsx_rearmed_drc") == 0) {
            variable->value = "enabled";
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


static void video_cb(const void *data,
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

    /*
     * The returned framebuffer dimensions are expressed in its
     * rotated memory orientation: 240 rows by 400 columns.
     */
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
        if (logfile) {
            fprintf(logfile,
                    "[PSXctr] Audio: ndspInit failed: 0x%08lX\n",
                    (unsigned long)result);
            fflush(logfile);
        }

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

            if (logfile) {
                fprintf(logfile,
                        "[PSXctr] Audio: buffer allocation failed\n");
                fflush(logfile);
            }

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

static size_t audio_batch_cb(const int16_t *data, size_t frames)
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

        /*
         * Do not overwrite a buffer currently being played.
         * If all buffers are busy, return only the frames queued.
         */
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

static void audio_cb(int16_t left, int16_t right)
{
    const int16_t sample[2] = { left, right };

    (void)audio_batch_cb(sample, 1);
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

    log_stage("Startup: enabling 3DS speedup");
    osSetSpeedupEnable(true);

    log_stage("Startup: initializing graphics");
    gfxInitDefault();

    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    consoleInit(GFX_BOTTOM, NULL);

    log_stage("Startup: clearing top screen");

    framebuffer = gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &fb_width, &fb_height
    );

    if (framebuffer)
        memset(framebuffer, 0,
               (size_t)fb_width * fb_height * sizeof(u16));

    log_stage("Startup: creating application directories");

    mkdir(BASE_DIR, 0777);
    mkdir(SYSTEM_DIR, 0777);
    mkdir(SAVE_DIR, 0777);

    logfile = fopen(LOG_PATH, "w");

    printf("PSXctr\n\n");
    log_stage("Startup: frontend initialized; log opened");

    log_stage("Audio: initializing NDSP");

    if (!audio_init())
        log_stage("Audio: unavailable; continuing without sound");

    log_stage("Core: registering environment callback");
    retro_set_environment(env_cb);

    log_stage("Core: registering video callback");
    retro_set_video_refresh(video_cb);

    log_stage("Core: registering audio callback");
    retro_set_audio_sample(audio_cb);

    log_stage("Core: registering audio batch callback");
    retro_set_audio_sample_batch(audio_batch_cb);

    log_stage("Core: registering input poll callback");
    retro_set_input_poll(input_poll_cb);

    log_stage("Core: registering input state callback");
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
            log_stage("Core: retro_load_game completed successfully");

            log_stage("Core: querying system AV information");
            retro_get_system_av_info(&av);

            log_stage("Core: setting joypad controller");
            retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

            printf("Running %ux%u at %.2f fps\n",
                   av.geometry.base_width,
                   av.geometry.base_height,
                   av.timing.fps);

            printf("Exit: hold L+R+START+SELECT\n");

            log_stage("Emulation: entering retro_run loop");

            while (aptMainLoop() && !quit_requested) {
                retro_run();

                gfxFlushBuffers();
                gfxSwapBuffers();
                gspWaitForVBlank();
            }

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

    if (logfile) {
        fprintf(logfile, "[PSXctr] Shutdown: closing log\n");
        fflush(logfile);
        fclose(logfile);
        logfile = NULL;
    }

    gfxExit();

    return 0;
}
