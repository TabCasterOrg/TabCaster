#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Find a suitable CRTC for the given output (allows reuse of CRTCs)
static RRCrtc find_suitable_crtc(DisplayManager *dm, RROutput target_output) {
    // First, try to find an unused CRTC
    for (int i = 0; i < dm->resources->ncrtc; i++) {
        RRCrtc crtc = dm->resources->crtcs[i];
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(dm->display, dm->resources, crtc);
        
        if (crtc_info) {
            bool available = (crtc_info->noutput == 0);
            XRRFreeCrtcInfo(crtc_info);
            
            if (available) {
                return crtc;
            }
        }
    }
    
    // If no unused CRTC, find one that can support this output
    // Check if any CRTC can be assigned to this output
    XRROutputInfo *output_info = XRRGetOutputInfo(dm->display, dm->resources, target_output);
    if (output_info && output_info->ncrtc > 0) {
        // Use the first compatible CRTC (we'll reconfigure it)
        RRCrtc suitable_crtc = output_info->crtcs[0];
        XRRFreeOutputInfo(output_info);
        return suitable_crtc;
    }
    
    if (output_info) XRRFreeOutputInfo(output_info);
    
    // Fallback: use any CRTC (this might fail, but worth trying)
    if (dm->resources->ncrtc > 0) {
        printf("Warning: Using fallback CRTC assignment\n");
        return dm->resources->crtcs[0];
    }
    
    return None;
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

// Create CVT mode using libxcvt and convert to XRandR
RRMode mode_create_cvt(DisplayManager *dm, unsigned int width, unsigned int height, 
                      double refresh_rate, bool reduced_blanking) {
    if (!dm) return 0;
    
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
    char mode_name[64];
    snprintf(mode_name, sizeof(mode_name), "%dx%d_%.2f", width, height, refresh_rate);
    convert_libxcvt_to_xrr(cvt_mode, &xrr_mode, mode_name);
    
    // Create the mode in XRandR
    RRMode new_mode_id = XRRCreateMode(dm->display, dm->root, &xrr_mode);
    
    // Clean up libxcvt resources
    free(cvt_mode);
    
    if (new_mode_id == 0) {
        fprintf(stderr, "XRRCreateMode failed\n");
        return 0;
    }
    
    printf("Created mode with ID: %lu\n", new_mode_id);
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
    
    // Add the mode to the output
    XRRAddOutputMode(dm->display, target_output, mode_id);
    XSync(dm->display, False);
    
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

// Print libxcvt mode info in readable format
void mode_print_libxcvt_info(const struct libxcvt_mode_info *cvt_mode, double refresh_rate) {
    if (!cvt_mode) return;
    
    printf("# %dx%d %.2f Hz (CVT) hsync: %.2f kHz; pclk: %.3f MHz\n",
           cvt_mode->hdisplay, cvt_mode->vdisplay, refresh_rate,
           cvt_mode->dot_clock / (double)cvt_mode->htotal, cvt_mode->dot_clock / 1000.0);
    printf("Modeline \"%dx%d_%.2f\" %.3f %d %d %d %d %d %d %d %d %shsync %svsync\n",
           cvt_mode->hdisplay, cvt_mode->vdisplay, refresh_rate, cvt_mode->dot_clock / 1000.0,
           cvt_mode->hdisplay, cvt_mode->hsync_start, cvt_mode->hsync_end, cvt_mode->htotal,
           cvt_mode->vdisplay, cvt_mode->vsync_start, cvt_mode->vsync_end, cvt_mode->vtotal,
           (cvt_mode->mode_flags & LIBXCVT_MODE_FLAG_HSYNC_POSITIVE) ? "+" : "-",
           (cvt_mode->mode_flags & LIBXCVT_MODE_FLAG_VSYNC_POSITIVE) ? "+" : "-");
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

// Enable output with a specific mode (works regardless of connection status)
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
    
    // Find a suitable CRTC (removed connection requirement)
    RRCrtc crtc = find_suitable_crtc(dm, output);
    if (crtc == None) {
        fprintf(stderr, "No suitable CRTC found for output '%s'\n", output_name);
        return -1;
    }
    
    printf("Using CRTC %lu for output '%s'\n", crtc, output_name);
    
    // Configure the CRTC with the output and mode
    Status result = XRRSetCrtcConfig(dm->display, dm->resources, crtc, 
                                    CurrentTime, x_pos, y_pos, mode, 
                                    RR_Rotate_0, &output, 1);
    
    XSync(dm->display, False);
    
    if (result == RRSetConfigSuccess) {
        printf("Enabled output '%s' with mode '%s' at position %d,%d\n", 
               output_name, mode_name, x_pos, y_pos);
        return 0;
    } else {
        fprintf(stderr, "Failed to enable output '%s' (error code: %d)\n", 
                output_name, result);
        return -1;
    }
}

// Enable output with mode ID (alternative version, works regardless of connection status)
int mode_enable_output_with_mode_id(DisplayManager *dm, const char *output_name, 
                                   RRMode mode_id, int x_pos, int y_pos) {
    if (!dm || !output_name || mode_id == 0) return -1;
    
    RROutput output = find_output_by_name(dm, output_name);
    if (output == None) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        return -1;
    }
    
    RRCrtc crtc = find_suitable_crtc(dm, output);
    if (crtc == None) {
        fprintf(stderr, "No suitable CRTC found for output '%s'\n", output_name);
        return -1;
    }
    
    printf("Using CRTC %lu for output '%s'\n", crtc, output_name);
    
    Status result = XRRSetCrtcConfig(dm->display, dm->resources, crtc, 
                                    CurrentTime, x_pos, y_pos, mode_id, 
                                    RR_Rotate_0, &output, 1);
    
    XSync(dm->display, False);
    
    if (result == RRSetConfigSuccess) {
        printf("Enabled output '%s' with mode ID %lu at position %d,%d\n", 
               output_name, mode_id, x_pos, y_pos);
        return 0;
    } else {
        fprintf(stderr, "Failed to enable output '%s' (error code: %d)\n", 
                output_name, result);
        return -1;
    }
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