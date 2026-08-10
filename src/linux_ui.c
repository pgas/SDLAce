#ifndef __APPLE__

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include "ui_events.h"
#include "linux_ui.h"

static Uint32 g_ace_event_type = 0;
static GtkWidget *main_window = NULL;
static GtkWidget *g_menubar = NULL;

static void
push_ace_event(int code, void *data1)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type          = g_ace_event_type;
    ev.user.code     = code;
    ev.user.data1    = data1;
    SDL_PushEvent(&ev);
}

static void on_paste(GtkWidget *w, gpointer data) { push_ace_event(ACE_EVENT_PASTE, NULL); }
static void on_delete_line(GtkWidget *w, gpointer data) { push_ace_event(ACE_EVENT_DELETE_LINE, NULL); }
static void on_inverse_video(GtkWidget *w, gpointer data) { push_ace_event(ACE_EVENT_INVERSE_VIDEO, NULL); }
static void on_graphics(GtkWidget *w, gpointer data) { push_ace_event(ACE_EVENT_GRAPHICS, NULL); }
static void on_reset(GtkWidget *w, gpointer data) { push_ace_event(ACE_EVENT_RESET, NULL); }
static void on_break(GtkWidget *w, gpointer data) { push_ace_event(ACE_EVENT_BREAK, NULL); }

static void on_attach_tape(GtkWidget *w, gpointer data) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Attach Tape Image",
                                                    GTK_WINDOW(main_window),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        push_ace_event(ACE_EVENT_ATTACH_TAPE, strdup(filename));
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_spool(GtkWidget *w, gpointer data) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Spool from File",
                                                    GTK_WINDOW(main_window),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        push_ace_event(ACE_EVENT_SPOOL, strdup(filename));
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_quit(GtkWidget *w, gpointer data) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_QUIT;
    SDL_PushEvent(&ev);
}

static gboolean on_key_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = (event->type == GDK_KEY_PRESS) ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = (event->type == GDK_KEY_PRESS) ? SDL_PRESSED : SDL_RELEASED;
    
    SDL_Keycode sym = event->keyval;
    if (sym >= 0x20 && sym <= 0x7E) {
        if (sym >= 'A' && sym <= 'Z') sym = sym - 'A' + 'a';
    } else {
        switch (sym) {
            case GDK_KEY_Return: sym = SDLK_RETURN; break;
            case GDK_KEY_Escape: sym = SDLK_ESCAPE; break;
            case GDK_KEY_BackSpace: sym = SDLK_BACKSPACE; break;
            case GDK_KEY_Delete: sym = SDLK_DELETE; break;
            case GDK_KEY_Left: sym = SDLK_LEFT; break;
            case GDK_KEY_Right: sym = SDLK_RIGHT; break;
            case GDK_KEY_Up: sym = SDLK_UP; break;
            case GDK_KEY_Down: sym = SDLK_DOWN; break;
            case GDK_KEY_F1: sym = SDLK_F1; break;
            case GDK_KEY_F2: sym = SDLK_F2; break;
            case GDK_KEY_F3: sym = SDLK_F3; break;
            case GDK_KEY_F4: sym = SDLK_F4; break;
            case GDK_KEY_F5: sym = SDLK_F5; break;
            case GDK_KEY_F9: sym = SDLK_F9; break;
            case GDK_KEY_Shift_L:
            case GDK_KEY_Shift_R: sym = SDLK_LSHIFT; break;
            case GDK_KEY_Control_L:
            case GDK_KEY_Control_R: sym = SDLK_LCTRL; break;
            case GDK_KEY_Alt_L:
            case GDK_KEY_Alt_R: sym = SDLK_LALT; break;
            default: return FALSE;
        }
    }
    
    ev.key.keysym.sym = sym;
    ev.key.keysym.mod = 0;
    if (event->state & GDK_SHIFT_MASK) ev.key.keysym.mod |= KMOD_SHIFT;
    if (event->state & GDK_CONTROL_MASK) ev.key.keysym.mod |= KMOD_CTRL;
    if (event->state & GDK_MOD1_MASK) ev.key.keysym.mod |= KMOD_ALT;
    
    SDL_PushEvent(&ev);
    return FALSE;
}

void* linux_create_window(int width, int height, const char* title, Uint32 user_event_type) {
    g_ace_event_type = user_event_type;

    setenv("GDK_BACKEND", "x11", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);

    gtk_init(NULL, NULL);

    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), title);
    g_signal_connect(main_window, "destroy", G_CALLBACK(on_quit), NULL);
    g_signal_connect(main_window, "key-press-event", G_CALLBACK(on_key_event), NULL);
    g_signal_connect(main_window, "key-release-event", G_CALLBACK(on_key_event), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);

    g_menubar = gtk_menu_bar_new();
    
    // File
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    
    GtkWidget *attach_item = gtk_menu_item_new_with_label("Attach Tape... (F3)");
    g_signal_connect(attach_item, "activate", G_CALLBACK(on_attach_tape), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), attach_item);
    
    GtkWidget *spool_item = gtk_menu_item_new_with_label("Spool... (F5)");
    g_signal_connect(spool_item, "activate", G_CALLBACK(on_spool), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), spool_item);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());
    
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit (Ctrl+Q)");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);
    
    // Edit
    GtkWidget *edit_menu = gtk_menu_new();
    GtkWidget *edit_item = gtk_menu_item_new_with_label("Edit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_item), edit_menu);
    
    GtkWidget *paste_item = gtk_menu_item_new_with_label("Paste from Host (Ctrl+V)");
    g_signal_connect(paste_item, "activate", G_CALLBACK(on_paste), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), paste_item);
    
    // Actions
    GtkWidget *actions_menu = gtk_menu_new();
    GtkWidget *actions_item = gtk_menu_item_new_with_label("Actions");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(actions_item), actions_menu);
    
    GtkWidget *del_item = gtk_menu_item_new_with_label("Delete Line (F1)");
    g_signal_connect(del_item, "activate", G_CALLBACK(on_delete_line), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), del_item);
    
    GtkWidget *inv_item = gtk_menu_item_new_with_label("Inverse Video (F4)");
    g_signal_connect(inv_item, "activate", G_CALLBACK(on_inverse_video), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), inv_item);
    
    GtkWidget *gfx_item = gtk_menu_item_new_with_label("Graphics (F9)");
    g_signal_connect(gfx_item, "activate", G_CALLBACK(on_graphics), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), gfx_item);
    
    GtkWidget *reset_item = gtk_menu_item_new_with_label("Reset (F2)");
    g_signal_connect(reset_item, "activate", G_CALLBACK(on_reset), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), reset_item);
    
    GtkWidget *break_item = gtk_menu_item_new_with_label("Break (Esc)");
    g_signal_connect(break_item, "activate", G_CALLBACK(on_break), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(actions_menu), break_item);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menubar), file_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menubar), edit_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_menubar), actions_item);

    gtk_box_pack_start(GTK_BOX(vbox), g_menubar, FALSE, FALSE, 0);

    GtkWidget *draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area, width, height);
    gtk_widget_set_can_focus(draw_area, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), draw_area, TRUE, TRUE, 0);

    gtk_widget_show_all(main_window);
    gtk_widget_realize(draw_area);
    
    Window xid = gdk_x11_window_get_xid(gtk_widget_get_window(draw_area));
    return (void*)(intptr_t)xid;
}

void linux_pump_events(void) {
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
}

void linux_cancel_menu(void) {
    if (g_menubar) {
        gtk_menu_shell_deactivate(GTK_MENU_SHELL(g_menubar));
    }
}

void linux_show_attach_tape_dialog(void) {
    on_attach_tape(NULL, NULL);
}

void linux_show_spool_dialog(void) {
    on_spool(NULL, NULL);
}
#endif
