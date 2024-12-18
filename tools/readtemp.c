// A Simple utility to display an ASCII graph of SoC temp every second.
// CWD SYSTEMS
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define CPU_TEMP_FILE "/sys/class/thermal/thermal_zone0/temp"

float read_cpu_temp() {
    FILE *fp;
    char tempBuffer[256];
    float temp = -1.0;

    fp = fopen(CPU_TEMP_FILE, "r");
    if (fp == NULL) {
        perror("Could not open temperature file");
        return temp;
    }

    if (fgets(tempBuffer, sizeof(tempBuffer), fp) != NULL) {
        temp = atof(tempBuffer) / 1000.0; 
    }
    fclose(fp);
    return temp;
}

void display_graph(float temp) {
    const int graph_width = 40;
    int temp_level = (int)(temp * 2);  
    
    if (temp_level > graph_width) temp_level = graph_width;
    
    printf("\rCPU Temperature: %.1f°C [", temp);
    for (int i = 0; i < graph_width; ++i) {
        if (i < temp_level) putchar('#');
        else putchar('-');
    }
    printf("] ");
}

int main() {
    float temp;
    while (1) {
        temp = read_cpu_temp();
        if (temp != -1.0) {
            display_graph(temp);
        } else {
            printf("\rFailed to read CPU temperature ");
        }
        fflush(stdout);
        sleep(1); 
    }
    return 0;
}
