#include "fixes.h"

#include "engine.h"
#include "util.h"

using namespace engine;

extern "C" {
volatile DWORD em2_p2_cursor = 0;
DWORD em2_cursor_feed_target = 0;
void em2_cursor_feed_thunk();
}
__asm__(
    ".text\n"
    ".globl _em2_cursor_feed_thunk\n"
    "_em2_cursor_feed_thunk:\n"
    "    testl %esi, %esi\n"
    "    jz 1f\n"
    "    cmpl %esi, _em2_p2_cursor\n"
    "    jne 1f\n"
    "    movl $1, 0x14(%esp)\n"
    "1:  jmp *_em2_cursor_feed_target\n");

namespace fixes {

void install_cursor_hud_feed() {
    em2_cursor_feed_target = reinterpret_cast<DWORD>(va(kCursorHudFeed));
    int patched = 0;
    for (DWORD site : kCursorHudFeedCalls) {
        patched += patch::redirect_call(
            va(site), va(kCursorHudFeed),
            reinterpret_cast<void*>(&em2_cursor_feed_thunk), "cursor HUD feed");
    }
    log_line("[fixes] cursor HUD feed: %d/2 call sites patched", patched);
}

void set_player_two_cursor(void* cursor) {
    em2_p2_cursor = reinterpret_cast<DWORD>(cursor);
}

}  // namespace fixes

namespace {

constexpr DWORD kNodeClip0 = 0x68;
constexpr DWORD kSeqCurrent = 0x0c;
constexpr DWORD kSeqStart = 0x14;
constexpr DWORD kSeqSlots = 0x13c;

using PlaylistEvalFn = void (EM2_THISCALL*)(void*, void**, float*, float, BYTE*);
using PlaylistResetFn = void (EM2_THISCALL*)(void*, BYTE*);
volatile LONG g_rescues = 0;

void* current_clip(BYTE* seq) {
    LONG cur = *reinterpret_cast<LONG*>(seq + kSeqCurrent);
    if (cur != 0 && cur != 1) return nullptr;
    return *reinterpret_cast<void**>(seq + kSeqSlots + cur * 4);
}

bool rescue(void* node, BYTE* seq, float time) {
    void* clip0 = *reinterpret_cast<void**>(static_cast<BYTE*>(node) + kNodeClip0);
    if (!clip0) return false;
    fn<PlaylistResetFn>(kPlaylistReset)(node, seq);
    *reinterpret_cast<float*>(seq + kSeqStart) = time;
    LONG total = InterlockedIncrement(&g_rescues);
    if (total <= 5 || total % 500 == 0) {
        log_line("[fixes] playlist %p ran past its last clip; restarted "
                 "(#%ld)", node, static_cast<long>(total));
    }
    return current_clip(seq) != nullptr;
}

}  // namespace

extern "C" void EM2_THISCALL em2_playlist_eval_guard(
    void* node, void** out_clip, float* out_time, float time, BYTE* seq) {
    auto eval = fn<PlaylistEvalFn>(kPlaylistEval);
    if (!current_clip(seq) && !rescue(node, seq, time)) {
        *out_clip = nullptr;   // callers treat null as "nothing to sample"
        *out_time = 0.0f;
        return;
    }
    eval(node, out_clip, out_time, time, seq);
    if (!current_clip(seq) && rescue(node, seq, time)) {
        eval(node, out_clip, out_time, time, seq);
    }
}

namespace fixes {

void install_playlist_wrap() {
    int patched = 0;
    for (DWORD site : kPlaylistEvalCalls) {
        patched += patch::redirect_call(
            va(site), va(kPlaylistEval),
            reinterpret_cast<void*>(&em2_playlist_eval_guard), "playlist");
    }
    log_line("[fixes] playlist wrap: %d/4 call sites guarded", patched);
}

}  // namespace fixes
