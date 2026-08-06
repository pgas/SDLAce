/* macos_ui.m — Native macOS menu and file dialogs for xAce
 *
 * SDL2 macOS port (C) 2026
 *
 * Adds a native NSMenuBar with:
 *   Edit    → Paste from Host  (Cmd+V)
 *   Actions → Delete Line, Attach Tape…, Inverse Video, Graphics,
 *             Spool from File…, Reset, Break
 *
 * Each menu action posts an SDL_UserEvent (type = g_ace_event_type) that
 * sdlmain.c's check_events() loop handles.
 *
 * This file must be compiled as Objective-C (CMAKE sets LANGUAGE OBJC).
 */

#import <Cocoa/Cocoa.h>
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "macos_ui.h"

/* The custom SDL event type registered by sdlmain.c */
static Uint32 g_ace_event_type = 0;

/* -------------------------------------------------------------------------
 * Helper: push a custom event onto the SDL event queue (thread-safe).
 * data1 must be a malloc'd pointer; the receiver will free() it.
 * ------------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * Menu delegate – one method per action
 * ------------------------------------------------------------------------- */
@interface AceMenuDelegate : NSObject
@end

@implementation AceMenuDelegate

- (void)acDeleteLine:(id)sender
{
    (void)sender;
    push_ace_event(ACE_EVENT_DELETE_LINE, NULL);
}

- (void)acInverseVideo:(id)sender
{
    (void)sender;
    push_ace_event(ACE_EVENT_INVERSE_VIDEO, NULL);
}

- (void)acGraphics:(id)sender
{
    (void)sender;
    push_ace_event(ACE_EVENT_GRAPHICS, NULL);
}

- (void)acReset:(id)sender
{
    (void)sender;
    push_ace_event(ACE_EVENT_RESET, NULL);
}

- (void)acBreak:(id)sender
{
    (void)sender;
    push_ace_event(ACE_EVENT_BREAK, NULL);
}

- (void)acPaste:(id)sender
{
    (void)sender;
    push_ace_event(ACE_EVENT_PASTE, NULL);
}

/* Shows an NSOpenPanel; posts ACE_EVENT_ATTACH_TAPE with the chosen path. */
- (void)acAttachTape:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title                  = @"Attach Tape Image";
    panel.message                = @"Select a Jupiter ACE tape file";
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories   = NO;
    panel.allowsOtherFileTypes   = YES;
    /* allowedFileTypes is deprecated in macOS 12 but still works */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    panel.allowedFileTypes = @[@"tap", @"TAP"];
#pragma clang diagnostic pop

    if ([panel runModal] == NSModalResponseOK) {
        const char *utf8 = [panel.URL.path UTF8String];
        if (utf8) {
            push_ace_event(ACE_EVENT_ATTACH_TAPE, strdup(utf8));
        }
    }
}

/* Shows an NSOpenPanel; posts ACE_EVENT_SPOOL with the chosen path. */
- (void)acSpoolFromFile:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title                  = @"Spool from File";
    panel.message                = @"Select a text file to spool into xAce";
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories   = NO;

    if ([panel runModal] == NSModalResponseOK) {
        const char *utf8 = [panel.URL.path UTF8String];
        if (utf8) {
            push_ace_event(ACE_EVENT_SPOOL, strdup(utf8));
        }
    }
}

@end

/* -------------------------------------------------------------------------
 * Helper: make a menu item with a title, selector, key-equivalent and target
 * ------------------------------------------------------------------------- */
static NSMenuItem *
make_item(NSMenu *menu, NSString *title,
          SEL sel, NSString *key,
          NSEventModifierFlags mods,
          id target)
{
    NSMenuItem *item = [menu addItemWithTitle:title
                                       action:sel
                                keyEquivalent:key];
    item.keyEquivalentModifierMask = mods;
    item.target = target;
    return item;
}

/* -------------------------------------------------------------------------
 * Public entry point called from sdlmain.c
 * ------------------------------------------------------------------------- */
void
macos_setup_menu(Uint32 user_event_type)
{
    g_ace_event_type = user_event_type;

    /* Must run on the main thread. */
    dispatch_async(dispatch_get_main_queue(), ^{

        AceMenuDelegate *delegate = [[AceMenuDelegate alloc] init];

        NSApplication *app  = [NSApplication sharedApplication];
        NSMenu        *bar  = [app mainMenu];

        if (!bar) {
            bar = [[NSMenu alloc] initWithTitle:@"MainMenu"];
            [app setMainMenu:bar];
        }

        /* ---- 1.  App menu (first slot – SDL may have already created it) ---- */
        /* Leave SDL's existing app menu in place; just make sure Quit is there.
           (SDL maps NSApplicationDelegate applicationShouldTerminate → SDL_QUIT) */

        /* ---- 2.  Edit menu (insert at index 1) ----------------------------- */
        NSMenuItem *editBarItem = [[NSMenuItem alloc] init];
        NSMenu     *editMenu    = [[NSMenu alloc] initWithTitle:@"Edit"];
        editBarItem.submenu = editMenu;

        make_item(editMenu, @"Paste from Host", @selector(acPaste:),
                  @"v", NSEventModifierFlagCommand, delegate);

        /* Only add if not already present */
        BOOL hasEdit = NO;
        for (NSMenuItem *m in bar.itemArray) {
            if ([m.title isEqualToString:@"Edit"]) { hasEdit = YES; break; }
        }
        if (!hasEdit) {
            NSInteger insertAt = MIN(1, (NSInteger)bar.numberOfItems);
            [bar insertItem:editBarItem atIndex:insertAt];
        }

        /* ---- 3.  Actions menu (insert at index 2) -------------------------- */
        NSMenuItem *actBarItem  = [[NSMenuItem alloc] init];
        NSMenu     *actMenu     = [[NSMenu alloc] initWithTitle:@"Actions"];
        actBarItem.submenu = actMenu;

        /* Delete Line – F1 */
        make_item(actMenu,
                  @"Delete Line\t\t\t F1",
                  @selector(acDeleteLine:), @"", 0, delegate);

        [actMenu addItem:[NSMenuItem separatorItem]];

        /* Attach Tape – F3 */
        make_item(actMenu,
                  @"Attach Tape Image…\t F3",
                  @selector(acAttachTape:), @"", 0, delegate);

        /* Inverse Video – F4 */
        make_item(actMenu,
                  @"Inverse Video\t\t F4",
                  @selector(acInverseVideo:), @"", 0, delegate);

        /* Graphics – F9 */
        make_item(actMenu,
                  @"Graphics\t\t\t F9",
                  @selector(acGraphics:), @"", 0, delegate);

        [actMenu addItem:[NSMenuItem separatorItem]];

        /* Spool from File – F11 */
        make_item(actMenu,
                  @"Spool from File…\t F11",
                  @selector(acSpoolFromFile:), @"", 0, delegate);

        /* Reset – F2 */
        make_item(actMenu,
                  @"Reset\t\t\t\t F2",
                  @selector(acReset:), @"", 0, delegate);

        [actMenu addItem:[NSMenuItem separatorItem]];

        /* Break – Esc */
        make_item(actMenu,
                  @"Break\t\t\t\t Esc",
                  @selector(acBreak:), @"", 0, delegate);

        BOOL hasActions = NO;
        for (NSMenuItem *m in bar.itemArray) {
            if ([m.title isEqualToString:@"Actions"]) { hasActions = YES; break; }
        }
        if (!hasActions) {
            NSInteger insertAt = MIN(2, (NSInteger)bar.numberOfItems);
            [bar insertItem:actBarItem atIndex:insertAt];
        }
    });
}
