/* Copyright (c) 2018 Joshua L Ervin. All rights reserved. */
/* Licensed under the MIT License. See the LICENSE file in the project root for full license information. */

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include "config.h"
#include "globals.h"
#include "ipc.h"
#include "types.h"
#include "utils.h"

static struct client *f_client = NULL; /* focused client */
static struct client *c_list[WORKSPACE_NUMBER]; /* 'stack' of managed clients in drawing order */
static struct client *f_list[WORKSPACE_NUMBER]; /* ordered lists for clients to be focused */
static struct monitor *m_list = NULL; /* All saved monitors */
static struct config conf; /* gloabl config */
static int ws_m_list[WORKSPACE_NUMBER]; /* Mapping from workspaces to associated monitors */
static int curr_ws = 0;
static int m_count = 0;
static Display *display;
static Atom net_atom[NetLast], wm_atom[WMLast];
static int point_x = -1, point_y = -1;
static Window root, check;
static bool running = true;
static int screen, display_width, display_height;
static int (*xerrorxlib)(Display *, XErrorEvent *);

/* All functions */

/* Client management functions */
static void client_cardinal_focus(struct client *c, int dir);
static void client_center(struct client *c);
static void client_close(struct client *c);
static void client_decorate_new(struct client *c);
static void client_decorations_create(struct client *c);
static void client_decorations_destroy(struct client *c);
static void client_delete(struct client *c);
static void client_fullscreen(struct client *c);
static void client_hide(struct client *c);
static void client_manage_focus(struct client *c);
static void client_move_absolute(struct client *c, int x, int y);
static void client_move_relative(struct client *c, int x, int y);
static void client_move_to_front(struct client *c);
static void client_monocle(struct client *c);
static void client_raise(struct client *c);
static void client_refresh(struct client *c);
static void client_resize_absolute(struct client *c, int w, int h);
static void client_resize_relative(struct client *c, int w, int h);
static void client_save(struct client *c, int ws);
static void client_send_to_ws(struct client *c, int ws);
static void client_set_color(struct client *c, unsigned long i_color, unsigned long b_color);
static void client_set_input(struct client *c);
static void client_show(struct client *c);
static void client_snap_left(struct client *c);
static void client_snap_right(struct client *c);
static void client_toggle_decorations(struct client *c);

/* Event handlers */
static void handle_client_message(XEvent *e);
static void handle_configure_notify(XEvent *e);
static void handle_configure_request(XEvent *e);
static void handle_map_request(XEvent *e);
static void handle_unmap_notify(XEvent *e);

/* IPC client functions */
static void ipc_move_absolute(long *d);
static void ipc_move_relative(long *d);
static void ipc_monocle(long *d);
static void ipc_raise(long *d);
static void ipc_resize_absolute(long *d);
static void ipc_resize_relative(long *d);
static void ipc_toggle_decorations(long *d);
static void ipc_window_close(long *d);
static void ipc_window_center(long *d);
static void ipc_bf_color(long *d);
static void ipc_bu_color(long *d);
static void ipc_if_color(long *d);
static void ipc_iu_color(long *d);
static void ipc_b_width(long *d);
static void ipc_i_width(long *d);
static void ipc_t_height(long *d);
static void ipc_switch_ws(long *d);
static void ipc_send_to_ws(long *d);
static void ipc_fullscreen(long *d);
static void ipc_snap_left(long *d);
static void ipc_snap_right(long *d);
static void ipc_cardinal_focus(long *d);
static void ipc_cycle_focus(long *d);
static void ipc_pointer_move(long *d);
static void ipc_top_gap(long *d);
static void ipc_save_monitor(long *d);

static void monitors_free(void);
static void setup_monitors(void);

static void close_wm(void);
static struct client* get_client_from_window(Window w);
static void load_config(char *conf_path);
static void manage_new_window(Window w, XWindowAttributes *wa);
static int manage_xsend_icccm(struct client *c, Atom atom);
static void refresh_config(void);
static void run(void);
static bool safe_to_focus(int ws);
static void setup(void);
static void switch_ws(int ws);
static void update_c_list(void);
static void usage(void);
static void version(void);
static int xerror(Display *display, XErrorEvent *e);

/* Native X11 Event handler */
static void (*event_handler[LASTEvent])(XEvent *e) = 
{
    [MapRequest]       = handle_map_request,
    [UnmapNotify]      = handle_unmap_notify,
    [ConfigureNotify]  = handle_configure_notify,
    [ConfigureRequest] = handle_configure_request,
    [ClientMessage]    = handle_client_message,
};

static void (*ipc_handler[IPCLast])(long *) = 
{
    [IPCWindowMoveRelative]       = ipc_move_relative,
    [IPCWindowMoveAbsolute]       = ipc_move_absolute,
    [IPCWindowMonocle]            = ipc_monocle,
    [IPCWindowRaise]              = ipc_raise,
    [IPCWindowResizeRelative]     = ipc_resize_relative,
    [IPCWindowResizeAbsolute]     = ipc_resize_absolute,
    [IPCWindowToggleDecorations]  = ipc_toggle_decorations,
    [IPCWindowClose]              = ipc_window_close,
    [IPCWindowCenter]             = ipc_window_center,
    [IPCFocusColor]               = ipc_bf_color,
    [IPCUnfocusColor]             = ipc_bu_color,
    [IPCInnerFocusColor]          = ipc_if_color,
    [IPCInnerUnfocusColor]        = ipc_iu_color,
    [IPCBorderWidth]              = ipc_b_width,
    [IPCInnerBorderWidth]         = ipc_i_width,
    [IPCTitleHeight]              = ipc_t_height,
    [IPCSwitchWorkspace]          = ipc_switch_ws,
    [IPCSendWorkspace]            = ipc_send_to_ws,
    [IPCFullscreen]               = ipc_fullscreen,
    [IPCSnapLeft]                 = ipc_snap_left,
    [IPCSnapRight]                = ipc_snap_right,
    [IPCCardinalFocus]            = ipc_cardinal_focus,
    [IPCCycleFocus]               = ipc_cycle_focus,
    [IPCPointerMove]              = ipc_pointer_move,
    [IPCSaveMonitor]              = ipc_save_monitor,
    [IPCTopGap]                   = ipc_top_gap,
};

/* Give focus to the given client in the given direction */
static void
client_cardinal_focus(struct client *c, int dir)
{
    struct client *tmp, *focus_next;
    int min;

    tmp = c_list[curr_ws];
    focus_next = NULL;
    min = INT_MAX;

    while (tmp != NULL)
    {
        int dist = euclidean_distance(c, tmp);
        switch (dir)
        {
            case EAST:
                fprintf(stderr, "Focusing EAST\n");
                if (tmp->geom.x > c->geom.x && dist < min) {
                    min = dist;
                    focus_next = tmp;
                }
                break;
            case SOUTH:
                fprintf(stderr, "Focusing SOUTH\n");
                if (tmp->geom.y > c->geom.y && dist < min) {
                    min = dist;
                    focus_next = tmp;
                }
                break;
            case WEST:
                fprintf(stderr, "Focusing WEST\n");
                if (tmp->geom.x < c->geom.x && dist < min) {
                    min = dist;
                    focus_next = tmp;
                }
                break;
            case NORTH:
                fprintf(stderr, "Focusing NORTH\n");
                if (tmp->geom.y < c->geom.y && dist < min) {
                    min = dist;
                    focus_next = tmp;
                }
                break;
        }
        tmp = tmp->next;
    }

    if (focus_next == NULL) {
        fprintf(stderr, WINDOW_MANAGER_NAME": Cannot cardinal focus, no valid windows found\n");
        return;
    } else {
        fprintf(stderr, WINDOW_MANAGER_NAME": Valid window found in direction %d, focusing\n", dir);
        client_manage_focus(focus_next);
    }
}

/* Move a client to the center of the screen, centered vertically and horizontally
 * by the middle of the Client
 */
static void
client_center(struct client *c)
{
    int mon;
    fprintf(stderr, WINDOW_MANAGER_NAME": Centering Client");
    mon = ws_m_list[c->ws];
    client_move_absolute(c, m_list[mon].x + m_list[mon].width / 2 - (c->geom.width / 2),
            m_list[mon].y + m_list[mon].height / 2 - (c->geom.height / 2));
}

/* Close connection to the current display */
static void
close_wm(void)
{
    fprintf(stderr, WINDOW_MANAGER_NAME": Closing display...");
    XCloseDisplay(display);
}

/* Communicate with the given Client, kindly telling it to close itself
 * and terminate any associated processes using the WM_DELETE_WINDOW protocol
 */
static void
client_close(struct client *c)
{
    XEvent ev;
    ev.type = ClientMessage;
    ev.xclient.window = c->window;
    ev.xclient.message_type = wm_atom[WMProtocols];
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = wm_atom[WMDeleteWindow];
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, c->window, False, NoEventMask, &ev);
    fprintf(stderr, WINDOW_MANAGER_NAME": Closing window...");
}

/* Create new "dummy" windows to be used as decorations for the given client */
static void
client_decorate_new(struct client *c)
{
    fprintf(stderr, "Decorating new client\n");
    int w = c->geom.width + 2 * conf.i_width;
    int h = c->geom.height + 2 * conf.i_width + conf.t_height;
    int x = c->geom.x - conf.i_width - conf.b_width;
    int y = c->geom.y - conf.i_width - conf.b_width - conf.t_height; 
    Window dec = XCreateSimpleWindow(display, root, x, y, w, h, conf.b_width,
            conf.bu_color, conf.bf_color);
    fprintf(stderr, "Mapping new decorations\n");
    XMapWindow(display, dec);
    c->dec = dec;
    c->decorated = true;
}

static void
client_decorations_create(struct client *c)
{
    client_decorate_new(c);
}

/* Destroy any "dummy" windows associated with the given Client as decorations */
static void 
client_decorations_destroy(struct client *c)
{
    fprintf(stderr, "Removing decorations\n");
    XUnmapWindow(display, c->dec);
    XDestroyWindow(display, c->dec);
    c->decorated = false;
}

/* Remove the given Client from the list of currently managed clients 
 * Does not free the given client from memory. 
 * */
static void
client_delete(struct client *c)
{
    int ws;
    ws = c->ws;

    if (ws == -1) {
        fprintf(stderr, "Cannot delete client, not found\n"); 
        return;
    } else {
        fprintf(stderr, "Deleting client on workspace %d\n", ws); 
    }

    /* Delete in the stack */
    if (c_list[ws] == c) {
        c_list[ws] = c_list[ws]->next;
    } else {
        struct client *tmp = c_list[ws];
        while (tmp != NULL && tmp->next != c)
            tmp = tmp->next;

        tmp->next = tmp->next->next;
    }

    /* Delete in the focus list */
    /* I'll factor this out later */
    /* Or actually it might not be so easy... */
    if (f_list[ws] == c) {
        f_list[ws] = f_list[ws]->f_next;
    } else {
        struct client *tmp = f_list[ws];
        while (tmp != NULL && tmp->f_next != c)
            tmp = tmp->f_next;

        tmp->f_next = tmp->f_next->f_next;
    }

    if (c_list[ws] == NULL)
        f_client = NULL;

    update_c_list();
}

static void
monitors_free(void)
{
    free(m_list);
    m_list = NULL;
}

/* Set the given Client to be fullscreen. Moves the window to fill the dimensions
 * of the given display. 
 * Updates the value of _NET_WM_STATE_FULLSCREEN to reflect fullscreen changes
 */
static void
client_fullscreen(struct client *c)
{
    int mon;
    mon = ws_m_list[c->ws];
    client_move_absolute(c, m_list[mon].x, m_list[mon].y);
    client_resize_absolute(c, m_list[mon].width, m_list[mon].height);
    if (!c->fullscreen)
        XChangeProperty(display, c->window, net_atom[NetWMState], XA_ATOM, 32, PropModeReplace, (unsigned char *)&net_atom[NetWMStateFullscreen], 1);
    else
        XChangeProperty(display, c->window, net_atom[NetWMState], XA_ATOM, 32, PropModeReplace, (unsigned char *) 0, 0);

    c->fullscreen = !c->fullscreen;
}

/* Focus the next window in the list. Windows are sorted by the order in which they are 
 * created (mapped to the window manager)
 */
static void
focus_next(struct client *c)
{
    if (c == NULL)
        return;

    int ws;
    ws = c->ws;

    if (f_list[ws] == c && f_list[ws]->f_next == NULL) {
        client_manage_focus(f_list[ws]);
        return;
    }

    struct client *tmp;
    tmp = c->f_next == NULL ? f_list[ws] : c->f_next;
    client_manage_focus(tmp);
}

/* Returns the struct client associated with the given struct Window */
static struct client*
get_client_from_window(Window w)
{
    for (int i = 0; i < WORKSPACE_NUMBER; i++) {
        for (struct client *tmp = c_list[i]; tmp != NULL; tmp = tmp->next) {
            if (tmp->window == w)
                return tmp;
            else if (!tmp->dec && tmp->dec == w)
                return tmp;
        }
    }

    return NULL;
}

/* Redirect an XEvent from berry's client program, berryc */
static void
handle_client_message(XEvent *e)
{
    XClientMessageEvent *cme = &e->xclient;
    long cmd, *data;

    if (cme->message_type == XInternAtom(display, BERRY_CLIENT_EVENT, False)) {
        fprintf(stderr, "Recieved event from berryc\n");
        if (cme->format != 32)
            return;
        cmd = cme->data.l[0];
        data = cme->data.l;
        ipc_handler[cmd](data);
    }
}

static void
handle_configure_notify(XEvent *e)
{
    XConfigureEvent *ev = &e->xconfigure;

    if (ev->window == root)
        return;

    fprintf(stderr, "Handling configure notify event\n");

    monitors_free();
    setup_monitors();
}

static void
handle_configure_request(XEvent *e) 
{
    struct client *c;
    XConfigureRequestEvent *ev = &e->xconfigurerequest;
    XWindowChanges wc;

    fprintf(stderr, "Handling configure request event\n");

    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.border_width = ev->border_width;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;
    XConfigureWindow(display, ev->window, ev->value_mask, &wc);
    c = get_client_from_window(ev->window);

    if (c != NULL)
        client_refresh(c);
}

static void
handle_map_request(XEvent *e)
{
    static XWindowAttributes wa;
    XMapRequestEvent *ev = &e->xmaprequest;

    if (!XGetWindowAttributes(display, ev->window, &wa))
        return;
    if (wa.override_redirect)
        return;

    manage_new_window(ev->window, &wa);
}

static void
handle_unmap_notify(XEvent *e)
{
    XUnmapEvent *ev = &e->xunmap;
    struct client *c;
    c = get_client_from_window(ev->window);

    if (c != NULL) {
        focus_next(c);
        if (c->decorated)
            XDestroyWindow(display, c->dec);
        client_delete(c);
        free(c);
    }
}

/* Hides the given Client by moving it outside of the visible display */
static void
client_hide(struct client *c)
{
    if (!c->hidden) {
        c->x_hide = c->geom.x;
        fprintf(stderr, "Hiding client");
        client_move_absolute(c, display_width + conf.b_width, c->geom.y);
        c->hidden = true;
    }
}

static void
ipc_move_absolute(long *d)
{
    int x, y;

    if (f_client == NULL)
        return;

    x = d[1];
    y = d[2];

    client_move_absolute(f_client, x, y);
}

static void
ipc_move_relative(long *d)
{
    int x, y;

    if (f_client == NULL)
        return;

    x = d[1];
    y = d[2];

    client_move_relative(f_client, x, y);
}

static void
ipc_monocle(long *d)
{
    if (f_client == NULL)
        return;

    client_monocle(f_client);
}

static void
ipc_raise(long *d) 
{
    if (f_client == NULL)
        return;

    client_raise(f_client);
}

static void 
ipc_resize_absolute(long *d)
{
    int w, h;

    if (f_client == NULL)
        return;

    w = d[1];
    h = d[2];

    client_resize_absolute(f_client, w, h);
}

static void 
ipc_resize_relative(long *d)
{
    int w, h;

    if (f_client == NULL)
        return;

    w = d[1];
    h = d[2];

    client_resize_relative(f_client, w, h);
}

static void 
ipc_toggle_decorations(long *d)
{
    if (f_client == NULL)
        return ;

    client_toggle_decorations(f_client);
}

static void
ipc_window_close(long *d)
{
    if (f_client == NULL)
        return;

    client_close(f_client);
}

static void
ipc_window_center(long *d)
{
    client_center(f_client);
}

static void 
ipc_bf_color(long *d)
{
    unsigned long nc;
    nc = d[1];
    conf.bf_color = nc;

    refresh_config();
}

void 
ipc_bu_color(long *d)
{
    unsigned long nc;
    nc = d[1];
    conf.bu_color = nc;

    refresh_config();
}

static void
ipc_if_color(long *d)
{
    unsigned long nc;
    nc = d[1];
    conf.if_color = nc;

    refresh_config();
}

static void
ipc_iu_color(long *d)
{
    unsigned long nc;
    nc = d[1];
    conf.iu_color = nc;

    refresh_config();
}

static void 
ipc_b_width(long *d)
{
    int w;
    w = d[1];
    conf.b_width = w;

    refresh_config();
    client_raise(f_client);
}

static void
ipc_i_width(long *d)
{
    int w;
    w = d[1];
    conf.i_width = w;

    refresh_config();
}

static void 
ipc_t_height(long *d)
{
    int th;
    th = d[1];
    conf.t_height = th;

    refresh_config();
}

static void
ipc_switch_ws(long *d)
{
    int ws = d[1];
    switch_ws(ws - 1);
}

static void
ipc_send_to_ws(long *d)
{
    if (f_client == NULL)
        return;

    int ws = d[1];
    client_send_to_ws(f_client, ws - 1);
}

static void
ipc_fullscreen(long *d)
{
    if (f_client == NULL)
        return;

    client_fullscreen(f_client);
}

static void
ipc_snap_left(long *d)
{
    if (f_client == NULL)
        return;

    client_snap_left(f_client);
}

static void
ipc_snap_right(long *d)
{
    if (f_client == NULL)
        return;

    client_snap_right(f_client);
}

static void
ipc_cardinal_focus(long *d)
{
    int dir = d[1];
    client_cardinal_focus(f_client, dir);
}

static void
ipc_cycle_focus(long *d)
{
    focus_next(f_client);
}

static void
ipc_pointer_move(long *d)
{
    /* Shoutout to vain for this methodology */
    int x, y, di, dx, dy;
    unsigned int dui;
    Window child, dummy;
    struct client *c;

    if (d[1] == 2) {
        point_x = -1;
        point_y = -1;
        return;
    }

    XQueryPointer(display, root, &dummy, &child, &x, &y, &di, &di, &dui);

    if (point_x == -1 && point_y == -1) {
        point_x = x;
        point_y = y;
    }

    dx = x - point_x;
    dy = y - point_y;

    point_x = x;
    point_y = y;

    c = get_client_from_window(child);
    fprintf(stderr, "Recieved pointer input, moving window by %d, %d\n", dx, dy);
    if(c != NULL)
    {
        /* Focus the client for either type of event */
        client_manage_focus(c);
        /* Only move if it is of type 1 */
        if (d[1] == 1)
            client_move_relative(c, dx, dy);
    }
}

static void
ipc_top_gap(long *d)
{
    int data = d[1];
    conf.top_gap = data;

    refresh_config();
}

static void
ipc_save_monitor(long *d)
{
    int ws, mon;
    ws = d[1];
    mon = d[2];

    if (mon >= m_count) {
        fprintf(stderr, "Cannot save monitor, number is too high\n");
        return;
    }

    fprintf(stderr, "Saving ws %d to monitor %d\n", ws, mon);

    /* Associate the given workspace to the given monitor */
    ws_m_list[ws] = mon;
}

static void
load_config(char *conf_path)
{
    if (fork() == 0) {
        setsid();
        execl(conf_path, conf_path, NULL);
        fprintf(stderr, "CONFIG PATH: %s\n", conf_path);
    }
}

static void
client_manage_focus(struct client *c)
{
    if (c != NULL && f_client != NULL) {
        client_set_color(f_client, conf.iu_color, conf.bu_color);
        manage_xsend_icccm(c, wm_atom[WMTakeFocus]);
    }

    if (c != NULL) {
        client_set_color(c, conf.if_color, conf.bf_color);
        client_raise(c);
        client_set_input(c);
        /* Remove focus from the old window */
        XDeleteProperty(display, root, net_atom[NetActiveWindow]);
        f_client = c;
        /* Tell EWMH about our new window */
        XChangeProperty(display, root, net_atom[NetActiveWindow], XA_WINDOW, 32, PropModeReplace, (unsigned char *) &(c->window), 1);
        client_move_to_front(c);
        manage_xsend_icccm(c, wm_atom[WMTakeFocus]);
    }
}

static void
manage_new_window(Window w, XWindowAttributes *wa)
{
    /* Credits to vain for XGWP checking */
    Atom prop, da;
    unsigned char *prop_ret = NULL;
    int di;
    unsigned long dl;
    if (XGetWindowProperty(display, w, net_atom[NetWMWindowType], 0,
                sizeof (Atom), False, XA_ATOM, &da, &di, &dl, &dl,
                &prop_ret) == Success) {
        if (prop_ret) {
            prop = ((Atom *)prop_ret)[0];
            if (prop == net_atom[NetWMWindowTypeDock] ||
                prop == net_atom[NetWMWindowTypeToolbar] ||
                prop == net_atom[NetWMWindowTypeUtility] ||
                prop == net_atom[NetWMWindowTypeMenu]) {
                fprintf(stderr, "Window is of type dock, toolbar, utility, menu, or splash: not managing\n");
                fprintf(stderr, "Mapping new window, not managed\n");
                XMapWindow(display, w);
                return;
            }
        }
    }


    struct client *c;
    c = malloc(sizeof(struct client));
    c->window = w;
    c->ws = curr_ws;
    c->geom.x = wa->x;
    c->geom.y = wa->y;
    c->geom.width = wa->width;
    c->geom.height = wa->height;
    c->hidden = false;
    c->fullscreen = false;

    client_decorate_new(c);
    XMapWindow(display, c->window);
    client_refresh(c); /* using our current factoring, w/h are set incorrectly */
    client_save(c, curr_ws);
    client_manage_focus(c);
    client_center(c);
    update_c_list();
}

static int
manage_xsend_icccm(struct client *c, Atom atom)
{
    /* This is from a dwm patch by Brendan MacDonell:
     * http://lists.suckless.org/dev/1104/7548.html */

    int n;
    Atom *protocols;
    int exists = 0;
    XEvent ev;

    if (XGetWMProtocols(display, c->window, &protocols, &n)) {
        while (!exists && n--)
            exists = protocols[n] == atom;
        XFree(protocols);
    }

    if (exists) {
        ev.type = ClientMessage;
        ev.xclient.window = c->window;
        ev.xclient.message_type = wm_atom[WMProtocols];
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = atom;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(display, c->window, False, NoEventMask, &ev);
    }

    return exists;
}

static void
client_move_absolute(struct client *c, int x, int y)
{
    int dest_x = x;
    int dest_y = y;

    if (c->decorated) {
        dest_x = x + conf.i_width + conf.b_width;
        dest_y = y + conf.i_width + conf.b_width + conf.t_height;
    }

    /* move relative to where decorations should go */
    XMoveWindow(display, c->window, dest_x, dest_y);
    if (c->decorated)
        XMoveWindow(display, c->dec, x, y);

    c->geom.x = x;
    c->geom.y = y;
}

static void
client_move_relative(struct client *c, int x, int y) 
{
    /* Constrain the current client to the w/h of display */
    if (conf.edge_lock) {
        int dx, dy, mon;
        mon = ws_m_list[c->ws];

        /* Lock on the right side of the screen */
        if (c->geom.x + c->geom.width + x > m_list[mon].width + m_list[mon].x)
            dx = m_list[mon].width + m_list[mon].x - c->geom.width;
        /* Lock on the left side of the screen */
        else if (c->geom.x + x < m_list[mon].x)
            dx = m_list[mon].x; 
        else
            dx = c->geom.x + x;

        /* Lock on the bottom of the screen */
        if (c->geom.y + c->geom.height + y > m_list[mon].height + m_list[mon].y)
            dy = m_list[mon].height + m_list[mon].y - c->geom.height;
        /* Lock on the top of the screen */
        else if (c->geom.y + y < m_list[mon].y + conf.top_gap)
            dy = m_list[mon].y;
        else
            dy = c->geom.y + y;

        client_move_absolute(c, dx, dy);
    }
    else
        client_move_absolute(c, c->geom.x + x, c->geom.y + y);
}

static void
client_move_to_front(struct client *c)
{
    int ws;
    ws = c->ws;

    /* If we didn't find the client */
    if (ws == -1)
        return;

    /* If the Client is at the front of the list, ignore command */
    if (c_list[ws] == c || c_list[ws]->next == NULL)
        return;

    struct client *tmp;
    for (tmp = c_list[ws]; tmp->next != NULL; tmp = tmp->next)
        if (tmp->next == c)
            break;

    tmp->next = tmp->next->next; /* remove the Client from the list */
    c->next = c_list[ws]; /* add the client to the front of the list */
    c_list[ws] = c;
}

static void
client_monocle(struct client *c)
{
    int mon;
    mon = ws_m_list[c->ws];
    client_move_absolute(c, m_list[mon].x, m_list[mon].y + conf.top_gap); 
    client_resize_absolute(c, m_list[mon].width, m_list[mon].height - conf.top_gap);
}

static void
client_raise(struct client *c)
{
    if (c != NULL) {
        if (c->decorated)
            XRaiseWindow(display, c->dec);

        XRaiseWindow(display, c->window);
    }
}

static void setup_monitors(void)
{
    XineramaScreenInfo *m_info;
    int n;

    if(!XineramaIsActive(display))
    {
        fprintf(stderr, "Xinerama not active, cannot read monitors\n");
        return;
    }

    m_info = XineramaQueryScreens(display, &n);
    fprintf(stderr, "Found %d screens active\n", n);
    m_count = n;

    /* First, we need to decide which monitors are unique.
     * Non-unique monitors can become a problem when displays
     * are mirrored. They will share the same information (which something
     * like xrandr will handle for us) but will have the exact same information.
     * We want to avoid creating duplicate structs for the same monitor if we dont
     * need to 
     */

    // TODO: Add support for repeated displays, just annoying for right now.

    m_list = malloc(sizeof(struct monitor) * n);

    for (int i = 0; i < n; i++) {
        m_list[i].screen = m_info[i].screen_number;
        m_list[i].width = m_info[i].width;
        m_list[i].height = m_info[i].height;
        m_list[i].x = m_info[i].x_org;
        m_list[i].y = m_info[i].y_org;
        fprintf(stderr, "Screen #%d with dim: x=%d y=%d w=%d h=%d\n",
                m_list[i].screen, m_list[i].x, m_list[i].y, m_list[i].width, m_list[i].height);
    }
}

static void
client_refresh(struct client *c)
{
    for (int i = 0; i < 2; i++) {
        client_move_relative(c, 0, 0);
        client_resize_relative(c, 0, 0);
    }
}

static void
refresh_config(void)
{
    for (int i = 0; i < WORKSPACE_NUMBER; i++) {
        for (struct client *tmp = c_list[i]; tmp != NULL; tmp = tmp->next) {
            /* We run into this annoying issue when where we have to 
             * re-create these windows since the border_width has changed.
             * We end up destroying and recreating this windows, but this
             * causes them to be redrawn on the wrong screen, regardless of
             * their current desktop. The easiest way around this is to move
             * them all to the current desktop and then back agian */
            if (tmp->decorated)
            {
                client_decorations_destroy(tmp);
                client_decorations_create(tmp);
            }

            client_refresh(tmp);
            client_show(tmp);

            if (f_client != tmp) 
                client_set_color(tmp, conf.iu_color, conf.bu_color);
            else
                client_set_color(tmp, conf.if_color, conf.bf_color);

            if (i != curr_ws) {
                client_hide(tmp);
            } else {
                client_show(tmp);
                client_raise(tmp);
            }
        }
    }
}

static void
client_resize_absolute(struct client *c, int w, int h) 
{
    int dw = w;
    int dh = h;
    int dec_w = w;
    int dec_h = h;

    if (c->decorated) {
        dw = w - (2 * conf.i_width) - (2 * conf.b_width);
        dh = h - (2 * conf.i_width) - (2 * conf.b_width) - conf.t_height;

        dec_w = w - (2 * conf.b_width);
        dec_h = h - (2 * conf.b_width);
    }

    XResizeWindow(display, c->window, MAX(dw, MINIMUM_DIM), MAX(dh, MINIMUM_DIM));
    if (c->decorated)
        XResizeWindow(display, c->dec, MAX(dec_w, MINIMUM_DIM), MAX(dec_h, MINIMUM_DIM));

    c->geom.width = MAX(w, MINIMUM_DIM);
    c->geom.height = MAX(h, MINIMUM_DIM);
}

static void
client_resize_relative(struct client *c, int w, int h) 
{
    if (conf.edge_lock) {
        int dw, dh, mon;
        mon = ws_m_list[c->ws];

        /* First, check if the resize will exceed the dimensions set by
         * the right side of the given monitor. If they do, cap the resize
         * amount to move only to the edge of the monitor.
         */
        if (c->geom.x + c->geom.width + w > m_list[mon].x + m_list[mon].width)
            dw = m_list[mon].x + m_list[mon].width - c->geom.x;
        else
            dw = c->geom.width + w;

        /* Next, check if the resize will exceed the dimensions set by
         * the bottom side of the given monitor. If they do, cap the resize
         * amount to move only to the edge of the monitor.
         */
        if (c->geom.y + c->geom.height + conf.t_height + h > m_list[mon].y + m_list[mon].height)
            dh = m_list[mon].height + m_list[mon].y - c->geom.y;
        else
            dh = c->geom.height + h;

        client_resize_absolute(c, dw, dh);
    } else {
        client_resize_absolute(c, c->geom.width + w, c->geom.height + h);
    }
}

static void
run(void)
{
    XEvent e;
    XSync(display, false);
    while(running)
    {
        fprintf(stderr, "Receieved new %d event\n", e.type);
        XNextEvent(display, &e);
        if (event_handler[e.type]) {
            fprintf(stderr, "Handling %d event\n", e.type);
            event_handler[e.type](&e);
        }
    }
}

static void
client_save(struct client *c, int ws)
{
    /* Save the client to the "stack" of managed clients */
    c->next = c_list[ws];
    c_list[ws] = c;

    /* Save the client o the list of focusing order */
    c->f_next = f_list[ws];
    f_list[ws] = c;
}

/* This method will return true if it is safe to show a client on the given workspace
 * based on the currently focused workspaces on each monitor.
 */
static bool
safe_to_focus(int ws)
{
    int mon = ws_m_list[ws];
    
    for (int i = 0; i < WORKSPACE_NUMBER; i++)
        if (i != ws && ws_m_list[i] == mon && c_list[i] != NULL && c_list[i]->hidden == false)
            return false;

    return true;
}

static void
client_send_to_ws(struct client *c, int ws)
{
    int prev;
    client_delete(c);
    prev = c->ws;
    c->ws = ws;
    client_save(c, ws);
    client_hide(c);
    focus_next(f_list[prev]);

    if (safe_to_focus(ws))
        client_show(c);
}

static void
client_set_color(struct client *c, unsigned long i_color, unsigned long b_color)
{
    if (c->decorated) {
        XSetWindowBackground(display, c->dec, i_color);
        XSetWindowBorder(display, c->dec, b_color);
        XClearWindow(display, c->dec);
    }
}


static void
client_set_input(struct client *c)
{
    XSetInputFocus(display, c->window, RevertToPointerRoot, CurrentTime);
}

static void
setup(void)
{
    unsigned long data[1], data2[1];
    // Setup our conf initially
    conf.b_width   = BORDER_WIDTH;
    conf.t_height  = TITLE_HEIGHT;
    conf.i_width   = INTERNAL_BORDER_WIDTH;
    conf.bf_color  = BORDER_FOCUS_COLOR;
    conf.bu_color  = BORDER_UNFOCUS_COLOR;
    conf.if_color  = INNER_FOCUS_COLOR;
    conf.iu_color  = INNER_UNFOCUS_COLOR; 
    conf.m_step    = MOVE_STEP;
    conf.r_step    = RESIZE_STEP;
    conf.focus_new = FOCUS_NEW;
    conf.edge_lock = EDGE_LOCK;
    conf.top_gap   = TOP_GAP;

    display = XOpenDisplay(NULL);
    root = DefaultRootWindow(display);
    screen = DefaultScreen(display);
    display_height = DisplayHeight(display, screen); /* Display height/width still needed for hiding clients */ 
    display_width = DisplayWidth(display, screen);

    XSelectInput(display, root,
            SubstructureRedirectMask|SubstructureNotifyMask);
    xerrorxlib = XSetErrorHandler(xerror);

    Atom utf8string;
    check = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);

    /* ewmh supported atoms */
    utf8string                       = XInternAtom(display, "UTF8_STRING", False);
    net_atom[NetSupported]           = XInternAtom(display, "_NET_SUPPORTED", False);
    net_atom[NetNumberOfDesktops]    = XInternAtom(display, "_NET_NUMBER_OF_DESKTOPS", False);
    net_atom[NetActiveWindow]        = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    net_atom[NetWMStateFullscreen]   = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    net_atom[NetWMCheck]             = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    net_atom[NetCurrentDesktop]      = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
    net_atom[NetWMState]             = XInternAtom(display, "_NET_WM_STATE", False);
    net_atom[NetWMName]              = XInternAtom(display, "_NET_WM_NAME", False);
    net_atom[NetClientList]          = XInternAtom(display, "_NET_CLIENT_LIST", False);
    net_atom[NetWMWindowType]        = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    net_atom[NetWMWindowTypeDock]    = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
    net_atom[NetWMWindowTypeToolbar] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    net_atom[NetWMWindowTypeMenu]    = XInternAtom(display, "_NET_WM_WINDOW_TYPE_MENU", False);
    net_atom[NetWMWindowTypeSplash]  = XInternAtom(display, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    /* Some icccm atoms */
    wm_atom[WMDeleteWindow]          = XInternAtom(display, "WM_DELETE_WINDOW", False);
    wm_atom[WMTakeFocus]             = XInternAtom(display, "WM_TAKE_FOCUS", False);
    wm_atom[WMProtocols]             = XInternAtom(display, "WM_PROTOCOLS", False);

    XChangeProperty(display , check , net_atom[NetWMCheck]   , XA_WINDOW  , 32 , PropModeReplace , (unsigned char *) &check              , 1);
    XChangeProperty(display , check , net_atom[NetWMName]    , utf8string , 8  , PropModeReplace , (unsigned char *) WINDOW_MANAGER_NAME , 5);
    XChangeProperty(display , root  , net_atom[NetWMCheck]   , XA_WINDOW  , 32 , PropModeReplace , (unsigned char *) &check              , 1);
    XChangeProperty(display , root  , net_atom[NetSupported] , XA_ATOM    , 32 , PropModeReplace , (unsigned char *) net_atom            , NetLast);

    /* Set the total number of desktops */
    data[0] = WORKSPACE_NUMBER;
    XChangeProperty(display, root, net_atom[NetNumberOfDesktops], XA_CARDINAL, 32, PropModeReplace, (unsigned char *) data, 1);

    /* Set the intial "current desktop" to 0 */
    data2[0] = curr_ws;
    XChangeProperty(display, root, net_atom[NetCurrentDesktop], XA_CARDINAL, 32, PropModeReplace, (unsigned char *) data2, 1);
    setup_monitors();
}

static void
client_show(struct client *c)
{
    if (c->hidden) {
        fprintf(stderr, "Showing client");
        client_move_absolute(c, c->x_hide, c->geom.y);
        client_raise(c);
        c->hidden = false;
        client_refresh(c);
    }
}

static void
client_snap_left(struct client *c)
{
    int mon;
    mon = ws_m_list[c->ws];
    client_move_absolute(c, m_list[mon].x, m_list[mon].y + conf.top_gap); 
    client_resize_absolute(c, m_list[mon].width / 2, m_list[mon].height - conf.top_gap);
}

static void
client_snap_right(struct client *c)
{
    int mon;
    mon = ws_m_list[c->ws];
    client_move_absolute(c, m_list[mon].x + m_list[mon].width / 2, m_list[mon].y + conf.top_gap); 
    client_resize_absolute(c, m_list[mon].width / 2, m_list[mon].height - conf.top_gap);
}

static void
switch_ws(int ws)
{
    unsigned long data[1];

    for (int i = 0; i < WORKSPACE_NUMBER; i++)
        if (i != ws && ws_m_list[i] == ws_m_list[ws])
            for (struct client *tmp = c_list[i]; tmp != NULL; tmp = tmp->next)
                client_hide(tmp);
        else if (i == ws)
            for (struct client *tmp = c_list[i]; tmp != NULL; tmp = tmp->next) {
                client_show(tmp);
                /* If we don't call XLowerWindow here we will run into an annoying issue
                 * where we draw the windows in reverse order. The easiest way around
                 * this is simply to draw all new windows at the "bottom" of the stack, meaning
                 * that the first element in the stack will end up on top, which is what
                 * we want :)
                 */
                XLowerWindow(display, tmp->window);
                XLowerWindow(display, tmp->dec);
            }


    curr_ws = ws;
    int mon = ws_m_list[ws];
    fprintf(stderr, "Setting Screen #%d with active workspace %d\n", m_list[mon].screen, ws);
    client_manage_focus(c_list[curr_ws]);

    data[0] = ws;
    XChangeProperty(display, root, net_atom[NetCurrentDesktop], XA_CARDINAL, 32,
            PropModeReplace, (unsigned char *) data, 1);
}

static void
client_toggle_decorations(struct client *c)
{
    if (c->decorated)
        client_decorations_destroy(c);
    else
        client_decorations_create(c);

    client_refresh(c);
    client_raise(c);
    client_manage_focus(c);
}

static void
update_c_list(void)
{
    /* Remove all current clients */
    XDeleteProperty(display, root, net_atom[NetClientList]);
    for (int i = 0; i < WORKSPACE_NUMBER; i++)
        for (struct client *tmp = c_list[i]; tmp != NULL; tmp = tmp->next)
            XChangeProperty(display, root, net_atom[NetClientList], XA_WINDOW, 32, PropModeAppend,
                    (unsigned char *) &(tmp->geom.width), 1);
}

static void
usage(void)
{
    fprintf(stderr, "Usage: berry [-h|-v|-c CONFIG_PATH]\n");
    exit(EXIT_SUCCESS);
}

static void
version(void)
{
    fprintf(stderr, "%s %s\n", WINDOW_MANAGER_NAME, __THIS_VERSION__);
    fprintf(stderr, "Copyright (c) 2018 Joshua L Ervin\n");
    fprintf(stderr, "Released under the MIT License\n");
    exit(EXIT_SUCCESS);
}

static int
xerror(Display *display, XErrorEvent *e)
{
    /* this is stolen verbatim from katriawm which stole it from dwm lol */
    if (e->error_code == BadWindow ||
            (e->request_code == X_SetInputFocus && e->error_code == BadMatch) ||
            (e->request_code == X_PolyText8 && e->error_code == BadDrawable) ||
            (e->request_code == X_PolyFillRectangle && e->error_code == BadDrawable) ||
            (e->request_code == X_PolySegment && e->error_code == BadDrawable) ||
            (e->request_code == X_ConfigureWindow && e->error_code == BadMatch) ||
            (e->request_code == X_GrabButton && e->error_code == BadAccess) ||
            (e->request_code == X_GrabKey && e->error_code == BadAccess) ||
            (e->request_code == X_CopyArea && e->error_code == BadDrawable))
        return 0;

    fprintf(stderr, "Fatal request. request code=%d, error code=%d\n",
            e->request_code, e->error_code);
    return xerrorxlib(display, e);
}

int
main(int argc, char *argv[])
{
    int opt;
    char *conf_path = malloc(MAXLEN * sizeof(char));
    bool conf_found = true;
    conf_path[0] = '\0';

    while ((opt = getopt(argc, argv, "hvc:")) != -1) {
        switch (opt) {
            case 'h':
                usage();
                break;
            case 'c':
                snprintf(conf_path, MAXLEN * sizeof(char), "%s", optarg);
                break;
            case 'v':
                version();
                break;
        }
    }

    if (conf_path[0] == '\0') {
        char *xdg_home = getenv("XDG_CONFIG_HOME");
        if (xdg_home != NULL) {
            snprintf(conf_path, MAXLEN * sizeof(char), "%s/%s", xdg_home, BERRY_AUTOSTART);
        } else {
            char *home = getenv("HOME");
            if (home == NULL) {
                fprintf(stderr, "Warning $XDG_CONFIG_HOME and $HOME not found"
                        "autostart will not be loaded.\n");
                conf_found = false;

            }
            snprintf(conf_path, MAXLEN * sizeof(char), "%s/%s/%s", home, ".config", BERRY_AUTOSTART);
        }
    }

    display = XOpenDisplay(NULL);
    if (!display)
        exit(EXIT_FAILURE);

    fprintf(stderr, "Successfully opened display\n");

    setup();
    if (conf_found)
        load_config(conf_path);
    run();
    close_wm();
}