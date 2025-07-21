#include <stdio.h>
#include <stdlib.h>
#include "display_manager.h"

// Main entry point - enumerate and display monitor information
int main(int argc, char *argv[]) {
    printf("Tabcaster - C Version\n");
    
    // Initialize display manager
    DisplayManager *dm = dm_init();
    if (!dm) {
        fprintf(stderr, "Failed to initialize display manager\n");
        return 1;
    }
    
    // Get monitor information
    int count = dm_get_screens(dm);
    if (count < 0) {
        fprintf(stderr, "Failed to get screen information\n");
        dm_cleanup(dm);
        return 1;
    }
    
    printf("Found %d connected monitor%s\n", count, count == 1 ? "" : "s");
    
    // Display monitor details
    dm_print_screens(dm);
    
    // Clean up
    dm_cleanup(dm);
    return 0;
}