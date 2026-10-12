#include "input.h"

#include "config.h"
#include "coop.h"
#include "engine.h"
#include "util.h"

using namespace engine;

namespace input {

namespace {

struct PadState {
    DWORD packet;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT left_x, left_y, right_x, right_y;
};

using GetStateFn = DWORD (WINAPI*)(DWORD, PadState*);
GetStateFn g_original = nullptr;
WORD g_previous_buttons[4] = {};
volatile LONG g_connected_mask = 0;
DWORD g_game_polled_at[4] = {};
CRITICAL_SECTION g_lock;   // the game thread and poll_unwatched share edges

DWORD read_pad(DWORD user, PadState* state) {
    DWORD result = g_original(user, state);
    bool join_pressed = false;
    EnterCriticalSection(&g_lock);
    if (result != ERROR_SUCCESS || !state) {
        InterlockedAnd(&g_connected_mask, ~(1L << user));
        g_previous_buttons[user] = 0;
    } else {
        InterlockedOr(&g_connected_mask, 1L << user);
        WORD button = static_cast<WORD>(g_config.join_button);
        WORD pressed = state->buttons & ~g_previous_buttons[user];
        g_previous_buttons[user] = state->buttons;
        join_pressed = button && (pressed & button);
    }
    LeaveCriticalSection(&g_lock);
    if (join_pressed) coop::on_join_button(static_cast<int>(user));
    return result;
}

DWORD WINAPI hooked_get_state(DWORD user, PadState* state) {
    if (!g_original) return ERROR_DEVICE_NOT_CONNECTED;
    if (user >= 4) return g_original(user, state);
    g_game_polled_at[user] = GetTickCount();
    return read_pad(user, state);
}

}  // namespace

void install() {
    static bool lock_ready = false;
    if (!lock_ready) {
        InitializeCriticalSection(&g_lock);
        lock_ready = true;
    }
    void** slot = reinterpret_cast<void**>(va(kXInputGetStatePtr));
    void* current = mem::get<void*>(slot);
    void* ours = reinterpret_cast<void*>(&hooked_get_state);
    if (!current || current == ours) return;
    g_original = reinterpret_cast<GetStateFn>(current);
    if (mem::put(slot, ours)) {
        log_line("[input] XInputGetState hooked (real %p)", current);
    }
}

void poll_unwatched() {
    if (!g_original) return;
    DWORD now = GetTickCount();
    for (DWORD user = 0; user < 4; ++user) {
        if (now - g_game_polled_at[user] < 200) continue;
        PadState state = {};
        read_pad(user, &state);
    }
}

int second_connected_pad(int exclude) {
    LONG mask = g_connected_mask;
    for (int pad = 0; pad < 4; ++pad) {
        if (pad != exclude && (mask & (1L << pad))) return pad;
    }
    return -1;
}

}  // namespace input
