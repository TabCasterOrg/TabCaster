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

// Enumerate connected monitors and get their info
int dm_get_screens(DisplayManager *dm) {
    if (!dm || !dm->resources) return -1;
    
    // Allocate space for monitor info
    dm->screens = calloc(dm->resources->noutput, sizeof(ScreenInfo));
    if (!dm->screens) return -1;
    
    dm->screen_count = 0;
    int connected_count = 0;  // Track only connected monitors
    
    // Check each available output
    for (int i = 0; i < dm->resources->noutput; i++) {
        XRROutputInfo *output_info = XRRGetOutputInfo(dm->display, 
                                                      dm->resources, 
                                                      dm->resources->outputs[i]);
        if (!output_info) continue;
        
        ScreenInfo *screen = &dm->screens[dm->screen_count];
        
        // Store basic info
        strncpy(screen->name, output_info->name, sizeof(screen->name) - 1);
        screen->name[sizeof(screen->name) - 1] = '\0';
        screen->output_id = dm->resources->outputs[i];
        screen->connected = (output_info->connection == RR_Connected);
        
        // Only increment connected count for actually connected monitors
        if (screen->connected) {
            connected_count++;
            
            // Get geometry if has CRTC
            if (output_info->crtc) {
                screen->crtc_id = output_info->crtc;
                
                XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dm->display, 
                                                        dm->resources, 
                                                        output_info->crtc);
                if (crtc_info) {
                    screen->x = crtc_info->x;
                    screen->y = crtc_info->y;
                    screen->width = crtc_info->width;
                    screen->height = crtc_info->height;
                    XRRFreeCrtcInfo(crtc_info);
                }
                
                // Check if primary monitor
                RROutput primary = XRRGetOutputPrimary(dm->display, dm->root);
                screen->primary = (primary == screen->output_id);
            }
        }
        
        XRRFreeOutputInfo(output_info);
        dm->screen_count++;
    }
    
    return connected_count;  // Return only connected count
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