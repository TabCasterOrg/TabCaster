#include "damage_tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

// Initialize damage tracker
DamageTracker* dt_init(unsigned int width, unsigned int height) {
    if (width == 0 || height == 0) {
        fprintf(stderr, "Invalid dimensions: %ux%u\n", width, height);
        return NULL;
    }
    
    DamageTracker *dt = calloc(1, sizeof(DamageTracker));
    if (!dt) {
        fprintf(stderr, "Failed to allocate damage tracker\n");
        return NULL;
    }
    
    dt->width = width;
    dt->height = height;
    dt->frame_size = (size_t)width * (size_t)height;
    dt->buffer_size = dt->frame_size * 3; // RGB24
    
    // Allocate frame buffers (using uint32_t for efficient XOR operations)
    // We'll store RGB24 data in the lower 24 bits of each uint32_t
    dt->previous_frame = calloc(dt->frame_size, sizeof(uint32_t));
    dt->current_frame = calloc(dt->frame_size, sizeof(uint32_t));
    
    if (!dt->previous_frame || !dt->current_frame) {
        fprintf(stderr, "Failed to allocate frame buffers\n");
        dt_cleanup(dt);
        return NULL;
    }
    
    // Allocate damage tracking arrays
    dt->row_damage_counts = calloc(height, sizeof(uint32_t));
    dt->col_damage_counts = calloc(width, sizeof(uint32_t));
    
    if (!dt->row_damage_counts || !dt->col_damage_counts) {
        fprintf(stderr, "Failed to allocate damage tracking arrays\n");
        dt_cleanup(dt);
        return NULL;
    }
    
    // Initialize damage mask if enabled (disabled by default for performance)
    dt->enable_damage_mask = false;
    dt->damage_mask = NULL;
    
    // Initialize configuration
    dt->min_damage_threshold = 1; // Consider any pixel change as damage
    dt->first_frame = true; // First frame will be treated as full damage
    
    // Initialize stats
    dt_reset_stats(dt);
    
    printf("Damage tracker initialized: %ux%u (%zu pixels, %zu bytes)\n", 
           width, height, dt->frame_size, dt->buffer_size);
    
    return dt;
}

// Convert XImage pixel to RGB24 uint32_t (lower 24 bits)
static uint32_t ximage_pixel_to_rgb24(unsigned long pixel) {
    // Extract RGB components and pack into 24-bit value
    unsigned char r = (pixel >> 16) & 0xFF;
    unsigned char g = (pixel >> 8) & 0xFF;
    unsigned char b = pixel & 0xFF;
    
    return (uint32_t)((r << 16) | (g << 8) | b);
}

// Update frame and detect damage
int dt_update_frame(DamageTracker *dt, XImage *current_image) {
    if (!dt || !current_image) {
        return -1;
    }
    
    // Validate image dimensions
    if (current_image->width != (int)dt->width || 
        current_image->height != (int)dt->height) {
        fprintf(stderr, "Image dimensions mismatch: expected %ux%u, got %dx%d\n",
                dt->width, dt->height, current_image->width, current_image->height);
        return -1;
    }
    
    // Clear damage tracking arrays
    memset(dt->row_damage_counts, 0, dt->height * sizeof(uint32_t));
    memset(dt->col_damage_counts, 0, dt->width * sizeof(uint32_t));
    
    if (dt->damage_mask) {
        memset(dt->damage_mask, 0, dt->frame_size * sizeof(bool));
    }
    
    // Convert current frame to RGB24 format and store in current_frame buffer
    for (unsigned int y = 0; y < dt->height; y++) {
        for (unsigned int x = 0; x < dt->width; x++) {
            unsigned long pixel = XGetPixel(current_image, x, y);
            uint32_t rgb24 = ximage_pixel_to_rgb24(pixel);
            
            size_t idx = y * dt->width + x;
            dt->current_frame[idx] = rgb24;
        }
    }
    
    // Perform XOR comparison and damage detection
    uint32_t total_damage_pixels = 0;
    
    if (dt->first_frame) {
        // First frame: mark entire frame as damaged
        total_damage_pixels = (uint32_t)dt->frame_size;
        
        // Mark all rows and columns as damaged
        for (unsigned int y = 0; y < dt->height; y++) {
            dt->row_damage_counts[y] = dt->width;
        }
        for (unsigned int x = 0; x < dt->width; x++) {
            dt->col_damage_counts[x] = dt->height;
        }
        
        // Update damage mask if enabled
        if (dt->damage_mask) {
            memset(dt->damage_mask, true, dt->frame_size * sizeof(bool));
        }
        
        dt->first_frame = false;
        printf("First frame detected - marking entire frame as damaged (%u pixels)\n", total_damage_pixels);
    } else {
        // Normal XOR comparison
        for (size_t i = 0; i < dt->frame_size; i++) {
            uint32_t prev_pixel = dt->previous_frame[i];
            uint32_t curr_pixel = dt->current_frame[i];
            uint32_t diff = prev_pixel ^ curr_pixel;
            
            dt->xor_operations++;
            
            if (diff != 0) {
                // Pixel has changed
                total_damage_pixels++;
                
                // Calculate row and column
                unsigned int y = (unsigned int)(i / dt->width);
                unsigned int x = (unsigned int)(i % dt->width);
                
                // Update damage counts
                dt->row_damage_counts[y]++;
                dt->col_damage_counts[x]++;
                
                // Update damage mask if enabled
                if (dt->damage_mask) {
                    dt->damage_mask[i] = true;
                }
            }
        }
        
        if (total_damage_pixels > 0) {
            printf("Damage detected: %u pixels changed\n", total_damage_pixels);
        }
    }
    
    dt->total_pixels_compared += dt->frame_size;
    dt->total_damage_pixels += total_damage_pixels;
    
    // Copy current frame to previous frame for next comparison
    // Don't swap pointers - copy the data instead since we always write to current_frame
    memcpy(dt->previous_frame, dt->current_frame, dt->frame_size * sizeof(uint32_t));
    
    return 0;
}

// Extract damage rectangles from row/column damage counts
int dt_get_damage_rects(DamageTracker *dt, DamageRect **rects_out, int *num_rects_out) {
    if (!dt || !rects_out || !num_rects_out) {
        return -1;
    }
    
    *rects_out = NULL;
    *num_rects_out = 0;
    
    // Find bounding box of damage
    int min_row = -1, max_row = -1;
    int min_col = -1, max_col = -1;
    
    // Find first and last damaged rows
    for (unsigned int y = 0; y < dt->height; y++) {
        if (dt->row_damage_counts[y] > 0) {
            if (min_row == -1) min_row = (int)y;
            max_row = (int)y;
        }
    }
    
    // Find first and last damaged columns
    for (unsigned int x = 0; x < dt->width; x++) {
        if (dt->col_damage_counts[x] > 0) {
            if (min_col == -1) min_col = (int)x;
            max_col = (int)x;
        }
    }
    
    // No damage detected
    if (min_row == -1 || min_col == -1) {
        return 0;
    }
    
    // Create single bounding rectangle
    dt->damage_rects[0].x = (int16_t)min_col;
    dt->damage_rects[0].y = (int16_t)min_row;
    dt->damage_rects[0].width = (uint16_t)(max_col - min_col + 1);
    dt->damage_rects[0].height = (uint16_t)(max_row - min_row + 1);
    dt->num_damage_rects = 1;
    
    // TODO: Implement multiple rectangle detection for disjoint regions
    // This would involve flood-fill or connected component analysis
    
    *rects_out = dt->damage_rects;
    *num_rects_out = dt->num_damage_rects;
    
    return 0;
}

// Check if there's any damage
bool dt_has_damage(DamageTracker *dt) {
    if (!dt) return false;
    
    for (unsigned int y = 0; y < dt->height; y++) {
        if (dt->row_damage_counts[y] > 0) {
            return true;
        }
    }
    return false;
}

// Print damage tracking statistics
void dt_print_stats(DamageTracker *dt) {
    if (!dt) return;
    
    printf("Damage Tracker Stats:\n");
    printf("  Dimensions: %ux%u\n", dt->width, dt->height);
    printf("  Total pixels compared: %lu\n", dt->total_pixels_compared);
    printf("  Total damage pixels: %lu\n", dt->total_damage_pixels);
    printf("  XOR operations: %lu\n", dt->xor_operations);
    
    if (dt->total_pixels_compared > 0) {
        double damage_ratio = (double)dt->total_damage_pixels / (double)dt->total_pixels_compared;
        printf("  Damage ratio: %.4f%%\n", damage_ratio * 100.0);
    }
    
    if (dt_has_damage(dt)) {
        printf("  Current damage: YES (%d rects)\n", dt->num_damage_rects);
        for (int i = 0; i < dt->num_damage_rects; i++) {
            printf("    Rect %d: %dx%d+%d+%d\n", i,
                   dt->damage_rects[i].width, dt->damage_rects[i].height,
                   dt->damage_rects[i].x, dt->damage_rects[i].y);
        }
    } else {
        printf("  Current damage: NO\n");
    }
}

// Reset statistics
void dt_reset_stats(DamageTracker *dt) {
    if (!dt) return;
    
    dt->total_pixels_compared = 0;
    dt->total_damage_pixels = 0;
    dt->xor_operations = 0;
    dt->num_damage_rects = 0;
    dt->first_frame = true; // Reset to first frame state
}

// Enable/disable damage mask
void dt_set_damage_mask_enabled(DamageTracker *dt, bool enabled) {
    if (!dt) return;
    
    if (enabled && !dt->damage_mask) {
        dt->damage_mask = calloc(dt->frame_size, sizeof(bool));
        if (!dt->damage_mask) {
            fprintf(stderr, "Failed to allocate damage mask\n");
            return;
        }
    } else if (!enabled && dt->damage_mask) {
        free(dt->damage_mask);
        dt->damage_mask = NULL;
    }
    
    dt->enable_damage_mask = enabled;
}

// Set minimum damage threshold
void dt_set_min_damage_threshold(DamageTracker *dt, uint32_t threshold) {
    if (!dt) return;
    dt->min_damage_threshold = threshold;
}

// Cleanup damage tracker
void dt_cleanup(DamageTracker *dt) {
    if (!dt) return;
    
    if (dt->previous_frame) {
        free(dt->previous_frame);
    }
    if (dt->current_frame) {
        free(dt->current_frame);
    }
    if (dt->row_damage_counts) {
        free(dt->row_damage_counts);
    }
    if (dt->col_damage_counts) {
        free(dt->col_damage_counts);
    }
    if (dt->damage_mask) {
        free(dt->damage_mask);
    }
    
    free(dt);
}
