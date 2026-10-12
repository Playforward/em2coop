#include "split_screen.h"

#include <cstring>

#include "players.h"
#include "util.h"
#include "world_views.h"

using namespace engine;

namespace split_screen {

namespace {

constexpr int kMaxPostClicks = 12;

struct PassSave {
    FrustumSave frustum;
    void* child = nullptr;          // scene view whose camera pointer moved
    void* child_camera = nullptr;
    void* view = nullptr;           // render view whose camera pointer moved
    void* view_camera = nullptr;
};

volatile LONG g_half = 0;           // 0 outside, 1 Mickey, 2 Oswald
volatile LONG g_in_scene = 0;       // inside a scene click (not its chain)
bool g_in_live_pass = false;        // inside the live scene click itself
PassSave g_pending[2];
bool g_pending_valid[2] = {};
void* g_pass_camera[2] = {};
bool g_scene_cleared[2] = {};

struct Pipeline {
    bool built = false;
    void* list = nullptr;            // SceneApp scene step's click list
    void* live_click = nullptr;
    void* source_scene_view = nullptr;
    float live_viewport[4] = {};
    void* mickey_click = nullptr;
    void* mickey_node = nullptr;
    void* oswald_click = nullptr;
    void* oswald_node = nullptr;
    float oswald_viewport[4] = {};
    DWORD oswald_clear_mode = 0;
    void* post_clicks[kMaxPostClicks] = {};
    void* post_nodes[kMaxPostClicks] = {};
    int post_count = 0;
    void* chain_end_click = nullptr;
};
Pipeline g_pipe;

patch::VtableShim g_live_shim;
patch::VtableShim g_mickey_shim;
patch::VtableShim g_oswald_shim;
patch::VtableShim g_chain_end_shim;
patch::VtableShim g_device_shim;

using ClickRenderFn = void (EM2_THISCALL*)(void*, void*, DWORD);
ClickRenderFn g_click_render = nullptr;
ClickRenderFn g_chain_end_render = nullptr;

volatile LONG g_requested = 0;
volatile LONG g_rebuild = 0;
DWORD g_requested_since = 0;

using SetViewportFn = HRESULT (EM2_STDCALL*)(void*, const D3DViewport9*);
using GetViewportFn = HRESULT (EM2_STDCALL*)(void*, D3DViewport9*);
using SetRenderTargetFn = HRESULT (EM2_STDCALL*)(void*, DWORD, void*);
using GetBackBufferFn = HRESULT (EM2_STDCALL*)(void*, UINT, UINT, int, void**);
using ClearFn = HRESULT (EM2_STDCALL*)(void*, DWORD, const void*, DWORD, DWORD,
                                       float, DWORD);
using ReleaseFn = ULONG (EM2_STDCALL*)(void*);

void* g_device = nullptr;
void* g_backbuffer = nullptr;
void* g_current_rt0 = nullptr;
void* g_scene_target = nullptr;     // the buffer the live pass renders into
void* g_scene_target_pending = nullptr;
SetViewportFn g_set_viewport = nullptr;
GetViewportFn g_get_viewport = nullptr;
SetRenderTargetFn g_set_render_target = nullptr;
ClearFn g_clear = nullptr;

void* list_head(void* list) { return mem::ptr(list, 0x04); }
DWORD list_count(void* list) { return mem::get<DWORD>(mem::at(list, 0x0c)); }
void* node_next(void* node) { return mem::ptr(node, 0x00); }
void* node_item(void* node) { return mem::ptr(node, 0x08); }

void* node_after(void* node, int steps) {
    for (int i = 0; i < steps && node; ++i) node = node_next(node);
    return node;
}

void* find_node(void* list, void* item) {
    void* node = list_head(list);
    for (DWORD i = 0; node && i < 64 && i < list_count(list); ++i) {
        if (node_item(node) == item) return node;
        node = node_next(node);
    }
    return nullptr;
}

bool node_in_list(void* list, void* wanted) {
    void* node = list_head(list);
    for (DWORD i = 0; node && i < 64 && i < list_count(list); ++i) {
        if (node == wanted) return true;
        node = node_next(node);
    }
    return false;
}

bool detach_node(void* list, void* node) {
    if (!list || !node || !node_in_list(list, node)) return false;
    void* next = node_next(node);
    void* previous = mem::ptr(node, 0x04);
    DWORD count = list_count(list);
    if (previous) mem::put(mem::at(previous, 0x00), next);
    else mem::put(mem::at(list, 0x04), next);
    if (next) mem::put(mem::at(next, 0x04), previous);
    else mem::put(mem::at(list, 0x08), previous);
    mem::put<void*>(mem::at(node, 0x00), nullptr);
    mem::put<void*>(mem::at(node, 0x04), nullptr);
    return mem::put(mem::at(list, 0x0c), count - 1);
}

bool insert_after(void* list, void* after, void* node) {
    if (!list || !after || !node) return false;
    void* next = node_next(after);
    mem::put(mem::at(node, 0x00), next);
    mem::put(mem::at(node, 0x04), after);
    mem::put(mem::at(after, 0x00), node);
    if (next) mem::put(mem::at(next, 0x04), node);
    else mem::put(mem::at(list, 0x08), node);
    return mem::put(mem::at(list, 0x0c), list_count(list) + 1);
}

void* append_and_take(void* step, void* list, void* click) {
    using AppendFn = void (EM2_THISCALL*)(void*, void*);
    fn<AppendFn>(kRenderStepAppend)(step, click);
    void* tail = mem::ptr(list, 0x08);
    if (!tail || node_item(tail) != click || !detach_node(list, tail)) {
        return nullptr;
    }
    return tail;
}

void click_name(void* click, char* out, size_t capacity) {
    DWORD offset = mem::ptr(click, 0) == va(kNiViewRenderClickVtbl)
                       ? kViewClickName : kClickName;
    mem::read_cstr(static_cast<const char*>(mem::ptr(click, offset)), out,
                   capacity);
}

void* scene_view_of(void* click) {
    return node_item(mem::ptr(mem::at(click, kClickChildList), 0x04));
}

void* scene_camera_of(void* click) {
    void* camera = mem::ptr(scene_view_of(click), kSceneViewCamera);
    return mem::ptr(camera, 0) == va(kNiCameraVtbl) ? camera : nullptr;
}

void* game_alloc(size_t size) {
    using AllocFn = void* (EM2_CDECL*)(size_t, int, int, const char*, int,
                                       const char*);
    return fn<AllocFn>(kGameAlloc)(
        size, 1, 0, reinterpret_cast<const char*>(va(kAllocSourceFile)), 0xf7,
        reinterpret_cast<const char*>(va(kAllocOperatorNew)));
}

void set_click_viewport(void* click, const float* viewport) {
    using SetFn = void (EM2_THISCALL*)(void*, const float*);
    fn<SetFn>(kClickSetViewport)(click, viewport);
}

void* construct_scene_view(void* live_view) {
    if (mem::ptr(live_view, 0) != va(kSceneViewVtbl)) return nullptr;
    void* camera = mem::ptr(live_view, kSceneViewCamera);
    void* process = mem::ptr(live_view, kSceneViewProcess);
    void* scene = mem::ptr(live_view, kSceneViewScene);
    void* culler = mem::ptr(live_view, kSceneViewCuller);
    DWORD view_index = mem::get<DWORD>(mem::at(live_view, kSceneViewIndex), 99);
    BYTE use_port = mem::get<BYTE>(mem::at(live_view, kSceneViewUsePort));
    if (!camera || !process || !scene || !culler || view_index >= 4) {
        return nullptr;
    }
    void* view = game_alloc(kSceneViewSize);
    if (!view) return nullptr;
    using CtorFn = void* (EM2_THISCALL*)(void*, void*, void*, void*, void*,
                                         int, int);
    if (fn<CtorFn>(kSceneViewCtor)(view, camera, process, scene, culler,
                                   view_index, use_port) != view) {
        return nullptr;
    }
    mem::put(view, va(kSceneViewVtbl));
    using PoolInitFn = void (EM2_THISCALL*)(void*, int);
    fn<PoolInitFn>(kPointerPoolInit)(mem::at(view, kSceneViewCache), 0x514);
    return view;
}

void set_target_group(void* click, void* group) {
    void* current = mem::ptr(click, kClickTargetGroup);
    if (!click || current == group) return;
    if (group) InterlockedIncrement(reinterpret_cast<LONG*>(mem::at(group, 4)));
    mem::put(mem::at(click, kClickTargetGroup), group);
    if (current && InterlockedDecrement(reinterpret_cast<LONG*>(mem::at(current, 4))) == 0) {
        using DeleteFn = void (EM2_THISCALL*)(void*);
        reinterpret_cast<DeleteFn>(mem::ptr(mem::ptr(current, 0), 0x04))(current);
    }
}

void sync_target_groups() {
    void* group = mem::ptr(g_pipe.live_click, kClickTargetGroup);
    if (!group) return;
    if (mem::ptr(g_pipe.oswald_click, kClickTargetGroup) != group) {
        log_line("[split] scene clicks render into target group %p", group);
    }
    set_target_group(g_pipe.mickey_click, group);
    set_target_group(g_pipe.oswald_click, group);
}

void* construct_scene_click(void* live_click, void* live_render_view,
                            bool own_view) {
    void* click = game_alloc(0x8c);
    if (!click) return nullptr;
    using CtorFn = void* (EM2_THISCALL*)(void*);
    fn<CtorFn>(kDefaultClickCtor)(click);
    mem::put(click, mem::ptr(live_click, 0));
    using PoolInitFn = void (EM2_THISCALL*)(void*, int);
    fn<PoolInitFn>(kPointerPoolInit)(mem::at(click, kClickPool), 0x514);
    mem::put(mem::at(click, kClickClearMode),
             mem::get<DWORD>(mem::at(live_click, kClickClearMode)));
    mem::put(mem::at(click, kClickDerivedByte),
             mem::get<BYTE>(mem::at(live_click, kClickDerivedByte), 1));
    set_target_group(click, mem::ptr(live_click, kClickTargetGroup));
    using BindFn = void (EM2_THISCALL*)(void*, void*, int);
    fn<BindFn>(kDefaultClickBindView)(click, live_render_view, 0);

    using AppendFn = void (EM2_THISCALL*)(void*, void*);
    BYTE* live_list = mem::at(live_click, kClickChildList);
    DWORD child_count = mem::get<DWORD>(live_list + 0x0c);
    void* node = mem::ptr(live_list, 0x04);
    DWORD appended = 0;
    for (DWORD i = 0; node && i < child_count && i < 32; ++i) {
        void* child = node_item(node);
        if (!child) return nullptr;
        if (own_view && i == 0) {
            child = construct_scene_view(child);
            if (!child) {
                log_line("[split] Oswald's scene view could not be built");
                return nullptr;
            }
        }
        fn<AppendFn>(kDefaultClickAppend)(click, child);
        ++appended;
        node = node_next(node);
    }
    if (!appended || appended != child_count ||
        mem::get<DWORD>(mem::at(click, kClickChildList + 0x0c)) != appended) {
        log_line("[split] scene click clone incomplete (%lu/%lu children)",
                 static_cast<unsigned long>(appended),
                 static_cast<unsigned long>(child_count));
        return nullptr;
    }
    return click;
}

void swap_camera_pointer(void* owner, DWORD offset, void* camera,
                         void** saved_owner, void** saved_camera) {
    void* current = mem::ptr(owner, offset);
    if (!current || current == camera) return;
    if (mem::put(mem::at(owner, offset), camera)) {
        *saved_owner = owner;
        *saved_camera = current;
    }
}

void restore_pass(const PassSave& save) {
    if (save.child) {
        mem::put(mem::at(save.child, kSceneViewCamera), save.child_camera);
    }
    if (save.view) mem::put(mem::at(save.view, kViewCamera), save.view_camera);
    restore_frustum(save.frustum);
}

void flush_pass(int index) {
    if (!g_pending_valid[index]) return;
    g_pending_valid[index] = false;
    restore_pass(g_pending[index]);
    g_pending[index] = PassSave();
}

void hold_pass(int index, const PassSave& save) {
    g_pending[index] = save;
    g_pending_valid[index] = true;
}

patch::VtableShim g_per_view_shim;
BYTE g_oswald_visible[kCullerMaxObjects];
volatile LONG g_oswald_visible_count = 0;

using PerViewVisibleFn = void* (EM2_THISCALL*)(void*);

void merge_oswald_visibility(BYTE* bytes) {
    int count = g_oswald_visible_count;
    if (!bytes || count <= 0) return;
    for (int i = 0; i < count; ++i) bytes[i] |= g_oswald_visible[i];
}

void* EM2_THISCALL shared_visible_bytes(void* per_view) {
    BYTE* bytes = static_cast<BYTE*>(fn<PerViewVisibleFn>(kPerViewGetVisible)(per_view));
    if (g_pipe.built) merge_oswald_visibility(bytes);
    return bytes;
}

void share_visibility(void* per_view) {
    g_per_view_shim.remove();
    g_oswald_visible_count = 0;
    if (!g_per_view_shim.capture(per_view, 32)) return;
    g_per_view_shim.set(8, reinterpret_cast<void*>(&shared_visible_bytes));
    if (g_per_view_shim.install()) {
        log_line("[split] view-0 visibility shared between both players");
    }
}

void stop_sharing_visibility() {
    g_oswald_visible_count = 0;
    g_per_view_shim.remove();
}

struct CullSave {
    bool engaged = false;
    void* view = nullptr;
    void* culler = nullptr;
    void* per_view = nullptr;
    void* original_camera = nullptr;
    DWORD view_index = 0;
    int object_count = 0;
};

using PerViewCameraFn = void (EM2_THISCALL*)(void*, void*);
using PerViewQueueFn = void (EM2_THISCALL*)(void*, int);
void mark_view_dirty(const CullSave& save) {
    mem::put<BYTE>(mem::at(save.culler, kCullerViewDirty + save.view_index), 1);
    mem::put<DWORD>(mem::at(save.view, kSceneViewCullFrame), 0xffffffffu);
}

CullSave enter_oswald_culling(void* click, void* camera) {
    CullSave save;
    if (mem::ptr(camera, 0) != va(kNiCameraVtbl)) return save;
    void* view = scene_view_of(click);
    void* culler = mem::ptr(view, kSceneViewCuller);
    DWORD view_index = mem::get<DWORD>(mem::at(view, kSceneViewIndex), 99);
    if (!culler || view_index >= 4) return save;
    void* per_view = mem::ptr(culler, kCullerPerView + view_index * 4);
    void* vtable = mem::ptr(per_view, 0);
    void* original = mem::ptr(per_view, kPerViewCamera);
    bool shimmed = g_per_view_shim.object == per_view && g_per_view_shim.installed();
    void* real_vtable = shimmed ? g_per_view_shim.original : vtable;
    if (!per_view || real_vtable != va(kPerViewVtbl) ||
        mem::ptr(real_vtable, 0x04) != va(kPerViewSetCamera) ||
        mem::ptr(real_vtable, 0x08) != va(kPerViewQueue) ||
        mem::ptr(real_vtable, 0x20) != va(kPerViewGetVisible) ||
        mem::ptr(original, 0) != va(kNiCameraVtbl)) {
        return save;
    }
    if (!shimmed) share_visibility(per_view);
    DWORD last = mem::get<DWORD>(mem::at(culler, kCullerLastIndex), 0xffffffffu);
    int count = last == 0xffffffffu
                    ? 0
                    : static_cast<int>(last + 1 > kCullerMaxObjects
                                           ? kCullerMaxObjects : last + 1);
    auto set_camera = fn<PerViewCameraFn>(kPerViewSetCamera);
    auto queue = fn<PerViewQueueFn>(kPerViewQueue);
    auto visible = fn<PerViewVisibleFn>(kPerViewGetVisible);

    visible(per_view);                 // finish any job still reading Mickey's
    set_camera(per_view, camera);
    if (mem::ptr(per_view, kPerViewCamera) != camera) {
        set_camera(per_view, original);
        return save;
    }
    queue(per_view, count);
    if (mem::get<BYTE>(mem::at(per_view, kPerViewPending)) != 1) {
        visible(per_view);
        set_camera(per_view, original);
        queue(per_view, count);
        visible(per_view);
        return save;
    }
    save.engaged = true;
    save.view = view;
    save.culler = culler;
    save.per_view = per_view;
    save.original_camera = original;
    save.view_index = view_index;
    save.object_count = count;
    mark_view_dirty(save);
    return save;
}

void leave_oswald_culling(const CullSave& save) {
    if (!save.engaged) return;
    auto visible = fn<PerViewVisibleFn>(kPerViewGetVisible);
    const BYTE* oswald = static_cast<const BYTE*>(visible(save.per_view));
    if (oswald) {
        mem::read(oswald, g_oswald_visible, save.object_count);
        g_oswald_visible_count = save.object_count;
    }
    fn<PerViewCameraFn>(kPerViewSetCamera)(save.per_view, save.original_camera);
    fn<PerViewQueueFn>(kPerViewQueue)(save.per_view, save.object_count);
    merge_oswald_visibility(static_cast<BYTE*>(visible(save.per_view)));
    mark_view_dirty(save);
}

struct SeenSave {
    WORD* flags = nullptr;
    WORD mask = 0;
    int count = 0;
};
SeenSave g_mickey_seen;
BYTE g_mickey_seen_bits[kCullerMaxObjects];

void save_mickey_seen(void* click) {
    g_mickey_seen = SeenSave();
    void* view = scene_view_of(click);
    void* culler = mem::ptr(view, kSceneViewCuller);
    DWORD view_index = mem::get<DWORD>(mem::at(view, kSceneViewIndex), 99);
    if (!culler || view_index >= 4) return;
    DWORD last = mem::get<DWORD>(mem::at(culler, kCullerLastIndex), 0xffffffffu);
    if (last == 0xffffffffu) return;
    int count = static_cast<int>(last + 1 > kCullerMaxObjects ? kCullerMaxObjects
                                                               : last + 1);
    WORD mask = static_cast<WORD>(
        mem::get<DWORD>(va(kViewCollectedBits + view_index * 4)));
    WORD* flags = reinterpret_cast<WORD*>(mem::at(culler, kCullerObjectFlags));
    if (!mask || IsBadWritePtr(flags, count * sizeof(WORD))) return;
    for (int i = 0; i < count; ++i) {
        g_mickey_seen_bits[i] = (flags[i] & mask) != 0;
    }
    g_mickey_seen.flags = flags;
    g_mickey_seen.mask = mask;
    g_mickey_seen.count = count;
}

void restore_mickey_seen() {
    SeenSave save = g_mickey_seen;
    g_mickey_seen = SeenSave();
    if (!save.flags) return;
    volatile WORD* flags = save.flags;
    for (int i = 0; i < save.count; ++i) {
        if (g_mickey_seen_bits[i]) flags[i] = flags[i] | save.mask;
    }
}

void queue_rebuild() {
    InterlockedExchange(&g_rebuild, 1);
}

void run_scene_pass(int half, void* click, void* argument, DWORD flags,
                    void* camera) {
    world_views::reaim_billboards(camera);
    g_pass_camera[half - 1] = camera;
    g_scene_cleared[half - 1] = false;
    InterlockedExchange(&g_half, half);
    InterlockedExchange(&g_in_scene, 1);
    world_views::begin_scene_pass();
    g_click_render(click, argument, flags);
    world_views::end_scene_pass(half);
    InterlockedExchange(&g_in_scene, 0);
    world_views::end_billboard_pass();
}

void EM2_THISCALL split_live_render(void* click, void* argument, DWORD flags) {
    void* scene_view = scene_view_of(click);
    if (scene_view && scene_view != g_pipe.source_scene_view) queue_rebuild();
    sync_target_groups();

    flush_pass(1);
    flush_pass(0);
    g_mickey_seen = SeenSave();

    PassSave save;
    void* camera = scene_camera_of(click);
    halve_frustum(camera, &save.frustum);
    g_scene_target_pending = nullptr;
    g_in_live_pass = true;
    run_scene_pass(1, click, argument, flags, camera);
    g_in_live_pass = false;
    // Published only now, so the whole frame redirects to one buffer.
    if (g_scene_target_pending) g_scene_target = g_scene_target_pending;
    hold_pass(0, save);
}

void EM2_THISCALL split_mickey_render(void* click, void* argument, DWORD flags) {
    flush_pass(0);
    PassSave save;
    void* camera = scene_camera_of(click);
    halve_frustum(camera, &save.frustum);
    run_scene_pass(1, click, argument, flags, camera);
    hold_pass(0, save);
}

void EM2_THISCALL split_oswald_render(void* click, void* argument, DWORD flags) {
    flush_pass(0);
    PassSave save;
    void* camera = scene_camera_of(click);
    players::View oswald;
    if (players::player_two_view(&oswald)) {
        camera = oswald.camera;
        swap_camera_pointer(scene_view_of(click), kSceneViewCamera, camera,
                            &save.child, &save.child_camera);
        swap_camera_pointer(mem::ptr(click, kClickView), kViewCamera, camera,
                            &save.view, &save.view_camera);
    }
    halve_frustum(camera, &save.frustum);
    save_mickey_seen(click);
    CullSave cull = enter_oswald_culling(click, camera);
    run_scene_pass(2, click, argument, flags, camera);
    leave_oswald_culling(cull);
    if (!g_chain_end_render) restore_mickey_seen();
    hold_pass(1, save);
}

void EM2_THISCALL split_chain_end_render(void* click, void* argument,
                                         DWORD flags) {
    LONG half = g_half;
    if (g_chain_end_render) g_chain_end_render(click, argument, flags);
    if (half == 1) {
        flush_pass(0);
    } else if (half == 2) {
        flush_pass(1);
        restore_mickey_seen();
        InterlockedExchange(&g_half, 0);
    }
}

void clear_whole_surface(void* device, void* surface) {
    void* get_desc = mem::ptr(mem::ptr(surface, 0), kSurfaceGetDesc * 4);
    if (!get_desc || !g_get_viewport || !g_clear) return;
    DWORD desc[8] = {};  // D3DSURFACE_DESC; width/height are the last two
    using GetDescFn = HRESULT (EM2_STDCALL*)(void*, void*);
    if (FAILED(reinterpret_cast<GetDescFn>(get_desc)(surface, desc)) ||
        !desc[6] || !desc[7]) {
        return;
    }
    D3DViewport9 saved = {};
    bool have_saved = SUCCEEDED(g_get_viewport(device, &saved));
    D3DViewport9 full = { 0, 0, desc[6], desc[7], 0.0f, 1.0f };
    g_set_viewport(device, &full);
    g_clear(device, 0, nullptr, 1 /* D3DCLEAR_TARGET */, 0, 1.0f, 0);
    if (have_saved) g_set_viewport(device, &saved);
}

HRESULT EM2_STDCALL split_set_render_target(void* device, DWORD index,
                                            void* surface) {
    void* scene_target = g_scene_target;
    LONG half = g_half;
    bool in_scene = g_in_scene != 0;
    if (index == 0 && surface && in_scene) {
        if (g_in_live_pass && surface != g_backbuffer) {
            g_scene_target_pending = surface;
        }
        if (surface == g_backbuffer && scene_target) surface = scene_target;
    }
    HRESULT hr = g_set_render_target(device, index, surface);
    if (index == 0 && SUCCEEDED(hr) && surface) {
        g_current_rt0 = surface;
        if (in_scene && (half == 1 || half == 2) && surface == scene_target &&
            !g_scene_cleared[half - 1]) {
            g_scene_cleared[half - 1] = true;
            clear_whole_surface(device, surface);
        }
    }
    return hr;
}

HRESULT EM2_STDCALL split_set_viewport(void* device,
                                       const D3DViewport9* requested) {
    LONG half = g_half;
    if (!requested || half < 1 || half > 2 || g_current_rt0 != g_backbuffer) {
        return g_set_viewport(device, requested);
    }
    D3DViewport9 adjusted = half_of(*requested, half - 1);
    return g_set_viewport(device, &adjusted);
}

bool is_d3d9_code(void* address) {
    HMODULE module = nullptr;
    char path[MAX_PATH];
    if (!address ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        !GetModuleFileNameA(module, path, sizeof(path))) {
        return false;
    }
    for (char* c = path; *c; ++c) {
        if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
    }
    return strstr(path, "d3d9") != nullptr;
}

void* find_device() {
    BYTE* renderer = global(kRenderer);
    if (!renderer) return nullptr;
    static const int kSlots[] = { 0, 3, 16, 42, 47, 48, 75, 118 };
    for (DWORD offset = 0; offset < 0x800; offset += 4) {
        void* candidate = mem::ptr(renderer, offset);
        void* vtable = mem::ptr(candidate, 0);
        if (!vtable) continue;
        bool match = true;
        for (int slot : kSlots) {
            if (!is_d3d9_code(mem::ptr(vtable, slot * 4))) {
                match = false;
                break;
            }
        }
        if (match) return candidate;
    }
    return nullptr;
}

void release_backbuffer() {
    if (g_backbuffer) {
        auto release = reinterpret_cast<ReleaseFn>(
            mem::ptr(mem::ptr(g_backbuffer, 0), 2 * 4));
        if (release) release(g_backbuffer);
    }
    g_backbuffer = nullptr;
    g_current_rt0 = nullptr;
    g_scene_target = nullptr;
    g_scene_target_pending = nullptr;
}

bool install_device_hooks() {
    void* device = find_device();
    if (!device || !g_device_shim.capture(device, kD3DDeviceVtableEntries)) {
        log_line("[split] Direct3D device not found");
        return false;
    }
    auto get_back_buffer = reinterpret_cast<GetBackBufferFn>(
        g_device_shim.slot(kD3DGetBackBuffer));
    void* surface = nullptr;
    if (!get_back_buffer ||
        FAILED(get_back_buffer(device, 0, 0, 0, &surface)) || !surface) {
        log_line("[split] backbuffer unavailable");
        g_device_shim = patch::VtableShim();
        return false;
    }
    g_device = device;
    g_backbuffer = surface;
    g_current_rt0 = surface;   // a frame starts with the backbuffer bound
    g_set_viewport = reinterpret_cast<SetViewportFn>(
        g_device_shim.slot(kD3DSetViewport));
    g_get_viewport = reinterpret_cast<GetViewportFn>(
        g_device_shim.slot(kD3DGetViewport));
    g_set_render_target = reinterpret_cast<SetRenderTargetFn>(
        g_device_shim.slot(kD3DSetRenderTarget));
    g_clear = reinterpret_cast<ClearFn>(g_device_shim.slot(kD3DClear));
    g_device_shim.set(kD3DSetViewport,
                      reinterpret_cast<void*>(&split_set_viewport));
    g_device_shim.set(kD3DSetRenderTarget,
                      reinterpret_cast<void*>(&split_set_render_target));
    if (!g_device_shim.install()) {
        release_backbuffer();
        return false;
    }
    return true;
}

void remove_hooks() {
    InterlockedExchange(&g_half, 0);
    flush_pass(0);
    flush_pass(1);
    g_live_shim.remove();
    g_mickey_shim.remove();
    g_oswald_shim.remove();
    g_chain_end_shim.remove();
    g_device_shim.remove();
    stop_sharing_visibility();
    release_backbuffer();
    g_pass_camera[0] = g_pass_camera[1] = nullptr;
}

bool pipeline_objects_alive() {
    BYTE* app = global(kNiApplication);
    void* step = mem::ptr(app, kAppSceneStep);
    return g_pipe.live_click && step &&
           mem::ptr(app, kAppLiveClick) == g_pipe.live_click &&
           mem::at(step, kStepClickList) == g_pipe.list;
}

void teardown(bool forget_level_objects) {
    if (g_pipe.built) {
        remove_hooks();   // each shim restores only if still installed
        if (pipeline_objects_alive()) {
            set_click_viewport(g_pipe.live_click, g_pipe.live_viewport);
            if (g_pipe.oswald_click) {
                set_click_viewport(g_pipe.oswald_click, g_pipe.oswald_viewport);
                mem::put(mem::at(g_pipe.oswald_click, kClickClearMode),
                         g_pipe.oswald_clear_mode);
            }
            for (void* node : g_pipe.post_nodes) detach_node(g_pipe.list, node);
            detach_node(g_pipe.list, g_pipe.mickey_node);
            detach_node(g_pipe.list, g_pipe.oswald_node);
        }
        g_pipe.built = false;
        log_line("[split] split screen off");
    }
    if (forget_level_objects) {
        set_target_group(g_pipe.mickey_click, nullptr);
        set_target_group(g_pipe.oswald_click, nullptr);
        g_pipe = Pipeline();
    }
}

bool install_click_hooks() {
    if (!g_live_shim.capture(g_pipe.live_click, kClickVtableEntries) ||
        !g_mickey_shim.capture(g_pipe.mickey_click, kClickVtableEntries) ||
        !g_oswald_shim.capture(g_pipe.oswald_click, kClickVtableEntries) ||
        !g_chain_end_shim.capture(g_pipe.chain_end_click, kClickVtableEntries)) {
        return false;
    }
    g_click_render = reinterpret_cast<ClickRenderFn>(
        g_live_shim.slot(kClickRenderSlot));
    g_chain_end_render = reinterpret_cast<ClickRenderFn>(
        g_chain_end_shim.slot(kClickRenderSlot));
    if (!g_click_render ||
        g_oswald_shim.slot(kClickRenderSlot) != g_live_shim.slot(kClickRenderSlot)) {
        return false;
    }
    g_live_shim.set(kClickRenderSlot, reinterpret_cast<void*>(&split_live_render));
    g_mickey_shim.set(kClickRenderSlot,
                      reinterpret_cast<void*>(&split_mickey_render));
    g_oswald_shim.set(kClickRenderSlot,
                      reinterpret_cast<void*>(&split_oswald_render));
    g_chain_end_shim.set(kClickRenderSlot,
                         reinterpret_cast<void*>(&split_chain_end_render));
    return g_device_shim.installed() && g_live_shim.install() &&
           g_mickey_shim.install() && g_oswald_shim.install() &&
           g_chain_end_shim.install();
}

bool build() {
    BYTE* app = global(kNiApplication);
    void* live_click = mem::ptr(app, kAppLiveClick);
    void* live_view = mem::ptr(app, kAppLiveView);
    void* step = mem::ptr(app, kAppSceneStep);
    if (!live_click || !live_view || !step) return false;
    void* list = mem::at(step, kStepClickList);
    void* scene_view = scene_view_of(live_click);
    void* live_node = find_node(list, live_click);
    if (!scene_view || !live_node) return false;

    if (g_pipe.list != list || g_pipe.live_click != live_click ||
        g_pipe.source_scene_view != scene_view) {
        g_pipe = Pipeline();
    }
    g_pipe.list = list;
    g_pipe.live_click = live_click;
    g_pipe.source_scene_view = scene_view;

    auto fail = [](const char* why) {
        log_line("[split] enable failed: %s", why);
        teardown(false);
        remove_hooks();
        for (void* node : g_pipe.post_nodes) detach_node(g_pipe.list, node);
        detach_node(g_pipe.list, g_pipe.mickey_node);
        detach_node(g_pipe.list, g_pipe.oswald_node);
        return false;
    };

    mem::read(mem::at(live_click, kClickViewport), g_pipe.live_viewport, 16);
    if (!g_pipe.oswald_click) {
        g_pipe.oswald_click = construct_scene_click(live_click, live_view, true);
        if (!g_pipe.oswald_click) return fail("Oswald's scene click");
        mem::read(mem::at(g_pipe.oswald_click, kClickViewport),
                  g_pipe.oswald_viewport, 16);
        g_pipe.oswald_clear_mode =
            mem::get<DWORD>(mem::at(g_pipe.oswald_click, kClickClearMode));
    }
    if (!g_pipe.mickey_click) {
        g_pipe.mickey_click = construct_scene_click(live_click, live_view, false);
        if (!g_pipe.mickey_click) return fail("Mickey's scene click");
    }

    g_pipe.post_count = 0;
    for (void* node = node_next(live_node);
         node && g_pipe.post_count < kMaxPostClicks; node = node_next(node)) {
        char name[96];
        click_name(node_item(node), name, sizeof(name));
        if (strncmp(name, "PostProcess_", 12) != 0) break;
        g_pipe.post_clicks[g_pipe.post_count++] = node_item(node);
    }
    if (!g_pipe.post_count) return fail("no PostProcess_ chain after the scene click");
    void* chain_last_node = node_after(live_node, g_pipe.post_count);
    g_pipe.chain_end_click = g_pipe.post_clicks[g_pipe.post_count - 1];

    if (!g_pipe.oswald_node) {
        g_pipe.oswald_node = append_and_take(step, list, g_pipe.oswald_click);
    }
    if (!g_pipe.mickey_node) {
        g_pipe.mickey_node = append_and_take(step, list, g_pipe.mickey_click);
    }
    if (!g_pipe.oswald_node || !g_pipe.mickey_node ||
        !insert_after(list, live_node, g_pipe.mickey_node) ||
        !insert_after(list, chain_last_node, g_pipe.oswald_node)) {
        return fail("could not place the scene clicks");
    }
    void* anchor = g_pipe.oswald_node;
    for (int i = 0; i < g_pipe.post_count; ++i) {
        if (!g_pipe.post_nodes[i]) {
            g_pipe.post_nodes[i] = append_and_take(step, list, g_pipe.post_clicks[i]);
        }
        if (!g_pipe.post_nodes[i] || !insert_after(list, anchor, g_pipe.post_nodes[i])) {
            return fail("could not repeat the composite chain");
        }
        anchor = g_pipe.post_nodes[i];
    }

    const float full[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    set_click_viewport(live_click, full);
    set_click_viewport(g_pipe.oswald_click, full);
    mem::put<DWORD>(mem::at(g_pipe.oswald_click, kClickClearMode),
                    kNiClearStencil | kNiClearZBuffer);

    g_pipe.built = true;   // so a failure below unwinds the list edits
    if (!install_device_hooks()) return fail("Direct3D hooks");
    if (!install_click_hooks()) return fail("click hooks");
    world_views::install_billboard_hook();
    world_views::install_prehud();
    log_line("[split] split screen on: Mickey left, Oswald right (%d-click "
             "composite chain, target group %p)", g_pipe.post_count,
             mem::ptr(g_pipe.oswald_click, kClickTargetGroup));
    return true;
}

}  // namespace

void request(bool on) {
    if (InterlockedExchange(&g_requested, on ? 1 : 0) != (on ? 1 : 0)) {
        g_requested_since = GetTickCount();
    }
}

bool requested() { return g_requested != 0; }
bool active() { return g_pipe.built; }

namespace {

bool pipeline_stale() {
    return g_pipe.built &&
           (!pipeline_objects_alive() || !g_device_shim.installed() ||
            !g_live_shim.installed());
}

}  // namespace

bool needs_maintenance() {
    return (g_requested != 0) != g_pipe.built || g_rebuild != 0 ||
           pipeline_stale();
}

void maintain() {
    bool want = g_requested != 0;
    bool rebuild = InterlockedExchange(&g_rebuild, 0) != 0 || pipeline_stale();
    if (g_pipe.built && (!want || rebuild)) teardown(rebuild);
    if (want && !g_pipe.built) {
        if (GetTickCount() - g_requested_since < 1000) return;
        if (!players::present(1)) return;
        build();
    }
}

void* pass_camera(int half) { return g_pass_camera[half]; }
LONG current_half() { return g_half; }
LONG set_current_half(LONG half) { return InterlockedExchange(&g_half, half); }

bool get_viewport(D3DViewport9* out) {
    return g_device && g_get_viewport && SUCCEEDED(g_get_viewport(g_device, out));
}

bool set_viewport_unsplit(const D3DViewport9* viewport) {
    return g_device && g_set_viewport &&
           SUCCEEDED(g_set_viewport(g_device, viewport));
}

D3DViewport9 half_of(const D3DViewport9& full, int half) {
    D3DViewport9 out = full;
    DWORD left_width = (full.width + 1) / 2;
    if (half == 0) {
        out.width = left_width;
    } else {
        out.x += left_width;
        out.width -= left_width;
    }
    if (!out.width) out.width = 1;
    return out;
}

void halve_frustum(void* camera, FrustumSave* save) {
    float frustum[6];
    if (!camera || !mem::read(mem::at(camera, kCameraFrustum), frustum, 24)) {
        return;
    }
    save->camera = camera;
    memcpy(save->frustum, frustum, sizeof(frustum));
    save->saved = true;
    frustum[0] *= 0.5f;
    frustum[1] *= 0.5f;
    mem::write(mem::at(camera, kCameraFrustum), frustum, 24);
}

void restore_frustum(const FrustumSave& save) {
    if (save.saved) {
        mem::write(mem::at(save.camera, kCameraFrustum), save.frustum, 24);
    }
}

}  // namespace split_screen
