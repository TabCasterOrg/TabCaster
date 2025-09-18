#ifndef DAMAGE_TRACKER_H
#define DAMAGE_TRACKER_H

#include <stdint.h>
#include <stdbool.h>
#include <X11/Xlib.h>

// Maximum number of damage rectangles to return
#define MAX_DAMAGE_RECTS 64

// Damage rectangle structure (compatible with XRectangle)
typedef struct {
    int16_t x, y;
    uint16_t width, height;
} DamageRect;

// Damage tracking structure
typedef struct {
    // Frame buffers for comparison
    uint32_t *previous_frame;    // Previous frame buffer (RGB24 as uint32_t)
    uint32_t *current_frame;     // Current frame buffer (RGB24 as uint32_t)
    size_t frame_size;           // Size in pixels (width * height)
    size_t buffer_size;          // Size in bytes (frame_size * 3)
    
    // Frame dimensions
    unsigned int width;
    unsigned int height;
    
    // Damage detection arrays
    uint32_t *row_damage_counts; // Damage count per row
    uint32_t *col_damage_counts; // Damage count per column
    bool *damage_mask;           // Per-pixel damage mask (optional)
    
    // Damage rectangles output
    DamageRect damage_rects[MAX_DAMAGE_RECTS];
    int num_damage_rects;
    
    // Performance tracking
    uint64_t total_pixels_compared;
    uint64_t total_damage_pixels;
    uint64_t xor_operations;
    
    // State tracking
    bool first_frame;            // True if this is the first frame (no previous frame to compare)
    
    // Configuration
    bool enable_damage_mask;     // Whether to build per-pixel damage mask
    uint32_t min_damage_threshold; // Minimum pixels to consider a region damaged
} DamageTracker;

// Core functions
DamageTracker* dt_init(unsigned int width, unsigned int height);
int dt_update_frame(DamageTracker *dt, XImage *current_image);
int dt_get_damage_rects(DamageTracker *dt, DamageRect **rects_out, int *num_rects_out);
void dt_cleanup(DamageTracker *dt);

// Utility functions
void dt_print_stats(DamageTracker *dt);
void dt_reset_stats(DamageTracker *dt);
bool dt_has_damage(DamageTracker *dt);

// Configuration functions
void dt_set_damage_mask_enabled(DamageTracker *dt, bool enabled);
void dt_set_min_damage_threshold(DamageTracker *dt, uint32_t threshold);

#endif // DAMAGE_TRACKER_H
