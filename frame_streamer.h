#ifndef FRAME_STREAMER_H
#define FRAME_STREAMER_H

#include "udp_server.h"
#include "frame_capture.h"


#include <stdint.h>

#define MAX_PACKET_SIZE 1400
#define DEFAULT_TILE_SIZE 128
#define MIN_CHANGE_PIXELS 4096
#define MAX_RECTS_PER_FRAME 64

// Delta packet header for raw RGB updates
// keyframe: send whole frame as one or multiple tiles
typedef struct __attribute__((packed)) {
    uint32_t frame_id;     // monotonically increasing
    uint16_t x;            // rect origin (absolute desktop coords)
    uint16_t y;
    uint16_t w;            // rect size
    uint16_t h;
    uint8_t  is_keyframe;  // 1 keyframe, 0 delta
    uint8_t  reserved[3];
    uint32_t data_size;    // bytes of RGB following
} PacketHeader;

// Frame streamer structure
typedef struct {
    UDPServer *udp_server;
    FrameCapture *frame_capture;
    
    // Streaming state
    bool streaming;
    uint32_t frame_id;
    int frames_sent;
    
    // Frame conversion buffer
    unsigned char *rgb_buffer;
    size_t buffer_size;
    int keyframe_interval; // frames between keyframes
    int since_keyframe;
    // Time-based keyframe control
    struct timeval last_keyframe_time;
    long keyframe_period_us; // e.g., 10s

    // Stats
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t tiles_sent;           // rect tiles encoded/sent
    uint64_t encode_us_accum;      // sum of encode times (us)
    uint64_t send_us_accum;        // sum of send times (us)
    struct timeval stats_window_start; // window start for periodic reporting
    uint32_t frames_in_window;     // frames completed in current window
} FrameStreamer;

// Core functions
FrameStreamer* frame_streamer_init(UDPServer *udp_server, const char *output_name, int fps);
int frame_streamer_start(FrameStreamer *streamer);
int frame_streamer_wait_for_start_command(FrameStreamer *streamer);
int frame_streamer_send_frame_info(FrameStreamer *streamer);
int frame_streamer_run_loop(FrameStreamer *streamer);
int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame);
void frame_streamer_stop(FrameStreamer *streamer);
void frame_streamer_cleanup(FrameStreamer *streamer);

// Helper functions
void frame_streamer_print_status(FrameStreamer *streamer);

#endif // FRAME_STREAMER_H