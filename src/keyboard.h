/* keyboard.h — SDL2 keyboard interface for SDLAce
 *
 * Copyright (C) 2012 Lawrence Woodman
 * SDL2 port (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <SDL2/SDL.h>

typedef SDL_Keycode AceKeySym;
#define ACE_CTRL_MASK  KMOD_CTRL

typedef void (*NonAceKeyHandler)(AceKeySym ks, int key_state);

extern void keyboard_init(NonAceKeyHandler non_ace_key_handler);
extern unsigned char keyboard_get_keyport(int port);
extern void keyboard_clear(void);
extern void keyboard_keypress(AceKeySym ks, int key_state);
extern void keyboard_keyrelease(AceKeySym ks, int key_state);

extern void keyboard_set_graphics_mode(int active);
extern int keyboard_get_graphics_mode(void);
extern void keyboard_toggle_graphics_mode(void);

extern void keyboard_set_jupiter_layout(int active);
extern int keyboard_get_jupiter_layout(void);
extern void keyboard_toggle_jupiter_layout(void);

#endif
