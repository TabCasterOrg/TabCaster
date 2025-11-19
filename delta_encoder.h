#ifndef DELTA_ENCODER_H
#define DELTA_ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include "frame_streamer.h" 

// Delta encoder structure
typedef struct DeltaEncoder {
    unsigned char *reference_frame_rgb;
    size_t reference_size;
    unsigned int reference_width;
    unsigned int reference_height;
    uint32_t reference_checksum;
    
    // Tunables
    int diff_threshold;
    int cover_threshold_pct;
    int region_padding;
    int min_region_size;
    int max_regions_per_frame;
    int region_cell_size;
} DeltaEncoder;

DeltaEncoder* delta_encoder_init(int width, int height);

int delta_encoder_create_frame(DeltaEncoder *encoder, XImage *current_frame, DeltaFrame *out);

void delta_encoder_update_reference(DeltaEncoder *encoder, const unsigned char *rgb_data);

void delta_encoder_cleanup_frame(DeltaFrame *delta_frame);

void delta_encoder_cleanup(DeltaEncoder *encoder);

uint32_t delta_encoder_compute_checksum(const unsigned char *data, size_t size);

#endif // DELTA_ENCODER_H

