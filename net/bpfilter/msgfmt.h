/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _NET_BPFILTER_MSGFMT_H
#define _NET_BPFILTER_MSGFMT_H

/* 
 * This header file defines the message formats used in the BPFilter implementation.
 * The message formats are used for communication between user space and kernel space.
 */

/* The message format used for requests */
struct bpfilter_request {
	__u64 buffer_address; /* The address of the buffer that contains the data to be processed */
	__u32 buffer_length;  /* The length of the buffer in bytes */
	__u32 is_set;         /* Flag to indicate if this is a set command (1) or a get command (0) */
	__u32 command;        /* The command to be executed (defined by the BPFilter implementation) */
	__u32 pid;            /* The PID of the process that sent the request */
};

/* The message format used for replies */
struct bpfilter_reply {
	__u32 status; /* The status of the operation (0 for success, negative error code for failure) */
};

struct bpfilter_request {
	__u64 buffer_address; /* The address of the buffer that contains the data to be processed */
	__u32 buffer_length;  /* The length of the buffer in bytes */
	__u32 is_set;         /* Flag to indicate if this is a set command (1) or a get command (0) */
	__u32 command;        /* The command to be executed (defined by the BPFilter implementation) */
	__u32 pid;            /* The PID of the process that sent the request */
	
	/* Check that the buffer length is within a reasonable range */
	static_assert(buffer_length <= MAX_BUFFER_LENGTH, "Buffer length too long");
	
	/* Check that the command is valid */
	static_assert(command >= BP_FILTER_CMD_MIN && command <= BP_FILTER_CMD_MAX, "Invalid command");
	
	/* Check that the process ID is not negative */
	static_assert(pid >= 0, "Invalid PID");
};

/*
 * Adding versioning to the message format can help ensure backward compatibility 
 * if the message format changes in the future.
 * For example, we can add a version field to the message format:
 */

struct bpfilter_request {
	__u32 version;        /* The version of the message format */
	__u32 message_length; /* The total length of the message in bytes */
	__u64 buffer_address; /* The address of the buffer that contains the data to be processed */
	
