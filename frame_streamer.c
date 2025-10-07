#include "frame_streamer.h"
#include "udp_server.h"
#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <png.h>
#include <setjmp.h>

static volatile bool keep_streaming = true;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    (void)sig;
    keep_streaming = false;
}

// PNG encoding structure for memory writing
typedef struct {
    unsigned char *data;
    size_t size;
    size_t allocated;
    size_t offset;
} PNGMemoryData;

// Simple checksum for reference frame validation
static uint32_t compute_checksum(const unsigned char *data, size_t size) {
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        checksum = checksum * 31 + data[i];
    }
    return checksum;
}

// Helper function to convert full frame from BGRX to RGB
static void convert_frame_bgrx_to_rgb(const XImage *frame, unsigned char *rgb_out) {
    if (!frame || !rgb_out) return;
    
    unsigned char *src = (unsigned char*)frame->data;
    for (int y = 0; y < frame->height; y++) {
        unsigned char *line_src = src + (y * frame->bytes_per_line);
        unsigned char *line_dst = rgb_out + (size_t)y * (size_t)frame->width * 3;
        for (int x = 0; x < frame->width; x++) {
            unsigned char b = line_src[x * 4 + 0];
            unsigned char g = line_src[x * 4 + 1];
            unsigned char r = line_src[x * 4 + 2];
            line_dst[x * 3 + 0] = r;
            line_dst[x * 3 + 1] = g;
            line_dst[x * 3 + 2] = b;
        }
    }
}

// Absolute difference-based region detection with pixel counting and debug output
static int compute_changed_bounds_rgb24(const unsigned char *prev_rgb,
                                        const unsigned char *curr_bgrx,
                                        int width,
                                        int height,
                                        int bytes_per_line,
                                        int threshold,
                                        int *out_x,
                                        int *out_y,
                                        int *out_w,
                                        int *out_h,
                                        int *out_changed_pixels) {
    // Validation for edge cases
    if (!prev_rgb || !curr_bgrx || !out_x || !out_y || !out_w || !out_h || !out_changed_pixels) {
        return 0;
    }
    if (width <= 0 || height <= 0 || threshold < 0) {
        return 0;
    }
    if (bytes_per_line < width * 4) {
        return 0; // Invalid bytes_per_line for BGRX format
    }
    
    // Initialize bounds tracking
    int min_x = width, min_y = height, max_x = -1, max_y = -1;
    int changed_pixels = 0;
    int total_pixels = width * height;
    
    // Debug output for first few frames
    static int debug_frame_count = 0;
    static int total_debug_frames = 3;
    bool debug_output = (debug_frame_count < total_debug_frames);
    
    if (debug_output) {
        printf("=== Change Detection Debug Frame %d ===\n", debug_frame_count);
        printf("  Dimensions: %dx%d, threshold=%d, bytes_per_line=%d\n", 
               width, height, threshold, bytes_per_line);
    }
    
    // Pixel-by-pixel comparison with better difference calculation
    for (int y = 0; y < height; y++) {
        const unsigned char *curr_line = curr_bgrx + y * bytes_per_line;
        const unsigned char *prev_line = prev_rgb + y * (width * 3);
        
        for (int x = 0; x < width; x++) {
            // Extract BGRX components from current frame
            unsigned char b = curr_line[x * 4 + 0];
            unsigned char g = curr_line[x * 4 + 1];
            unsigned char r = curr_line[x * 4 + 2];
            
            // Extract RGB components from previous frame
            int prev_idx = x * 3;
            unsigned char prev_r = prev_line[prev_idx + 0];
            unsigned char prev_g = prev_line[prev_idx + 1];
            unsigned char prev_b = prev_line[prev_idx + 2];
            
            // Absolute difference calculation
            int dr = (int)r - (int)prev_r;
            int dg = (int)g - (int)prev_g;
            int db = (int)b - (int)prev_b;
            
            // Calculate absolute differences
            int abs_dr = (dr < 0) ? -dr : dr;
            int abs_dg = (dg < 0) ? -dg : dg;
            int abs_db = (db < 0) ? -db : db;
            
            // Sum of absolute differences (SAD)
            int total_diff = abs_dr + abs_dg + abs_db;
            
            // Check if pixel has changed significantly
            if (total_diff > threshold) {
                changed_pixels++;
                
                // Update bounding box
                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
                if (x > max_x) max_x = x;
                if (y > max_y) max_y = y;
                
                // Debug output for first few changed pixels in debug frames
                if (debug_output && changed_pixels <= 5) {
                    printf("  Changed pixel[%d,%d]: RGB(%d,%d,%d) -> RGB(%d,%d,%d), diff=%d\n",
                           x, y, prev_r, prev_g, prev_b, r, g, b, total_diff);
                }
            }
        }
    }
    
    // Calculate coverage percentage
    double coverage_pct = (double)changed_pixels / (double)total_pixels * 100.0;
    
    // Debug output for frame summary
    if (debug_output) {
        printf("  Changed pixels: %d/%d (%.2f%%)\n", changed_pixels, total_pixels, coverage_pct);
        if (changed_pixels > 0) {
            printf("  Bounding box: (%d,%d) to (%d,%d) = %dx%d\n",
                   min_x, min_y, max_x, max_y, max_x - min_x + 1, max_y - min_y + 1);
        }
        printf("=== End Debug Frame %d ===\n\n", debug_frame_count);
        debug_frame_count++;
    }
    
    // Set changed pixels count
    *out_changed_pixels = changed_pixels;
    
    // No changes detected
    if (max_x < 0) {
        return 0;
    }
    
    // Validate and set output bounds
    *out_x = min_x;
    *out_y = min_y;
    *out_w = (max_x - min_x + 1);
    *out_h = (max_y - min_y + 1);
    
    // Additional validation for edge cases
    if (*out_w <= 0 || *out_h <= 0) {
        return 0;
    }
    if (*out_x < 0 || *out_y < 0 || *out_x + *out_w > width || *out_y + *out_h > height) {
        return 0;
    }
    
    return 1;
}

// Apply padding and minimum size constraints to region bounds
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

// Structure to hold a single change region
typedef struct {
    int x, y, width, height;
    int changed_pixels;
} ChangeRegion;

// Detect multiple separate change regions using connected component analysis
static int detect_multiple_regions(const unsigned char *prev_rgb,
                                  const unsigned char *curr_bgrx,
                                  int width, int height, int bytes_per_line,
                                  int threshold, int max_regions, int cell_size,
                                  ChangeRegion *regions, int *region_count) {
    if (!prev_rgb || !curr_bgrx || !regions || !region_count || max_regions <= 0) {
        return 0;
    }
    
    *region_count = 0;
    
    // Use smaller cells for better granularity, but group nearby changes
    int min_cell_size = 16; // Minimum 16x16 cells for better granularity
    int actual_cell_size = (cell_size < min_cell_size) ? min_cell_size : cell_size;
    
    int cells_x = (width + actual_cell_size - 1) / actual_cell_size;
    int cells_y = (height + actual_cell_size - 1) / actual_cell_size;
    
    // Create a change map to track which cells have changes
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
                    
                    // Check neighbors (4-connected)
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

// PNG write callback for memory
static void png_write_data_callback(png_structp png_ptr, png_bytep data, png_size_t length) {
    PNGMemoryData *mem_data = (PNGMemoryData*)png_get_io_ptr(png_ptr);
    
    // Ensure we have enough space
    if (mem_data->offset + length > mem_data->allocated) {
        size_t new_size = mem_data->allocated * 2;
        if (new_size < mem_data->offset + length) {
            new_size = mem_data->offset + length + 4096;
        }
        
        unsigned char *new_data = realloc(mem_data->data, new_size);
        if (!new_data) {
            png_error(png_ptr, "Failed to allocate memory for PNG");
            return;
        }
        
        mem_data->data = new_data;
        mem_data->allocated = new_size;
    }
    
    memcpy(mem_data->data + mem_data->offset, data, length);
    mem_data->offset += length;
    mem_data->size = mem_data->offset;
}

static void png_flush_callback(png_structp png_ptr) {
    // No-op for memory writing
    (void)png_ptr;
}

// Encode raw RGB24 buffer to PNG (fast settings)
static int encode_rgb_to_png(const unsigned char *rgb,
                             int width,
                             int height,
                             unsigned char **png_data,
                             size_t *png_size) {
    if (!rgb || width <= 0 || height <= 0 || !png_data || !png_size) return -1;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) return -1;

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return -1;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return -1;
    }

    PNGMemoryData mem_data = (PNGMemoryData){0};
    mem_data.allocated = (size_t)width * (size_t)height * 2;
    mem_data.data = malloc(mem_data.allocated);
    if (!mem_data.data) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return -1;
    }

    png_set_write_fn(png_ptr, &mem_data, png_write_data_callback, png_flush_callback);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png_ptr, 1);
    png_set_filter(png_ptr, 0, PNG_FILTER_NONE);
    png_write_info(png_ptr, info_ptr);

    png_bytep *row_pointers = malloc(sizeof(png_bytep) * (size_t)height);
    if (!row_pointers) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(mem_data.data);
        return -1;
    }
    for (int y = 0; y < height; y++) {
        row_pointers[y] = (png_bytep)(rgb + (size_t)y * (size_t)width * 3);
    }
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);
    free(row_pointers);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    *png_data = mem_data.data;
    *png_size = mem_data.size;
    return 0;
}

// Fallback conversion for uncommon XImage formats
static int encode_ximage_fallback_png(XImage *img, unsigned char **png_data, 
                                     size_t *png_size) {
    printf("Using fallback XImage conversion for PNG: depth=%d, bpp=%d\n", 
           img->depth, img->bits_per_pixel);
           
    size_t rgb_size = img->width * img->height * 3;
    unsigned char *rgb_buffer = malloc(rgb_size);
    if (!rgb_buffer) return -1;
    
    // Pixel-by-pixel conversion using XGetPixel (slower but reliable)
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            int idx = (y * img->width + x) * 3;
            
            // Extract RGB components based on masks
            rgb_buffer[idx]     = (pixel & img->red_mask) >> 16;   // R
            rgb_buffer[idx + 1] = (pixel & img->green_mask) >> 8;  // G  
            rgb_buffer[idx + 2] = (pixel & img->blue_mask);        // B
        }
    }
    
    // Encode RGB to PNG
    int result = encode_rgb_to_png(rgb_buffer, img->width, img->height, png_data, png_size);
    free(rgb_buffer);
    
    return result;
}

// Direct XImage to PNG conversion
static int encode_ximage_to_png(XImage *img, unsigned char **png_data, 
                               size_t *png_size) {
    int width = img->width;
    int height = img->height;
    
    // Debug the XImage format for first frame
    static int first_call = 1;
    if (first_call) {
        printf("XImage format debug for PNG:\n");
        printf("  Depth: %d, BPP: %d, Byte order: %s\n", 
               img->depth, img->bits_per_pixel, 
               img->byte_order == LSBFirst ? "LSBFirst" : "MSBFirst");
        printf("  Red mask: 0x%lx, Green mask: 0x%lx, Blue mask: 0x%lx\n",
               img->red_mask, img->green_mask, img->blue_mask);
        first_call = 0;
    }
    
    // For specific format (24-bit depth, 32 BPP, LSBFirst)
    if (img->depth == 24 && img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
        // This is BGRX format (Blue, Green, Red, X padding)
        if (img->red_mask == 0xff0000 && img->green_mask == 0xff00 && img->blue_mask == 0xff) {
            // Standard BGRX format - convert to RGB for PNG
            
            // Allocate RGB buffer
            size_t rgb_size = width * height * 3;
            unsigned char *rgb_buffer = malloc(rgb_size);
            if (!rgb_buffer) return -1;
            
            // Convert BGRX to RGB
            unsigned char *src = (unsigned char*)img->data;
            for (int y = 0; y < height; y++) {
                unsigned char *line_src = src + (y * img->bytes_per_line);
                unsigned char *line_dst = rgb_buffer + (y * width * 3);
                
                for (int x = 0; x < width; x++) {
                    unsigned char b = line_src[x * 4 + 0];
                    unsigned char g = line_src[x * 4 + 1];
                    unsigned char r = line_src[x * 4 + 2];
                    
                    line_dst[x * 3 + 0] = r;
                    line_dst[x * 3 + 1] = g;
                    line_dst[x * 3 + 2] = b;
                }
            }
            
            // Encode to PNG
            int result = encode_rgb_to_png(rgb_buffer, width, height, png_data, png_size);
            free(rgb_buffer);
            return result;
            
        } else {
            // Non-standard masks - use pixel-by-pixel conversion
            return encode_ximage_fallback_png(img, png_data, png_size);
        }
    } else {
        // Other formats - use fallback
        return encode_ximage_fallback_png(img, png_data, png_size);
    }
}

// Initialize frame streamer 
FrameStreamer* frame_streamer_init(UDPServer *udp_server, const char *output_name, int fps) {
    if (!udp_server || !output_name) return NULL;
    
    FrameStreamer *streamer = calloc(1, sizeof(FrameStreamer));
    if (!streamer) return NULL;
    
    streamer->udp_server = udp_server;
    
    // Initialize frame capture
    streamer->frame_capture = fc_init(udp_server->dm, output_name, fps);
    if (!streamer->frame_capture) {
        fprintf(stderr, "Failed to initialize frame capture for '%s'\n", output_name);
        free(streamer);
        return NULL;
    }
    
    // Initialize delta scaffolding fields
    streamer->reference_frame_rgb = NULL;
    streamer->reference_size = 0;
    streamer->reference_width = 0;
    streamer->reference_height = 0;
    // Toggle via environment variable TABC_DELTA=1
    const char *delta_env = getenv("TABC_DELTA");
    streamer->delta_mode_enabled = (delta_env && strcmp(delta_env, "1") == 0);

    // Tunables
    streamer->diff_threshold = 30;
    streamer->cover_threshold_pct = 80;
    streamer->keyframe_interval = 120;
    streamer->keyframe_interval_sec = 3; 
    streamer->region_padding = 8; 
    streamer->min_region_size = 32; 
    streamer->max_regions_per_frame = 8; 
    streamer->region_cell_size = 32; 
    const char *th_env = getenv("TABC_THRESH");
    const char *cov_env = getenv("TABC_COVER");
    const char *key_env = getenv("TABC_KEYINT");
    const char *keysec_env = getenv("TABC_KEYSEC");
    const char *pad_env = getenv("TABC_PADDING");
    const char *minsize_env = getenv("TABC_MINSIZE");
    const char *maxregions_env = getenv("TABC_MAXREGIONS");
    const char *cellsize_env = getenv("TABC_CELLSIZE");
    if (th_env) streamer->diff_threshold = atoi(th_env);
    if (cov_env) streamer->cover_threshold_pct = atoi(cov_env);
    if (key_env) streamer->keyframe_interval = atoi(key_env);
    if (keysec_env) streamer->keyframe_interval_sec = atoi(keysec_env);
    if (pad_env) streamer->region_padding = atoi(pad_env);
    if (minsize_env) streamer->min_region_size = atoi(minsize_env);
    if (maxregions_env) streamer->max_regions_per_frame = atoi(maxregions_env);
    if (cellsize_env) streamer->region_cell_size = atoi(cellsize_env);
    
    // Cap max_regions_per_frame to prevent stack overflow
    if (streamer->max_regions_per_frame > MAX_DELTA_OPERATIONS / 2) {
        streamer->max_regions_per_frame = MAX_DELTA_OPERATIONS / 2;
        printf("Warning: max_regions_per_frame capped at %d (MAX_DELTA_OPERATIONS/2)\n", 
               streamer->max_regions_per_frame);
    }

    streamer->captures_since_keyframe = 0;
    
    // Initialize time-based keyframe tracking
    gettimeofday(&streamer->last_keyframe_time, NULL);
    
    // Initialize inactivity detection
    gettimeofday(&streamer->last_activity_time, NULL);
    streamer->inactivity_threshold_sec = 5; // Force keyframe after 5 seconds of inactivity
    
    // Initialize reference frame validation
    streamer->reference_frame_checksum = 0;
    streamer->last_sent_frame_id = 0;

    printf("Optimized frame streamer initialized for '%s' | delta %s | thresh=%d cover=%d%% keyint=%d keysec=%d pad=%d minsize=%d maxregions=%d cellsize=%d\n",
           output_name,
           streamer->delta_mode_enabled ? "ENABLED" : "DISABLED",
           streamer->diff_threshold,
           streamer->cover_threshold_pct,
           streamer->keyframe_interval,
           streamer->keyframe_interval_sec,
           streamer->region_padding,
           streamer->min_region_size,
           streamer->max_regions_per_frame,
           streamer->region_cell_size);
    return streamer;
}

// Wait for START_STREAM command from client
int frame_streamer_wait_for_start_command(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    char buffer[UDP_BUFFER_SIZE];
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    printf("Waiting for START_STREAM command...\n");
    
    ssize_t bytes_received = recvfrom(server->socket_fd, buffer, sizeof(buffer) - 1, 0,
                                     (struct sockaddr*)&client->address,
                                     &client->address_len);
    
    if (bytes_received < 0) {
        perror("Failed to receive start command");
        return -1;
    }
    
    buffer[bytes_received] = '\0';
    printf("Received: %s\n", buffer);
    
    if (strcmp(buffer, "START_STREAM") == 0) {
        client->state = CLIENT_STATE_STREAMING;
        
        if (udp_server_send_response(server, "STREAM_STARTED") != 0) {
            return -1;
        }
        
        printf("Streaming started by client request\n");
        return 0;
    } else {
        printf("Unexpected command: %s\n", buffer);
        return -1;
    }
}

// Handle keyframe request from client
int frame_streamer_handle_keyframe_request(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    char buffer[UDP_BUFFER_SIZE];
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    // Non-blocking check for keyframe request
    fd_set readfds;
    struct timeval timeout = {0, 0}; // No wait
    
    FD_ZERO(&readfds);
    FD_SET(server->socket_fd, &readfds);
    
    int result = select(server->socket_fd + 1, &readfds, NULL, NULL, &timeout);
    if (result > 0 && FD_ISSET(server->socket_fd, &readfds)) {
        ssize_t bytes_received = recvfrom(server->socket_fd, buffer, sizeof(buffer) - 1, 0,
                                         (struct sockaddr*)&client->address,
                                         &client->address_len);
        
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            if (strcmp(buffer, "REQUEST_KEYFRAME") == 0) {
                printf("Client requested keyframe - forcing next frame to be full frame\n");
                // Force next frame to be a keyframe by resetting capture counter
                streamer->captures_since_keyframe = streamer->keyframe_interval;
                return 1; // Indicate keyframe was requested
            }
        }
    }
    
    return 0; // No keyframe request
}

// Send frame info to client - indicates PNG format
int frame_streamer_send_frame_info(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    ClientInfo *client = udp_server_get_client(streamer->udp_server);
    UDPServer *server = streamer->udp_server;
    
    // Include format and delta capability information
    // Format: INFO:<w>:<h>:PNG[:DELTA]
    char info_packet[96];
    if (streamer->delta_mode_enabled) {
        snprintf(info_packet, sizeof(info_packet), "INFO:%d:%d:PNG:DELTA", 
                 client->width, client->height);
    } else {
        snprintf(info_packet, sizeof(info_packet), "INFO:%d:%d:PNG", 
                 client->width, client->height);
    }
    
    if (udp_server_send_response(server, info_packet) != 0) {
        return -1;
    }
    
    printf("Sent frame info: %dx%d PNG%s\n", client->width, client->height,
           streamer->delta_mode_enabled ? "+DELTA" : "");
    
    // Small delay to ensure client receives frame info before data packets
    usleep(10000); // 10ms delay
    
    return 0;
}

// Frame sending logic with proper synchronization
int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame) {
    if (!streamer || !frame) return -1;
    
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    // Debug output
    static int debug_frame_count = 0;
    static int total_debug_frames = 5;
    bool debug_output = (debug_frame_count < total_debug_frames);
    
    // Allocate/update reference frame on first frame
    if (streamer->reference_frame_rgb == NULL ||
        streamer->reference_width != (unsigned int)frame->width ||
        streamer->reference_height != (unsigned int)frame->height) {
        
        free(streamer->reference_frame_rgb);
        streamer->reference_width = frame->width;
        streamer->reference_height = frame->height;
        streamer->reference_size = (size_t)frame->width * (size_t)frame->height * 3;
        streamer->reference_frame_rgb = (unsigned char*)malloc(streamer->reference_size);
        
        if (!streamer->reference_frame_rgb) {
            fprintf(stderr, "Failed to allocate reference frame buffer\n");
            return -1;
        }
        
        if (debug_output) {
            printf("  Allocated reference frame buffer: %zu bytes\n", streamer->reference_size);
        }
        
        // Send first frame as keyframe (no delta comparison possible)
        goto send_full_frame;
    }

    // Delta mode logic - Try to create delta frame with multiple operations
    if (streamer->delta_mode_enabled && streamer->frame_id > 0 &&
        frame->bits_per_pixel == 32 && frame->byte_order == LSBFirst) {
        
        // Check force keyframe conditions first
        bool force_keyframe = (streamer->captures_since_keyframe >= streamer->keyframe_interval);
        
        struct timeval now;
        gettimeofday(&now, NULL);
        
        // Check time-based keyframe interval
        long time_since_keyframe = (now.tv_sec - streamer->last_keyframe_time.tv_sec) * 1000000 +
                                  (now.tv_usec - streamer->last_keyframe_time.tv_usec);
        bool time_keyframe = (time_since_keyframe > streamer->keyframe_interval_sec * 1000000);
        
        long time_since_activity = (now.tv_sec - streamer->last_activity_time.tv_sec) * 1000000 +
                                  (now.tv_usec - streamer->last_activity_time.tv_usec);
        bool inactivity_keyframe = (time_since_activity > streamer->inactivity_threshold_sec * 1000000);
        
        if (debug_output) {
            printf("  Force keyframe: %s, Time keyframe: %s, Inactivity keyframe: %s\n", 
                   force_keyframe ? "YES" : "NO", time_keyframe ? "YES" : "NO", inactivity_keyframe ? "YES" : "NO");
        }
        
        // Try to create delta frame if not forcing keyframe
        if (!force_keyframe && !time_keyframe && !inactivity_keyframe) {
            DeltaFrame delta_frame;
            int delta_result = frame_streamer_create_delta_frame(streamer, frame, &delta_frame);
            
            if (delta_result == 0) {
                // Successfully created delta frame - send it
                if (debug_output) {
                    printf("  Sending delta frame with %d operations\n", delta_frame.operation_count);
                }
                
                int send_result = frame_streamer_send_delta_frame(streamer, &delta_frame);
                frame_streamer_cleanup_delta_frame(&delta_frame);
                
                if (send_result == 0) {
                    // Update reference frame with current frame data
                    convert_frame_bgrx_to_rgb(frame, streamer->reference_frame_rgb);
                    
                    // Update counters and timestamps
                    streamer->captures_since_keyframe++;
                    gettimeofday(&streamer->last_activity_time, NULL);
                    streamer->reference_frame_checksum = compute_checksum(streamer->reference_frame_rgb, streamer->reference_size);
                    streamer->last_sent_frame_id = streamer->frame_id;
                    
                    if (debug_output) {
                        printf("  Delta frame sent successfully\n");
                    }
                    
                    return 0; // Successfully sent delta
                } else {
                    fprintf(stderr, "Failed to send delta frame\n");
                    return -1;
                }
            } else if (delta_result == 1) {
                // No changes detected or coverage too high - skip sending
                if (debug_output) {
                    printf("  No changes detected or coverage too high - skipping send\n");
                }
                return 1; // Skip send (no changes)
            } else {
                // Error creating delta frame
                fprintf(stderr, "Failed to create delta frame\n");
                return -1;
            }
        }
    }

send_full_frame:
    // Send full frame (keyframe)
    if (debug_output) {
        printf("  Sending full keyframe\n");
    }
    
    // Convert frame to RGB FIRST, then encode
    // This ensures reference frame matches exactly what we encode
    size_t full_rgb_size = (size_t)frame->width * (size_t)frame->height * 3;
    unsigned char *full_rgb = (unsigned char*)malloc(full_rgb_size);
    if (!full_rgb) {
        fprintf(stderr, "Failed to allocate RGB buffer for full frame\n");
        return -1;
    }
    
    // Convert BGRX to RGB
    convert_frame_bgrx_to_rgb(frame, full_rgb);
    
    // Encode the RGB buffer to PNG
    unsigned char *png_data = NULL;
    size_t png_size = 0;
    
    if (encode_rgb_to_png(full_rgb, frame->width, frame->height, &png_data, &png_size) != 0) {
        fprintf(stderr, "Failed to encode frame %d to PNG\n", streamer->frame_id);
        free(full_rgb);
        return -1;
    }
    
    // Send full frame in packets
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (png_size + data_per_packet - 1) / data_per_packet;
    
    if (streamer->frame_id < 3 || debug_output) {
        size_t estimated_rgb_size = frame->width * frame->height * 3;
        float compression_ratio = (float)estimated_rgb_size / png_size;
        printf("Frame %d: %dx%d -> PNG(%zu bytes) = %.1fx compression, %zu packets\n",
               streamer->frame_id, frame->width, frame->height, 
               png_size, compression_ratio, total_packets);
    }
    
    bool send_failed = false;
    for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
        size_t remaining = png_size - (packet_id * data_per_packet);
        size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
        
        char packet[MAX_PACKET_SIZE];
        PacketHeader *header = (PacketHeader*)packet;
        
        header->frame_id = htonl(streamer->frame_id);
        header->packet_id = htonl(packet_id);
        header->total_packets = htonl(total_packets);
        header->data_size = htonl(current_data_size);
        
        memcpy(packet + sizeof(PacketHeader), 
               png_data + (packet_id * data_per_packet), 
               current_data_size);
        
        ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
        ssize_t bytes_sent = sendto(server->socket_fd, packet, packet_size, 0,
                                   (struct sockaddr*)&client->address, 
                                   client->address_len);
        
        if (bytes_sent < 0) {
            fprintf(stderr, "Failed to send packet %zu/%zu: %s\n", 
                    packet_id + 1, total_packets, strerror(errno));
            send_failed = true;
            break;
        }
        
        if (total_packets > 100) {
            usleep(5);
        }
    }
    
    free(png_data);
    
    // Only update reference frame if send was successful
    if (!send_failed) {
        // Update reference frame with the EXACT RGB data we just encoded and sent
        memcpy(streamer->reference_frame_rgb, full_rgb, full_rgb_size);
        
        // Reset keyframe counter and update timestamps
        streamer->captures_since_keyframe = 0;
        gettimeofday(&streamer->last_keyframe_time, NULL);
        gettimeofday(&streamer->last_activity_time, NULL);
        streamer->reference_frame_checksum = compute_checksum(streamer->reference_frame_rgb, streamer->reference_size);
        streamer->last_sent_frame_id = streamer->frame_id;
        
        if (debug_output) {
            printf("  Full keyframe sent successfully\n");
            debug_frame_count++;
        }
    }
    
    free(full_rgb);
    
    if (send_failed) {
        return -1;
    }
    
    return 0;
}

// main streaming loop
int frame_streamer_run_loop(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    
    printf("Starting optimized PNG streaming loop... Press Ctrl+C to stop\n");
    frame_streamer_print_status(streamer);
    
    // Start frame capture
    if (fc_start(streamer->frame_capture) != 0) {
        fprintf(stderr, "Failed to start frame capture\n");
        return -1;
    }
    
    streamer->streaming = true;
    
    while (keep_streaming && streamer->streaming) {
        // Check for keyframe requests from client
        frame_streamer_handle_keyframe_request(streamer);
        
        int result = fc_capture_frame(streamer->frame_capture);
        
        if (result == 1) {  // New frame captured
            XImage *frame = fc_get_frame(streamer->frame_capture);
            if (frame) {
                int send_result = frame_streamer_send_frame(streamer, frame);
                if (send_result == 0) {
                    streamer->frames_sent++;
                    streamer->frame_id++;
                    streamer->captures_since_keyframe = 0;
                    
                    if (streamer->frames_sent % 60 == 0) {
                        printf("Sent %d PNG frames\n", streamer->frames_sent);
                    }
                } else if (send_result < 0) {
                    fprintf(stderr, "Failed to send frame %d\n", streamer->frame_id);
                } else {
                    // Skipped send: increment cadence counter
                    streamer->captures_since_keyframe++;
                }
                
                fc_mark_frame_processed(streamer->frame_capture);
            }
        } else if (result < 0) {
            fprintf(stderr, "Capture failed\n");
            break;
        }
        
        // Minimal sleep - let frame capture handle most timing
        usleep(500); // Reduced from 5000μs to 500μs
    }
    
    printf("\nStreamed %d PNG frames total\n", streamer->frames_sent);
    return 0;
}

// Start streaming
int frame_streamer_start(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    // STEP 1: Wait for client to request streaming
    if (frame_streamer_wait_for_start_command(streamer) != 0) {
        return -1;
    }
    
    // STEP 2: Send frame info (now includes PNG format)
    if (frame_streamer_send_frame_info(streamer) != 0) {
        return -1;
    }
    
    // STEP 3: Run streaming loop
    return frame_streamer_run_loop(streamer);
}

// Print streamer status with PNG info
void frame_streamer_print_status(FrameStreamer *streamer) {
    if (!streamer) return;
    
    printf("Optimized Frame Streamer Status (Direct PNG):\n");
    printf("  Streaming: %s\n", streamer->streaming ? "YES" : "NO");
    printf("  Frames sent: %d\n", streamer->frames_sent);
    printf("  Current frame ID: %d\n", streamer->frame_id);
    printf("  Mode: Direct XImage->PNG (lossless, fast decode)\n");
    
    if (streamer->frame_capture) {
        fc_print_frame_info(streamer->frame_capture);
    }
}

// Cleanup display configuration (disable output, remove mode, delete mode)
static int cleanup_display_config(FrameStreamer *streamer) {
    if (!streamer || !streamer->udp_server) {
        return -1;
    }
    
    // Get display configuration from UDP server
    RRMode mode_id = udp_server_get_created_mode_id(streamer->udp_server);
    const char* output_name = udp_server_get_output_name(streamer->udp_server);
    DisplayManager *dm = streamer->udp_server->dm;
    
    if (mode_id == 0 || !output_name || !dm) {
        printf("No display cleanup needed (no mode was created)\n");
        return 0;
    }
    
    printf("\n=== Cleaning up stream display '%s' ===\n", output_name);
    
    // STEP 1: Disable output
    printf("STEP 1: Disabling output '%s'...\n", output_name);
    if (mode_disable_output(dm, output_name) != 0) {
        fprintf(stderr, "Warning: Failed to disable output '%s'\n", output_name);
    } else {
        printf(" Output disabled successfully\n");
    }
    
    // STEP 2: Remove mode from output
    printf("STEP 2: Removing mode %lu from output '%s'...\n", mode_id, output_name);
    if (mode_remove_from_output(dm, output_name, mode_id) != 0) {
        fprintf(stderr, "Warning: Failed to remove mode from output '%s'\n", output_name);
    } else {
        printf(" Mode removed from output successfully\n");
    }
    
    // STEP 3: Delete mode from XRandR
    printf("STEP 3: Deleting mode %lu from XRandR...\n", mode_id);
    if (mode_delete_from_xrandr(dm, mode_id) != 0) {
        fprintf(stderr, "Warning: Failed to delete mode from XRandR\n");
    } else {
        printf(" Mode deleted from XRandR successfully\n");
    }
    
    printf(" Stream display cleanup completed\n");
    return 0;
}

// Stop streaming
void frame_streamer_stop(FrameStreamer *streamer) {
    if (!streamer) return;
    
    streamer->streaming = false;
    if (streamer->frame_capture) {
        fc_stop(streamer->frame_capture);
    }
}

// Cleanup resources
void frame_streamer_cleanup(FrameStreamer *streamer) {
    if (!streamer) return;
    
    // Stop streaming first
    frame_streamer_stop(streamer);
    
    // Cleanup display configuration
    cleanup_display_config(streamer);
    
    // Cleanup frame capture
    if (streamer->frame_capture) {
        fc_cleanup(streamer->frame_capture);
    }
    // Free delta scaffolding buffers
    free(streamer->reference_frame_rgb);
    streamer->reference_frame_rgb = NULL;
    
    free(streamer);
}

// Create a delta frame with multiple operations (CLEAR + DRAW pattern)
int frame_streamer_create_delta_frame(FrameStreamer *streamer, XImage *frame, DeltaFrame *delta_frame) {
    if (!streamer || !frame || !delta_frame) return -1;
    
    // Initialize delta frame
    memset(delta_frame, 0, sizeof(DeltaFrame));
    delta_frame->frame_id = streamer->frame_id;
    delta_frame->operation_count = 0;
    delta_frame->total_payload_size = 0;
    
    // Only proceed if delta mode is enabled and we have a reference frame
    if (!streamer->delta_mode_enabled || !streamer->reference_frame_rgb ||
        streamer->reference_width != (unsigned int)frame->width ||
        streamer->reference_height != (unsigned int)frame->height) {
        return -1;
    }
    
    // Detect multiple change regions
    // Limit regions to half of max operations (each region needs 2 operations: CLEAR + DRAW)
    int max_regions = (MAX_DELTA_OPERATIONS / 2 < streamer->max_regions_per_frame) ? 
                      (MAX_DELTA_OPERATIONS / 2) : streamer->max_regions_per_frame;
    
    ChangeRegion *regions = (ChangeRegion*)malloc(max_regions * sizeof(ChangeRegion));
    if (!regions) {
        fprintf(stderr, "Failed to allocate regions array\n");
        return -1;
    }
    
    int region_count = 0;
    
    int changed = detect_multiple_regions(streamer->reference_frame_rgb,
                                        (const unsigned char*)frame->data,
                                        frame->width, frame->height,
                                        frame->bytes_per_line,
                                        streamer->diff_threshold,
                                        max_regions,
                                        streamer->region_cell_size,
                                        regions, &region_count);
    
    if (!changed || region_count == 0) {
        free(regions);
        return 1; // No changes detected
    }
    
    // Check total coverage threshold
    size_t total_pixels = (size_t)frame->width * (size_t)frame->height;
    int total_changed_pixels = 0;
    for (int i = 0; i < region_count; i++) {
        total_changed_pixels += regions[i].changed_pixels;
    }
    double coverage_pct = (double)total_changed_pixels / (double)total_pixels * 100.0;
    
    if (coverage_pct > streamer->cover_threshold_pct) {
        free(regions);
        return 1; // Coverage too high, should send keyframe instead
    }
    
    // Process each detected region
    for (int r = 0; r < region_count && delta_frame->operation_count < MAX_DELTA_OPERATIONS - 1; r++) {
        int rx = regions[r].x;
        int ry = regions[r].y;
        int rw = regions[r].width;
        int rh = regions[r].height;
        
        // Apply region constraints (padding and minimum size)
        apply_region_constraints(&rx, &ry, &rw, &rh, 
                               frame->width, frame->height,
                               streamer->region_padding, streamer->min_region_size);
        
        // Create CLEAR operation (CREG) - restore region to base frame state
        DeltaOperation *clear_op = &delta_frame->operations[delta_frame->operation_count];
        clear_op->type = OP_CREG;
        clear_op->x = rx;
        clear_op->y = ry;
        clear_op->width = rw;
        clear_op->height = rh;
        
        // Extract the region from reference frame (what client currently has)
        size_t region_pixels = (size_t)rw * (size_t)rh;
        size_t region_rgb_size = region_pixels * 3;
        unsigned char *region_rgb = (unsigned char*)malloc(region_rgb_size);
        if (!region_rgb) {
            fprintf(stderr, "Failed to allocate region RGB buffer for CREG\n");
            frame_streamer_cleanup_delta_frame(delta_frame);
            free(regions);
            return -1;
        }
        
        // Extract region from reference frame (RGB format)
        for (int y = 0; y < rh; y++) {
            unsigned char *ref_line = streamer->reference_frame_rgb + 
                                     ((size_t)(ry + y) * (size_t)frame->width + (size_t)rx) * 3;
            unsigned char *region_line = region_rgb + (size_t)y * (size_t)rw * 3;
            memcpy(region_line, ref_line, (size_t)rw * 3);
        }
        
        // Encode reference region to PNG
        if (encode_rgb_to_png(region_rgb, rw, rh, &clear_op->png_data, &clear_op->png_size) != 0) {
            fprintf(stderr, "Failed to encode CREG region to PNG\n");
            free(region_rgb);
            frame_streamer_cleanup_delta_frame(delta_frame);
            free(regions);
            return -1;
        }
        
        free(region_rgb);
        delta_frame->operation_count++;
        delta_frame->total_payload_size += 4 + 2+2+2+2 + 1+1+2 + clear_op->png_size; // Header + PNG
        
        // Create DRAW operation (DREG) - apply new content
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
            frame_streamer_cleanup_delta_frame(delta_frame);
            free(regions);
            return -1;
        }
        
        for (int y = 0; y < rh; y++) {
            const unsigned char *line_src = (unsigned char*)frame->data + 
                                           (size_t)(ry + y) * (size_t)frame->bytes_per_line;
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
        if (encode_rgb_to_png(current_region_rgb, rw, rh, &draw_op->png_data, &draw_op->png_size) != 0) {
            fprintf(stderr, "Failed to encode DREG region to PNG\n");
            free(current_region_rgb);
            frame_streamer_cleanup_delta_frame(delta_frame);
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

// Send a delta frame with multiple operations
int frame_streamer_send_delta_frame(FrameStreamer *streamer, DeltaFrame *delta_frame) {
    if (!streamer || !delta_frame || delta_frame->operation_count == 0) return -1;
    
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    // Build combined payload with all operations
    unsigned char *payload = (unsigned char*)malloc(delta_frame->total_payload_size);
    if (!payload) {
        fprintf(stderr, "Failed to allocate payload buffer\n");
        return -1;
    }
    
    size_t offset = 0;
    
    // Add each operation to the payload
    for (int i = 0; i < delta_frame->operation_count; i++) {
        DeltaOperation *op = &delta_frame->operations[i];
        
        // Add operation header
        const char *magic = (op->type == OP_CREG) ? "CREG" : "DREG";
        memcpy(payload + offset, magic, 4); offset += 4;
        
        uint16_t nx = htons(op->x);
        uint16_t ny = htons(op->y);
        uint16_t nw = htons(op->width);
        uint16_t nh = htons(op->height);
        memcpy(payload + offset, &nx, 2); offset += 2;
        memcpy(payload + offset, &ny, 2); offset += 2;
        memcpy(payload + offset, &nw, 2); offset += 2;
        memcpy(payload + offset, &nh, 2); offset += 2;
        payload[offset++] = 0; // flags
        payload[offset++] = 90; // quality hint
        uint16_t nres = htons(0);
        memcpy(payload + offset, &nres, 2); offset += 2;
        
        // Add PNG data
        memcpy(payload + offset, op->png_data, op->png_size);
        offset += op->png_size;
    }
    
    // Send payload in packets
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (delta_frame->total_payload_size + data_per_packet - 1) / data_per_packet;
    
    bool send_failed = false;
    for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
        size_t remaining = delta_frame->total_payload_size - (packet_id * data_per_packet);
        size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
        
        char packet[MAX_PACKET_SIZE];
        PacketHeader *header = (PacketHeader*)packet;
        header->frame_id = htonl(delta_frame->frame_id);
        header->packet_id = htonl(packet_id);
        header->total_packets = htonl(total_packets);
        header->data_size = htonl(current_data_size);
        
        memcpy(packet + sizeof(PacketHeader), payload + (packet_id * data_per_packet), current_data_size);
        
        ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
        ssize_t bytes_sent = sendto(server->socket_fd, packet, packet_size, 0,
                                   (struct sockaddr*)&client->address,
                                   client->address_len);
        
        if (bytes_sent < 0) {
            fprintf(stderr, "Failed to send delta frame packet %zu/%zu: %s\n", 
                    packet_id + 1, total_packets, strerror(errno));
            send_failed = true;
            break;
        }
    }
    
    free(payload);
    
    if (send_failed) {
        return -1;
    }
    
    printf("Sent delta frame with %d operations in %zu packets\n", 
           delta_frame->operation_count, total_packets);
    
    return 0;
}

// Cleanup delta frame resources
void frame_streamer_cleanup_delta_frame(DeltaFrame *delta_frame) {
    if (!delta_frame) return;
    
    for (int i = 0; i < delta_frame->operation_count; i++) {
        if (delta_frame->operations[i].png_data) {
            free(delta_frame->operations[i].png_data);
            delta_frame->operations[i].png_data = NULL;
        }
    }
    
    memset(delta_frame, 0, sizeof(DeltaFrame));
}