#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include "display_manager.h"
#include <stdbool.h>
#include <X11/extensions/Xrandr.h>
#include <libxcvt/libxcvt.h>

// CVT mode creation using libxcvt
RRMode mode_create_cvt(DisplayManager *dm, unsigned int width, unsigned int height, 
                      double refresh_rate, bool reduced_blanking);

// Mode management functions
int mode_add_to_output(DisplayManager *dm, const char *output_name, RRMode mode_id);
int mode_remove_from_output(DisplayManager *dm, const char *output_name, RRMode mode_id);
int mode_delete_from_xrandr(DisplayManager *dm, RRMode mode_id);

// Mode search and information
RRMode mode_find_by_name(DisplayManager *dm, const char *mode_name);
void mode_print_libxcvt_info(const struct libxcvt_mode_info *cvt_mode, double refresh_rate);
void mode_print_output_modes(DisplayManager *dm, const char *output_name);
void mode_print_all_output_modes(DisplayManager *dm);

// Output control functions
int mode_enable_output_with_mode(DisplayManager *dm, const char *output_name, 
                                 const char *mode_name, int x_pos, int y_pos);

// Original positioning version (maintains backward compatibility)
int mode_enable_output_with_mode_id(DisplayManager *dm, const char *output_name, 
                                   RRMode mode_id, int x_pos, int y_pos);

// Enhanced positioning version with auto left-of support
int mode_enable_output_with_mode_id_positioned(DisplayManager *dm, const char *output_name, 
                                             RRMode mode_id, int x_pos, int y_pos, 
                                             bool auto_right_of);

int mode_disable_output(DisplayManager *dm, const char *output_name);

// Output status and configuration
bool mode_is_output_enabled(DisplayManager *dm, const char *output_name);
int mode_get_output_config(DisplayManager *dm, const char *output_name, 
                          RRMode *current_mode, int *x, int *y, 
                          unsigned int *width, unsigned int *height);

// Desktop management functions (NEW)
int mode_calculate_right_of_position(DisplayManager *dm, int *x, int *y, 
                                   unsigned int width, unsigned int height);
int mode_expand_desktop_for_screens(DisplayManager *dm);

// Error handler for X11
int ignore_badmatch(Display *d, XErrorEvent *e);

#endif // MODE_MANAGER_H