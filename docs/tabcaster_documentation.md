# Tabcaster - Complete API Documentation

## Table of Contents

1. [Overview](#overview)
2. [Display Manager Module](#display-manager-module)
3. [Mode Manager Module](#mode-manager-module)
4. [Frame Capture Module](#frame-capture-module)
5. [UDP Streamer Module](#udp-streamer-module)
6. [Main Application](#main-application)
7. [Building and Installation](#building-and-installation)
8. [Usage Examples](#usage-examples)
9. [Troubleshooting](#troubleshooting)

## Overview

Tabcaster is a comprehensive C implementation for X11 display management, virtual display creation, screen capture, and real-time streaming. It provides direct API access to X11/XRandR for high-performance display operations without relying on external command-line tools.

### Key Features

- **Display Management**: Query, configure, and manage all display outputs (connected and disconnected)
- **Custom Mode Creation**: Generate CVT timing modes for any resolution using libxcvt
- **Virtual Displays**: Create and manage virtual displays for headless scenarios
- **Real-time Capture**: High-performance screen capture using XGetImage
- **UDP Streaming**: Real-time frame streaming over UDP with packet fragmentation
- **Multi-monitor Support**: Full multi-monitor configuration and positioning

### Dependencies

- **Xlib**: Core X11 library
- **XRandR**: X11 extension for display configuration
- **libxcvt**: CVT timing calculation library
- **POSIX**: Socket operations and threading



---

## Display Manager Module

The display manager provides core X11 connectivity and display enumeration functionality.

### Data Structures

#### `ScreenInfo`
```c
typedef struct {
    char name[32];          // Output name (e.g., "HDMI-1", "eDP-1")
    unsigned int width;     // Current width in pixels (0 if disconnected)
    unsigned int height;    // Current height in pixels (0 if disconnected)  
    int x;                  // X position on virtual desktop
    int y;                  // Y position on virtual desktop
    bool connected;         // Physical connection status
    bool primary;           // Primary display flag
    RROutput output_id;     // XRandR output identifier
    RRCrtc crtc_id;        // XRandR CRTC identifier (0 if disconnected)
} ScreenInfo;
```

#### `DisplayManager`
```c
typedef struct {
    Display *display;              // X11 display connection
    Window root;                   // Root window handle
    int screen;                    // Default screen number
    XRRScreenResources *resources; // XRandR screen resources
    ScreenInfo *screens;           // Array of ALL screen information
    int screen_count;              // Total number of outputs
} DisplayManager;
```

### Core Functions

#### `DisplayManager* dm_init(void)`

**Purpose**: Initialize display manager and establish X11 connection.

**Parameters**: None

**Returns**: 
- `DisplayManager*`: Initialized display manager on success
- `NULL`: On failure (cannot connect to X server, XRandR unavailable)

**Example**:
```c
DisplayManager *dm = dm_init();
if (!dm) {
    fprintf(stderr, "Failed to initialize display manager\n");
    return 1;
}
```

**Error Conditions**:
- X server not running
- `DISPLAY` environment variable not set
- XRandR extension not available
- Memory allocation failure

---

#### `int dm_get_screens(DisplayManager *dm)`

**Purpose**: Query and populate information about ALL display outputs (connected and disconnected).

**Parameters**: 
- `dm`: Initialized DisplayManager pointer

**Returns**:
- `int >= 0`: Number of connected displays (for convenience)
- `-1`: Error occurred

**Side Effects**: 
- Populates `dm->screens` array
- Sets `dm->screen_count` to total number of outputs

**Example**:
```c
int connected_count = dm_get_screens(dm);
if (connected_count < 0) {
    fprintf(stderr, "Failed to get screen information\n");
    return 1;
}
printf("Found %d connected outputs out of %d total\n", 
       connected_count, dm->screen_count);
```

**Output Structure**: After calling, each `ScreenInfo` contains:
- **Connected displays**: Full geometry, CRTC assignment, primary status
- **Disconnected displays**: Name and output ID only, zero geometry

---

#### `void dm_print_screens(DisplayManager *dm)`

**Purpose**: Print human-readable summary of all displays.

**Parameters**:
- `dm`: DisplayManager with populated screen information

**Returns**: None (prints to stdout)

**Example Output**:
```
All outputs:
  eDP-1: 1920x1080+0+0 (primary) [CONNECTED]
  HDMI-1: 2336x1080+1920+0 [CONNECTED]
  DP-1: [DISCONNECTED]
  VGA-1: [DISCONNECTED]
```

**Example Usage**:
```c
dm_get_screens(dm);  // Must call this first
dm_print_screens(dm);
```

---

#### `void dm_cleanup(DisplayManager *dm)`

**Purpose**: Free all resources and close X11 connection.

**Parameters**:
- `dm`: DisplayManager to clean up (safe to pass NULL)

**Returns**: None

**Cleanup Actions**:
- Frees screen array
- Releases XRandR resources
- Closes X11 display connection
- Frees DisplayManager structure

**Example**:
```c
// Always call at program exit
dm_cleanup(dm);
dm = NULL;  // Good practice
```

### Utility Functions

#### `int dm_count_connected_screens(DisplayManager *dm)`

**Purpose**: Count currently connected displays.

**Parameters**:
- `dm`: DisplayManager with populated screen information

**Returns**:
- `int >= 0`: Number of connected displays
- `0`: No connected displays or invalid input

**Example**:
```c
int connected = dm_count_connected_screens(dm);
printf("Active displays: %d\n", connected);
```

---

#### `int dm_count_disconnected_screens(DisplayManager *dm)`

**Purpose**: Count currently disconnected displays.

**Parameters**:
- `dm`: DisplayManager with populated screen information

**Returns**:
- `int >= 0`: Number of disconnected displays
- `0`: All displays connected or invalid input

**Example**:
```c
int disconnected = dm_count_disconnected_screens(dm);
printf("Available but unused outputs: %d\n", disconnected);
```

---

#### `ScreenInfo* dm_get_primary_screen(DisplayManager *dm)`

**Purpose**: Find the primary display.

**Parameters**:
- `dm`: DisplayManager with populated screen information

**Returns**:
- `ScreenInfo*`: Pointer to primary display info
- `NULL`: No primary display found or invalid input

**Example**:
```c
ScreenInfo *primary = dm_get_primary_screen(dm);
if (primary) {
    printf("Primary: %s (%dx%d)\n", 
           primary->name, primary->width, primary->height);
} else {
    printf("No primary display configured\n");
}
```

---

## Mode Manager Module

The mode manager handles custom display mode creation and output configuration using XRandR and libxcvt.

### Core Functions

#### `RRMode mode_create_cvt(DisplayManager *dm, unsigned int width, unsigned int height, double refresh_rate, bool reduced_blanking)`

**Purpose**: Create a new CVT (Coordinated Video Timings) mode using libxcvt.

**Parameters**:
- `dm`: Initialized DisplayManager
- `width`: Horizontal resolution in pixels (1-32767)
- `height`: Vertical resolution in pixels (1-32767)
- `refresh_rate`: Refresh rate in Hz (e.g., 60.0, 59.93)
- `reduced_blanking`: Use reduced blanking timings (saves bandwidth)

**Returns**:
- `RRMode != 0`: XRandR mode ID on success
- `0`: Failed to create mode

**Example**:
```c
// Create 2336x1080 at 60Hz with reduced blanking
RRMode mode_id = mode_create_cvt(dm, 2336, 1080, 60.0, true);
if (mode_id != 0) {
    printf("Created mode with ID: %lu\n", mode_id);
} else {
    fprintf(stderr, "Failed to create CVT mode\n");
}
```

**Console Output Example**:
```
Generated CVT mode:
# 2336x1080 60.00 Hz (CVT) hsync: 66.62 kHz; pclk: 196.520 MHz
Modeline "2336x1080_60.00" 196.520 2336 2352 2384 2448 1080 1083 1093 1111 +hsync -vsync
Created mode with ID: 524
```

---

#### `int mode_add_to_output(DisplayManager *dm, const char *output_name, RRMode mode_id)`

**Purpose**: Add an existing mode to a specific output's available modes list.

**Parameters**:
- `dm`: DisplayManager
- `output_name`: Output name (e.g., "HDMI-1", "DP-2")
- `mode_id`: XRandR mode ID to add

**Returns**:
- `0`: Success
- `-1`: Error (output not found, invalid mode ID)

**Example**:
```c
// Add mode to HDMI-1 output
if (mode_add_to_output(dm, "HDMI-1", mode_id) == 0) {
    printf("Mode added to HDMI-1\n");
} else {
    fprintf(stderr, "Failed to add mode to output\n");
}
```

---

#### `int mode_enable_output_with_mode(DisplayManager *dm, const char *output_name, const char *mode_name, int x_pos, int y_pos)`

**Purpose**: Enable output with a specific mode name and position.

**Parameters**:
- `dm`: DisplayManager
- `output_name`: Output to enable (e.g., "HDMI-1")
- `mode_name`: Mode name (e.g., "2336x1080_60.00")
- `x_pos`: X position in virtual desktop
- `y_pos`: Y position in virtual desktop

**Returns**:
- `0`: Success
- `-1`: Error (output/mode not found, no available CRTC)

**Example**:
```c
// Enable HDMI-1 with custom mode to the right of primary display
int result = mode_enable_output_with_mode(dm, "HDMI-1", 
                                         "2336x1080_60.00", 
                                         1920, 0);
if (result == 0) {
    printf("HDMI-1 enabled successfully\n");
}
```

---

#### `int mode_disable_output(DisplayManager *dm, const char *output_name)`

**Purpose**: Disable (turn off) a specific output.

**Parameters**:
- `dm`: DisplayManager
- `output_name`: Output to disable

**Returns**:
- `0`: Success
- `-1`: Error (output not found)

**Example**:
```c
// Turn off HDMI-1
if (mode_disable_output(dm, "HDMI-1") == 0) {
    printf("HDMI-1 disabled\n");
} else {
    fprintf(stderr, "Failed to disable HDMI-1\n");
}
```

---

#### `void mode_print_output_modes(DisplayManager *dm, const char *output_name)`

**Purpose**: List all available modes for a specific output.

**Parameters**:
- `dm`: DisplayManager
- `output_name`: Output to query

**Returns**: None (prints to stdout)

**Example Output**:
```
Available modes for output 'HDMI-1':
  1920x1080_60.00 (1920x1080 @ 60.00 Hz) [ID: 123]
  2336x1080_60.00 (2336x1080 @ 60.00 Hz) [ID: 524]
  1024x768_60.00 (1024x768 @ 60.00 Hz) [ID: 125]
```

**Example Usage**:
```c
mode_print_output_modes(dm, "HDMI-1");
```

---

#### `bool mode_is_output_enabled(DisplayManager *dm, const char *output_name)`

**Purpose**: Check if an output is currently enabled (has active CRTC).

**Parameters**:
- `dm`: DisplayManager  
- `output_name`: Output to check

**Returns**:
- `true`: Output is enabled
- `false`: Output is disabled or not found

**Example**:
```c
if (mode_is_output_enabled(dm, "HDMI-1")) {
    printf("HDMI-1 is currently active\n");
} else {
    printf("HDMI-1 is disabled\n");
}
```

---

#### `int mode_get_output_config(DisplayManager *dm, const char *output_name, RRMode *current_mode, int *x, int *y, unsigned int *width, unsigned int *height)`

**Purpose**: Get current configuration of an enabled output.

**Parameters**:
- `dm`: DisplayManager
- `output_name`: Output to query
- `current_mode`: (out) Current mode ID
- `x`, `y`: (out) Position coordinates
- `width`, `height`: (out) Current resolution

**Returns**:
- `0`: Success
- `-1`: Error (output not found or not enabled)

**Example**:
```c
RRMode current_mode;
int x, y;
unsigned int width, height;

if (mode_get_output_config(dm, "HDMI-1", &current_mode, 
                          &x, &y, &width, &height) == 0) {
    printf("HDMI-1: Mode %lu, %ux%u at %d,%d\n", 
           current_mode, width, height, x, y);
} else {
    printf("HDMI-1 is not currently enabled\n");
}
```

---

## Frame Capture Module

The frame capture module provides high-performance screen capture using XGetImage with rate limiting and format conversion.

### Data Structures

#### `FrameCapture`
```c
typedef struct {
    DisplayManager *dm;       // Reference to display manager
    
    // Target screen region
    int x, y;                // Capture area position
    unsigned int width, height; // Capture area size
    char output_name[32];    // Source output name
    
    // Frame timing
    int target_fps;          // Target frames per second
    long frame_interval_us;  // Microseconds between frames
    struct timeval last_capture; // Last capture timestamp
    
    // Current frame data
    XImage *current_frame;   // Current captured frame
    bool frame_ready;        // New frame available flag
    bool capturing;          // Capture active state
} FrameCapture;
```

### Core Functions

#### `FrameCapture* fc_init(DisplayManager *dm, const char *output_name, int fps)`

**Purpose**: Initialize frame capture for a specific output.

**Parameters**:
- `dm`: DisplayManager with populated screen information
- `output_name`: Output to capture from (e.g., "HDMI-1")
- `fps`: Target frame rate (1-240, defaults to 30)

**Returns**:
- `FrameCapture*`: Initialized capture context
- `NULL`: Error (output not found, invalid parameters)

**Example**:
```c
// Initialize capture for HDMI-1 at 60 FPS
FrameCapture *fc = fc_init(dm, "HDMI-1", 60);
if (!fc) {
    fprintf(stderr, "Failed to initialize capture for HDMI-1\n");
    return 1;
}
```

**Console Output Example**:
```
Found connected output 'HDMI-1'
Capture initialized for 'HDMI-1': 2336x1080+1920+0 @ 60 fps
```

**Special Cases**:
- Works with virtual/disconnected displays that have active modes
- Automatically creates `captures/` directory
- Validates capture dimensions (must be > 0)

---

#### `int fc_start(FrameCapture *fc)`

**Purpose**: Start the capture process.

**Parameters**:
- `fc`: Initialized FrameCapture

**Returns**:
- `0`: Success
- `-1`: Error (invalid parameter)

**Example**:
```c
if (fc_start(fc) != 0) {
    fprintf(stderr, "Failed to start capture\n");
    return 1;
}
```

---

#### `int fc_capture_frame(FrameCapture *fc)`

**Purpose**: Capture a single frame with rate limiting.

**Parameters**:
- `fc`: Active FrameCapture

**Returns**:
- `1`: New frame captured successfully
- `0`: Too soon for next frame (rate limited)
- `-1`: Error occurred

**Example**:
```c
// Typical capture loop
while (keep_running) {
    int result = fc_capture_frame(fc);
    
    if (result == 1) {
        // New frame available
        XImage *frame = fc_get_frame(fc);
        if (frame) {
            // Process frame...
            fc_mark_frame_processed(fc);
        }
    } else if (result < 0) {
        fprintf(stderr, "Capture failed\n");
        break;
    }
    // result == 0: waiting for next frame interval
    
    usleep(5000); // 5ms sleep to prevent busy waiting
}
```

**Rate Limiting**: Automatically enforces the target FPS by checking elapsed time since last capture.

---

#### `XImage* fc_get_frame(FrameCapture *fc)`

**Purpose**: Get pointer to current captured frame.

**Parameters**:
- `fc`: FrameCapture with captured frame

**Returns**:
- `XImage*`: Current frame data
- `NULL`: No frame available or invalid parameter

**Example**:
```c
XImage *frame = fc_get_frame(fc);
if (frame) {
    printf("Frame: %dx%d, %d bits per pixel\n", 
           frame->width, frame->height, frame->bits_per_pixel);
}
```

---

#### `bool fc_has_new_frame(FrameCapture *fc)`

**Purpose**: Check if a new frame is ready for processing.

**Parameters**:
- `fc`: FrameCapture

**Returns**:
- `true`: New frame available
- `false`: No new frame or invalid parameter

**Example**:
```c
if (fc_has_new_frame(fc)) {
    // Process the frame
    XImage *frame = fc_get_frame(fc);
    // ... processing ...
    fc_mark_frame_processed(fc);
}
```

---

#### `void fc_mark_frame_processed(FrameCapture *fc)`

**Purpose**: Mark current frame as processed (clears new frame flag).

**Parameters**:
- `fc`: FrameCapture

**Returns**: None

**Example**:
```c
// After processing frame
fc_mark_frame_processed(fc);
```

---

#### `int fc_save_frame_ppm(FrameCapture *fc, const char *filename)`

**Purpose**: Save current frame as PPM image file in captures directory.

**Parameters**:
- `fc`: FrameCapture with current frame
- `filename`: Output filename (e.g., "frame_001.ppm")

**Returns**:
- `0`: Success
- `-1`: Error (no frame, file error)

**Example**:
```c
// Save frame with timestamp
char filename[64];
snprintf(filename, sizeof(filename), "capture_%ld.ppm", time(NULL));

if (fc_save_frame_ppm(fc, filename) == 0) {
    printf("Frame saved to captures/%s\n", filename);
} else {
    fprintf(stderr, "Failed to save frame\n");
}
```

**Output Format**: PPM P6 format (binary RGB)
**File Location**: Always saved in `./captures/` directory

---

#### `void fc_print_frame_info(FrameCapture *fc)`

**Purpose**: Print detailed information about capture status and current frame.

**Parameters**:
- `fc`: FrameCapture

**Returns**: None (prints to stdout)

**Example Output**:
```
Capture Status for 'HDMI-1':
  Screen region: 2336x1080+1920+0
  Target FPS: 60 (interval: 16666 μs)
  Capturing: YES
  Frame ready: YES
  Current frame:
    Dimensions: 2336x1080
    Depth: 24 bits
    Bits per pixel: 32
    Bytes per line: 9344
    Format: ZPixmap
    Byte order: LSBFirst
```

**Example Usage**:
```c
fc_print_frame_info(fc);  // Debug information
```

---

#### `void fc_cleanup(FrameCapture *fc)`

**Purpose**: Clean up all capture resources.

**Parameters**:
- `fc`: FrameCapture to clean up (safe to pass NULL)

**Returns**: None

**Cleanup Actions**:
- Stops capture if active
- Destroys current XImage (frees image data)
- Frees FrameCapture structure

**Example**:
```c
fc_cleanup(fc);
fc = NULL;  // Good practice
```

---

## UDP Streamer Module

The UDP streamer provides real-time frame streaming over UDP with packet fragmentation and handshake protocol.

### Data Structures

#### `UDPStreamer`
```c
typedef struct {
    int socket_fd;                  // UDP socket file descriptor
    struct sockaddr_in server_addr; // Server bind address
    struct sockaddr_in client_addr; // Connected client address
    socklen_t client_len;          // Client address length
    bool client_connected;         // Client connection status
    int port;                      // Server port
    
    // Frame streaming parameters
    unsigned int frame_width;      // Current frame width
    unsigned int frame_height;     // Current frame height
    unsigned int bytes_per_pixel;  // Bytes per pixel (RGB = 3)
} UDPStreamer;
```

#### `PacketHeader`
```c
typedef struct __attribute__((packed)) {
    uint32_t frame_id;      // Frame sequence number
    uint32_t packet_id;     // Packet sequence in this frame
    uint32_t total_packets; // Total packets for this frame
    uint32_t data_size;     // Size of data in this packet
} PacketHeader;
```

### Core Functions

#### `UDPStreamer* udp_init(int port)`

**Purpose**: Initialize UDP streamer server on specified port.

**Parameters**:
- `port`: UDP port to bind (1024-65535)

**Returns**:
- `UDPStreamer*`: Initialized streamer
- `NULL`: Error (socket creation failed, bind failed)

**Example**:
```c
UDPStreamer *streamer = udp_init(8888);
if (!streamer) {
    fprintf(stderr, "Failed to initialize UDP streamer\n");
    return 1;
}
```

**Console Output**:
```
UDP streamer initialized on port 8888
```

**Socket Configuration**:
- Uses `SO_REUSEADDR` for quick restart
- Binds to `INADDR_ANY` (all interfaces)
- Non-blocking operation

---

#### `int udp_wait_for_client(UDPStreamer *streamer)`

**Purpose**: Wait for client connection and perform handshake.

**Parameters**:
- `streamer`: Initialized UDPStreamer

**Returns**:
- `0`: Client connected successfully
- `-1`: Error (handshake failed, network error)

**Handshake Protocol**:
1. Client sends `"HELLO"` message
2. Server validates message
3. Server responds with `"HELLO_ACK"`
4. Connection established

**Example**:
```c
printf("Waiting for client...\n");
if (udp_wait_for_client(streamer) != 0) {
    fprintf(stderr, "Failed to connect to client\n");
    return 1;
}
printf("Client connected!\n");
```

**Console Output**:
```
Waiting for client connection on port 8888...
Received handshake: 'HELLO' from 192.168.1.100:54321
Client connected and handshake completed: 192.168.1.100:54321
```

---

#### `int udp_send_frame_info(UDPStreamer *streamer, unsigned int width, unsigned int height)`

**Purpose**: Send frame dimensions to connected client.

**Parameters**:
- `streamer`: Connected UDPStreamer
- `width`: Frame width in pixels
- `height`: Frame height in pixels

**Returns**:
- `0`: Success
- `-1`: Error (not connected, send failed)

**Protocol**: Sends `"INFO:width:height"` packet

**Example**:
```c
if (udp_send_frame_info(streamer, 2336, 1080) != 0) {
    fprintf(stderr, "Failed to send frame info\n");
}
```

**Console Output**:
```
Sent frame info: 2336x1080
```

---

#### `int udp_send_frame(UDPStreamer *streamer, XImage *frame, uint32_t frame_id)`

**Purpose**: Send complete frame data as fragmented UDP packets.

**Parameters**:
- `streamer`: Connected UDPStreamer
- `frame`: XImage frame data to send
- `frame_id`: Sequential frame identifier

**Returns**:
- `0`: Success (all packets sent)
- `-1`: Error (not connected, send failed, memory allocation failed)

**Fragmentation Details**:
- Maximum packet size: 1400 bytes (safe for most networks)
- Packet header: 16 bytes
- Data per packet: ~1384 bytes
- Automatic RGB conversion from XImage
- Network byte order for header fields

**Example**:
```c
// In streaming loop
uint32_t frame_id = 0;
while (streaming) {
    if (fc_capture_frame(fc) == 1) {
        XImage *frame = fc_get_frame(fc);
        if (udp_send_frame(streamer, frame, frame_id) == 0) {
            frame_id++;
            if (frame_id % 60 == 0) {
                printf("Sent %u frames\n", frame_id);
            }
        }
        fc_mark_frame_processed(fc);
    }
    usleep(5000);
}
```

**Console Output**:
```
Sending frame 0: 2336x1080 (7560960 bytes) in 5467 packets
Sent frame 60 (5467 packets)
Sent frame 120 (5467 packets)
```

**Performance**: 
- Inter-packet delay: 50μs (configurable)
- For 2336x1080: ~5467 packets per frame
- Progress reporting every 10th frame

---

#### `void udp_print_status(UDPStreamer *streamer)`

**Purpose**: Print current streamer status and configuration.

**Parameters**:
- `streamer`: UDPStreamer

**Returns**: None (prints to stdout)

**Example Output**:
```
UDP Streamer Status:
  Port: 8888
  Socket FD: 3
  Client connected: YES
  Client: 192.168.1.100:54321
  Frame size: 2336x1080 (3 bytes per pixel)
```

**Example Usage**:
```c
udp_print_status(streamer);  // Debug information
```

---

#### `void udp_cleanup(UDPStreamer *streamer)`

**Purpose**: Clean up all UDP resources.

**Parameters**:
- `streamer`: UDPStreamer to clean up (safe to pass NULL)

**Returns**: None

**Cleanup Actions**:
- Closes UDP socket
- Frees UDPStreamer structure

**Example**:
```c
udp_cleanup(streamer);
streamer = NULL;
```

---

## Main Application

The main application provides a comprehensive command-line interface for all functionality.

### Command Line Interface

#### Display Management Commands

**List all outputs**:
```bash
./tabcaster --list
```
Output:
```
Found 4 total outputs, 2 connected
All outputs:
  eDP-1: 1920x1080+0+0 (primary) [CONNECTED]
  HDMI-1: 2336x1080+1920+0 [CONNECTED]
  DP-1: [DISCONNECTED]
  VGA-1: [DISCONNECTED]
```

**List modes for specific output**:
```bash
./tabcaster --list-modes HDMI-1
```

**Show output status**:
```bash
./tabcaster --status HDMI-1
```
Output:
```
Status for output 'HDMI-1':
  Connection: CONNECTED
  Primary: NO
  Enabled: YES
  Current mode ID: 524
  Resolution: 2336x1080
  Position: 1920,0
```

#### Mode Management Commands

**Create custom CVT mode**:
```bash
./tabcaster --create-mode 2336x1080@60
./tabcaster --create-mode 3440x1440@100 --reduced-blanking
```

**Add mode to output**:
```bash
./tabcaster --add-mode HDMI-1 524
```

**Enable output with mode**:
```bash
# By mode name
./tabcaster --enable HDMI-1 2336x1080_60.00 --position 1920,0

# By mode ID
./tabcaster --enable-id HDMI-1 524 --position 1920,0
```

**Disable output**:
```bash
./tabcaster --disable HDMI-1
```

#### Streaming Commands

**Stream output via UDP**:
```bash
# Default port 8888, 30 FPS
./tabcaster --stream HDMI-1

# Custom port and frame rate
./tabcaster --stream HDMI-1 --port 9999 --fps 60
```

### Signal Handling

The application handles `SIGINT` (Ctrl+C) gracefully:

```c
static volatile bool keep_running = true;

void signal_handler(int sig) {
    keep_running = false;
}

// In main()
signal(SIGINT, signal_handler);

while (keep_running) {
    // Main loop operations
    // ... streaming/capture code ...
}
```

### Complete Usage Examples

#### Basic Display Management

```c
#include "display_manager.h"
#include "mode_manager.h"

int main() {
    // Initialize display manager
    DisplayManager *dm = dm_init();
    if (!dm) {
        fprintf(stderr, "Cannot connect to X server\n");
        return 1;
    }
    
    // Get display information
    int connected = dm_get_screens(dm);
    printf("Found %d connected displays\n", connected);
    dm_print_screens(dm);
    
    // Find specific display
    ScreenInfo *hdmi = NULL;
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, "HDMI-1") == 0) {
            hdmi = &dm->screens[i];
            break;
        }
    }
    
    if (hdmi) {
        if (hdmi->connected) {
            printf("HDMI-1: %dx%d at %d,%d\n", 
                   hdmi->width, hdmi->height, hdmi->x, hdmi->y);
        } else {
            printf("HDMI-1 available but not connected\n");
        }
    }
    
    dm_cleanup(dm);
    return 0;
}
```

#### Creating and Using Custom Modes

```c
int setup_custom_display() {
    DisplayManager *dm = dm_init();
    if (!dm) return 1;
    
    dm_get_screens(dm);
    
    // Create custom ultrawide mode
    printf("Creating 3440x1440@100Hz mode...\n");
    RRMode mode_id = mode_create_cvt(dm, 3440, 1440, 100.0, true);
    if (mode_id == 0) {
        fprintf(stderr, "Failed to create mode\n");
        dm_cleanup(dm);
        return 1;
    }
    
    // Add mode to DP-1 output
    if (mode_add_to_output(dm, "DP-1", mode_id) != 0) {
        fprintf(stderr, "Failed to add mode to DP-1\n");
        mode_delete_from_xrandr(dm, mode_id);
        dm_cleanup(dm);
        return 1;
    }
    
    // Enable the output with the new mode
    if (mode_enable_output_with_mode_id(dm, "DP-1", mode_id, 1920, 0) != 0) {
        fprintf(stderr, "Failed to enable DP-1\n");
        mode_remove_from_output(dm, "DP-1", mode_id);
        mode_delete_from_xrandr(dm, mode_id);
        dm_cleanup(dm);
        return 1;
    }
    
    printf("DP-1 enabled with custom mode at 1920,0\n");
    
    // Cleanup
    dm_cleanup(dm);
    return 0;
}
```

#### Frame Capture Example

```c
#include "frame_capture.h"

int capture_example() {
    DisplayManager *dm = dm_init();
    if (!dm) return 1;
    
    dm_get_screens(dm);
    
    // Initialize capture for primary display
    ScreenInfo *primary = dm_get_primary_screen(dm);
    if (!primary) {
        fprintf(stderr, "No primary display found\n");
        dm_cleanup(dm);
        return 1;
    }
    
    FrameCapture *fc = fc_init(dm, primary->name, 30);
    if (!fc) {
        fprintf(stderr, "Failed to initialize capture\n");
        dm_cleanup(dm);
        return 1;
    }
    
    fc_start(fc);
    fc_print_frame_info(fc);
    
    // Capture 10 frames
    int frames_captured = 0;
    while (frames_captured < 10) {
        int result = fc_capture_frame(fc);
        
        if (result == 1) {
            // Save frame
            char filename[64];
            snprintf(filename, sizeof(filename), "frame_%03d.ppm", frames_captured);
            
            if (fc_save_frame_ppm(fc, filename) == 0) {
                printf("Saved %s\n", filename);
                frames_captured++;
            }
            
            fc_mark_frame_processed(fc);
        } else if (result < 0) {
            fprintf(stderr, "Capture failed\n");
            break;
        }
        
        usleep(100000); // 100ms delay
    }
    
    fc_cleanup(fc);
    dm_cleanup(dm);
    return 0;
}
```

#### UDP Streaming Example

```c
#include "udp_streamer.h"
#include "frame_capture.h"

int streaming_example() {
    DisplayManager *dm = dm_init();
    if (!dm) return 1;
    
    dm_get_screens(dm);
    
    // Initialize capture for HDMI-1
    FrameCapture *fc = fc_init(dm, "HDMI-1", 60);
    if (!fc) {
        fprintf(stderr, "Failed to initialize capture\n");
        dm_cleanup(dm);
        return 1;
    }
    
    // Initialize UDP streamer
    UDPStreamer *streamer = udp_init(8888);
    if (!streamer) {
        fprintf(stderr, "Failed to initialize streamer\n");
        fc_cleanup(fc);
        dm_cleanup(dm);
        return 1;
    }
    
    fc_start(fc);
    
    printf("Waiting for client connection...\n");
    if (udp_wait_for_client(streamer) != 0) {
        fprintf(stderr, "Client connection failed\n");
        udp_cleanup(streamer);
        fc_cleanup(fc);
        dm_cleanup(dm);
        return 1;
    }
    
    // Send frame info to client
    udp_send_frame_info(streamer, fc->width, fc->height);
    
    // Streaming loop
    uint32_t frame_id = 0;
    int frames_sent = 0;
    
    signal(SIGINT, signal_handler);
    printf("Streaming... Press Ctrl+C to stop\n");
    
    while (keep_running) {
        int result = fc_capture_frame(fc);
        
        if (result == 1) {
            XImage *frame = fc_get_frame(fc);
            if (frame && udp_send_frame(streamer, frame, frame_id) == 0) {
                frame_id++;
                frames_sent++;
                
                if (frames_sent % 60 == 0) {
                    printf("Streamed %d frames\n", frames_sent);
                }
            }
            fc_mark_frame_processed(fc);
        } else if (result < 0) {
            fprintf(stderr, "Capture error\n");
            break;
        }
        
        usleep(5000); // 5ms
    }
    
    printf("Streamed %d total frames\n", frames_sent);
    
    udp_cleanup(streamer);
    fc_cleanup(fc);
    dm_cleanup(dm);
    return 0;
}
```

---

## Building and Installation

### Prerequisites

**System Requirements**:
- Linux with X11 server
- GCC compiler with C99 support
- Development headers for required libraries

**Package Installation**:

**Debian/Ubuntu**:
```bash
sudo apt-get update
sudo apt-get install build-essential
sudo apt-get install libx11-dev libxrandr-dev
sudo apt-get install libxcvt-dev
```

**Fedora/RHEL/CentOS**:
```bash
sudo dnf install gcc make
sudo dnf install libX11-devel libXrandr-devel
sudo dnf install libxcvt-devel
```

**Arch Linux**:
```bash
sudo pacman -S base-devel
sudo pacman -S libx11 libxrandr
sudo pacman -S libxcvt
```

### Building

**Standard Build**:
```bash
# Clone or extract source code
cd tabcaster

# Build release version
make

# The executable will be created as 'tabcaster'
ls -la tabcaster
```

**Debug Build**:
```bash
# Build with debug symbols and verbose output
make clean
make CFLAGS="-g -O0 -DDEBUG -Wall -Wextra"

# Run with debugger
gdb ./tabcaster
```

**Custom Build Options**:
```bash
# Optimize for specific architecture
make CFLAGS="-O3 -march=native"

# Static linking (for distribution)
make LDFLAGS="-static"

# Enable all warnings
make CFLAGS="-Wall -Wextra -Wpedantic -Werror"
```

### Makefile Structure

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2
LDFLAGS = -lX11 -lXrandr -lxcvt

TARGET = tabcaster
SOURCES = main.c display_manager.c mode_manager.c frame_capture.c udp_streamer.c
OBJECTS = $(SOURCES:.c=.o)
HEADERS = display_manager.h mode_manager.h frame_capture.h udp_streamer.h

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all clean install
```

### Installation

**System-wide Installation**:
```bash
sudo make install
# Installs to /usr/local/bin/tabcaster

# Verify installation
which tabcaster
tabcaster --help
```

**Local Installation**:
```bash
# Create local bin directory
mkdir -p ~/bin

# Copy executable
cp tabcaster ~/bin/

# Add to PATH (add to ~/.bashrc for persistence)
export PATH="$HOME/bin:$PATH"
```

### Dependencies Verification

**Runtime Dependencies Check**:
```bash
# Check required shared libraries
ldd tabcaster

# Expected output:
# libxcvt.so.0 => /usr/lib/x86_64-linux-gnu/libxcvt.so.0
# libXrandr.so.2 => /usr/lib/x86_64-linux-gnu/libXrandr.so.2
# libX11.so.6 => /usr/lib/x86_64-linux-gnu/libX11.so.6
# libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
```

**X11 Extensions Check**:
```bash
# Verify XRandR extension is available
xdpyinfo | grep RANDR

# Expected output:
# RANDR version 1.6 opcode: 140
```

---

## Troubleshooting

### Common Issues and Solutions

#### Connection Issues

**"Cannot open X display"**

**Causes**:
- X server not running
- `DISPLAY` environment variable not set
- Permission issues

**Solutions**:
```bash
# Check if X server is running
ps aux | grep Xorg

# Set DISPLAY variable
export DISPLAY=:0

# Check X server permissions
xhost +local:

# For SSH connections
ssh -X user@host
# or
ssh -Y user@host  # Trusted X11 forwarding
```

**"XRRGetScreenResources failed"**

**Causes**:
- XRandR extension not available
- X server too old
- Graphics driver issues

**Solutions**:
```bash
# Check XRandR availability
xrandr --version

# Verify extension support
xdpyinfo | grep RANDR

# Update graphics drivers
sudo apt update && sudo apt upgrade  # Debian/Ubuntu
sudo dnf update                      # Fedora
```

#### Mode Creation Issues

**"libxcvt failed to generate mode"**

**Causes**:
- Invalid resolution parameters
- Extreme refresh rates
- libxcvt not installed

**Solutions**:
```bash
# Validate parameters
./tabcaster --create-mode 1920x1080@60  # Known good values

# Check libxcvt installation
pkg-config --modversion libxcvt

# Install libxcvt if missing
sudo apt-get install libxcvt-dev
```

**"XRRCreateMode failed"**

**Causes**:
- Mode already exists
- Invalid timing parameters
- X server limitations

**Solutions**:
```bash
# List existing modes
xrandr

# Delete conflicting mode
xrandr --delmode OUTPUT MODE_NAME
xrandr --rmmode MODE_NAME

# Try different timing
./tabcaster --create-mode 1920x1080@59.93
```

#### Capture Issues

**"XGetImage failed"**

**Causes**:
- Invalid capture coordinates
- Display not active
- Memory allocation failure

**Solutions**:
```bash
# Verify display is active
./tabcaster --status HDMI-1

# Check available memory
free -h

# Enable display first
./tabcaster --enable HDMI-1 1920x1080_60.00
```

**"Failed to create captures directory"**

**Causes**:
- Permission issues
- Disk full
- Invalid filesystem

**Solutions**:
```bash
# Check permissions
ls -la ./

# Create directory manually
mkdir -p captures
chmod 755 captures

# Check disk space
df -h .
```

#### Streaming Issues

**"Failed to initialize UDP streamer"**

**Causes**:
- Port already in use
- Firewall blocking
- Permission issues (ports < 1024)

**Solutions**:
```bash
# Check if port is in use
netstat -ulpn | grep 8888
ss -ulpn | grep 8888

# Use different port
./tabcaster --stream HDMI-1 --port 9999

# Check firewall
sudo ufw status
sudo firewall-cmd --list-ports
```

**"Client connection failed"**

**Causes**:
- Network connectivity issues
- Incorrect handshake protocol
- Client timeout

**Solutions**:
```bash
# Test basic connectivity
telnet server_ip 8888
nc -u server_ip 8888

# Check network interface
ip addr show
ifconfig

# Disable firewall temporarily
sudo ufw disable
```

### Debug Techniques

#### Memory Debugging

**Valgrind Memory Check**:
```bash
# Check for memory leaks
valgrind --leak-check=full --show-leak-kinds=all ./tabcaster --list

# Check for buffer overflows
valgrind --tool=memcheck --track-origins=yes ./tabcaster --stream HDMI-1

# Expected clean output:
# ==1234== HEAP SUMMARY:
# ==1234==     in use at exit: 0 bytes in 0 blocks
# ==1234== All heap blocks were freed -- no leaks are possible
```

**Address Sanitizer**:
```bash
# Build with AddressSanitizer
make clean
make CFLAGS="-g -fsanitize=address -fno-omit-frame-pointer"

# Run and check for issues
./tabcaster --list
```

#### Network Debugging

**tcpdump Packet Capture**:
```bash
# Capture UDP traffic on streaming port
sudo tcpdump -i any -n udp port 8888

# Example output:
# 12:34:56.789 IP 192.168.1.100.54321 > 192.168.1.200.8888: UDP, length 1400
# 12:34:56.790 IP 192.168.1.100.54321 > 192.168.1.200.8888: UDP, length 1400
```

**netstat Connection Monitoring**:
```bash
# Monitor UDP socket status
watch 'netstat -ulpn | grep tabcaster'

# Check socket buffer usage
ss -u -a -n | grep 8888
```

#### Performance Profiling

**gprof Profiling**:
```bash
# Build with profiling
make clean
make CFLAGS="-g -pg -O2"

# Run application
./tabcaster --stream HDMI-1 &
# ... let it run for a while ...
kill %1

# Generate profile report
gprof ./tabcaster gmon.out > profile.txt
less profile.txt
```

**Real-time Performance Monitoring**:
```bash
# Monitor CPU usage
top -p $(pgrep tabcaster)

# Monitor memory usage
watch 'cat /proc/$(pgrep tabcaster)/status | grep -E "(VmRSS|VmSize)"'

# Monitor network traffic
iftop -i eth0 -f "port 8888"
```

#### X11 Debugging

**X11 Error Debugging**:
```bash
# Enable X11 synchronous mode (slower but better error reporting)
export DISPLAY=:0
./tabcaster --list

# Monitor X server messages
tail -f /var/log/Xorg.0.log

# Check X resource usage
xrestop
```

**XRandR State Debugging**:
```bash
# Get detailed XRandR information
xrandr --verbose > xrandr_before.txt
./tabcaster --enable HDMI-1 2560x1080_60.00
xrandr --verbose > xrandr_after.txt
diff xrandr_before.txt xrandr_after.txt
```

### Error Codes and Messages

#### Display Manager Errors

| Error Message | Code | Cause | Solution |
|---------------|------|-------|----------|
| "Cannot open X display" | -1 | X server not accessible | Check DISPLAY, start X server |
| "XRRGetScreenResources failed" | -1 | XRandR unavailable | Update X server, check drivers |
| "Failed to get screen info" | -1 | Resource allocation failed | Check memory, restart X server |

#### Mode Manager Errors

| Error Message | Code | Cause | Solution |
|---------------|------|-------|----------|
| "libxcvt failed to generate mode" | 0 | Invalid parameters | Check resolution/refresh values |
| "XRRCreateMode failed" | 0 | Mode creation failed | Check existing modes, try different values |
| "Output not found" | -1 | Invalid output name | Use --list to see available outputs |
| "No available CRTC found" | -1 | All CRTCs in use | Disable unused output first |

#### Frame Capture Errors

| Error Message | Code | Cause | Solution |
|---------------|------|-------|----------|
| "Invalid capture dimensions" | NULL | Zero width/height | Enable output first |
| "XGetImage failed" | -1 | Screen capture failed | Check output status, memory |
| "Failed to create captures directory" | NULL | Permission/disk issue | Check permissions, disk space |

#### UDP Streamer Errors

| Error Message | Code | Cause | Solution |
|---------------|------|-------|----------|
| "Socket creation failed" | NULL | Network subsystem issue | Check network configuration |
| "Bind failed" | NULL | Port in use/permissions | Use different port, check permissions |
| "Invalid handshake received" | -1 | Client protocol error | Check client implementation |
| "Failed to send packet" | -1 | Network error | Check network connectivity |

This comprehensive documentation provides detailed information about every function, parameter, return value, and usage example in the Tabcaster codebase. Each module is thoroughly documented with practical examples and troubleshooting guidance.