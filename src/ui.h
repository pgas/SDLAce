#ifndef UI_H
#define UI_H

#include "keyboard.h" /* For AceKeySym if needed */

typedef enum {
    UI_EVENT_NONE,
    UI_EVENT_QUIT,
    UI_EVENT_DELETE_LINE,
    UI_EVENT_ATTACH_TAPE,    /* data1 = malloc'd filepath */
    UI_EVENT_REWIND_TAPE,
    UI_EVENT_INVERSE_VIDEO,
    UI_EVENT_GRAPHICS,
    UI_EVENT_SPOOL,          /* data1 = malloc'd filepath */
    UI_EVENT_RESET,
    UI_EVENT_BREAK,
    UI_EVENT_PASTE,
    UI_EVENT_COPY,
    UI_EVENT_JUPITER_LAYOUT,
    UI_EVENT_TOGGLE_GRAPHICS,
    UI_EVENT_TOGGLE_JUPITER
} UiEventType;

typedef struct {
    UiEventType type;
    void *data1;
} UiEvent;

typedef void (*UiEventCallback)(UiEvent *event);

void ui_init(void);
void ui_closedown(void);

/* Poll events and fire the callback for logical emulator events */
void ui_check_events(UiEventCallback cb);

/* Render the current screen */
void ui_refresh(const unsigned char *video_ram, const unsigned char *charset, int full_refresh);

/* Audio / Frame Pacing */
void ui_audio_init(int sample_rate);
void ui_audio_queue(float *buffer, int samples);
void ui_sync_frame(void);

/* Clipboard */
char *ui_get_clipboard_text(void);
void ui_free_clipboard_text(char *text);

/* Path helpers */
const char *ui_get_base_path(void);

#endif /* UI_H */
