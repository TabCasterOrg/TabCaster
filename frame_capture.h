#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include "display_manager.h"
#include "damage_tracker.h"
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XShm.h>
#include <sys/time.h>
#include <stdbool.h>

#define MAX_RECTS_PER_FRAME 64

// Frame capture structure - using XShmGetImage
typedef struct {
    DisplayManager *dm;
    
    // Target screen region (from your ScreenInfo)
    int x, y;
    unsigned int width, height;
    char output_name[32];
    
    // Frame timing
    int target_fps;
    long frame_interval_us; // Microseconds between frames
    struct timeval last_capture;
    
    // Current frame - using XShm
    XImage *current_frame;
    XShmSegmentInfo shm_info;    // Shared memory segment info
    bool frame_ready;
    bool capturing;
    
    // Cursor capture
    bool capture_cursor;
    int xfixes_event_base;
    int xfixes_error_base;

    // Damage tracking (XOR-based)
    bool damage_enabled;
    DamageTracker *damage_tracker;
} FrameCapture;

// Core functions
FrameCapture* fc_init(DisplayManager *dm, const char *output_name, int fps);
int fc_start(FrameCapture *fc);
int fc_capture_frame(FrameCapture *fc); // Returns 1 if new frame, 0 if too soon, -1 on error
int fc_stop(FrameCapture *fc);
void fc_cleanup(FrameCapture *fc);
int fc_update_position(FrameCapture *fc);

// Frame access
XImage* fc_get_frame(FrameCapture *fc);
bool fc_has_new_frame(FrameCapture *fc);
void fc_mark_frame_processed(FrameCapture *fc);

// Utilities
void fc_print_frame_info(FrameCapture *fc);

// Cursor compositing
void fc_composite_cursor(FrameCapture *fc);

// Damage tracking (XOR-based)
int fc_init_damage(FrameCapture *fc);
// Fetch damage rects since last call. Returns damage rectangles from XOR comparison.
int fc_get_damage_rects(FrameCapture *fc, DamageRect **rects_out, int *num_rects_out);

#endif // FRAME_CAPTURE_H