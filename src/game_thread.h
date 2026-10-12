#pragma once

#include <windows.h>

namespace game_thread {

using Task = void (*)();
bool attach(HINSTANCE self);
void detach();
void post(Task task);
bool is_current();

}  // namespace game_thread
