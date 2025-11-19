#ifndef FRAME_STREAMER_H
#define FRAME_STREAMER_H

#include "udp_server.h"
#include "frame_capture.h"
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

#define MAX_PACKET_SIZE 1400
#define FRAME_HEADER_SIZE 16
#define MAX_DELTA_OPERATIONS 16  

typedef struct __attribute__((packed)) {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t flags;      
    uint8_t quality;    
    uint16_t reserved;  
} RegionHeader;

// Operation types 
typedef enum {
    OP_CREG = 0,  
    OP_DREG = 1   
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

// Forward declarations
typedef struct DeltaEncoder DeltaEncoder;
typedef struct FrameTransmitter FrameTransmitter;

// Frame streamer structure
typedef struct {
    UDPServer *udp_server;  
    FrameCapture *frame_capture;
    DeltaEncoder *delta_encoder;      
    FrameTransmitter *transmitter;
    
    bool delta_mode_enabled;
    int keyframe_interval;
    int keyframe_interval_sec;
    int captures_since_keyframe;
    struct timeval last_keyframe_time;
    struct timeval last_activity_time;
    
    // Stats
    int frames_sent;
    bool streaming;
    uint32_t frame_id;
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