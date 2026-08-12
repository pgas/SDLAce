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
#include <SDL2/SDL_ttf.h>

#include "z80.h"
#include "tape.h"
#include "keyboard.h"
#include "spooler.h"
#include "ui_events.h"
#ifdef __APPLE__
#  include "macos_ui.h"
#else
#  include "linux_ui.h"
#endif

/* Top border is taller to fit the "JUPITER ACE" brand text + red stripes.
 * The value here is at SCALE=2 (native 1× = half); we scale proportionally. */
#define BORDER_SIDE   (16 * SCALE)   /* left/right/bottom border width */
#define BORDER_TOP    (56 * SCALE)   /* top border (brand area) */

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

/* Bezel fonts (SDL_ttf) — sizes scaled for HiDPI in startup() */
static TTF_Font *bezel_font_jupiter = NULL;  /* "Jupiter" large bold italic */
static TTF_Font *bezel_font_ace     = NULL;  /* "ACE" medium regular */
static TTF_Font *bezel_font_small   = NULL;  /* version label */

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
#define CYCLES_PER_FRAME  62500
#define SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE / 50) /* 882 */

static int speaker_diaphragm_pos = 0; /* 0 or 1 */
static unsigned long last_speaker_tstates = 0;
static float audio_buffer[SAMPLES_PER_FRAME];
static float dc_blocker_prev_x = 0.0f;
static float dc_blocker_prev_y = 0.0f;

static void
set_speaker_diaphragm(int pos)
{
  if (tsmax != 62500 || sdl_audio_device == 0) {
    speaker_diaphragm_pos = pos;
    return;
  }

  unsigned long current_t = tstates;
  if (current_t < last_speaker_tstates) {
    last_speaker_tstates = 0;
  }
  if (current_t > 62500) current_t = 62500;

  unsigned int last_s = (last_speaker_tstates * SAMPLES_PER_FRAME) / 62500;
  unsigned int current_s = (current_t * SAMPLES_PER_FRAME) / 62500;

  if (current_s > SAMPLES_PER_FRAME) current_s = SAMPLES_PER_FRAME;

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

/* --------------------------------------------------------------------------
 * Paste from clipboard — writes clipboard text to a temp file, then feeds
 * it through the existing spooler (press/release timing is handled there).
 * The temp file is unlink'd immediately after open; the inode stays alive
 * until the spooler fclose's it (standard POSIX behaviour).
 * -------------------------------------------------------------------------- */
static void
paste_from_clipboard(void)
{
  char *text;
  char tmppath[256];
  FILE *f;

  if (spooler_active()) return;  /* already spooling */

  text = SDL_GetClipboardText();
  if (!text || !*text) {
    if (text) SDL_free(text);
    return;
  }

  snprintf(tmppath, sizeof(tmppath), "/tmp/.xace_paste_%d", (int)getpid());
  f = fopen(tmppath, "w");
  if (f) {
    fputs(text, f);
    fclose(f);
    spooler_open(tmppath);
    unlink(tmppath);   /* safe: spooler holds the fd */
  }
  SDL_free(text);
}

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
    default:
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
  if ((l & 1) == 0) {
    set_speaker_diaphragm(0);
  }
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
  if ((l & 1) == 0) {
    set_speaker_diaphragm(1);
  }
  (void)h; (void)a;
  return 0;
}

/* --------------------------------------------------------------------------
 * Timing helpers (called by Z80 core)
 * -------------------------------------------------------------------------- */
void
fix_tstates(void)
{
  if (tsmax == 62500 && sdl_audio_device != 0) {
    unsigned int last_s = (last_speaker_tstates * SAMPLES_PER_FRAME) / 62500;
    if (last_s > SAMPLES_PER_FRAME) last_s = SAMPLES_PER_FRAME;
    float val = speaker_diaphragm_pos ? 0.15f : -0.15f;
    for (unsigned int i = last_s; i < SAMPLES_PER_FRAME; i++) {
      audio_buffer[i] = val;
    }

    // Apply DC blocker filter to eliminate pops/crackles when idle or during underflows
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

    Uint32 queued_bytes = SDL_GetQueuedAudioSize(sdl_audio_device);
    if (queued_bytes < SAMPLES_PER_FRAME * sizeof(float) * 3) {
      SDL_QueueAudio(sdl_audio_device, audio_buffer, SAMPLES_PER_FRAME * sizeof(float));
    }

    last_speaker_tstates = 0;
  }

  tstates = 0;
  pause();
}

/* --------------------------------------------------------------------------
 * Interrupt handler (called by Z80 core each instruction)
 * -------------------------------------------------------------------------- */
static int pending_release_key = 0;
static int pending_release_timer = 0;

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
void
startup(void)
{
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
    fprintf(stderr, "Warning: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
  }

  /* Register one custom event type for macOS menu actions */
  ace_sdl_event_type = SDL_RegisterEvents(1);

  /* Use linear filtering when scaling the texture */
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

  int win_w = hsize + BORDER_SIDE * 2;
  int win_h = vsize + BORDER_TOP + BORDER_SIDE;

#ifndef __APPLE__
  void* xid = linux_create_window(win_w, win_h, "SDLAce — Jupiter ACE Emulator", ace_sdl_event_type);
  sdl_window = SDL_CreateWindowFrom(xid);
#else
  sdl_window = SDL_CreateWindow(
    "SDLAce — Jupiter ACE Emulator",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    win_w, win_h,
    SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE
  );
#endif

  if (!sdl_window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    exit(1);
  }

  /* Don't allow the window to shrink below half the native ACE resolution */
  SDL_SetWindowMinimumSize(sdl_window, hsize / 2 + BORDER_SIDE, vsize / 2 + BORDER_TOP / 2);

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

  refresh_screen = 1;

  /* ---- SDL_ttf for bezel text ---- */
  if (TTF_Init() != 0) {
    fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
  } else {
    /* HiDPI pixel ratio: TTF_OpenFont uses physical pixels, but we render
     * into renderer-output space which is 2× on Retina.  Scale font pt
     * sizes by this ratio so text appears at the right visual size. */
    int log_w_tmp, log_h_tmp, ren_w_tmp;
    SDL_GetWindowSize(sdl_window, &log_w_tmp, &log_h_tmp);
    SDL_GetRendererOutputSize(sdl_renderer, &ren_w_tmp, &log_h_tmp);
    float px = (log_w_tmp > 0) ? (float)ren_w_tmp / log_w_tmp : 1.0f;

    /* Font sizes: "Jupiter" 34pt, "ACE" 19pt, version 10pt */
    int pt_jupiter = (int)(34 * px);
    int pt_ace     = (int)(19 * px);
    int pt_small   = (int)(10 * px);
    if (pt_small < 8) pt_small = 8;

    const char *font_candidates[] = {
#ifdef __APPLE__
      "/System/Library/Fonts/HelveticaNeue.ttc",
      "/System/Library/Fonts/Helvetica.ttc",
      "/System/Library/Fonts/ArialHB.ttc",
#endif
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      NULL
    };
    int i;
    for (i = 0; font_candidates[i] && !bezel_font_jupiter; i++) {
      bezel_font_jupiter = TTF_OpenFont(font_candidates[i], pt_jupiter);
      if (bezel_font_jupiter) {
        /* Apply bold+italic style to "Jupiter" */
        TTF_SetFontStyle(bezel_font_jupiter, TTF_STYLE_BOLD | TTF_STYLE_ITALIC);
        bezel_font_ace   = TTF_OpenFont(font_candidates[i], pt_ace);
        bezel_font_small = TTF_OpenFont(font_candidates[i], pt_small);
      }
    }
    if (!bezel_font_jupiter)
      fprintf(stderr, "SDLAce: no bezel font found\n");
  }

#ifdef __APPLE__
  /* Set up the native macOS menu bar (runs on main queue asynchronously) */
  macos_setup_menu(ace_sdl_event_type);
#endif
}

/* --------------------------------------------------------------------------
 * Event processing
 * -------------------------------------------------------------------------- */
void
check_events(void)
{
  SDL_Event ev;

  while (SDL_PollEvent(&ev)) {
    /* Custom event from macOS menu */
    if (ev.type == ace_sdl_event_type) {
      char *path = (char *)ev.user.data1;  /* may be NULL */
      switch (ev.user.code) {
        case ACE_EVENT_DELETE_LINE:
          keyboard_keypress(SDLK_F1, 0);
          pending_release_key = SDLK_F1;
          pending_release_timer = 10;
          break;
        case ACE_EVENT_ATTACH_TAPE:
          if (path) { tape_attach(path); free(path); }
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
          if (path) { spooler_open(path); free(path); }
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
        SDL_Keycode ks   = ev.key.keysym.sym;
        int         mods = (int)ev.key.keysym.mod;
#ifdef __APPLE__
        /* On macOS, native menus handle all Cmd shortcuts (Cmd-1..9, Cmd-V, etc).
           Ignore them here to prevent double actions or sending keys to the emulator. */
        if (mods & KMOD_GUI) {
          break;
        }
#endif
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
          SDL_Keycode ks   = ev.key.keysym.sym;
          int         mods = (int)ev.key.keysym.mod;
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
static void
draw_bezel(int win_w, int win_h, int dst_x, int dst_y, int dst_w, int dst_h)
{
  /* Palette */
  const Uint8 BG_R = 235, BG_G = 235, BG_B = 237; /* light grey  */
  const Uint8 TX_R =  22, TX_G =  22, TX_B =  28; /* near-black text */
  const Uint8 LN_R = 195, LN_G =  28, LN_B =  38; /* Jupiter ACE red */

  /* 1. Fill background */
  SDL_SetRenderDrawColor(sdl_renderer, BG_R, BG_G, BG_B, 255);
  SDL_RenderClear(sdl_renderer);

  /* 2. Subtle double-line inset border around the emulated screen */
  SDL_SetRenderDrawColor(sdl_renderer, 160, 160, 162, 255);
  SDL_Rect r1 = { dst_x - 3, dst_y - 3, dst_w + 6, dst_h + 6 };
  SDL_RenderDrawRect(sdl_renderer, &r1);
  SDL_SetRenderDrawColor(sdl_renderer, TX_R, TX_G, TX_B, 255);
  SDL_Rect r2 = { dst_x - 1, dst_y - 1, dst_w + 2, dst_h + 2 };
  SDL_RenderDrawRect(sdl_renderer, &r2);

  /* 3. Text: "Jupiter" + "ACE" ----------------------------------------- */
  /* Layout: text sits in the top brand area (y=0 .. dst_y).              */
  /* Horizontal padding from left: ~4% of window width.                    */
  int pad_l = (int)(win_w * 0.04f);
  if (pad_l < 8) pad_l = 8;

  /* Vertical centre of the top brand area */
  int brand_mid = dst_y / 2;

  int jupiter_w = 0, jupiter_h = 0;  /* rendered size of "Jupiter" */
  int ace_w     = 0, ace_h     = 0;  /* rendered size of "ACE"     */

  SDL_Color col_text = { TX_R, TX_G, TX_B, 255 };

  /* Measure/render "Jupiter" */
  if (bezel_font_jupiter) {
    TTF_SizeUTF8(bezel_font_jupiter, "Jupiter", &jupiter_w, &jupiter_h);
    /* Position: vertically centred but shifted up so ACE fits below */
    int jup_y = brand_mid - (int)(jupiter_h * 0.65f);
    SDL_Surface *s = TTF_RenderUTF8_Blended(bezel_font_jupiter, "Jupiter", col_text);
    if (s) {
      SDL_Texture *t = SDL_CreateTextureFromSurface(sdl_renderer, s);
      if (t) {
        SDL_Rect tr = { pad_l, jup_y, s->w, s->h };
        SDL_RenderCopy(sdl_renderer, t, NULL, &tr);
        SDL_DestroyTexture(t);
      }
      SDL_FreeSurface(s);
    }
  }

  /* Measure/render "ACE" — slightly indented, immediately below Jupiter */
  if (bezel_font_ace) {
    TTF_SizeUTF8(bezel_font_ace, "ACE", &ace_w, &ace_h);
    int jup_y    = brand_mid - (int)(jupiter_h * 0.65f);
    int ace_x    = pad_l + (int)(jupiter_w * 0.15f);  /* indent */
    int ace_y    = jup_y + (int)(jupiter_h * 0.80f);  /* overlap slightly */
    SDL_Surface *s = TTF_RenderUTF8_Blended(bezel_font_ace, "ACE", col_text);
    if (s) {
      SDL_Texture *t = SDL_CreateTextureFromSurface(sdl_renderer, s);
      if (t) {
        SDL_Rect tr = { ace_x, ace_y, s->w, s->h };
        SDL_RenderCopy(sdl_renderer, t, NULL, &tr);
        SDL_DestroyTexture(t);
      }
      SDL_FreeSurface(s);
    }
  }

  /* 4. Cascading red lines --------------------------------------------- */
  /* The lines form a staircase fan:
   *   - N_LINES lines total, all ending at the right edge of the window
   *   - Each successive line's LEFT endpoint is shifted right by cascade_step
   *   - This creates the speed-lines / fan motif from the real ACE logo
   * Vertical span: the full top brand area (y=0 .. dst_y)
   * First line starts where the text ends (x ≈ text_right_edge + gap)   */
  int n_lines = 9;

  /* Starting x: right edge of the text block + small gap */
  int text_right = pad_l + (jupiter_w > 0 ? jupiter_w : (int)(win_w * 0.35f));
  int line_x0    = text_right + (int)(win_w * 0.025f);
  int line_x1    = win_w;     /* all lines end here */

  /* Cascade: last line starts cascade_total px further right than first */
  float cascade_total = (float)(line_x1 - line_x0) * 0.55f;
  float cascade_step  = (n_lines > 1) ? cascade_total / (n_lines - 1) : 0;

  /* Vertical spacing: lines fill ~85% of the top brand height */
  float line_span = dst_y * 0.85f;
  float line_step = (n_lines > 1) ? line_span / (n_lines - 1) : line_span;
  float line_y0   = dst_y * 0.07f;  /* first line near top */

  /* Line thickness: 1px at small windows, 2px at larger ones */
  int lthick = (win_h > 600) ? 2 : 1;

  SDL_SetRenderDrawColor(sdl_renderer, LN_R, LN_G, LN_B, 255);

  int i;
  for (i = 0; i < n_lines; i++) {
    int lx = (int)(line_x0 + i * cascade_step);
    int ly = (int)(line_y0 + i * line_step);
    int t;
    if (lx >= line_x1) continue;   /* don't draw zero-length lines */
    for (t = 0; t < lthick; t++)
      SDL_RenderDrawLine(sdl_renderer, lx, ly + t, line_x1, ly + t);
  }

  /* 5. Tiny "SDLAce" label in the bottom border — right-aligned, muted */
  if (bezel_font_small) {
    SDL_Color col_dim = { 160, 158, 155, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Blended(bezel_font_small,
                                            "SDLAce v" XACE_VERSION, col_dim);
    if (s) {
      SDL_Texture *t = SDL_CreateTextureFromSurface(sdl_renderer, s);
      if (t) {
        int bot_mid = dst_y + dst_h + (win_h - dst_y - dst_h) / 2;
        SDL_Rect tr = {
          win_w - s->w - pad_l,
          bot_mid - s->h / 2,
          s->w, s->h
        };
        SDL_RenderCopy(sdl_renderer, t, NULL, &tr);
        SDL_DestroyTexture(t);
      }
      SDL_FreeSurface(s);
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
    int btop  = (int)(BORDER_TOP  * px_ratio);

    int avail_w = win_w - bside * 2;
    int avail_h = win_h - btop - bside;

    /* Scale to fill available area, preserving 4:3 ACE aspect ratio */
    float scale_x = (float)avail_w / hsize;
    float scale_y = (float)avail_h / vsize;
    float scale   = (scale_x < scale_y) ? scale_x : scale_y;

    int dst_w = (int)(hsize * scale);
    int dst_h = (int)(vsize * scale);
    /* Centre horizontally, push to bottom of top-brand area */
    int dst_x = (win_w - dst_w) / 2;
    int dst_y = btop + (avail_h - dst_h) / 2;

    SDL_Rect dst = { dst_x, dst_y, dst_w, dst_h };

    /* Draw the Jupiter ACE bezel (cream + red stripes + text) */
    draw_bezel(win_w, win_h, dst_x, dst_y, dst_w, dst_h);

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
  if (sdl_audio_device) {
    SDL_CloseAudioDevice(sdl_audio_device);
    sdl_audio_device = 0;
  }
  tape_clear_observers();
  free(pixel_buf);
  if (bezel_font_jupiter) TTF_CloseFont(bezel_font_jupiter);
  if (bezel_font_ace)     TTF_CloseFont(bezel_font_ace);
  if (bezel_font_small)   TTF_CloseFont(bezel_font_small);
  TTF_Quit();
  if (sdl_texture)  SDL_DestroyTexture(sdl_texture);
  if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
  if (sdl_window)   SDL_DestroyWindow(sdl_window);
  SDL_Quit();
}
