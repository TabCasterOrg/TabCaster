#include "frame_capture.h"
#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// Create captures directory if it doesn't exist
static int create_captures_directory() {
    struct stat st = {0};
    
    if (stat("captures", &st) == -1) {
        if (mkdir("captures", 0755) == -1) {
            perror("Failed to create captures directory");
            return -1;
        }
        printf("Created captures directory\n");
    }
    return 0;
}

// Initialize frame capture for a specific output
FrameCapture* fc_init(DisplayManager *dm, const char *output_name, int fps) {
    if (!dm || !output_name) return NULL;
    
    FrameCapture *fc = calloc(1, sizeof(FrameCapture));
    if (!fc) return NULL;
    
    fc->dm = dm;
    fc->target_fps = fps > 0 ? fps : 30;
    fc->frame_interval_us = 1000000 / fc->target_fps;
    strncpy(fc->output_name, output_name, sizeof(fc->output_name) - 1);
    
    // Find the target screen in existing ScreenInfo array
    bool found = false;
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, output_name) == 0) {
            // For connected displays, use the existing screen info
            if (dm->screens[i].connected) {
                fc->x = dm->screens[i].x;
                fc->y = dm->screens[i].y;
                fc->width = dm->screens[i].width;
                fc->height = dm->screens[i].height;
                found = true;
                printf("Found connected output '%s'\n", output_name);
            } else {
                // For disconnected displays, check if they have an active mode
                RRMode current_mode;
                int x, y;
                unsigned int width, height;
                
                if (mode_get_output_config(dm, output_name, &current_mode, &x, &y, &width, &height) == 0) {
                    // Output has an active mode even though it's "disconnected"
                    fc->x = x;
                    fc->y = y;
                    fc->width = width;
                    fc->height = height;
                    found = true;
                    printf("Found virtual/enabled output '%s' (not physically connected but has active mode)\n", output_name);
                } else {
                    printf("Output '%s' exists but has no active mode - cannot capture\n", output_name);
                    free(fc);
                    return NULL;
                }
            }
            break;
        }
    }
    
    if (!found) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        free(fc);
        return NULL;
    }
    
    // Validate capture area
    if (fc->width == 0 || fc->height == 0) {
        fprintf(stderr, "Invalid capture dimensions: %dx%d\n", fc->width, fc->height);
        free(fc);
        return NULL;
    }
    
    // Create captures directory
    if (create_captures_directory() != 0) {
        free(fc);
        return NULL;
    }
    
    printf("Capture initialized for '%s': %dx%d+%d+%d @ %d fps\n",
           output_name, fc->width, fc->height, fc->x, fc->y, fc->target_fps);
    
    return fc;
}

// Start capturing
int fc_start(FrameCapture *fc) {
    if (!fc) return -1;
    
    fc->capturing = true;
    gettimeofday(&fc->last_capture, NULL);
    fc->frame_ready = false;
    
    printf("Started capturing from '%s'\n", fc->output_name);
    return 0;
}

// Main capture function
int fc_capture_frame(FrameCapture *fc) {
    if (!fc || !fc->capturing) return -1;
    
    // Rate limiting - check if enough time has passed
    struct timeval now;
    gettimeofday(&now, NULL);
    
    long time_diff = (now.tv_sec - fc->last_capture.tv_sec) * 1000000 +
                     (now.tv_usec - fc->last_capture.tv_usec);
    
    if (time_diff < fc->frame_interval_us) {
        return 0; // Too soon for next frame
    }
    
    // Free previous frame if it exists
    if (fc->current_frame) {
        XDestroyImage(fc->current_frame);
        fc->current_frame = NULL;
    }
    
    // Capture the screen region using XGetImage
    // For virtual displays, this captures whatever is rendered to that screen area
    // Note: The content might be black/empty if nothing is actually rendering there
    fc->current_frame = XGetImage(fc->dm->display, fc->dm->root,
                                  fc->x, fc->y, fc->width, fc->height,
                                  AllPlanes, ZPixmap);
    
    if (!fc->current_frame) {
        fprintf(stderr, "XGetImage failed for %s (%dx%d+%d+%d)\n",
                fc->output_name, fc->width, fc->height, fc->x, fc->y);
        return -1;
    }
    
    fc->frame_ready = true;
    fc->last_capture = now;
    
    return 1; // New frame captured
}

// Stop capturing
int fc_stop(FrameCapture *fc) {
    if (!fc) return -1;
    
    fc->capturing = false;
    printf("Stopped capturing from '%s'\n", fc->output_name);
    return 0;
}

// Get current frame (returns the XImage*)
XImage* fc_get_frame(FrameCapture *fc) {
    return fc ? fc->current_frame : NULL;
}

// Check if new frame is ready
bool fc_has_new_frame(FrameCapture *fc) {
    return fc ? fc->frame_ready : false;
}

// Mark frame as processed
void fc_mark_frame_processed(FrameCapture *fc) {
    if (fc) fc->frame_ready = false;
}

// Save frame as PPM (simple RGB format) in captures directory
int fc_save_frame_ppm(FrameCapture *fc, const char *filename) {
    if (!fc || !fc->current_frame || !filename) return -1;
    
    XImage *img = fc->current_frame;
    
    // Create full path with captures directory
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "captures/%s", filename);
    
    FILE *fp = fopen(full_path, "wb");
    if (!fp) {
        perror("Failed to open file for writing");
        return -1;
    }
    
    // PPM header
    fprintf(fp, "P6\n%d %d\n255\n", img->width, img->height);
    
    // Convert pixels to RGB
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            // XGetPixel extracts pixel value at x,y
            unsigned long pixel = XGetPixel(img, x, y);
            
            // Extract RGB components (most displays use BGRA or similar)
            unsigned char r = (pixel >> 16) & 0xFF;
            unsigned char g = (pixel >> 8) & 0xFF;
            unsigned char b = pixel & 0xFF;
            
            fputc(r, fp);
            fputc(g, fp);
            fputc(b, fp);
        }
    }
    
    fclose(fp);
    printf("Saved frame to %s (%dx%d)\n", full_path, img->width, img->height);
    return 0;
}

// Print detailed frame and capture info
void fc_print_frame_info(FrameCapture *fc) {
    if (!fc) return;
    
    printf("Capture Status for '%s':\n", fc->output_name);
    printf("  Screen region: %dx%d+%d+%d\n", fc->width, fc->height, fc->x, fc->y);
    printf("  Target FPS: %d (interval: %ld μs)\n", fc->target_fps, fc->frame_interval_us);
    printf("  Capturing: %s\n", fc->capturing ? "YES" : "NO");
    printf("  Frame ready: %s\n", fc->frame_ready ? "YES" : "NO");
    
    if (fc->current_frame) {
        XImage *img = fc->current_frame;
        printf("  Current frame:\n");
        printf("    Dimensions: %dx%d\n", img->width, img->height);
        printf("    Depth: %d bits\n", img->depth);
        printf("    Bits per pixel: %d\n", img->bits_per_pixel);
        printf("    Bytes per line: %d\n", img->bytes_per_line);
        printf("    Format: %s\n", img->format == ZPixmap ? "ZPixmap" : 
                                   img->format == XYPixmap ? "XYPixmap" : "XYBitmap");
        printf("    Byte order: %s\n", img->byte_order == LSBFirst ? "LSBFirst" : "MSBFirst");
    } else {
        printf("  No frame captured yet\n");
    }
}

// Cleanup all resources
void fc_cleanup(FrameCapture *fc) {
    if (!fc) return;
    
    fc_stop(fc);
    
    if (fc->current_frame) {
        XDestroyImage(fc->current_frame);  // This also frees the image data
        fc->current_frame = NULL;
    }
    
    free(fc);
}