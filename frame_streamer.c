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

// Helper function to update reference frame from BGRX to RGB
static void update_reference_frame_from_bgrx(FrameStreamer *streamer, XImage *frame) {
    if (!streamer || !frame || !streamer->reference_frame_rgb) return;
    
    unsigned char *src = (unsigned char*)frame->data;
    for (int y = 0; y < frame->height; y++) {
        unsigned char *line_src = src + (y * frame->bytes_per_line);
        unsigned char *line_dst = streamer->reference_frame_rgb + (size_t)y * (size_t)frame->width * 3;
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

//  absolute difference-based region detection with pixel counting and debug output
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
    //  validation for edge cases
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
    
    //  pixel-by-pixel comparison with better difference calculation
    for (int y = 0; y < height; y++) {
        const unsigned char *curr_line = curr_bgrx + y * bytes_per_line;
        const unsigned char *prev_line = prev_rgb + y * (width * 3);
        
        for (int x = 0; x < width; x++) {
            // Extract BGRX components from current frame
            unsigned char b = curr_line[x * 4 + 0];
            unsigned char g = curr_line[x * 4 + 1];
            unsigned char r = curr_line[x * 4 + 2];
            // Skip X (padding byte at curr_line[x * 4 + 3])
            
            // Extract RGB components from previous frame
            int prev_idx = x * 3;
            unsigned char prev_r = prev_line[prev_idx + 0];
            unsigned char prev_g = prev_line[prev_idx + 1];
            unsigned char prev_b = prev_line[prev_idx + 2];
            
            //  absolute difference calculation
            // Use proper signed arithmetic to avoid underflow issues
            int dr = (int)r - (int)prev_r;
            int dg = (int)g - (int)prev_g;
            int db = (int)b - (int)prev_b;
            
            // Calculate absolute differences (avoiding abs() for performance)
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
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        free(rgb_buffer);
        return -1;
    }
    
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        free(rgb_buffer);
        return -1;
    }
    
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(rgb_buffer);
        return -1;
    }
    
    // Set up memory writing
    PNGMemoryData mem_data = {0};
    mem_data.allocated = img->width * img->height * 4; // Initial allocation
    mem_data.data = malloc(mem_data.allocated);
    if (!mem_data.data) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(rgb_buffer);
        return -1;
    }
    
    png_set_write_fn(png_ptr, &mem_data, png_write_data_callback, png_flush_callback);
    
    // Set PNG parameters for speed
    png_set_IHDR(png_ptr, info_ptr, img->width, img->height, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    
    // Optimize for speed
    png_set_compression_level(png_ptr, 1); // Fastest compression
    png_set_filter(png_ptr, 0, PNG_FILTER_NONE); // No filtering for speed
    
    png_write_info(png_ptr, info_ptr);
    
    // Write image data row by row
    png_bytep *row_pointers = malloc(sizeof(png_bytep) * img->height);
    for (int y = 0; y < img->height; y++) {
        row_pointers[y] = rgb_buffer + (y * img->width * 3);
    }
    
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);
    
    // Cleanup
    free(row_pointers);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    free(rgb_buffer);
    
    *png_data = mem_data.data;
    *png_size = mem_data.size;
    
    return 0;
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
    
    // Set up memory writing
    PNGMemoryData mem_data = {0};
    mem_data.allocated = width * height * 4; // Initial allocation
    mem_data.data = malloc(mem_data.allocated);
    if (!mem_data.data) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return -1;
    }
    
    png_set_write_fn(png_ptr, &mem_data, png_write_data_callback, png_flush_callback);
    
    // For specific format (24-bit depth, 32 BPP, LSBFirst)
    if (img->depth == 24 && img->bits_per_pixel == 32 && img->byte_order == LSBFirst) {
        // This is BGRX format (Blue, Green, Red, X padding)
        if (img->red_mask == 0xff0000 && img->green_mask == 0xff00 && img->blue_mask == 0xff) {
            // Standard BGRX format - convert to RGB for PNG
            
            // Set PNG parameters for RGB
            png_set_IHDR(png_ptr, info_ptr, width, height, 8,
                         PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                         PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            
            // Optimize for speed
            png_set_compression_level(png_ptr, 1); // Fastest compression
            png_set_filter(png_ptr, 0, PNG_FILTER_NONE); // No filtering for speed
            
            png_write_info(png_ptr, info_ptr);
            
            // Convert and write data row by row
            unsigned char *row_buffer = malloc(width * 3);
            if (!row_buffer) {
                png_destroy_write_struct(&png_ptr, &info_ptr);
                free(mem_data.data);
                return -1;
            }
            
            unsigned char *src = (unsigned char*)img->data;
            
            for (int y = 0; y < height; y++) {
                unsigned char *line_src = src + (y * img->bytes_per_line);
                
                for (int x = 0; x < width; x++) {
                    // BGRX → RGB conversion
                    unsigned char b = line_src[x * 4 + 0];
                    unsigned char g = line_src[x * 4 + 1];
                    unsigned char r = line_src[x * 4 + 2];
                    // Skip X (padding byte at line_src[x * 4 + 3])
                    
                    row_buffer[x * 3 + 0] = r; // R
                    row_buffer[x * 3 + 1] = g; // G
                    row_buffer[x * 3 + 2] = b; // B
                }
                
                png_write_row(png_ptr, row_buffer);
            }
            
            free(row_buffer);
            
        } else {
            // Non-standard masks - use pixel-by-pixel conversion
            png_destroy_write_struct(&png_ptr, &info_ptr);
            free(mem_data.data);
            return encode_ximage_fallback_png(img, png_data, png_size);
        }
    } else {
        // Other formats - use fallback
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(mem_data.data);
        return encode_ximage_fallback_png(img, png_data, png_size);
    }
    
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    
    *png_data = mem_data.data;
    *png_size = mem_data.size;
    
    return 0;
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

    // Tunables with sensible defaults
    streamer->diff_threshold = 30;
    streamer->cover_threshold_pct = 80;
    streamer->keyframe_interval = 120;
    const char *th_env = getenv("TABC_THRESH");
    const char *cov_env = getenv("TABC_COVER");
    const char *key_env = getenv("TABC_KEYINT");
    if (th_env) streamer->diff_threshold = atoi(th_env);
    if (cov_env) streamer->cover_threshold_pct = atoi(cov_env);
    if (key_env) streamer->keyframe_interval = atoi(key_env);

    streamer->captures_since_keyframe = 0;
    
    // Initialize inactivity detection
    gettimeofday(&streamer->last_activity_time, NULL);
    streamer->inactivity_threshold_sec = 5; // Force keyframe after 5 seconds of inactivity
    
    // Initialize reference frame validation
    streamer->reference_frame_checksum = 0;
    streamer->last_sent_frame_id = 0;

    printf("Optimized frame streamer initialized for '%s' | delta %s | thresh=%d cover=%d%% keyint=%d\n",
           output_name,
           streamer->delta_mode_enabled ? "ENABLED" : "DISABLED",
           streamer->diff_threshold,
           streamer->cover_threshold_pct,
           streamer->keyframe_interval);
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

//  frame sending logic with improved ghosting prevention
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
        
        // Initialize reference frame for first frame
        update_reference_frame_from_bgrx(streamer, frame);
        
        if (debug_output) {
            printf("  Allocated and initialized reference frame buffer: %zu bytes\n", streamer->reference_size);
        }
        
        // Send first frame as keyframe (no delta comparison possible)
        goto send_full_frame;
    }

    // CRITICAL FIX: Store the CURRENT reference frame before updating it
    // This ensures we compare against the frame that was actually sent to client
    unsigned char *comparison_reference = NULL;
    if (streamer->delta_mode_enabled && streamer->frame_id > 0) {
        // Make a copy of current reference frame for comparison
        comparison_reference = (unsigned char*)malloc(streamer->reference_size);
        if (comparison_reference) {
            memcpy(comparison_reference, streamer->reference_frame_rgb, streamer->reference_size);
        }
    }

    // CRITICAL FIX: Update reference frame IMMEDIATELY after capture
    // This ensures reference frame always matches what's currently on screen
    update_reference_frame_from_bgrx(streamer, frame);

    // Delta mode logic with proper reference frame
    if (streamer->delta_mode_enabled && streamer->frame_id > 0 &&
        comparison_reference != NULL &&
        frame->bits_per_pixel == 32 && frame->byte_order == LSBFirst) {
        
        int rx = 0, ry = 0, rw = 0, rh = 0, changed_pixels = 0;
        
        // Compare current frame against the PREVIOUSLY SENT frame (comparison_reference)
        int changed = compute_changed_bounds_rgb24(comparison_reference,  // Previous sent frame
                                                   (const unsigned char*)frame->data,  // Current frame
                                                   frame->width,
                                                   frame->height,
                                                   frame->bytes_per_line,
                                                   streamer->diff_threshold,
                                                   &rx, &ry, &rw, &rh, &changed_pixels);
        
        free(comparison_reference); // Clean up temporary reference
        
        if (debug_output) {
            printf("  Frame %d: Change detection result: %s\n", streamer->frame_id, 
                   changed ? "CHANGES_FOUND" : "NO_CHANGES");
            if (changed) {
                printf("  Changed pixels: %d, Bounding box: (%d,%d) %dx%d\n", 
                       changed_pixels, rx, ry, rw, rh);
            }
        }
        
        if (changed) {
            size_t total_pixels = (size_t)frame->width * (size_t)frame->height;
            double actual_coverage = (double)changed_pixels / (double)total_pixels;
            
            // Force keyframe conditions
            bool force_keyframe = (streamer->captures_since_keyframe >= streamer->keyframe_interval);
            
            struct timeval now;
            gettimeofday(&now, NULL);
            long time_since_activity = (now.tv_sec - streamer->last_activity_time.tv_sec) * 1000000 +
                                      (now.tv_usec - streamer->last_activity_time.tv_usec);
            bool inactivity_keyframe = (time_since_activity > streamer->inactivity_threshold_sec * 1000000);
            
            if (debug_output) {
                printf("  Coverage: actual=%.2f%%, threshold=%d%%\n", 
                       actual_coverage * 100.0, streamer->cover_threshold_pct);
                printf("  Force keyframe: %s, Inactivity keyframe: %s\n", 
                       force_keyframe ? "YES" : "NO", inactivity_keyframe ? "YES" : "NO");
            }
            
            // Send delta region if coverage is acceptable
            if ((int)(actual_coverage * 100.0 + 0.5) <= streamer->cover_threshold_pct && 
                !force_keyframe && !inactivity_keyframe) {
                
                if (debug_output) {
                    printf("  Sending delta region: (%d,%d) %dx%d\n", rx, ry, rw, rh);
                }
                
                // Extract and send delta region
                size_t region_pixels = (size_t)rw * (size_t)rh;
                size_t region_rgb_size = region_pixels * 3;
                unsigned char *region_rgb = (unsigned char*)malloc(region_rgb_size);
                if (!region_rgb) {
                    fprintf(stderr, "Failed to allocate region RGB buffer\n");
                    return -1;
                }
                
                // Extract region from current frame
                for (int y = 0; y < rh; y++) {
                    const unsigned char *line_src = (unsigned char*)frame->data + (size_t)(ry + y) * (size_t)frame->bytes_per_line;
                    unsigned char *line_dst = region_rgb + (size_t)y * (size_t)rw * 3;
                    for (int x = 0; x < rw; x++) {
                        unsigned char b = line_src[(rx + x) * 4 + 0];
                        unsigned char g = line_src[(rx + x) * 4 + 1];
                        unsigned char r = line_src[(rx + x) * 4 + 2];
                        line_dst[x * 3 + 0] = r;
                        line_dst[x * 3 + 1] = g;
                        line_dst[x * 3 + 2] = b;
                    }
                }
                
                // Encode and send delta region
                unsigned char *region_png = NULL;
                size_t region_png_size = 0;
                if (encode_rgb_to_png(region_rgb, rw, rh, &region_png, &region_png_size) != 0) {
                    fprintf(stderr, "Failed to encode delta region to PNG\n");
                    free(region_rgb);
                    return -1;
                }
                
                // Create delta payload with header
                const char magic[4] = {'D','R','E','G'};
                size_t header_size = 4 + 2+2+2+2 + 1 + 1 + 2;
                size_t payload_size = header_size + region_png_size;
                unsigned char *payload = (unsigned char*)malloc(payload_size);
                if (!payload) {
                    fprintf(stderr, "Failed to allocate delta payload buffer\n");
                    free(region_rgb);
                    free(region_png);
                    return -1;
                }
                
                // Build header
                size_t off = 0;
                memcpy(payload + off, magic, 4); off += 4;
                uint16_t nx = htons((uint16_t)rx);
                uint16_t ny = htons((uint16_t)ry);
                uint16_t nw = htons((uint16_t)rw);
                uint16_t nh = htons((uint16_t)rh);
                memcpy(payload + off, &nx, 2); off += 2;
                memcpy(payload + off, &ny, 2); off += 2;
                memcpy(payload + off, &nw, 2); off += 2;
                memcpy(payload + off, &nh, 2); off += 2;
                payload[off++] = 0; // flags
                payload[off++] = 90; // quality hint
                uint16_t nres = htons(0);
                memcpy(payload + off, &nres, 2); off += 2;
                memcpy(payload + off, region_png, region_png_size);
                
                // Send delta payload in packets
                size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
                size_t total_packets = (payload_size + data_per_packet - 1) / data_per_packet;
                
                for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
                    size_t remaining = payload_size - (packet_id * data_per_packet);
                    size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
                    char packet[MAX_PACKET_SIZE];
                    PacketHeader *header = (PacketHeader*)packet;
                    header->frame_id = htonl(streamer->frame_id);
                    header->packet_id = htonl(packet_id);
                    header->total_packets = htonl(total_packets);
                    header->data_size = htonl(current_data_size);
                    memcpy(packet + sizeof(PacketHeader), payload + (packet_id * data_per_packet), current_data_size);
                    ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
                    ssize_t bytes_sent = sendto(server->socket_fd, packet, packet_size, 0,
                                                (struct sockaddr*)&client->address,
                                                client->address_len);
                    if (bytes_sent < 0) {
                        fprintf(stderr, "Failed to send delta packet %zu/%zu: %s\n", 
                                packet_id + 1, total_packets, strerror(errno));
                        free(region_rgb);
                        free(region_png);
                        free(payload);
                        return -1;
                    }
                }
                
                free(region_rgb);
                free(region_png);
                free(payload);
                
                // Update counters and timestamps
                streamer->captures_since_keyframe++;
                gettimeofday(&streamer->last_activity_time, NULL);
                streamer->reference_frame_checksum = compute_checksum(streamer->reference_frame_rgb, streamer->reference_size);
                streamer->last_sent_frame_id = streamer->frame_id;
                
                if (debug_output) {
                    printf("  Delta region sent successfully\n");
                }
                
                return 0; // Successfully sent delta
            }
        } else {
            // No changes detected - skip sending but reference frame is already updated
            if (debug_output) {
                printf("  No changes detected - skipping send\n");
            }
            return 1; // Skip send (no changes)
        }
    }

send_full_frame:
    // Send full frame (keyframe)
    if (debug_output) {
        printf("  Sending full keyframe\n");
    }
    
    unsigned char *png_data = NULL;
    size_t png_size = 0;
    
    if (encode_ximage_to_png(frame, &png_data, &png_size) != 0) {
        fprintf(stderr, "Failed to encode frame %d to PNG\n", streamer->frame_id);
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
            free(png_data);
            return -1;
        }
        
        if (total_packets > 100) {
            usleep(5);
        }
    }
    
    free(png_data);
    
    // Reset keyframe counter and update timestamps
    streamer->captures_since_keyframe = 0;
    gettimeofday(&streamer->last_activity_time, NULL);
    streamer->reference_frame_checksum = compute_checksum(streamer->reference_frame_rgb, streamer->reference_size);
    streamer->last_sent_frame_id = streamer->frame_id;
    
    if (debug_output) {
        printf("  Full keyframe sent successfully\n");
        debug_frame_count++;
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