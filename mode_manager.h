#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include "display_manager.h"
#include <libxcvt/libxcvt.h>
#include <X11/extensions/Xrandr.h>
#include <stdbool.h>

// Existing functions
RRMode mode_create_cvt(DisplayManager *dm, unsigned int width, unsigned int height, 
                      double refresh_rate, bool reduced_blanking);
int mode_add_to_output(DisplayManager *dm, const char *output_name, RRMode mode_id);
int mode_remove_from_output(DisplayManager *dm, const char *output_name, RRMode mode_id);
int mode_delete_from_xrandr(DisplayManager *dm, RRMode mode_id);
void mode_print_libxcvt_info(const struct libxcvt_mode_info *cvt_mode, double refresh_rate);
RRMode mode_find_by_name(DisplayManager *dm, const char *mode_name);

// New functions - Enable/disable output functions
int mode_enable_output_with_mode(DisplayManager *dm, const char *output_name, 
                                 const char *mode_name, int x_pos, int y_pos);
int mode_enable_output_with_mode_id(DisplayManager *dm, const char *output_name, 
                                   RRMode mode_id, int x_pos, int y_pos);
int mode_disable_output(DisplayManager *dm, const char *output_name);

// New functions - Mode listing functions
void mode_print_output_modes(DisplayManager *dm, const char *output_name);
void mode_print_all_output_modes(DisplayManager *dm);

// New functions - Output status functions
bool mode_is_output_enabled(DisplayManager *dm, const char *output_name);
int mode_get_output_config(DisplayManager *dm, const char *output_name, 
                          RRMode *current_mode, int *x, int *y, 
                          unsigned int *width, unsigned int *height);

#endif // MODE_MANAGER_H