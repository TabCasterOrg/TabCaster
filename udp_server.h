#ifndef UDP_SERVER_H
#define UDP_SERVER_H

#include "display_manager.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#define UDP_BUFFER_SIZE 1024
#define MAX_OUTPUT_NAME 64

// Client connection states
typedef enum {
    CLIENT_STATE_DISCONNECTED,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_RESOLUTION_SET,
    CLIENT_STATE_DISPLAY_READY,
    CLIENT_STATE_STREAMING
} ClientState;

// Client information and resolution request
typedef struct {
    struct sockaddr_in address;
    socklen_t address_len;
    ClientState state;
    
    // Resolution parameters from client
    unsigned int width;
    unsigned int height;
    double refresh_rate;
    bool reduced_blanking;
    
    // Display configuration result
    char output_name[MAX_OUTPUT_NAME];
    RRMode mode_id;
    int x_pos, y_pos;
} ClientInfo;

// UDP Server structure
typedef struct {
    int socket_fd;
    struct sockaddr_in server_addr;
    int port;
    
    // Client management
    ClientInfo client;
    bool client_connected;
    
    // Display manager reference
    DisplayManager *dm;
} UDPServer;

// Core functions
UDPServer* udp_server_init(int port, DisplayManager *dm);
int udp_server_wait_for_client(UDPServer *server);
int udp_server_handle_handshake(UDPServer *server);
int udp_server_create_display_for_client(UDPServer *server, const char *target_output);
int udp_server_send_response(UDPServer *server, const char *message);
int udp_server_send_display_info(UDPServer *server);

ClientInfo* udp_server_get_client(UDPServer *server);
RRMode udp_server_get_created_mode_id(UDPServer *server);
const char* udp_server_get_output_name(UDPServer *server);
void udp_server_cleanup(UDPServer *server);

// Protocol helpers
int udp_server_parse_resolution(const char *message, unsigned int *width, 
                               unsigned int *height, double *refresh_rate);
void udp_server_print_status(UDPServer *server);

#endif // UDP_SERVER_H
