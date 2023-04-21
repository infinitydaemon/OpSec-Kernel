// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/arch/arm/kernel/arch_timer.c
 *
 *  Copyright (C) 2011 ARM Ltd.
 *  All Rights Reserved
 */

#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <asm/delay.h>
#include <asm/arch_timer.h>
#include <clocksource/arm_arch_timer.h>

// Read the 64-bit architected timer value and return it as a 32-bit integer.
static u32 read_arch_timer(void)
{
    return arch_timer_read_counter();
}

// Structure for delay timer based on the architected timer.
static struct delay_timer arch_delay_timer;

// Register the architected timer as the delay timer.
static void register_arch_timer_delay(void)
{
    arch_delay_timer.read_current_timer = read_arch_timer;
    arch_delay_timer.freq = arch_timer_get_rate();
    register_current_timer_delay(&arch_delay_timer);
}

// Initialize the architected timer and register it as the delay timer.
int initialize_arch_timer(void)
{
    // Get the architected timer rate.
    u32 arch_timer_rate = arch_timer_get_rate();

    if (arch_timer_rate == 0)
    {
        // Return error if the timer rate is 0.
        return -ENXIO;
    }

    // Register the architected timer as the delay timer.
    register_arch_timer_delay();

    return 0;
}
