#ifndef ARES_DUAL_PROTOCOL_H
#define ARES_DUAL_PROTOCOL_H

#include <zephyr/net_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include "ares/protocol/ares_protocol.h"

void ares_dual_protocol_handle(struct AresProtocol *protocol, struct net_buf *buf);
void ares_dual_protocol_event(struct AresProtocol *protocol, enum AresProtocolEvent event);
int ares_dual_protocol_init(struct AresProtocol *protocol);

#define ARES_DUAL_PROTOCOL_DEFINE(Protocol_name)                                                   \
	struct AresProtocol Protocol_name = {                                                      \
		.name = #Protocol_name,                                                            \
		.api = &ares_dual_protocol_api,                                                    \
	};

#define TO_FLOAT(x)                                                                                \
	({                                                                                         \
		uint32_t temp = (x);                                                               \
		float result;                                                                      \
		memcpy(&result, &temp, sizeof(result));                                            \
		result;                                                                            \
	})

// Convert uint32_t back to double
#define TO_DOUBLE(x)                                                                               \
	({                                                                                         \
		uint32_t temp = (x);                                                               \
		double result;                                                                     \
		memcpy(&result, &temp, sizeof(result));                                            \
		result;                                                                            \
	})

#define SYNC_FRAME_HEAD          0x5A5A
#define SYNC_HEAD_IDX            0
#define SYNC_ID_IDX              2
#define SYNC_DATA_IDX            4
#define SYNC_FRAME_LENGTH_OFFSET 4

#define FUNC_FRAME_HEAD   0xFECA
#define FUNC_HEAD_IDX     0
#define FUNC_ID_IDX       2
#define FUNC_ARG1_IDX     4
#define FUNC_ARG2_IDX     8
#define FUNC_ARG3_IDX     12
#define FUNC_REQ_IDX      16
#define FUNC_FRAME_LENGTH 18

#define ERROR_FRAME_HEAD   0xDECA
#define ERROR_HEAD_IDX     0
#define ERROR_ID_IDX       2
#define ERROR_CODE_IDX     4
#define ERROR_FRAME_LENGTH 6

#define REPL_FRAME_HEAD   0xDEC0
#define REPL_HEAD_IDX     0
#define REPL_FUNC_ID_IDX  2
#define REPL_RET_IDX      4
#define REPL_REQ_ID_IDX   8
#define REPL_FRAME_LENGTH 10

#define HEARTBEAT_ID 0xFF

#define UNKNOWN_HEAD sys_cpu_to_be16(BIT(1))
#define UNKNOWN_TAIL sys_cpu_to_be16(BIT(2))
#define UNKNOWN_FUNC sys_cpu_to_be16(BIT(3))
#define UNKNOWN_SYNC sys_cpu_to_be16(BIT(4))
#define TOOHIGH_FREQ sys_cpu_to_be16(BIT(5))
#define CLEANED_PACK sys_cpu_to_be16(BIT(6))
#define REQUEST_SYNC sys_cpu_to_be16(BIT(7))
#define HEART_BEAT   sys_cpu_to_be16(BIT(8))

#define SYNC_PACK_STATUS_READ  BIT(0)
#define SYNC_PACK_STATUS_WRITE BIT(1)
#define SYNC_PACK_STATUS_DONE  BIT(2)

#define GET_8BITS(buf, n_byte)  (*(uint8_t *)(buf + n_byte))
#define GET_16BITS(buf, n_byte) (*(uint16_t *)(buf + n_byte))
#define GET_32BITS(buf, n_byte) (*(uint32_t *)(buf + n_byte))

typedef struct id_mapping func_table_t;
typedef struct sync_pack sync_table_t;

typedef void (*dual_trans_cb_t)(int status);

typedef uint32_t (*dual_trans_func_t)(uint32_t arg1, uint32_t arg2, uint32_t arg3);

struct sync_pack {
	uint16_t ID;
	struct AresInterface *interface;
	uint8_t *data;
	uint8_t request_id;
	size_t len;
	dual_trans_cb_t cb;
	uint8_t *buf;

	struct k_mutex mutex;
};

struct id_mapping {
	uint16_t id;
	dual_trans_func_t cb;
	uint32_t arg1;
	uint32_t arg2;
	uint32_t arg3;
	uint16_t req_id;

	struct k_mutex mutex;
	__aligned(4) uint8_t buf[REPL_FRAME_LENGTH];
};

#define MAX_PACK_COUNT 8
#define MAX_FUNC_COUNT MAX_PACK_COUNT

struct dual_protocol_data {
	struct k_timer heart_beat_timer;
	struct k_mutex err_frame_mutex;

	uint8_t func_cnt;
	func_table_t func_table[MAX_FUNC_COUNT];
	uint8_t sync_cnt;
	sync_table_t sync_table[MAX_PACK_COUNT];

	struct k_msgq func_tx_bckup_msgq;

	bool online;
	uint32_t last_heart_beat;
	uint32_t last_receive;

	uint8_t func_tx_bckup_cnt;
	__aligned(4) uint8_t error_frame_buf[ERROR_FRAME_LENGTH];
};

void dual_func_add(struct AresProtocol *protocol, uint16_t header, dual_trans_func_t cb);

void dual_func_remove(struct AresProtocol *protocol, uint16_t header);

sync_table_t *dual_sync_add(struct AresProtocol *protocol, uint16_t ID, uint8_t *buf, size_t len,
			    dual_trans_cb_t cb);

int dual_sync_flush(struct AresProtocol *protocol, sync_table_t *pack);

void dual_call_func(struct AresProtocol *protocol, uint16_t ID, void *arg1, void *arg2, void *arg3,
		    dual_trans_cb_t cb);

// Convert uint32_t back to float (C compatible)
#define TO_FLOAT(x)                                                                                \
	({                                                                                         \
		uint32_t temp = (x);                                                               \
		float result;                                                                      \
		memcpy(&result, &temp, sizeof(result));                                            \
		result;                                                                            \
	})

// Convert uint32_t back to double
#define TO_DOUBLE(x)                                                                               \
	({                                                                                         \
		uint32_t temp = (x);                                                               \
		double result;                                                                     \
		memcpy(&result, &temp, sizeof(result));                                            \
		result;                                                                            \
	})

#define DUAL_PROPOSE_PROTOCOL_DEFINE(Protocol_name)                                                \
	struct AresProtocolAPI Protocol_name##_api = {                                             \
		.handle = ares_dual_protocol_handle,                                               \
		.event = ares_dual_protocol_event,                                                 \
		.init = ares_dual_protocol_init,                                                   \
	};                                                                                         \
	struct dual_protocol_data Protocol_name##_data = {                                         \
		.heart_beat_timer = {0},                                                           \
		.err_frame_mutex = {0},                                                            \
		.func_cnt = 0,                                                                     \
		.sync_cnt = 0,                                                                     \
		.func_tx_bckup_msgq = {0},                                                         \
		.online = false,                                                                   \
	};                                                                                         \
	struct AresProtocol Protocol_name = {                                                      \
		.name = #Protocol_name,                                                            \
		.api = &Protocol_name##_api,                                                       \
		.priv_data = &Protocol_name##_data,                                               \
	};

#endif