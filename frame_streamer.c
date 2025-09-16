#include "frame_streamer.h"
#include "udp_server.h"
#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
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
    
    printf("Optimized frame streamer initialized for '%s' with direct PNG encoding\n", output_name);
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

// Send frame info to client - indicates PNG format
int frame_streamer_send_frame_info(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    ClientInfo *client = udp_server_get_client(streamer->udp_server);
    UDPServer *server = streamer->udp_server;
    
    // Include format information
    char info_packet[64];
    snprintf(info_packet, sizeof(info_packet), "INFO:%d:%d:PNG", 
             client->width, client->height);
    
    if (udp_server_send_response(server, info_packet) != 0) {
        return -1;
    }
    
    printf("Sent frame info: %dx%d PNG format\n", client->width, client->height);
    
    // Small delay to ensure client receives frame info before data packets
    usleep(10000); // 10ms delay
    
    return 0;
}

// Optimized frame sending with direct PNG encoding
int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame) {
    if (!streamer || !frame) return -1;
    
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    // Direct XImage to PNG conversion
    unsigned char *png_data = NULL;
    size_t png_size = 0;
    
    if (encode_ximage_to_png(frame, &png_data, &png_size) != 0) {
        fprintf(stderr, "Failed to encode frame %d to PNG\n", streamer->frame_id);
        return -1;
    }
    
    // Calculate packet info
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (png_size + data_per_packet - 1) / data_per_packet;
    
    // Debug info for first few frames only
    if (streamer->frame_id < 3) {
        size_t estimated_rgb_size = frame->width * frame->height * 3;
        float compression_ratio = (float)estimated_rgb_size / png_size;
        printf("Frame %d: %dx%d -> PNG(%zu bytes) = %.1fx compression, %zu packets\n",
               streamer->frame_id, frame->width, frame->height, 
               png_size, compression_ratio, total_packets);
    }
    
    // Send all packets with minimal delays
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
            perror("Failed to send packet");
            free(png_data);
            return -1;
        }
        
        // Minimal delay only for very large frames
        if (total_packets > 100) {
            usleep(5); // Reduced from 50μs to 5μs, only for huge frames
        }
    }
    
    free(png_data);
    
    // Less frequent progress updates
    if (streamer->frame_id % 60 == 0) {
        printf("Sent frame %d (%zu PNG bytes in %zu packets)\n", 
               streamer->frame_id, png_size, total_packets);
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
        int result = fc_capture_frame(streamer->frame_capture);
        
        if (result == 1) {  // New frame captured
            XImage *frame = fc_get_frame(streamer->frame_capture);
            if (frame) {
                if (frame_streamer_send_frame(streamer, frame) == 0) {
                    streamer->frames_sent++;
                    streamer->frame_id++;
                    
                    if (streamer->frames_sent % 60 == 0) {
                        printf("Sent %d PNG frames\n", streamer->frames_sent);
                    }
                } else {
                    fprintf(stderr, "Failed to send PNG frame %d\n", streamer->frame_id);
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
    
    free(streamer);
}