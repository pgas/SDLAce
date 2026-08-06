/* Emulation of the keyboard — SDL2 backend
 *
 * Copyright (C) 2012 Lawrence Woodman
 * SDL2 port (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * --------------------------------------------------------------------------
 *
 * For information on how the Ace's keyboard works look at:
 *   http://www.jupiter-ace.co.uk/prog_keyboardread.html
 *
 * This file replaces keyboard.c when building with USE_SDL.
 * SDL_Keycode values are used instead of X11 KeySym values.
 */
#include <stdio.h>
#include <SDL2/SDL.h>

#include "keyboard.h"

static NonAceKeyHandler keyboard_non_ace_key_handler = NULL;

static unsigned char keyboard_ports[8] = {
  0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff
};


/* key, keyport_index, and_value, keyport_index, and_value
 * if keyport_index == -1 then no action for that port */
static const int keypress_response[] = {
  SDLK_F1,         3, 0xfe, 0, 0xfe,   /* Delete line */
  SDLK_F4,         3, 0xf7, 0, 0xfe,   /* Inverse video */
  SDLK_F9,         4, 0xfd, 0, 0xfe,   /* Graphics */
  SDLK_LEFT,       3, 0xef, 0, 0xfe,
  SDLK_DOWN,       4, 0xf7, 0, 0xfe,
  SDLK_UP,         4, 0xef, 0, 0xfe,
  SDLK_RIGHT,      4, 0xfb, 0, 0xfe,
  SDLK_BACKSPACE,  0, 0xfe, 4, 0xfe,
  SDLK_DELETE,     0, 0xfe, 4, 0xfe,
  SDLK_1,          3, 0xfe, -1, 0,
  SDLK_2,          3, 0xfd, -1, 0,
  SDLK_3,          3, 0xfb, -1, 0,
  SDLK_4,          3, 0xf7, -1, 0,
  SDLK_5,          3, 0xef, -1, 0,
  SDLK_6,          4, 0xef, -1, 0,
  SDLK_7,          4, 0xf7, -1, 0,
  SDLK_8,          4, 0xfb, -1, 0,
  SDLK_9,          4, 0xfd, -1, 0,
  SDLK_0,          4, 0xfe, -1, 0,
  SDLK_EXCLAIM,    3, 0xfe, 0, 0xfd,
  SDLK_AT,         3, 0xfd, 0, 0xfd,
  SDLK_HASH,       3, 0xfb, 0, 0xfd,
  SDLK_DOLLAR,     3, 0xf7, 0, 0xfd,
  SDLK_PERCENT,    3, 0xef, 0, 0xfd,
  SDLK_ESCAPE,     7, 0xfe, 0, 0xfe,   /* Break */
  SDLK_AMPERSAND,  4, 0xef, 0, 0xfd,
  SDLK_QUOTE,      4, 0xf7, 0, 0xfd,   /* apostrophe */
  SDLK_LEFTPAREN,  4, 0xfb, 0, 0xfd,
  SDLK_RIGHTPAREN, 4, 0xfd, 0, 0xfd,
  SDLK_UNDERSCORE, 4, 0xfe, 0, 0xfd,
  SDLK_a,          1, 0xfe, -1, 0,
  SDLK_b,          7, 0xf7, -1, 0,
  SDLK_c,          0, 0xef, -1, 0,
  SDLK_d,          1, 0xfb, -1, 0,
  SDLK_e,          2, 0xfb, -1, 0,
  SDLK_f,          1, 0xf7, -1, 0,
  SDLK_g,          1, 0xef, -1, 0,
  SDLK_h,          6, 0xef, -1, 0,
  SDLK_i,          5, 0xfb, -1, 0,
  SDLK_j,          6, 0xf7, -1, 0,
  SDLK_k,          6, 0xfb, -1, 0,
  SDLK_l,          6, 0xfd, -1, 0,
  SDLK_m,          7, 0xfd, -1, 0,
  SDLK_n,          7, 0xfb, -1, 0,
  SDLK_o,          5, 0xfd, -1, 0,
  SDLK_p,          5, 0xfe, -1, 0,
  SDLK_q,          2, 0xfe, -1, 0,
  SDLK_r,          2, 0xf7, -1, 0,
  SDLK_s,          1, 0xfd, -1, 0,
  SDLK_t,          2, 0xef, -1, 0,
  SDLK_u,          5, 0xf7, -1, 0,
  SDLK_v,          7, 0xef, -1, 0,
  SDLK_w,          2, 0xfd, -1, 0,
  SDLK_x,          0, 0xf7, -1, 0,
  SDLK_y,          5, 0xef, -1, 0,
  SDLK_z,          0, 0xfb, -1, 0,
  SDLK_LESS,       2, 0xf7, 0, 0xfd,
  SDLK_GREATER,    2, 0xef, 0, 0xfd,
  SDLK_LEFTBRACKET,  5, 0xef, 0, 0xfd,
  SDLK_RIGHTBRACKET, 5, 0xf7, 0, 0xfd,
  SDLK_SEMICOLON,  5, 0xfd, 0, 0xfd,
  SDLK_QUOTEDBL,   5, 0xfe, 0, 0xfd,
  SDLK_BACKQUOTE,  1, 0xfe, 0, 0xfd,   /* tilde/backtick */
  SDLK_BACKSLASH,  1, 0xfb, 0, 0xfd,
  SDLK_CARET,      6, 0xef, 0, 0xfd,
  SDLK_MINUS,      6, 0xf7, 0, 0xfd,
  SDLK_PLUS,       6, 0xfb, 0, 0xfd,
  SDLK_EQUALS,     6, 0xfd, 0, 0xfd,
  SDLK_RETURN,     6, 0xfe, -1, 0,
  SDLK_KP_ENTER,   6, 0xfe, -1, 0,
  SDLK_COLON,      0, 0xf9, -1, 0,
  SDLK_QUESTION,   0, 0xed, -1, 0,
  SDLK_SLASH,      7, 0xef, 0, 0xfd,
  SDLK_ASTERISK,   7, 0xf7, 0, 0xfd,
  SDLK_COMMA,      7, 0xfb, 0, 0xfd,
  SDLK_PERIOD,     7, 0xfd, 0, 0xfd,
  SDLK_SPACE,      7, 0xfe, -1, 0,
  SDLK_TAB,        7, 0xfe, -1, 0
};

void
keyboard_init(NonAceKeyHandler non_ace_key_handler)
{
  keyboard_non_ace_key_handler = non_ace_key_handler;
  keyboard_clear();
}

unsigned char
keyboard_get_keyport(int port)
{
  return keyboard_ports[port];
}

void
keyboard_clear(void)
{
  int i;
  for (i = 0; i < 8; i++)
    keyboard_ports[i] = 0xff;
}

static int
keyboard_get_key_response(AceKeySym ks, int *keyport1, int *keyport2,
                          int *keyport1_response, int *keyport2_response)
{
  int i;
  int num_keys = sizeof(keypress_response)/sizeof(keypress_response[0]);

  for (i = 0; i < num_keys; i += 5) {
    if (keypress_response[i] == (int)ks) {
      *keyport1 = keypress_response[i+1];
      *keyport2 = keypress_response[i+3];
      *keyport1_response = keypress_response[i+2];
      *keyport2_response = keypress_response[i+4];
      return 1;
    }
  }
  return 0;
}

static void
keyboard_process_keypress_keyports(AceKeySym ks)
{
  int key_found;
  int keyport1, keyport2;
  int keyport1_and_value, keyport2_and_value;

  key_found = keyboard_get_key_response(ks, &keyport1, &keyport2,
                &keyport1_and_value, &keyport2_and_value);
  if (key_found) {
    keyboard_ports[keyport1] &= keyport1_and_value;
    if (keyport2 != -1)
      keyboard_ports[keyport2] &= keyport2_and_value;
  }
}

static void
keyboard_process_keyrelease_keyports(AceKeySym ks)
{
  int key_found;
  int keyport1, keyport2;
  int keyport1_or_value, keyport2_or_value;

  key_found = keyboard_get_key_response(ks, &keyport1, &keyport2,
                &keyport1_or_value, &keyport2_or_value);
  if (key_found) {
    keyboard_ports[keyport1] |= ~keyport1_or_value;
    if (keyport2 != -1)
      keyboard_ports[keyport2] |= ~keyport2_or_value;
  }
}

/* ACE shift key: port 0, AND 0xfe */
#define ACE_SHIFT_PORT 0
#define ACE_SHIFT_MASK 0xfe

/* Returns non-zero if the keycode is a letter key */
static int
is_letter_key(AceKeySym ks)
{
  return (ks >= SDLK_a && ks <= SDLK_z);
}

void
keyboard_keypress(AceKeySym ks, int key_state)
{
  if (!(key_state & ACE_CTRL_MASK)) {
    keyboard_process_keypress_keyports(ks);
    /* If shift is held for a letter key, also press the ACE shift port */
    if (is_letter_key(ks) && (key_state & (KMOD_SHIFT | KMOD_CAPS)))
      keyboard_ports[ACE_SHIFT_PORT] &= ACE_SHIFT_MASK;
  }
  keyboard_non_ace_key_handler(ks, key_state);
}

void
keyboard_keyrelease(AceKeySym ks, int key_state)
{
  if (!(key_state & ACE_CTRL_MASK)) {
    keyboard_process_keyrelease_keyports(ks);
    /* Release the ACE shift port when the shift modifier is released */
    if (is_letter_key(ks) && (key_state & (KMOD_SHIFT | KMOD_CAPS)))
      keyboard_ports[ACE_SHIFT_PORT] |= ~ACE_SHIFT_MASK;
  }
}
