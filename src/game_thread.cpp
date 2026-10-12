#include "game_thread.h"

#include "util.h"

namespace game_thread {

namespace {

constexpr int kMaxTasks = 32;

HWND g_window = nullptr;
DWORD g_thread_id = 0;
HHOOK g_hook = nullptr;
CRITICAL_SECTION g_lock;
Task g_tasks[kMaxTasks];
int g_task_count = 0;

BOOL CALLBACK find_window(HWND window, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(window)) {
        g_window = window;
        return FALSE;
    }
    return TRUE;
}

void run_pending() {
    Task tasks[kMaxTasks];
    int count;
    EnterCriticalSection(&g_lock);
    count = g_task_count;
    for (int i = 0; i < count; ++i) tasks[i] = g_tasks[i];
    g_task_count = 0;
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < count; ++i) tasks[i]();
}

LRESULT CALLBACK get_message_hook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && g_task_count) run_pending();
    return CallNextHookEx(g_hook, code, wparam, lparam);
}

}  // namespace

bool attach(HINSTANCE self) {
    InitializeCriticalSection(&g_lock);
    for (int tries = 0; tries < 120 && !g_window; ++tries) {
        EnumWindows(find_window, 0);
        if (!g_window) Sleep(500);
    }
    if (!g_window) {
        log_line("[thread] game window not found; co-op disabled");
        return false;
    }
    g_thread_id = GetWindowThreadProcessId(g_window, nullptr);
    g_hook = SetWindowsHookExA(WH_GETMESSAGE, get_message_hook, self,
                               g_thread_id);
    log_line("[thread] game window %p thread %lu hook %p", g_window,
             g_thread_id, g_hook);
    return g_hook != nullptr;
}

void detach() {
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

void post(Task task) {
    EnterCriticalSection(&g_lock);
    bool queued = false;
    for (int i = 0; i < g_task_count; ++i) queued |= g_tasks[i] == task;
    if (!queued && g_task_count < kMaxTasks) g_tasks[g_task_count++] = task;
    LeaveCriticalSection(&g_lock);
    if (g_window) PostMessageA(g_window, WM_NULL, 0, 0);
}

bool is_current() { return GetCurrentThreadId() == g_thread_id; }

}  // namespace game_thread
