#include "world_views.h"

#include <cstring>

#include "engine.h"
#include "split_screen.h"
#include "util.h"

using namespace engine;

extern "C" {
DWORD em2_billboard_rotate_target = 0;
DWORD em2_billboard_override_block = 0;
void em2_billboard_record(void* billboard);
void em2_billboard_update_thunk();
}
__asm__(
    ".text\n"
    ".globl _em2_billboard_update_thunk\n"
    "_em2_billboard_update_thunk:\n"
    "    pushl %ecx\n"
    "    pushl %ecx\n"
    "    call _em2_billboard_record\n"
    "    addl $4, %esp\n"
    "    popl %ecx\n"
    "    movl _em2_billboard_override_block, %eax\n"
    "    testl %eax, %eax\n"
    "    jz 1f\n"
    "    movl %eax, 4(%esp)\n"
    "1:  jmp *_em2_billboard_rotate_target\n");

namespace {

constexpr int kMaxBillboards = 512;
void* g_billboards[kMaxBillboards];
int g_billboard_count = 0;
bool g_billboard_hook = false;

}  // namespace

extern "C" void em2_billboard_record(void* billboard) {
    for (int i = 0; i < g_billboard_count; ++i) {
        if (g_billboards[i] == billboard) return;
    }
    if (g_billboard_count < kMaxBillboards) g_billboards[g_billboard_count++] = billboard;
}

namespace world_views {

void install_billboard_hook() {
    if (g_billboard_hook) return;
    em2_billboard_rotate_target = reinterpret_cast<DWORD>(va(kRotateToCamera));
    for (DWORD site : kRotateToCameraCalls) {
        BYTE code[5];
        LONG rel = 0;
        if (!mem::read(va(site), code, 5)) return;
        memcpy(&rel, code + 1, 4);
        if (code[0] != 0xE8 || va(site) + 5 + rel != va(kRotateToCamera)) {
            log_line("[views] RotateToCamera call sites changed; billboards "
                     "left as they are");
            return;
        }
    }
    for (DWORD site : kRotateToCameraCalls) {
        patch::redirect_call(va(site), va(kRotateToCamera),
                             reinterpret_cast<void*>(&em2_billboard_update_thunk),
                             "billboard");
    }
    g_billboard_hook = true;
}

void end_billboard_pass() { em2_billboard_override_block = 0; }

void reaim_billboards(void* camera) {
    if (!g_billboard_hook || !camera) return;
    BYTE* renderer = global(kRenderer);
    static float block[kRendererCameraDataSize / sizeof(float)];
    float rotate[9], translate[3];
    if (!renderer ||
        !mem::read(renderer + kRendererCameraData, block, sizeof(block)) ||
        !mem::read(mem::at(camera, kCameraRotate), rotate, sizeof(rotate)) ||
        !mem::read(mem::at(camera, kCameraTranslate), translate,
                   sizeof(translate))) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        block[0 + i] = translate[i];
        block[3 + i] = rotate[i * 3 + 2];
        block[6 + i] = rotate[i * 3 + 1];
        block[9 + i] = -rotate[i * 3 + 0];
    }
    using RotateFn = void (EM2_THISCALL*)(void*, const void*);
    auto rotate_to_camera = fn<RotateFn>(kRotateToCamera);
    int kept = 0;
    for (int i = 0; i < g_billboard_count; ++i) {
        void* billboard = g_billboards[i];
        if (mem::ptr(billboard, 0) != va(kBillboardVtbl)) continue;
        g_billboards[kept++] = billboard;
        rotate_to_camera(billboard, block);
    }
    g_billboard_count = kept;
    em2_billboard_override_block = reinterpret_cast<DWORD>(block);
}

}  // namespace world_views

namespace {

using ClickRenderFn = void (EM2_THISCALL*)(void*, void*, DWORD);
patch::VtableShim g_prehud_shim;
ClickRenderFn g_prehud_render = nullptr;
BYTE* g_on_top = nullptr;            // the accumulator's on-top list
constexpr int kMaxOnTop = 512;

struct PassRange {
    int start = 0;
    int end = 0;
    bool valid = false;
};
PassRange g_range[2];
int g_range_start = 0;

int on_top_count() { return g_on_top ? mem::get<int>(g_on_top + kOnTopCount) : 0; }

void EM2_THISCALL prehud_render(void* click, void* argument, DWORD flags) {
    PassRange ranges[2] = { g_range[0], g_range[1] };
    g_range[0] = g_range[1] = PassRange();
    int count = on_top_count();
    void** items = g_on_top ? static_cast<void**>(mem::ptr(g_on_top, kOnTopItems))
                            : nullptr;
    BYTE* renderer = global(kRenderer);
    if (!split_screen::active() || !items || !renderer || count <= 0 ||
        count > kMaxOnTop || (!ranges[0].valid && !ranges[1].valid)) {
        g_prehud_render(click, argument, flags);
        return;
    }
    void* all[kMaxOnTop];
    memcpy(all, items, count * sizeof(void*));

    using SetCameraFn = void (EM2_THISCALL*)(void*, void*);
    auto set_camera = fn<SetCameraFn>(kRendererSetCamera);
    void* previous_camera = mem::ptr(renderer, kRendererCurrentCamera);
    BYTE saved_camera_data[kRendererCameraBlockSize];
    memcpy(saved_camera_data, renderer + kRendererSavedCameraData,
           kRendererCameraBlockSize);
    for (int half = 0; half < 2; ++half) {
        const PassRange& range = ranges[half];
        void* camera = split_screen::pass_camera(half);
        if (!range.valid || !camera || range.start < 0 || range.end > count ||
            range.start >= range.end) {
            continue;
        }
        int n = range.end - range.start;
        for (int i = 0; i < n; ++i) items[i] = all[range.end - 1 - i];
        mem::put<int>(g_on_top + kOnTopCount, n);
        split_screen::FrustumSave frustum;
        split_screen::halve_frustum(camera, &frustum);
        set_camera(renderer, camera);
        memcpy(renderer + kRendererSavedCameraData,
               renderer + kRendererCameraData, kRendererCameraBlockSize);
        world_views::reaim_billboards(camera);
        LONG previous_half = split_screen::set_current_half(half + 1);
        g_prehud_render(click, argument, flags);   // draws and clears the list
        split_screen::set_current_half(previous_half);
        world_views::end_billboard_pass();
        split_screen::restore_frustum(frustum);
    }
    mem::put<int>(g_on_top + kOnTopCount, 0);
    if (previous_camera) set_camera(renderer, previous_camera);
    memcpy(renderer + kRendererSavedCameraData, saved_camera_data,
           kRendererCameraBlockSize);
}

}  // namespace

namespace world_views {

void begin_scene_pass() { g_range_start = on_top_count(); }

void end_scene_pass(int half) {
    if (half < 1 || half > 2 || !g_on_top) return;
    g_range[half - 1] = { g_range_start, on_top_count(), true };
}

void install_prehud() {
    if (g_prehud_shim.installed()) return;
    BYTE* app = global(kNiApplication);
    BYTE* frame = static_cast<BYTE*>(mem::ptr(app, kAppRenderFrame));
    if (!frame) return;
    void* step_node = mem::ptr(frame + kFrameStepList, 0x04);
    DWORD steps = mem::get<DWORD>(frame + kFrameStepList + 0x0c);
    for (DWORD i = 0; step_node && i < steps && i < 16; ++i) {
        BYTE* step = static_cast<BYTE*>(mem::ptr(step_node, 0x08));
        void* click_node = step ? mem::ptr(step + kStepClickList, 0x04) : nullptr;
        DWORD clicks = step ? mem::get<DWORD>(step + kStepClickList + 0x0c) : 0;
        for (DWORD j = 0; click_node && j < clicks && j < 64; ++j) {
            BYTE* click = static_cast<BYTE*>(mem::ptr(click_node, 0x08));
            char name[40] = {};
            mem::read_cstr(static_cast<const char*>(mem::ptr(click, kClickName)),
                           name, sizeof(name));
            if (click && strcmp(name, "Pre HUD Render Click") == 0) {
                BYTE* accumulator = static_cast<BYTE*>(mem::ptr(click, kClickAccumulator));
                if (mem::ptr(accumulator, 0) != va(kDepthAccumulatorVtbl) ||
                    mem::ptr(accumulator + kOnTopList, 0) != va(kOnTopListVtbl)) {
                    log_line("[views] Pre HUD click has no known accumulator; "
                             "prompts left full screen");
                    return;
                }
                g_on_top = accumulator + kOnTopList;
                if (!g_prehud_shim.capture(click, 32)) return;
                g_prehud_render = reinterpret_cast<ClickRenderFn>(
                    g_prehud_shim.slot(kClickRenderSlot));
                g_prehud_shim.set(kClickRenderSlot,
                                  reinterpret_cast<void*>(&prehud_render));
                if (g_prehud_shim.install()) {
                    log_line("[views] world prompts drawn per half (on-top list %p)",
                             g_on_top);
                }
                return;
            }
            click_node = mem::ptr(click_node, 0x00);
        }
        step_node = mem::ptr(step_node, 0x00);
    }
    log_line("[views] Pre HUD Render Click not found");
}

}  // namespace world_views

