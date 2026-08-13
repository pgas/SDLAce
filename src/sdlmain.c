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

#include <SDL2/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "ace_logo_data.h"
#include "stb_image.h"

#include "keyboard.h"
#include "spooler.h"
#include "tape.h"
#include "ui_events.h"
#include "z80.h"
#ifdef __APPLE__
#include "macos_ui.h"
#else
#include "linux_ui.h"
#endif

/* Top border is taller to fit the "JUPITER ACE" brand text + red stripes.
 * The value here is at SCALE=2 (native 1× = half); we scale proportionally. */
#define BORDER_SIDE (16 * SCALE) /* left/right/bottom border width */
#define BORDER_TOP (56 * SCALE)  /* top border (brand area) */

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

int hsize = 256 * SCALE, vsize = 192 * SCALE;

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
 * SDL state
 * -------------------------------------------------------------------------- */
static SDL_Window *sdl_window = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture *sdl_texture = NULL;

/* Bezel logo texture (ace.png) */
static SDL_Texture *bezel_logo_texture = NULL;
static int bezel_logo_w = 0;
static int bezel_logo_h = 0;

/* Pixel buffer: 32-bit ARGB, hsize × vsize */
static Uint32 *pixel_buf = NULL;

static Uint32 col_black = 0;
static Uint32 col_white = 0;

static int invert = 0;

/* --------------------------------------------------------------------------
 * Audio state
 * -------------------------------------------------------------------------- */
static SDL_AudioDeviceID sdl_audio_device = 0;
#define AUDIO_SAMPLE_RATE 44100
#define CYCLES_PER_FRAME 65000
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 50) /* 882 */

static int speaker_diaphragm_pos = 0; /* 0 or 1 */
static unsigned long last_speaker_tstates = 0;
static float audio_buffer[SAMPLES_PER_FRAME];
static float dc_blocker_prev_x = 0.0f;
static float dc_blocker_prev_y = 0.0f;

static void set_speaker_diaphragm(int pos) {
  if (tsmax != CYCLES_PER_FRAME || sdl_audio_device == 0) {
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
static Uint32 ace_sdl_event_type = (Uint32)-1;
static void normal_speed(void);
static void fast_speed(void);

/* --------------------------------------------------------------------------
 * Mouse selection & Clipboard state
 * -------------------------------------------------------------------------- */
static int selecting_text = 0;
static int sel_start_col = -1, sel_start_row = -1;
static int sel_end_col = -1, sel_end_row = -1;

/* Check if character cell at (col, row) is currently selected */
static int is_cell_selected(int col, int row) {
  if (sel_start_col < 0 || sel_start_row < 0 || sel_end_col < 0 || sel_end_row < 0)
    return 0;

  int idx = row * 32 + col;
  int start_idx = sel_start_row * 32 + sel_start_col;
  int end_idx = sel_end_row * 32 + sel_end_col;

  if (start_idx > end_idx) {
    int tmp = start_idx;
    start_idx = end_idx;
    end_idx = tmp;
  }

  return (idx >= start_idx && idx <= end_idx);
}

static void clear_selection(void) {
  if (sel_start_col >= 0 || sel_start_row >= 0 || sel_end_col >= 0 || sel_end_row >= 0 || selecting_text) {
    sel_start_col = sel_start_row = sel_end_col = sel_end_row = -1;
    selecting_text = 0;
    refresh_screen = 1;
  }
}

/* Convert mouse window coordinates (win_x, win_y) to Ace text grid (col, row).
   If clamp_bounds is true, clamps out-of-bounds coordinates to edge cells (0..31, 0..23).
   Returns 1 if inside display area (or clamped), 0 if strictly outside when clamp_bounds is false. */
static int get_grid_pos(int win_x, int win_y, int *col, int *row, int clamp_bounds) {
  int win_w, win_h;
  SDL_GetRendererOutputSize(sdl_renderer, &win_w, &win_h);

  int log_w, log_h;
  SDL_GetWindowSize(sdl_window, &log_w, &log_h);
  float px_ratio = (float)win_w / (float)log_w;

  win_x = (int)(win_x * px_ratio);
  win_y = (int)(win_y * px_ratio);

  int bside = (int)(BORDER_SIDE * px_ratio);
  int btop = (int)(BORDER_TOP * px_ratio);

  int avail_w = win_w - bside * 2;
  int avail_h = win_h - btop - bside;

  float scale_x = (float)avail_w / hsize;
  float scale_y = (float)avail_h / vsize;
  float scale = (scale_x < scale_y) ? scale_x : scale_y;

  int dst_w = (int)(hsize * scale);
  int dst_h = (int)(vsize * scale);
  int dst_x = (win_w - dst_w) / 2;
  int dst_y = btop + (avail_h - dst_h) / 2;

  int margin = (int)(16 * scale / SCALE);
  if (margin < 8) margin = 8;

  int active_w = dst_w - margin * 2;
  int active_h = dst_h - margin * 2;
  int active_x = dst_x + margin;
  int active_y = dst_y + margin;

  if (win_x < active_x || win_x >= active_x + active_w ||
      win_y < active_y || win_y >= active_y + active_h) {
    if (!clamp_bounds)
      return 0; /* Strictly outside active display area */
  }

  if (win_x < active_x) win_x = active_x;
  if (win_x >= active_x + active_w) win_x = active_x + active_w - 1;
  if (win_y < active_y) win_y = active_y;
  if (win_y >= active_y + active_h) win_y = active_y + active_h - 1;

  float rel_x = (float)(win_x - active_x) / active_w;
  float rel_y = (float)(win_y - active_y) / active_h;

  int c = (int)(rel_x * 32);
  int r = (int)(rel_y * 24);

  if (c < 0) c = 0;
  if (c > 31) c = 31;
  if (r < 0) r = 0;
  if (r > 23) r = 23;

  *col = c;
  *row = r;
  return 1;
}

/* Convert Jupiter Ace character byte into standard UTF-8 string.
   Handles standard ASCII (0x20..0x7E), inverted characters, and 2x2 graphics block characters. */
static void ace_char_to_utf8(unsigned char c, unsigned char *font_bitmap, char *out_buf, size_t out_buf_size) {
  int is_inv = (c & 128) ? 1 : 0;
  unsigned char code = c & 127;

  /* Check for standard printable ASCII range */
  if (code >= 32 && code <= 126) {
    out_buf[0] = (char)code;
    out_buf[1] = '\0';
    return;
  }

  /* Sample top-left, top-right, bottom-left, bottom-right 4x4 pixel quadrants from character bitmap */
  int quad_tl = 0, quad_tr = 0, quad_bl = 0, quad_br = 0;
  if (font_bitmap) {
    for (int y = 0; y < 4; y++) {
      if (font_bitmap[y] & 0xF0) quad_tl = 1;
      if (font_bitmap[y] & 0x0F) quad_tr = 1;
    }
    for (int y = 4; y < 8; y++) {
      if (font_bitmap[y] & 0xF0) quad_bl = 1;
      if (font_bitmap[y] & 0x0F) quad_br = 1;
    }
  }

  if (is_inv) {
    quad_tl = !quad_tl;
    quad_tr = !quad_tr;
    quad_bl = !quad_bl;
    quad_br = !quad_br;
  }

  int mask = (quad_tl << 3) | (quad_tr << 2) | (quad_bl << 1) | quad_br;

  /* Map 2x2 quadrant mask to Unicode Block Elements (U+2580..U+259F) */
  const char *utf8_block = " ";
  switch (mask) {
    case 0x0: utf8_block = " "; break;            /* 0000 empty */
    case 0x1: utf8_block = "\xe2\x96\x97"; break; /* 0001 lower right quadrant (▗) */
    case 0x2: utf8_block = "\xe2\x96\x96"; break; /* 0010 lower left quadrant (▖) */
    case 0x3: utf8_block = "\xe2\x96\x84"; break; /* 0011 lower half block (▄) */
    case 0x4: utf8_block = "\xe2\x96\x9d"; break; /* 0100 upper right quadrant (▝) */
    case 0x5: utf8_block = "\xe2\x96\x90"; break; /* 0101 right half block (▐) */
    case 0x6: utf8_block = "\xe2\x96\x9a"; break; /* 0110 upper right & lower left (▚) */
    case 0x7: utf8_block = "\xe2\x96\x97"; break; /* 0111 lower right, lower left, upper right */
    case 0x8: utf8_block = "\xe2\x96\x98"; break; /* 1000 upper left quadrant (▘) */
    case 0x9: utf8_block = "\xe2\x96\x9e"; break; /* 1001 upper left & lower right (▞) */
    case 0xA: utf8_block = "\xe2\x96\x8c"; break; /* 1010 left half block (▌) */
    case 0xB: utf8_block = "\xe2\x96\x99"; break; /* 1011 upper left, lower left, lower right */
    case 0xC: utf8_block = "\xe2\x96\x80"; break; /* 1100 upper half block (▀) */
    case 0xD: utf8_block = "\xe2\x96\x9c"; break; /* 1101 upper left, upper right, lower right */
    case 0xE: utf8_block = "\xe2\x96\x9b"; break; /* 1110 upper left, upper right, lower left */
    case 0xF: utf8_block = "\xe2\x96\x88"; break; /* 1111 full block (█) */
  }

  snprintf(out_buf, out_buf_size, "%s", utf8_block);
}

/* Copy current selected text range (or full screen if no selection) to system clipboard */
static void copy_selection_to_clipboard(void) {
  int start_col = sel_start_col;
  int start_row = sel_start_row;
  int end_col = sel_end_col;
  int end_row = sel_end_row;

  /* If no active selection range, default to full screen copy */
  if (start_col < 0 || start_row < 0 || end_col < 0 || end_row < 0) {
    start_col = 0;
    start_row = 0;
    end_col = 31;
    end_row = 23;
  }

  int start_idx = start_row * 32 + start_col;
  int end_idx = end_row * 32 + end_col;
  if (start_idx > end_idx) {
    int tmp_r = start_row, tmp_c = start_col;
    start_row = end_row; start_col = end_col;
    end_row = tmp_r; end_col = tmp_c;
  }

  /* Allocate buffer for UTF-8 formatted text */
  size_t buf_cap = 4096;
  char *buf = malloc(buf_cap);
  if (!buf) return;
  buf[0] = '\0';
  size_t buf_len = 0;

  unsigned char *video_ram = mem + 0x2400;
  unsigned char *charset = mem + 0x2c00;

  for (int r = start_row; r <= end_row; r++) {
    int col_from = (r == start_row) ? start_col : 0;
    int col_to = (r == end_row) ? end_col : 31;

    char line_buf[256] = "";
    size_t line_len = 0;

    for (int c = col_from; c <= col_to; c++) {
      unsigned char ace_c = video_ram[r * 32 + c];
      unsigned char char_code = ace_c & 127;
      unsigned char *font_bmp = charset + char_code * 8;

      char utf8_char[16];
      ace_char_to_utf8(ace_c, font_bmp, utf8_char, sizeof(utf8_char));

      size_t char_len = strlen(utf8_char);
      if (line_len + char_len < sizeof(line_buf) - 1) {
        strcpy(line_buf + line_len, utf8_char);
        line_len += char_len;
      }
    }

    /* Trim trailing spaces from line */
    while (line_len > 0 && line_buf[line_len - 1] == ' ') {
      line_buf[line_len - 1] = '\0';
      line_len--;
    }

    if (buf_len + line_len + 2 >= buf_cap) {
      buf_cap *= 2;
      char *new_buf = realloc(buf, buf_cap);
      if (!new_buf) {
        free(buf);
        return;
      }
      buf = new_buf;
    }

    if (r > start_row) {
      buf[buf_len++] = '\n';
      buf[buf_len] = '\0';
    }

    strcpy(buf + buf_len, line_buf);
    buf_len += line_len;
  }

  if (buf_len > 0) {
    SDL_SetClipboardText(buf);
  }
  free(buf);
}

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

  text = SDL_GetClipboardText();
  if (!text || !*text) {
    if (text) SDL_free(text);
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
    SDL_free(text);
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
  SDL_free(text);
}



/* --------------------------------------------------------------------------
 * Signal / timer helpers
 * -------------------------------------------------------------------------- */

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
 * Emulator key handler (special keys not mapped to Ace keys)
 * -------------------------------------------------------------------------- */
static void emu_key_handler(AceKeySym ks, int key_state) {
  char spool_filename[257];
  char tape_filename[257];

  switch (ks) {
  case SDLK_q:
    /* Ctrl-Q → quit */
    if (key_state & ACE_CTRL_MASK) {
      raise(SIGQUIT);
    }
    break;

  case SDLK_F3:
#ifdef __APPLE__
    macos_show_attach_tape_dialog();
#elif defined(__linux__)
    linux_show_attach_tape_dialog();
#else
    printf("Enter tape image file: ");
    fflush(stdout);
    if (scanf("%256s", tape_filename) == 1)
      tape_attach(tape_filename);
#endif
    break;

  case SDLK_F5:
#ifdef __APPLE__
    macos_show_spool_dialog();
#elif defined(__linux__)
    linux_show_spool_dialog();
#else
    printf("Enter spool file: ");
    fflush(stdout);
    if (scanf("%256s", spool_filename) == 1)
      spooler_open(spool_filename);
#endif
    break;

  case SDLK_F2:
    reset_ace = 1;
    memset(mem + 8192, 0xff, 57344);
    refresh_screen = 1;
    keyboard_clear();
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
  tape_patches(mem);
  memset(mem + 8192, 0xff, 57344);
  memset(video_ram_old, 0xff, 768);

  spooler_init(spooler_observer, keyboard_clear, keyboard_keypress);
  startup();
  setup_sighandlers();
  normal_speed();
  handle_cli_args(argc, argv);
  tape_add_observer(tape_observer);
  keyboard_init(emu_key_handler);
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
    /* Try next to the bundle/executable */
    char path[4096];
    Uint32 size = sizeof(path);
    if (SDL_GetBasePath() != NULL) {
      const char *base = SDL_GetBasePath();
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
    if (sdl_audio_device != 0) {
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
      SDL_QueueAudio(sdl_audio_device, audio_buffer,
                     SAMPLES_PER_FRAME * sizeof(float));
      last_speaker_tstates = 0;
    }

    /* 2. Trigger Z80 50Hz maskable interrupt for screen refresh & timing */
    if (interrupted == 0) {
      interrupted = 1;
    }

    /* 3. Frame pacing: audio queue throttle + high-resolution wall-clock
     * fallback */
    static Uint64 next_frame_counter = 0;
    Uint64 perf_freq = SDL_GetPerformanceFrequency();
    Uint64 ticks_per_frame = perf_freq / 50;
    Uint64 now = SDL_GetPerformanceCounter();

    /* Audio queue throttling: sleep if audio queue exceeds 2 frames (~40ms
     * buffer) */
    if (sdl_audio_device != 0) {
      Uint32 queued_bytes = SDL_GetQueuedAudioSize(sdl_audio_device);
      Uint32 target_bytes = SAMPLES_PER_FRAME * sizeof(float) * 2;
      if (queued_bytes > target_bytes) {
        Uint32 excess_bytes = queued_bytes - target_bytes;
        Uint32 sleep_ms =
            (excess_bytes * 1000) / (AUDIO_SAMPLE_RATE * sizeof(float));
        if (sleep_ms >= 1) {
          SDL_Delay(sleep_ms);
        }
      }
    }

    /* High-resolution wall-clock pacing fallback */
    now = SDL_GetPerformanceCounter();
    if (next_frame_counter == 0 ||
        now > next_frame_counter + ticks_per_frame * 5) {
      next_frame_counter = now + ticks_per_frame;
    } else {
      if (now < next_frame_counter) {
        Uint64 diff = next_frame_counter - now;
        Uint32 sleep_ms = (Uint32)((diff * 1000) / perf_freq);
        if (sleep_ms >= 1) {
          SDL_Delay(sleep_ms);
        }
        while (SDL_GetPerformanceCounter() < next_frame_counter) {
          /* Sub-millisecond spin-wait for exact frame timing */
        }
      }
      next_frame_counter += ticks_per_frame;
    }
  }

  tstates = 0;
}

/* --------------------------------------------------------------------------
 * Interrupt handler (called by Z80 core each instruction)
 * -------------------------------------------------------------------------- */
static int pending_release_key = 0;
static int pending_release_timer = 0;

void do_interrupt(void) {
  static int count = 0;
  if (interrupted == 1) {
    interrupted = 2;

    count++;
    if (count >= scrn_freq) {
      count = 0;
      spooler_read();
      refresh();
    }

    check_events();
    if (pending_release_timer > 0) {
      pending_release_timer--;
      if (pending_release_timer == 0 && pending_release_key != 0) {
        keyboard_keyrelease(pending_release_key, 0);
        pending_release_key = 0;
      }
    }
#ifndef __APPLE__
    linux_pump_events();
#endif
    interrupted = 0;
  }
}

/* --------------------------------------------------------------------------
 * SDL startup
 * -------------------------------------------------------------------------- */
void startup(void) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    exit(1);
  }

  /* Open SDL Audio Device */
  SDL_AudioSpec wanted;
  SDL_zero(wanted);
  wanted.freq = AUDIO_SAMPLE_RATE;
  wanted.format = AUDIO_F32SYS;
  wanted.channels = 1;
  wanted.samples = 1024;
  wanted.callback = NULL;

  sdl_audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
  if (sdl_audio_device != 0) {
    SDL_PauseAudioDevice(sdl_audio_device, 0); /* start playing */
  } else {
    fprintf(stderr, "Warning: SDL_OpenAudioDevice failed: %s\n",
            SDL_GetError());
  }

  /* Register one custom event type for macOS menu actions */
  ace_sdl_event_type = SDL_RegisterEvents(1);

  /* Use linear filtering when scaling the texture */
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

  int win_w = hsize + BORDER_SIDE * 2;
  int win_h = vsize + BORDER_TOP + BORDER_SIDE;

#ifndef __APPLE__
  void *xid = linux_create_window(win_w, win_h, "SDLAce — Jupiter ACE Emulator",
                                  ace_sdl_event_type);
  sdl_window = SDL_CreateWindowFrom(xid);
#else
  sdl_window = SDL_CreateWindow(
      "SDLAce — Jupiter ACE Emulator", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, win_w, win_h,
      SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
#endif

  if (!sdl_window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    exit(1);
  }

  /* Don't allow the window to shrink below half the native ACE resolution */
  SDL_SetWindowMinimumSize(sdl_window, hsize / 2 + BORDER_SIDE,
                           vsize / 2 + BORDER_TOP / 2);

  sdl_renderer = SDL_CreateRenderer(
      sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!sdl_renderer) {
    /* Fall back to software renderer */
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
    if (!sdl_renderer) {
      fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
      exit(1);
    }
  }

  /* Draw border colour */
  SDL_SetRenderDrawColor(sdl_renderer, 255, 255, 255, 255);
  SDL_RenderClear(sdl_renderer);

  /* Inner screen texture (streaming = we update it every frame) */
  sdl_texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, hsize, vsize);
  if (!sdl_texture) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    exit(1);
  }

  pixel_buf = calloc(hsize * vsize, sizeof(Uint32));
  if (!pixel_buf) {
    fprintf(stderr, "Out of memory for pixel buffer\n");
    exit(1);
  }

  /* Resolve black/white in ARGB8888 */
#ifdef WHITE_ON_BLACK
  col_black = 0xFFFFFFFF; /* white pixels  */
  col_white = 0xFF000000; /* black pixels  */
#else
  col_white = 0xFFFFFFFF;
  col_black = 0xFF000000;
#endif

  refresh_screen = 1;

  /* ---- Load bezel logo texture from embedded memory (ace_logo_png) ---- */
  {
    int w, h, channels;
    unsigned char *img_data = stbi_load_from_memory(
        ace_logo_png, (int)ace_logo_png_len, &w, &h, &channels, 4);
    if (img_data) {
      SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
          img_data, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
      if (surface) {
        bezel_logo_texture =
            SDL_CreateTextureFromSurface(sdl_renderer, surface);
        if (bezel_logo_texture) {
          bezel_logo_w = w;
          bezel_logo_h = h;
          /* Enable alpha blending for the logo texture */
          SDL_SetTextureBlendMode(bezel_logo_texture, SDL_BLENDMODE_BLEND);
        }
        SDL_FreeSurface(surface);
      }
      stbi_image_free(img_data);
    }
  }
  if (!bezel_logo_texture) {
    fprintf(stderr, "SDLAce: could not load embedded bezel logo image\n");
  }

#ifdef __APPLE__
  /* Set up the native macOS menu bar (runs on main queue asynchronously) */
  macos_setup_menu(ace_sdl_event_type);
#endif
}

/* --------------------------------------------------------------------------
 * Event processing
 * -------------------------------------------------------------------------- */
void check_events(void) {
  SDL_Event ev;

  while (SDL_PollEvent(&ev)) {
    /* Custom event from macOS menu */
    if (ev.type == ace_sdl_event_type) {
      char *path = (char *)ev.user.data1; /* may be NULL */
      switch (ev.user.code) {
      case ACE_EVENT_DELETE_LINE:
        keyboard_keypress(SDLK_F1, 0);
        pending_release_key = SDLK_F1;
        pending_release_timer = 10;
        break;
      case ACE_EVENT_ATTACH_TAPE:
        if (path) {
          tape_attach(path);
          free(path);
        }
        break;
      case ACE_EVENT_REWIND_TAPE:
        tape_rewind();
        break;
      case ACE_EVENT_INVERSE_VIDEO:
        keyboard_keypress(SDLK_F4, 0);
        pending_release_key = SDLK_F4;
        pending_release_timer = 10;
        break;
      case ACE_EVENT_GRAPHICS:
        keyboard_keypress(SDLK_F9, 0);
        pending_release_key = SDLK_F9;
        pending_release_timer = 10;
        break;
      case ACE_EVENT_SPOOL:
        if (path) {
          spooler_open(path);
          free(path);
        }
        break;
      case ACE_EVENT_RESET:
        reset_ace = 1;
        memset(mem + 8192, 0xff, 57344);
        refresh_screen = 1;
        keyboard_clear();
        break;
      case ACE_EVENT_BREAK:
        keyboard_keypress(SDLK_ESCAPE, 0);
        pending_release_key = SDLK_ESCAPE;
        pending_release_timer = 10;
        break;
      case ACE_EVENT_PASTE:
        paste_from_clipboard();
        break;
      case ACE_EVENT_COPY:
        copy_selection_to_clipboard();
        break;
      }
      continue;
    }

    switch (ev.type) {
    case SDL_QUIT:
      tape_detach();
      closedown();
      exit(0);
      break;

    case SDL_WINDOWEVENT:
      if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
          ev.window.event == SDL_WINDOWEVENT_RESTORED ||
          ev.window.event == SDL_WINDOWEVENT_RESIZED ||
          ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
        refresh_screen = 1;
      break;

    case SDL_KEYDOWN: {
      SDL_Keycode ks = ev.key.keysym.sym;
      int mods = (int)ev.key.keysym.mod;
#ifdef __APPLE__
      /* On macOS, native menus handle all Cmd shortcuts (Cmd-1..9, Cmd-C, Cmd-V, etc).
         Ignore them here to prevent double actions or sending keys to the
         emulator. */
      if (mods & KMOD_GUI) {
        break;
      }
#endif
      /* Ctrl+C (Linux/Windows) — copy selection to host clipboard */
      if (ks == SDLK_c && (mods & KMOD_CTRL)) {
        copy_selection_to_clipboard();
        break;
      }
      /* Ctrl+V (Linux/Windows) — paste from host clipboard */
      if (ks == SDLK_v && (mods & KMOD_CTRL)) {
        paste_from_clipboard();
        break;
      }

      if (!spooler_active())
        keyboard_keypress(ks, mods);
      break;
    }

    case SDL_KEYUP:
      if (!spooler_active()) {
        SDL_Keycode ks = ev.key.keysym.sym;
        int mods = (int)ev.key.keysym.mod;
#ifdef __APPLE__
        if (mods & KMOD_GUI) {
          break;
        }
#endif
        keyboard_keyrelease(ks, mods);
      }
      break;

    case SDL_MOUSEBUTTONDOWN:
#ifndef __APPLE__
      linux_cancel_menu();
#endif
      if (ev.button.button == SDL_BUTTON_LEFT) {
        int c, r;
        if (get_grid_pos(ev.button.x, ev.button.y, &c, &r, 0)) {
          /* Record potential selection anchor point, but do NOT set active selection range until mouse moves */
          selecting_text = 1;
          sel_start_col = c; sel_start_row = r;
          sel_end_col = -1;  sel_end_row = -1;
        } else {
          /* Clicked outside active screen display -> clear selection */
          clear_selection();
        }
      }
      break;

    case SDL_MOUSEMOTION:
      if (selecting_text && (ev.motion.state & SDL_BUTTON_LMASK)) {
        int c, r;
        if (get_grid_pos(ev.motion.x, ev.motion.y, &c, &r, 1)) {
          /* Update selection target end position */
          if (c != sel_end_col || r != sel_end_row) {
            sel_end_col = c;
            sel_end_row = r;
            refresh_screen = 1;
          }
        }
      }
      break;

    case SDL_MOUSEBUTTONUP:
      if (ev.button.button == SDL_BUTTON_LEFT && selecting_text) {
        /* If mouse was never dragged to another cell, clear selection (single click) */
        if (sel_end_col < 0 || sel_end_row < 0 ||
            (sel_start_col == sel_end_col && sel_start_row == sel_end_row)) {
          clear_selection();
        } else {
          selecting_text = 0;
        }
      }
      break;



    default:
      break;
    }
  }
}

/* --------------------------------------------------------------------------
 * Pixel helpers
 * -------------------------------------------------------------------------- */

/* Set a scaled pixel in pixel_buf */
static void set_pixel(int x, int y, Uint32 colour) {
  int sx, sy;
  for (sy = 0; sy < SCALE; sy++)
    for (sx = 0; sx < SCALE; sx++)
      pixel_buf[(y * SCALE + sy) * hsize + (x * SCALE + sx)] = colour;
}

/* Draw one ACE character cell at grid position (x, y) */
static void set_image_character(int x, int y, int inv,
                                unsigned char *charbmap) {
  int charbmap_x, charbmap_y;
  unsigned char charbmap_row;
  unsigned char charbmap_row_mask;

  if (is_cell_selected(x, y)) {
    inv = !inv;
  }

  for (charbmap_y = 0; charbmap_y < 8; charbmap_y++) {
    charbmap_row = charbmap[charbmap_y];
    if (inv)
      charbmap_row ^= 255;

    charbmap_row_mask = 128;
    for (charbmap_x = 0; charbmap_x < 8; charbmap_x++) {
      Uint32 colour =
          (charbmap_row & charbmap_row_mask) ? col_black : col_white;
      set_pixel(x * 8 + charbmap_x, y * 8 + charbmap_y, colour);
      charbmap_row_mask >>= 1;
    }
  }
}

/* --------------------------------------------------------------------------
 * Bezel drawing — faithful recreation of the Jupiter ACE logo design:
 *   • Very light grey background (matches the physical ACE case)
 *   • "Jupiter" in large bold italic, top-left
 *   • "ACE" in medium regular weight, slightly indented below "Jupiter"
 *   • Cascading fan of red horizontal lines: each successive line starts
 *     further right, all ending at the right edge — creating the
 *     characteristic staircase/speed-lines motif from the real hardware.
 *
 * All coordinates are renderer-output pixels (HiDPI-aware).
 * -------------------------------------------------------------------------- */
static void draw_bezel(int win_w, int win_h, int dst_x, int dst_y, int dst_w,
                       int dst_h) {
  /* Palette */
  const Uint8 BG_R = 235, BG_G = 235, BG_B = 237; /* light grey  */
  const Uint8 TX_R = 22, TX_G = 22, TX_B = 28;    /* near-black text */
  const Uint8 LN_R = 195, LN_G = 28, LN_B = 38;   /* Jupiter ACE red */

  /* 1. Fill background */
  SDL_SetRenderDrawColor(sdl_renderer, BG_R, BG_G, BG_B, 255);
  SDL_RenderClear(sdl_renderer);

  /* 2. Subtle double-line inset border around the emulated screen */
  SDL_SetRenderDrawColor(sdl_renderer, 160, 160, 162, 255);
  SDL_Rect r1 = {dst_x - 3, dst_y - 3, dst_w + 6, dst_h + 6};
  SDL_RenderDrawRect(sdl_renderer, &r1);
  /* Inner screen border: 2px dark grey outline around emulated screen */
  SDL_SetRenderDrawColor(sdl_renderer, 60, 58, 55, 255);
  SDL_Rect outline = {dst_x - 2, dst_y - 2, dst_w + 4, dst_h + 4};
  SDL_RenderDrawRect(sdl_renderer, &outline);

  /* Horizontal padding from left: ~4% of window width */
  int pad_l = (int)(win_w * 0.04f);
  if (pad_l < 8)
    pad_l = 8;

  int logo_rendered_w = 0;

  /* Render logo image in top bezel header */
  if (bezel_logo_texture && bezel_logo_w > 0 && bezel_logo_h > 0) {
    /* Scale logo height to fit ~70% of top bezel height (dst_y) */
    int target_h = (int)(dst_y * 0.70f);
    if (target_h < 12)
      target_h = 12;
    int target_w = (int)((float)bezel_logo_w * target_h / bezel_logo_h);

    int logo_y = (dst_y - target_h) / 2;
    SDL_Rect logo_rect = {pad_l, logo_y, target_w, target_h};
    SDL_RenderCopy(sdl_renderer, bezel_logo_texture, NULL, &logo_rect);
  }
}

/* --------------------------------------------------------------------------
 * Screen refresh
 * -------------------------------------------------------------------------- */
void refresh(void) {
  unsigned char *video_ram, *charset;
  int x, y, c, inv;
  int xmin, ymin, xmax, ymax;
  int video_ram_old_ofs;

  charset = mem + 0x2c00;
  video_ram = mem + 0x2400;
  xmin = 31;
  ymin = 23;
  xmax = 0;
  ymax = 0;

  if (video_ram - mem > 0xf000)
    video_ram = mem + 0xf000;

  video_ram_old_ofs = 0;
  for (y = 0; y < 24; y++) {
    for (x = 0; x < 32; x++, video_ram++, video_ram_old_ofs++) {
      c = *video_ram;
      if (c != video_ram_old[video_ram_old_ofs] || refresh_screen) {
        video_ram_old[video_ram_old_ofs] = c;
        if (x < xmin)
          xmin = x;
        if (y < ymin)
          ymin = y;
        if (x > xmax)
          xmax = x;
        if (y > ymax)
          ymax = y;
        inv = c & 128;
        c &= 127;
        set_image_character(x, y, inv, charset + c * 8);
      }
    }
  }

  if (refresh_screen) {
    xmin = 0;
    ymin = 0;
    xmax = 31;
    ymax = 23;
  }

  if (xmax >= xmin && ymax >= ymin) {
    /* Upload pixel_buf to texture */
    SDL_UpdateTexture(sdl_texture, NULL, pixel_buf, hsize * sizeof(Uint32));

    /* Compute layout in renderer output pixels (HiDPI-aware) */
    int win_w, win_h;
    SDL_GetRendererOutputSize(sdl_renderer, &win_w, &win_h);

    /* Scale factor from window logical points to renderer pixels
     * (on Retina this is 2.0, on regular displays 1.0) */
    int log_w, log_h;
    SDL_GetWindowSize(sdl_window, &log_w, &log_h);
    float px_ratio = (float)win_w / (float)log_w;

    /* Border sizes in renderer pixels */
    int bside = (int)(BORDER_SIDE * px_ratio);
    int btop = (int)(BORDER_TOP * px_ratio);

    int avail_w = win_w - bside * 2;
    int avail_h = win_h - btop - bside;

    /* Scale to fill available area, preserving 4:3 ACE aspect ratio */
    float scale_x = (float)avail_w / hsize;
    float scale_y = (float)avail_h / vsize;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;

    int dst_w = (int)(hsize * scale);
    int dst_h = (int)(vsize * scale);
    /* Centre horizontally, push to bottom of top-brand area */
    int dst_x = (win_w - dst_w) / 2;
    int dst_y = btop + (avail_h - dst_h) / 2;

    /* Draw outer case bezel */
    draw_bezel(win_w, win_h, dst_x, dst_y, dst_w, dst_h);

    /* Fill screen aperture with black (acts as display bezel housing) */
    SDL_Rect screen_area = {dst_x, dst_y, dst_w, dst_h};
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(sdl_renderer, &screen_area);

    /* Inset active display area inside the black screen housing */
    int margin = (int)(16 * scale / SCALE);
    if (margin < 8) margin = 8;

    int active_w = dst_w - margin * 2;
    int active_h = dst_h - margin * 2;
    int active_x = dst_x + margin;
    int active_y = dst_y + margin;

    SDL_Rect dst = {active_x, active_y, active_w, active_h};
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, &dst);

    SDL_RenderPresent(sdl_renderer);
  }

  refresh_screen = 0;
}

/* --------------------------------------------------------------------------
 * Closedown
 * -------------------------------------------------------------------------- */
void closedown(void) {
  if (sdl_audio_device) {
    SDL_CloseAudioDevice(sdl_audio_device);
    sdl_audio_device = 0;
  }
  tape_clear_observers();
  free(pixel_buf);
  if (bezel_logo_texture) {
    SDL_DestroyTexture(bezel_logo_texture);
    bezel_logo_texture = NULL;
  }
  if (sdl_texture)
    SDL_DestroyTexture(sdl_texture);
  if (sdl_renderer)
    SDL_DestroyRenderer(sdl_renderer);
  if (sdl_window)
    SDL_DestroyWindow(sdl_window);
  SDL_Quit();
}
