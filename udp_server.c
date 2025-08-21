#include "udp_server.h"
#include "mode_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

// Fallback resolutions in order of preference
static const struct {
    unsigned int width;
    unsigned int height;
    double refresh_rate;
} fallback_resolutions[] = {
    {1920, 1080, 60.0},   // Full HD
    {1680, 1050, 60.0},   // WSXGA+
    {1600, 900, 60.0},    // HD+
    {1440, 900, 60.0},    // WXGA+
    {1366, 768, 60.0},    // HD
    {1280, 1024, 60.0},   // SXGA
    {1280, 720, 60.0},    // HD 720p
    {1024, 768, 60.0},    // XGA
    {800, 600, 60.0},     // SVGA
};

static const size_t num_fallback_resolutions = sizeof(fallback_resolutions) / sizeof(fallback_resolutions[0]);

// Initialize UDP server
UDPServer* udp_server_init(int port, DisplayManager *dm) {
    if (!dm) return NULL;
    
    UDPServer *server = calloc(1, sizeof(UDPServer));
    if (!server) return NULL;
    
    server->port = port;
    server->dm = dm;
    server->client.address_len = sizeof(server->client.address);
    server->client.state = CLIENT_STATE_DISCONNECTED;
    
    // Create UDP socket
    server->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->socket_fd < 0) {
        perror("Socket creation failed");
        free(server);
        return NULL;
    }
    
    // Enable address reuse
    int opt = 1;
    if (setsockopt(server->socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server->socket_fd);
        free(server);
        return NULL;
    }
    
    // Configure server address
    memset(&server->server_addr, 0, sizeof(server->server_addr));
    server->server_addr.sin_family = AF_INET;
    server->server_addr.sin_addr.s_addr = INADDR_ANY;
    server->server_addr.sin_port = htons(port);
    
    // Bind socket
    if (bind(server->socket_fd, (struct sockaddr*)&server->server_addr, 
             sizeof(server->server_addr)) < 0) {
        perror("Bind failed");
        close(server->socket_fd);
        free(server);
        return NULL;
    }
    
    printf("UDP server initialized on port %d\n", port);
    return server;
}

// Send response to client
int udp_server_send_response(UDPServer *server, const char *message) {
    if (!server || !message || !server->client_connected) return -1;
    
    ssize_t bytes_sent = sendto(server->socket_fd, message, strlen(message), 0,
                               (struct sockaddr*)&server->client.address,
                               server->client.address_len);
    
    if (bytes_sent < 0) {
        perror("Failed to send response");
        return -1;
    }
    
    printf("Sent: %s\n", message);
    return 0;
}

// Parse resolution message: "RESOLUTION:1920:1080:60"
int udp_server_parse_resolution(const char *message, unsigned int *width, 
                               unsigned int *height, double *refresh_rate) {
    if (!message || !width || !height || !refresh_rate) return -1;
    
    if (strncmp(message, "RESOLUTION:", 11) != 0) return -1;
    
    const char *data = message + 11;  // Skip "RESOLUTION:"
    int parsed = sscanf(data, "%u:%u:%lf", width, height, refresh_rate);
    
    if (parsed != 3) {
        fprintf(stderr, "Invalid resolution format: %s\n", message);
        return -1;
    }
    
    // Basic validation
    if (*width < 640 || *width > 32767 || *height < 480 || *height > 32767) {
        fprintf(stderr, "Invalid resolution: %ux%u\n", *width, *height);
        return -1;
    }
    
    if (*refresh_rate <= 0 || *refresh_rate > 240) {
        fprintf(stderr, "Invalid refresh rate: %.2f\n", *refresh_rate);
        return -1;
    }
    
    return 0;
}

// Try to create display with a specific resolution
int udp_server_try_resolution(UDPServer *server, const char *target_output,
                             unsigned int width, unsigned int height, double refresh_rate) {
    if (!server || !target_output) return -1;
    
    ClientInfo *client = &server->client;
    
    printf("Attempting resolution: %ux%u @ %.2f Hz\n", width, height, refresh_rate);
    
    // Step 1: Create CVT mode
    RRMode mode_id = mode_create_cvt(server->dm, width, height, refresh_rate, 
                                    client->reduced_blanking);
    
    if (mode_id == 0) {
        printf("  ✗ Failed to create CVT mode\n");
        return -1;
    }
    
    printf("  ✓ CVT mode created (ID: %lu)\n", mode_id);
    
    // Step 2: Add mode to output
    if (mode_add_to_output(server->dm, target_output, mode_id) != 0) {
        printf("  ✗ Failed to add mode to output\n");
        mode_delete_from_xrandr(server->dm, mode_id); // Cleanup
        return -1;
    }
    
    printf("  ✓ Mode added to output with ID: %lu\n", mode_id);
    
    // Step 3: Enable output with mode
    if (mode_enable_output_with_mode_id(server->dm, target_output, mode_id,
                                       client->x_pos, client->y_pos) != 0) {
        printf("  ✗ Failed to enable output with mode\n");
        mode_remove_from_output(server->dm, target_output, mode_id);
        mode_delete_from_xrandr(server->dm, mode_id);
        return -1;
    }
    
    printf("  ✓ Output enabled successfully\n");
    
    // Store successful configuration
    strncpy(client->output_name, target_output, sizeof(client->output_name) - 1);
    client->mode_id = mode_id;
    client->width = width;    // Update to actual resolution used
    client->height = height;
    client->refresh_rate = refresh_rate;
    client->state = CLIENT_STATE_DISPLAY_READY;
    
    return 0;
}

// Create display for client with fallback resolution support
int udp_server_create_display_for_client(UDPServer *server, const char *target_output) {
    if (!server || !target_output || server->client.state != CLIENT_STATE_RESOLUTION_SET) {
        return -1;
    }
    
    ClientInfo *client = &server->client;
    
    printf("\n=== Creating display for client ===\n");
    printf("Target output: %s\n", target_output);
    printf("Requested resolution: %ux%u @ %.2f Hz\n", 
           client->width, client->height, client->refresh_rate);
    
    // First, try the client's requested resolution
    printf("\n--- Trying client's requested resolution ---\n");
    if (udp_server_try_resolution(server, target_output, 
                                 client->width, client->height, client->refresh_rate) == 0) {
        printf("✓ Client's requested resolution works!\n");
        dm_get_screens(server->dm);
        return udp_server_send_display_info(server);
    }
    
    printf("✗ Client's requested resolution failed\n");
    
    // If that fails, try fallback resolutions
    printf("\n--- Trying fallback resolutions ---\n");
    
    for (size_t i = 0; i < num_fallback_resolutions; i++) {
        unsigned int width = fallback_resolutions[i].width;
        unsigned int height = fallback_resolutions[i].height;
        double refresh_rate = fallback_resolutions[i].refresh_rate;
        printf("Trying resolution: %ux%u",width,height);
        bool validRefreshRate = fabs(refresh_rate - client->refresh_rate) < 0.1;
        printf("The refresh rate is valid: %u", validRefreshRate);
        // Skip if it's the same as what we already tried
        if (width == client->width && height == client->height && validRefreshRate) {
            printf("Skipping %ux%u @ %.1f Hz (already tried)\n", width, height, refresh_rate);
            continue;
        }
        
        printf("\nFallback %zu/%zu: ", i + 1, num_fallback_resolutions);
        if (udp_server_try_resolution(server, target_output, width, height, refresh_rate) == 0) {
            printf("✓ Fallback resolution %ux%u @ %.1f Hz works!\n", width, height, refresh_rate);
            
            // Refresh display manager state
            dm_get_screens(server->dm);
            
            // Send resolution change notification to client
            char resolution_change[256];
            snprintf(resolution_change, sizeof(resolution_change), 
                    "RESOLUTION_CHANGED:%ux%u:%.1f", width, height, refresh_rate);
            
            if (udp_server_send_response(server, resolution_change) != 0) {
                return -1;
            }
            
            return udp_server_send_display_info(server);
        }
    }
    
    // All resolutions failed
    printf("\n✗ All resolutions failed - graphics card may not support any of these modes\n");
    printf("Fallback resolutions tried:\n");
    for (size_t i = 0; i < num_fallback_resolutions; i++) {
        printf("  - %ux%u @ %.1f Hz\n", 
               fallback_resolutions[i].width, 
               fallback_resolutions[i].height,
               fallback_resolutions[i].refresh_rate);
    }
    
    udp_server_send_response(server, "DISPLAY_ERROR:All resolutions failed - graphics card incompatible");
    return -1;
}

// Handle complete handshake process
int udp_server_handle_handshake(UDPServer *server) {
    if (!server) return -1;
    
    char buffer[UDP_BUFFER_SIZE];
    
    while (server->client.state != CLIENT_STATE_DISPLAY_READY) {
        // Receive message from client
        ssize_t bytes_received = recvfrom(server->socket_fd, buffer, sizeof(buffer) - 1, 0,
                                         (struct sockaddr*)&server->client.address,
                                         &server->client.address_len);
        
        if (bytes_received < 0) {
            perror("Failed to receive from client");
            return -1;
        }
        
        buffer[bytes_received] = '\0';
        printf("Received: %s\n", buffer);
        
        switch (server->client.state) {
            case CLIENT_STATE_DISCONNECTED:
                if (strcmp(buffer, "HELLO") == 0) {
                    server->client_connected = true;
                    server->client.state = CLIENT_STATE_CONNECTED;
                    
                    if (udp_server_send_response(server, "HELLO_ACK") != 0) {
                        return -1;
                    }
                    
                    printf("Client connected: %s:%d\n",
                           inet_ntoa(server->client.address.sin_addr),
                           ntohs(server->client.address.sin_port));
                } else {
                    printf("Invalid handshake: %s\n", buffer);
                    return -1;
                }
                break;
                
            case CLIENT_STATE_CONNECTED:
                if (udp_server_parse_resolution(buffer, &server->client.width,
                                               &server->client.height,
                                               &server->client.refresh_rate) == 0) {
                    server->client.state = CLIENT_STATE_RESOLUTION_SET;
                    
                    printf("Client resolution: %ux%u @ %.2f Hz\n",
                           server->client.width, server->client.height,
                           server->client.refresh_rate);
                    
                    if (udp_server_send_response(server, "RESOLUTION_ACK") != 0) {
                        return -1;
                    }
                    
                    // Immediately try to create display
                    return 0; // Let caller handle display creation
                } else {
                    udp_server_send_response(server, "RESOLUTION_ERROR:Invalid format");
                    return -1;
                }
                break;
                
            default:
                printf("Unexpected message in state %d: %s\n", server->client.state, buffer);
                break;
        }
    }
    
    return 0;
}

// Send display info to client
int udp_server_send_display_info(UDPServer *server) {
    if (!server || server->client.state != CLIENT_STATE_DISPLAY_READY) return -1;
    
    ClientInfo *client = &server->client;
    char response[256];
    
    snprintf(response, sizeof(response), "DISPLAY_READY:%s:%ux%u:%d:%d",
             client->output_name, client->width, client->height,
             client->x_pos, client->y_pos);
    
    return udp_server_send_response(server, response);
}

// Wait for client and complete handshake
int udp_server_wait_for_client(UDPServer *server) {
    if (!server) return -1;
    
    printf("Waiting for client connection on port %d...\n", server->port);
    return udp_server_handle_handshake(server);
}

// Get client info
ClientInfo* udp_server_get_client(UDPServer *server) {
    return server ? &server->client : NULL;
}

// Get the mode ID that was created for the client
RRMode udp_server_get_created_mode_id(UDPServer *server) {

    
    if (server->client.state < CLIENT_STATE_DISPLAY_READY) {
        fprintf(stderr, "udp_server_get_created_mode_id: client not in ready state (%d)\n", 
                server->client.state);
        return 0;
    }
    
    return server->client.mode_id;
}

// Get the mode name that was created for the client

const char* udp_server_get_output_name(UDPServer *server) {

    
    if (server->client.state < CLIENT_STATE_DISPLAY_READY) {
        fprintf(stderr, "udp_server_get_output_name: client not in ready state (%d)\n", 
                server->client.state);
        return NULL;
    }
    
    return server->client.output_name;
}

// Print server status
void udp_server_print_status(UDPServer *server) {
    if (!server) return;
    
    printf("UDP Server Status:\n");
    printf("  Port: %d\n", server->port);
    printf("  Socket FD: %d\n", server->socket_fd);
    printf("  Client connected: %s\n", server->client_connected ? "YES" : "NO");
    
    if (server->client_connected) {
        ClientInfo *client = &server->client;
        printf("  Client: %s:%d\n",
               inet_ntoa(client->address.sin_addr),
               ntohs(client->address.sin_port));
        printf("  State: %d\n", client->state);
        
        if (client->state >= CLIENT_STATE_RESOLUTION_SET) {
            printf("  Resolution: %ux%u @ %.2f Hz\n",
                   client->width, client->height, client->refresh_rate);
        }
        
        if (client->state >= CLIENT_STATE_DISPLAY_READY) {
            printf("  Output: %s (Mode ID: %lu)\n", client->output_name, client->mode_id);
            printf("  Position: %d,%d\n", client->x_pos, client->y_pos);
        }
    }
}

// Cleanup resources
void udp_server_cleanup(UDPServer *server) {
    if (!server) return;
    
    if (server->socket_fd >= 0) {
        close(server->socket_fd);
    }
    
    free(server);
}
