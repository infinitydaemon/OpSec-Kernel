// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2020 Google LLC.
 */
#include <linux/lsm_hooks.h>
#include <linux/bpf_lsm.h>

static int bpf_lsm_inode_free_security(struct inode *inode);
static void bpf_lsm_task_free(struct task_struct *task);

static struct security_hook_list bpf_lsm_hooks[] __lsm_ro_after_init = {
	LSM_HOOK_INIT(bprm_check_security, bpf_lsm_bprm_check_security),
	LSM_HOOK_INIT(file_mmap_security, bpf_lsm_file_mmap_security),
	LSM_HOOK_INIT(inode_alloc_security, bpf_lsm_inode_alloc_security),
	LSM_HOOK_INIT(inode_free_security, bpf_lsm_inode_free_security),
	LSM_HOOK_INIT(inode_init_security, bpf_lsm_inode_init_security),
	LSM_HOOK_INIT(ipc_permission, bpf_lsm_ipc_permission),
	LSM_HOOK_INIT(socket_post_create, bpf_lsm_socket_post_create),
	LSM_HOOK_INIT(socket_socketpair, bpf_lsm_socket_socketpair),
	LSM_HOOK_INIT(socket_connect, bpf_lsm_socket_connect),
	LSM_HOOK_INIT(socket_bind, bpf_lsm_socket_bind),
	LSM_HOOK_INIT(socket_listen, bpf_lsm_socket_listen),
	LSM_HOOK_INIT(socket_accept, bpf_lsm_socket_accept),
	LSM_HOOK_INIT(socket_sendmsg, bpf_lsm_socket_sendmsg),
	LSM_HOOK_INIT(socket_recvmsg, bpf_lsm_socket_recvmsg),
	LSM_HOOK_INIT(socket_getsockname, bpf_lsm_socket_getsockname),
	LSM_HOOK_INIT(socket_getpeername, bpf_lsm_socket_getpeername),
	LSM_HOOK_INIT(socket_getsockopt, bpf_lsm_socket_getsockopt),
	LSM_HOOK_INIT(socket_setsockopt, bpf_lsm_socket_setsockopt),
	LSM_HOOK_INIT(socket_shutdown, bpf_lsm_socket_shutdown),
	LSM_HOOK_INIT(socket_sock_rcv_skb, bpf_lsm_socket_sock_rcv_skb),
	LSM_HOOK_INIT(inet_csk_accept, bpf_lsm_inet_csk_accept),
	LSM_HOOK_INIT(inet_csk_listen_start, bpf_lsm_inet_csk_listen_start),
	LSM_HOOK_INIT(inet6_csk_accept, bpf_lsm_inet6_csk_accept),
	LSM_HOOK_INIT(inet6_csk_listen_start, bpf_lsm_inet6_csk_listen_start),
	LSM_HOOK_INIT(task_create, bpf_lsm_task_create),
	LSM_HOOK_INIT(task_free, bpf_lsm_task_free),
};

static int bpf_lsm_inode_free_security(struct inode *inode)
{
	bpf_inode_storage_free(inode);
	return 0;
}

static void bpf_lsm_task_free(struct task_struct *task)
{
	bpf_task_storage_free(task);
}

static int __init bpf_lsm_init(void)
{
	security_add_hooks(bpf_lsm_hooks, ARRAY_SIZE(bpf_lsm_hooks), "bpf");
	pr_info("LSM support for eBPF active\n");
	return 0;
}

struct lsm_blob_sizes bpf_lsm_blob_sizes __lsm_ro_after_init = {
	.lbs_inode = sizeof(struct bpf_storage_blob),
	.lbs_task = sizeof(struct bpf_storage_blob),
};

DEFINE_LSM(bpf) = {
	.name = "bpf",
	.init = bpf_lsm_init,
	.blobs = &bpf_lsm_blob_sizes
};
