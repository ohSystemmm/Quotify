#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>

#include "../cairo_draw_text.h"
#include "../log.h"
#include "../options.h"

// generated function: returns XEvent name
const char *XEventName(int type);
// check if compositor is running
static bool compositor_check(Display *d, int screen)
{
    char prop_name[16];
    snprintf(prop_name, 16, "_NET_WM_CM_S%d", screen);
    Atom prop_atom = XInternAtom(d, prop_name, False);
    return XGetSelectionOwner(d, prop_atom) != None;
}

int x11_backend_start(void)
{
    __debug__("Opening display\n");
    Display *d = XOpenDisplay(NULL);
    __debug__("Finding root window\n");
    Window root = DefaultRootWindow(d);
    __debug__("Finding default screen\n");
    int default_screen = XDefaultScreen(d);

    __debug__("Checking compositor\n");
    bool compositor_running = compositor_check(d, XDefaultScreen(d));
    if (!compositor_running)
    {
        __info__("No running compositor detected. Program may not work as intended\n");
    }
	if (options.force_xshape == true) {
		__debug__("Forcing XShape\n");
		compositor_running = false;
	}
    // https://x.org/releases/current/doc/man/man3/Xinerama.3.xhtml
    __debug__("Finding all screens in use using Xinerama\n");
    int num_entries = 0;
    XineramaScreenInfo *si = XineramaQueryScreens(d, &num_entries);
    // if xinerama fails
    if (si == NULL)
    {
        __perror__(
            "Required X extension Xinerama is not active. It is needed for displaying watermark on multiple screens");
        XCloseDisplay(d);
        return 1;
    }
    __debug__("Found %d screen(s)\n", num_entries);

    // https://cgit.freedesktop.org/xorg/proto/randrproto/tree/randrproto.txt
    __debug__("Initializing Xrandr\n");
    int xrr_error_base;
    int xrr_event_base;
    if (!XRRQueryExtension(d, &xrr_event_base, &xrr_error_base))
    {
        __perror__("Required X extension Xrandr is not active. It is needed for handling screen size change (e.g. in "
                   "virtual machine window)");
        XFree(si);
        XCloseDisplay(d);
        return 1;
    }
    __debug__("Subscribing on screen change events\n");
    XRRSelectInput(d, root, RRScreenChangeNotifyMask);

    XSetWindowAttributes attrs;
    attrs.override_redirect = 1;

    XVisualInfo vinfo;

    // MacOS doesn't support 32 bit color through XQuartz, massive hack
#ifdef __APPLE__
    int colorDepth = 24;
#else
    int colorDepth = 32;
#endif

    __debug__("Checking default screen to be %d bit color depth\n", colorDepth);
    if (!XMatchVisualInfo(d, default_screen, colorDepth, TrueColor, &vinfo))
    {
        __error__("No screens supporting %i bit color found, terminating\n", colorDepth);
        exit(EXIT_FAILURE);
    }

    __debug__("Set %d bit color depth\n", colorDepth);
    attrs.colormap = XCreateColormap(d, root, vinfo.visual, AllocNone);
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;

    Window overlay[num_entries];
    cairo_surface_t *surface[num_entries];
    cairo_t *cairo_ctx[num_entries];

    Pixmap xshape_mask[num_entries];
    cairo_surface_t *xshape_surface[num_entries];
    cairo_t *xshape_ctx[num_entries];

    int overlay_height = options.overlay_height * options.scale;
    __debug__("Scaled height: %d px\n", overlay_height);
    int overlay_width = options.overlay_width * options.scale;
    __debug__("Scaled width:  %d px\n", overlay_width);

    for (int i = 0; i < num_entries; i++)
    {
        __debug__("Creating overlay on %d screen\n", i);
        overlay[i] = XCreateWindow(d,                                                                // display
                                   root,                                                             // parent
                                   si[i].x_org + si[i].width + options.offset_left - overlay_width,  // x position
                                   si[i].y_org + si[i].height + options.offset_top - overlay_height, // y position
                                   overlay_width,                                                    // width
                                   overlay_height,                                                   // height
                                   0,                                                                // border width
                                   vinfo.depth,                                                      // depth
                                   InputOutput,                                                      // class
                                   vinfo.visual,                                                     // visual
                                   CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel,    // value mask
                                   &attrs                                                            // attributes
        );
        // subscribe to Exposure Events, required for redrawing after DPMS blanking
        XSelectInput(d, overlay[i], ExposureMask);
        XMapWindow(d, overlay[i]);

        // allows the mouse to click through the overlay
        XRectangle rect;
        XserverRegion region = XFixesCreateRegion(d, &rect, 1);
        XFixesSetWindowShapeRegion(d, overlay[i], ShapeInput, 0, 0, region);
        XFixesDestroyRegion(d, region);

        // sets a WM_CLASS to allow the user to blacklist some effect from compositor
        XClassHint *xch = XAllocClassHint();
        xch->res_name = "activate-linux";
        xch->res_class = "activate-linux";
        XSetClassHint(d, overlay[i], xch);

        // set _NET_WM_BYPASS_COMPOSITOR
        // https://specifications.freedesktop.org/wm-spec/wm-spec-latest.html#idm45446104333040
        if (options.bypass_compositor)
        {
            __debug__("Bypassing compositor\n");
            unsigned char data = 1;
            XChangeProperty(d, overlay[i], XInternAtom(d, "_NET_WM_BYPASS_COMPOSITOR", False), XA_CARDINAL, 32,
                            PropModeReplace, &data, 1);
        }

        if (options.gamescope_overlay)
        {
            // https://github.com/Plagman/gamescope/issues/288
            // https://github.com/flightlessmango/MangoHud/blob/9a6809daca63cf6860ac9d92ae4b2dde36239b0e/src/app/main.cpp#L47
            // https://github.com/flightlessmango/MangoHud/blob/9a6809daca63cf6860ac9d92ae4b2dde36239b0e/src/app/main.cpp#L189
            // https://github.com/trigg/Discover/blob/de83063f3452b1cdee89b4c3779103eae2c90cbb/discover_overlay/overlay.py#L107

            __debug__("Setting GAMESCOPE_EXTERNAL_OVERLAY\n");
            unsigned char data = 1;
            XChangeProperty(d, overlay[i], XInternAtom(d, "GAMESCOPE_EXTERNAL_OVERLAY", False), XA_CARDINAL, 32,
                            PropModeReplace, &data, 1);
        }

        __debug__("Creating cairo context\n");
        surface[i] = cairo_xlib_surface_create(d, overlay[i], vinfo.visual, overlay_width, overlay_height);
        cairo_ctx[i] = cairo_create(surface[i]);

        __debug__("Creating cario surface/context and mask pixmap for XShape support\n");
        if (!compositor_running)
        {
            xshape_mask[i] = XCreatePixmap(d, overlay[i], overlay_width, overlay_height, 1);
            xshape_surface[i] = cairo_xlib_surface_create_for_bitmap(d, xshape_mask[i], DefaultScreenOfDisplay(d),
                                                                     overlay_width, overlay_height);
            xshape_ctx[i] = cairo_create(xshape_surface[i]);
        }
    }

    __info__("All done. Going into X windows event endless loop\n\n");
    XEvent event;
    while (1)
    {
        XNextEvent(d, &event);
        // handle screen resize via catching Xrandr event
        if (XRRUpdateConfiguration(&event))
        {
            if (event.type - xrr_event_base == RRScreenChangeNotify)
            {
                __debug__("! Got Xrandr event about screen change\n");
                __debug__("  Updating info about screen sizes\n");
                si = XineramaQueryScreens(d, &num_entries);
                for (int i = 0; i < num_entries; i++)
                {
                    __debug__("  Moving window on screen %d according new position\n", i);
                    XMoveWindow(d,                                                               // display
                                overlay[i],                                                      // window
                                si[i].x_org + si[i].width + options.offset_left - overlay_width, // x position
                                si[i].y_org + si[i].height + options.offset_top - overlay_height // y position
                    );
                }
            }
            else
            {
                __debug__("! Got Xrandr event, type: %d (0x%X)\n", event.type - xrr_event_base,
                          event.type - xrr_event_base);
            }
        }
        else if (event.type == Expose)
        {
            /*
             * See https://www.x.org/releases/X11R7.5/doc/man/man3/XExposeEvent.3.html
             * removed draw_text() call from elsewhere because XExposeEvent is emitted
             * on both window init and window damage.
             */

            __debug__("! Got X event, type: %s (0x%X)\n", XEventName(event.type), event.type);
            for (int i = 0; i < num_entries && event.xexpose.count == 0; i++)
            {
                if (overlay[i] == event.xexpose.window)
                {
                    __debug__("  Redrawing overlay: %d\n", i);

                    if (!compositor_running)
                    {
                        __debug__("Shaping window %d using XShape\n", i);
                        draw_text(cairo_ctx[i], 2);
                        draw_text(xshape_ctx[i], 1);
                        XShapeCombineMask(d, overlay[i], ShapeBounding, 0, 0,
                                          cairo_xlib_surface_get_drawable(xshape_surface[i]), ShapeSet);
                    } else {
			 draw_text(cairo_ctx[i], 0);
		    }
                    break;
                }
            }
        }
        else
        {
            __debug__("! Got X event, type: %s (0x%X)\n", XEventName(event.type), event.type);
        }
    }

    // free used resources
    for (int i = 0; i < num_entries; i++)
    {
        XUnmapWindow(d, overlay[i]);
        cairo_destroy(cairo_ctx[i]);
        cairo_surface_destroy(surface[i]);
    }
	
    if (!compositor_running)
    {
  	  for (int i = 0; i < num_entries; i++)
   	  {
    	    XFreePixmap(d, xshape_mask[i]);
            cairo_destroy(xshape_ctx[i]);                                
        	cairo_surface_destroy(xshape_surface[i]);
	  }                  
    }
	
    XFree(si);
    XCloseDisplay(d);

    return 0;
}

int x11_backend_kill_running(void)
{
    __error__("x11_backend_kill_running currently is not implemented\n");
    return 1;
}
