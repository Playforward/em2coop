#pragma once

namespace world_views {
void install_billboard_hook();
void reaim_billboards(void* camera);
void end_billboard_pass();
void install_prehud();
void begin_scene_pass();
void end_scene_pass(int half);

}  // namespace world_views
