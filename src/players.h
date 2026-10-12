#pragma once

#include <windows.h>

namespace players {

BYTE* manager();
BYTE* pad_manager();
BYTE* record(int player);
DWORD entity_handle(int player);
void* entity(int player);            // game thread only (resolver may allocate)
int control_mode(int player);
bool is_human(int player);
bool present(int player);            // record+0x25; player 2's = the co-op gate
int pad(int player);                 // -1 = none
bool clear_residual_ai(int player);
void apply_2d_state(int player);
bool in_2d_section(int player);
void revive(int player);
void* cursor(void* player_entity);

struct View {
    void* controller = nullptr;
    void* camera = nullptr;
    DWORD target = 0;
};
bool view(int player, View* out);
void reset_camera(int player);
bool player_two_view(View* out);
void reset_player_two_repair();
void retarget_player_two_cursor();

}  // namespace players
