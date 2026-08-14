/* sdlmain.c — SDL2 backend for SDLAce (Jupiter ACE emulator)
 *
 * Copyright (C) 1994 Ian Collier.
 * xz81 changes (C) 1995-6 Russell Marks.
 * xace changes (C) 1997 Edward Patel.
 * xace changes (C) 2010-12 Lawrence Woodman.
 * SDLAce SDL2 port (C) 2026
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

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>



#include "keyboard.h"
#include "spooler.h"
#include "tape.h"
#include "ui_events.h"
#include "z80.h"
#include "ui.h"
#ifdef __APPLE__
#else
#endif

/* Top border is taller to fit the "JUPITER ACE" brand text + red stripes.
 * The value here is at SCALE=2 (native 1× = half); we scale proportionally. */

/* --------------------------------------------------------------------------
 * Global state shared with the Z80 core
 * -------------------------------------------------------------------------- */
int rrnoshm = 4;
unsigned char mem[65536];
unsigned char *memptr[8] = {mem,          mem + 0x2000, mem + 0x4000,
                            mem + 0x6000, mem + 0x8000, mem + 0xa000,
                            mem + 0xc000, mem + 0xe000};

unsigned long tstates = 0, tsmax = 62500;

int memattr[8] = {0, 1, 1, 1, 1, 1, 1, 1}; /* 8K RAM Banks */


/*
 * interrupted states:
 *   0 No interrupt
 *   1 Interrupted
 *   2 Processing Interrupt
 */
volatile int interrupted = 0;
int reset_ace = 0;
int scrn_freq = 2;

/* Used to see if image needs refreshing */
unsigned char video_ram_old[24 * 32];

int refresh_screen = 1;

/* --------------------------------------------------------------------------
 * Audio state
 * -------------------------------------------------------------------------- */
#define AUDIO_SAMPLE_RATE 44100
#define CYCLES_PER_FRAME 65000
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 50) /* 882 */

static int speaker_diaphragm_pos = 0; /* 0 or 1 */
static unsigned long last_speaker_tstates = 0;
static float audio_buffer[SAMPLES_PER_FRAME];
static float dc_blocker_prev_x = 0.0f;
static float dc_blocker_prev_y = 0.0f;

static void set_speaker_diaphragm(int pos) {
  if (tsmax != CYCLES_PER_FRAME || 0) {
    speaker_diaphragm_pos = pos;
    return;
  }

  unsigned long current_t = tstates;
  if (current_t < last_speaker_tstates) {
    last_speaker_tstates = 0;
  }
  if (current_t > CYCLES_PER_FRAME)
    current_t = CYCLES_PER_FRAME;

  unsigned int last_s =
      (last_speaker_tstates * SAMPLES_PER_FRAME) / CYCLES_PER_FRAME;
  unsigned int current_s = (current_t * SAMPLES_PER_FRAME) / CYCLES_PER_FRAME;

  if (current_s > SAMPLES_PER_FRAME)
    current_s = SAMPLES_PER_FRAME;

  float val = speaker_diaphragm_pos ? 0.15f : -0.15f;
  for (unsigned int i = last_s; i < current_s; i++) {
    audio_buffer[i] = val;
  }

  speaker_diaphragm_pos = pos;
  last_speaker_tstates = current_t;
}

/* --------------------------------------------------------------------------
 * Prototypes
 * -------------------------------------------------------------------------- */
void loadrom(unsigned char *x);
void startup(void);
void check_events(void);
void refresh(void);
void closedown(void);

/* Custom SDL event type for macOS menu actions (registered at startup) */
static void normal_speed(void);
static void fast_speed(void);

/* --------------------------------------------------------------------------
 * Paste from clipboard — writes clipboard text to a temp file, then feeds
 * it through the existing spooler (press/release timing is handled there).
 * The temp file is unlink'd immediately after open; the inode stays alive
 * until the spooler fclose's it (standard POSIX behaviour).
 * -------------------------------------------------------------------------- */
static void paste_from_clipboard(void) {
  char *text;
  char *p, *q;
  char tmppath[] = "/tmp/.xace_paste_XXXXXX";
  int fd;

  if (spooler_active()) {
    return;
  }

  text = ui_get_clipboard_text();
  if (!text || !*text) {
    if (text) ui_free_clipboard_text(text);
    return;
  }

  /* Strip carriage returns (\r) to avoid double-Enters on Windows text */
  for (p = text, q = text; *p; p++) {
    if (*p != '\r') {
      *q++ = *p;
    }
  }
  *q = '\0';

  if (!*text) {
    ui_free_clipboard_text(text);
    return;
  }

  fd = mkstemp(tmppath);
  if (fd != -1) {
    /* write returns ssize_t, cast to void to ignore the result */
    (void)write(fd, text, strlen(text));
    close(fd);
    fast_speed(); /* Speed up emulator timing so pasted text streams rapidly */
    spooler_open(tmppath);
    unlink(tmppath); /* safe: spooler holds the fd */
  }
  ui_free_clipboard_text(text);
}



/* -------------------------------------------------------------------------- */
static int pending_release_key = 0;
static int pending_release_timer = 0;

static void ui_event_handler(UiEvent *ev) {
  char spool_filename[257];
  char tape_filename[257];
  switch (ev->type) {
    case UI_EVENT_QUIT:
      raise(SIGQUIT);
      break;
    case UI_EVENT_DELETE_LINE:
      keyboard_keypress(SDLK_F1, 0);
      pending_release_key = SDLK_F1;
      pending_release_timer = 10;
      break;
    case UI_EVENT_ATTACH_TAPE:
      if (ev->data1) {
        tape_attach((char*)ev->data1);
        free(ev->data1);
      } else {
        printf("Enter tape image file: ");
        fflush(stdout);
        if (scanf("%256s", tape_filename) == 1) tape_attach(tape_filename);
      }
      break;
    case UI_EVENT_REWIND_TAPE:
      tape_rewind();
      break;
    case UI_EVENT_INVERSE_VIDEO:
      keyboard_keypress(SDLK_F4, 0);
      pending_release_key = SDLK_F4;
      pending_release_timer = 10;
      break;
    case UI_EVENT_GRAPHICS:
    case UI_EVENT_TOGGLE_GRAPHICS:
      keyboard_keypress(SDLK_F9, 0);
      pending_release_key = SDLK_F9;
      pending_release_timer = 10;
      break;
    case UI_EVENT_SPOOL:
      if (ev->data1) {
        spooler_open((char*)ev->data1);
        free(ev->data1);
      } else {
        printf("Enter spool file: ");
        fflush(stdout);
        if (scanf("%256s", spool_filename) == 1) spooler_open(spool_filename);
      }
      break;
    case UI_EVENT_RESET:
      reset_ace = 1;
      memset(mem + 8192, 0xff, 57344);
      refresh_screen = 1;
      keyboard_clear();
      break;
    case UI_EVENT_BREAK:
      keyboard_keypress(SDLK_ESCAPE, 0);
      pending_release_key = SDLK_ESCAPE;
      pending_release_timer = 10;
      break;
    case UI_EVENT_PASTE:
      paste_from_clipboard();
      break;
    case UI_EVENT_JUPITER_LAYOUT:
    case UI_EVENT_TOGGLE_JUPITER:
      keyboard_toggle_jupiter_layout();
      break;
    default:
      break;
  }
}

void startup(void) {
  ui_init();
  ui_audio_init(AUDIO_SAMPLE_RATE);
}

void closedown(void) {
  ui_closedown();
}
/* -------------------------------------------------------------------------- */
/* Signal / timer helpers */
/* -------------------------------------------------------------------------- */

/* Handle the SIGALRM signal used for the Ace's interrupt */
static void sigint_handler(int signum) {
  (void)signum;
  if (interrupted == 0)
    interrupted = 1;
}

/* Handle any signals to do with quitting the program */
static void sigquit_handler(int signum) {
  (void)signum;
  tape_detach();
  closedown();
  exit(1);
}

/* ints_per_sec — interrupts per second, up to 1000 */
static void set_itimer(int ints_per_sec) {
  struct itimerval itv;
  int freq = 1000 / ints_per_sec;

  itv.it_interval.tv_sec = 0;
  itv.it_interval.tv_usec = (freq % 1000) * 1000;
  itv.it_value.tv_sec = itv.it_interval.tv_sec;
  itv.it_value.tv_usec = itv.it_interval.tv_usec;
  setitimer(ITIMER_REAL, &itv, NULL);
}

static void normal_speed(void) {
  set_itimer(50);           /* 50 ints/sec */
  scrn_freq = 1;            /* refresh screen every 50Hz frame */
  tsmax = CYCLES_PER_FRAME; /* 3.25 MHz CPU: 65,000 t-states per 50Hz frame */
}

static void fast_speed(void) {
  set_itimer(1000); /* 1000 ints/sec */
  scrn_freq = 4;
  tsmax = ULONG_MAX;
}

/* --------------------------------------------------------------------------
 * Observer callbacks
 * -------------------------------------------------------------------------- */
static void tape_observer(int tape_attached, int tape_pos,
                          const char tape_filename[TAPE_MAX_FILENAME_SIZE],
                          TapeMessageType message_type,
                          const char message[TAPE_MAX_MESSAGE_SIZE]) {
  switch (message_type) {
  case TAPE_NO_MESSAGE:
    if (tape_attached)
      printf("TAPE: %s Pos: %04d\n", tape_filename, tape_pos);
    break;
  case TAPE_MESSAGE:
    if (tape_attached)
      printf("TAPE: %s Pos: %04d - %s\n", tape_filename, tape_pos, message);
    else
      printf("TAPE: empty tape Pos: %04d - %s\n", tape_pos, message);
    break;
  case TAPE_ERROR:
    if (tape_attached)
      fprintf(stderr, "TAPE: %s Pos: %04d - Error: %s\n", tape_filename,
              tape_pos, message);
    else
      fprintf(stderr, "TAPE: empty tape Pos: %04d - Error: %s\n", tape_pos,
              message);
    break;
  }
}

static void spooler_observer(SpoolerMessage message) {
  switch (message) {
  case SPOOLER_OPENED:
    printf("Opened spool file.\n");
    break;
  case SPOOLER_OPEN_ERROR:
    fprintf(stderr, "Couldn't open spool file.\n");
    break;
  case SPOOLER_CLOSED:
    normal_speed();
    printf("Closed spool file.\n");
    break;
  default:
    break;
  }
}

/* --------------------------------------------------------------------------
 * CLI args
 * -------------------------------------------------------------------------- */
static void handle_cli_args(int argc, char **argv) {
  int arg_pos = 0;
  char *cli_switch;

  while (arg_pos < argc) {
    cli_switch = argv[arg_pos];
    if (strcasecmp("-s", cli_switch) == 0) {
      if (strcmp("-S", cli_switch) == 0)
        fast_speed();
      if (++arg_pos < argc)
        spooler_open(argv[arg_pos]);
      else
        fprintf(stderr, "Error: Missing filename for %s arg\n", cli_switch);
    } else if (strcasecmp("-t", cli_switch) == 0) {
      if (++arg_pos < argc)
        tape_attach(argv[arg_pos]);
      else
        fprintf(stderr, "Error: Missing filename for %s arg\n", cli_switch);
    }
    arg_pos++;
  }
}

/* --------------------------------------------------------------------------
 * Signal handlers
 * -------------------------------------------------------------------------- */
static void setup_sighandlers(void) {
  struct sigaction sa_quit;
  struct sigaction sa_alarm;
  memset(&sa_quit, 0, sizeof(sa_quit));
  memset(&sa_alarm, 0, sizeof(sa_alarm));

  sa_quit.sa_handler = sigquit_handler;
  sa_quit.sa_flags = 0;
  sa_alarm.sa_handler = sigint_handler;
  sa_alarm.sa_flags = SA_RESTART;

  if (sigaction(SIGINT, &sa_quit, NULL) < 0)
    goto error;
  if (sigaction(SIGHUP, &sa_quit, NULL) < 0)
    goto error;
  if (sigaction(SIGILL, &sa_quit, NULL) < 0)
    goto error;
  if (sigaction(SIGTERM, &sa_quit, NULL) < 0)
    goto error;
  if (sigaction(SIGQUIT, &sa_quit, NULL) < 0)
    goto error;
  if (sigaction(SIGSEGV, &sa_quit, NULL) < 0)
    goto error;
  if (sigaction(SIGALRM, &sa_alarm, NULL) < 0)
    goto error;
  return;

error:
  perror("sigaction failed");
  exit(1);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv) {
  printf("SDLAce: Jupiter ACE emulator v%s\n", XACE_VERSION);
  printf("Keys:\n");
#ifdef __APPLE__
  printf("\tCmd-1  - Delete Line\n");
  printf("\tCmd-3  - Attach a tape image\n");
  printf("\tCmd-4  - Inverse Video\n");
  printf("\tCmd-9  - Graphics\n");
  printf("\tCmd-5  - Spool from a file\n");
  printf("\tCmd-2  - Reset\n");
  printf("\tEsc    - Break\n");
  printf("\tCmd-Q  - Quit SDLAce\n");
#else
  printf("\tF1     - Delete Line\n");
  printf("\tF3     - Attach a tape image\n");
  printf("\tF4     - Inverse Video\n");
  printf("\tF9     - Graphics\n");
  printf("\tF5     - Spool from a file\n");
  printf("\tF2     - Reset\n");
  printf("\tEsc    - Break\n");
  printf("\tCtrl-Q - Quit SDLAce\n");
#endif

  loadrom(mem);
  tape_patches((char *)mem);
  memset(mem + 8192, 0xff, 57344);
  memset(video_ram_old, 0xff, 768);

  spooler_init(spooler_observer, keyboard_clear, keyboard_keypress);
  startup();
  setup_sighandlers();
  normal_speed();
  handle_cli_args(argc, argv);
  tape_add_observer(tape_observer);
  keyboard_init(NULL);
  mainloop();
  return 0;
}

/* --------------------------------------------------------------------------
 * ROM loading
 * -------------------------------------------------------------------------- */
void loadrom(unsigned char *x) {
  FILE *in;

  /* First try CWD, then the directory of the executable */
  in = fopen("ace.rom", "rb");

#ifdef __APPLE__
  if (!in) {
    const char *base = ui_get_base_path();
    if (base != NULL) {
      char path[4096];
      snprintf(path, sizeof(path), "%sace.rom", base);
      in = fopen(path, "rb");
    }
  }
#endif

  if (in) {
    if (fread(x, 1, 8192, in) != 8192) {
      printf("Couldn't load ROM.\n");
      fclose(in);
      exit(1);
    }
    fclose(in);
  } else {
    printf("Couldn't load ROM (looked in current directory).\n");
    exit(1);
  }
}

/* --------------------------------------------------------------------------
 * I/O port handlers (called by Z80 core)
 * -------------------------------------------------------------------------- */
unsigned int in(int h, int l) {
  if ((l & 1) == 0) {
    set_speaker_diaphragm(0);
  }
  if (l == 0xfe) /* keyboard */
    switch (h) {
    case 0xfe:
      return keyboard_get_keyport(0);
    case 0xfd:
      return keyboard_get_keyport(1);
    case 0xfb:
      return keyboard_get_keyport(2);
    case 0xf7:
      return keyboard_get_keyport(3);
    case 0xef:
      return keyboard_get_keyport(4);
    case 0xdf:
      return keyboard_get_keyport(5);
    case 0xbf:
      return keyboard_get_keyport(6);
    case 0x7f:
      return keyboard_get_keyport(7);
    default:
      return 255;
    }
  return 255;
}

unsigned int out(int h, int l, int a) {
  if ((l & 1) == 0) {
    set_speaker_diaphragm(1);
  }
  (void)h;
  (void)a;
  return 0;
}

/* --------------------------------------------------------------------------
 * Timing helpers (called by Z80 core)
 * -------------------------------------------------------------------------- */
void fix_tstates(void) {
  if (tsmax == CYCLES_PER_FRAME) {
    /* 1. Generate audio for this 50Hz frame (65,000 t-states) */
    if (1) {
      unsigned int last_s =
          (last_speaker_tstates * SAMPLES_PER_FRAME) / CYCLES_PER_FRAME;
      if (last_s > SAMPLES_PER_FRAME)
        last_s = SAMPLES_PER_FRAME;
      float val = speaker_diaphragm_pos ? 0.15f : -0.15f;
      for (unsigned int i = last_s; i < SAMPLES_PER_FRAME; i++) {
        audio_buffer[i] = val;
      }

      /* Apply DC blocker filter to eliminate pops/crackles */
      float prev_x = dc_blocker_prev_x;
      float prev_y = dc_blocker_prev_y;
      for (unsigned int i = 0; i < SAMPLES_PER_FRAME; i++) {
        float x = audio_buffer[i];
        float y = x - prev_x + 0.995f * prev_y;
        prev_x = x;
        prev_y = y;
        audio_buffer[i] = y;
      }
      dc_blocker_prev_x = prev_x;
      dc_blocker_prev_y = prev_y;

      /* Queue audio */
      ui_audio_queue(audio_buffer, SAMPLES_PER_FRAME);
      last_speaker_tstates = 0;
    }

    /* 2. Trigger Z80 50Hz maskable interrupt for screen refresh & timing */
    if (interrupted == 0) {
      interrupted = 1;
    }

    ui_sync_frame();
  }

  tstates = 0;
}

/* --------------------------------------------------------------------------
 * Interrupt handler (called by Z80 core each instruction)
 * -------------------------------------------------------------------------- */

void do_interrupt(void) {
  static int count = 0;
  if (interrupted == 1) {
    interrupted = 2;

    count++;
    if (count >= scrn_freq) {
      count = 0;
      spooler_read();
      ui_refresh(mem + 0x2400, mem + 0x2c00, refresh_screen);
      refresh_screen = 0;
    }

    ui_check_events(ui_event_handler);
    if (pending_release_timer > 0) {
      pending_release_timer--;
      if (pending_release_timer == 0 && pending_release_key != 0) {
        keyboard_keyrelease(pending_release_key, 0);
        pending_release_key = 0;
      }
    }
    interrupted = 0;
  }
}

/* -------------------------------------------------------------------------- */
