#pragma once

// em2coop.ini
struct Config {
    // 1 = player 2 joins as soon as a second controller is connected
    // 0 = player 2 presses the join button.
    int auto_join = 0;
    // XInput button mask on player 2's pad that toggles join/leave
    // 0x0010 = Start, 0x0020 = Back, 0 = disabled.
    int join_button = 0x0010;
    // 1 = two players get a vertical split screen
    // 0 = both players share player 1's screen.
    int split_screen = 1;
    // 1 = level scripts' EnableSplitScreen(false/true) turn the split off and on around transitions
    // 0 = ignores that
    int follow_script_split = 1;
};

extern Config g_config;

void config_load(const char* path);
