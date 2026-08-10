/* macos_ui.h — Native macOS menu and UI for xAce
 *
 * SDL2 macOS port (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once
#ifdef __APPLE__

#include <SDL2/SDL.h>

#include "ui_events.h"

/* Initialize the native macOS menu bar. 
 * user_event_type: the SDL event type to post when menu items are selected. */
void macos_setup_menu(Uint32 user_event_type);

void macos_show_attach_tape_dialog(void);
void macos_show_spool_dialog(void);

#endif /* __APPLE__ */
