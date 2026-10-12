#include "coop.h"

#include "config.h"
#include "engine.h"
#include "fixes.h"
#include "game_thread.h"
#include "input.h"
#include "players.h"
#include "split_screen.h"
#include "util.h"

using namespace engine;

namespace coop {

namespace {

volatile LONG g_players = 1;          // +0x234 numberOfPlayers
volatile LONG g_split_active = 0;     // +0x238
volatile LONG g_split_enabled = 1;    // +0x239
volatile LONG g_toggle_disabled = 0;  // +0x23a
DWORD g_last_toggle = 0;              // +0x240, against +0x23c
// [Mickey_SplitScreen] ActivationDelay=1.0 in ConfigFiles.ini
constexpr DWORD kActivationDelayMs = 1000;

volatile LONG g_join_pad = kNoPad;    // pad that asked to join
volatile LONG g_session_pad = kNoPad; // pad player 2 plays on
volatile LONG g_left_by_choice = 0;   // auto_join must not undo a leave

constexpr DWORD kGateStableMs = 5000;
DWORD g_gate_open_since = 0;
volatile LONG g_gate_stable = 0;

DWORD g_settle_at = 0;                // post-join follow-up time, 0 = none
volatile LONG g_revive_pending = 0;
DWORD g_last_entity = 0;
DWORD g_last_rejoin = 0;


void publish_oswald_hud() {
    BYTE* bindings = global(kHudBindings);
    if (!bindings) return;
    using GetterFn = BYTE (EM2_CDECL*)(HudValue*);
    using PublishFn = void (EM2_STDCALL*)(void*, HudValue*);
    for (const HudSlot& slot : kOswaldHudSlots) {
        HudValue value = {};
        fn<GetterFn>(slot.getter)(&value);
        fn<PublishFn>(kHudPublish)(bindings + slot.slot, &value);
    }
}

volatile LONG g_hud_split = 0;

BYTE EM2_CDECL hud_split_getter(HudValue* out) {
    out->type = 3;
    out->value = split_screen::active() ? 1.0 : 0.0;
    return 1;
}

void task_publish_split_hud() {
    BYTE* bindings = global(kHudBindings);
    if (!bindings) return;
    HudValue value = {};
    hud_split_getter(&value);
    InterlockedExchange(&g_hud_split, value.value != 0.0 ? 1 : 0);
    using PublishFn = void (EM2_STDCALL*)(void*, HudValue*);
    fn<PublishFn>(kHudPublish)(bindings + kHudSplitSlot, &value);
    log_line("[coop] HUD split flag = %d", static_cast<int>(g_hud_split));
}

void activate_split_screen() {
    if (!g_split_enabled || g_split_active || g_players != 2) return;
    InterlockedExchange(&g_split_active, 1);
    split_screen::request(g_config.split_screen != 0);
    log_line("[coop] ActivateSplitScreen");
}

void deactivate_split_screen() {
    if (!g_split_active) return;
    InterlockedExchange(&g_split_active, 0);
    split_screen::request(false);
    log_line("[coop] DeactivateSplitScreen");
}

void enable_split_screen(bool enabled) {
    InterlockedExchange(&g_split_enabled, enabled ? 1 : 0);
    log_line("[coop] EnableSplitScreen(%s)", enabled ? "true" : "false");
    if (!enabled) deactivate_split_screen();
    else activate_split_screen();
}

void disable_toggle(bool disabled) {
    InterlockedExchange(&g_toggle_disabled, disabled ? 1 : 0);
    log_line("[coop] DisableToggleSecondPlayer(%s)", disabled ? "true" : "false");
}

using SetModeFn = void (EM2_THISCALL*)(void*, int, int, int);
using PadFn = void (EM2_THISCALL*)(void*, int, int);

void take_control(int pad) {
    BYTE* mgr = players::manager();
    BYTE* pads = players::pad_manager();
    if (!mgr || !pads || pad == kNoPad) return;
    if (mem::get<BYTE>(pads + kPadCoopFlag)) {
        log_line("[coop] pad manager is in control-swap mode; not joining");
        return;
    }
    fn<SetModeFn>(kSetPlayerControlMode)(mgr, 1, kModeHuman, 0);
    int current = players::pad(1);
    if (current != pad) {
        if (current != kNoPad) fn<PadFn>(kUnassignPlayer)(pads, 1, 1);
        fn<PadFn>(kAssignPadToPlayer)(pads, 1, pad);
    }
    if (!players::entity_handle(1)) {
        void* spawner = global(kSpawner);
        using SpawnFn = void (EM2_THISCALL*)(void*, int);
        if (spawner) fn<SpawnFn>(kSpawnRequest)(spawner, 0);
    }
    players::apply_2d_state(1);
    InterlockedExchange(&g_session_pad, pad);
    g_settle_at = GetTickCount() + 2000;
}

void join(int pad) {
    take_control(pad);
    if (players::pad(1) != pad) {
        log_line("[coop] join on pad %d failed (player 2 pad %d)", pad,
                 players::pad(1));
        return;
    }
    InterlockedExchange(&g_players, 2);
    InterlockedExchange(&g_left_by_choice, 0);
    InterlockedExchange(&g_revive_pending, players::in_2d_section(1) ? 1 : 0);
    publish_oswald_hud();
    activate_split_screen();
    players::reset_player_two_repair();
    players::reset_camera(1);          // "refresh player 2's camera target"
    log_line("[coop] player 2 joined on pad %d", pad);
}

void leave() {
    BYTE* mgr = players::manager();
    if (!mgr) return;
    using ReleaseFn = void (EM2_THISCALL*)(void*);
    fn<ReleaseFn>(kReleaseSecondPlayer)(mgr);
    players::apply_2d_state(1);
    deactivate_split_screen();
    fixes::set_player_two_cursor(nullptr);
    InterlockedExchange(&g_players, 1);
    InterlockedExchange(&g_session_pad, kNoPad);
    InterlockedExchange(&g_left_by_choice, 1);
    g_settle_at = 0;
    publish_oswald_hud();
    players::reset_camera(1);
    log_line("[coop] player 2 left; Oswald is AI-controlled again");
}

void task_toggle() {
    if (g_toggle_disabled) return;
    DWORD now = GetTickCount();
    if (g_last_toggle && now - g_last_toggle < kActivationDelayMs) return;
    if (!g_gate_stable) return;
    using GuardFn = BYTE (EM2_THISCALL*)(void*);
    BYTE* mgr = players::manager();
    if (!mgr || !fn<GuardFn>(kCanToggleSecondPlayer)(mgr)) return;
    if (g_players == 1) join(g_join_pad);
    else leave();
    g_last_toggle = now;
}

void task_settle() {
    if (g_players != 2 || !players::is_human(1)) return;
    players::clear_residual_ai(1);
    players::apply_2d_state(1);
    players::reset_player_two_repair();
    if (InterlockedExchange(&g_revive_pending, 0) && players::in_2d_section(1)) {
        players::revive(1);            // includes the camera reset
    } else {
        players::reset_camera(1);
    }
    players::retarget_player_two_cursor();
    publish_oswald_hud();
}

void task_rejoin() {
    if (g_players != 2) return;
    log_line("[coop] level load reset player 2 (mode %d, pad %d); restoring",
             players::control_mode(1), players::pad(1));
    take_control(g_session_pad);
    activate_split_screen();
}

void task_split_maintain() { split_screen::maintain(); }

void task_cursor() {
    if (g_players == 2) players::retarget_player_two_cursor();
}

bool script_bool(void* args) {
    alignas(8) BYTE reader[kScriptReaderSize] = {};
    *reinterpret_cast<void**>(reader + 4) = args;
    using CtorFn = void (EM2_THISCALL*)(void*);
    using ReadFn = void (EM2_THISCALL*)(void*, BYTE*);
    fn<CtorFn>(kScriptReaderCtor)(reader + 8);
    BYTE value = 0;
    fn<ReadFn>(kScriptReadBool)(reader, &value);
    fn<CtorFn>(kScriptReaderDtor)(reader);
    return value != 0;
}

using ScriptFn = int (EM2_CDECL*)(void*);
ScriptFn g_activate_original = nullptr;
ScriptFn g_deactivate_original = nullptr;

int EM2_CDECL script_enable_split_screen(void* args) {
    bool enabled = script_bool(args);   // always consume the argument
    if (g_config.follow_script_split) enable_split_screen(enabled);
    else log_line("[coop] EnableSplitScreen(%s) ignored", enabled ? "true" : "false");
    return 0;
}

int EM2_CDECL script_disable_toggle(void* args) {
    disable_toggle(script_bool(args));
    return 0;
}

int EM2_CDECL script_activate_split_screen(void* args) {
    int result = g_activate_original(args);
    activate_split_screen();
    return result;
}

int EM2_CDECL script_deactivate_split_screen(void* args) {
    int result = g_deactivate_original(args);
    deactivate_split_screen();
    return result;
}

}  // namespace

void install_script_bindings() {
    static const BYTE kPrologue[] = { 0x83, 0xec, 0x60, 0x8b, 0x44, 0x24, 0x64 };
    struct Binding {
        DWORD entry;
        void* hook;
        ScriptFn* original;
        const char* name;
    };
    const Binding bindings[] = {
        { kScriptEnableSplitScreen,
          reinterpret_cast<void*>(&script_enable_split_screen), nullptr,
          "EnableSplitScreen" },
        { kScriptDisableToggleSecondPlayer,
          reinterpret_cast<void*>(&script_disable_toggle), nullptr,
          "DisableToggleSecondPlayer" },
        { kScriptActivateSplitScreen,
          reinterpret_cast<void*>(&script_activate_split_screen),
          &g_activate_original, "ActivateSplitScreen" },
        { kScriptDeactivateSplitScreen,
          reinterpret_cast<void*>(&script_deactivate_split_screen),
          &g_deactivate_original, "DeactivateSplitScreen" },
    };
    for (const Binding& b : bindings) {
        void* trampoline = patch::hook_entry(va(b.entry), kPrologue,
                                             sizeof(kPrologue), b.hook, b.name);
        if (b.original) *b.original = reinterpret_cast<ScriptFn>(trampoline);
        if (trampoline) log_line("[coop] script binding %s restored", b.name);
    }
}

void install_hud_split_flag() {
    if (patch::hook_entry(va(kHudSplitGetter), kHudSplitGetterPrologue,
                          sizeof(kHudSplitGetterPrologue),
                          reinterpret_cast<void*>(&hud_split_getter),
                          "UseHorizontalSplitScreen")) {
        log_line("[coop] HUD UseHorizontalSplitScreen follows the split");
    }
}

void on_join_button(int pad) {
    if (g_players == 2 && pad != g_session_pad) return;   // someone else's pad
    if (g_players == 1 && pad == players::pad(0)) return; // player 1's pad
    InterlockedExchange(&g_join_pad, pad);
    game_thread::post(task_toggle);
}

void tick() {
    DWORD now = GetTickCount();
    if (!players::manager()) return;

    if (players::present(1)) {
        if (!g_gate_open_since) g_gate_open_since = now;
        InterlockedExchange(&g_gate_stable,
                            now - g_gate_open_since >= kGateStableMs ? 1 : 0);
    } else {
        g_gate_open_since = 0;
        InterlockedExchange(&g_gate_stable, 0);
    }

    if (g_config.auto_join && g_players == 1 && !g_left_by_choice &&
        g_gate_stable) {
        int pad = input::second_connected_pad(players::pad(0));
        if (pad != kNoPad) on_join_button(pad);
    }

    if (g_players == 2 && g_gate_stable) {
        if ((!players::is_human(1) || players::pad(1) != g_session_pad) &&
            every(&g_last_rejoin, 3000)) {
            game_thread::post(task_rejoin);
        }
        DWORD entity = players::entity_handle(1);
        if (entity && entity != g_last_entity) {
            g_last_entity = entity;
            g_settle_at = now + 500;
        }
        if (g_settle_at && static_cast<LONG>(now - g_settle_at) >= 0) {
            g_settle_at = 0;
            game_thread::post(task_settle);
        }
        static DWORD last_cursor = 0;
        if (every(&last_cursor, 1000)) game_thread::post(task_cursor);
    }

    if (split_screen::needs_maintenance()) game_thread::post(task_split_maintain);
    if ((split_screen::active() ? 1 : 0) != g_hud_split) {
        game_thread::post(task_publish_split_hud);
    }
}

int number_of_players() { return g_players; }

}  // namespace coop
