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

/* -------------------------------------------------------------------------
 * Custom SDL user event codes (stored in SDL_UserEvent.code)
 * -------------------------------------------------------------------------
 * When a menu item is selected, macos_ui.m posts an SDL_UserEvent of type
 * g_ace_event_type with one of these codes.  sdlmain.c's check_events()
 * handles them identically to the corresponding key presses.
 *
 * For ACE_EVENT_ATTACH_TAPE and ACE_EVENT_SPOOL, ev.user.data1 points to
 * a malloc'd C-string containing the chosen file path.  The receiver is
 * responsible for free()ing it.
 * ------------------------------------------------------------------------- */
#define ACE_EVENT_DELETE_LINE   1
#define ACE_EVENT_ATTACH_TAPE   2   /* data1 = malloc'd filepath */
#define ACE_EVENT_INVERSE_VIDEO 3
#define ACE_EVENT_GRAPHICS      4
#define ACE_EVENT_SPOOL         5   /* data1 = malloc'd filepath */
#define ACE_EVENT_RESET         6
#define ACE_EVENT_BREAK         7
#define ACE_EVENT_PASTE         8

/* Call once after SDL_Init + SDL_CreateWindow.
 * user_event_type is the value returned by SDL_RegisterEvents(1). */
void macos_setup_menu(Uint32 user_event_type);

#endif /* __APPLE__ */
