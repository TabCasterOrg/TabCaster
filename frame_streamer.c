#include "frame_streamer.h"
#include "udp_server.h"
#include "mode_manager.h"
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

// Convert XImage to RGB data (assumes 24bpp RGB target)
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
    
    // Allocate RGB conversion buffer based on actual capture size
    ClientInfo *client = udp_server_get_client(udp_server);
    XImage *initial_frame = fc_get_frame(streamer->frame_capture);
    unsigned int target_width = client->width;
    unsigned int target_height = client->height;
    if (initial_frame) {
        target_width = (unsigned int)initial_frame->width;
        target_height = (unsigned int)initial_frame->height;
    }
    streamer->buffer_size = (size_t)target_width * (size_t)target_height * 3; // RGB
    streamer->rgb_buffer = malloc(streamer->buffer_size);
    
    if (!streamer->rgb_buffer) {
        fprintf(stderr, "Failed to allocate RGB buffer\n");
        fc_cleanup(streamer->frame_capture);
        free(streamer);
        return NULL;
    }
    
    // Configure keyframe cadence (~4s by frame count and 10s as time fallback)
    streamer->keyframe_interval = fps > 0 ? (fps * 4) : 120;
    streamer->since_keyframe = 0;
    gettimeofday(&streamer->last_keyframe_time, NULL);
    streamer->keyframe_period_us = 10000000L; // 10 seconds

    // Initialize damage tracking
    fc_init_damage(streamer->frame_capture);

    // Init stats
    streamer->packets_sent = 0;
    streamer->bytes_sent = 0;
    streamer->tiles_sent = 0;
    streamer->encode_us_accum = 0;
    streamer->send_us_accum = 0;
    streamer->frames_in_window = 0;
    gettimeofday(&streamer->stats_window_start, NULL);

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

// Convert a rectangle region of XImage to RGB buffer at offset
static void ximage_rect_to_rgb(XImage *img, int rx, int ry, int rw, int rh, unsigned char *rgb_out) {
    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < rw; x++) {
            unsigned long pixel = XGetPixel(img, rx + x, ry + y);
            unsigned char r = (pixel >> 16) & 0xFF;
            unsigned char g = (pixel >> 8) & 0xFF;
            unsigned char b = pixel & 0xFF;
            int idx = (y * rw + x) * 3;
            rgb_out[idx] = r;
            rgb_out[idx + 1] = g;
            rgb_out[idx + 2] = b;
        }
    }
}

// Send a single rectangle update (or keyframe tile) as one UDP packet
static int send_rect_packet(FrameStreamer *streamer, XImage *frame, int rx, int ry, int rw, int rh, bool is_keyframe) {
    if (rw <= 0 || rh <= 0) return 0;
    UDPServer *server = streamer->udp_server;
    ClientInfo *client = udp_server_get_client(server);

    size_t header_size = sizeof(PacketHeader);
    size_t max_data = MAX_PACKET_SIZE - header_size; // max RGB bytes per packet
    if (max_data < 3) return -1; // impossible to send even one pixel

    // Determine tile dimensions that always fit in one packet
    int tile_w = rw;
    int tile_h = rh;
    size_t rect_bytes = (size_t)tile_w * (size_t)tile_h * 3;
    if (rect_bytes > max_data) {
        // First cap width to fit at least one row
        size_t max_pixels = max_data / 3; // RGB24
        if ((size_t)tile_w > max_pixels) {
            tile_w = (int)max_pixels;
            if (tile_w <= 0) tile_w = 1;
        }
        // Then choose height so that tile_w*tile_h fits
        size_t max_rows = max_pixels / (size_t)tile_w;
        tile_h = (int)(max_rows > 0 ? max_rows : 1);
        if (tile_h <= 0) tile_h = 1;
    }

    // Snap tiles to a default block size for better batching
    if (tile_w > DEFAULT_TILE_SIZE) tile_w = DEFAULT_TILE_SIZE;
    if (tile_h > DEFAULT_TILE_SIZE) tile_h = DEFAULT_TILE_SIZE;

    for (int ty = 0; ty < rh; ty += tile_h) {
        int cur_h = ty + tile_h > rh ? (rh - ty) : tile_h;
        for (int tx = 0; tx < rw; tx += tile_w) {
            int cur_w = tx + tile_w > rw ? (rw - tx) : tile_w;

            size_t cur_rgb_size = (size_t)cur_w * (size_t)cur_h * 3;
            if (cur_rgb_size > streamer->buffer_size) {
                unsigned char *nb = realloc(streamer->rgb_buffer, cur_rgb_size);
                if (!nb) {
                    perror("Failed to resize RGB buffer");
                    return -1;
                }
                streamer->rgb_buffer = nb;
                streamer->buffer_size = cur_rgb_size;
            }

            struct timeval t0, t1, t2;
            gettimeofday(&t0, NULL);
            ximage_rect_to_rgb(frame, rx + tx, ry + ty, cur_w, cur_h, streamer->rgb_buffer);
            gettimeofday(&t1, NULL);

            char packet[MAX_PACKET_SIZE];
            PacketHeader *header = (PacketHeader*)packet;
            header->frame_id = htonl(streamer->frame_id);
            // Send coordinates RELATIVE to capture origin so client blits into 0..width/height
            int origin_x = streamer->frame_capture->x;
            int origin_y = streamer->frame_capture->y;
            header->x = htons((uint16_t)((rx + tx) - origin_x));
            header->y = htons((uint16_t)((ry + ty) - origin_y));
            header->w = htons((uint16_t)cur_w);
            header->h = htons((uint16_t)cur_h);
            header->is_keyframe = is_keyframe ? 1 : 0;
            header->data_size = htonl((uint32_t)cur_rgb_size);

            memcpy(packet + sizeof(PacketHeader), streamer->rgb_buffer, cur_rgb_size);

            ssize_t packet_size = (ssize_t)(sizeof(PacketHeader) + cur_rgb_size);
            if (packet_size > (ssize_t)MAX_PACKET_SIZE) {
                fprintf(stderr, "Packet assembly overflow: %zd > %d\n", packet_size, MAX_PACKET_SIZE);
                return -1;
            }

            ssize_t sent = sendto(server->socket_fd, packet, packet_size, 0,
                                  (struct sockaddr*)&client->address, client->address_len);
            if (sent < 0) {
                perror("Failed to send rect packet");
                return -1;
            }

            gettimeofday(&t2, NULL);
            long encode_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_usec - t0.tv_usec);
            long send_us = (t2.tv_sec - t1.tv_sec) * 1000000L + (t2.tv_usec - t1.tv_usec);
            streamer->encode_us_accum += (uint64_t)encode_us;
            streamer->send_us_accum += (uint64_t)send_us;
            streamer->packets_sent++;
            streamer->tiles_sent++;
            streamer->bytes_sent += (uint64_t)packet_size;
        }
    }
    return 0;
}

// Send frame data using damage tracking (keyframe or deltas)
int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame) {
    if (!streamer || !frame) return -1;

    bool force_keyframe = (streamer->since_keyframe >= streamer->keyframe_interval);
    // Also force keyframe if enough time elapsed
    struct timeval now_kf;
    gettimeofday(&now_kf, NULL);
    long since_kf_us = (now_kf.tv_sec - streamer->last_keyframe_time.tv_sec) * 1000000L +
                      (now_kf.tv_usec - streamer->last_keyframe_time.tv_usec);
    if (since_kf_us >= streamer->keyframe_period_us) {
        force_keyframe = true;
    }
    if (!fc_has_new_frame(streamer->frame_capture)) return 0;

    // Gather damage
    XRectangle *rects = NULL;
    int nrects = 0;
    if (!force_keyframe) {
        fc_get_damage_rects(streamer->frame_capture, &rects, &nrects);
    }

    int result = 0;
    if (force_keyframe || nrects <= 0) {
        // Send a keyframe in tiles to respect MTU
        result = send_rect_packet(streamer, frame, streamer->frame_capture->x, streamer->frame_capture->y,
                                  (int)frame->width, (int)frame->height, true);
        streamer->since_keyframe = 0;
        streamer->last_keyframe_time = now_kf;
    } else {
        // Send each damaged rect (already in absolute coords). Optionally coalesce further if needed
        // Filter out tiny changes to reduce packet storms
        for (int i = 0; i < nrects && result == 0; i++) {
            int area = rects[i].width * rects[i].height;
            if (area < MIN_CHANGE_PIXELS) continue;
            result = send_rect_packet(streamer, frame,
                                      rects[i].x, rects[i].y,
                                      rects[i].width, rects[i].height, false);
        }
        streamer->since_keyframe++;
    }

    if (rects) XFree(rects);
    return result;
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
    
    Display *dpy = streamer->udp_server->dm->display;
    const long idle_sleep_us = 10000; // 10ms when no events and no keyframe due
    
    while (keep_streaming && streamer->streaming) {
        bool sent_any = false;

        // Check if a keyframe is due regardless of damage
        if (streamer->since_keyframe >= streamer->keyframe_interval) {
            if (fc_capture_frame(streamer->frame_capture) < 0) {
                fprintf(stderr, "Capture failed (keyframe)\n");
                break;
            }
            XImage *frame = fc_get_frame(streamer->frame_capture);
            if (frame) {
                if (frame_streamer_send_frame(streamer, frame) == 0) {
                    streamer->frames_sent++;
                    streamer->frame_id++;
                }
                fc_mark_frame_processed(streamer->frame_capture);
                sent_any = true;
                // since_keyframe reset is done inside send when keyframe path is used
            }
        } else if (streamer->frame_capture->damage_enabled) {
            // Process pending X events to register damage
            int pending = XPending(dpy);
            if (pending > 0) {
                XEvent ev;
                // Drain event queue quickly; damage is accumulated and fetched below
                while (pending-- > 0) {
                    XNextEvent(dpy, &ev);
                }
            }

            // Fetch damage rects; if none, idle
            XRectangle *rects = NULL;
            int nrects = 0;
            if (fc_get_damage_rects(streamer->frame_capture, &rects, &nrects) == 0 && nrects > 0) {
                // Capture once to get a fresh frame for rect extraction
                if (fc_capture_frame(streamer->frame_capture) < 0) {
                    fprintf(stderr, "Capture failed (damage)\n");
                    if (rects) XFree(rects);
                    break;
                }
                XImage *frame = fc_get_frame(streamer->frame_capture);
                if (frame) {
                    // Send deltas using the filtered rects; skip tiny ones and cap count
                    int sent_rects = 0;
                    for (int i = 0; i < nrects; i++) {
                        int area = rects[i].width * rects[i].height;
                        if (area < MIN_CHANGE_PIXELS) continue;
                        if (sent_rects >= MAX_RECTS_PER_FRAME) break;
                        if (send_rect_packet(streamer, frame,
                                             rects[i].x, rects[i].y,
                                             rects[i].width, rects[i].height, false) != 0) {
                            fprintf(stderr, "Failed to send delta rect\n");
                            break;
                        }
                        sent_rects++;
                    }
                    streamer->frames_sent++;
                    streamer->frame_id++;
                    sent_any = true;
                    fc_mark_frame_processed(streamer->frame_capture);
                }
                if (rects) XFree(rects);
            }
        }

        if (!sent_any) {
            // Idle briefly to avoid busy spin; events will wake in next loop
            usleep(idle_sleep_us);
        }

        // Periodic stats every second
        struct timeval now;
        gettimeofday(&now, NULL);
        long window_us = (now.tv_sec - streamer->stats_window_start.tv_sec) * 1000000L +
                         (now.tv_usec - streamer->stats_window_start.tv_usec);
        if (window_us >= 1000000L) {
            double mbps = (streamer->bytes_sent * 8.0) / (double)window_us; // Mbit/us
            mbps *= 1000000.0 / (1024.0 * 1024.0); // convert to Mbit/s
            double avg_enc_us = streamer->packets_sent ? (double)streamer->encode_us_accum / (double)streamer->packets_sent : 0.0;
            double avg_send_us = streamer->packets_sent ? (double)streamer->send_us_accum / (double)streamer->packets_sent : 0.0;
            int fps_window = streamer->frames_sent - (int)streamer->frames_in_window;
            printf("[STATS] fps=%d tiles=%lu packets=%lu bytes=%lu avg_enc=%.1fus avg_send=%.1fus bitrate=%.2f Mbit/s\n",
                   fps_window,
                   streamer->tiles_sent, streamer->packets_sent, streamer->bytes_sent,
                   avg_enc_us, avg_send_us, mbps);
            streamer->frames_in_window = streamer->frames_sent;
            streamer->packets_sent = 0;
            streamer->tiles_sent = 0;
            streamer->bytes_sent = 0;
            streamer->encode_us_accum = 0;
            streamer->send_us_accum = 0;
            streamer->stats_window_start = now;
        }
    }
    
    printf("\nStreamed %d frames total\n", streamer->frames_sent);
    return 0;
}

// Start streaming (complete flow)
int frame_streamer_start(FrameStreamer *streamer) {
    if (!streamer) return -1;
    
    // STEP 1: Wait for client to request streaming
    if (frame_streamer_wait_for_start_command(streamer) != 0) {
        return -1;
    }
    
    // STEP 2: Send frame info
    if (frame_streamer_send_frame_info(streamer) != 0) {
        return -1;
    }
    
    // STEP 3: Run streaming loop
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
        // Continue with cleanup even if disable fails
    } else {
        printf(" Output disabled successfully\n");
    }
    
    // STEP 2: Remove mode from output
    printf("STEP 2: Removing mode %lu from output '%s'...\n", mode_id, output_name);
    if (mode_remove_from_output(dm, output_name, mode_id) != 0) {
        fprintf(stderr, "Warning: Failed to remove mode from output '%s'\n", output_name);
        // Continue with cleanup even if remove fails
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

// Cleanup resources- calls every other cleanup function 
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
    
    // Free RGB buffer
    if (streamer->rgb_buffer) {
        free(streamer->rgb_buffer);
    }
    
    free(streamer);
}