#include "frame_streamer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static volatile bool keep_streaming = true;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    (void)sig;
    keep_streaming = false;
}

// Convert XImage to RGB data
static void ximage_to_rgb(XImage *img, unsigned char *rgb_buffer) {
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            
            // Extract RGB components
            unsigned char r = (pixel >> 16) & 0xFF;
            unsigned char g = (pixel >> 8) & 0xFF;
            unsigned char b = pixel & 0xFF;
            
            int idx = (y * img->width + x) * 3;
            rgb_buffer[idx] = r;
            rgb_buffer[idx + 1] = g;
            rgb_buffer[idx + 2] = b;
        }
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
    
    // Allocate RGB conversion buffer
    ClientInfo *client = udp_server_get_client(udp_server);
    streamer->buffer_size = client->width * client->height * 3; // RGB
    streamer->rgb_buffer = malloc(streamer->buffer_size);
    
    if (!streamer->rgb_buffer) {
        fprintf(stderr, "Failed to allocate RGB buffer\n");
        fc_cleanup(streamer->frame_capture);
        free(streamer);
        return NULL;
    }
    
    printf("Frame streamer initialized for output '%s'\n", output_name);
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

// Send frame info to client
int frame_streamer_send_frame_info(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    ClientInfo *client = udp_server_get_client(streamer->udp_server);
    UDPServer *server = streamer->udp_server;
    
    char info_packet[64];
    snprintf(info_packet, sizeof(info_packet), "INFO:%d:%d", 
             client->width, client->height);
    
    if (udp_server_send_response(server, info_packet) != 0) {
        return -1;
    }
    
    printf("Sent frame info: %dx%d\n", client->width, client->height);
    
    // Small delay to ensure client receives frame info before data packets
    usleep(10000); // 10ms delay
    
    return 0;
}

// Send frame data in chunks
int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame) {
    if (!streamer || !frame) return -1;
    
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    // Convert XImage to RGB
    ximage_to_rgb(frame, streamer->rgb_buffer);
    
    // Calculate packet info
    size_t frame_size = streamer->buffer_size;
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (frame_size + data_per_packet - 1) / data_per_packet;
    
    // Debug info for first frame
    if (streamer->frame_id == 0) {
        printf("Sending frame %d: %dx%d (%zu bytes) in %zu packets\n",
               streamer->frame_id, frame->width, frame->height, frame_size, total_packets);
    }
    
    // Send packets
    for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
        // Calculate data size for this packet
        size_t remaining = frame_size - (packet_id * data_per_packet);
        size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
        
        // Create packet
        char packet[MAX_PACKET_SIZE];
        PacketHeader *header = (PacketHeader*)packet;
        
        header->frame_id = htonl(streamer->frame_id);
        header->packet_id = htonl(packet_id);
        header->total_packets = htonl(total_packets);
        header->data_size = htonl(current_data_size);
        
        // Copy data
        memcpy(packet + sizeof(PacketHeader), 
               streamer->rgb_buffer + (packet_id * data_per_packet), 
               current_data_size);
        
        // Send packet
        ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
        ssize_t bytes_sent = sendto(server->socket_fd, packet, packet_size, 0,
                                   (struct sockaddr*)&client->address, 
                                   client->address_len);
        
        if (bytes_sent < 0) {
            perror("Failed to send packet");
            return -1;
        }
        
        // Small delay between packets
        usleep(50); // 0.05ms delay
    }
    
    // Print progress for every 10th frame
    if (streamer->frame_id % 10 == 0) {
        printf("Sent frame %d (%zu packets)\n", streamer->frame_id, total_packets);
    }
    
    return 0;
}

// Main streaming loop
int frame_streamer_run_loop(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    
    printf("Starting streaming loop... Press Ctrl+C to stop\n");
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
                        printf("Sent %d frames\n", streamer->frames_sent);
                    }
                } else {
                    fprintf(stderr, "Failed to send frame %d\n", streamer->frame_id);
                }
                
                fc_mark_frame_processed(streamer->frame_capture);
            }
        } else if (result < 0) {
            fprintf(stderr, "Capture failed\n");
            break;
        }
        
        // Small sleep to prevent busy waiting
        usleep(5000); // 5ms
    }
    
    printf("\nStreamed %d frames total\n", streamer->frames_sent);
    return 0;
}

// Start streaming (complete flow)
int frame_streamer_start(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    // Step 1: Wait for client to request streaming
    if (frame_streamer_wait_for_start_command(streamer) != 0) {
        return -1;
    }
    
    // Step 2: Send frame info
    if (frame_streamer_send_frame_info(streamer) != 0) {
        return -1;
    }
    
    // Step 3: Run streaming loop
    return frame_streamer_run_loop(streamer);
}

// Print streamer status
void frame_streamer_print_status(FrameStreamer *streamer) {
    if (!streamer) return;
    
    printf("Frame Streamer Status:\n");
    printf("  Streaming: %s\n", streamer->streaming ? "YES" : "NO");
    printf("  Frames sent: %d\n", streamer->frames_sent);
    printf("  Current frame ID: %d\n", streamer->frame_id);
    printf("  Buffer size: %zu bytes\n", streamer->buffer_size);
    
    if (streamer->frame_capture) {
        fc_print_frame_info(streamer->frame_capture);
    }
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
    
    frame_streamer_stop(streamer);
    
    if (streamer->frame_capture) {
        fc_cleanup(streamer->frame_capture);
    }
    
    if (streamer->rgb_buffer) {
        free(streamer->rgb_buffer);
    }
    
    free(streamer);
}