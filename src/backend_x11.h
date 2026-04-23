/* X11/XCB backend internal header.
 * Only included by X11-specific source files (window.c, events.c, ewmh.c, pointer.c, bspwm.c).
 */

#ifndef BSPWM_BACKEND_X11_H
#define BSPWM_BACKEND_X11_H

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/randr.h>

/* X11 keysym constants for lock keys */
#define XK_Num_Lock     0xff7f
#define XK_Caps_Lock    0xffe5
#define XK_Scroll_Lock  0xff14

/* X11 global state — defined in backend_x11.c */
extern xcb_connection_t *dpy;
extern xcb_screen_t *screen;
extern xcb_ewmh_connection_t *ewmh;
extern xcb_atom_t WM_STATE;
extern xcb_atom_t WM_TAKE_FOCUS;
extern xcb_atom_t WM_DELETE_WINDOW;
extern uint8_t randr_base;

/* X11 event masks */
#define ROOT_EVENT_MASK     (XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE)
#define CLIENT_EVENT_MASK   (XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE)

/* X11 class/instance strings */
#define META_WINDOW_IC      "wm\0Bspwm"
#define ROOT_WINDOW_IC      "root\0Bspwm"
#define PRESEL_FEEDBACK_IC  "presel_feedback\0Bspwm"
#define MOTION_RECORDER_IC  "motion_recorder\0Bspwm"

/* X11 config window masks */
#define XCB_CONFIG_WINDOW_X_Y               (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)
#define XCB_CONFIG_WINDOW_WIDTH_HEIGHT      (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)
#define XCB_CONFIG_WINDOW_X_Y_WIDTH_HEIGHT  (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)

/* X11-specific setup functions — called from bspwm.c setup() */
void x11_setup_screen(void);
void x11_setup_atoms(void);
void x11_setup_randr(void);
bool x11_try_xinerama(void);
void x11_setup_ewmh_supported(void);

#endif
