#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display_manager.h"
#include "mode_manager.h"

// Print usage information
void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  --list                    List all outputs and their status\n");
    printf("  --create-mode WxH@R       Create CVT mode (e.g., 2336x1080@60)\n");
    printf("  --add-mode OUTPUT ID      Add existing mode (by ID) to output\n");
    printf("  --remove-mode OUTPUT ID   Remove mode (by ID) from output\n");
    printf("  --delete-mode ID          Delete mode (by ID) from XRandR entirely\n");
    printf("  --reduced-blanking        Use reduced blanking for CVT (with --create-mode)\n");
    printf("  --help                    Show this help\n");
    printf("\nExamples:\n");
    printf("  %s --create-mode 2336x1080@60\n", program_name);
    printf("  %s --add-mode HDMI1 123456789\n", program_name);
    printf("  %s --remove-mode HDMI1 2336x1080_60.00\n", program_name);
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

// Main entry point with command line argument parsing
int main(int argc, char *argv[]) {
    printf("Tabcaster - C Version with CVT Mode Creation\n");
    
    // Parse command line arguments
    bool list_mode = false;
    bool create_mode = false;
    bool add_mode = false;
    bool remove_mode = false;
    bool delete_mode = false;
    bool reduced_blanking = false;
    
    char *mode_spec = NULL;
    char *output_name = NULL;
    RRMode mode_id = 0;
    
    // Simple argument parsing
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_mode = true;
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
        } else if (strcmp(argv[i], "--reduced-blanking") == 0) {
            reduced_blanking = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
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
    
    if (create_mode) {
        unsigned int width, height;
        double refresh_rate;
        
        if (parse_mode_spec(mode_spec, &width, &height, &refresh_rate) == 0) {
            printf("Creating CVT mode: %ux%u @ %.2f Hz%s\n", 
                   width, height, refresh_rate,
                   reduced_blanking ? " (reduced blanking)" : "");
            
            // Direct libxcvt usage - much simpler!
            RRMode new_mode_id = mode_create_cvt(dm, width, height, refresh_rate, reduced_blanking);
            
            if (new_mode_id != 0) {
                printf("Mode created successfully with ID: %lu\n", new_mode_id);
                printf("To use this mode, add it to an output with:\n");
                printf("  %s --add-mode OUTPUT_NAME %lu\n", argv[0], new_mode_id);
            } else {
                fprintf(stderr, "Failed to create CVT mode\n");
            }
        }
    }
    
    if (add_mode) {
        if (mode_add_to_output(dm, output_name, mode_id) != 0) {
            fprintf(stderr, "Failed to add mode to output\n");
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
    
    // Clean up
    dm_cleanup(dm);
    return 0;
}