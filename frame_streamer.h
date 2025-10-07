#ifndef FRAME_STREAMER_H
#define FRAME_STREAMER_H

#include "udp_server.h"
#include "frame_capture.h"
#include <webp/encode.h>


#include <stdint.h>

#define MAX_PACKET_SIZE 1400
#define FRAME_HEADER_SIZE 16
#define MAX_DELTA_OPERATIONS 16  // Support up to 16 operations per frame

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

// Operation types for delta frames
typedef enum {
    OP_CREG = 0,  // Clear region (restore to base frame)
    OP_DREG = 1   // Draw region (apply new content)
} OperationType;

// Individual delta operation
typedef struct {
    OperationType type;
    uint16_t x, y, width, height;
    unsigned char *png_data;
    size_t png_size;
} DeltaOperation;

// Delta frame containing multiple operations
typedef struct {
    uint32_t frame_id;
    DeltaOperation operations[MAX_DELTA_OPERATIONS];
    int operation_count;
    size_t total_payload_size;
} DeltaFrame;

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
    int keyframe_interval_sec;  // send full keyframe every N seconds (default 3)
    int region_padding;         // pixels to pad around detected change regions (default 8)
    int min_region_size;        // minimum region width/height to avoid tiny regions (default 32)
    int max_regions_per_frame;  // maximum number of regions to process per frame (default 8)
    int region_cell_size;       // size of grid cells for multi-region detection (default 64)

    // Keyframe cadence based on captures (independent of sends)
    int captures_since_keyframe;
    
    // Time-based keyframe tracking
    struct timeval last_keyframe_time;
    
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

// Delta frame functions
int frame_streamer_create_delta_frame(FrameStreamer *streamer, XImage *frame, DeltaFrame *delta_frame);
int frame_streamer_send_delta_frame(FrameStreamer *streamer, DeltaFrame *delta_frame);
void frame_streamer_cleanup_delta_frame(DeltaFrame *delta_frame);

#endif // FRAME_STREAMER_H