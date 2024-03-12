#include <zymbit/scm4.h>  // Replace with actual SCM4 library header

int main() {
    // Initialize the SCM4 board
    if (scm4_init() != SCM4_SUCCESS) {
        fprintf(stderr, "Error initializing SCM4\n");
        return EXIT_FAILURE;
    }

    // Get the status of the SCM4 board
    int status = scm4_get_status();
    if (status == SCM4_SUCCESS) {
        printf("SCM4 board is operational.\n");
    } else {
        fprintf(stderr, "Error getting SCM4 status: %d\n", status);
    }

    // Cleanup resources
    scm4_cleanup();

    return EXIT_SUCCESS;
}
