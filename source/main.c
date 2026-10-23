
#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
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

u32 __stacksize__ = 0x100000;

/*
 * The PCSX-ReARMed 3DS frontend declares this symbol as weak.
 * Zero means privileged access is unavailable.
 */
int __ctr_svchax = 0;

static FILE *logfile;
static volatile int quit_requested;

static void log_stage(const char *stage)
{
    if (logfile) {
        fprintf(logfile, "[PSXctr] %s\n", stage);
        fflush(logfile);
    }

    printf("%s\n", stage);
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    (void)level;

    if (!logfile)
        return;

    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fflush(logfile);
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data ==
               RETRO_PIXEL_FORMAT_RGB565;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = SYSTEM_DIR;
        return true;

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = SAVE_DIR;
        return true;

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;

    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    enum { DW = 320, DH = 240, XOFF = 40 };
    const u16 *rows[DH];
    u16 fbw, fbh;
    u16 *fb;
    int x, y;

    if (!data || !w || !h)
        return;

    fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);

    for (y = 0; y < DH; y++)
        rows[y] = (const u16 *)(
            (const u8 *)data + (size_t)(y * h / DH) * pitch
        );

    for (x = 0; x < DW; x++) {
        u16 *col = fb + (XOFF + x) * DH;
        unsigned sx = (unsigned)x * w / DW;

        for (y = 0; y < DH; y++)
            col[DH - 1 - y] = rows[y][sx];
    }
}

static void audio_cb(int16_t left, int16_t right)
{
    (void)left;
    (void)right;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    (void)data;
    return frames;
}

static void input_poll_cb(void)
{
    hidScanInput();

    if ((hidKeysHeld() & EXIT_COMBO) == EXIT_COMBO)
        quit_requested = 1;
}

static int16_t input_state_cb(unsigned port, unsigned device,
                              unsigned index, unsigned id)
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

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name);
    size_t e = strlen(ext);

    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static int find_game(char *out, size_t cap)
{
    DIR *dir = opendir(ROM_DIR);
    struct dirent *ent;

    if (!dir)
        return 0;

    while ((ent = readdir(dir)) != NULL) {
        if (has_ext(ent->d_name, ".cue") ||
            has_ext(ent->d_name, ".chd") ||
            has_ext(ent->d_name, ".pbp") ||
            has_ext(ent->d_name, ".iso")) {
            snprintf(out, cap, "%s/%s", ROM_DIR, ent->d_name);
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
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

int main(void)
{
    char path[512];
    struct retro_game_info game;
    struct retro_system_av_info av;
    u16 fbw, fbh;
    u8 *fb;

    log_stage("Startup: enabling 3DS speedup");
    osSetSpeedupEnable(true);

    log_stage("Startup: initializing graphics");
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    consoleInit(GFX_BOTTOM, NULL);

    log_stage("Startup: clearing top screen");
    fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
    memset(fb, 0, (size_t)fbw * fbh * 2);

    log_stage("Startup: creating application directories");
    mkdir(BASE_DIR, 0777);
    mkdir(SYSTEM_DIR, 0777);
    mkdir(SAVE_DIR, 0777);

    logfile = fopen(LOG_PATH, "w");

    printf("PSXctr\n\n");
    log_stage("Startup: frontend initialized; log opened");

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

    if (find_game(path, sizeof(path))) {
        memset(&game, 0, sizeof(game));
        game.path = path;

        printf("Loading:\n%s\n\n", path);
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

    if (logfile) {
        fprintf(logfile, "[PSXctr] Shutdown: closing log\n");
        fflush(logfile);
        fclose(logfile);
    }

    gfxExit();
    return 0;
}
