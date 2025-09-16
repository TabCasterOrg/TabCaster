#include "frame_capture.h"
#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

// Update capture coordinates when display position changes
int fc_update_position(FrameCapture *fc) {
    if (!fc || !fc->dm) return -1;
    
    // Find the current screen info
    for (int i = 0; i < fc->dm->screen_count; i++) {
        if (strcmp(fc->dm->screens[i].name, fc->output_name) == 0) {
            if (fc->dm->screens[i].connected) {
                fc->x = fc->dm->screens[i].x;
                fc->y = fc->dm->screens[i].y;
                fc->width = fc->dm->screens[i].width;
                fc->height = fc->dm->screens[i].height;
                printf("Updated capture position for '%s': %dx%d+%d+%d\n",
                       fc->output_name, fc->width, fc->height, fc->x, fc->y);
                return 0;
            } else {
                // For virtual displays, get current config
                RRMode current_mode;
                int x, y;
                unsigned int width, height;
                
                if (mode_get_output_config(fc->dm, fc->output_name, &current_mode, &x, &y, &width, &height) == 0) {
                    fc->x = x;
                    fc->y = y;
                    fc->width = width;
                    fc->height = height;
                    printf("Updated virtual capture position for '%s': %dx%d+%d+%d\n",
                           fc->output_name, fc->width, fc->height, fc->x, fc->y);
                    return 0;
                }
            }
            break;
        }
    }
    
    fprintf(stderr, "Could not update position for output '%s'\n", fc->output_name);
    return -1;
}

// Initialize shared memory for XShm
static int fc_init_shm(FrameCapture *fc) {
    if (!fc) return -1;
    
    // Verify XShm extension is available
    if (!XShmQueryExtension(fc->dm->display)) {
        fprintf(stderr, "XShm extension not available - ensure libXext is installed\n");
        return -1;
    }
    
    // Calculate image size - create XImage first to get proper format
    XImage *temp_image = XCreateImage(fc->dm->display, 
                                      DefaultVisual(fc->dm->display, DefaultScreen(fc->dm->display)),
                                      DefaultDepth(fc->dm->display, DefaultScreen(fc->dm->display)),
                                      ZPixmap, 0, NULL,
                                      fc->width, fc->height, 32, 0);
    
    if (!temp_image) {
        fprintf(stderr, "Failed to create temporary XImage for size calculation\n");
        return -1;
    }
    
    // Calculate the actual size needed
    size_t image_size = temp_image->bytes_per_line * temp_image->height;
    XDestroyImage(temp_image);
    
    // Create shared memory segment
    fc->shm_info.shmid = shmget(IPC_PRIVATE, image_size, IPC_CREAT | 0600);
    if (fc->shm_info.shmid < 0) {
        perror("shmget failed");
        return -1;
    }
    
    // Attach shared memory
    fc->shm_info.shmaddr = shmat(fc->shm_info.shmid, 0, 0);
    if (fc->shm_info.shmaddr == (char *)-1) {
        perror("shmat failed");
        shmctl(fc->shm_info.shmid, IPC_RMID, 0);
        return -1;
    }
    
    fc->shm_info.readOnly = False;
    
    // Attach to X server
    if (!XShmAttach(fc->dm->display, &fc->shm_info)) {
        fprintf(stderr, "XShmAttach failed\n");
        shmdt(fc->shm_info.shmaddr);
        shmctl(fc->shm_info.shmid, IPC_RMID, 0);
        return -1;
    }
    
    // Create XImage using shared memory
    fc->current_frame = XShmCreateImage(fc->dm->display,
                                        DefaultVisual(fc->dm->display, DefaultScreen(fc->dm->display)),
                                        DefaultDepth(fc->dm->display, DefaultScreen(fc->dm->display)),
                                        ZPixmap, fc->shm_info.shmaddr,
                                        &fc->shm_info,
                                        fc->width, fc->height);
    
    if (!fc->current_frame) {
        fprintf(stderr, "XShmCreateImage failed\n");
        XShmDetach(fc->dm->display, &fc->shm_info);
        shmdt(fc->shm_info.shmaddr);
        shmctl(fc->shm_info.shmid, IPC_RMID, 0);
        return -1;
    }
    
    printf("XShm initialized successfully (%zu bytes)\n", image_size);
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
    
    // Initialize XShm
    if (fc_init_shm(fc) != 0) {
        fprintf(stderr, "Failed to initialize XShm for '%s'\n", output_name);
        free(fc);
        return NULL;
    }

    printf("XShm capture initialized for '%s': %dx%d+%d+%d @ %d fps\n",
           output_name, fc->width, fc->height, fc->x, fc->y, fc->target_fps);

    // Check for XFixes extension for cursor capture       
    if (XFixesQueryExtension(dm->display, &fc->xfixes_event_base, &fc->xfixes_error_base)) {
        fc->capture_cursor = true;
        printf("XFixes extension available - cursor capture enabled\n");
    } else {
        fc->capture_cursor = false;
        printf("XFixes extension not available - cursor capture disabled\n");
    }
    
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
    if (!fc || !fc->capturing || !fc->current_frame) return -1;
    
    // Rate limiting - check if enough time has passed
    struct timeval now;
    gettimeofday(&now, NULL);
    
    long time_diff = (now.tv_sec - fc->last_capture.tv_sec) * 1000000 +
                     (now.tv_usec - fc->last_capture.tv_usec);
    
    if (time_diff < fc->frame_interval_us) {
        return 0; // Too soon for next frame
    }
    
    // Use XShmGetImage - data goes directly into shared memory
    if (!XShmGetImage(fc->dm->display, fc->dm->root, fc->current_frame,
                      fc->x, fc->y, AllPlanes)) {
        fprintf(stderr, "XShmGetImage failed for %s (%dx%d+%d+%d)\n",
                fc->output_name, fc->width, fc->height, fc->x, fc->y);
        return -1;
    }
    
    // Composite cursor onto the frame
    fc_composite_cursor(fc);
    
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

// Composite cursor onto the captured frame if enabled
void fc_composite_cursor(FrameCapture *fc) {
    if (!fc || !fc->current_frame || !fc->capture_cursor) return;
    
    // Get cursor image
    XFixesCursorImage *cursor_img = XFixesGetCursorImage(fc->dm->display);
    if (!cursor_img) return;
    
    // Calculate cursor position relative to capture area
    int cursor_x = cursor_img->x - fc->x;
    int cursor_y = cursor_img->y - fc->y;
    
    // Check if cursor is within capture bounds
    if (cursor_x >= -cursor_img->xhot && cursor_x < (int)fc->width &&
        cursor_y >= -cursor_img->yhot && cursor_y < (int)fc->height) {
        
        // Composite cursor onto the frame
        for (unsigned int cy = 0; cy < cursor_img->height; cy++) {
            for (unsigned int cx = 0; cx < cursor_img->width; cx++) {
                int frame_x = cursor_x - cursor_img->xhot + cx;
                int frame_y = cursor_y - cursor_img->yhot + cy;
                
                // Bounds check
                if (frame_x >= 0 && frame_x < (int)fc->width && 
                    frame_y >= 0 && frame_y < (int)fc->height) {
                    
                    unsigned long cursor_pixel = cursor_img->pixels[cy * cursor_img->width + cx];
                    unsigned long alpha = (cursor_pixel >> 24) & 0xFF;
                    
                    if (alpha > 0) { // If cursor pixel is not fully transparent
                        XPutPixel(fc->current_frame, frame_x, frame_y, cursor_pixel);
                    }
                }
            }
        }
    }
    
    XFree(cursor_img);
}

// Print detailed frame and capture info
void fc_print_frame_info(FrameCapture *fc) {
    if (!fc) return;
    
    printf("Capture Status for '%s':\n", fc->output_name);
    printf("  Screen region: %dx%d+%d+%d\n", fc->width, fc->height, fc->x, fc->y);
    printf("  Target FPS: %d (interval: %ld μs)\n", fc->target_fps, fc->frame_interval_us);
    printf("  Method: XShmGetImage (shared memory)\n");
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

    // Destroy damage tracking
    if (fc->damage_enabled && fc->damage_handle) {
        XDamageDestroy(fc->dm->display, fc->damage_handle);
        fc->damage_handle = 0;
        fc->damage_enabled = false;
    }
    
    if (fc->current_frame) {
        XDestroyImage(fc->current_frame);
        fc->current_frame = NULL;
    }
    
    // Cleanup shared memory
    XShmDetach(fc->dm->display, &fc->shm_info);
    shmdt(fc->shm_info.shmaddr);
    shmctl(fc->shm_info.shmid, IPC_RMID, 0);
    
    printf("XShm resources cleaned up\n");
    free(fc);
}

// Initialize XDamage tracking on the root window for our capture region
int fc_init_damage(FrameCapture *fc) {
    if (!fc || !fc->dm) return -1;

    if (!XDamageQueryExtension(fc->dm->display, &fc->xdamage_event_base, &fc->xdamage_error_base)) {
        printf("XDamage extension not available - delta streaming disabled\n");
        fc->damage_enabled = false;
        return 0;
    }

    // Track damage on the root; we will filter rects to our region later
    fc->damage_handle = XDamageCreate(fc->dm->display, fc->dm->root, XDamageReportDeltaRectangles);
    if (!fc->damage_handle) {
        fprintf(stderr, "Failed to create XDamage handle\n");
        return -1;
    }

    fc->damage_enabled = true;
    printf("XDamage initialized (base=%d)\n", fc->xdamage_event_base);
    return 0;
}

// Retrieve and coalesce damage since last call
int fc_get_damage_rects(FrameCapture *fc, XRectangle **rects_out, int *num_rects_out) {
    if (!fc || !rects_out || !num_rects_out) return -1;
    *rects_out = NULL;
    *num_rects_out = 0;
    if (!fc->damage_enabled || !fc->damage_handle) return 0;

    XserverRegion region = XFixesCreateRegion(fc->dm->display, NULL, 0);
    if (!region) return -1;

    // Subtract to get accumulated damage and reset
    XDamageSubtract(fc->dm->display, fc->damage_handle, None, region);

    int nrects = 0;
    XRectangle *rects = XFixesFetchRegion(fc->dm->display, region, &nrects);
    XFixesDestroyRegion(fc->dm->display, region);

    if (!rects || nrects <= 0) {
        if (rects) XFree(rects);
        return 0;
    }

    // Clip rects to our capture area
    int rx = fc->x;
    int ry = fc->y;
    unsigned int rw = fc->width;
    unsigned int rh = fc->height;

    int write_idx = 0;
    // Coalesce simple adjacent rectangles horizontally to reduce packet count
    for (int i = 0; i < nrects; i++) {
        int x1 = rects[i].x;
        int y1 = rects[i].y;
        int x2 = x1 + (int)rects[i].width;
        int y2 = y1 + (int)rects[i].height;

        // Intersect with capture rect
        int ix1 = x1 < rx ? rx : x1;
        int iy1 = y1 < ry ? ry : y1;
        int ix2 = x2 > (rx + (int)rw) ? (rx + (int)rw) : x2;
        int iy2 = y2 > (ry + (int)rh) ? (ry + (int)rh) : y2;

        if (ix2 > ix1 && iy2 > iy1) {
            // Try merge with previous if same row and touching
            if (write_idx > 0) {
                XRectangle *prev = &rects[write_idx - 1];
                if (prev->y == (short)iy1 && prev->height == (unsigned short)(iy2 - iy1) &&
                    (prev->x + prev->width) == (unsigned short)ix1) {
                    prev->width = (unsigned short)((ix2 - prev->x));
                    continue;
                }
            }
            rects[write_idx].x = (short)ix1;
            rects[write_idx].y = (short)iy1;
            rects[write_idx].width = (unsigned short)(ix2 - ix1);
            rects[write_idx].height = (unsigned short)(iy2 - iy1);
            write_idx++;
        }
    }

    // Hard cap number of rects to avoid storms
    if (write_idx > MAX_RECTS_PER_FRAME) write_idx = MAX_RECTS_PER_FRAME;

    *rects_out = rects;
    *num_rects_out = write_idx;
    return 0;
}