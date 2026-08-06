/* sdlmain.c — SDL2 backend for xAce (Jupiter ACE emulator)
 *
 * Copyright (C) 1994 Ian Collier.
 * xz81 changes (C) 1995-6 Russell Marks.
 * xace changes (C) 1997 Edward Patel.
 * xace changes (C) 2010-12 Lawrence Woodman.
 * SDL2 macOS port (C) 2026
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

#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "z80.h"
#include "tape.h"
#include "keyboard.h"
#include "spooler.h"

#define BORDER_WIDTH  (20 * SCALE)

/* --------------------------------------------------------------------------
 * Global state shared with the Z80 core
 * -------------------------------------------------------------------------- */
int rrnoshm = 4;
unsigned char mem[65536];
unsigned char *memptr[8] = {
  mem,
  mem + 0x2000,
  mem + 0x4000,
  mem + 0x6000,
  mem + 0x8000,
  mem + 0xa000,
  mem + 0xc000,
  mem + 0xe000
};

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
static SDL_Window   *sdl_window   = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture  *sdl_texture  = NULL;

/* Pixel buffer: 32-bit ARGB, hsize × vsize */
static Uint32 *pixel_buf = NULL;

static Uint32 col_black = 0;
static Uint32 col_white = 0;

static int invert = 0;

/* --------------------------------------------------------------------------
 * Prototypes
 * -------------------------------------------------------------------------- */
void loadrom(unsigned char *x);
void startup(void);
void check_events(void);
void refresh(void);
void closedown(void);

/* --------------------------------------------------------------------------
 * Signal / timer helpers
 * -------------------------------------------------------------------------- */

/* Handle the SIGALRM signal used for the Ace's interrupt */
static void
sigint_handler(int signum)
{
  (void)signum;
  if (interrupted == 0) interrupted = 1;
}

/* Handle any signals to do with quitting the program */
static void
sigquit_handler(int signum)
{
  (void)signum;
  tape_detach();
  closedown();
  exit(1);
}

/* ints_per_sec — interrupts per second, up to 1000 */
static void
set_itimer(int ints_per_sec)
{
  struct itimerval itv;
  int freq = 1000 / ints_per_sec;

  itv.it_interval.tv_sec  = 0;
  itv.it_interval.tv_usec = (freq % 1000) * 1000;
  itv.it_value.tv_sec     = itv.it_interval.tv_sec;
  itv.it_value.tv_usec    = itv.it_interval.tv_usec;
  setitimer(ITIMER_REAL, &itv, NULL);
}

static void
normal_speed(void)
{
  set_itimer(50);   /* 50 ints/sec */
  scrn_freq = 4;
  tsmax = 62500;
}

static void
fast_speed(void)
{
  set_itimer(1000); /* 1000 ints/sec */
  scrn_freq = 4;
  tsmax = ULONG_MAX;
}

/* --------------------------------------------------------------------------
 * Observer callbacks
 * -------------------------------------------------------------------------- */
static void
tape_observer(int tape_attached, int tape_pos,
  const char tape_filename[TAPE_MAX_FILENAME_SIZE],
  TapeMessageType message_type, const char message[TAPE_MAX_MESSAGE_SIZE])
{
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
        fprintf(stderr, "TAPE: %s Pos: %04d - Error: %s\n",
                tape_filename, tape_pos, message);
      else
        fprintf(stderr, "TAPE: empty tape Pos: %04d - Error: %s\n",
                tape_pos, message);
      break;
  }
}

static void
spooler_observer(SpoolerMessage message)
{
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
  }
}

/* --------------------------------------------------------------------------
 * Emulator key handler (special keys not mapped to Ace keys)
 * -------------------------------------------------------------------------- */
static void
emu_key_handler(AceKeySym ks, int key_state)
{
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
      printf("Enter tape image file: ");
      fflush(stdout);
      if (scanf("%256s", tape_filename) == 1)
        tape_attach(tape_filename);
      break;

    case SDLK_F11:
      printf("Enter spool file: ");
      fflush(stdout);
      if (scanf("%256s", spool_filename) == 1)
        spooler_open(spool_filename);
      break;

    case SDLK_F12:
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
static void
handle_cli_args(int argc, char **argv)
{
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
    }
    arg_pos++;
  }
}

/* --------------------------------------------------------------------------
 * Signal handlers
 * -------------------------------------------------------------------------- */
static void
setup_sighandlers(void)
{
  struct sigaction sa_quit;
  struct sigaction sa_alarm;
  memset(&sa_quit,  0, sizeof(sa_quit));
  memset(&sa_alarm, 0, sizeof(sa_alarm));

  sa_quit.sa_handler  = sigquit_handler;
  sa_quit.sa_flags    = 0;
  sa_alarm.sa_handler = sigint_handler;
  sa_alarm.sa_flags   = SA_RESTART;

  if (sigaction(SIGINT,  &sa_quit,  NULL) < 0) goto error;
  if (sigaction(SIGHUP,  &sa_quit,  NULL) < 0) goto error;
  if (sigaction(SIGILL,  &sa_quit,  NULL) < 0) goto error;
  if (sigaction(SIGTERM, &sa_quit,  NULL) < 0) goto error;
  if (sigaction(SIGQUIT, &sa_quit,  NULL) < 0) goto error;
  if (sigaction(SIGSEGV, &sa_quit,  NULL) < 0) goto error;
  if (sigaction(SIGALRM, &sa_alarm, NULL) < 0) goto error;
  return;

error:
  perror("sigaction failed");
  exit(1);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
  printf("xace: Jupiter ACE emulator v%s (by Edward Patel)\n", XACE_VERSION);
  printf("Keys:\n");
  printf("\tF1     - Delete Line\n");
  printf("\tF3     - Attach a tape image\n");
  printf("\tF4     - Inverse Video\n");
  printf("\tF9     - Graphics\n");
  printf("\tF11    - Spool from a file\n");
  printf("\tF12    - Reset\n");
  printf("\tEsc    - Break\n");
  printf("\tCtrl-Q - Quit xAce\n");

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
void
loadrom(unsigned char *x)
{
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
unsigned int
in(int h, int l)
{
  if (l == 0xfe) /* keyboard */
    switch (h) {
      case 0xfe: return keyboard_get_keyport(0);
      case 0xfd: return keyboard_get_keyport(1);
      case 0xfb: return keyboard_get_keyport(2);
      case 0xf7: return keyboard_get_keyport(3);
      case 0xef: return keyboard_get_keyport(4);
      case 0xdf: return keyboard_get_keyport(5);
      case 0xbf: return keyboard_get_keyport(6);
      case 0x7f: return keyboard_get_keyport(7);
      default:   return 255;
    }
  return 255;
}

unsigned int
out(int h, int l, int a)
{
  (void)h; (void)l; (void)a;
  return 0;
}

/* --------------------------------------------------------------------------
 * Timing helpers (called by Z80 core)
 * -------------------------------------------------------------------------- */
void
fix_tstates(void)
{
  tstates = 0;
  pause();
}

/* --------------------------------------------------------------------------
 * Interrupt handler (called by Z80 core each instruction)
 * -------------------------------------------------------------------------- */
void
do_interrupt(void)
{
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
    interrupted = 0;
  }
}

/* --------------------------------------------------------------------------
 * SDL startup
 * -------------------------------------------------------------------------- */
void
startup(void)
{
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    exit(1);
  }

  int win_w = hsize + BORDER_WIDTH * 2;
  int win_h = vsize + BORDER_WIDTH * 2;

  sdl_window = SDL_CreateWindow(
    "xAce — Jupiter ACE Emulator",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    win_w, win_h,
    SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
  );
  if (!sdl_window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    exit(1);
  }

  sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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
  sdl_texture = SDL_CreateTexture(
    sdl_renderer,
    SDL_PIXELFORMAT_ARGB8888,
    SDL_TEXTUREACCESS_STREAMING,
    hsize, vsize
  );
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

  /* Disable key repeat — we handle it ourselves */
  /* (SDL2 has no XAutoRepeatOff equivalent; key repeat is from
     repeated SDL_KEYDOWN events which we simply don't accumulate.) */

  refresh_screen = 1;
}

/* --------------------------------------------------------------------------
 * Event processing
 * -------------------------------------------------------------------------- */
void
check_events(void)
{
  SDL_Event ev;

  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_QUIT:
        tape_detach();
        closedown();
        exit(0);
        break;

      case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
            ev.window.event == SDL_WINDOWEVENT_RESTORED)
          refresh_screen = 1;
        break;

      case SDL_KEYDOWN:
        if (!spooler_active()) {
          SDL_Keycode ks    = ev.key.keysym.sym;
          int         mods  = (int)ev.key.keysym.mod;
          keyboard_keypress(ks, mods);
        }
        break;

      case SDL_KEYUP:
        if (!spooler_active()) {
          SDL_Keycode ks   = ev.key.keysym.sym;
          int         mods = (int)ev.key.keysym.mod;
          keyboard_keyrelease(ks, mods);
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
static void
set_pixel(int x, int y, Uint32 colour)
{
  int sx, sy;
  for (sy = 0; sy < SCALE; sy++)
    for (sx = 0; sx < SCALE; sx++)
      pixel_buf[(y * SCALE + sy) * hsize + (x * SCALE + sx)] = colour;
}

/* Draw one ACE character cell at grid position (x, y) */
static void
set_image_character(int x, int y, int inv, unsigned char *charbmap)
{
  int charbmap_x, charbmap_y;
  unsigned char charbmap_row;
  unsigned char charbmap_row_mask;

  for (charbmap_y = 0; charbmap_y < 8; charbmap_y++) {
    charbmap_row = charbmap[charbmap_y];
    if (inv) charbmap_row ^= 255;

    charbmap_row_mask = 128;
    for (charbmap_x = 0; charbmap_x < 8; charbmap_x++) {
      Uint32 colour = (charbmap_row & charbmap_row_mask) ? col_black : col_white;
      set_pixel(x * 8 + charbmap_x, y * 8 + charbmap_y, colour);
      charbmap_row_mask >>= 1;
    }
  }
}

/* --------------------------------------------------------------------------
 * Screen refresh
 * -------------------------------------------------------------------------- */
void
refresh(void)
{
  unsigned char *video_ram, *charset;
  int x, y, c, inv;
  int xmin, ymin, xmax, ymax;
  int video_ram_old_ofs;

  charset   = mem + 0x2c00;
  video_ram = mem + 0x2400;
  xmin = 31; ymin = 23; xmax = 0; ymax = 0;

  if (video_ram - mem > 0xf000) video_ram = mem + 0xf000;

  video_ram_old_ofs = 0;
  for (y = 0; y < 24; y++) {
    for (x = 0; x < 32; x++, video_ram++, video_ram_old_ofs++) {
      c = *video_ram;
      if (c != video_ram_old[video_ram_old_ofs] || refresh_screen) {
        video_ram_old[video_ram_old_ofs] = c;
        if (x < xmin) xmin = x;
        if (y < ymin) ymin = y;
        if (x > xmax) xmax = x;
        if (y > ymax) ymax = y;
        inv = c & 128;
        c  &= 127;
        set_image_character(x, y, inv, charset + c * 8);
      }
    }
  }

  if (refresh_screen) {
    xmin = 0; ymin = 0; xmax = 31; ymax = 23;
  }

  if (xmax >= xmin && ymax >= ymin) {
    /* Upload changed region of pixel_buf to the texture */
    SDL_UpdateTexture(sdl_texture, NULL, pixel_buf, hsize * sizeof(Uint32));

    /* Render: white border + inner screen */
    SDL_SetRenderDrawColor(sdl_renderer, 255, 255, 255, 255);
    SDL_RenderClear(sdl_renderer);

    SDL_Rect dst = {
      BORDER_WIDTH, BORDER_WIDTH,
      hsize, vsize
    };
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, &dst);
    SDL_RenderPresent(sdl_renderer);
  }

  refresh_screen = 0;
}

/* --------------------------------------------------------------------------
 * Closedown
 * -------------------------------------------------------------------------- */
void
closedown(void)
{
  tape_clear_observers();
  free(pixel_buf);
  if (sdl_texture)  SDL_DestroyTexture(sdl_texture);
  if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
  if (sdl_window)   SDL_DestroyWindow(sdl_window);
  SDL_Quit();
}
