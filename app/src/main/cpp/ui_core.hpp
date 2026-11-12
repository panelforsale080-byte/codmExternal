#pragma once

void ui_gl_surface_created();
void ui_gl_surface_changed(int w, int h);
void ui_gl_surface_destroyed();
void ui_gl_draw_frame();
int ui_gl_frame_delay_ms();

void ui_touch(int action, float x, float y);
void ui_open_menu();
void ui_toggle_menu();
void ui_minimize_menu();
bool ui_menu_interactive();
bool ui_needs_continuous_gl();

void ui_esp_start();
void ui_esp_stop();
void ui_set_context(void *ctx);
void ui_sync_menu_touch_state();
