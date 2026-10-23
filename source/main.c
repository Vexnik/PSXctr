#include <3ds.h>
#include <stdio.h>
#include "libretro.h"

int main(int argc, char **argv)
{
    struct retro_system_info info;

    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("PSXctr\n\n");
    printf("libretro API version: %u\n", retro_api_version());

    retro_get_system_info(&info);
    printf("Core: %s\n", info.library_name);
    printf("Version: %s\n", info.library_version);
    printf("\nPress START to exit.\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
