#pragma once

// The co-op session: the PlayerManager state machine the PC build removed,
// restored from the Wii build (REL base 0x808d0000) and driven through the PC
// engine's own calls.
//
//   Wii 0x80afab28   ToggleSecondPlayer listener      -> toggle_second_player()
//   Wii 0x80afaac4   EnableSplitScreen(bool)          -> enable_split_screen()
//   Wii 0x80afab20   DisableToggleSecondPlayer(bool)  -> disable_toggle()
//   Wii 0x80afa35c   ActivateSplitScreen()            -> activate_split_screen()
//   Wii 0x80afa680   DeactivateSplitScreen()          -> deactivate_split_screen()
//
// The Wii keeps this state in PlayerManager +0x234..+0x240; on PC those
// offsets hold unrelated fields, so it lives here instead.
namespace coop {

void install_script_bindings();
void install_hud_split_flag();
void on_join_button(int pad);
void tick();
int number_of_players();

}  // namespace coop
