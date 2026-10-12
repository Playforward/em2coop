#pragma once

#include <windows.h>

void log_open(const char* path);
void log_close();
void log_line(const char* format, ...) __attribute__((format(printf, 1, 2)));

namespace mem {

bool read(const void* address, void* out, SIZE_T size);
bool write(void* address, const void* in, SIZE_T size);

template <typename T>
bool read(const void* address, T* out) { return read(address, out, sizeof(T)); }

template <typename T>
T get(const void* address, T fallback = T()) {
    T value;
    return read(address, &value, sizeof(T)) ? value : fallback;
}

template <typename T>
bool put(void* address, const T& value) { return write(address, &value, sizeof(T)); }

inline BYTE* at(void* base, DWORD offset) { return static_cast<BYTE*>(base) + offset; }

inline void* ptr(void* base, DWORD offset) {
    return base ? get<void*>(at(base, offset)) : nullptr;
}

bool read_cstr(const char* address, char* out, size_t capacity);

}  // namespace mem

namespace patch {

bool redirect_call(void* site, void* expected, void* target, const char* what);
void* hook_entry(void* entry, const BYTE* prologue, size_t length, void* hook,
                 const char* what);

struct VtableShim {
    static constexpr int kMaxEntries = 160;
    void* object = nullptr;
    void* original = nullptr;
    void* entries[kMaxEntries] = {};
    int count = 0;

    bool capture(void* obj, int entry_count);
    void* slot(int index) const { return entries[index]; }
    void set(int index, void* fn) { entries[index] = fn; }
    bool install();
    void remove();
    bool installed() const;
};

}  // namespace patch

bool every(DWORD* last, DWORD interval_ms);
