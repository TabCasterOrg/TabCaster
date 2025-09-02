#ifndef FRAME_STREAMER_H
#define FRAME_STREAMER_H

#include "udp_server.h"
#include "frame_capture.h"
#include <stdint.h>

#define MAX_PACKET_SIZE 1400
#define FRAME_HEADER_SIZE 16

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
    
    // Frame conversion buffer
    unsigned char *rgb_buffer;
    size_t buffer_size;
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