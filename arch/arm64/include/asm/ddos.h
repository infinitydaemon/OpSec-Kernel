#ifndef DDOS_PROTECTION_H
#define DDOS_PROTECTION_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

#define DDOS_MAX_PACKETS_PER_SECOND 1000
#define DDOS_TIMER_INTERVAL_MS 1000

struct ddos_packet {
    struct list_head list;
    unsigned long timestamp;
};

struct ddos_protection {
    spinlock_t lock;
    struct list_head packet_list;
    struct timer_list timer;
    unsigned long packets_per_second;
};

void ddos_protection_init(struct ddos_protection *dp);
void ddos_protection_exit(struct ddos_protection *dp);
void ddos_protection_add_packet(struct ddos_protection *dp, struct ddos_packet *pkt);

#endif /* DDOS_PROTECTION_H */
