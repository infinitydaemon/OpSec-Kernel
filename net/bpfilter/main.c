// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/uio.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include "../../include/uapi/linux/bpf.h"
#include <asm/unistd.h>
#include "msgfmt.h"

#define DEBUG_FILE_PATH "/dev/kmsg"
#define REQ_SIZE sizeof(struct mbox_request)
#define REP_SIZE sizeof(struct mbox_reply)

FILE *debug_f;

/**
 * @brief Handles GET command
 * 
 * @param cmd The mailbox request command
 * @return int The status code of the command execution
 */
static int handle_get_cmd(const struct mbox_request *cmd)
{
	switch (cmd->cmd) {
	case 0:
		return 0;
	default:
		break;
	}
	return -ENOPROTOOPT;
}

/**
 * @brief Handles SET command
 * 
 * @param cmd The mailbox request command
 * @return int The status code of the command execution
 */
static int handle_set_cmd(const struct mbox_request *cmd)
{
	return -ENOPROTOOPT;
}

/**
 * @brief The main loop of the program
 */
static void loop(void)
{
	while (1) {
		struct mbox_request req;
		struct mbox_reply reply;
		ssize_t n;

		n = read(STDIN_FILENO, &req, REQ_SIZE);
		if (n != REQ_SIZE) {
			perror("invalid request");
			return;
		}

		reply.status = req.is_set ?
			handle_set_cmd(&req) :
			handle_get_cmd(&req);

		n = write(STDOUT_FILENO, &reply, REP_SIZE);
		if (n != REP_SIZE) {
			perror("reply failed");
			return;
		}
	}
}

/**
 * @brief The main entry point of the program
 * 
 * @return int The exit code of the program
 */
int main(void)
{
	debug_f = fopen(DEBUG_FILE_PATH, "w");
	if (debug_f == NULL) {
		perror("failed to open debug file");
		return -1;
	}
	setvbuf(debug_f, NULL, _IOLBF, 0);
	fprintf(debug_f, "<5>Started bpfilter\n");
	loop();
	fclose(debug_f);
	return 0;
}
