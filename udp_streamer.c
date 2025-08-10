#include "udp_streamer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

// Initialize UDP streamer
UDPStreamer* udp_init(int port) {
    UDPStreamer *streamer = calloc(1, sizeof(UDPStreamer));
    if (!streamer) return NULL;
    
    streamer->port = port;
    streamer->client_len = sizeof(streamer->client_addr);
    streamer->bytes_per_pixel = 3; // RGB format
    
    // Create UDP socket
    streamer->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (streamer->socket_fd < 0) {
        perror("Socket creation failed");
        free(streamer);
        return NULL;
    }
    
    // Enable address reuse
    int opt = 1;
    if (setsockopt(streamer->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(streamer->socket_fd);
        free(streamer);
        return NULL;
    }
    
    // Configure server address
    memset(&streamer->server_addr, 0, sizeof(streamer->server_addr));
    streamer->server_addr.sin_family = AF_INET;
    streamer->server_addr.sin_addr.s_addr = INADDR_ANY;
    streamer->server_addr.sin_port = htons(port);
    
    // Bind socket
    if (bind(streamer->socket_fd, (struct sockaddr*)&streamer->server_addr, 
             sizeof(streamer->server_addr)) < 0) {
        perror("Bind failed");
        close(streamer->socket_fd);
        free(streamer);
        return NULL;
    }
    
    printf("UDP streamer initialized on port %d\n", port);
    return streamer;
}

// Wait for a client to connect and handle handshake properly
int udp_wait_for_client(UDPStreamer *streamer) {
    if (!streamer) return -1;
    
    printf("Waiting for client connection on port %d...\n", streamer->port);
    
    char buffer[32];
    ssize_t bytes_received = recvfrom(streamer->socket_fd, buffer, sizeof(buffer) - 1, 0,
                                     (struct sockaddr*)&streamer->client_addr, 
                                     &streamer->client_len);
    
    if (bytes_received < 0) {
        perror("Failed to receive from client");
        return -1;
    }
    
    // Null-terminate the received data
    buffer[bytes_received] = '\0';
    
    printf("Received handshake: '%s' from %s:%d\n", 
           buffer,
           inet_ntoa(streamer->client_addr.sin_addr),
           ntohs(streamer->client_addr.sin_port));
    
    // Check if it's a proper handshake
    if (strcmp(buffer, "HELLO") == 0) {
        streamer->client_connected = true;
        
        // Send acknowledgment
        const char *ack = "HELLO_ACK";
        ssize_t bytes_sent = sendto(streamer->socket_fd, ack, strlen(ack), 0,
                                   (struct sockaddr*)&streamer->client_addr, streamer->client_len);
        
        if (bytes_sent < 0) {
            perror("Failed to send handshake acknowledgment");
            return -1;
        }
        
        printf("Client connected and handshake completed: %s:%d\n", 
               inet_ntoa(streamer->client_addr.sin_addr),
               ntohs(streamer->client_addr.sin_port));
        
        return 0;
    } else {
        printf("Invalid handshake received: '%s'\n", buffer);
        return -1;
    }
}

// Send frame information to client
int udp_send_frame_info(UDPStreamer *streamer, unsigned int width, unsigned int height) {
    if (!streamer || !streamer->client_connected) return -1;
    
    streamer->frame_width = width;
    streamer->frame_height = height;
    
    // Send frame info packet
    char info_packet[64];
    snprintf(info_packet, sizeof(info_packet), "INFO:%d:%d", width, height);
    
    ssize_t bytes_sent = sendto(streamer->socket_fd, info_packet, strlen(info_packet), 0,
                               (struct sockaddr*)&streamer->client_addr, streamer->client_len);
    
    if (bytes_sent < 0) {
        perror("Failed to send frame info");
        return -1;
    }
    
    printf("Sent frame info: %dx%d\n", width, height);
    
    // Small delay to ensure client receives frame info before data packets
    usleep(10000); // 10ms delay
    
    return 0;
}

// Convert XImage to RGB data
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

// Send frame data in chunks
int udp_send_frame(UDPStreamer *streamer, XImage *frame, uint32_t frame_id) {
    if (!streamer || !frame || !streamer->client_connected) return -1;
    
    // Calculate frame size
    size_t frame_size = frame->width * frame->height * streamer->bytes_per_pixel;
    size_t data_per_packet = MAX_PACKET_SIZE - sizeof(PacketHeader);
    size_t total_packets = (frame_size + data_per_packet - 1) / data_per_packet;
    
    // Allocate RGB buffer
    unsigned char *rgb_buffer = malloc(frame_size);
    if (!rgb_buffer) {
        fprintf(stderr, "Failed to allocate RGB buffer\n");
        return -1;
    }
    
    // Convert XImage to RGB
    ximage_to_rgb(frame, rgb_buffer);
    
    // Debug info for first frame
    if (frame_id == 0) {
        printf("Sending frame %d: %dx%d (%zu bytes) in %zu packets\n",
               frame_id, frame->width, frame->height, frame_size, total_packets);
    }
    
    // Send packets
    for (size_t packet_id = 0; packet_id < total_packets; packet_id++) {
        // Calculate data size for this packet
        size_t remaining = frame_size - (packet_id * data_per_packet);
        size_t current_data_size = (remaining > data_per_packet) ? data_per_packet : remaining;
        
        // Create packet
        char packet[MAX_PACKET_SIZE];
        PacketHeader *header = (PacketHeader*)packet;
        
        header->frame_id = htonl(frame_id);
        header->packet_id = htonl(packet_id);
        header->total_packets = htonl(total_packets);
        header->data_size = htonl(current_data_size);
        
        // Copy data
        memcpy(packet + sizeof(PacketHeader), 
               rgb_buffer + (packet_id * data_per_packet), 
               current_data_size);
        
        // Send packet
        ssize_t packet_size = sizeof(PacketHeader) + current_data_size;
        ssize_t bytes_sent = sendto(streamer->socket_fd, packet, packet_size, 0,
                                   (struct sockaddr*)&streamer->client_addr, 
                                   streamer->client_len);
        
        if (bytes_sent < 0) {
            perror("Failed to send packet");
            free(rgb_buffer);
            return -1;
        }
        
        // Verify all bytes were sent
        if (bytes_sent != packet_size) {
            fprintf(stderr, "Partial packet sent: %zd/%zd bytes\n", bytes_sent, packet_size);
        }
        
        // Small delay between packets to avoid overwhelming the network
        usleep(50); // 0.05ms delay (reduced from 100μs)
    }
    
    free(rgb_buffer);
    
    // Print progress for every 10th frame
    if (frame_id % 10 == 0) {
        printf("Sent frame %d (%zu packets)\n", frame_id, total_packets);
    }
    
    return 0;
}

// Print streamer status
void udp_print_status(UDPStreamer *streamer) {
    if (!streamer) return;
    
    printf("UDP Streamer Status:\n");
    printf("  Port: %d\n", streamer->port);
    printf("  Socket FD: %d\n", streamer->socket_fd);
    printf("  Client connected: %s\n", streamer->client_connected ? "YES" : "NO");
    
    if (streamer->client_connected) {
        printf("  Client: %s:%d\n", 
               inet_ntoa(streamer->client_addr.sin_addr),
               ntohs(streamer->client_addr.sin_port));
        printf("  Frame size: %dx%d (%d bytes per pixel)\n", 
               streamer->frame_width, streamer->frame_height, streamer->bytes_per_pixel);
    }
}

// Cleanup resources
void udp_cleanup(UDPStreamer *streamer) {
    if (!streamer) return;
    
    if (streamer->socket_fd >= 0) {
        close(streamer->socket_fd);
    }
    
    free(streamer);
}