#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Global flag for tracking X11 errors during mode operations
static volatile int x11_operation_failed = 0;

// Temp error handler that sets a flag for critical operations (file-local)
static int ignore_badmatch_with_flag(Display *d, XErrorEvent *e) {
    if (e->error_code == BadMatch) {
        printf("Found an X11 BadMatch error. Continuing.\n");
        x11_operation_failed = 1; // Set flag
        return 0; // swallow it
    }
    if (e->error_code == BadName) {
        printf("Found an X11 BadName error. Continuing.\n");
        x11_operation_failed = 1; // Set flag
        return 0; // swallow it
    }

    return 0; // let other errors just pass silently
}

// Helper function to reset error flag and install handler
static void reset_x11_error_tracking() {
    x11_operation_failed = 0;
    XSetErrorHandler(ignore_badmatch_with_flag);
}

// Helper function to check if operation failed
static int check_x11_operation_result() {
    return x11_operation_failed;
}

// Convert libxcvt_mode_info to XRRModeInfo
static void convert_libxcvt_to_xrr(const struct libxcvt_mode_info *cvt_mode, XRRModeInfo *xrr_mode, const char *mode_name) {
    memset(xrr_mode, 0, sizeof(XRRModeInfo));
    
    // Basic properties
    xrr_mode->width = cvt_mode->hdisplay;
    xrr_mode->height = cvt_mode->vdisplay;
    xrr_mode->dotClock = (unsigned long)(cvt_mode->dot_clock * 1000); // Convert kHz to Hz
    
    // Horizontal timing
    xrr_mode->hSyncStart = cvt_mode->hsync_start;
    xrr_mode->hSyncEnd = cvt_mode->hsync_end; 
    xrr_mode->hTotal = cvt_mode->htotal;
    
    // Vertical timing
    xrr_mode->vSyncStart = cvt_mode->vsync_start;
    xrr_mode->vSyncEnd = cvt_mode->vsync_end;
    xrr_mode->vTotal = cvt_mode->vtotal;
    
    // Sync polarity flags
    xrr_mode->modeFlags = 0;
    if (cvt_mode->mode_flags & LIBXCVT_MODE_FLAG_HSYNC_POSITIVE) {
        xrr_mode->modeFlags |= RR_HSyncPositive;
    } else {
        xrr_mode->modeFlags |= RR_HSyncNegative;
    }
    
    if (cvt_mode->mode_flags & LIBXCVT_MODE_FLAG_VSYNC_POSITIVE) {
        xrr_mode->modeFlags |= RR_VSyncPositive;
    } else {
        xrr_mode->modeFlags |= RR_VSyncNegative;
    }
    
    // Mode name
    xrr_mode->name = (char *)mode_name;
    xrr_mode->nameLength = strlen(mode_name);
}

// Find an available CRTC that's not currently in use
static RRCrtc find_available_crtc(DisplayManager *dm) {
    RRCrtc best_crtc = None;
    
    for (int i = 0; i < dm->resources->ncrtc; i++) {
        RRCrtc crtc = dm->resources->crtcs[i];
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dm->display, dm->resources, crtc);
        
        if (crtc_info) {
            // Prefer CRTCs with no outputs (original logic)
            if (crtc_info->noutput == 0) {
                XRRFreeCrtcInfo(crtc_info);
                return crtc; // Return immediately if we find a completely free CRTC
            }
            
            // Fallback: any CRTC can potentially be used (store the first one)
            if (best_crtc == None) {
                best_crtc = crtc;
            }
            
            XRRFreeCrtcInfo(crtc_info);
        }
    }
    
    // If no completely free CRTC found, return the first available CRTC
    // XRandR can often reassign CRTCs as needed
    if (best_crtc != None) {
        printf("No free CRTC found, using CRTC that may be reassigned\n");
    }
    
    return best_crtc;
}

// Find output ID by name
static RROutput find_output_by_name(DisplayManager *dm, const char *output_name) {
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, output_name) == 0) {
            return dm->screens[i].output_id;
        }
    }
    return None;
}

// Get mode dimensions from mode ID 
static int get_mode_dimensions(DisplayManager *dm, RRMode mode_id, unsigned int *width, unsigned int *height) {
    if (!dm || mode_id == 0 || !width || !height) return -1;
    
    // First try the cached resources
    for (int i = 0; i < dm->resources->nmode; i++) {
        XRRModeInfo *mode_info = &dm->resources->modes[i];
        if (mode_info->id == mode_id) {
            *width = mode_info->width;
            *height = mode_info->height;
            return 0;
        }
    }
    
    // If not found in cached resources, get fresh resources
    XRRScreenResources *fresh_resources = XRRGetScreenResources(dm->display, dm->root);
    if (!fresh_resources) return -1;
    
    for (int i = 0; i < fresh_resources->nmode; i++) {
        XRRModeInfo *mode_info = &fresh_resources->modes[i];
        if (mode_info->id == mode_id) {
            *width = mode_info->width;
            *height = mode_info->height;
            XRRFreeScreenResources(fresh_resources);
            return 0;
        }
    }
    
    XRRFreeScreenResources(fresh_resources);
    return -1; // Mode not found
}

// Calculate position for --right-of placement relative to primary screen
int mode_calculate_right_of_position(DisplayManager *dm, int *x, int *y, 
                                     unsigned int width, unsigned int height) {
    if (!dm || !x || !y) return -1;
    
    ScreenInfo *primary = dm_get_primary_screen(dm);
    if (primary) {
        // Place to the right of primary screen
        *x = primary->x + (int)primary->width;
        *y = primary->y;  // Align vertically with primary
        printf("Positioning right of primary screen '%s': %d,%d\n", primary->name, *x, *y);
    } else {
        // Fallback: find rightmost active screen and place to its right
        int rightmost_x = 0;
        unsigned int rightmost_width = 0;
        bool found_active = false;
        
        for (int i = 0; i < dm->screen_count; i++) {
            if (dm->screens[i].connected && dm->screens[i].width > 0) {
                int screen_right = dm->screens[i].x + (int)dm->screens[i].width;
                if (!found_active || screen_right > (rightmost_x + (int)rightmost_width)) {
                    rightmost_x = dm->screens[i].x;
                    rightmost_width = dm->screens[i].width;
                    found_active = true;
                }
            }
        }
        
        if (found_active) {
            *x = rightmost_x + (int)rightmost_width;
            *y = 0;
            printf("No primary screen found, positioning right of rightmost active screen: %d,%d\n", *x, *y);
        } else {
            // Ultimate fallback: place at origin
            *x = 0;
            *y = 0;
            printf("No active screens found, using fallback position: %d,%d\n", *x, *y);
        }
    }
    
    return 0;
}

// Calculate and set new desktop size to encompass all active screens
int mode_expand_desktop_for_screens(DisplayManager *dm) {
    if (!dm) return -1;
    
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    bool first = true;
    
    // Find bounding box of all active screens (including newly enabled ones)
    // We need to check both connected screens and active CRTCs
    for (int i = 0; i < dm->resources->ncrtc; i++) {
        RRCrtc crtc = dm->resources->crtcs[i];
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dm->display, dm->resources, crtc);
        
        if (crtc_info && crtc_info->mode != None && crtc_info->width > 0 && crtc_info->height > 0) {
            int screen_max_x = crtc_info->x + (int)crtc_info->width;
            int screen_max_y = crtc_info->y + (int)crtc_info->height;
            
            if (first) {
                min_x = crtc_info->x;
                max_x = screen_max_x;
                min_y = crtc_info->y;
                max_y = screen_max_y;
                first = false;
            } else {
                if (crtc_info->x < min_x) min_x = crtc_info->x;
                if (screen_max_x > max_x) max_x = screen_max_x;
                if (crtc_info->y < min_y) min_y = crtc_info->y;
                if (screen_max_y > max_y) max_y = screen_max_y;
            }
            
            printf("Active CRTC found: %dx%d+%d+%d\n", 
                   crtc_info->width, crtc_info->height, crtc_info->x, crtc_info->y);
        }
        
        if (crtc_info) XRRFreeCrtcInfo(crtc_info);
    }
    
    if (first) {
        printf("No active screens found for desktop expansion\n");
        return -1;
    }
    
    unsigned int new_width = max_x - min_x;
    unsigned int new_height = max_y - min_y;
    
    printf("Calculated desktop bounds: %d,%d to %d,%d (size: %ux%u)\n",
           min_x, min_y, max_x, max_y, new_width, new_height);
    
    // Get current screen size for comparison
    int current_width = DisplayWidth(dm->display, dm->screen);
    int current_height = DisplayHeight(dm->display, dm->screen);
    
    if (new_width != (unsigned int)current_width || new_height != (unsigned int)current_height) {
        // Calculate DPI-appropriate physical size
        int current_width_mm = DisplayWidthMM(dm->display, dm->screen);
        int current_height_mm = DisplayHeightMM(dm->display, dm->screen);
        
        // Scale physical dimensions proportionally
        int new_width_mm = (current_width_mm * (int)new_width) / current_width;
        int new_height_mm = (current_height_mm * (int)new_height) / current_height;
        
        printf("Expanding desktop from %dx%d to %ux%u (physical: %dx%d mm to %dx%d mm)\n",
               current_width, current_height, new_width, new_height,
               current_width_mm, current_height_mm, new_width_mm, new_height_mm);
        
        // Set the new screen size
        XRRSetScreenSize(dm->display, dm->root, new_width, new_height,
                         new_width_mm, new_height_mm);
        XSync(dm->display, False);
        
        printf("Desktop expanded successfully\n");
    } else {
        printf("Desktop size already encompasses all screens (%ux%u)\n", new_width, new_height);
    }
    
    return 0;
}

// Create CVT mode using libxcvt and convert to XRandR
RRMode mode_create_cvt(DisplayManager *dm, unsigned int width, unsigned int height, 
                      double refresh_rate, bool reduced_blanking) {
    if (!dm) return 0;
    
    // Generate the mode name that would be used
    char mode_name[64];
    snprintf(mode_name, sizeof(mode_name), "%dx%d_%.2f", width, height, refresh_rate);
    
    // Check if mode already exists
    RRMode existing_mode = mode_find_by_name(dm, mode_name);
    if (existing_mode != 0) {
        printf("Mode '%s' already exists with ID: %lu\n", mode_name, existing_mode);
        return existing_mode; // Return the existing mode ID
    }
    
    // Use libxcvt to calculate CVT timing
    struct libxcvt_mode_info *cvt_mode = libxcvt_gen_mode_info(width, height, refresh_rate, 
                                                               reduced_blanking, false);
    if (!cvt_mode) {
        fprintf(stderr, "libxcvt failed to generate mode for %dx%d@%.2f\n", 
                width, height, refresh_rate);
        return 0;
    }
    
    // Print the calculated mode info (using libxcvt structure)
    printf("Generated CVT mode:\n");
    printf("# %dx%d %.2f Hz (CVT) hsync: %.2f kHz; pclk: %.3f MHz\n",
           cvt_mode->hdisplay, cvt_mode->vdisplay, refresh_rate,
           cvt_mode->dot_clock / (double)cvt_mode->htotal, cvt_mode->dot_clock / 1000.0);
    printf("Modeline \"%dx%d_%.2f\" %.3f %d %d %d %d %d %d %d %d %shsync %svsync\n",
           width, height, refresh_rate, cvt_mode->dot_clock / 1000.0,
           cvt_mode->hdisplay, cvt_mode->hsync_start, cvt_mode->hsync_end, cvt_mode->htotal,
           cvt_mode->vdisplay, cvt_mode->vsync_start, cvt_mode->vsync_end, cvt_mode->vtotal,
           (cvt_mode->mode_flags & LIBXCVT_MODE_FLAG_HSYNC_POSITIVE) ? "+" : "-",
           (cvt_mode->mode_flags & LIBXCVT_MODE_FLAG_VSYNC_POSITIVE) ? "+" : "-");
    
    // Convert to XRRModeInfo
    XRRModeInfo xrr_mode;
    convert_libxcvt_to_xrr(cvt_mode, &xrr_mode, mode_name);
    
    // Create the mode in XRandR
    RRMode new_mode_id = XRRCreateMode(dm->display, dm->root, &xrr_mode);
    XSync(dm->display, false);
    
    // Clean up libxcvt resources
    free(cvt_mode);
    
    if (new_mode_id == 0) {
        fprintf(stderr, "XRRCreateMode failed\n");
        return 0;
    }
    
    printf("Created new mode with ID: %lu\n", new_mode_id);
    return new_mode_id;
}

// Add mode to a specific output using RRMode ID
int mode_add_to_output(DisplayManager *dm, const char *output_name, RRMode mode_id) {
    if (!dm || !output_name || mode_id == 0) return -1;
    
    // Find the output by name
    RROutput target_output = 0;
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, output_name) == 0) {
            target_output = dm->screens[i].output_id;
            break;
        }
    }
    
    if (target_output == 0) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        return -1;
    }
    
    // Reset error tracking before the operation
    reset_x11_error_tracking();
    
    // Add the mode to the output
    XRRAddOutputMode(dm->display, target_output, mode_id);
    XSync(dm->display, False); // Force processing of the request
    
    // Check if the operation failed
    if (check_x11_operation_result()) {
        printf("XRRAddOutputMode failed for mode ID %lu on output '%s'\n", mode_id, output_name);
        return -1;
    }
    
    printf("Added mode ID %lu to output '%s'\n", mode_id, output_name);
    return 0;
}


// Remove mode from a specific output using RRMode ID
int mode_remove_from_output(DisplayManager *dm, const char *output_name, RRMode mode_id) {
    if (!dm || !output_name || mode_id == 0) return -1;
    
    // Find the output by name
    RROutput target_output = 0;
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, output_name) == 0) {
            target_output = dm->screens[i].output_id;
            break;
        }
    }
    
    if (target_output == 0) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        return -1;
    }
    
    // Remove the mode from the output
    XRRDeleteOutputMode(dm->display, target_output, mode_id);
    XSync(dm->display, False);
    
    printf("Removed mode ID %lu from output '%s'\n", mode_id, output_name);
    return 0;
}

// Delete mode from XRandR entirely using RRMode ID
int mode_delete_from_xrandr(DisplayManager *dm, RRMode mode_id) {
    if (!dm || mode_id == 0) return -1;
    
    // Delete the mode from XRandR
    XRRDestroyMode(dm->display, mode_id);
    XSync(dm->display, False);
    
    printf("Deleted mode ID %lu from XRandR\n", mode_id);
    return 0;
}


// Find mode ID by name in current XRandR configuration
RRMode mode_find_by_name(DisplayManager *dm, const char *mode_name) {
    if (!dm || !mode_name) return 0;
    
    // Get current screen resources
    XRRScreenResources *current_resources = XRRGetScreenResources(dm->display, dm->root);
    if (!current_resources) return 0;
    
    // Search through all modes
    for (int i = 0; i < current_resources->nmode; i++) {
        XRRModeInfo *mode_info = &current_resources->modes[i];
        
        // Compare mode names
        if (strlen(mode_name) == mode_info->nameLength && 
            strncmp(mode_name, mode_info->name, mode_info->nameLength) == 0) {
            RRMode found_id = mode_info->id;
            XRRFreeScreenResources(current_resources);
            return found_id;
        }
    }
    
    XRRFreeScreenResources(current_resources);
    return 0; // Mode not found
}

// Enable output with a specific mode (mimics xrandr --output HDMI-1 --mode 2336x1080_60.00)
int mode_enable_output_with_mode(DisplayManager *dm, const char *output_name, 
                                 const char *mode_name, int x_pos, int y_pos) {
    if (!dm || !output_name || !mode_name) return -1;
    
    // Find the output
    RROutput output = find_output_by_name(dm, output_name);
    if (output == None) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        return -1;
    }
    
    // Find the mode by name
    RRMode mode = mode_find_by_name(dm, mode_name);
    if (mode == 0) {
        fprintf(stderr, "Mode '%s' not found\n", mode_name);
        return -1;
    }
    
    // Find an available CRTC
    RRCrtc crtc = find_available_crtc(dm);
    if (crtc == None) {
        fprintf(stderr, "No available CRTC found for output '%s'\n", output_name);
        return -1;
    }
    
    // Configure the CRTC with the output and mode
    Status result = XRRSetCrtcConfig(dm->display, dm->resources, crtc, 
                                    CurrentTime, x_pos, y_pos, mode, 
                                    RR_Rotate_0, &output, 1);
    
    XSync(dm->display, False);
    
    if (result == RRSetConfigSuccess) {
        printf("Enabled output '%s' with mode '%s' at position %d,%d\n", 
               output_name, mode_name, x_pos, y_pos);
        
        // Expand desktop to accommodate new screen
        mode_expand_desktop_for_screens(dm);
        
        return 0;
    } else {
        fprintf(stderr, "Failed to enable output '%s' (error code: %d)\n", 
                output_name, result);
        return -1;
    }
}

// Enable output with mode ID (original version)
int mode_enable_output_with_mode_id(DisplayManager *dm, const char *output_name, 
                                   RRMode mode_id, int x_pos, int y_pos) {
    return mode_enable_output_with_mode_id_positioned(dm, output_name, mode_id, x_pos, y_pos, false);
}

// Enable output with mode ID and positioning options (new enhanced version)
int mode_enable_output_with_mode_id_positioned(DisplayManager *dm, const char *output_name, 
                                             RRMode mode_id, int x_pos, int y_pos, 
                                             bool auto_right_of) {
    if (!dm || !output_name || mode_id == 0) return -1;
    
    RROutput output = find_output_by_name(dm, output_name);
    if (output == None) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        return -1;
    }
    
    RRCrtc crtc = find_available_crtc(dm);
    if (crtc == None) {
        fprintf(stderr, "No available CRTC found for output '%s'\n", output_name);
        return -1;
    }
    
    int final_x = x_pos;
    int final_y = y_pos;
    
    // Auto-positioning logic
    if (auto_right_of) {
        // Refresh resources to make sure we have the latest mode information
        XRRScreenResources *fresh_resources = XRRGetScreenResources(dm->display, dm->root);
        if (fresh_resources) {
            // Free old resources and use fresh ones
            if (dm->resources) {
                XRRFreeScreenResources(dm->resources);
            }
            dm->resources = fresh_resources;
        }
        
        unsigned int width = 0, height = 0;
        if (get_mode_dimensions(dm, mode_id, &width, &height) == 0) {
            if (mode_calculate_right_of_position(dm, &final_x, &final_y, width, height) != 0) {
                printf("Warning: Could not calculate left-of position, using provided coordinates\n");
                final_x = x_pos;
                final_y = y_pos;
            }
        } else {
            printf("Warning: Could not get mode dimensions for auto-positioning, using provided coordinates\n");
            final_x = x_pos;
            final_y = y_pos;
        }
    }
    
    // Reset error tracking before the operation
    reset_x11_error_tracking();
    
    Status result = XRRSetCrtcConfig(dm->display, dm->resources, crtc, 
                                    CurrentTime, final_x, final_y, mode_id, 
                                    RR_Rotate_0, &output, 1);
    
    XSync(dm->display, False);
    
    // Check both the Status return and any X11 errors
    if (result != RRSetConfigSuccess || check_x11_operation_result()) {
        fprintf(stderr, "Failed to enable output '%s' (status: %d, x11_error: %d)\n", 
                output_name, result, check_x11_operation_result());
        return -1;
    }
    
    printf("Enabled output '%s' with mode ID %lu at position %d,%d\n", 
           output_name, mode_id, final_x, final_y);
    
    // Expand desktop to accommodate new screen
    mode_expand_desktop_for_screens(dm);
    
    return 0;
}

// Disable output (mimics xrandr --output HDMI-1 --off)
int mode_disable_output(DisplayManager *dm, const char *output_name) {
    if (!dm || !output_name) return -1;
    
    RROutput output = find_output_by_name(dm, output_name);
    if (output == None) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        return -1;
    }
    
    // Get current CRTC for this output
    XRROutputInfo *output_info = XRRGetOutputInfo(dm->display, dm->resources, output);
    if (!output_info) {
        fprintf(stderr, "Failed to get output info for '%s'\n", output_name);
        return -1;
    }
    
    if (output_info->crtc == None) {
        printf("Output '%s' is already disabled\n", output_name);
        XRRFreeOutputInfo(output_info);
        return 0;
    }
    
    RRCrtc crtc = output_info->crtc;
    XRRFreeOutputInfo(output_info);
    
    // Disable the CRTC (set no mode, no outputs)
    Status result = XRRSetCrtcConfig(dm->display, dm->resources, crtc, 
                                    CurrentTime, 0, 0, None, 
                                    RR_Rotate_0, NULL, 0);
    
    XSync(dm->display, False);
    
    if (result == RRSetConfigSuccess) {
        printf("Disabled output '%s'\n", output_name);
        
        // Recalculate desktop size after disabling
        mode_expand_desktop_for_screens(dm);
        
        return 0;
    } else {
        fprintf(stderr, "Failed to disable output '%s' (error code: %d)\n", 
                output_name, result);
        return -1;
    }
}

// Print all modes available for a specific output
void mode_print_output_modes(DisplayManager *dm, const char *output_name) {
    if (!dm || !output_name) return;
    
    RROutput output = find_output_by_name(dm, output_name);
    if (output == None) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        return;
    }
    
    XRROutputInfo *output_info = XRRGetOutputInfo(dm->display, dm->resources, output);
    if (!output_info) {
        fprintf(stderr, "Failed to get output info for '%s'\n", output_name);
        return;
    }
    
    printf("Available modes for output '%s':\n", output_name);
    if (output_info->nmode == 0) {
        printf("  No modes available\n");
    } else {
        for (int i = 0; i < output_info->nmode; i++) {
            RRMode mode_id = output_info->modes[i];
            
            // Find mode info in screen resources
            XRRModeInfo *mode_info = NULL;
            for (int j = 0; j < dm->resources->nmode; j++) {
                if (dm->resources->modes[j].id == mode_id) {
                    mode_info = &dm->resources->modes[j];
                    break;
                }
            }
            
            if (mode_info) {
                // Calculate refresh rate
                double refresh_rate = 0.0;
                if (mode_info->hTotal && mode_info->vTotal) {
                    refresh_rate = (double)mode_info->dotClock / 
                                  (double)(mode_info->hTotal * mode_info->vTotal);
                }
                
                // Print mode name or create one if it doesn't have a proper name
                char mode_name[256];
                if (mode_info->nameLength > 0 && mode_info->name) {
                    snprintf(mode_name, sizeof(mode_name), "%.*s", 
                            (int)mode_info->nameLength, mode_info->name);
                } else {
                    snprintf(mode_name, sizeof(mode_name), "%dx%d_%.2f", 
                            mode_info->width, mode_info->height, refresh_rate);
                }
                
                printf("  %s (%dx%d @ %.2f Hz) [ID: %lu]\n", 
                       mode_name, mode_info->width, mode_info->height, 
                       refresh_rate, mode_id);
            } else {
                printf("  [Mode ID: %lu - info not available]\n", mode_id);
            }
        }
    }
    
    XRRFreeOutputInfo(output_info);
}

// Print modes for all outputs
void mode_print_all_output_modes(DisplayManager *dm) {
    if (!dm || !dm->screens) return;
    
    for (int i = 0; i < dm->screen_count; i++) {
        mode_print_output_modes(dm, dm->screens[i].name);
        printf("\n");
    }
}

// Check if output is currently enabled (has an active CRTC)
bool mode_is_output_enabled(DisplayManager *dm, const char *output_name) {
    if (!dm || !output_name) return false;
    
    RROutput output = find_output_by_name(dm, output_name);
    if (output == None) return false;
    
    XRROutputInfo *output_info = XRRGetOutputInfo(dm->display, dm->resources, output);
    if (!output_info) return false;
    
    bool enabled = (output_info->crtc != None);
    XRRFreeOutputInfo(output_info);
    
    return enabled;
}

// Get current mode and position for an enabled output
int mode_get_output_config(DisplayManager *dm, const char *output_name, 
                          RRMode *current_mode, int *x, int *y, 
                          unsigned int *width, unsigned int *height) {
    if (!dm || !output_name || !current_mode || !x || !y || !width || !height) 
        return -1;
    
    RROutput output = find_output_by_name(dm, output_name);
    if (output == None) return -1;
    
    XRROutputInfo *output_info = XRRGetOutputInfo(dm->display, dm->resources, output);
    if (!output_info || output_info->crtc == None) {
        if (output_info) XRRFreeOutputInfo(output_info);
        return -1; // Output not enabled
    }
    
    XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dm->display, dm->resources, output_info->crtc);
    if (!crtc_info) {
        XRRFreeOutputInfo(output_info);
        return -1;
    }
    
    *current_mode = crtc_info->mode;
    *x = crtc_info->x;
    *y = crtc_info->y;
    *width = crtc_info->width;
    *height = crtc_info->height;
    
    XRRFreeCrtcInfo(crtc_info);
    XRRFreeOutputInfo(output_info);
    
    return 0;
}