#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include "display_manager.h"
#include <sys/time.h>
#include <stdbool.h>

// Frame capture structure - using XGetImage
typedef struct {
    DisplayManager *dm;
    
    // Target screen region (from your ScreenInfo)
    int x, y;
    unsigned int width, height;
    char output_name[32];
    
    // Frame timing
    int target_fps;
    long frame_interval_us;  // Microseconds between frames
    struct timeval last_capture;
    
    // Current frame - using XImage directly
    XImage *current_frame;
    bool frame_ready;
    bool capturing;
} FrameCapture;

// Core functions
FrameCapture* fc_init(DisplayManager *dm, const char *output_name, int fps);
int fc_start(FrameCapture *fc);
int fc_capture_frame(FrameCapture *fc);  // Returns 1 if new frame, 0 if too soon, -1 on error
int fc_stop(FrameCapture *fc);
void fc_cleanup(FrameCapture *fc);

// Frame access
XImage* fc_get_frame(FrameCapture *fc);
bool fc_has_new_frame(FrameCapture *fc);
void fc_mark_frame_processed(FrameCapture *fc);

// Utilities
int fc_save_frame_ppm(FrameCapture *fc, const char *filename);
void fc_print_frame_info(FrameCapture *fc);

#endif // FRAME_CAPTURE_H