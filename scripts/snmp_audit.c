#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#define PORT 161
#define TIMEOUT 1000000 // Timeout for SNMP requests in microseconds

void scan_subnet(char *subnet) {
    int sockfd;
    struct sockaddr_in dest;
    struct hostent *host;
    char ip[16];

    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    // Scan subnet for SNMP devices
    printf("Scanning subnet %s for SNMP devices...\n", subnet);
    for (int i = 1; i <= 254; i++) {
        sprintf(ip, "%s.%d", subnet, i);
        dest.sin_family = AF_INET;
        dest.sin_port = htons(PORT);
        dest.sin_addr.s_addr = inet_addr(ip);

        // Send SNMP request to device
        if (sendto(sockfd, NULL, 0, 0, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
            printf("No response from %s\n", ip);
            continue;
        }

        // Check SNMP audit
        snmp_sess_init(&sess);
        sess.peername = strdup(ip);
        sess.version = SNMP_VERSION_2c;
        sess.community = "public";
        sess.community_len = strlen(sess.community);

        // Open SNMP session
        ss = snmp_open(&sess);
        if (!ss) {
            fprintf(stderr, "Could not open SNMP session to %s\n", ip);
            continue;
        }

        // Get SNMP audit
        netsnmp_variable_list *vars;
        oid sysuptime_oid[] = {1,3,6,1,2,1,1,3,0}; // SNMP OID for system uptime
        snmp_pdu *pdu = snmp_pdu_create(SNMP_MSG_GET);
        snmp_add_null_var(pdu, sysuptime_oid, sizeof(sysuptime_oid)/sizeof(oid));

        // Send SNMP GET request
        status = snmp_synch_response(ss, pdu, &response);
        if (status != STAT_SUCCESS) {
            fprintf(stderr, "SNMP GET error from %s\n", ip);
            snmp_close(ss);
            continue;
        }

        // Check SNMP response
        if (!response) {
            fprintf(stderr, "No SNMP response from %s\n", ip);
            snmp_close(ss);
            continue;
        }

        // Process SNMP response
        if (response->errstat == SNMP_ERR_NOERROR) {
            printf("SNMP audit from %s:\n", ip);
            for (vars = response->variables; vars; vars = vars->next_variable) {
                print_variable(vars->name, vars->name_length, vars);
            }
        } else {
            fprintf(stderr, "Error in SNMP response from %s\n", ip);
        }

        // Clean up
        snmp_free_pdu(response);
        snmp_close(ss);
    }

    close(sockfd);
}

int main() {
    char subnet[16];
    
    // Prompt user for LAN subnet
    printf("Enter your LAN subnet (e.g., 192.168.1): ");
    scanf("%15s", subnet);

    // Scan subnet for SNMP devices and check SNMP audit
    scan_subnet(subnet);

    return 0;
}
