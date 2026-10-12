#include "players.h"

#include <cmath>
#include <cstring>

#include "engine.h"
#include "fixes.h"
#include "game_thread.h"
#include "util.h"

using namespace engine;

namespace players {

BYTE* manager() { return global(kPlayerManager); }
BYTE* pad_manager() { return global(kPadManager); }

BYTE* record(int player) {
    BYTE* mgr = manager();
    return mgr ? mgr + player * kRecordStride : nullptr;
}

DWORD entity_handle(int player) {
    BYTE* rec = record(player);
    return rec ? mem::get<DWORD>(rec + kRecEntityHandle) : 0;
}

void* entity(int player) {
    BYTE* rec = record(player);
    if (!rec || !entity_handle(player)) return nullptr;
    using ResolveFn = void* (EM2_CDECL*)(const void*);
    return fn<ResolveFn>(kResolveEntity)(rec + kRecEntityHandle);
}

int control_mode(int player) {
    BYTE* rec = record(player);
    return rec ? mem::get<int>(rec + kRecControlMode, -1) : -1;
}

bool is_human(int player) {
    int mode = control_mode(player);
    return mode == 0 || mode == 3;
}

bool present(int player) {
    BYTE* rec = record(player);
    return rec && mem::get<BYTE>(rec + kRecPresent) != 0;
}

int pad(int player) {
    BYTE* pads = pad_manager();
    return pads ? mem::get<int>(pads + kPadSlotBase + player * 4, kNoPad)
                : kNoPad;
}

namespace {

void* ai_controller(void* ent) {
    BYTE* key = static_cast<BYTE*>(mem::ptr(ent, kEntityComponentKey));
    if (!key) return nullptr;
    BYTE offset = mem::get<BYTE>(va(kControllerSlotOffsetByte));
    BYTE slot = mem::get<BYTE>(key + offset, 0xff);
    if (slot == 0xff) return nullptr;
    BYTE* components = static_cast<BYTE*>(mem::ptr(ent, kEntityComponents));
    DWORD raw = components ? mem::get<DWORD>(components + slot * 4) : 0;
    return raw > 8 ? reinterpret_cast<void*>(raw - 8) : nullptr;
}

}  // namespace

bool clear_residual_ai(int player) {
    void* ent = entity(player);
    void* controller = ent ? ai_controller(ent) : nullptr;
    if (!controller) return false;
    using ClearFn = void (EM2_THISCALL*)(void*, void*);
    fn<ClearFn>(kClearControllerBehaviors)(
        mem::at(controller, kControllerBehaviors), nullptr);
    log_line("[players] cleared residual AI on player %d (entity %p)", player + 1,
             ent);
    return true;
}

bool in_2d_section(int player) {
    BYTE* rec = record(player);
    return rec && mem::get<DWORD>(rec + kRec2DAnchor) != 0;
}

void apply_2d_state(int player) {
    BYTE* mgr = manager();
    void* apply = mgr ? mem::ptr(mem::ptr(mgr, 0), kApply2DAnchorSlot) : nullptr;
    if (!apply) return;
    using ApplyFn = void (EM2_THISCALL*)(void*, int);
    reinterpret_cast<ApplyFn>(apply)(mgr, player);
    if (in_2d_section(player)) {
        log_line("[players] player %d entity 0x%lx given the 2D section anchor",
                 player + 1, static_cast<unsigned long>(entity_handle(player)));
    }
}

void revive(int player) {
    void* ent = entity(player);
    BYTE* rec = record(player);
    if (!ent || !rec) return;
    // BroadcastRevive only reads the entity at state+4.
    struct { void* unused; void* entity; } state = { nullptr, ent };
    using BroadcastFn = void (EM2_THISCALL*)(void*, int);
    auto broadcast = fn<BroadcastFn>(kBroadcastRevive);
    broadcast(&state, 1);

    bool warped = false;
    BYTE* key = static_cast<BYTE*>(mem::ptr(ent, kEntityComponentKey));
    BYTE slot = key ? mem::get<BYTE>(key + mem::get<BYTE>(va(k2DComponentSlotByte)),
                                     0xff)
                    : 0xff;
    BYTE* components = static_cast<BYTE*>(mem::ptr(ent, kEntityComponents));
    DWORD raw = (slot != 0xff && components)
                    ? mem::get<DWORD>(components + slot * 4) : 0;
    if (raw > k2DComponentAdjust && mem::get<BYTE>(rec + kRecHasSpawn)) {
        using WarpFn = void (EM2_THISCALL*)(void*);
        fn<WarpFn>(kWarpToSpawn)(reinterpret_cast<void*>(raw - k2DComponentAdjust));
        warped = true;
    }
    reset_camera(player);
    broadcast(&state, 2);
    log_line("[players] player %d revived in place (entity 0x%lx, %s)",
             player + 1, static_cast<unsigned long>(entity_handle(player)),
             warped ? "warped to spawn" : "no warp");
}

void* cursor(void* player_entity) {
    BYTE* components = static_cast<BYTE*>(mem::ptr(player_entity,
                                                   kEntityComponents));
    if (!components) return nullptr;
    for (int i = 0; i < 64; ++i) {
        DWORD raw = 0;
        if (!mem::read(components + i * 4, &raw)) break;
        if (raw <= 8) continue;
        void* component = reinterpret_cast<void*>(raw - 8);
        if (mem::ptr(component, 0) == va(kCursorVtbl)) return component;
    }
    return nullptr;
}

bool view(int player, View* out) {
    *out = View();
    BYTE* cameras = global(kCameraManager);
    if (!cameras) return false;
    using GetControllerFn = void* (EM2_THISCALL*)(void*, int);
    void* controller = fn<GetControllerFn>(kGetCameraController)(cameras, player);
    void* camera = mem::ptr(controller, kControllerNiCamera);
    if (!camera || mem::ptr(camera, 0) != va(kNiCameraVtbl)) return false;
    float transform[12];
    if (!mem::read(mem::at(camera, kCameraRotate), transform, 36) ||
        !mem::read(mem::at(camera, kCameraTranslate), transform + 9, 12)) {
        return false;
    }
    for (float v : transform) {
        if (!std::isfinite(v)) return false;
    }
    out->controller = controller;
    out->camera = camera;
    out->target = mem::get<DWORD>(mem::at(controller, kControllerTarget));
    return true;
}

void reset_camera(int player) {
    void* camera = global(player ? kPlayerCamera1 : kPlayerCamera0);
    if (!camera) return;
    using ResetFn = void (EM2_THISCALL*)(void*);
    fn<ResetFn>(kPlayerCameraReset)(camera);
    log_line("[camera] PlayerCamera(%d)::Reset (entity handle 0x%lx)", player,
             static_cast<unsigned long>(entity_handle(player)));
}

namespace {

volatile LONG g_repair_attempts = 0;
DWORD g_last_repair = 0;

void connect_player_two_target() {
    View second;
    void* provider = global(kPlayerCamera1);
    if (!provider || !view(1, &second)) return;
    void* getter = mem::ptr(mem::ptr(provider, 0), 0x24);
    if (!getter) return;
    using GetTargetFn = void (EM2_THISCALL*)(void*, DWORD*);
    DWORD target = 0;
    reinterpret_cast<GetTargetFn>(getter)(provider, &target);
    if (!target || target == second.target) return;
    mem::put(mem::at(second.controller, kControllerTarget), target);
    using ChangedFn = void (EM2_THISCALL*)(void*);
    fn<ChangedFn>(kCameraTargetChanged)(
        mem::at(second.controller, kControllerInterface));
    log_line("[camera] player 2 controller %p connected to target %08lx "
             "(was %08lx)", second.controller,
             static_cast<unsigned long>(target),
             static_cast<unsigned long>(second.target));
}

void repair_player_two_view() {
    LONG attempt = InterlockedIncrement(&g_repair_attempts);
    if (attempt <= 2) {
        reset_camera(1);
    } else if (attempt <= 10) {
        connect_player_two_target();
    } else if (attempt == 11) {
        log_line("[camera] player 2 camera still shares player 1's; giving "
                 "up until the next join or level load");
    }
}

bool independent(const View& first, const View& second) {
    return first.controller != second.controller &&
           first.camera != second.camera && first.target && second.target &&
           first.target != second.target;
}

}  // namespace

bool player_two_view(View* out) {
    View first;
    bool ok = view(0, &first) && view(1, out) && independent(first, *out);
    if (ok) {
        if (g_repair_attempts) {
            log_line("[camera] player 2 camera independent after %ld repair "
                     "step(s)", static_cast<long>(g_repair_attempts));
            InterlockedExchange(&g_repair_attempts, 0);
        }
        return true;
    }
    if (every(&g_last_repair, 1000)) game_thread::post(repair_player_two_view);
    return false;
}

void reset_player_two_repair() { InterlockedExchange(&g_repair_attempts, 0); }

void retarget_player_two_cursor() {
    void* component = cursor(entity(1));
    fixes::set_player_two_cursor(component);
    if (!component) return;
    using InternFn = char* (EM2_CDECL*)(const char*);
    using ReleaseFn = void (EM2_CDECL*)(char**);
    static const struct { DWORD offset; const char* name; } kWanted[] = {
        { kCursorPropertyAction, "MouseCursor2" },
        { kCursorMoveAction, "MouseCursor2" },
        { kCursorPushAction, "MouseCursor2Push" },
    };
    int changed = 0;
    for (const auto& want : kWanted) {
        char** slot = reinterpret_cast<char**>(mem::at(component, want.offset));
        char current[32];
        mem::read_cstr(*slot, current, sizeof(current));
        if (strcmp(current, want.name) == 0) continue;
        char* old = *slot;
        *slot = fn<InternFn>(kIntern)(want.name);
        if (old) fn<ReleaseFn>(kReleaseString)(&old);
        ++changed;
    }
    if (changed) {
        log_line("[players] Oswald's cursor %p now uses MouseCursor2 / "
                 "MouseCursor2Push", component);
    }
}

}  // namespace players
