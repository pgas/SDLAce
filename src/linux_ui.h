#pragma once
#ifndef __APPLE__

#include <SDL2/SDL.h>

void* linux_create_window(int width, int height, const char* title, Uint32 user_event_type);
void linux_pump_events(void);
void linux_cancel_menu(void);
void linux_show_attach_tape_dialog(void);
void linux_show_spool_dialog(void);

#endif
