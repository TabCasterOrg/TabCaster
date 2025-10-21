#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  
#include "display_manager.h"
#include "mode_manager.h"
#include "frame_capture.h"  
#include "udp_server.h"
#include "frame_streamer.h"
#include <signal.h>



static volatile bool keep_running = true;

// Print usage information
void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  --list                    List all outputs and their status\n");
    printf("  --list-modes [OUTPUT]     List modes for specific output or all outputs\n");
    printf("  --create-mode WxH@R       Create CVT mode (e.g., 2336x1080@60)\n");
    printf("  --add-mode OUTPUT ID      Add existing mode (by ID) to output\n");
    printf("  --remove-mode OUTPUT ID   Remove mode (by ID) from output\n");
    printf("  --delete-mode ID          Delete mode (by ID) from XRandR entirely\n");
    printf("  --enable OUTPUT MODE      Enable output with specific mode name\n");
    printf("  --enable-id OUTPUT ID     Enable output with specific mode ID\n");
    printf("  --disable OUTPUT          Disable output\n");
    printf("  --status OUTPUT           Show current status of output\n");
    printf("  --position X,Y            Set position when enabling output (default: 0,0)\n");
    printf("  --left-of                 Auto-position left of primary screen (use with --enable/--enable-id)\n");
    printf("  --reduced-blanking        Use reduced blanking for CVT (with --create-mode)\n");
    printf("  --test-display OUTPUT WxH@R  Create mode, add to output, and enable (all-in-one)\n");
    printf("  --stream OUTPUT           Stream frames from output via UDP (with resolution exchange)\n");
    printf("  --port PORT               UDP port for streaming (default: 23532, use with --stream)\n");
    printf("  --fps FPS                 Set capture frame rate (default: 30, use with --stream)\n");
    printf("  --delta                   Enable delta region streaming (with --stream)\n");
    printf("  --delta-thresh N          Delta per-pixel diff threshold (default: 30)\n");
    printf("  --delta-cover N           Max delta coverage %% before keyframe (default: 80)\n");
    printf("  --delta-keyint N          Keyframe interval in frames (default: 120)\n");
    printf("  --delta-keysec N          Keyframe interval in seconds (default: 3)\n");
    printf("  --delta-padding N         Pixels to pad around change regions (default: 8)\n");
    printf("  --delta-minsize N         Minimum region width/height (default: 32)\n");
    printf("  --delta-maxregions N      Maximum regions per frame (default: 8)\n");
    printf("  --delta-cellsize N        Grid cell size for region detection (default: 64)\n");
    printf("  --help                    Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --create-mode 2336x1080@60\n", program_name);
    printf("  %s --add-mode HDMI-1 123456789\n", program_name);
    printf("  %s --enable HDMI-1 2336x1080_60.00\n", program_name);
    printf("  %s --enable-id HDMI-1 123456789 --position 1920,0\n", program_name);
    printf("  %s --enable-id HDMI-1 123456789 --left-of\n", program_name);
    printf("  %s --disable HDMI-1\n", program_name);
    printf("  %s --list-modes HDMI-1\n", program_name);
    printf("  %s --status HDMI-1\n", program_name);
    printf("  %s --test-display HDMI-1 2336x1080@60 --position 1920,0\n", program_name);
    printf("  %s --test-display DP-2 3440x1440@100 --reduced-blanking --left-of\n", program_name);
    printf("  %s --stream HDMI-1 --fps 60\n", program_name);
    printf("\nStreaming:\n");
    printf("  --stream creates a display based on client resolution and streams frames.\n");
    printf("  The client sends its resolution during handshake.\n");
    printf("  Streaming always uses auto-positioning (right of primary).\n");
}

// Parse mode specification (WxH@R format)
int parse_mode_spec(const char *spec, unsigned int *width, unsigned int *height, double *refresh) {
    if (!spec || !width || !height || !refresh) return -1;
    
    // Format: WIDTHxHEIGHT@REFRESH (e.g., "2336x1080@60" or "1920x1080@59.93")
    int parsed = sscanf(spec, "%ux%u@%lf", width, height, refresh);
    if (parsed != 3) {
        fprintf(stderr, "Invalid mode specification: %s\n", spec);
        fprintf(stderr, "Expected format: WIDTHxHEIGHT@REFRESH (e.g., 2336x1080@60)\n");
        return -1;
    }
    
    // Basic validation
    if (*width < 1 || *width > 32767 || *height < 1 || *height > 32767) {
        fprintf(stderr, "Invalid resolution: %ux%u\n", *width, *height);
        return -1;
    }
    
    if (*refresh <= 0 || *refresh > 240) {
        fprintf(stderr, "Invalid refresh rate: %.2f\n", *refresh);
        return -1;
    }
    
    return 0;
}

// Parse position specification (X,Y format)
int parse_position(const char *pos_str, int *x, int *y) {
    if (!pos_str || !x || !y) return -1;
    
    int parsed = sscanf(pos_str, "%d,%d", x, y);
    if (parsed != 2) {
        fprintf(stderr, "Invalid position specification: %s\n", pos_str);
        fprintf(stderr, "Expected format: X,Y (e.g., 1920,0)\n");
        return -1;
    }
    
    return 0;
}

// Signal handler to stop operations on Ctrl+C
void signal_handler(int sig) {
    (void)sig; // Suppress unused parameter warning
    keep_running = false;
}

// Print output status information
void print_output_status(DisplayManager *dm, const char *output_name) {
    if (!dm || !output_name) return;
    
    printf("Status for output '%s':\n", output_name);
    
    // Check if output exists and is connected
    bool found = false;
    bool connected = false;
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, output_name) == 0) {
            found = true;
            connected = dm->screens[i].connected;
            printf("  Connection: %s\n", connected ? "CONNECTED" : "DISCONNECTED");
            if (connected) {
                printf("  Primary: %s\n", dm->screens[i].primary ? "YES" : "NO");
            }
            break;
        }
    }
    
    if (!found) {
        printf("  Output not found\n");
        return;
    }
    
    // Check if enabled
    bool enabled = mode_is_output_enabled(dm, output_name);
    printf("  Enabled: %s\n", enabled ? "YES" : "NO");
    
    if (enabled) {
        RRMode current_mode;
        int x, y;
        unsigned int width, height;
        
        if (mode_get_output_config(dm, output_name, &current_mode, &x, &y, &width, &height) == 0) {
            printf("  Current mode ID: %lu\n", current_mode);
            printf("  Resolution: %ux%u\n", width, height);
            printf("  Position: %d,%d\n", x, y);
        }
    }
    
    printf("\n");
}

// Test display functionality - create mode, add to output, and enable
int setup_test_display(DisplayManager *dm, const char *output_name, const char *mode_spec, 
                      int pos_x, int pos_y, bool reduced_blanking, bool auto_right_of) {
    if (!dm || !output_name || !mode_spec) {
        fprintf(stderr, "Invalid parameters for test display setup\n");
        return -1;
    }
    
    // Parse mode specification
    unsigned int width, height;
    double refresh_rate;
    
    if (parse_mode_spec(mode_spec, &width, &height, &refresh_rate) != 0) {
        return -1;
    }
    
    printf("\n=== Setting up test display '%s' ===\n", output_name);
    printf("Mode specification: %ux%u @ %.2f Hz%s\n", 
           width, height, refresh_rate,
           reduced_blanking ? " (reduced blanking)" : "");
    
    if (auto_right_of) {
        printf("Positioning: Auto (left of primary screen)\n");
    } else {
        printf("Position: %d,%d\n", pos_x, pos_y);
    }
    
    // Step 1: Create CVT mode
    printf("\nStep 1: Creating CVT mode...\n");
    RRMode mode_id = mode_create_cvt(dm, width, height, refresh_rate, reduced_blanking);
    if (mode_id == 0) {
        fprintf(stderr, "Failed to create CVT mode\n");
        return -1;
    }
    printf("✓ Mode created successfully with ID: %lu\n", mode_id);
    
    // Step 2: Add mode to output
    printf("\nStep 2: Adding mode to output '%s'...\n", output_name);
    if (mode_add_to_output(dm, output_name, mode_id) != 0) {
        fprintf(stderr, "Failed to add mode to output '%s'\n", output_name);
        fprintf(stderr, "Cleaning up: deleting created mode\n");
        mode_delete_from_xrandr(dm, mode_id);
        return -1;
    }
    printf("✓ Mode added to output successfully\n");
    
    // Step 3: Enable output with the new mode (with positioning option)
    printf("\nStep 3: Enabling output with new mode...\n");
    
    int result;
    if (auto_right_of) {
        result = mode_enable_output_with_mode_id_positioned(dm, output_name, mode_id, 
                                                           pos_x, pos_y, true);
    } else {
        result = mode_enable_output_with_mode_id(dm, output_name, mode_id, pos_x, pos_y);
    }
    
    if (result != 0) {
        fprintf(stderr, "Failed to enable output '%s' with mode ID %lu\n", output_name, mode_id);
        fprintf(stderr, "Cleaning up: removing mode from output and deleting\n");
        mode_remove_from_output(dm, output_name, mode_id);
        mode_delete_from_xrandr(dm, mode_id);
        return -1;
    }
    printf("✓ Output enabled successfully\n");
    
    // Refresh screen information to get updated status
    dm_get_screens(dm);
    
    // Find and display the configured output
    ScreenInfo *configured_screen = NULL;
    for (int i = 0; i < dm->screen_count; i++) {
        if (strcmp(dm->screens[i].name, output_name) == 0) {
            configured_screen = &dm->screens[i];
            break;
        }
    }
    
    if (configured_screen) {
        printf("✓ Test display setup completed successfully!\n");
        printf("\nTest display status:\n");
        printf("  Output: %s\n", configured_screen->name);
        printf("  Resolution: %ux%u\n", configured_screen->width, configured_screen->height);
        printf("  Position: %d,%d\n", configured_screen->x, configured_screen->y);
        printf("  Mode ID: %lu\n", mode_id);
        
        printf("\nYou can now:\n");
        printf("  - Stream from this display: %s --stream %s\n", "tabcaster", output_name);
        printf("  - Check status: %s --status %s\n", "tabcaster", output_name);
        printf("  - Disable when done: %s --disable %s\n", "tabcaster", output_name);
        printf("  - Remove mode: %s --remove-mode %s %lu\n", "tabcaster", output_name, mode_id);
        printf("  - Delete mode: %s --delete-mode %lu\n", "tabcaster", mode_id);
        
        return 0;
    } else {
        fprintf(stderr, "Setup appears to have failed, but mode was created\n");
        
        // Still show the mode ID for manual cleanup if needed
        printf("\nCreated mode ID: %lu\n", mode_id);
        printf("If you need to clean up manually:\n");
        printf("  %s --disable %s\n", "tabcaster", output_name);
        printf("  %s --remove-mode %s %lu\n", "tabcaster", output_name, mode_id);
        printf("  %s --delete-mode %lu\n", "tabcaster", mode_id);
        
        return 0; // Don't treat as failure since mode was created
    }
}

// Stream with resolution exchange with client (auto-positioning is built into UDP server now)
int stream_with_resolution_exchange(DisplayManager *dm, const char *output_name, 
                                   int port, int fps) {
    if (!dm || !output_name) {
        fprintf(stderr, "Invalid parameters for streaming\n");
        return -1;
    }
    
    printf("\n=== UDP Streaming ===\n");
    printf("Output: %s\n", output_name);
    printf("Port: %d\n", port);
    printf("FPS: %d\n", fps);
    printf("Positioning: Auto (left of primary screen)\n");
    
    // Step 1: Initialize UDP server
    UDPServer *server = udp_server_init(port, dm);
    if (!server) {
        fprintf(stderr, "Failed to initialize UDP server\n");
        return -1;
    }
    
    udp_server_print_status(server);
    
    // Step 2: Wait for client and complete handshake
    printf("\n=== Handshake Phase ===\n");
    if (udp_server_wait_for_client(server) != 0) {
        fprintf(stderr, "Handshake failed\n");
        udp_server_cleanup(server);
        return -1;
    }
    
    // Step 3: Create display based on client resolution
    printf("\n=== Display Creation Phase ===\n");
    if (udp_server_create_display_for_client(server, output_name) != 0) {
        fprintf(stderr, "Failed to create display for client\n");
        udp_server_cleanup(server);
        return -1;
    }
    
    // Step 4: Initialize frame streamer (now responsible for cleanup)
    printf("\n=== Streaming Setup Phase ===\n");
    FrameStreamer *streamer = frame_streamer_init(server, output_name, fps);
    if (!streamer) {
        fprintf(stderr, "Failed to initialize frame streamer\n");
        udp_server_cleanup(server);
        return -1;
    }
    
    // Step 5: Start streaming
    printf("\n=== Streaming Phase ===\n");
    signal(SIGINT, signal_handler);
    
    int result = frame_streamer_start(streamer);
    fc_update_position(streamer->frame_capture); // Update position in case of changes

    // Step 6: Cleanup (server shutdown and display cleanup)
    printf("\n=== Cleanup Phase ===\n");
    frame_streamer_cleanup(streamer);  // This now includes display cleanup
    udp_server_cleanup(server);
    
    return result;
}

// Main entry point with command line argument parsing
int main(int argc, char *argv[]) {
    printf("Tabcaster - C Version with Output Management\n");
    // Initialize X11 error handling TEMP DEBUG
    XSetErrorHandler(ignore_badmatch_with_flag);
    
    // Parse command line arguments
    bool list_mode = false;
    bool list_modes = false;
    bool create_mode = false;
    bool add_mode = false;
    bool remove_mode = false;
    bool delete_mode = false;
    bool enable_output = false;
    bool enable_output_id = false;
    bool disable_output = false;
    bool show_status = false;
    bool reduced_blanking = false;
    bool test_display = false;
    bool auto_right_of = false;

    // UDP streaming variables
    bool enable_stream = false;
    char *stream_output = NULL;
    int stream_port = 23532;

    // Frame capture variables
    int capture_fps = 30;

    // Delta streaming CLI controls
    bool cli_delta_enable = false;
    int cli_delta_thresh = -1;
    int cli_delta_cover = -1;
    int cli_delta_keyint = -1;
    int cli_delta_keysec = -1;
    int cli_delta_padding = -1;
    int cli_delta_minsize = -1;
    int cli_delta_maxregions = -1;
    int cli_delta_cellsize = -1;

    char *mode_spec = NULL;
    char *output_name = NULL;
    char *mode_name = NULL;
    char *status_output = NULL;
    char *list_modes_output = NULL;
    char *test_output_name = NULL;
    char *test_mode_spec = NULL;
    RRMode mode_id = 0;
    int pos_x = 0, pos_y = 0;
    
    // Simple argument parsing
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_mode = true;
        } else if (strcmp(argv[i], "--list-modes") == 0) {
            list_modes = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                list_modes_output = argv[++i];
            }
        } else if (strcmp(argv[i], "--create-mode") == 0 && i + 1 < argc) {
            create_mode = true;
            mode_spec = argv[++i];
        } else if (strcmp(argv[i], "--add-mode") == 0 && i + 2 < argc) {
            add_mode = true;
            output_name = argv[++i];
            mode_id = (RRMode)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--remove-mode") == 0 && i + 2 < argc) {
            remove_mode = true;
            output_name = argv[++i];
            mode_id = (RRMode)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--delete-mode") == 0 && i + 1 < argc) {
            delete_mode = true;
            mode_id = (RRMode)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--enable") == 0 && i + 2 < argc) {
            enable_output = true;
            output_name = argv[++i];
            mode_name = argv[++i];
        } else if (strcmp(argv[i], "--enable-id") == 0 && i + 2 < argc) {
            enable_output_id = true;
            output_name = argv[++i];
            mode_id = (RRMode)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--disable") == 0 && i + 1 < argc) {
            disable_output = true;
            output_name = argv[++i];
        } else if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) {
            show_status = true;
            status_output = argv[++i];
        } else if (strcmp(argv[i], "--test-display") == 0 && i + 2 < argc) {
            test_display = true;
            test_output_name = argv[++i];
            test_mode_spec = argv[++i];
        } else if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc) {
            enable_stream = true;
            stream_output = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            stream_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            capture_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta") == 0) {
            cli_delta_enable = true;
        } else if (strcmp(argv[i], "--delta-thresh") == 0 && i + 1 < argc) {
            cli_delta_thresh = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta-cover") == 0 && i + 1 < argc) {
            cli_delta_cover = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta-keyint") == 0 && i + 1 < argc) {
            cli_delta_keyint = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta-keysec") == 0 && i + 1 < argc) {
            cli_delta_keysec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta-padding") == 0 && i + 1 < argc) {
            cli_delta_padding = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta-minsize") == 0 && i + 1 < argc) {
            cli_delta_minsize = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta-maxregions") == 0 && i + 1 < argc) {
            cli_delta_maxregions = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delta-cellsize") == 0 && i + 1 < argc) {
            cli_delta_cellsize = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--position") == 0 && i + 1 < argc) {
            if (parse_position(argv[++i], &pos_x, &pos_y) != 0) {
                return 1;
            }
        } else if (strcmp(argv[i], "--left-of") == 0) {
            auto_right_of = true;
        } else if (strcmp(argv[i], "--reduced-blanking") == 0) {
            reduced_blanking = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } 
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        
    }
    
    // Default to list mode if no arguments
    if (argc == 1) {
        list_mode = true;
    }
    
    // Validate conflicting options
    if (auto_right_of && (pos_x != 0 || pos_y != 0)) {
        fprintf(stderr, "Warning: --left-of overrides --position coordinates\n");
        pos_x = 0;
        pos_y = 0;
    }
    
    // Initialize display manager
    DisplayManager *dm = dm_init();
    if (!dm) {
        fprintf(stderr, "Failed to initialize display manager\n");
        return 1;
    }
    
    // Get monitor information
    int connected_count = dm_get_screens(dm);
    if (connected_count < 0) {
        fprintf(stderr, "Failed to get screen information\n");
        dm_cleanup(dm);
        return 1;
    }
    
    // Execute requested operations
    if (list_mode) {
        printf("Found %d total output%s, %d connected\n", 
               dm->screen_count, 
               dm->screen_count == 1 ? "" : "s",
               connected_count);
        dm_print_screens(dm);
    }
    
    if (list_modes) {
        if (list_modes_output) {
            mode_print_output_modes(dm, list_modes_output);
        } else {
            mode_print_all_output_modes(dm);
        }
    }
    
    if (create_mode) {
        unsigned int width, height;
        double refresh_rate;
        
        if (parse_mode_spec(mode_spec, &width, &height, &refresh_rate) == 0) {
            printf("Creating CVT mode: %ux%u @ %.2f Hz%s\n", 
                   width, height, refresh_rate,
                   reduced_blanking ? " (reduced blanking)" : "");
            
            RRMode new_mode_id = mode_create_cvt(dm, width, height, refresh_rate, reduced_blanking);
            
            if (new_mode_id != 0) {
                printf("Mode created successfully with ID: %lu\n", new_mode_id);
                printf("To use this mode:\n");
                printf("  Add to output: %s --add-mode OUTPUT_NAME %lu\n", argv[0], new_mode_id);
                printf("  Enable output: %s --enable-id OUTPUT_NAME %lu\n", argv[0], new_mode_id);
                printf("  Enable left of primary: %s --enable-id OUTPUT_NAME %lu --left-of\n", argv[0], new_mode_id);
            } else {
                fprintf(stderr, "Failed to create CVT mode\n");
            }
        }
    }
    
    if (add_mode) {
        if (mode_add_to_output(dm, output_name, mode_id) != 0) {
            fprintf(stderr, "Failed to add mode to output\n");
        } else {
            printf("Mode added successfully. You can now enable it with:\n");
            printf("  %s --enable-id %s %lu\n", argv[0], output_name, mode_id);
            printf("  %s --enable-id %s %lu --left-of\n", argv[0], output_name, mode_id);
        }
    }
    
    if (remove_mode) {
        if (mode_remove_from_output(dm, output_name, mode_id) != 0) {
            fprintf(stderr, "Failed to remove mode from output\n");
        }
    }
    
    if (delete_mode) {
        if (mode_delete_from_xrandr(dm, mode_id) != 0) {
            fprintf(stderr, "Failed to delete mode\n");
        }
    }
    
    if (enable_output) {
        if (auto_right_of) {
            printf("Enabling output '%s' with mode '%s' (auto-positioned left of primary)\n", 
                   output_name, mode_name);
        } else {
            printf("Enabling output '%s' with mode '%s' at position %d,%d\n", 
                   output_name, mode_name, pos_x, pos_y);
        }
        
        if (mode_enable_output_with_mode(dm, output_name, mode_name, pos_x, pos_y) != 0) {
            fprintf(stderr, "Failed to enable output with mode\n");
        }
    }
    
    if (enable_output_id) {
        if (auto_right_of) {
            printf("Enabling output '%s' with mode ID %lu (auto-positioned left of primary)\n", 
                   output_name, mode_id);
            
            if (mode_enable_output_with_mode_id_positioned(dm, output_name, mode_id, 
                                                          pos_x, pos_y, true) != 0) {
                fprintf(stderr, "Failed to enable output with mode ID (auto-positioned)\n");
            }
        } else {
            printf("Enabling output '%s' with mode ID %lu at position %d,%d\n", 
                   output_name, mode_id, pos_x, pos_y);
            
            if (mode_enable_output_with_mode_id(dm, output_name, mode_id, pos_x, pos_y) != 0) {
                fprintf(stderr, "Failed to enable output with mode ID\n");
            }
        }
    }
    
    if (disable_output) {
        if (mode_disable_output(dm, output_name) != 0) {
            fprintf(stderr, "Failed to disable output\n");
        }
    }
    
    if (show_status) {
        print_output_status(dm, status_output);
    }
    
    // Test display setup with auto-positioning option
    if (test_display) {
        if (setup_test_display(dm, test_output_name, test_mode_spec, pos_x, pos_y, 
                              reduced_blanking, auto_right_of) != 0) {
            fprintf(stderr, "Test display setup failed\n");
            dm_cleanup(dm);
            return 1;
        }
    }
        
    // UDP streaming (auto-positioning is built into the UDP server)
    if (enable_stream && stream_output) {
        // Apply delta env flags so downstream streamer picks them up at init
        if (cli_delta_enable) setenv("TABC_DELTA", "1", 1);
        if (cli_delta_thresh >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_thresh);
            setenv("TABC_THRESH", buf, 1);
        }
        if (cli_delta_cover >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_cover);
            setenv("TABC_COVER", buf, 1);
        }
        if (cli_delta_keyint >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_keyint);
            setenv("TABC_KEYINT", buf, 1);
        }
        if (cli_delta_keysec >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_keysec);
            setenv("TABC_KEYSEC", buf, 1);
        }
        if (cli_delta_padding >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_padding);
            setenv("TABC_PADDING", buf, 1);
        }
        if (cli_delta_minsize >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_minsize);
            setenv("TABC_MINSIZE", buf, 1);
        }
        if (cli_delta_maxregions >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_maxregions);
            setenv("TABC_MAXREGIONS", buf, 1);
        }
        if (cli_delta_cellsize >= 0) {
            char buf[16]; snprintf(buf, sizeof(buf), "%d", cli_delta_cellsize);
            setenv("TABC_CELLSIZE", buf, 1);
        }
        int result = stream_with_resolution_exchange(dm, stream_output, stream_port, capture_fps);
        dm_cleanup(dm);
        return result == 0 ? 0 : 1;
    }
    
    // Clean up
    dm_cleanup(dm);
    return 0;
}