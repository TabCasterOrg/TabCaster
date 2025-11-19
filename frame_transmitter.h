#ifndef FRAME_TRANSMITTER_H
#define FRAME_TRANSMITTER_H

#include "udp_server.h"
#include "frame_streamer.h" 
#include <stdint.h>
#include <stddef.h>

typedef struct FrameTransmitter {
    UDPServer *udp_server;
    uint32_t frame_id;
    uint32_t last_sent_frame_id;
} FrameTransmitter;

FrameTransmitter* transmitter_init(UDPServer *server);
int transmitter_send_keyframe(FrameTransmitter *tx, const unsigned char *png_data, size_t png_size);
int transmitter_send_delta(FrameTransmitter *tx, DeltaFrame *delta);
void transmitter_cleanup(FrameTransmitter *tx);

#endif // FRAME_TRANSMITTER_H

