/* Tests for keyboard emulation
 *
 * Copyright (C) 2012 Lawrence Woodman
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
 */
#include <assert.h>
#include <stdlib.h>

#include "keyboard.h"

#ifdef USE_SDL
/* SDL keysym shims so test code reads naturally */
#  define XK_3         SDLK_3
#  define XK_7         SDLK_7
#  define XK_A         SDLK_a      /* SDL lowercases everything */
#  define XK_Tab       SDLK_TAB
#  define XK_u         SDLK_u
#  define XK_e         SDLK_e
#  define XK_f         SDLK_f
#  define XK_l         SDLK_l
#  define XK_n         SDLK_n
#  define XK_z         SDLK_z
#  define XK_asterisk  SDLK_ASTERISK
#  define XK_Sys_Req   SDLK_SYSREQ
#  define ControlMask  ACE_CTRL_MASK
#endif

static void
check_keyports(unsigned char *expected_keyports)
{
  int i;
  for (i = 0; i < 8; i++) {
    assert(keyboard_get_keyport(i) == expected_keyports[i]);
  }
}

static struct {
  int handler_called;
  AceKeySym keySym;
  int key_state;
} non_ace_key_handler_status;

static void
non_ace_key_handler_init(void)
{
  non_ace_key_handler_status.handler_called = 0;
}

static void
non_ace_key_handler(AceKeySym ks, int key_state)
{
  non_ace_key_handler_status.handler_called = 1;
  non_ace_key_handler_status.keySym = ks;
  non_ace_key_handler_status.key_state = key_state;
}

static void
test_keyboard_clear()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
  };

  keyboard_init(non_ace_key_handler);
  keyboard_keypress(XK_3, 0);
  keyboard_keypress(XK_7, 0);
  keyboard_keypress(XK_u, 0);
  keyboard_keypress(XK_e, 0);
  keyboard_keypress(XK_f, 0);
  keyboard_keypress(XK_l, 0);
  keyboard_keypress(XK_n, 0);
  keyboard_keypress(XK_z, 0);
  keyboard_clear();

  check_keyports(expected_keyports);
}

static void
test_keyboard_keypress_single_key()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xfe
  };

  keyboard_init(non_ace_key_handler);
  keyboard_keypress('\t', 0);
  check_keyports(expected_keyports);
}

static void
test_keyboard_keypress_multiple_keys()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xf7, 0xfb, 0xff,
    0xf7, 0xf7, 0xff, 0xfb
  };

  keyboard_init(non_ace_key_handler);
  keyboard_keypress(XK_7, 0);
  keyboard_keypress(XK_u, 0);
  keyboard_keypress(XK_e, 0);
  keyboard_keypress(XK_f, 0);
  keyboard_keypress(XK_n, 0);
  check_keyports(expected_keyports);
}

static void
test_keyboard_keypress_symbol_on_physical_keyboard()
{
  unsigned char expected_keyports[8] = {
    0xfd, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xf7
  };

  keyboard_init(non_ace_key_handler);
  keyboard_keypress(XK_asterisk, 0);
  check_keyports(expected_keyports);
}

static void
test_keyboard_keypress_key_not_found()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
  };

  keyboard_init(non_ace_key_handler);
  keyboard_keypress(XK_Sys_Req, 0);
  check_keyports(expected_keyports);
}

static void
test_keyboard_keypress_pass_to_non_ace_key_handler()
{
#ifdef USE_SDL
  /* SDL: uppercase 'A' = SDLK_a + KMOD_SHIFT modifier */
  unsigned char expected_keyports[8] = {
    0xfe, 0xfe, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
  };

  non_ace_key_handler_init();
  keyboard_init(non_ace_key_handler);
  keyboard_keypress(XK_A, KMOD_SHIFT);
  assert(non_ace_key_handler_status.handler_called);
  assert(non_ace_key_handler_status.keySym == XK_A);
  assert(non_ace_key_handler_status.key_state == KMOD_SHIFT);

  check_keyports(expected_keyports);
#else
  unsigned char expected_keyports[8] = {
    0xfe, 0xfe, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
  };

  non_ace_key_handler_init();
  keyboard_init(non_ace_key_handler);
  keyboard_keypress(XK_A, 0);
  assert(non_ace_key_handler_status.handler_called);
  assert(non_ace_key_handler_status.keySym == XK_A);
  assert(non_ace_key_handler_status.key_state == 0);

  check_keyports(expected_keyports);
#endif
}

static void
test_keyboard_keypress_ignore_keyports_for_keys_pressed_with_control_key()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
  };

  non_ace_key_handler_init();
  keyboard_init(non_ace_key_handler);
  keyboard_keypress(XK_A, ControlMask);
  assert(non_ace_key_handler_status.handler_called);
  assert(non_ace_key_handler_status.keySym == XK_A);
  assert(non_ace_key_handler_status.key_state == ControlMask);

  check_keyports(expected_keyports);
}

static void
test_keyboard_keyrelease_from_single_key()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
  };

  keyboard_init(non_ace_key_handler);
#ifdef USE_SDL
  keyboard_keypress(XK_A, KMOD_SHIFT);
  keyboard_keyrelease(XK_A, KMOD_SHIFT);
#else
  keyboard_keypress(XK_A, 0);
  keyboard_keyrelease(XK_A, 0);
#endif
  check_keyports(expected_keyports);
}

static void
test_keyboard_keyrelease_from_single_key_with_multiple_pressed()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xfe
  };

  keyboard_init(non_ace_key_handler);
#ifdef USE_SDL
  keyboard_keypress(XK_A, KMOD_SHIFT);
  keyboard_keypress(XK_Tab, 0);
  keyboard_keyrelease(XK_A, KMOD_SHIFT);
#else
  keyboard_keypress(XK_A, 0);
  keyboard_keypress(XK_Tab, 0);
  keyboard_keyrelease(XK_A, 0);
#endif
  check_keyports(expected_keyports);
}

static void
test_keyboard_keyrelease_ignore_keyports_for_keys_pressed_with_control_key()
{
  unsigned char expected_keyports[8] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
  };

  keyboard_init(non_ace_key_handler);
  keyboard_keyrelease(XK_A, ControlMask);

  check_keyports(expected_keyports);
}

static void
test_keyboard_shift_digit_and_graphics_mode()
{
  /* Shift+8 (SDLK_8 + KMOD_SHIFT) maps to Symbol Shift (Port 0, 0xfd) + B (Port 7, 0xf7) in normal mode */
  unsigned char expected_star_keyports[8] = {
    0xfd, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xf7
  };

  keyboard_init(non_ace_key_handler);
  keyboard_set_graphics_mode(0);
  keyboard_keypress(SDLK_8, KMOD_SHIFT);
  check_keyports(expected_star_keyports);
  keyboard_keyrelease(SDLK_8, KMOD_SHIFT);

  /* In Graphics Mode, Shift+8 maps to Symbol Shift (Port 0, 0xfd) + 8 (Port 4, 0xfb) */
  keyboard_clear();
  keyboard_set_graphics_mode(1);
  keyboard_keypress(SDLK_8, KMOD_SHIFT);
  unsigned char expected_graphics_keyports[8] = {
    0xfd, 0xff, 0xff, 0xff,
    0xfb, 0xff, 0xff, 0xff
  };
  check_keyports(expected_graphics_keyports);
  keyboard_set_graphics_mode(0);
}

int main()
{
  test_keyboard_clear();
  test_keyboard_keypress_single_key();
  test_keyboard_keypress_multiple_keys();
  test_keyboard_keypress_symbol_on_physical_keyboard();
  test_keyboard_keypress_key_not_found();
  test_keyboard_keypress_pass_to_non_ace_key_handler();
  test_keyboard_keypress_ignore_keyports_for_keys_pressed_with_control_key();
  test_keyboard_keyrelease_from_single_key();
  test_keyboard_keyrelease_from_single_key_with_multiple_pressed();
  test_keyboard_keyrelease_ignore_keyports_for_keys_pressed_with_control_key();
  test_keyboard_shift_digit_and_graphics_mode();
  exit(0);
}
