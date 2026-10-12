#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
FILE* g_log = nullptr;
CRITICAL_SECTION g_log_lock;
bool g_log_lock_ready = false;
}  // namespace

void log_open(const char* path) {
    if (!g_log_lock_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_ready = true;
    }
    fopen_s(&g_log, path, "a");
}

void log_close() {
    if (g_log) fclose(g_log);
    g_log = nullptr;
}

void log_line(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    OutputDebugStringA(message);
    if (!g_log || !g_log_lock_ready) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log, "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\n", now.wYear,
            now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, message);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

namespace mem {

bool read(const void* address, void* out, SIZE_T size) {
    SIZE_T done = 0;
    return address &&
           ReadProcessMemory(GetCurrentProcess(), address, out, size, &done) &&
           done == size;
}

bool write(void* address, const void* in, SIZE_T size) {
    SIZE_T done = 0;
    return address &&
           WriteProcessMemory(GetCurrentProcess(), address, in, size, &done) &&
           done == size;
}

bool read_cstr(const char* address, char* out, size_t capacity) {
    if (!out || !capacity) return false;
    out[0] = '\0';
    if (!address) return false;
    for (size_t i = 0; i + 1 < capacity; ++i) {
        if (!read(address + i, &out[i], 1)) {
            out[i] = '\0';
            return false;
        }
        if (!out[i]) return true;
    }
    out[capacity - 1] = '\0';
    return true;
}

}  // namespace mem

namespace patch {

namespace {

bool write_code(void* site, const void* bytes, size_t length) {
    DWORD old_protect = 0;
    if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    memcpy(site, bytes, length);
    VirtualProtect(site, length, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, length);
    return true;
}

LONG rel32(const void* from_next, const void* to) {
    return static_cast<LONG>(reinterpret_cast<DWORD_PTR>(to) -
                             reinterpret_cast<DWORD_PTR>(from_next));
}

}  // namespace

bool redirect_call(void* site, void* expected, void* target, const char* what) {
    BYTE* at = static_cast<BYTE*>(site);
    BYTE current[5];
    if (!mem::read(at, current, sizeof(current))) return false;
    LONG rel = 0;
    memcpy(&rel, current + 1, sizeof(rel));
    if (current[0] != 0xE8 || at + 5 + rel != expected) {
        log_line("[patch] %s: %p is not `call %p` (%02x ...); left alone",
                 what, site, expected, current[0]);
        return false;
    }
    LONG new_rel = rel32(at + 5, target);
    if (!write_code(at + 1, &new_rel, sizeof(new_rel))) {
        log_line("[patch] %s: VirtualProtect failed at %p (%lu)", what, site,
                 GetLastError());
        return false;
    }
    return true;
}

void* hook_entry(void* entry, const BYTE* prologue, size_t length, void* hook,
                 const char* what) {
    BYTE* site = static_cast<BYTE*>(entry);
    BYTE current[16];
    if (length < 5 || length > sizeof(current) ||
        !mem::read(site, current, length) ||
        memcmp(current, prologue, length) != 0) {
        log_line("[patch] %s: unexpected prologue at %p; not hooked", what,
                 site);
        return nullptr;
    }
    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(
        nullptr, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return nullptr;
    memcpy(trampoline, prologue, length);
    trampoline[length] = 0xE9;
    LONG back = rel32(trampoline + length + 5, site + length);
    memcpy(trampoline + length + 1, &back, sizeof(back));
    FlushInstructionCache(GetCurrentProcess(), trampoline, length + 5);

    BYTE jump[5] = { 0xE9 };
    LONG to_hook = rel32(site + 5, hook);
    memcpy(jump + 1, &to_hook, sizeof(to_hook));
    if (!write_code(site, jump, sizeof(jump))) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    return trampoline;
}

bool VtableShim::capture(void* obj, int entry_count) {
    if (!obj || entry_count <= 0 || entry_count > kMaxEntries) return false;
    void* vtable = mem::ptr(obj, 0);
    if (!vtable || !mem::read(vtable, entries, entry_count * sizeof(void*))) {
        return false;
    }
    object = obj;
    original = vtable;
    count = entry_count;
    return true;
}

bool VtableShim::install() {
    if (!object) return false;
    void* shim = entries;
    return mem::put(object, shim);
}

bool VtableShim::installed() const {
    return object && mem::ptr(object, 0) == entries;
}

void VtableShim::remove() {
    if (installed()) mem::put(object, original);
    object = nullptr;
    original = nullptr;
}

}  // namespace patch

bool every(DWORD* last, DWORD interval_ms) {
    DWORD now = GetTickCount();
    if (*last && now - *last < interval_ms) return false;
    *last = now ? now : 1;
    return true;
}
