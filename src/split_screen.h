#pragma once

#include <windows.h>

#include "engine.h"

namespace split_screen {

void request(bool on);
bool requested();
bool active();
void maintain();
bool needs_maintenance();
void* pass_camera(int half);
LONG current_half();
LONG set_current_half(LONG half);   // returns the previous value
bool get_viewport(engine::D3DViewport9* out);
bool set_viewport_unsplit(const engine::D3DViewport9* viewport);
engine::D3DViewport9 half_of(const engine::D3DViewport9& full, int half);

struct FrustumSave {
    void* camera = nullptr;
    float frustum[6] = {};
    bool saved = false;
};
void halve_frustum(void* camera, FrustumSave* save);
void restore_frustum(const FrustumSave& save);

}  // namespace split_screen
