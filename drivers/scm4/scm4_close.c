#include <stdio.h>
#include <stdlib.h>
#include <zymbit/scm4.h> 

int main() {
    // Initialize the SCM4 board
    if (scm4_init() != SCM4_SUCCESS) {
        fprintf(stderr, "Error initializing SCM4\n");
        return EXIT_FAILURE;
    }

    // Lock encryption on the SCM4 board
    int result = scm4_lock_encryption();
    if (result == SCM4_SUCCESS) {
        printf("Encryption locked on SCM4.\n");
    } else {
        fprintf(stderr, "Error locking encryption on SCM4: %d\n", result);
    }

    // Cleanup resources
    scm4_cleanup();

    return EXIT_SUCCESS;
}
