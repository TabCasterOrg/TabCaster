#include "delta_encoder.h"
#include "png_encoder.h"  
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    int x, y, width, height;
    int changed_pixels;
} ChangeRegion;

static void apply_region_constraints(int *x, int *y, int *w, int *h, 
                                   int frame_width, int frame_height,
                                   int padding, int min_size) {
    if (!x || !y || !w || !h) return;
    
    // Apply padding
    int padded_x = *x - padding;
    int padded_y = *y - padding;
    int padded_w = *w + (2 * padding);
    int padded_h = *h + (2 * padding);
    
    // Clamp to frame boundaries
    if (padded_x < 0) padded_x = 0;
    if (padded_y < 0) padded_y = 0;
    if (padded_x + padded_w > frame_width) padded_w = frame_width - padded_x;
    if (padded_y + padded_h > frame_height) padded_h = frame_height - padded_y;
    
    // Apply minimum size constraint
    if (padded_w < min_size) {
        // Center the region horizontally
        int center_x = padded_x + padded_w / 2;
        padded_x = center_x - min_size / 2;
        if (padded_x < 0) padded_x = 0;
        if (padded_x + min_size > frame_width) padded_x = frame_width - min_size;
        padded_w = min_size;
    }
    
    if (padded_h < min_size) {
        // Center the region vertically
        int center_y = padded_y + padded_h / 2;
        padded_y = center_y - min_size / 2;
        if (padded_y < 0) padded_y = 0;
        if (padded_y + min_size > frame_height) padded_y = frame_height - min_size;
        padded_h = min_size;
    }
    
    // Update output values
    *x = padded_x;
    *y = padded_y;
    *w = padded_w;
    *h = padded_h;
}

static int detect_multiple_regions(const unsigned char *prev_rgb,
                                  const unsigned char *curr_bgrx,
                                  int width, int height, int bytes_per_line,
                                  int threshold, int max_regions, int cell_size,
                                  ChangeRegion *regions, int *region_count) {
    if (!prev_rgb || !curr_bgrx || !regions || !region_count || max_regions <= 0) {
        return 0;
    }
    
    *region_count = 0;
    
    int min_cell_size = 16; 
    int actual_cell_size = (cell_size < min_cell_size) ? min_cell_size : cell_size;
    
    int cells_x = (width + actual_cell_size - 1) / actual_cell_size;
    int cells_y = (height + actual_cell_size - 1) / actual_cell_size;
    
    bool *change_map = (bool*)calloc(cells_x * cells_y, sizeof(bool));
    if (!change_map) return 0;
    
    // First pass: detect changed cells
    for (int cy = 0; cy < cells_y; cy++) {
        for (int cx = 0; cx < cells_x; cx++) {
            int cell_x = cx * actual_cell_size;
            int cell_y = cy * actual_cell_size;
            int cell_w = (cell_x + actual_cell_size > width) ? width - cell_x : actual_cell_size;
            int cell_h = (cell_y + actual_cell_size > height) ? height - cell_y : actual_cell_size;
            
            // Check if this cell has significant changes
            int changed_pixels = 0;
            for (int y = cell_y; y < cell_y + cell_h; y++) {
                const unsigned char *curr_line = curr_bgrx + y * bytes_per_line;
                const unsigned char *prev_line = prev_rgb + y * (width * 3);
                
                for (int x = cell_x; x < cell_x + cell_w; x++) {
                    // Extract BGRX components from current frame
                    unsigned char b = curr_line[x * 4 + 0];
                    unsigned char g = curr_line[x * 4 + 1];
                    unsigned char r = curr_line[x * 4 + 2];
                    
                    // Extract RGB components from previous frame
                    int prev_idx = x * 3;
                    unsigned char prev_r = prev_line[prev_idx + 0];
                    unsigned char prev_g = prev_line[prev_idx + 1];
                    unsigned char prev_b = prev_line[prev_idx + 2];
                    
                    // Calculate absolute differences
                    int dr = (int)r - (int)prev_r;
                    int dg = (int)g - (int)prev_g;
                    int db = (int)b - (int)prev_b;
                    
                    int abs_dr = (dr < 0) ? -dr : dr;
                    int abs_dg = (dg < 0) ? -dg : dg;
                    int abs_db = (db < 0) ? -db : db;
                    
                    int total_diff = abs_dr + abs_dg + abs_db;
                    
                    if (total_diff > threshold) {
                        changed_pixels++;
                    }
                }
            }
            
            // Mark cell as changed if it has enough changes
            int cell_pixels = cell_w * cell_h;
            double change_ratio = (double)changed_pixels / (double)cell_pixels;
            
            if (change_ratio > 0.02) { // Lower threshold: 2% of pixels changed
                change_map[cy * cells_x + cx] = true;
            }
        }
    }
    
    // Second pass: group connected changed cells into regions
    bool *visited = (bool*)calloc(cells_x * cells_y, sizeof(bool));
    if (!visited) {
        free(change_map);
        return 0;
    }
    
    for (int cy = 0; cy < cells_y && *region_count < max_regions; cy++) {
        for (int cx = 0; cx < cells_x && *region_count < max_regions; cx++) {
            int cell_idx = cy * cells_x + cx;
            
            if (change_map[cell_idx] && !visited[cell_idx]) {
                // Found a new region, flood-fill to find connected cells
                int min_x = cx, max_x = cx, min_y = cy, max_y = cy;
                int total_changed_pixels = 0;
                
                // Simple flood-fill to find connected region
                int stack_size = 0;
                int *stack = (int*)malloc(cells_x * cells_y * 2 * sizeof(int));
                if (!stack) break;
                
                stack[stack_size++] = cx;
                stack[stack_size++] = cy;
                visited[cell_idx] = true;
                
                while (stack_size > 0) {
                    int current_cy = stack[--stack_size];
                    int current_cx = stack[--stack_size];
                    
                    // Update bounding box
                    if (current_cx < min_x) min_x = current_cx;
                    if (current_cx > max_x) max_x = current_cx;
                    if (current_cy < min_y) min_y = current_cy;
                    if (current_cy > max_y) max_y = current_cy;
                    
                    // Check neighbors 
                    int neighbors[4][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}};
                    for (int n = 0; n < 4; n++) {
                        int nx = current_cx + neighbors[n][0];
                        int ny = current_cy + neighbors[n][1];
                        
                        if (nx >= 0 && nx < cells_x && ny >= 0 && ny < cells_y) {
                            int neighbor_idx = ny * cells_x + nx;
                            if (change_map[neighbor_idx] && !visited[neighbor_idx]) {
                                visited[neighbor_idx] = true;
                                if (stack_size < cells_x * cells_y * 2 - 2) {
                                    stack[stack_size++] = nx;
                                    stack[stack_size++] = ny;
                                }
                            }
                        }
                    }
                }
                
                free(stack);
                
                // Convert cell coordinates to pixel coordinates
                regions[*region_count].x = min_x * actual_cell_size;
                regions[*region_count].y = min_y * actual_cell_size;
                regions[*region_count].width = (max_x - min_x + 1) * actual_cell_size;
                regions[*region_count].height = (max_y - min_y + 1) * actual_cell_size;
                
                // Clamp to image boundaries
                if (regions[*region_count].x + regions[*region_count].width > width) {
                    regions[*region_count].width = width - regions[*region_count].x;
                }
                if (regions[*region_count].y + regions[*region_count].height > height) {
                    regions[*region_count].height = height - regions[*region_count].y;
                }
                
                regions[*region_count].changed_pixels = total_changed_pixels;
                (*region_count)++;
            }
        }
    }
    
    free(change_map);
    free(visited);
    
    return (*region_count > 0) ? 1 : 0;
}

// Compute checksum for reference frame validation
uint32_t delta_encoder_compute_checksum(const unsigned char *data, size_t size) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        checksum = checksum * 31 + data[i];
    }
    return checksum;
}

// Initialize delta encoder
DeltaEncoder* delta_encoder_init(int width, int height) {
    DeltaEncoder *encoder = calloc(1, sizeof(DeltaEncoder));
    if (!encoder) return NULL;
    
    encoder->reference_width = width;
    encoder->reference_height = height;
    encoder->reference_size = (size_t)width * (size_t)height * 3;
    encoder->reference_frame_rgb = (unsigned char*)malloc(encoder->reference_size);
    
    if (!encoder->reference_frame_rgb) {
        free(encoder);
        return NULL;
    }
    
    // Default tunables
    encoder->diff_threshold = 30;
    encoder->cover_threshold_pct = 80;
    encoder->region_padding = 8;
    encoder->min_region_size = 32;
    encoder->max_regions_per_frame = 8;
    encoder->region_cell_size = 32;
    
    // Load from environment variables
    const char *th_env = getenv("TABC_THRESH");
    const char *cov_env = getenv("TABC_COVER");
    const char *pad_env = getenv("TABC_PADDING");
    const char *minsize_env = getenv("TABC_MINSIZE");
    const char *maxregions_env = getenv("TABC_MAXREGIONS");
    const char *cellsize_env = getenv("TABC_CELLSIZE");
    
    if (th_env) encoder->diff_threshold = atoi(th_env);
    if (cov_env) encoder->cover_threshold_pct = atoi(cov_env);
    if (pad_env) encoder->region_padding = atoi(pad_env);
    if (minsize_env) encoder->min_region_size = atoi(minsize_env);
    if (maxregions_env) encoder->max_regions_per_frame = atoi(maxregions_env);
    if (cellsize_env) encoder->region_cell_size = atoi(cellsize_env);
    
    // Cap max_regions_per_frame to prevent stack overflow
    if (encoder->max_regions_per_frame > MAX_DELTA_OPERATIONS / 2) {
        encoder->max_regions_per_frame = MAX_DELTA_OPERATIONS / 2;
        printf("Warning: max_regions_per_frame capped at %d (MAX_DELTA_OPERATIONS/2)\n", 
               encoder->max_regions_per_frame);
    }
    
    return encoder;
}

// Create a delta frame from current frame
int delta_encoder_create_frame(DeltaEncoder *encoder, XImage *current_frame, DeltaFrame *delta_frame) {
    if (!encoder || !current_frame || !delta_frame) return -1;
    
    // Initialize delta frame
    memset(delta_frame, 0, sizeof(DeltaFrame));
    delta_frame->operation_count = 0;
    delta_frame->total_payload_size = 0;
    
    // Only proceed if we have a reference frame
    if (!encoder->reference_frame_rgb ||
        encoder->reference_width != (unsigned int)current_frame->width ||
        encoder->reference_height != (unsigned int)current_frame->height) {
        return -1;
    }
    
    // Detect multiple change regions
    // Limit regions to half of max operations (each region needs 2 operations: CLEAR + DRAW)
    int max_regions = (MAX_DELTA_OPERATIONS / 2 < encoder->max_regions_per_frame) ? 
                      (MAX_DELTA_OPERATIONS / 2) : encoder->max_regions_per_frame;
    
    ChangeRegion *regions = (ChangeRegion*)malloc(max_regions * sizeof(ChangeRegion));
    if (!regions) {
        fprintf(stderr, "Failed to allocate regions array\n");
        return -1;
    }
    
    int region_count = 0;
    
    int changed = detect_multiple_regions(encoder->reference_frame_rgb,
                                        (const unsigned char*)current_frame->data,
                                        current_frame->width, current_frame->height,
                                        current_frame->bytes_per_line,
                                        encoder->diff_threshold,
                                        max_regions,
                                        encoder->region_cell_size,
                                        regions, &region_count);
    
    if (!changed || region_count == 0) {
        free(regions);
        return 1; // No changes detected
    }
    
    // Check total coverage threshold
    size_t total_pixels = (size_t)current_frame->width * (size_t)current_frame->height;
    int total_changed_pixels = 0;
    for (int i = 0; i < region_count; i++) {
        total_changed_pixels += regions[i].changed_pixels;
    }
    double coverage_pct = (double)total_changed_pixels / (double)total_pixels * 100.0;
    
    if (coverage_pct > encoder->cover_threshold_pct) {
        free(regions);
        return 1; // Coverage too high
    }
    
    for (int r = 0; r < region_count && delta_frame->operation_count < MAX_DELTA_OPERATIONS - 1; r++) {
        int rx = regions[r].x;
        int ry = regions[r].y;
        int rw = regions[r].width;
        int rh = regions[r].height;
        
        apply_region_constraints(&rx, &ry, &rw, &rh, 
                               current_frame->width, current_frame->height,
                               encoder->region_padding, encoder->min_region_size);
        
        DeltaOperation *clear_op = &delta_frame->operations[delta_frame->operation_count];
        clear_op->type = OP_CREG;
        clear_op->x = rx;
        clear_op->y = ry;
        clear_op->width = rw;
        clear_op->height = rh;
        
        size_t region_pixels = (size_t)rw * (size_t)rh;
        size_t region_rgb_size = region_pixels * 3;
        unsigned char *region_rgb = (unsigned char*)malloc(region_rgb_size);
        if (!region_rgb) {
            fprintf(stderr, "Failed to allocate region RGB buffer for CREG\n");
            delta_encoder_cleanup_frame(delta_frame);
            free(regions);
            return -1;
        }
        
        // Extract region from reference frame (RGB format)
        for (int y = 0; y < rh; y++) {
            unsigned char *ref_line = encoder->reference_frame_rgb + 
                                     ((size_t)(ry + y) * (size_t)current_frame->width + (size_t)rx) * 3;
            unsigned char *region_line = region_rgb + (size_t)y * (size_t)rw * 3;
            memcpy(region_line, ref_line, (size_t)rw * 3);
        }
        
        // Encode reference region to PNG
        if (png_encode_rgb(region_rgb, rw, rh, &clear_op->png_data, &clear_op->png_size) != 0) {
            fprintf(stderr, "Failed to encode CREG region to PNG\n");
            free(region_rgb);
            delta_encoder_cleanup_frame(delta_frame);
            free(regions);
            return -1;
        }
        
        free(region_rgb);
        delta_frame->operation_count++;
        delta_frame->total_payload_size += 4 + 2+2+2+2 + 1+1+2 + clear_op->png_size; // Header + PNG
        
        DeltaOperation *draw_op = &delta_frame->operations[delta_frame->operation_count];
        draw_op->type = OP_DREG;
        draw_op->x = rx;
        draw_op->y = ry;
        draw_op->width = rw;
        draw_op->height = rh;
        
        // Extract the region from current frame (BGRX -> RGB)
        unsigned char *current_region_rgb = (unsigned char*)malloc(region_rgb_size);
        if (!current_region_rgb) {
            fprintf(stderr, "Failed to allocate current region RGB buffer for DREG\n");
            delta_encoder_cleanup_frame(delta_frame);
            free(regions);
            return -1;
        }
        
        for (int y = 0; y < rh; y++) {
            const unsigned char *line_src = (unsigned char*)current_frame->data + 
                                           (size_t)(ry + y) * (size_t)current_frame->bytes_per_line;
            unsigned char *line_dst = current_region_rgb + (size_t)y * (size_t)rw * 3;
            for (int x = 0; x < rw; x++) {
                unsigned char b = line_src[(rx + x) * 4 + 0];
                unsigned char g = line_src[(rx + x) * 4 + 1];
                unsigned char r = line_src[(rx + x) * 4 + 2];
                line_dst[x * 3 + 0] = r;
                line_dst[x * 3 + 1] = g;
                line_dst[x * 3 + 2] = b;
            }
        }
        
        // Encode current region to PNG
        if (png_encode_rgb(current_region_rgb, rw, rh, &draw_op->png_data, &draw_op->png_size) != 0) {
            fprintf(stderr, "Failed to encode DREG region to PNG\n");
            free(current_region_rgb);
            delta_encoder_cleanup_frame(delta_frame);
            free(regions);
            return -1;
        }
        
        free(current_region_rgb);
        delta_frame->operation_count++;
        delta_frame->total_payload_size += 4 + 2+2+2+2 + 1+1+2 + draw_op->png_size; // Header + PNG
    }
    
    printf("Created delta frame with %d operations (%d regions): ", 
           delta_frame->operation_count, region_count);
    for (int r = 0; r < region_count; r++) {
        printf("(%d,%d) %dx%d ", regions[r].x, regions[r].y, regions[r].width, regions[r].height);
    }
    printf("\n");
    
    // Clean up regions array
    free(regions);
    
    return 0; // Success
}

// Update reference frame with new RGB data
void delta_encoder_update_reference(DeltaEncoder *encoder, const unsigned char *rgb_data) {
    if (!encoder || !rgb_data || !encoder->reference_frame_rgb) return;
    
    memcpy(encoder->reference_frame_rgb, rgb_data, encoder->reference_size);
    encoder->reference_checksum = delta_encoder_compute_checksum(rgb_data, encoder->reference_size);
}

// Cleanup delta frame resources
void delta_encoder_cleanup_frame(DeltaFrame *delta_frame) {
    if (!delta_frame) return;
    
    for (int i = 0; i < delta_frame->operation_count; i++) {
        if (delta_frame->operations[i].png_data) {
            free(delta_frame->operations[i].png_data);
            delta_frame->operations[i].png_data = NULL;
        }
    }
    
    memset(delta_frame, 0, sizeof(DeltaFrame));
}

// Cleanup encoder
void delta_encoder_cleanup(DeltaEncoder *encoder) {
    if (!encoder) return;
    
    free(encoder->reference_frame_rgb);
    encoder->reference_frame_rgb = NULL;
    free(encoder);
}

