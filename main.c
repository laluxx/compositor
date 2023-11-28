#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#include "cvector.h"
#include "cvector_utils.h"
#include "stdbool.h"

enum Window_Opaqueness {
  SOLID = 0,
  TRANSPARENT = 1,
  ARGB = 2,
};

typedef struct Client {
    Window window;
    Pixmap pixmap;
    XWindowAttributes attr;
    enum Window_Opaqueness opaqueness;
    int damaged;
    Damage damage;
    Picture picture;
    Picture alpha_pict;
    XserverRegion border_size;
    XserverRegion extents;
    bool shaped;
    XRectangle shape_bounds;

    XserverRegion border_clip;
} Client;

cvector(Client *) clients = NULL;


Display *display;
int default_screen;
Window root_window;
int root_height, root_width;

// When we go to paint (composite the screen)
// we draw everything we need to the root_buffer
// then once we have finished that, we transfer it over to the root_picture in one go.
// Why _exactly_ it was chosen to be done this way, I'm not sure. But it's fine.
Picture root_picture; // the actual reference to the root picture
Picture root_buffer; // the temporary buffer
Picture root_tile; // holds the desktop wallpaper image

XserverRegion all_damage; // when this is not zero, it means the screen was damaged and we need to redraw
bool clip_changed; // Seems to be set to true when the bounds of a window has changed

int xfixes_event, xfixes_error;
int damage_event, damage_error;
int composite_event, composite_error;
int render_event, render_error;
int xshape_event, xshape_error;
int composite_opcode;

Atom opacity_atom;

const char *backgroundProps[] = {
        "_XROOTPMAP_ID",
        "_XSETROOT_ID",
        NULL,
};

/////////////////////////////////////////////////////////////////////////////////////
// This takes the desktop wallpaper (if one is set) and turns it into a picture
// so that we can draw it when it's time to composite the screen
//
Picture create_root_tile() {
    Pixmap pixmap = 0;
    int actual_format;
    unsigned long items_count;
    unsigned long bytes_after;
    unsigned char *prop;
    bool fill = false;

    Atom actual_type;
    for (int p = 0; backgroundProps[p]; p++) {
        if (XGetWindowProperty(display, root_window, XInternAtom(display, backgroundProps[p], false),
                               0, 4, false, AnyPropertyType,
                               &actual_type, &actual_format, &items_count, &bytes_after, &prop) == Success &&
            actual_type == XInternAtom(display, "PIXMAP", false) && actual_format == 32 && items_count == 1) {
            memcpy(&pixmap, prop, 4);
            XFree(prop);
            fill = false;
            break;
        }
    }
    if (!pixmap) {
        pixmap = XCreatePixmap(display, root_window, 1, 1, XDefaultDepth(display, default_screen));
        fill = true;
    }

    XRenderPictureAttributes pa;
    pa.repeat = true;
    Picture picture = XRenderCreatePicture(display, pixmap,
                                           XRenderFindVisualFormat(display, XDefaultVisual(display, default_screen)),
                                           CPRepeat, &pa);
    if (fill) { // If no background is set, then will just fill the background with the color 0x8080
        XRenderColor c;
        c.red = c.green = c.blue = 0x8080;
        c.alpha = 0xffff;
        XRenderFillRectangle(display, PictOpSrc, picture, &c, 0, 0, 1, 1);
    }
    return picture;
}



// This draws the root_tile (desktop wallpaper) and draws it into the root_buffer
// which will later go into the actual root_picture
void paint_root() {
    if (!root_tile)
        root_tile = create_root_tile();

    XRenderComposite(display, PictOpSrc,
                     root_tile, 0, root_buffer,
                     0, 0, 0, 0, 0, 0, root_width, root_height);
}


XserverRegion client_extents(Client *client) {
    XRectangle r;
    r.x = client->attr.x;
    r.y = client->attr.y;
    r.width = client->attr.width + client->attr.border_width * 2;
    r.height = client->attr.height + client->attr.border_width * 2;
    return XFixesCreateRegion(display, &r, 1);
}


XserverRegion get_border_size(Client *client) {
    XserverRegion border;
    /*
     * if window doesn't exist anymore,  this will generate an error_handler
     * as well as not generate a region.  Perhaps a better XFixes
     * architecture would be to have a request that copies instead
     * of creates, that way you'd just end up with an empty region
     * instead of an invalid XID.
     */
    border = XFixesCreateRegionFromWindow(display, client->window, WindowRegionBounding);
    /* translate this */
    XFixesTranslateRegion(display, border,
                          client->attr.x + client->attr.border_width,
                          client->attr.y + client->attr.border_width);
    return border;
}



void paint_all(XserverRegion region) {
    if (!region) {
        XRectangle r;
        r.x = 0;
        r.y = 0;
        r.width = root_width;
        r.height = root_height;
        region = XFixesCreateRegion(display, &r, 1);
    }
    if (!root_buffer) {
        Pixmap rootPixmap = XCreatePixmap(display, root_window, root_width, root_height,
                                          XDefaultDepth(display, default_screen));
        root_buffer = XRenderCreatePicture(display, rootPixmap,
                                           XRenderFindVisualFormat(display, XDefaultVisual(display, default_screen)),
                                           0, NULL);
        XFreePixmap(display, rootPixmap);
    }
    XFixesSetPictureClipRegion(display, root_picture, 0, 0, region);

    /* for (Client *w : clients) { */
    for (size_t i = 0; i < cvector_size(clients); ++i) {
        Client *w = clients[i];
        /* never painted, ignore it */
        if (!w->damaged) {
            continue;
        }
        /* if invisible, ignore it */
        if (w->attr.x + w->attr.width < 1 || w->attr.y + w->attr.height < 1
            || w->attr.x >= root_width || w->attr.y >= root_height)
            continue;
        if (!w->picture) {
            XRenderPictureAttributes pa;
            XRenderPictFormat *format;
            Drawable draw = w->window;

            if (!w->pixmap)
                w->pixmap = XCompositeNameWindowPixmap(display, w->window);
            if (w->pixmap)
                draw = w->pixmap;

            format = XRenderFindVisualFormat(display, w->attr.visual);
            pa.subwindow_mode = IncludeInferiors;
            w->picture = XRenderCreatePicture(display, draw,
                                              format,
                                              CPSubwindowMode,
                                              &pa);
        }
        if (clip_changed) {
            if (w->border_size) {
                XFixesDestroyRegion(display, w->border_size);
                w->border_size = 0;
            }
            if (w->extents) {
                XFixesDestroyRegion(display, w->extents);
                w->extents = 0;
            }
            if (w->border_clip) {
                XFixesDestroyRegion(display, w->border_clip);
                w->border_clip = 0;
            }
        }
        if (w->border_size == 0)
            w->border_size = get_border_size(w);
        if (w->extents == 0)
            w->extents = client_extents(w);
        if (w->opaqueness == SOLID) {
            int x, y, wid, hei;

            x = w->attr.x;
            y = w->attr.y;
            wid = w->attr.width + w->attr.border_width * 2;
            hei = w->attr.height + w->attr.border_width * 2;

            XFixesSetPictureClipRegion(display, root_buffer, 0, 0, region);
            XFixesSubtractRegion(display, region, region, w->border_size);
            XRenderComposite(display, PictOpSrc, w->picture, 0, root_buffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        }
        if (!w->border_clip) {
            w->border_clip = XFixesCreateRegion(display, NULL, 0);
            XFixesCopyRegion(display, w->border_clip, region);
        }
    }

    XFixesSetPictureClipRegion(display, root_buffer, 0, 0, region);

    // This is the start of actually compositing the screen
    // this composites the root_tile which is the background image of your computer to the root_buffer.
    // If you didn't do this step, you would end up drawing the windows on top of themselves over and over
    // leading to a trailing effect
    //
    paint_root();

    // This is just a fancy for loop used in order to iterate through the
    // clients list in reverse order. The reason we do this is because the
    // clients list has the window that is at the top of the window hierarchy,
    // at the front of the list. Therefore we have to composite the windows in
    // reverse if we want the front item in the list to be rendered on top of
    // all other windows.

    /* for (int i = clients.size(); i--;) { */
    for (int i = cvector_size(clients) - 1; i >= 0; --i) {
        Client *w = clients[i];
        XFixesSetPictureClipRegion(display, root_buffer, 0, 0, w->border_clip);

        if (w->opaqueness == TRANSPARENT) {
            int x, y, wid, hei;
            XFixesIntersectRegion(display, w->border_clip, w->border_clip, w->border_size);
            XFixesSetPictureClipRegion(display, root_buffer, 0, 0, w->border_clip);

            x = w->attr.x;
            y = w->attr.y;
            wid = w->attr.width + w->attr.border_width * 2;
            hei = w->attr.height + w->attr.border_width * 2;

            XRenderComposite(display, PictOpOver, w->picture, w->alpha_pict, root_buffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        } else if (w->opaqueness == ARGB) {
            int x, y, wid, hei;
            XFixesIntersectRegion(display, w->border_clip, w->border_clip, w->border_size);
            XFixesSetPictureClipRegion(display, root_buffer, 0, 0, w->border_clip);

            x = w->attr.x;
            y = w->attr.y;
            wid = w->attr.width + w->attr.border_width * 2;
            hei = w->attr.height + w->attr.border_width * 2;

            XRenderComposite(display, PictOpOver, w->picture, w->alpha_pict, root_buffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        }
        XFixesDestroyRegion(display, w->border_clip);
        w->border_clip = 0;
    }
    XFixesDestroyRegion(display, region);
    if (root_buffer != root_picture) {
        XFixesSetPictureClipRegion(display, root_buffer, 0, 0, 0);
        XRenderComposite(display, PictOpSrc, root_buffer, 0, root_picture,
                         0, 0, 0, 0, 0, 0, root_width, root_height);
    }
}
//////////////////////////////////////////////////////////////////////////////////


void add_damage(XserverRegion damage) {
    if (all_damage) {
        XFixesUnionRegion(display, all_damage, all_damage, damage);
        XFixesDestroyRegion(display, damage);
    } else
        all_damage = damage;
}


void finish_unmap_client(Client *client) {
    client->damaged = 0;

    if (client->extents != 0) {
        add_damage(client->extents);    /* destroys region */
        client->extents = 0;
    }

    if (client->pixmap) {
        XFreePixmap(display, client->pixmap);
        client->pixmap = 0;
    }

    if (client->picture) {
        XRenderFreePicture(display, client->picture);
        client->picture = 0;
    }

    /* don't care about properties anymore */
    XSelectInput(display, client->window, 0);

    if (client->border_size) {
        XFixesDestroyRegion(display, client->border_size);
        client->border_size = 0;
    }
    if (client->border_clip) {
        XFixesDestroyRegion(display, client->border_clip);
        client->border_clip = 0;
    }

    clip_changed = true;
}


//////////////////////////////////////////////////////////////////////////////////


Client *get_client_from_window(Window id) {
    for (size_t i = 0; i < cvector_size(clients); ++i) {
        if (clients[i]->window == id)
            return clients[i];
    }
    return NULL;
}


void unmap_win(Window window) {
    Client *client = get_client_from_window(window);
    if (!client) return;
    client->attr.map_state = IsUnmapped;

    finish_unmap_client(client);
}


void determine_opaqueness(Client *client) {
    XRenderPictFormat *format;

    if (client->alpha_pict) {
        XRenderFreePicture(display, client->alpha_pict);
        client->alpha_pict = 0;
    }

    if (client->attr.class == InputOnly) {
        format = NULL;
    } else {
        format = XRenderFindVisualFormat(display, client->attr.visual);
    }

    enum Window_Opaqueness opaqueness;
    if (format && format->type == PictTypeDirect && format->direct.alphaMask) {
        opaqueness = ARGB;
    } else {
        opaqueness = SOLID;
    }
    client->opaqueness = opaqueness;
    if (client->extents) {
        XserverRegion damage;
        damage = XFixesCreateRegion(display, NULL, 0);
        XFixesCopyRegion(display, damage, client->extents);
        add_damage(damage);
    }
}

void map_win(Window window) {
    Client *client = get_client_from_window(window);

    if (!client) return;

    client->attr.map_state = IsViewable;

    determine_opaqueness(client);
    client->damaged = 0;
}

void add_client(Window window) {
    // Allocate memory for a new Client
    Client *client = (Client *)malloc(sizeof(Client));
    if (!client) {
        // Memory allocation failed
        return;
    }

    client->window = window;

    // Get window attributes
    if (!XGetWindowAttributes(display, window, &client->attr)) {
        // If getting attributes fails, free allocated memory
        free(client);
        return;
    }

    client->shaped = false;
    client->shape_bounds.x = client->attr.x;
    client->shape_bounds.y = client->attr.y;
    client->shape_bounds.width = client->attr.width;
    client->shape_bounds.height = client->attr.height;
    client->damaged = 0;

    client->pixmap = 0;
    client->picture = 0;

    if (client->attr.class == InputOnly) {
        client->damage = 0;
    } else {
        client->damage = XDamageCreate(display, window, XDamageReportNonEmpty);
        XShapeSelectInput(display, window, ShapeNotifyMask);
    }

    client->alpha_pict = 0;
    client->border_size = 0;
    client->extents = 0;
    client->border_clip = 0;

    // Add the new client to the beginning of the vector
    cvector_insert(clients, 0, client);

    if (client->attr.map_state == IsViewable) {
        map_win(window);
    }
}




void restack_win(Window moving_window, Window target_window) {
    //  The moving_window wants to be placed in front
    // of the target_window and we shall do just that
    Client *moving_client = NULL;
    int moving_client_index = -1;

    // Find the moving client
    for (size_t i = 0; i < cvector_size(clients); ++i) {
        if (clients[i]->window == moving_window) {
            moving_client = clients[i];
            moving_client_index = i;
            break;
        }
    }

    // If the moving client was found, remove it from its current position
    if (moving_client != NULL && moving_client_index != -1) {
        cvector_erase(clients, moving_client_index);

        // If a target window is specified, insert the moving client before it
        if (target_window != 0) {
            for (size_t i = 0; i < cvector_size(clients); ++i) {
                if (clients[i]->window == target_window) {
                    cvector_insert(clients, i, moving_client);
                    return;
                }
            }
        } else { // The moving client wants to go to the bottom of the list
            cvector_push_back(clients, moving_client);
        }
    }
}


void configure_client(XConfigureEvent *ce) {
    Client *client = get_client_from_window(ce->window);

    if (client == NULL) {
        if (ce->window == root_window) {
            if (root_buffer != 0) {
                XRenderFreePicture(display, root_buffer);
                root_buffer = 0;
            }
            root_width = ce->width;
            root_height = ce->height;
        }
        return;
    }

    XserverRegion damage = XFixesCreateRegion(display, NULL, 0);
    if (client->extents != 0)
        XFixesCopyRegion(display, damage, client->extents);

    client->shape_bounds.x -= client->attr.x;
    client->shape_bounds.y -= client->attr.y;
    client->attr.x = ce->x;
    client->attr.y = ce->y;
    if (client->attr.width != ce->width || client->attr.height != ce->height) {
        if (client->pixmap) {
            XFreePixmap(display, client->pixmap);
            client->pixmap = 0;
            if (client->picture) {
                XRenderFreePicture(display, client->picture);
                client->picture = 0;
            }
        }
    }
    client->attr.width = ce->width;
    client->attr.height = ce->height;
    client->attr.border_width = ce->border_width;
    client->attr.override_redirect = ce->override_redirect;

    restack_win(ce->window, ce->above);

    if (damage) {
        XserverRegion extents = client_extents(client);
        XFixesUnionRegion(display, damage, damage, extents);
        XFixesDestroyRegion(display, extents);
        add_damage(damage);
    }
    client->shape_bounds.x += client->attr.x;
    client->shape_bounds.y += client->attr.y;
    if (!client->shaped) {
        client->shape_bounds.width = client->attr.width;
        client->shape_bounds.height = client->attr.height;
    }

    clip_changed = true;
}


void circulate_client(XCirculateEvent *ce) {
    Client *client = get_client_from_window(ce->window);

    if (!client) return;

    Window target_window;
    if (ce->place == PlaceOnTop)
        target_window = clients[0]->window;
    else if (ce->place == PlaceOnBottom)
        target_window = 0;

    restack_win(client->window, target_window);
    clip_changed = true;
}



void destroy_win(Window window, bool gone) {
    size_t i;
    for (i = 0; i < cvector_size(clients); ++i) {
        Client *w = clients[i];
        if (w->window == window) {
            if (gone) {
                finish_unmap_client(w);
            }
            // Clean up resources owned by the client
            if (w->picture) {
                XRenderFreePicture(display, w->picture);
                w->picture = 0;
            }
            if (w->alpha_pict) {
                XRenderFreePicture(display, w->alpha_pict);
                w->alpha_pict = 0;
            }
            if (w->damage != 0) {
                XDamageDestroy(display, w->damage);
                w->damage = 0;
            }
            // More cleanup can be added here if needed

            break;
        }
    }
    if (i < cvector_size(clients)) {
        // Perform any additional cleanup needed for the client before removing
        // e.g., free(w->some_allocated_field);

        // Remove the client from the vector
        cvector_erase(clients, i);
    }
}

void damage_client(XDamageNotifyEvent *de) {
    Client *client = get_client_from_window(de->drawable);

    if (!client) return;

    XserverRegion parts;
    if (!client->damaged) {
        parts = client_extents(client);
        XDamageSubtract(display, client->damage, 0, 0);
    } else {
        parts = XFixesCreateRegion(display, NULL, 0);
        XDamageSubtract(display, client->damage, 0, parts);
        XFixesTranslateRegion(display, parts,
                              client->attr.x + client->attr.border_width,
                              client->attr.y + client->attr.border_width);
    }
    add_damage(parts);
    client->damaged = 1;
}


void shape_win(XShapeEvent *se) {
    Client *client = get_client_from_window(se->window);

    if (!client) return;

    if (se->kind == ShapeClip || se->kind == ShapeBounding) {
        XserverRegion region0;
        XserverRegion region1;
        clip_changed = true;

        region0 = XFixesCreateRegion(display, &client->shape_bounds, 1);

        if (se->shaped) {
            client->shaped = true;
            client->shape_bounds.x = client->attr.x + se->x;
            client->shape_bounds.y = client->attr.y + se->y;
            client->shape_bounds.width = se->width;
            client->shape_bounds.height = se->height;
        } else {
            client->shaped = false;
            client->shape_bounds.x = client->attr.x;
            client->shape_bounds.y = client->attr.y;
            client->shape_bounds.width = client->attr.width;
            client->shape_bounds.height = client->attr.height;
        }

        region1 = XFixesCreateRegion(display, &client->shape_bounds, 1);
        XFixesUnionRegion(display, region0, region0, region1);
        XFixesDestroyRegion(display, region1);

        /* ask for repaint of the old and new region */
        paint_all(region0);
    }
}


int error_handler(Display *dpy, XErrorEvent *ev) {
    // You should do something here but we do nothing when an error happens
    //
    // abort();
    return 0;
}

void expose_root(cvector(XRectangle *) rectangles) {
    if (cvector_size(rectangles) > 0) {
        // Assuming rectangles is a cvector of XRectangle pointers
        XserverRegion region = XFixesCreateRegion(display, rectangles[0], cvector_size(rectangles));
        add_damage(region);
    }
}


// If you are making a windows manager with a compositor
// and not just a compositor, then this isn't that relevant
bool register_as_the_composite_manager() {
    Window w;
    Atom a;
    char net_wm_cm[20];  // Ensure this is large enough for "_NET_WM_CM_Sxx" and the screen number.

    snprintf(net_wm_cm, sizeof(net_wm_cm), "_NET_WM_CM_S%d", default_screen);
    a = XInternAtom(display, net_wm_cm, False);

    w = XGetSelectionOwner(display, a);
    if (w != 0) {
        XTextProperty tp;
        char **strs;
        int count;
        Atom winNameAtom = XInternAtom(display, "_NET_WM_NAME", False);

        if (!XGetTextProperty(display, w, &tp, winNameAtom) &&
            !XGetTextProperty(display, w, &tp, XA_WM_NAME)) {
            fprintf(stderr,
                    "Another composite manager is already running (0x%lx)\n",
                    (unsigned long) w);
            return false;
        }
        if (XmbTextPropertyToTextList(display, &tp, &strs, &count) == Success) {
            fprintf(stderr,
                    "Another composite manager is already running (%s)\n",
                    strs[0]);

            XFreeStringList(strs);
        }

        XFree(tp.value);

        return false;
    }

    w = XCreateSimpleWindow(display, RootWindow(display, default_screen), 0, 0, 1, 1, 0, 0, 0);
    Xutf8SetWMProperties(display, w, "xcompmgr", "xcompmgr", NULL, 0, NULL, NULL, NULL);
    XSetSelectionOwner(display, a, w, 0);

    return true;
}



int main(int argc, char **argv) {
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Can't open display\n");
        exit(1);
    }

    XSetErrorHandler(error_handler);
    XSynchronize(display, 1);

    default_screen = XDefaultScreen(display);
    root_window = XRootWindow(display, default_screen);
    root_width = XDisplayWidth(display, default_screen);
    root_height = XDisplayHeight(display, default_screen);

    if (!XRenderQueryExtension(display, &render_event, &render_error)) {
        fprintf(stderr, "No render extension\n");
        exit(1);
    }
    if (!XQueryExtension(display, COMPOSITE_NAME, &composite_opcode,
                         &composite_event, &composite_error)) {
        fprintf(stderr, "No composite extension\n");
        exit(1);
    }

    int composite_major, composite_minor;
    XCompositeQueryVersion(display, &composite_major, &composite_minor);
    if (composite_major <= 0 && composite_minor < 2) {
        fprintf(stderr, "Current composite extension version is too low\n");
        exit(1);
    }

    if (!XDamageQueryExtension(display, &damage_event, &damage_error)) {
        fprintf(stderr, "No damage extension\n");
        exit(1);
    }

    if (!XFixesQueryExtension(display, &xfixes_event, &xfixes_error)) {
        fprintf(stderr, "No XFixes extension\n");
        exit(1);
    }

    if (!XShapeQueryExtension(display, &xshape_event, &xshape_error)) {
        fprintf(stderr, "No XShape extension\n");
        exit(1);
    }

    if (!register_as_the_composite_manager()) {
        exit(1);
    }

    opacity_atom = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);

    XRenderPictureAttributes pa;
    pa.subwindow_mode = IncludeInferiors;
    root_picture = XRenderCreatePicture(display, root_window,
                                        XRenderFindVisualFormat(display, XDefaultVisual(display, default_screen)),
                                        CPSubwindowMode, &pa);
    all_damage = 0;
    clip_changed = true;

    XCompositeRedirectSubwindows(display, root_window, CompositeRedirectManual);
    XSelectInput(display, root_window, SubstructureNotifyMask | ExposureMask | StructureNotifyMask | PropertyChangeMask);
    XShapeSelectInput(display, root_window, ShapeNotifyMask);

    XGrabServer(display);
    Window *children;
    unsigned int children_count;
    Window root_return, parent_return;
    XQueryTree(display, root_window, &root_return, &parent_return, &children, &children_count);
    for (int i = 0; i < children_count; i++) {
        add_client(children[i]);
    }
    XFree(children);
    XUngrabServer(display);

    // Note: The handling of root_expose_rects needs to be adapted from C++ std::vector to a C equivalent.
    // Assuming you have defined a suitable data structure or array for root_expose_rects

    paint_all(0);

    XEvent ev;
    while (1) {
        do {
            XNextEvent(display, &ev);
            switch (ev.type) {
                case CreateNotify:
                    add_client(ev.xcreatewindow.window);
                    break;
                case ConfigureNotify:
                    configure_client(&ev.xconfigure);
                    break;
                case DestroyNotify:
                    destroy_win(ev.xdestroywindow.window, 1);
                    break;
                case MapNotify:
                    map_win(ev.xmap.window);
                    break;
                case UnmapNotify:
                    unmap_win(ev.xunmap.window);
                    break;
                case ReparentNotify:
                    if (ev.xreparent.parent == root_window) {
                        add_client(ev.xreparent.window);
                    } else {
                        destroy_win(ev.xreparent.window, 0);
                    }
                    break;
                case CirculateNotify:
                    circulate_client(&ev.xcirculate);
                    break;
                case Expose:
                    // Adapt the handling of expose events for root_expose_rects
                    break;
                case PropertyNotify:
                    // Handle property notifications
                    break;
                default:
                    if (ev.type == damage_event + XDamageNotify) {
                        damage_client((XDamageNotifyEvent *) &ev);
                    } else if (ev.type == xshape_event + ShapeNotify) {
                        shape_win((XShapeEvent *) &ev);
                    }
                    break;
            }
        } while (XQLength(display));

        if (all_damage != 0) {
            paint_all(all_damage);
            XSync(display, False);
            all_damage = 0;
            clip_changed = false;
        }
    }

    return 0;
}
