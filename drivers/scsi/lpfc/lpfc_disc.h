/*******************************************************************
 * This file is part of the Emulex Linux Device Driver for         *
 * Fibre Channel Host Bus Adapters.                                *
 * Copyright (C) 2017-2022 Broadcom. All Rights Reserved. The term *
 * “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.     *
 * Copyright (C) 2004-2013 Emulex.  All rights reserved.           *
 * EMULEX and SLI are trademarks of Emulex.                        *
 * www.broadcom.com                                                *
 *                                                                 *
 * This program is free software; you can redistribute it and/or   *
 * modify it under the terms of version 2 of the GNU General       *
 * Public License as published by the Free Software Foundation.    *
 * This program is distributed in the hope that it will be useful. *
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND          *
 * WARRANTIES, INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,  *
 * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ARE      *
 * DISCLAIMED, EXCEPT TO THE EXTENT THAT SUCH DISCLAIMERS ARE HELD *
 * TO BE LEGALLY INVALID.  See the GNU General Public License for  *
 * more details, a copy of which can be found in the file COPYING  *
 * included with this package.                                     *
 *******************************************************************/

#define FC_MAX_HOLD_RSCN     32	      /* max number of deferred RSCNs */
#define FC_MAX_NS_RSP        64512    /* max size NameServer rsp */
#define FC_MAXLOOP           126      /* max devices supported on a fc loop */
#define LPFC_DISC_FLOGI_TMO  10	      /* Discovery FLOGI ratov */


/* This is the protocol dependent definition for a Node List Entry.
 * This is used by Fibre Channel protocol to support FCP.
 */

/* worker thread events */
enum lpfc_work_type {
	LPFC_EVT_ONLINE,
	LPFC_EVT_OFFLINE_PREP,
	LPFC_EVT_OFFLINE,
	LPFC_EVT_WARM_START,
	LPFC_EVT_KILL,
	LPFC_EVT_ELS_RETRY,
	LPFC_EVT_DEV_LOSS,
	LPFC_EVT_FASTPATH_MGMT_EVT,
	LPFC_EVT_RESET_HBA,
	LPFC_EVT_RECOVER_PORT
};

/* structure used to queue event to the discovery tasklet */
struct lpfc_work_evt {
	struct list_head      evt_listp;
	void                 *evt_arg1;
	void                 *evt_arg2;
	enum lpfc_work_type   evt;
};

struct lpfc_scsi_check_condition_event;
struct lpfc_scsi_varqueuedepth_event;
struct lpfc_scsi_event_header;
struct lpfc_fabric_event_header;
struct lpfc_fcprdchkerr_event;

/* structure used for sending events from fast path */
struct lpfc_fast_path_event {
	struct lpfc_work_evt work_evt;
	struct lpfc_vport     *vport;
	union {
		struct lpfc_scsi_check_condition_event check_cond_evt;
		struct lpfc_scsi_varqueuedepth_event queue_depth_evt;
		struct lpfc_scsi_event_header scsi_evt;
		struct lpfc_fabric_event_header fabric_evt;
		struct lpfc_fcprdchkerr_event read_check_error;
	} un;
};

#define LPFC_SLI4_MAX_XRI	1024	/* Used to make the ndlp's xri_bitmap */
#define XRI_BITMAP_ULONGS (LPFC_SLI4_MAX_XRI / BITS_PER_LONG)
struct lpfc_node_rrqs {
	unsigned long xri_bitmap[XRI_BITMAP_ULONGS];
};

enum lpfc_fc4_xpt_flags {
	NLP_XPT_REGD		= 0x1,
	SCSI_XPT_REGD		= 0x2,
	NVME_XPT_REGD		= 0x4,
	NVME_XPT_UNREG_WAIT	= 0x8,
	NLP_XPT_HAS_HH		= 0x10
};

enum lpfc_nlp_save_flags {
	/* devloss occurred during recovery */
	NLP_IN_RECOV_POST_DEV_LOSS	= 0x1,
	/* wait for outstanding LOGO to cmpl */
	NLP_WAIT_FOR_LOGO		= 0x2,
	/* wait for outstanding DA_ID to finish */
	NLP_WAIT_FOR_DA_ID              = 0x4
};

struct lpfc_nodelist {
	struct list_head nlp_listp;
	struct serv_parm fc_sparam;		/* buffer for service params */
	struct lpfc_name nlp_portname;
	struct lpfc_name nlp_nodename;

	spinlock_t	lock;			/* Node management lock */

	unsigned long    nlp_flag;		/* entry flags */
	uint32_t         nlp_DID;		/* FC D_ID of entry */
	uint32_t         nlp_last_elscmd;	/* Last ELS cmd sent */
	uint16_t         nlp_type;
#define NLP_FC_NODE        0x1			/* entry is an FC node */
#define NLP_FABRIC         0x4			/* entry rep a Fabric entity */
#define NLP_FCP_TARGET     0x8			/* entry is an FCP target */
#define NLP_FCP_INITIATOR  0x10			/* entry is an FCP Initiator */
#define NLP_NVME_TARGET    0x20			/* entry is a NVME Target */
#define NLP_NVME_INITIATOR 0x40			/* entry is a NVME Initiator */
#define NLP_NVME_DISCOVERY 0x80                 /* entry has NVME disc srvc */

	uint16_t	nlp_fc4_type;		/* FC types node supports. */
						/* Assigned from GID_FF, only
						 * FCP (0x8) and NVME (0x28)
						 * supported.
						 */
#define NLP_FC4_NONE	0x0
#define NLP_FC4_FCP	0x1			/* FC4 Type FCP (value x8)) */
#define NLP_FC4_NVME	0x2			/* FC4 TYPE NVME (value x28) */

	uint16_t        nlp_rpi;
	uint16_t        nlp_state;		/* state transition indicator */
	uint16_t        nlp_prev_state;		/* state transition indicator */
	uint16_t        nlp_xri;		/* output exchange id for RPI */
	uint16_t        nlp_sid;		/* scsi id */
#define NLP_NO_SID		0xffff
	uint16_t	nlp_maxframe;		/* Max RCV frame size */
	uint8_t		nlp_class_sup;		/* Supported Classes */
	uint8_t         nlp_retry;		/* used for ELS retries */
	uint8_t         nlp_fcp_info;	        /* class info, bits 0-3 */
#define NLP_FCP_2_DEVICE   0x10			/* FCP-2 device */
	u8		nlp_nvme_info;	        /* NVME NSLER Support */
	uint8_t		vmid_support;		/* destination VMID support */
#define NLP_NVME_NSLER     0x1			/* NVME NSLER device */

	struct timer_list   nlp_delayfunc;	/* Used for delayed ELS cmds */
	struct lpfc_hba *phba;
	struct fc_rport *rport;		/* scsi_transport_fc port structure */
	struct lpfc_nvme_rport *nrport;	/* nvme transport rport struct. */
	struct lpfc_vport *vport;
	struct lpfc_work_evt els_retry_evt;
	struct lpfc_work_evt dev_loss_evt;
	struct lpfc_work_evt recovery_evt;
	struct kref     kref;
	atomic_t cmd_pending;
	uint32_t cmd_qdepth;
	unsigned long last_change_time;
	unsigned long *active_rrqs_xri_bitmap;
	uint32_t fc4_prli_sent;

	/* flags to keep ndlp alive until special conditions are met */
	enum lpfc_nlp_save_flags save_flags;

	enum lpfc_fc4_xpt_flags fc4_xpt_flags;

	uint32_t nvme_fb_size; /* NVME target's supported byte cnt */
#define NVME_FB_BIT_SHIFT 9    /* PRLI Rsp first burst in 512B units. */
	uint32_t nlp_defer_did;

	/* These wait objects are NPIV specific.  These IOs must complete
	 * synchronously.
	 */
	wait_queue_head_t *logo_waitq;
	wait_queue_head_t *da_id_waitq;
};

struct lpfc_node_rrq {
	struct list_head list;
	uint16_t xritag;
	uint16_t send_rrq;
	uint16_t rxid;
	uint32_t         nlp_DID;		/* FC D_ID of entry */
	struct lpfc_vport *vport;
	unsigned long rrq_stop_time;
};

#define lpfc_ndlp_check_qdepth(phba, ndlp) \
	(ndlp->cmd_qdepth < phba->sli4_hba.max_cfg_param.max_xri)

/* nlp_flag mask bits */
enum lpfc_nlp_flag {
	NLP_IGNR_REG_CMPL  = 0,         /* Rcvd rscn before we cmpl reg login */
	NLP_REG_LOGIN_SEND = 1,         /* sent reglogin to adapter */
	NLP_SUPPRESS_RSP   = 4,         /* Remote NPort supports suppress rsp */
	NLP_PLOGI_SND      = 5,         /* sent PLOGI request for this entry */
	NLP_PRLI_SND       = 6,         /* sent PRLI request for this entry */
	NLP_ADISC_SND      = 7,         /* sent ADISC request for this entry */
	NLP_LOGO_SND       = 8,         /* sent LOGO request for this entry */
	NLP_RNID_SND       = 10,        /* sent RNID request for this entry */
	NLP_NVMET_RECOV    = 12,        /* NVMET auditing node for recovery. */
	NLP_UNREG_INP      = 15,        /* UNREG_RPI cmd is in progress */
	NLP_DROPPED        = 16,        /* Init ref count has been dropped */
	NLP_DELAY_TMO      = 17,        /* delay timeout is running for node */
	NLP_NPR_2B_DISC    = 18,        /* node is included in num_disc_nodes */
	NLP_RCV_PLOGI      = 19,        /* Rcv'ed PLOGI from remote system */
	NLP_LOGO_ACC       = 20,        /* Process LOGO after ACC completes */
	NLP_TGT_NO_SCSIID  = 21,        /* good PRLI but no binding for scsid */
	NLP_ISSUE_LOGO     = 22,        /* waiting to issue a LOGO */
	NLP_IN_DEV_LOSS    = 23,        /* devloss in progress */
	NLP_ACC_REGLOGIN   = 24,        /* Issue Reg Login after successful
					   ACC */
	NLP_NPR_ADISC      = 25,        /* Issue ADISC when dq'ed from
					   NPR list */
	NLP_RM_DFLT_RPI    = 26,        /* need to remove leftover dflt RPI */
	NLP_NODEV_REMOVE   = 27,        /* Defer removal till discovery ends */
	NLP_TARGET_REMOVE  = 28,        /* Target remove in process */
	NLP_SC_REQ         = 29,        /* Target requires authentication */
	NLP_FIRSTBURST     = 30,        /* Target supports FirstBurst */
	NLP_RPI_REGISTERED = 31         /* nlp_rpi is valid */
};

/* There are 4 different double linked lists nodelist entries can reside on.
 * The Port Login (PLOGI) list and Address Discovery (ADISC) list are used
 * when Link Up discovery or Registered State Change Notification (RSCN)
 * processing is needed.  Each list holds the nodes that require a PLOGI or
 * ADISC Extended Link Service (ELS) request.  These lists keep track of the
 * nodes affected by an RSCN, or a Link Up (Typically, all nodes are effected
 * by Link Up) event.  The unmapped_list contains all nodes that have
 * successfully logged into at the Fibre Channel level.  The
 * mapped_list will contain all nodes that are mapped FCP targets.
 *
 * The bind list is a list of undiscovered (potentially non-existent) nodes
 * that we have saved binding information on. This information is used when
 * nodes transition from the unmapped to the mapped list.
 */

/* Defines for nlp_state */
#define NLP_STE_UNUSED_NODE       0x0	/* node is just allocated */
#define NLP_STE_PLOGI_ISSUE       0x1	/* PLOGI was sent to NL_PORT */
#define NLP_STE_ADISC_ISSUE       0x2	/* ADISC was sent to NL_PORT */
#define NLP_STE_REG_LOGIN_ISSUE   0x3	/* REG_LOGIN was issued for NL_PORT */
#define NLP_STE_PRLI_ISSUE        0x4	/* PRLI was sent to NL_PORT */
#define NLP_STE_LOGO_ISSUE	  0x5	/* LOGO was sent to NL_PORT */
#define NLP_STE_UNMAPPED_NODE     0x6	/* PRLI completed from NL_PORT */
#define NLP_STE_MAPPED_NODE       0x7	/* Identified as a FCP Target */
#define NLP_STE_NPR_NODE          0x8	/* NPort disappeared */
#define NLP_STE_MAX_STATE         0x9
#define NLP_STE_FREED_NODE        0xff	/* node entry was freed to MEM_NLP */

/* For UNUSED_NODE state, the node has just been allocated.
 * For PLOGI_ISSUE and REG_LOGIN_ISSUE, the node is on
 * the PLOGI list. For REG_LOGIN_COMPL, the node is taken off the PLOGI list
 * and put on the unmapped list. For ADISC processing, the node is taken off
 * the ADISC list and placed on either the mapped or unmapped list (depending
 * on its previous state). Once on the unmapped list, a PRLI is issued and the
 * state changed to PRLI_ISSUE. When the PRLI completion occurs, the state is
 * changed to PRLI_COMPL. If the completion indicates a mapped
 * node, the node is taken off the unmapped list. The binding list is checked
 * for a valid binding, or a binding is automatically assigned. If binding
 * assignment is unsuccessful, the node is left on the unmapped list. If
 * binding assignment is successful, the associated binding list entry (if
 * any) is removed, and the node is placed on the mapped list.
 */
/*
 * For a Link Down, all nodes on the ADISC, PLOGI, unmapped or mapped
 * lists will receive a DEVICE_RECOVERY event. If the linkdown or devloss timers
 * expire, all effected nodes will receive a DEVICE_RM event.
 */
/*
 * For a Link Up or RSCN, all nodes will move from the mapped / unmapped lists
 * to either the ADISC or PLOGI list.  After a Nameserver query or ALPA loopmap
 * check, additional nodes may be added (DEVICE_ADD) or removed (DEVICE_RM) to /
 * from the PLOGI or ADISC lists. Once the PLOGI and ADISC lists are populated,
 * we will first process the ADISC list.  32 entries are processed initially and
 * ADISC is initited for each one.  Completions / Events for each node are
 * funnelled thru the state machine.  As each node finishes ADISC processing, it
 * starts ADISC for any nodes waiting for ADISC processing. If no nodes are
 * waiting, and the ADISC list count is identically 0, then we are done. For
 * Link Up discovery, since all nodes on the PLOGI list are UNREG_LOGIN'ed, we
 * can issue a CLEAR_LA and reenable Link Events. Next we will process the PLOGI
 * list.  32 entries are processed initially and PLOGI is initited for each one.
 * Completions / Events for each node are funnelled thru the state machine.  As
 * each node finishes PLOGI processing, it starts PLOGI for any nodes waiting
 * for PLOGI processing. If no nodes are waiting, and the PLOGI list count is
 * identically 0, then we are done. We have now completed discovery / RSCN
 * handling. Upon completion, ALL nodes should be on either the mapped or
 * unmapped lists.
 */

/* Defines for Node List Entry Events that could happen */
#define NLP_EVT_RCV_PLOGI         0x0	/* Rcv'd an ELS PLOGI command */
#define NLP_EVT_RCV_PRLI          0x1	/* Rcv'd an ELS PRLI  command */
#define NLP_EVT_RCV_LOGO          0x2	/* Rcv'd an ELS LOGO  command */
#define NLP_EVT_RCV_ADISC         0x3	/* Rcv'd an ELS ADISC command */
#define NLP_EVT_RCV_PDISC         0x4	/* Rcv'd an ELS PDISC command */
#define NLP_EVT_RCV_PRLO          0x5	/* Rcv'd an ELS PRLO  command */
#define NLP_EVT_CMPL_PLOGI        0x6	/* Sent an ELS PLOGI command */
#define NLP_EVT_CMPL_PRLI         0x7	/* Sent an ELS PRLI  command */
#define NLP_EVT_CMPL_LOGO         0x8	/* Sent an ELS LOGO  command */
#define NLP_EVT_CMPL_ADISC        0x9	/* Sent an ELS ADISC command */
#define NLP_EVT_CMPL_REG_LOGIN    0xa	/* REG_LOGIN mbox cmd completed */
#define NLP_EVT_DEVICE_RM         0xb	/* Device not found in NS / ALPAmap */
#define NLP_EVT_DEVICE_RECOVERY   0xc	/* Device existence unknown */
#define NLP_EVT_MAX_EVENT         0xd
#define NLP_EVT_NOTHING_PENDING   0xff
