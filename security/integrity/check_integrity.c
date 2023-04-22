#include <stdio.h>
#include <linux/ima.h>
#include <linux/integrity.h>

int main() {
    struct integrity_iint_cache iint;
    struct ima_event_data event_data = {.iint = &iint};
    struct ima_template_desc template_desc;
    int result;

    result = ima_init();
    if (result) {
        fprintf(stderr, "Failed to initialize IMA\n");
        return 1;
    }

    result = ima_measure_file("path/to/file", &event_data, &template_desc);
    if (result) {
        fprintf(stderr, "Failed to measure file\n");
        return 1;
    }

    result = ima_appraise_measurement(&event_data, &template_desc);
    if (result) {
        fprintf(stderr, "File has been modified\n");
        return 1;
    }

    printf("File is intact\n");
    return 0;
}
