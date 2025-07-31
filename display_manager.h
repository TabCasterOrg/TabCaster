#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <stdbool.h>

// Information about a single monitor
typedef struct {
    char name[32];          // Monitor name (e.g., "HDMI-1", "eDP-1")
    unsigned int width;     // Screen width in pixels
    unsigned int height;    // Screen height in pixels
    int x;                  // X position on virtual desktop
    int y;                  // Y position on virtual desktop
    bool connected;         // Is monitor connected?
    bool primary;           // Is this the primary monitor?
    RROutput output_id;     // X11 output identifier
    RRCrtc crtc_id;        // X11 CRTC identifier
} ScreenInfo;

// Main structure for managing X11 display and monitors
typedef struct {
    Display *display;              // Connection to X11 server
    Window root;                   // Root window (desktop)
    int screen;                    // Default screen number
    XRRScreenResources *resources; // XRandR screen resources (outputs/monitors)
    ScreenInfo *screens;           // Array of monitor info (all outputs)
    int screen_count;              // Total number of outputs (connected + disconnected)
} DisplayManager;

// Core functions
DisplayManager* dm_init(void);                    // Initialize display manager - returns NULL on failure
int dm_get_screens(DisplayManager *dm);           // Get all screen info - returns number of connected monitors, -1 on error
void dm_print_screens(DisplayManager *dm);        // Print monitor info to stdout
void dm_cleanup(DisplayManager *dm);              // Clean up resources - safe to call with NULL

// Utility functions for working with screen data
int dm_count_connected_screens(DisplayManager *dm);     // Count currently connected screens
int dm_count_disconnected_screens(DisplayManager *dm);  // Count currently disconnected screens
ScreenInfo* dm_get_primary_screen(DisplayManager *dm);  // Get pointer to primary screen (NULL if none)

#endif