#include "config.h"

#include <windows.h>

#include "util.h"

Config g_config;

void config_load(const char* path) {
    g_config.auto_join =
        GetPrivateProfileIntA("coop", "auto_join", g_config.auto_join, path);
    g_config.join_button =
        GetPrivateProfileIntA("coop", "join_button", g_config.join_button, path);
    g_config.split_screen =
        GetPrivateProfileIntA("coop", "split_screen", g_config.split_screen, path);
    g_config.follow_script_split = GetPrivateProfileIntA(
        "coop", "follow_script_split", g_config.follow_script_split, path);
    log_line("[config] auto_join=%d join_button=0x%04x split_screen=%d "
             "follow_script_split=%d (%s)",
             g_config.auto_join, g_config.join_button, g_config.split_screen,
             g_config.follow_script_split, path);
}
