#include "frame_transmitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

// Initialize transmitter
FrameTransmitter* transmitter_init(UDPServer *server) {
    if (!server) return NULL;
    
    FrameTransmitter *tx = calloc(1, sizeof(FrameTransmitter));
    if (!tx) return NULL;
    
    tx->udp_server = server;
    tx->frame_id = 0;
    tx->last_sent_frame_id = 0;
    
    return tx;
}

// Send a keyframe (full PNG frame)
int transmitter_send_keyframe(FrameTransmitter *tx, const unsigned char *png_data, size_t png_size) {
    if (!tx || !png_data || png_size == 0) return -1;
    
    UDPServer *server = tx->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (png_size + data_per_packet - 1) / data_per_packet;
    
    bool send_failed = false;
    for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
        size_t remaining = png_size - (packet_id * data_per_packet);
        size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
        
        char packet[MAX_PACKET_SIZE];
        PacketHeader *header = (PacketHeader*)packet;
        
        header->frame_id = htonl(tx->frame_id);
        header->packet_id = htonl(packet_id);
        header->total_packets = htonl(total_packets);
        header->data_size = htonl(current_data_size);
        
        memcpy(packet + sizeof(PacketHeader), 
               png_data + (packet_id * data_per_packet), 
               current_data_size);
        
        ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
        ssize_t bytes_sent = sendto(server->socket_fd, packet, packet_size, 0,
                                   (struct sockaddr*)&client->address, 
                                   client->address_len);
        
        if (bytes_sent < 0) {
            fprintf(stderr, "Failed to send packet %zu/%zu: %s\n", 
                    packet_id + 1, total_packets, strerror(errno));
            send_failed = true;
            break;
        }
        
        if (total_packets > 100) {
            usleep(5);
        }
    }
    
    if (!send_failed) {
        tx->last_sent_frame_id = tx->frame_id;
    }
    
    return send_failed ? -1 : 0;
}

// Send a delta frame with multiple operations
int transmitter_send_delta(FrameTransmitter *tx, DeltaFrame *delta_frame) {
    if (!tx || !delta_frame || delta_frame->operation_count == 0) return -1;
    
    UDPServer *server = tx->udp_server;
    ClientInfo *client = udp_server_get_client(server);
    
    // Build combined payload with all operations
    unsigned char *payload = (unsigned char*)malloc(delta_frame->total_payload_size);
    if (!payload) {
        fprintf(stderr, "Failed to allocate payload buffer\n");
        return -1;
    }
    
    size_t offset = 0;
    
    // Add each operation to the payload
    for (int i = 0; i < delta_frame->operation_count; i++) {
        DeltaOperation *op = &delta_frame->operations[i];
        
        // Add operation header
        const char *magic = (op->type == OP_CREG) ? "CREG" : "DREG";
        memcpy(payload + offset, magic, 4); offset += 4;
        
        uint16_t nx = htons(op->x);
        uint16_t ny = htons(op->y);
        uint16_t nw = htons(op->width);
        uint16_t nh = htons(op->height);
        memcpy(payload + offset, &nx, 2); offset += 2;
        memcpy(payload + offset, &ny, 2); offset += 2;
        memcpy(payload + offset, &nw, 2); offset += 2;
        memcpy(payload + offset, &nh, 2); offset += 2;
        payload[offset++] = 0; // flags
        payload[offset++] = 90; // quality hint
        uint16_t nres = htons(0);
        memcpy(payload + offset, &nres, 2); offset += 2;
        
        // Add PNG data
        memcpy(payload + offset, op->png_data, op->png_size);
        offset += op->png_size;
    }
    
    // Send payload in packets
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (delta_frame->total_payload_size + data_per_packet - 1) / data_per_packet;
    
    bool send_failed = false;
    for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
        size_t remaining = delta_frame->total_payload_size - (packet_id * data_per_packet);
        size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
        
        char packet[MAX_PACKET_SIZE];
        PacketHeader *header = (PacketHeader*)packet;
        header->frame_id = htonl(delta_frame->frame_id);
        header->packet_id = htonl(packet_id);
        header->total_packets = htonl(total_packets);
        header->data_size = htonl(current_data_size);
        
        memcpy(packet + sizeof(PacketHeader), payload + (packet_id * data_per_packet), current_data_size);
        
        ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
        ssize_t bytes_sent = sendto(server->socket_fd, packet, packet_size, 0,
                                   (struct sockaddr*)&client->address,
                                   client->address_len);
        
        if (bytes_sent < 0) {
            fprintf(stderr, "Failed to send delta frame packet %zu/%zu: %s\n", 
                    packet_id + 1, total_packets, strerror(errno));
            send_failed = true;
            break;
        }
    }
    
    free(payload);
    
    if (!send_failed) {
        tx->last_sent_frame_id = delta_frame->frame_id;
        printf("Sent delta frame with %d operations in %zu packets\n", 
               delta_frame->operation_count, total_packets);
    }
    
    return send_failed ? -1 : 0;
}

// Cleanup transmitter
void transmitter_cleanup(FrameTransmitter *tx) {
    if (!tx) return;
    free(tx);
}

