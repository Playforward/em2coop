#include <windows.h>

#include <cstdio>

#include "build_check.h"
#include "config.h"
#include "coop.h"
#include "engine.h"
#include "fixes.h"
#include "game_thread.h"
#include "input.h"
#include "util.h"

namespace {

HINSTANCE g_self = nullptr;
HMODULE g_real_bink = nullptr;
char g_directory[MAX_PATH] = {};

void task_install() {
    fixes::install_playlist_wrap();
    fixes::install_cursor_hud_feed();
    coop::install_script_bindings();
    coop::install_hud_split_flag();
    input::install();
    log_line("[em2coop] ready: player 2 presses %s to join or leave",
             g_config.auto_join ? "(auto-join on)" : "the join button");
}

DWORD WINAPI bootstrap(LPVOID) {
    // Bink keeps working either way: the exports forward regardless.
    if (!build_check::is_supported_game()) {
        log_line("[em2coop] this DEM2.exe is not the build em2coop was made "
                 "for; co-op disabled, the game runs unmodified");
        return 1;
    }

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\em2coop.ini", g_directory);
    config_load(path);

    for (int i = 0; i < 1200 && !engine::global(engine::kPlayerManager); ++i) {
        Sleep(100);
    }
    if (!engine::global(engine::kPlayerManager)) {
        log_line("[em2coop] PlayerManager never appeared; co-op disabled");
        return 1;
    }
    if (!game_thread::attach(g_self)) return 1;
    game_thread::post(task_install);

    for (DWORD slice = 0;; ++slice) {
        input::poll_unwatched();
        if (slice % 5 == 0) {
            input::install();   // re-arms if the game re-resolved XInput
            coop::tick();
        }
        Sleep(50);
    }
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        g_self = instance;
        GetModuleFileNameA(instance, g_directory, sizeof(g_directory));
        if (char* slash = strrchr(g_directory, '\\')) *slash = '\0';

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\em2coop.log", g_directory);
        log_open(path);
        log_line("[em2coop] ---- session start ----");

        snprintf(path, sizeof(path), "%s\\binkw32_real.dll", g_directory);
        g_real_bink = LoadLibraryA(path);
        if (!g_real_bink) {
            log_line("[em2coop] cannot load binkw32_real.dll (%lu)",
                     GetLastError());
        }
        HANDLE thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        game_thread::detach();
        log_close();
        if (g_real_bink) FreeLibrary(g_real_bink);
    }
    return TRUE;
}
