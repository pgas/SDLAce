/* Spooler controller
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

#include <stdio.h>

#include "spooler.h"

static FILE *spooler_file = NULL;
static int spooler_wait = 0;
static int last_char_was_newline = 0;
static enum {
  SPOOLER_INACTIVE,
  SPOOLER_READ_CHAR,
  SPOOLER_CLEAR_CHAR
} spooler_state = SPOOLER_INACTIVE;

static SpoolerObserver spooler_observer = NULL;
static ClearKeyboardFunc clear_keyboard = NULL;
static KeypressFunc keypress = NULL;

void
spooler_init(SpoolerObserver spooler_observer_func,
             ClearKeyboardFunc clear_keyboard_func,
             KeypressFunc keypress_func)
{
  spooler_observer = spooler_observer_func;
  clear_keyboard = clear_keyboard_func;
  keypress = keypress_func;
  spooler_state = SPOOLER_INACTIVE;
}

void
spooler_open(char *filename)
{
  spooler_file = fopen(filename, "rt");
  if (spooler_file) {
    spooler_state = SPOOLER_READ_CHAR;
    spooler_wait = 0;
    last_char_was_newline = 0;
    spooler_observer(SPOOLER_OPENED);
    clear_keyboard();
  } else {
    spooler_observer(SPOOLER_OPEN_ERROR);
  }
}

void
spooler_close(void)
{
  if (spooler_active()) {
    fclose(spooler_file);
    clear_keyboard();
    spooler_file = NULL;
    spooler_state = SPOOLER_INACTIVE;
    spooler_observer(SPOOLER_CLOSED);
  }
}

static void
spooler_read_char(void)
{
  AceKeySym ks;

  ks = fgetc(spooler_file);
  if (ks == EOF) {
    spooler_close();
  } else {
    last_char_was_newline = (ks == '\n' || ks == '\r');
    keypress(ks, 0);
  }
}

/* Key hold frames: 1 frame (2 Z80 interrupts / 40ms) prevents Jupiter Ace ROM auto-repeat */
#define SPOOLER_HOLD_FRAMES 1
/* Release frames for normal characters */
#define SPOOLER_RELEASE_FRAMES 1
/* Extra release frames after ENTER so Jupiter Ace ROM has time to process/compile line */
#define SPOOLER_NEWLINE_RELEASE_FRAMES 6

void
spooler_read(void)
{
  switch (spooler_state) {
    case SPOOLER_INACTIVE:
      break;
    case SPOOLER_READ_CHAR:
      if (spooler_wait == 0) {
        spooler_read_char();
      }
      if (spooler_active()) {
        spooler_wait++;
        if (spooler_wait >= SPOOLER_HOLD_FRAMES) {
          spooler_wait = 0;
          spooler_state = SPOOLER_CLEAR_CHAR;
        }
      }
      break;
    case SPOOLER_CLEAR_CHAR:
      if (spooler_wait == 0) {
        clear_keyboard();
      }
      spooler_wait++;
      int target_release = last_char_was_newline ? SPOOLER_NEWLINE_RELEASE_FRAMES : SPOOLER_RELEASE_FRAMES;
      if (spooler_wait >= target_release) {
        spooler_wait = 0;
        spooler_state = SPOOLER_READ_CHAR;
      }
      break;
  }
}

int
spooler_active(void)
{
  return spooler_state != SPOOLER_INACTIVE;
}
