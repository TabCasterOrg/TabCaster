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