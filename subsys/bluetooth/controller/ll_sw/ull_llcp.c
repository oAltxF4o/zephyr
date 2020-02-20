/*
 * Copyright (c) 2020 Demant
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>

#include <bluetooth/hci.h>
#include <sys/byteorder.h>
#include <sys/slist.h>
#include <sys/util.h>

#include "hal/ccm.h"

#include "util/mem.h"
#include "util/memq.h"

#include "pdu.h"
#include "lll.h"
#include "lll_conn.h"

#include "ll.h"
#include "ll_settings.h"

#include "ull_tx_queue.h"
#include "ull_llcp.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_HCI_DRIVER)
#define LOG_MODULE_NAME bt_ctlr_ull_llcp
#include "common/log.h"
#include <soc.h>
#include "hal/debug.h"

/* LLCP Local Procedure Common FSM states */
enum {
	LP_COMMON_STATE_IDLE,
	LP_COMMON_STATE_WAIT_TX,
	LP_COMMON_STATE_WAIT_RX,
	LP_COMMON_STATE_WAIT_NTF,
};

/* LLCP Local Procedure Common FSM events */
enum {
	/* Procedure run */
	LP_COMMON_EVT_RUN,

	/* Response recieved */
	LP_COMMON_EVT_RESPONSE,

	/* Reject response recieved */
	LP_COMMON_EVT_REJECT,

	/* Unknown response recieved */
	LP_COMMON_EVT_UNKNOWN,

	/* Instant collision detected */
	LP_COMMON_EVT_COLLISION,
};

/* LLCP Remote Procedure Common FSM states */
enum {
	RP_COMMON_STATE_IDLE,
	RP_COMMON_STATE_WAIT_RX,
	RP_COMMON_STATE_WAIT_TX,
	RP_COMMON_STATE_WAIT_NTF,
};

/* LLCP Remote Procedure Common FSM events */
enum {
	/* Procedure run */
	RP_COMMON_EVT_RUN,

	/* Request recieved */
	RP_COMMON_EVT_REQUEST,
};

/* LLCP Procedure */
enum {
	PROC_UNKNOWN,
	PROC_VERSION_EXCHANGE
};

/* LLCP Local Request FSM State */
enum {
	LR_STATE_IDLE,
	LR_STATE_ACTIVE,
	LR_STATE_DISCONNECT
};

/* LLCP Local Request FSM Event */
enum {
	/* Procedure run */
	LR_EVT_RUN,

	/* Procedure completed */
	LR_EVT_COMPLETE,

	/* Link connected */
	LR_EVT_CONNECT,

	/* Link disconnected */
	LR_EVT_DISCONNECT,
};

/* LLCP Remote Request FSM State */
enum {
	RR_STATE_IDLE,
	RR_STATE_ACTIVE,
	RR_STATE_DISCONNECT
};

/* LLCP Remote Request FSM Event */
enum {
	/* Procedure run */
	RR_EVT_RUN,

	/* Procedure completed */
	RR_EVT_COMPLETE,

	/* Link connected */
	RR_EVT_CONNECT,

	/* Link disconnected */
	RR_EVT_DISCONNECT,
};

/* LLCP Procedure Context */
struct proc_ctx {
	/* Must be the first for sys_slist to work */
	sys_snode_t node;

	/* PROC_ */
	u8_t proc;

	/* LP_STATE_ */
	u8_t state;

	/* Expected opcode to be recieved next */
	u8_t opcode;

	/* Instant collision */
	int collision;

	/* Procedure pause */
	int pause;
};

/* LLCP Memory Pool Descriptor */
struct mem_pool {
	void *free;
	u8_t *pool;
};

#define LLCTRL_PDU_SIZE		(offsetof(struct pdu_data, llctrl) + sizeof(struct pdu_data_llctrl))
#define PROC_CTX_BUF_SIZE	WB_UP(sizeof(struct proc_ctx))
#define TX_CTRL_BUF_SIZE	WB_UP(offsetof(struct node_tx, pdu) + LLCTRL_PDU_SIZE)
#define NTF_BUF_SIZE		WB_UP(offsetof(struct node_rx_pdu, pdu) + LLCTRL_PDU_SIZE)

/* LLCP Allocations */

/* TODO(thoh): Placeholder until Kconfig setting is made */
#if !defined(TX_CTRL_BUF_NUM)
#define TX_CTRL_BUF_NUM 1
#endif

/* TODO(thoh): Placeholder until Kconfig setting is made */
#if !defined(NTF_BUF_NUM)
#define NTF_BUF_NUM 1
#endif

/* TODO(thoh): Placeholder until Kconfig setting is made */
#if !defined(PROC_CTX_BUF_NUM)
#define PROC_CTX_BUF_NUM 1
#endif

/* TODO: Determine correct number of tx nodes */
static u8_t buffer_mem_tx[TX_CTRL_BUF_SIZE * TX_CTRL_BUF_NUM];
static struct mem_pool mem_tx = { .pool = buffer_mem_tx };

/* TODO: Determine correct number of ntf nodes */
static u8_t buffer_mem_ntf[NTF_BUF_SIZE * NTF_BUF_NUM];
static struct mem_pool mem_ntf = { .pool = buffer_mem_ntf };

/* TODO: Determine correct number of ctx */
static u8_t buffer_mem_ctx[PROC_CTX_BUF_SIZE * PROC_CTX_BUF_NUM];
static struct mem_pool mem_ctx = { .pool = buffer_mem_ctx };

/*
 * LLCP Resource Management
 */

static struct proc_ctx *proc_ctx_acquire(void)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *) mem_acquire(&mem_ctx.free);
	return ctx;
}

static void proc_ctx_release(struct proc_ctx *ctx)
{
	mem_release(ctx, &mem_ctx.free);
}

static bool tx_alloc_peek(void)
{
	u16_t mem_free_count;

	mem_free_count = mem_free_count_get(mem_tx.free);
	return mem_free_count > 0;
}

static struct node_tx *tx_alloc(void)
{
	struct node_tx *tx;

	tx = (struct node_tx *) mem_acquire(&mem_tx.free);
	return tx;
}

static bool ntf_alloc_peek(void)
{
	u16_t mem_free_count;

	mem_free_count = mem_free_count_get(mem_ntf.free);
	return mem_free_count > 0;
}

static struct node_rx_pdu *ntf_alloc(void)
{
	struct node_rx_pdu *ntf;

	ntf = (struct node_rx_pdu *) mem_acquire(&mem_ntf.free);
	return ntf;
}

/*
 * ULL -> LLL Interface
 */

static void ull_tx_enqueue(struct ull_cp_conn *conn, struct node_tx *tx)
{
	ull_tx_q_enqueue_ctrl(conn->tx_q, tx);
}

/*
 * ULL -> LL Interface
 */

static void ll_rx_enqueue(struct node_rx_pdu *rx)
{
#if !defined(ULL_LLCP_UNITTEST)
	/* TODO:
	 * Implement correct ULL->LL path
	 * */
#else
	/* UNIT TEST SOLUTION TO AVOID INCLUDING THE WORLD */
	extern sys_slist_t ll_rx_q;
	sys_slist_append(&ll_rx_q, (sys_snode_t *) rx);
#endif
}

/*
 * LLCP Procedure Creation
 */

static struct proc_ctx *create_procedure(u8_t proc)
{
	struct proc_ctx *ctx;

	ctx = proc_ctx_acquire();
	if (!ctx) {
		return NULL;
	}

	ctx->proc = proc;
	/* TODO: Fix the initial state */
	ctx->state = LP_COMMON_STATE_IDLE;
	ctx->collision = 0U;
	ctx->pause = 0U;

	return ctx;
}

/*
 * Version Exchange Procedure Helper
 */

static void pdu_encode_version_ind(struct pdu_data *pdu)
{
	u16_t cid;
	u16_t svn;
	struct pdu_data_llctrl_version_ind *p;


	pdu->ll_id = PDU_DATA_LLID_CTRL;
	pdu->len = offsetof(struct pdu_data_llctrl, version_ind) + sizeof(struct pdu_data_llctrl_version_ind);
	pdu->llctrl.opcode = PDU_DATA_LLCTRL_TYPE_VERSION_IND;

	p = &pdu->llctrl.version_ind;
	p->version_number = LL_VERSION_NUMBER;
	cid = sys_cpu_to_le16(ll_settings_company_id());
	svn = sys_cpu_to_le16(ll_settings_subversion_number());
	p->company_id = cid;
	p->sub_version_number = svn;
}

static void ntf_encode_version_ind(struct ull_cp_conn *conn, struct pdu_data *pdu)
{
	struct pdu_data_llctrl_version_ind *p;


	pdu->ll_id = PDU_DATA_LLID_CTRL;
	pdu->len = offsetof(struct pdu_data_llctrl, version_ind) + sizeof(struct pdu_data_llctrl_version_ind);
	pdu->llctrl.opcode = PDU_DATA_LLCTRL_TYPE_VERSION_IND;

	p = &pdu->llctrl.version_ind;
	p->version_number = conn->vex.cached.version_number;
	p->company_id = sys_cpu_to_le16(conn->vex.cached.company_id);
	p->sub_version_number = sys_cpu_to_le16(conn->vex.cached.sub_version_number);
}

static void pdu_decode_version_ind(struct ull_cp_conn *conn, struct pdu_data *pdu)
{
	conn->vex.valid = 1;
	conn->vex.cached.version_number = pdu->llctrl.version_ind.version_number;
	conn->vex.cached.company_id = sys_le16_to_cpu(pdu->llctrl.version_ind.company_id);
	conn->vex.cached.sub_version_number = sys_le16_to_cpu(pdu->llctrl.version_ind.sub_version_number);
}

/*
 * LLCP Local Procedure Common FSM
 */

static void lr_complete(struct ull_cp_conn *conn);

static void lp_comm_tx(struct ull_cp_conn *conn, struct proc_ctx *ctx)
{
	struct node_tx *tx;
	struct pdu_data *pdu;

	/* Allocate tx node */
	tx = tx_alloc();
	LL_ASSERT(tx);

	pdu = (struct pdu_data *)tx->pdu;

	/* Encode LL Control PDU */
	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		pdu_encode_version_ind(pdu);
		ctx->opcode = PDU_DATA_LLCTRL_TYPE_VERSION_IND;
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}

	/* Enqueue LL Control PDU towards LLL */
	ull_tx_enqueue(conn, tx);
}

static void lp_comm_ntf(struct ull_cp_conn *conn, struct proc_ctx *ctx)
{
	struct node_rx_pdu *ntf;
	struct pdu_data *pdu;

	/* Allocate ntf node */
	ntf = ntf_alloc();
	LL_ASSERT(ntf);

	pdu = (struct pdu_data *) ntf->pdu;

	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		ntf_encode_version_ind(conn, pdu);
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}

	/* Enqueue notification towards LL */
	ll_rx_enqueue(ntf);
}

static void lp_comm_complete(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		if (!ntf_alloc_peek()) {
			ctx->state = LP_COMMON_STATE_WAIT_NTF;
		} else {
			lp_comm_ntf(conn, ctx);
			lr_complete(conn);
			ctx->state = LP_COMMON_STATE_IDLE;
		}
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}
}

static void lp_comm_send_req(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		/* The Link Layer shall only queue for transmission a maximum of one LL_VERSION_IND PDU during a connection. */
		if (!conn->vex.sent) {
			if (!tx_alloc_peek() || ctx->pause) {
				ctx->state = LP_COMMON_STATE_WAIT_TX;
			} else {
				lp_comm_tx(conn, ctx);
				conn->vex.sent = 1;
				ctx->state = LP_COMMON_STATE_WAIT_RX;
			}
		} else {
			lp_comm_complete(conn, ctx, evt, param);
		}
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}
}

static void lp_comm_st_idle(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (evt) {
	case LP_COMMON_EVT_RUN:
		if (ctx->pause) {
			ctx->state = LP_COMMON_STATE_WAIT_TX;
		} else {
			lp_comm_send_req(conn, ctx, evt, param);
		}
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_comm_st_wait_tx(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	/* TODO */
}

static void lp_comm_rx_decode(struct ull_cp_conn *conn, struct pdu_data *pdu)
{
	switch (pdu->llctrl.opcode) {
	case PDU_DATA_LLCTRL_TYPE_VERSION_IND:
		pdu_decode_version_ind(conn, pdu);
		break;
	default:
		/* Unknown opcode */
		LL_ASSERT(0);
	}
}

static void lp_comm_st_wait_rx(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (evt) {
	case LP_COMMON_EVT_RESPONSE:
		lp_comm_rx_decode(conn, (struct pdu_data *) param);
		lp_comm_complete(conn, ctx, evt, param);
	default:
		/* Ignore other evts */
		break;
	}
}

static void lp_comm_st_wait_ntf(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	/* TODO */
}

static void lp_comm_execute_fsm(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (ctx->state) {
	case LP_COMMON_STATE_IDLE:
		lp_comm_st_idle(conn, ctx, evt, param);
		break;
	case LP_COMMON_STATE_WAIT_TX:
		lp_comm_st_wait_tx(conn, ctx, evt, param);
		break;
	case LP_COMMON_STATE_WAIT_RX:
		lp_comm_st_wait_rx(conn, ctx, evt, param);
		break;
	case LP_COMMON_STATE_WAIT_NTF:
		lp_comm_st_wait_ntf(conn, ctx, evt, param);
		break;
	default:
		/* Unknown state */
		LL_ASSERT(0);
	}
}

/*
 * LLCP Local Request FSM
 */

static void lr_enqueue(struct ull_cp_conn *conn, struct proc_ctx *ctx)
{
	sys_slist_append(&conn->local.pend_proc_list, &ctx->node);
}

static struct proc_ctx *lr_dequeue(struct ull_cp_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *) sys_slist_get(&conn->local.pend_proc_list);
	return ctx;
}

static struct proc_ctx *lr_peek(struct ull_cp_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *) sys_slist_peek_head(&conn->local.pend_proc_list);
	return ctx;
}

static void lr_rx(struct ull_cp_conn *conn, struct proc_ctx *ctx, struct node_rx_pdu *rx)
{
	lp_comm_execute_fsm(conn, ctx, LP_COMMON_EVT_RESPONSE, rx->pdu);
}

static void lr_act_run(struct ull_cp_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = lr_peek(conn);

	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		/* TODO */
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}

	lp_comm_execute_fsm(conn, ctx, LP_COMMON_EVT_RUN, NULL);
}

static void lr_act_complete(struct ull_cp_conn *conn)
{
	/* Dequeue pending request that just completed */
	(void) lr_dequeue(conn);
}

static void lr_act_connect(struct ull_cp_conn *conn)
{
	/* TODO */
}

static void lr_act_disconnect(struct ull_cp_conn *conn)
{
	lr_dequeue(conn);
}

static void lr_st_disconnect(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (evt) {
	case LR_EVT_CONNECT:
		lr_act_connect(conn);
		conn->local.state = LR_STATE_IDLE;
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lr_st_idle(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (evt) {
	case LR_EVT_RUN:
		if (lr_peek(conn)) {
			lr_act_run(conn);
			conn->local.state = LR_STATE_ACTIVE;
		}
		break;
	case LR_EVT_DISCONNECT:
		lr_act_disconnect(conn);
		conn->local.state = LR_STATE_DISCONNECT;
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lr_st_active(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (evt) {
	case LR_EVT_COMPLETE:
		lr_act_complete(conn);
		conn->local.state = LR_STATE_IDLE;
		break;
	case LR_EVT_DISCONNECT:
		lr_act_disconnect(conn);
		conn->local.state = LR_STATE_DISCONNECT;
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void lr_execute_fsm(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (conn->local.state) {
	case LR_STATE_DISCONNECT:
		lr_st_disconnect(conn, evt, param);
		break;
	case LR_STATE_IDLE:
		lr_st_idle(conn, evt, param);
		break;
	case LR_STATE_ACTIVE:
		lr_st_active(conn, evt, param);
		break;
	default:
		/* Unknown state */
		LL_ASSERT(0);
	}
}

static void lr_run(struct ull_cp_conn *conn)
{
	lr_execute_fsm(conn, LR_EVT_RUN, NULL);
}

static void lr_complete(struct ull_cp_conn *conn)
{
	lr_execute_fsm(conn, LR_EVT_COMPLETE, NULL);
}

static void lr_connect(struct ull_cp_conn *conn)
{
	lr_execute_fsm(conn, LR_EVT_CONNECT, NULL);
}

static void lr_disconnect(struct ull_cp_conn *conn)
{
	lr_execute_fsm(conn, LR_EVT_DISCONNECT, NULL);
}

/*
 * LLCP Remote Procedure Common FSM
 */

static void rr_complete(struct ull_cp_conn *conn);

static void rp_comm_rx_decode(struct ull_cp_conn *conn, struct pdu_data *pdu)
{
	switch (pdu->llctrl.opcode) {
	case PDU_DATA_LLCTRL_TYPE_VERSION_IND:
		pdu_decode_version_ind(conn, pdu);
		break;
	default:
		/* Unknown opcode */
		LL_ASSERT(0);
	}
}

static void rp_comm_tx(struct ull_cp_conn *conn, struct proc_ctx *ctx)
{
	struct node_tx *tx;
	struct pdu_data *pdu;

	/* Allocate tx node */
	tx = tx_alloc();
	LL_ASSERT(tx);

	pdu = (struct pdu_data *)tx->pdu;

	/* Encode LL Control PDU */
	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		pdu_encode_version_ind(pdu);
		ctx->opcode = PDU_DATA_LLCTRL_TYPE_VERSION_IND;
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}

	/* Enqueue LL Control PDU towards LLL */
	ull_tx_enqueue(conn, tx);
}

static void rp_comm_st_idle(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (evt) {
	case RP_COMMON_EVT_RUN:
		ctx->state = RP_COMMON_STATE_WAIT_RX;
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_comm_send_rsp(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		/* The Link Layer shall only queue for transmission a maximum of one LL_VERSION_IND PDU during a connection. */
		if (!conn->vex.sent) {
			if (!tx_alloc_peek() || ctx->pause) {
				ctx->state = RP_COMMON_STATE_WAIT_TX;
			} else {
				rp_comm_tx(conn, ctx);
				conn->vex.sent = 1;
				rr_complete(conn);
				ctx->state = RP_COMMON_STATE_IDLE;
			}
		} else {
			/* Protocol Error.
			 *
			 * A procedure already sent a LL_VERSION_IND and recieved a LL_VERSION_IND.
			 */
			/* TODO */
			LL_ASSERT(0);
		}
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}
}

static void rp_comm_st_wait_rx(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (evt) {
	case RP_COMMON_EVT_REQUEST:
		rp_comm_rx_decode(conn, (struct pdu_data *) param);
		if (ctx->pause) {
			ctx->state = RP_COMMON_STATE_WAIT_TX;
		} else {
			rp_comm_send_rsp(conn, ctx, evt, param);
		}
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rp_comm_st_wait_tx(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	/* TODO */
}

static void rp_comm_st_wait_ntf(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	/* TODO */
}

static void rp_comm_execute_fsm(struct ull_cp_conn *conn, struct proc_ctx *ctx, u8_t evt, void *param)
{
	switch (ctx->state) {
	case RP_COMMON_STATE_IDLE:
		rp_comm_st_idle(conn, ctx, evt, param);
		break;
	case RP_COMMON_STATE_WAIT_RX:
		rp_comm_st_wait_rx(conn, ctx, evt, param);
		break;
	case RP_COMMON_STATE_WAIT_TX:
		rp_comm_st_wait_tx(conn, ctx, evt, param);
		break;
	case RP_COMMON_STATE_WAIT_NTF:
		rp_comm_st_wait_ntf(conn, ctx, evt, param);
		break;
	default:
		/* Unknown state */
		LL_ASSERT(0);
	}
}

/*
 * LLCP Remote Request FSM
 */

static void rr_enqueue(struct ull_cp_conn *conn, struct proc_ctx *ctx)
{
	sys_slist_append(&conn->remote.pend_proc_list, &ctx->node);
}

static struct proc_ctx *rr_dequeue(struct ull_cp_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *) sys_slist_get(&conn->remote.pend_proc_list);
	return ctx;
}

static struct proc_ctx *rr_peek(struct ull_cp_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *) sys_slist_peek_head(&conn->remote.pend_proc_list);
	return ctx;
}

static void rr_rx(struct ull_cp_conn *conn, struct proc_ctx *ctx, struct node_rx_pdu *rx)
{
	rp_comm_execute_fsm(conn, ctx, RP_COMMON_EVT_REQUEST, rx->pdu);
}

static void rr_act_run(struct ull_cp_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = rr_peek(conn);

	switch (ctx->proc) {
	case PROC_VERSION_EXCHANGE:
		/* TODO */
		break;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
	}

	rp_comm_execute_fsm(conn, ctx, RP_COMMON_EVT_RUN, NULL);
}

static void rr_act_complete(struct ull_cp_conn *conn)
{
	/* Dequeue pending request that just completed */
	(void) rr_dequeue(conn);
}

static void rr_act_connect(struct ull_cp_conn *conn)
{
	/* TODO */
}

static void rr_act_disconnect(struct ull_cp_conn *conn)
{
	rr_dequeue(conn);
}

static void rr_st_disconnect(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (evt) {
	case RR_EVT_CONNECT:
		rr_act_connect(conn);
		conn->remote.state = RR_STATE_IDLE;
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rr_st_idle(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (evt) {
	case RR_EVT_RUN:
		if (rr_peek(conn)) {
			rr_act_run(conn);
			conn->remote.state = RR_STATE_ACTIVE;
		}
		break;
	case RR_EVT_DISCONNECT:
		rr_act_disconnect(conn);
		conn->remote.state = RR_STATE_DISCONNECT;
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rr_st_active(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (evt) {
	case RR_EVT_COMPLETE:
		rr_act_complete(conn);
		conn->remote.state = RR_STATE_IDLE;
		break;
	case RR_EVT_DISCONNECT:
		rr_act_disconnect(conn);
		conn->remote.state = RR_STATE_DISCONNECT;
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rr_execute_fsm(struct ull_cp_conn *conn, u8_t evt, void *param)
{
	switch (conn->remote.state) {
	case RR_STATE_DISCONNECT:
		rr_st_disconnect(conn, evt, param);
		break;
	case RR_STATE_IDLE:
		rr_st_idle(conn, evt, param);
		break;
	case RR_STATE_ACTIVE:
		rr_st_active(conn, evt, param);
		break;
	default:
		/* Unknown state */
		LL_ASSERT(0);
	}
}

static void rr_run(struct ull_cp_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_RUN, NULL);
}

static void rr_complete(struct ull_cp_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_COMPLETE, NULL);
}

static void rr_connect(struct ull_cp_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_CONNECT, NULL);
}

static void rr_disconnect(struct ull_cp_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_DISCONNECT, NULL);
}

static void rr_new(struct ull_cp_conn *conn, struct node_rx_pdu *rx)
{
	struct proc_ctx *ctx;
	struct pdu_data *pdu;
	u8_t proc;

	pdu = (struct pdu_data *) rx->pdu;

	switch (pdu->llctrl.opcode) {
	case PDU_DATA_LLCTRL_TYPE_VERSION_IND:
		proc = PROC_VERSION_EXCHANGE;
		break;
	default:
		/* Unknown opcode */
		LL_ASSERT(0);
	}

	ctx = create_procedure(proc);
	if (!ctx) {
		return;
	}

	/* Enqueue procedure */
	rr_enqueue(conn, ctx);

	/* Prepare procedure */
	rr_run(conn);

	/* Handle PDU */
	ctx = rr_peek(conn);
	rr_rx(conn, ctx, rx);
}

/*
 * LLCP Public API
 */

void ull_cp_init(void)
{
	/**/
	mem_init(mem_ctx.pool, PROC_CTX_BUF_SIZE, PROC_CTX_BUF_NUM, &mem_ctx.free);
	mem_init(mem_tx.pool, TX_CTRL_BUF_SIZE, TX_CTRL_BUF_NUM, &mem_tx.free);
	mem_init(mem_ntf.pool, NTF_BUF_SIZE, NTF_BUF_NUM, &mem_ntf.free);
}

void ull_cp_conn_init(struct ull_cp_conn *conn)
{
	/* Reset local request fsm */
	conn->local.state = LR_STATE_DISCONNECT;
	sys_slist_init(&conn->local.pend_proc_list);

	/* Reset remote request fsm */
	conn->remote.state = RR_STATE_DISCONNECT;
	sys_slist_init(&conn->remote.pend_proc_list);

	/* Reset the cached version Information (PROC_VERSION_EXCHANGE) */
	memset(&conn->vex, 0, sizeof(conn->vex));
}

void ull_cp_run(struct ull_cp_conn *conn)
{
	rr_run(conn);
	lr_run(conn);
}

void ull_cp_connect(struct ull_cp_conn *conn)
{
	rr_connect(conn);
	lr_connect(conn);
}

void ull_cp_disconnect(struct ull_cp_conn *conn)
{
	rr_disconnect(conn);
	lr_disconnect(conn);
}

u8_t ull_cp_version_exchange(struct ull_cp_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = create_procedure(PROC_VERSION_EXCHANGE);
	if (!ctx) {
		return BT_HCI_ERR_CMD_DISALLOWED;
	}

	lr_enqueue(conn, ctx);

	return BT_HCI_ERR_SUCCESS;
}

void ull_cp_rx(struct ull_cp_conn *conn, struct node_rx_pdu *rx)
{
	struct pdu_data *pdu;
	struct proc_ctx *ctx;

	pdu = (struct pdu_data *) rx->pdu;

	/* TODO(thoh):
	 * Could be optimized by storing the active local opcode in ull_cp_conn,
	 * and then move the peek into lr_rx() */
	ctx = lr_peek(conn);
	if (ctx && ctx->opcode == pdu->llctrl.opcode) {
		/* Response on local procedure */
		lr_rx(conn, ctx, rx);
		return;
	}

	/* TODO(thoh):
	 * Could be optimized by storing the active remote opcode in ull_cp_conn,
	 * and then move the peek into rr_rx() */
	ctx = rr_peek(conn);
	if (ctx && ctx->opcode == pdu->llctrl.opcode) {
		/* Response on remote procedure */
		rr_rx(conn, ctx, rx);
		return;
	}

	switch (pdu->llctrl.opcode) {
	default:
		/* New remote request */
		rr_new(conn, rx);
	}
}