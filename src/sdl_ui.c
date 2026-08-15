#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <SDL2/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "ace_logo_data.h"
#include "stb_image.h"

#include "keyboard.h"
#include "ui.h"
#include "ui_events.h"
#include "tape.h"    /* for TAPE_MAX_FILENAME_SIZE etc */
#include "spooler.h" /* for spooler_active */

#ifdef __APPLE__
#include "macos_ui.h"
#else
#include "linux_ui.h"
#endif

/* Global dimensions for the UI */
static int hsize = 256 * SCALE;
static int vsize = 192 * SCALE;
#define BORDER_SIDE (16 * SCALE)
#define BORDER_TOP (56 * SCALE)

static SDL_Window *sdl_window = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture *sdl_texture = NULL;
static SDL_Texture *bezel_logo_texture = NULL;
static int bezel_logo_w = 0;
static int bezel_logo_h = 0;

static Uint32 *pixel_buf = NULL;
static Uint32 col_black = 0;
static Uint32 col_white = 0;

static SDL_AudioDeviceID sdl_audio_device = 0;
static Uint32 ace_sdl_event_type = (Uint32)-1;

static unsigned char video_ram_old[24 * 32];
static int refresh_screen_flag = 1;

/* Selection state */
static int selecting_text = 0;
static int sel_start_col = -1, sel_start_row = -1;
static int sel_end_col = -1, sel_end_row = -1;

static int is_cell_selected(int col, int row) {
  if (sel_start_col < 0 || sel_start_row < 0 || sel_end_col < 0 || sel_end_row < 0) return 0;
  int idx = row * 32 + col;
  int start_idx = sel_start_row * 32 + sel_start_col;
  int end_idx = sel_end_row * 32 + sel_end_col;
  if (start_idx > end_idx) {
    int tmp = start_idx; start_idx = end_idx; end_idx = tmp;
  }
  return (idx >= start_idx && idx <= end_idx);
}

static void clear_selection(void) {
  if (sel_start_col >= 0 || sel_start_row >= 0 || sel_end_col >= 0 || sel_end_row >= 0 || selecting_text) {
    sel_start_col = sel_start_row = sel_end_col = sel_end_row = -1;
    selecting_text = 0;
    refresh_screen_flag = 1;
  }
}

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
    if (!clamp_bounds) return 0;
  }

  if (win_x < active_x) win_x = active_x;
  if (win_x >= active_x + active_w) win_x = active_x + active_w - 1;
  if (win_y < active_y) win_y = active_y;
  if (win_y >= active_y + active_h) win_y = active_y + active_h - 1;

  float rel_x = (float)(win_x - active_x) / active_w;
  float rel_y = (float)(win_y - active_y) / active_h;

  int c = (int)(rel_x * 32);
  int r = (int)(rel_y * 24);

  if (c < 0) c = 0; if (c > 31) c = 31;
  if (r < 0) r = 0; if (r > 23) r = 23;
  *col = c; *row = r;
  return 1;
}

static void ace_char_to_utf8(unsigned char c, const unsigned char *font_bitmap, char *out_buf, size_t out_buf_size) {
  int is_inv = (c & 128) ? 1 : 0;
  unsigned char code = c & 127;
  if (code >= 32 && code <= 126) {
    out_buf[0] = (char)code;
    out_buf[1] = '\0';
    return;
  }
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
    quad_tl = !quad_tl; quad_tr = !quad_tr; quad_bl = !quad_bl; quad_br = !quad_br;
  }
  int mask = (quad_tl << 3) | (quad_tr << 2) | (quad_bl << 1) | quad_br;
  const char *utf8_block = " ";
  switch (mask) {
    case 0x0: utf8_block = " "; break;
    case 0x1: utf8_block = "\xe2\x96\x97"; break;
    case 0x2: utf8_block = "\xe2\x96\x96"; break;
    case 0x3: utf8_block = "\xe2\x96\x84"; break;
    case 0x4: utf8_block = "\xe2\x96\x9d"; break;
    case 0x5: utf8_block = "\xe2\x96\x90"; break;
    case 0x6: utf8_block = "\xe2\x96\x9a"; break;
    case 0x7: utf8_block = "\xe2\x96\x97"; break;
    case 0x8: utf8_block = "\xe2\x96\x98"; break;
    case 0x9: utf8_block = "\xe2\x96\x9e"; break;
    case 0xA: utf8_block = "\xe2\x96\x8c"; break;
    case 0xB: utf8_block = "\xe2\x96\x99"; break;
    case 0xC: utf8_block = "\xe2\x96\x80"; break;
    case 0xD: utf8_block = "\xe2\x96\x9c"; break;
    case 0xE: utf8_block = "\xe2\x96\x9b"; break;
    case 0xF: utf8_block = "\xe2\x96\x88"; break;
  }
  snprintf(out_buf, out_buf_size, "%s", utf8_block);
}

/* We don't have access to global mem here directly anymore, so copy_selection needs the active screen buffer.
   We can cache it from ui_refresh, or let the core pass it when requested via an event.
   For now, we can just cache the last seen video_ram/charset in ui_refresh for clipboard use. */
static const unsigned char *last_video_ram = NULL;
static const unsigned char *last_charset = NULL;

static void copy_selection_to_clipboard(void) {
  if (!last_video_ram || !last_charset) return;
  int start_col = sel_start_col, start_row = sel_start_row;
  int end_col = sel_end_col, end_row = sel_end_row;

  if (start_col < 0 || start_row < 0 || end_col < 0 || end_row < 0) {
    start_col = 0; start_row = 0; end_col = 31; end_row = 23;
  }
  int start_idx = start_row * 32 + start_col;
  int end_idx = end_row * 32 + end_col;
  if (start_idx > end_idx) {
    int tmp_r = start_row, tmp_c = start_col;
    start_row = end_row; start_col = end_col;
    end_row = tmp_r; end_col = tmp_c;
  }

  size_t buf_cap = 4096;
  char *buf = malloc(buf_cap);
  if (!buf) return;
  buf[0] = '\0';
  size_t buf_len = 0;

  for (int r = start_row; r <= end_row; r++) {
    int col_from = (r == start_row) ? start_col : 0;
    int col_to = (r == end_row) ? end_col : 31;
    char line_buf[256] = "";
    size_t line_len = 0;
    for (int c = col_from; c <= col_to; c++) {
      unsigned char ace_c = last_video_ram[r * 32 + c];
      unsigned char char_code = ace_c & 127;
      const unsigned char *font_bmp = last_charset + char_code * 8;
      char utf8_char[16];
      ace_char_to_utf8(ace_c, font_bmp, utf8_char, sizeof(utf8_char));
      size_t char_len = strlen(utf8_char);
      if (line_len + char_len < sizeof(line_buf) - 1) {
        strcpy(line_buf + line_len, utf8_char);
        line_len += char_len;
      }
    }
    while (line_len > 0 && line_buf[line_len - 1] == ' ') {
      line_buf[line_len - 1] = '\0';
      line_len--;
    }
    if (buf_len + line_len + 2 >= buf_cap) {
      buf_cap *= 2;
      char *new_buf = realloc(buf, buf_cap);
      if (!new_buf) { free(buf); return; }
      buf = new_buf;
    }
    if (r > start_row) {
      buf[buf_len++] = '\n';
      buf[buf_len] = '\0';
    }
    strcpy(buf + buf_len, line_buf);
    buf_len += line_len;
  }
  if (buf_len > 0) SDL_SetClipboardText(buf);
  free(buf);
}

void ui_init(void) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    exit(1);
  }

  ace_sdl_event_type = SDL_RegisterEvents(1);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

  int win_w = hsize + BORDER_SIDE * 2;
  int win_h = vsize + BORDER_TOP + BORDER_SIDE;

#ifndef __APPLE__
  void *xid = linux_create_window(win_w, win_h, "SDLAce \u2014 Jupiter ACE Emulator", ace_sdl_event_type);
  sdl_window = SDL_CreateWindowFrom(xid);
#else
  sdl_window = SDL_CreateWindow("SDLAce \u2014 Jupiter ACE Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
#endif

  if (!sdl_window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    exit(1);
  }
  SDL_SetWindowMinimumSize(sdl_window, hsize / 2 + BORDER_SIDE, vsize / 2 + BORDER_TOP / 2);

  sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!sdl_renderer) {
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
    if (!sdl_renderer) {
      fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
      exit(1);
    }
  }

  SDL_SetRenderDrawColor(sdl_renderer, 255, 255, 255, 255);
  SDL_RenderClear(sdl_renderer);

  sdl_texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, hsize, vsize);
  pixel_buf = calloc(hsize * vsize, sizeof(Uint32));

#ifdef WHITE_ON_BLACK
  col_black = 0xFFFFFFFF;
  col_white = 0xFF000000;
#else
  col_white = 0xFFFFFFFF;
  col_black = 0xFF000000;
#endif

  refresh_screen_flag = 1;
  memset(video_ram_old, 0xff, sizeof(video_ram_old));

  {
    int w, h, channels;
    unsigned char *img_data = stbi_load_from_memory(ace_logo_png, (int)ace_logo_png_len, &w, &h, &channels, 4);
    if (img_data) {
      SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(img_data, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
      if (surface) {
        bezel_logo_texture = SDL_CreateTextureFromSurface(sdl_renderer, surface);
        if (bezel_logo_texture) {
          bezel_logo_w = w; bezel_logo_h = h;
          SDL_SetTextureBlendMode(bezel_logo_texture, SDL_BLENDMODE_BLEND);
        }
        SDL_FreeSurface(surface);
      }
      stbi_image_free(img_data);
    }
  }

#ifdef __APPLE__
  macos_setup_menu(ace_sdl_event_type);
#endif
}

void ui_audio_init(int sample_rate) {
  SDL_AudioSpec wanted;
  SDL_zero(wanted);
  wanted.freq = sample_rate;
  wanted.format = AUDIO_F32SYS;
  wanted.channels = 1;
  wanted.samples = 1024;
  wanted.callback = NULL;

  sdl_audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
  if (sdl_audio_device != 0) {
    SDL_PauseAudioDevice(sdl_audio_device, 0);
  }
}

void ui_audio_queue(float *buffer, int samples) {
  if (sdl_audio_device != 0) {
    SDL_QueueAudio(sdl_audio_device, buffer, samples * sizeof(float));
  }
}

void ui_sync_frame(void) {
  /* Frame pacing using high-precision SDL counter (50 Hz / 20.0 ms) */
  static Uint64 next_frame_counter = 0;
  Uint64 perf_freq = SDL_GetPerformanceFrequency();
  Uint64 ticks_per_frame = perf_freq / 50;

  Uint64 now = SDL_GetPerformanceCounter();
  if (next_frame_counter == 0 || now > next_frame_counter + ticks_per_frame * 5) {
    next_frame_counter = now + ticks_per_frame;
  } else {
    if (now < next_frame_counter) {
      Uint64 diff = next_frame_counter - now;
      Uint32 sleep_ms = (Uint32)((diff * 1000) / perf_freq);
      if (sleep_ms >= 2) SDL_Delay(sleep_ms - 1);
      while (SDL_GetPerformanceCounter() < next_frame_counter) {}
    }
    next_frame_counter += ticks_per_frame;
  }
}

void ui_closedown(void) {
  if (sdl_audio_device) {
    SDL_CloseAudioDevice(sdl_audio_device);
    sdl_audio_device = 0;
  }
  free(pixel_buf);
  if (bezel_logo_texture) SDL_DestroyTexture(bezel_logo_texture);
  if (sdl_texture) SDL_DestroyTexture(sdl_texture);
  if (sdl_renderer) SDL_DestroyRenderer(sdl_renderer);
  if (sdl_window) SDL_DestroyWindow(sdl_window);
  SDL_Quit();
}

static void set_pixel(int x, int y, Uint32 colour) {
  for (int sy = 0; sy < SCALE; sy++)
    for (int sx = 0; sx < SCALE; sx++)
      pixel_buf[(y * SCALE + sy) * hsize + (x * SCALE + sx)] = colour;
}

static void set_image_character(int x, int y, int inv, const unsigned char *charbmap) {
  if (is_cell_selected(x, y)) inv = !inv;
  for (int charbmap_y = 0; charbmap_y < 8; charbmap_y++) {
    unsigned char charbmap_row = charbmap[charbmap_y];
    if (inv) charbmap_row ^= 255;
    unsigned char charbmap_row_mask = 128;
    for (int charbmap_x = 0; charbmap_x < 8; charbmap_x++) {
      Uint32 colour = (charbmap_row & charbmap_row_mask) ? col_black : col_white;
      set_pixel(x * 8 + charbmap_x, y * 8 + charbmap_y, colour);
      charbmap_row_mask >>= 1;
    }
  }
}

static void draw_bezel(int win_w, int win_h, int dst_x, int dst_y, int dst_w, int dst_h) {
  SDL_SetRenderDrawColor(sdl_renderer, 235, 235, 237, 255);
  SDL_RenderClear(sdl_renderer);
  SDL_SetRenderDrawColor(sdl_renderer, 160, 160, 162, 255);
  SDL_Rect r1 = {dst_x - 3, dst_y - 3, dst_w + 6, dst_h + 6};
  SDL_RenderDrawRect(sdl_renderer, &r1);
  SDL_SetRenderDrawColor(sdl_renderer, 60, 58, 55, 255);
  SDL_Rect outline = {dst_x - 2, dst_y - 2, dst_w + 4, dst_h + 4};
  SDL_RenderDrawRect(sdl_renderer, &outline);

  int pad_l = (int)(win_w * 0.04f);
  if (pad_l < 8) pad_l = 8;
  if (bezel_logo_texture && bezel_logo_w > 0 && bezel_logo_h > 0) {
    int target_h = (int)(dst_y * 0.70f);
    if (target_h < 12) target_h = 12;
    int target_w = (int)((float)bezel_logo_w * target_h / bezel_logo_h);
    int logo_y = (dst_y - target_h) / 2;
    SDL_Rect logo_rect = {pad_l, logo_y, target_w, target_h};
    SDL_RenderCopy(sdl_renderer, bezel_logo_texture, NULL, &logo_rect);
  }
}

void ui_refresh(const unsigned char *video_ram, const unsigned char *charset, int full_refresh) {
  last_video_ram = video_ram;
  last_charset = charset;

  if (full_refresh) refresh_screen_flag = 1;
  int xmin = 31, ymin = 23, xmax = 0, ymax = 0;
  int video_ram_old_ofs = 0;

  for (int y = 0; y < 24; y++) {
    for (int x = 0; x < 32; x++, video_ram++, video_ram_old_ofs++) {
      unsigned char c = *video_ram;
      if (c != video_ram_old[video_ram_old_ofs] || refresh_screen_flag) {
        video_ram_old[video_ram_old_ofs] = c;
        if (x < xmin) xmin = x;
        if (y < ymin) ymin = y;
        if (x > xmax) xmax = x;
        if (y > ymax) ymax = y;
        int inv = c & 128;
        c &= 127;
        set_image_character(x, y, inv, charset + c * 8);
      }
    }
  }

  if (refresh_screen_flag) {
    xmin = 0; ymin = 0; xmax = 31; ymax = 23;
  }

  if (xmax >= xmin && ymax >= ymin) {
    SDL_UpdateTexture(sdl_texture, NULL, pixel_buf, hsize * sizeof(Uint32));
    int win_w, win_h;
    SDL_GetRendererOutputSize(sdl_renderer, &win_w, &win_h);
    int log_w, log_h;
    SDL_GetWindowSize(sdl_window, &log_w, &log_h);
    float px_ratio = (float)win_w / (float)log_w;

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

    draw_bezel(win_w, win_h, dst_x, dst_y, dst_w, dst_h);

    SDL_Rect screen_area = {dst_x, dst_y, dst_w, dst_h};
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_RenderFillRect(sdl_renderer, &screen_area);

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
  refresh_screen_flag = 0;
}

static void send_event(UiEventCallback cb, UiEventType type, void *data) {
  if (cb) {
    UiEvent ev = {type, data};
    cb(&ev);
  }
}

void ui_check_events(UiEventCallback cb) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == ace_sdl_event_type) {
      char *path = (char *)ev.user.data1;
      switch (ev.user.code) {
        case ACE_EVENT_DELETE_LINE:    send_event(cb, UI_EVENT_DELETE_LINE, NULL); break;
        case ACE_EVENT_ATTACH_TAPE:    send_event(cb, UI_EVENT_ATTACH_TAPE, path); break;
        case ACE_EVENT_REWIND_TAPE:    send_event(cb, UI_EVENT_REWIND_TAPE, NULL); break;
        case ACE_EVENT_INVERSE_VIDEO:  send_event(cb, UI_EVENT_INVERSE_VIDEO, NULL); break;
        case ACE_EVENT_JUPITER_LAYOUT: send_event(cb, UI_EVENT_JUPITER_LAYOUT, NULL); break;
        case ACE_EVENT_GRAPHICS:       send_event(cb, UI_EVENT_GRAPHICS, NULL); break;
        case ACE_EVENT_SPOOL:          send_event(cb, UI_EVENT_SPOOL, path); break;
        case ACE_EVENT_RESET:          send_event(cb, UI_EVENT_RESET, NULL); break;
        case ACE_EVENT_BREAK:          send_event(cb, UI_EVENT_BREAK, NULL); break;
        case ACE_EVENT_PASTE:          send_event(cb, UI_EVENT_PASTE, NULL); break;
        case ACE_EVENT_COPY:           copy_selection_to_clipboard(); break;
      }
      continue;
    }

    switch (ev.type) {
      case SDL_QUIT:
        send_event(cb, UI_EVENT_QUIT, NULL);
        break;
      case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
            ev.window.event == SDL_WINDOWEVENT_RESTORED ||
            ev.window.event == SDL_WINDOWEVENT_RESIZED ||
            ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
          refresh_screen_flag = 1;
        break;
      case SDL_KEYDOWN: {
        if (ev.key.repeat != 0) break;
        SDL_Keycode ks = ev.key.keysym.sym;
        int mods = (int)ev.key.keysym.mod;
        if (keyboard_get_jupiter_layout()) ks = SDL_GetKeyFromScancode(ev.key.keysym.scancode);
#ifdef __APPLE__
        if (mods & KMOD_GUI) break;
#endif
        if (ks == SDLK_c && (mods & KMOD_CTRL)) { copy_selection_to_clipboard(); break; }
        if (ks == SDLK_v && (mods & KMOD_CTRL)) { send_event(cb, UI_EVENT_PASTE, NULL); break; }
        if (ks == SDLK_F8) { send_event(cb, UI_EVENT_TOGGLE_JUPITER, NULL); break; }
        if (ks == SDLK_F9) { send_event(cb, UI_EVENT_TOGGLE_GRAPHICS, NULL); }

        if (!spooler_active()) keyboard_keypress(ks, mods);
        break;
      }
      case SDL_KEYUP:
        if (!spooler_active()) {
          SDL_Keycode ks = ev.key.keysym.sym;
          int mods = (int)ev.key.keysym.mod;
          if (keyboard_get_jupiter_layout()) ks = SDL_GetKeyFromScancode(ev.key.keysym.scancode);
#ifdef __APPLE__
          if (mods & KMOD_GUI) break;
#endif
          if (ks == SDLK_F8) break;
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
            selecting_text = 1;
            sel_start_col = c; sel_start_row = r;
            sel_end_col = -1;  sel_end_row = -1;
          } else {
            clear_selection();
          }
        }
        break;
      case SDL_MOUSEMOTION:
        if (selecting_text && (ev.motion.state & SDL_BUTTON_LMASK)) {
          int c, r;
          if (get_grid_pos(ev.motion.x, ev.motion.y, &c, &r, 1)) {
            if (c != sel_end_col || r != sel_end_row) {
              sel_end_col = c; sel_end_row = r;
              refresh_screen_flag = 1;
            }
          }
        }
        break;
      case SDL_MOUSEBUTTONUP:
        if (ev.button.button == SDL_BUTTON_LEFT && selecting_text) {
          if (sel_end_col < 0 || sel_end_row < 0 || (sel_start_col == sel_end_col && sel_start_row == sel_end_row)) {
            clear_selection();
          } else {
            selecting_text = 0;
          }
        }
        break;
    }
  }
}

char *ui_get_clipboard_text(void) {
  return SDL_GetClipboardText();
}

void ui_free_clipboard_text(char *text) {
  if (text) SDL_free(text);
}

const char *ui_get_base_path(void) {
  return SDL_GetBasePath();
}
