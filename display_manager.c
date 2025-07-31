#include "display_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Initialize display manager and connect to X11
DisplayManager* dm_init(void) {
    DisplayManager *dm = calloc(1, sizeof(DisplayManager));
    if (!dm) return NULL;
    
    // Open X display, NULL means default display
    dm->display = XOpenDisplay(NULL); 
    if (!dm->display) {
        fprintf(stderr, "Cannot open X display\n");
        free(dm);
        return NULL;
    }
    
    dm->screen = DefaultScreen(dm->display);
    dm->root = RootWindow(dm->display, dm->screen); // Desktop background
    
    // Get XRandR resources for monitor enumeration
    dm->resources = XRRGetScreenResources(dm->display, dm->root);
    if (!dm->resources) {
        fprintf(stderr, "XRRGetScreenResources failed\n");
        XCloseDisplay(dm->display);
        free(dm);
        return NULL;
    }
    
    return dm;
}

// Extract geometry information from CRTC
static void extract_geometry(DisplayManager *dm, ScreenInfo *screen, RRCrtc crtc) {
    XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dm->display, dm->resources, crtc);
    if (crtc_info) {
        screen->x = crtc_info->x;
        screen->y = crtc_info->y;
        screen->width = crtc_info->width;
        screen->height = crtc_info->height;
        XRRFreeCrtcInfo(crtc_info);
    }
}

// Check if output is the primary monitor
static bool is_primary_output(DisplayManager *dm, RROutput output_id) {
    RROutput primary = XRRGetOutputPrimary(dm->display, dm->root);
    return (primary == output_id);
}

// Populate a single ScreenInfo structure from XRROutputInfo
static void populate_screen_info(DisplayManager *dm, ScreenInfo *screen, 
                                RROutput output_id, XRROutputInfo *output_info) {
    // Basic information
    snprintf(screen->name, sizeof(screen->name), "%s", output_info->name);
    screen->output_id = output_id;
    screen->connected = (output_info->connection == RR_Connected);
    
    // Only get geometry and primary status for connected monitors
    if (screen->connected && output_info->crtc) {
        screen->crtc_id = output_info->crtc;
        extract_geometry(dm, screen, output_info->crtc);
        screen->primary = is_primary_output(dm, output_id);
    } else {
        // Initialize disconnected monitor fields
        screen->crtc_id = 0;
        screen->x = screen->y = 0;
        screen->width = screen->height = 0;
        screen->primary = false;
    }
}

// Allocate screen array for all outputs
static int allocate_screen_array(DisplayManager *dm) {
    int array_size = dm->resources->noutput;
    
    if (array_size <= 0) return 0;
    
    dm->screens = calloc(array_size, sizeof(ScreenInfo));
    return dm->screens ? array_size : -1;
}

// Utility function to count connected screens (after population)
int dm_count_connected_screens(DisplayManager *dm) {
    if (!dm || !dm->screens) return 0;
    
    int connected = 0;
    for (int i = 0; i < dm->screen_count; i++) {
        if (dm->screens[i].connected) {
            connected++;
        }
    }
    return connected;
}

// Utility function to count disconnected screens
int dm_count_disconnected_screens(DisplayManager *dm) {
    if (!dm || !dm->screens) return 0;
    
    return dm->screen_count - dm_count_connected_screens(dm);
}

// Get pointer to primary screen (returns NULL if no primary found)
ScreenInfo* dm_get_primary_screen(DisplayManager *dm) {
    if (!dm || !dm->screens) return NULL;
    
    for (int i = 0; i < dm->screen_count; i++) {
        if (dm->screens[i].connected && dm->screens[i].primary) {
            return &dm->screens[i];
        }
    }
    return NULL;
}

// Main function - enumerate and populate all screens
int dm_get_screens(DisplayManager *dm) {
    if (!dm || !dm->resources) return -1;
    
    // Allocate space for all outputs
    int max_screens = allocate_screen_array(dm);
    if (max_screens <= 0) return max_screens;
    
    dm->screen_count = 0;
    
    // Process each output
    for (int i = 0; i < dm->resources->noutput; i++) {
        XRROutputInfo *output_info = XRRGetOutputInfo(dm->display, 
                                                      dm->resources, 
                                                      dm->resources->outputs[i]);
        if (!output_info) continue;
        
        ScreenInfo *screen = &dm->screens[dm->screen_count];
        populate_screen_info(dm, screen, dm->resources->outputs[i], output_info);
        
        XRRFreeOutputInfo(output_info);
        dm->screen_count++;
    }
    
    // Return count of connected screens (calculated after population)
    return dm_count_connected_screens(dm);
}

// Print all outputs and their connection status
void dm_print_screens(DisplayManager *dm) {
    if (!dm || !dm->screens) return;
    
    printf("All outputs:\n");
    for (int i = 0; i < dm->screen_count; i++) {
        ScreenInfo *s = &dm->screens[i];
        
        if (s->connected) {
            printf("  %s: %dx%d+%d+%d%s [CONNECTED]\n", 
                   s->name, s->width, s->height, s->x, s->y,
                   s->primary ? " (primary)" : "");
        } else {
            printf("  %s: [DISCONNECTED]\n", s->name);
        }
    }
}

// Free all resources
void dm_cleanup(DisplayManager *dm) {
    if (!dm) return;
    
    if (dm->screens) free(dm->screens);
    if (dm->resources) XRRFreeScreenResources(dm->resources);
    if (dm->display) XCloseDisplay(dm->display);
    free(dm);
}