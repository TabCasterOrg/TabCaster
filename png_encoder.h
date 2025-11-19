#ifndef PNG_ENCODER_H
#define PNG_ENCODER_H

#include <X11/Xlib.h>
#include <stddef.h>

// Encode raw RGB24 buffer to PNG
int png_encode_rgb(const unsigned char *rgb, int width, int height, 
                   unsigned char **png_out, size_t *size_out);

// Convert frame from BGRX to RGB format
void png_convert_bgrx_to_rgb(const XImage *frame, unsigned char *rgb_out);

#endif // PNG_ENCODER_H

