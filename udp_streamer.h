#ifndef UDP_STREAMER_H
#define UDP_STREAMER_H

#include "display_manager.h"
#include "frame_capture.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>

#define MAX_PACKET_SIZE 1400  // Safe UDP packet size
#define FRAME_HEADER_SIZE 16  // Header size for frame info

typedef struct {
    int socket_fd;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    bool client_connected;
    int port;
    
    // Frame info for streaming
    unsigned int frame_width;
    unsigned int frame_height;
    unsigned int bytes_per_pixel;
} UDPStreamer;

// Frame packet header structure
typedef struct __attribute__((packed)) {
    uint32_t frame_id;      // Frame sequence number
    uint32_t packet_id;     // Packet sequence in this frame
    uint32_t total_packets; // Total packets for this frame
    uint32_t data_size;     // Size of data in this packet
} PacketHeader;

// Core functions
UDPStreamer* udp_init(int port);
int udp_wait_for_client(UDPStreamer *streamer);
int udp_send_frame(UDPStreamer *streamer, XImage *frame, uint32_t frame_id);
int udp_send_frame_info(UDPStreamer *streamer, unsigned int width, unsigned int height);
void udp_cleanup(UDPStreamer *streamer);

// Utility functions
void udp_print_status(UDPStreamer *streamer);

#endif // UDP_STREAMER_H