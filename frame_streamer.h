#ifndef FRAME_STREAMER_H
#define FRAME_STREAMER_H

#include "udp_server.h"
#include "frame_capture.h"
#include <webp/encode.h>


#include <stdint.h>

#define MAX_PACKET_SIZE 1400
#define FRAME_HEADER_SIZE 16

// Delta streaming protocol (scaffolding)
typedef struct __attribute__((packed)) {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t flags;      // bit0: is_keyframe, bit1: allow_lossy, others reserved
    uint8_t quality;    // 0-100 for adaptive quality
    uint16_t reserved;  // alignment
} RegionHeader;

// Frame packet header structure
typedef struct __attribute__((packed)) {
    uint32_t frame_id;
    uint32_t packet_id;
    uint32_t total_packets;
    uint32_t data_size;
} PacketHeader;

// Frame streamer structure
typedef struct {
    UDPServer *udp_server;
    FrameCapture *frame_capture;
    
    // Streaming state
    bool streaming;
    uint32_t frame_id;
    int frames_sent;

    float webp_quality;        //field for WebP quality (0-100)
    
    // Frame conversion buffer
    unsigned char *rgb_buffer;
    size_t buffer_size;

    // Persistent reference frame for delta encoding (RGB)
    unsigned char *reference_frame_rgb;
    size_t reference_size;
    unsigned int reference_width;
    unsigned int reference_height;
    bool delta_mode_enabled;    // off by default; scaffolding only

    // Delta tuning parameters
    int diff_threshold;         // pixel difference sum threshold per pixel (default 30)
    int cover_threshold_pct;    // max changed coverage percentage for delta (default 80)
    int keyframe_interval;      // send full keyframe every N frames when coverage high (default 120)

    // Keyframe cadence based on captures (independent of sends)
    int captures_since_keyframe;
    
    // Inactivity detection for preventing reference frame desync
    struct timeval last_activity_time;
    int inactivity_threshold_sec;
    
    // Reference frame validation
    uint32_t reference_frame_checksum;
    uint32_t last_sent_frame_id;
} FrameStreamer;

// Core functions
FrameStreamer* frame_streamer_init(UDPServer *udp_server, const char *output_name, int fps);
int frame_streamer_start(FrameStreamer *streamer);
int frame_streamer_wait_for_start_command(FrameStreamer *streamer);
int frame_streamer_handle_keyframe_request(FrameStreamer *streamer);
int frame_streamer_send_frame_info(FrameStreamer *streamer);
int frame_streamer_run_loop(FrameStreamer *streamer);
int frame_streamer_send_frame(FrameStreamer *streamer, XImage *frame);
void frame_streamer_stop(FrameStreamer *streamer);
void frame_streamer_cleanup(FrameStreamer *streamer);

// Helper functions
void frame_streamer_print_status(FrameStreamer *streamer);

#endif // FRAME_STREAMER_H