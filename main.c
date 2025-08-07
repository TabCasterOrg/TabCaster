#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  
#include "display_manager.h"
#include "mode_manager.h"
#include "frame_capture.h"  
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
    printf("  --reduced-blanking        Use reduced blanking for CVT (with --create-mode)\n");
    printf("  --capture OUTPUT          Capture frames from output\n");
    printf("  --fps FPS                 Set capture frame rate (default: 30, use with --capture)\n");
    printf("  --help                    Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --create-mode 2336x1080@60\n", program_name);
    printf("  %s --add-mode HDMI-1 123456789\n", program_name);
    printf("  %s --enable HDMI-1 2336x1080_60.00\n", program_name);
    printf("  %s --enable-id HDMI-1 123456789 --position 1920,0\n", program_name);
    printf("  %s --disable HDMI-1\n", program_name);
    printf("  %s --list-modes HDMI-1\n", program_name);
    printf("  %s --status HDMI-1\n", program_name);
    printf("  %s --capture HDMI-1\n", program_name);
    printf("  %s --fps 60\n", program_name);
    printf("\nCapture files are saved in ./captures/ directory\n");
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

// Signal handler to stop capturing on Ctrl+C
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

// Main entry point with command line argument parsing
int main(int argc, char *argv[]) {
    printf("Tabcaster - C Version with Output Management\n");
    
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

    // Frame capture variables
    bool enable_capture = false;
    char *capture_output = NULL;
    int capture_fps = 30;

    char *mode_spec = NULL;
    char *output_name = NULL;
    char *mode_name = NULL;
    char *status_output = NULL;
    char *list_modes_output = NULL;
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
        } 
        
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            enable_capture = true;
            capture_output = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            capture_fps = atoi(argv[++i]);
        
        } else if (strcmp(argv[i], "--position") == 0 && i + 1 < argc) {
            if (parse_position(argv[++i], &pos_x, &pos_y) != 0) {
                return 1;
            }
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
        printf("Enabling output '%s' with mode '%s' at position %d,%d\n", 
               output_name, mode_name, pos_x, pos_y);
        
        if (mode_enable_output_with_mode(dm, output_name, mode_name, pos_x, pos_y) != 0) {
            fprintf(stderr, "Failed to enable output with mode\n");
        }
    }
    
    if (enable_output_id) {
        printf("Enabling output '%s' with mode ID %lu at position %d,%d\n", 
               output_name, mode_id, pos_x, pos_y);
        
        if (mode_enable_output_with_mode_id(dm, output_name, mode_id, pos_x, pos_y) != 0) {
            fprintf(stderr, "Failed to enable output with mode ID\n");
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
        
    // Frame capture (if requested)
    if (enable_capture && capture_output) {
        printf("\n=== Frame Capture ===\n");
        
        FrameCapture *fc = fc_init(dm, capture_output, capture_fps);
        if (!fc) {
            fprintf(stderr, "Failed to initialize capture\n");
            dm_cleanup(dm);
            return 1;
        }
        
        fc_print_frame_info(fc);
        
        if (fc_start(fc) != 0) {
            fc_cleanup(fc);
            dm_cleanup(dm);
            return 1;
        }
        
        signal(SIGINT, signal_handler);
        printf("Capturing... Press Ctrl+C to stop\n");
        printf("Frames will be saved to ./captures/ directory\n");
        
        // Simple capture loop
        int frame_count = 0;
        while (keep_running) {
            int result = fc_capture_frame(fc);
            
            if (result == 1) {  // New frame captured
                frame_count++;
                printf("Frame %d\r", frame_count);
                fflush(stdout);
                
                // Save every 60th frame as example
                if (frame_count % 60 == 0) {
                    char filename[64];
                    snprintf(filename, sizeof(filename), "capture_%04d.ppm", frame_count);
                    fc_save_frame_ppm(fc, filename);
                }
                
                fc_mark_frame_processed(fc);
            } else if (result < 0) {
                fprintf(stderr, "\nCapture failed\n");
                break;
            }
            
            // Small sleep to prevent busy waiting
            usleep(5000); // 5ms
        }
        
        printf("\nCaptured %d frames\n", frame_count);
        fc_cleanup(fc);
    }
    
    // Clean up
    dm_cleanup(dm);
    return 0;
}