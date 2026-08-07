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

/* Call once after SDL_Init + SDL_CreateWindow.
 * user_event_type is the value returned by SDL_RegisterEvents(1). */
void macos_setup_menu(Uint32 user_event_type);

#endif /* __APPLE__ */
