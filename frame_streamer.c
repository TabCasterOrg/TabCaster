#include "frame_streamer.h"
#include "udp_server.h"
#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <webp/encode.h>

static volatile bool keep_streaming = true;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    (void)sig;
    keep_streaming = false;
}

// Fallback conversion for uncommon XImage formats
static int encode_ximage_fallback(XImage *img, unsigned char **webp_data, 
                                 size_t *webp_size, float quality) {
    printf("Using fallback XImage conversion for format: depth=%d, bpp=%d\n", 
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
    
    *webp_size = WebPEncodeRGB(rgb_buffer, img->width, img->height, 
                              img->width * 3, quality, webp_data);
    
    free(rgb_buffer);
    return (*webp_size > 0) ? 0 : -1;
}

// Direct XImage to WebP conversion
static int encode_ximage_to_webp(XImage *img, unsigned char **webp_data, 
                                size_t *webp_size, float quality) {
    int width = img->width;
    int height = img->height;
    
    // Debug the XImage format for first frame
    static int first_call = 1;
    if (first_call) {
        printf("XImage format debug:\n");
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
        // X11 typically uses BGRX with red_mask=0xff0000, green_mask=0xff00, blue_mask=0xff
        
        if (img->red_mask == 0xff0000 && img->green_mask == 0xff00 && img->blue_mask == 0xff) {
            // Standard BGRX format - convert to RGBA for WebP
            size_t rgba_size = width * height * 4;
            unsigned char *rgba_data = malloc(rgba_size);
            if (!rgba_data) return -1;
            
            unsigned char *src = (unsigned char*)img->data;
            unsigned char *dst = rgba_data;
            
            for (int y = 0; y < height; y++) {
                unsigned char *line_src = src + (y * img->bytes_per_line);
                for (int x = 0; x < width; x++) {
                    // BGRX → RGBA conversion
                    unsigned char b = line_src[x * 4 + 0];
                    unsigned char g = line_src[x * 4 + 1];
                    unsigned char r = line_src[x * 4 + 2];
                    // Skip X (padding byte at line_src[x * 4 + 3])
                    
                    dst[(y * width + x) * 4 + 0] = r; // R
                    dst[(y * width + x) * 4 + 1] = g; // G
                    dst[(y * width + x) * 4 + 2] = b; // B
                    dst[(y * width + x) * 4 + 3] = 255; // A (fully opaque)
                }
            }
            
            // Encode RGBA to WebP
            *webp_size = WebPEncodeRGBA(rgba_data, width, height, width * 4, quality, webp_data);
            free(rgba_data);
            
        } else {
            // Non-standard masks - use pixel-by-pixel conversion
            return encode_ximage_fallback(img, webp_data, webp_size, quality);
        }
    } else {
        // Other formats - use fallback
        return encode_ximage_fallback(img, webp_data, webp_size, quality);
    }
    
    return (*webp_size > 0) ? 0 : -1;
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
    
    // No RGB buffer allocation - direct WebP encoding
    streamer->webp_quality = 100.0f; // Good balance of quality and compression speed
    
    printf("Optimized frame streamer initialized for '%s' with direct WebP encoding (quality: %.1f)\n", 
           output_name, streamer->webp_quality);
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

// Send frame info to client - indicates WebP format
int frame_streamer_send_frame_info(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    ClientInfo *client = udp_server_get_client(streamer->udp_server);
    UDPServer *server = streamer->udp_server;
    
    // Include format information
    char info_packet[64];
    snprintf(info_packet, sizeof(info_packet), "INFO:%d:%d:WEBP", 
             client->width, client->height);
    
    if (udp_server_send_response(server, info_packet) != 0) {
        return -1;
    }
    
    printf("Sent frame info: %dx%d WebP format\n", client->width, client->height);
    
    // Small delay to ensure client receives frame info before data packets
    usleep(10000); // 10ms delay
    
    return 0;
}

// Optimized frame sending with direct WebP encoding
int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame) {
    if (!streamer || !frame) return -1;
    
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    // Direct XImage to WebP conversion - no intermediate RGB buffer
    unsigned char *webp_data = NULL;
    size_t webp_size = 0;
    
    if (encode_ximage_to_webp(frame, &webp_data, &webp_size, streamer->webp_quality) != 0) {
        fprintf(stderr, "Failed to encode frame %d to WebP\n", streamer->frame_id);
        return -1;
    }
    
    // Calculate packet info
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (webp_size + data_per_packet - 1) / data_per_packet;
    
    // Debug info for first few frames only
    if (streamer->frame_id < 3) {
        size_t estimated_rgb_size = frame->width * frame->height * 3;
        float compression_ratio = (float)estimated_rgb_size / webp_size;
        printf("Frame %d: %dx%d -> WebP(%zu bytes) = %.1fx compression, %zu packets\n",
               streamer->frame_id, frame->width, frame->height, 
               webp_size, compression_ratio, total_packets);
    }
    
    // Send all packets with minimal delays
    for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
        size_t remaining = webp_size - (packet_id * data_per_packet);
        size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
        
        char packet[MAX_PACKET_SIZE];
        PacketHeader *header = (PacketHeader*)packet;
        
        header->frame_id = htonl(streamer->frame_id);
        header->packet_id = htonl(packet_id);
        header->total_packets = htonl(total_packets);
        header->data_size = htonl(current_data_size);
        
        memcpy(packet + sizeof(PacketHeader), 
               webp_data + (packet_id * data_per_packet), 
               current_data_size);
        
        ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
        ssize_t bytes_sent = sendto(server->socket_fd, packet, packet_size, 0,
                                   (struct sockaddr*)&client->address, 
                                   client->address_len);
        
        if (bytes_sent < 0) {
            perror("Failed to send packet");
            WebPFree(webp_data);
            return -1;
        }
        
        // Minimal delay only for very large frames
        if (total_packets > 100) {
            usleep(5); // Reduced from 50μs to 5μs, only for huge frames
        }
    }
    
    WebPFree(webp_data);
    
    // Less frequent progress updates
    if (streamer->frame_id % 60 == 0) {
        printf("Sent frame %d (%zu WebP bytes in %zu packets)\n", 
               streamer->frame_id, webp_size, total_packets);
    }
    
    return 0;
}

// main streaming loop
int frame_streamer_run_loop(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    
    printf("Starting optimized WebP streaming loop... Press Ctrl+C to stop\n");
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
                        printf("Sent %d WebP frames\n", streamer->frames_sent);
                    }
                } else {
                    fprintf(stderr, "Failed to send WebP frame %d\n", streamer->frame_id);
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
    
    printf("\nStreamed %d WebP frames total\n", streamer->frames_sent);
    return 0;
}

// Start streaming
int frame_streamer_start(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    // STEP 1: Wait for client to request streaming
    if (frame_streamer_wait_for_start_command(streamer) != 0) {
        return -1;
    }
    
    // STEP 2: Send frame info (now includes WebP format)
    if (frame_streamer_send_frame_info(streamer) != 0) {
        return -1;
    }
    
    // STEP 3: Run streaming loop
    return frame_streamer_run_loop(streamer);
}

// Print streamer status with WebP info
void frame_streamer_print_status(FrameStreamer *streamer) {
    if (!streamer) return;
    
    printf("Optimized Frame Streamer Status (Direct WebP):\n");
    printf("  Streaming: %s\n", streamer->streaming ? "YES" : "NO");
    printf("  Frames sent: %d\n", streamer->frames_sent);
    printf("  Current frame ID: %d\n", streamer->frame_id);
    printf("  WebP quality: %.1f\n", streamer->webp_quality);
    printf("  Mode: Direct XImage->WebP (no RGB buffer)\n");
    
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

// Cleanup resources - no RGB buffer to free
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