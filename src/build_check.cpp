#include "build_check.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>

#include "engine.h"
#include "util.h"

namespace build_check {

namespace {

constexpr DWORD kSha256Bytes = 32;
constexpr DWORD kReadChunkBytes = 1 << 20;

// SHA-256 of a file through Windows CNG, as lowercase hex into out
// (2 * kSha256Bytes + 1 chars).
bool sha256_file(const char* path, char* out) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    static BYTE buffer[kReadChunkBytes];
    BYTE digest[kSha256Bytes];
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0,
                                        nullptr, 0, 0))) {
        ok = true;
        DWORD read = 0;
        while (ok && ReadFile(file, buffer, sizeof(buffer), &read, nullptr) &&
               read > 0) {
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer, read, 0));
        }
        ok = ok && BCRYPT_SUCCESS(
                       BCryptFinishHash(hash, digest, sizeof(digest), 0));
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);

    if (ok) {
        for (DWORD i = 0; i < kSha256Bytes; ++i) {
            snprintf(out + 2 * i, 3, "%02x", digest[i]);
        }
    }
    return ok;
}

}  // namespace

bool is_supported_game() {
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) {
        log_line("[build] cannot find the game's executable (%lu)",
                 GetLastError());
        return false;
    }
    char digest[2 * kSha256Bytes + 1] = {};
    if (!sha256_file(path, digest)) {
        log_line("[build] cannot read %s (%lu)", path, GetLastError());
        return false;
    }
    if (strcmp(digest, engine::kGameSha256) != 0) {
        log_line("[build] %s has sha256 %s, expected %s", path, digest,
                 engine::kGameSha256);
        return false;
    }
    log_line("[build] %s is the supported build", path);
    return true;
}

}  // namespace build_check
