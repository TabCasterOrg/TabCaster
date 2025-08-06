#include "frame_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Initialize simple capture for a specific output
SimpleCapture* sc_init(DisplayManager *dm, const char *output_name, int fps) {
    if (!dm || !output_name) return NULL;
    
    SimpleCapture *sc = calloc(1, sizeof(SimpleCapture));
    if (!sc) return NULL;
    
    sc->dm = dm;
    sc->target_fps = fps > 0 ? fps : 30;
    sc->frame_interval_us = 1000000 / sc->target_fps;
    strncpy(sc->output_name, output_name, sizeof(sc->output_name) - 1);
    
    // Find the target screen in your existing ScreenInfo array
    bool found = false;
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, output_name) == 0) {
            // For connected displays, use the existing screen info
            if (dm->screens[i].connected) {
                sc->x = dm->screens[i].x;
                sc->y = dm->screens[i].y;
                sc->width = dm->screens[i].width;
                sc->height = dm->screens[i].height;
                found = true;
                printf("Found connected output '%s'\n", output_name);
            } else {
                // For disconnected displays, check if they have an active mode
                RRMode current_mode;
                int x, y;
                unsigned int width, height;
                
                if (mode_get_output_config(dm, output_name, &current_mode, &x, &y, &width, &height) == 0) {
                    // Output has an active mode even though it's "disconnected"
                    sc->x = x;
                    sc->y = y;
                    sc->width = width;
                    sc->height = height;
                    found = true;
                    printf("Found virtual/enabled output '%s' (not physically connected but has active mode)\n", output_name);
                } else {
                    printf("Output '%s' exists but has no active mode - cannot capture\n", output_name);
                    free(sc);
                    return NULL;
                }
            }
            break;
        }
    }
    
    if (!found) {
        fprintf(stderr, "Output '%s' not found\n", output_name);
        free(sc);
        return NULL;
    }
    
    // Validate capture area
    if (sc->width == 0 || sc->height == 0) {
        fprintf(stderr, "Invalid capture dimensions: %dx%d\n", sc->width, sc->height);
        free(sc);
        return NULL;
    }
    
    printf("Capture initialized for '%s': %dx%d+%d+%d @ %d fps\n",
           output_name, sc->width, sc->height, sc->x, sc->y, sc->target_fps);
    
    return sc;
}

// Start capturing
int sc_start(SimpleCapture *sc) {
    if (!sc) return -1;
    
    sc->capturing = true;
    gettimeofday(&sc->last_capture, NULL);
    sc->frame_ready = false;
    
    printf("Started capturing from '%s'\n", sc->output_name);
    return 0;
}

// Main capture function - call this repeatedly in your loop
int sc_capture_frame(SimpleCapture *sc) {
    if (!sc || !sc->capturing) return -1;
    
    // Rate limiting - check if enough time has passed
    struct timeval now;
    gettimeofday(&now, NULL);
    
    long time_diff = (now.tv_sec - sc->last_capture.tv_sec) * 1000000 +
                     (now.tv_usec - sc->last_capture.tv_usec);
    
    if (time_diff < sc->frame_interval_us) {
        return 0; // Too soon for next frame
    }
    
    // Free previous frame if it exists
    if (sc->current_frame) {
        XDestroyImage(sc->current_frame);
        sc->current_frame = NULL;
    }
    
    // Capture the screen region using XGetImage
    // For virtual displays, this captures whatever is rendered to that screen area
    // Note: The content might be black/empty if nothing is actually rendering there
    sc->current_frame = XGetImage(sc->dm->display, sc->dm->root,
                                  sc->x, sc->y, sc->width, sc->height,
                                  AllPlanes, ZPixmap);
    
    if (!sc->current_frame) {
        fprintf(stderr, "XGetImage failed for %s (%dx%d+%d+%d)\n",
                sc->output_name, sc->width, sc->height, sc->x, sc->y);
        return -1;
    }
    
    sc->frame_ready = true;
    sc->last_capture = now;
    
    return 1; // New frame captured
}

// Stop capturing
int sc_stop(SimpleCapture *sc) {
    if (!sc) return -1;
    
    sc->capturing = false;
    printf("Stopped capturing from '%s'\n", sc->output_name);
    return 0;
}

// Get current frame (returns the XImage*)
XImage* sc_get_frame(SimpleCapture *sc) {
    return sc ? sc->current_frame : NULL;
}

// Check if new frame is ready
bool sc_has_new_frame(SimpleCapture *sc) {
    return sc ? sc->frame_ready : false;
}

// Mark frame as processed
void sc_mark_frame_processed(SimpleCapture *sc) {
    if (sc) sc->frame_ready = false;
}

// Save frame as PPM (simple RGB format)
int sc_save_frame_ppm(SimpleCapture *sc, const char *filename) {
    if (!sc || !sc->current_frame || !filename) return -1;
    
    XImage *img = sc->current_frame;
    FILE *fp = fopen(filename, "wb");
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
    printf("Saved frame to %s (%dx%d)\n", filename, img->width, img->height);
    return 0;
}

// Print detailed frame and capture info
void sc_print_frame_info(SimpleCapture *sc) {
    if (!sc) return;
    
    printf("Capture Status for '%s':\n", sc->output_name);
    printf("  Screen region: %dx%d+%d+%d\n", sc->width, sc->height, sc->x, sc->y);
    printf("  Target FPS: %d (interval: %ld μs)\n", sc->target_fps, sc->frame_interval_us);
    printf("  Capturing: %s\n", sc->capturing ? "YES" : "NO");
    printf("  Frame ready: %s\n", sc->frame_ready ? "YES" : "NO");
    
    if (sc->current_frame) {
        XImage *img = sc->current_frame;
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
void sc_cleanup(SimpleCapture *sc) {
    if (!sc) return;
    
    sc_stop(sc);
    
    if (sc->current_frame) {
        XDestroyImage(sc->current_frame);  // This also frees the image data
        sc->current_frame = NULL;
    }
    
    free(sc);
}