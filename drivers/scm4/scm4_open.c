#include <stdio.h>
#include <stdlib.h>
#include <zymbit/scm4.h>  // Replace with the actual SCM4 library header

int main() {
    // Initialize the SCM4 board
    if (scm4_init() != SCM4_SUCCESS) {
        fprintf(stderr, "Error initializing SCM4\n");
        return EXIT_FAILURE;
    }

    // Open encryption on the SCM4 board
    int encryption_status = scm4_open_encryption();
    if (encryption_status == SCM4_SUCCESS) {
        printf("Encryption opened successfully.\n");
    } else {
        fprintf(stderr, "Error opening encryption: %d\n", encryption_status);
    }

    // Cleanup resources
    scm4_cleanup();

    return EXIT_SUCCESS;
}
