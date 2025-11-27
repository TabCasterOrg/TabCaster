#include "frame_streamer.h"
#include "udp_server.h"
#include "mode_manager.h"
#include "delta_encoder.h"
#include "png_encoder.h"
#include "frame_transmitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

static volatile bool keep_streaming = true;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    (void)sig;
    keep_streaming = false;
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
    
    // Initialize transmitter
    streamer->transmitter = transmitter_init(udp_server);
    if (!streamer->transmitter) {
        fprintf(stderr, "Failed to initialize frame transmitter\n");
        fc_cleanup(streamer->frame_capture);
        free(streamer);
        return NULL;
    }
    
    // Toggle delta mode via environment variable TABC_DELTA=1
    const char *delta_env = getenv("TABC_DELTA");
    streamer->delta_mode_enabled = (delta_env && strcmp(delta_env, "1") == 0);
    
    // Initialize delta encoder if enabled (will be initialized on first frame when we know dimensions)
    streamer->delta_encoder = NULL;

    // Keyframe tunables
    streamer->keyframe_interval = 120;
    streamer->keyframe_interval_sec = 3;
    const char *key_env = getenv("TABC_KEYINT");
    const char *keysec_env = getenv("TABC_KEYSEC");
    if (key_env) streamer->keyframe_interval = atoi(key_env);
    if (keysec_env) streamer->keyframe_interval_sec = atoi(keysec_env);

    streamer->captures_since_keyframe = 0;
    
    // Initialize time-based keyframe tracking
    gettimeofday(&streamer->last_keyframe_time, NULL);
    gettimeofday(&streamer->last_activity_time, NULL);

    printf("Optimized frame streamer initialized for '%s' | delta %s | keyint=%d keysec=%d\n",
           output_name,
           streamer->delta_mode_enabled ? "ENABLED" : "DISABLED",
           streamer->keyframe_interval,
           streamer->keyframe_interval_sec);
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
    struct timeval timeout = {0, 0}; 
    
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
                streamer->captures_since_keyframe = streamer->keyframe_interval;
                return 1; 
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
    
    usleep(10000); 
    
    return 0;
}

int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame) {
    if (!streamer || !frame) return -1;
    
    // Debug output
    static int debug_frame_count = 0;
    static int total_debug_frames = 5;
    bool debug_output = (debug_frame_count < total_debug_frames);
    
    // Initialize delta encoder on first frame if enabled
    if (streamer->delta_mode_enabled && !streamer->delta_encoder) {
        streamer->delta_encoder = delta_encoder_init(frame->width, frame->height);
        if (!streamer->delta_encoder) {
            fprintf(stderr, "Failed to initialize delta encoder\n");
            // Continue without delta encoding
            streamer->delta_mode_enabled = false;
        }
    }
    
    // Update transmitter frame ID
    streamer->transmitter->frame_id = streamer->frame_id;
    
    // Delta mode logic
    if (streamer->delta_mode_enabled && streamer->delta_encoder && streamer->frame_id > 0 &&
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
        bool inactivity_keyframe = (time_since_activity > 5000000); // 5 seconds
        
        if (debug_output) {
            printf("  Force keyframe: %s, Time keyframe: %s, Inactivity keyframe: %s\n", 
                   force_keyframe ? "YES" : "NO", time_keyframe ? "YES" : "NO", inactivity_keyframe ? "YES" : "NO");
        }
        
        // Try to create delta frame if not forcing keyframe
        if (!force_keyframe && !time_keyframe && !inactivity_keyframe) {
            DeltaFrame delta_frame;
            delta_frame.frame_id = streamer->frame_id;
            int delta_result = delta_encoder_create_frame(streamer->delta_encoder, frame, &delta_frame);
            
            if (delta_result == 0) {
                if (debug_output) {
                    printf("  Sending delta frame with %d operations\n", delta_frame.operation_count);
                }
                
                int send_result = transmitter_send_delta(streamer->transmitter, &delta_frame);
                delta_encoder_cleanup_frame(&delta_frame);
                
                if (send_result == 0) {
                    // Update reference frame with current frame data
                    size_t rgb_size = (size_t)frame->width * (size_t)frame->height * 3;
                    unsigned char *rgb_buf = (unsigned char*)malloc(rgb_size);
                    if (rgb_buf) {
                        png_convert_bgrx_to_rgb(frame, rgb_buf);
                        delta_encoder_update_reference(streamer->delta_encoder, rgb_buf);
                        free(rgb_buf);
                    }
                    
                    // Update counters and timestamps
                    streamer->captures_since_keyframe++;
                    gettimeofday(&streamer->last_activity_time, NULL);
                    
                    if (debug_output) {
                        printf("  Delta frame sent successfully\n");
                    }
                    
                    return 0; 
                } else {
                    fprintf(stderr, "Failed to send delta frame\n");
                    return -1;
                }
            } else if (delta_result == 1) {
                // No changes detected or coverage too high - skip sending
                if (debug_output) {
                    printf("  No changes detected or coverage too high - skipping send\n");
                }
                return 1; 
            } else {
                fprintf(stderr, "Failed to create delta frame\n");
                return -1;
            }
        }
    }

    // Send full frame (keyframe)
    if (debug_output) {
        printf("  Sending full keyframe\n");
    }
    
    // Convert frame to RGB FIRST, then encode
    size_t full_rgb_size = (size_t)frame->width * (size_t)frame->height * 3;
    unsigned char *full_rgb = (unsigned char*)malloc(full_rgb_size);
    if (!full_rgb) {
        fprintf(stderr, "Failed to allocate RGB buffer for full frame\n");
        return -1;
    }
    
    png_convert_bgrx_to_rgb(frame, full_rgb);
    
    // Encode the RGB buffer to PNG
    unsigned char *png_data = NULL;
    size_t png_size = 0;
    
    if (png_encode_rgb(full_rgb, frame->width, frame->height, &png_data, &png_size) != 0) {
        fprintf(stderr, "Failed to encode frame %d to PNG\n", streamer->frame_id);
        free(full_rgb);
        return -1;
    }
    
    if (streamer->frame_id < 3 || debug_output) {
        size_t estimated_rgb_size = frame->width * frame->height * 3;
        float compression_ratio = (float)estimated_rgb_size / png_size;
        printf("Frame %d: %dx%d -> PNG(%zu bytes) = %.1fx compression\n",
               streamer->frame_id, frame->width, frame->height, 
               png_size, compression_ratio);
    }
    
    int send_result = transmitter_send_keyframe(streamer->transmitter, png_data, png_size);
    free(png_data);
    
    // Only update reference frame if send was successful
    if (send_result == 0) {
        if (streamer->delta_mode_enabled && streamer->delta_encoder) {
            delta_encoder_update_reference(streamer->delta_encoder, full_rgb);
        }
        
        // Reset keyframe counter and update timestamps
        streamer->captures_since_keyframe = 0;
        gettimeofday(&streamer->last_keyframe_time, NULL);
        gettimeofday(&streamer->last_activity_time, NULL);
        
        if (debug_output) {
            printf("  Full keyframe sent successfully\n");
            debug_frame_count++;
        }
    }
    
    free(full_rgb);
    
    return send_result;
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
    
    // Detect client disconnection via timeout
    static time_t last_successful_send = 0;
    if (last_successful_send == 0) last_successful_send = time(NULL);
    
    while (keep_streaming && streamer->streaming) {
        frame_streamer_handle_keyframe_request(streamer);
        
        int result = fc_capture_frame(streamer->frame_capture);
        
        if (result == 1) {  
            XImage *frame = fc_get_frame(streamer->frame_capture);
            if (frame) {
                int send_result = frame_streamer_send_frame(streamer, frame);
                if (send_result == 0) {
                    last_successful_send = time(NULL);
                    streamer->frames_sent++;
                    streamer->frame_id++;
                    streamer->captures_since_keyframe = 0;
                    
                    if (streamer->frames_sent % 60 == 0) {
                        printf("Sent %d PNG frames\n", streamer->frames_sent);
                    }
                } else if (send_result < 0) {
                    fprintf(stderr, "Failed to send frame %d\n", streamer->frame_id);
                    // Check for client disconnection timeout
                    if (time(NULL) - last_successful_send > 5) {
                        printf("Client appears disconnected (5s timeout)\n");
                        break;  
                    }
                } else {
                    streamer->captures_since_keyframe++;
                }
                
                fc_mark_frame_processed(streamer->frame_capture);
            }
        } else if (result < 0) {
            fprintf(stderr, "Capture failed\n");
            break;
        }
        
        usleep(500); 
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
    
    frame_streamer_stop(streamer);
    
    cleanup_display_config(streamer);
    
    if (streamer->udp_server) {
        udp_server_reset_client_state(streamer->udp_server);
    }
    
    if (streamer->frame_capture) {
        fc_cleanup(streamer->frame_capture);
    }
    
    if (streamer->delta_encoder) {
        delta_encoder_cleanup(streamer->delta_encoder);
    }
    
    if (streamer->transmitter) {
        transmitter_cleanup(streamer->transmitter);
    }
    
    free(streamer);
}