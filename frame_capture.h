#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include "display_manager.h"
#include <sys/time.h>
#include <stdbool.h>

// Minimal capture structure - just XGetImage
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
} SimpleCapture;

// Core functions
SimpleCapture* sc_init(DisplayManager *dm, const char *output_name, int fps);
int sc_start(SimpleCapture *sc);
int sc_capture_frame(SimpleCapture *sc);  // Returns 1 if new frame, 0 if too soon, -1 on error
int sc_stop(SimpleCapture *sc);
void sc_cleanup(SimpleCapture *sc);

// Frame access
XImage* sc_get_frame(SimpleCapture *sc);
bool sc_has_new_frame(SimpleCapture *sc);
void sc_mark_frame_processed(SimpleCapture *sc);

// Simple utilities
int sc_save_frame_ppm(SimpleCapture *sc, const char *filename);
void sc_print_frame_info(SimpleCapture *sc);

#endif // FRAME_CAPTURE_H