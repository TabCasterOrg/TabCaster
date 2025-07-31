#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include "display_manager.h"
#include <libxcvt/libxcvt.h>
#include <X11/extensions/Xrandr.h>
#include <stdbool.h>

// Simple mode specification for input
typedef struct {
    unsigned int width;
    unsigned int height;
    double refresh_rate;
    bool reduced_blanking;
} ModeSpec;

// Core mode management functions 
RRMode mode_create_cvt(DisplayManager *dm, unsigned int width, unsigned int height, 
                      double refresh_rate, bool reduced_blanking);
int mode_add_to_output(DisplayManager *dm, const char *output_name, RRMode mode_id);
int mode_remove_from_output(DisplayManager *dm, const char *output_name, RRMode mode_id);
int mode_delete_from_xrandr(DisplayManager *dm, RRMode mode_id);

// Utility functions
void mode_print_libxcvt_info(const struct libxcvt_mode_info *cvt_mode, double refresh_rate);
RRMode mode_find_by_name(DisplayManager *dm, const char *mode_name);

#endif