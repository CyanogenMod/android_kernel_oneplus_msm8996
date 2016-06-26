/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/ion.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/hdcp_qseecom.h>
#include <linux/kthread.h>

#include "qseecom_kernel.h"

#define TZAPP_NAME            "hdcp2p2"
#define HDCP1_APP_NAME        "hdcp1"
#define QSEECOM_SBUFF_SIZE    0x1000

#define MAX_TX_MESSAGE_SIZE	129
#define MAX_RX_MESSAGE_SIZE	534
#define MAX_TOPOLOGY_ELEMS	32
#define HDCP1_AKSV_SIZE         8

/* parameters related to LC_Init message */
#define MESSAGE_ID_SIZE            1
#define LC_INIT_MESSAGE_SIZE       (MESSAGE_ID_SIZE+BITS_64_IN_BYTES)

/* parameters related to SKE_Send_EKS message */
#define SKE_SEND_EKS_MESSAGE_SIZE \
	(MESSAGE_ID_SIZE+BITS_128_IN_BYTES+BITS_64_IN_BYTES)

/* all message IDs */
#define AKE_SEND_CERT_MESSAGE_ID         3
#define AKE_NO_STORED_KM_MESSAGE_ID      4
#define AKE_STORED_KM_MESSAGE_ID         5
#define AKE_SEND_H_PRIME_MESSAGE_ID      7
#define AKE_SEND_PAIRING_INFO_MESSAGE_ID 8
#define LC_INIT_MESSAGE_ID               9
#define LC_SEND_L_PRIME_MESSAGE_ID      10
#define SKE_SEND_EKS_MESSAGE_ID         11
#define REPEATER_AUTH_SEND_RECEIVERID_LIST_MESSAGE_ID 12
#define REPEATER_AUTH_SEND_ACK_MESSAGE_ID      15
#define REPEATER_AUTH_STREAM_MANAGE_MESSAGE_ID 16
#define REPEATER_AUTH_STREAM_READY_MESSAGE_ID  17
#define HDCP1_SET_KEY_MESSAGE_ID       202
#define HDCP1_SET_ENC_MESSAGE_ID       205

#define BITS_8_IN_BYTES       1
#define BITS_16_IN_BYTES      2
#define BITS_24_IN_BYTES      3
#define BITS_32_IN_BYTES      4
#define BITS_40_IN_BYTES      5
#define BITS_64_IN_BYTES      8
#define BITS_128_IN_BYTES    16
#define BITS_160_IN_BYTES    20
#define BITS_256_IN_BYTES    32
#define BITS_1024_IN_BYTES  128
#define BITS_3072_IN_BYTES  384
#define TXCAPS_SIZE           3
#define RXCAPS_SIZE           3
#define RXINFO_SIZE           2
#define SEQ_NUM_V_SIZE        3

#define RCVR_ID_SIZE BITS_40_IN_BYTES
#define MAX_RCVR_IDS_ALLOWED_IN_LIST 31
#define MAX_RCVR_ID_LIST_SIZE \
		(RCVR_ID_SIZE*MAX_RCVR_IDS_ALLOWED_IN_LIST)
/*
 * minimum wait as per standard is 200 ms. keep it 300 ms
 * to be on safe side.
 */
#define SLEEP_SET_HW_KEY_MS 300


#define QSEECOM_ALIGN_SIZE    0x40
#define QSEECOM_ALIGN_MASK    (QSEECOM_ALIGN_SIZE - 1)
#define QSEECOM_ALIGN(x)\
	((x + QSEECOM_ALIGN_SIZE) & (~QSEECOM_ALIGN_MASK))

/* hdcp command status */
#define HDCP_SUCCESS      0

/* flags set by tz in response message */
#define HDCP_TXMTR_SUBSTATE_INIT                              0
#define HDCP_TXMTR_SUBSTATE_WAITING_FOR_RECIEVERID_LIST       1
#define HDCP_TXMTR_SUBSTATE_PROCESSED_RECIEVERID_LIST         2
#define HDCP_TXMTR_SUBSTATE_WAITING_FOR_STREAM_READY_MESSAGE  3
#define HDCP_TXMTR_SUBSTATE_REPEATER_AUTH_COMPLETE            4


#define HDCP_TXMTR_SERVICE_ID                 0x0001000
#define SERVICE_TXMTR_CREATE_CMD(x)          (HDCP_TXMTR_SERVICE_ID | x)

#define HDCP_TXMTR_INIT                           SERVICE_TXMTR_CREATE_CMD(1)
#define HDCP_TXMTR_DEINIT                         SERVICE_TXMTR_CREATE_CMD(2)
#define HDCP_TXMTR_PROCESS_RECEIVED_MESSAGE       SERVICE_TXMTR_CREATE_CMD(3)
#define HDCP_TXMTR_SEND_MESSAGE_TIMEOUT           SERVICE_TXMTR_CREATE_CMD(4)
#define HDCP_TXMTR_SET_HW_KEY                     SERVICE_TXMTR_CREATE_CMD(5)
#define HDCP_TXMTR_QUERY_STREAM_TYPE              SERVICE_TXMTR_CREATE_CMD(6)
#define HDCP_TXMTR_GET_KSXORLC128_AND_RIV         SERVICE_TXMTR_CREATE_CMD(7)
#define HDCP_TXMTR_PROVISION_KEY                  SERVICE_TXMTR_CREATE_CMD(8)
#define HDCP_TXMTR_GET_TOPOLOGY_INFO              SERVICE_TXMTR_CREATE_CMD(9)
#define HDCP_TXMTR_UPDATE_SRM                     SERVICE_TXMTR_CREATE_CMD(10)
/*This API calls the library init function */
#define HDCP_LIB_INIT                             SERVICE_TXMTR_CREATE_CMD(11)
/*This API calls the library deinit function */
#define HDCP_LIB_DEINIT                           SERVICE_TXMTR_CREATE_CMD(12)

#define HDCP_LIB_EXECUTE(x) {\
	if (handle->tethered)\
		hdcp_lib_##x(handle);\
	else\
		queue_kthread_work(&handle->worker, &handle->wk_##x);\
}

enum hdcp_state {
	HDCP_STATE_INIT = 0x00,
	HDCP_STATE_APP_LOADED = 0x01,
	HDCP_STATE_TXMTR_INIT = 0x02,
	HDCP_STATE_AUTHENTICATED = 0x04,
	HDCP_STATE_ERROR = 0x08
};

enum hdcp_element {
	HDCP_TYPE_UNKNOWN,
	HDCP_TYPE_RECEIVER,
	HDCP_TYPE_REPEATER,
};

enum hdcp_version {
	HDCP_VERSION_UNKNOWN,
	HDCP_VERSION_2_2,
	HDCP_VERSION_1_4
};

struct receiver_info {
	unsigned char rcvrInfo[RCVR_ID_SIZE];
	enum hdcp_element elem_type;
	enum hdcp_version hdcp_version;
};

struct topology_info {
	unsigned int nNumRcvrs;
	struct receiver_info rcvinfo[MAX_TOPOLOGY_ELEMS];
};

struct __attribute__ ((__packed__)) hdcp1_key_set_req {
	uint32_t commandid;
};

struct __attribute__ ((__packed__)) hdcp1_key_set_rsp {
	uint32_t commandid;
	uint32_t ret;
	uint8_t ksv[HDCP1_AKSV_SIZE];
};

struct __attribute__ ((__packed__)) hdcp_init_req {
	uint32_t commandid;
};

struct __attribute__ ((__packed__)) hdcp_init_rsp {
	uint32_t status;
	uint32_t commandid;
	uint32_t ctxhandle;
	uint32_t timeout;
	uint32_t msglen;
	uint8_t  message[MAX_TX_MESSAGE_SIZE];
};


struct __attribute__ ((__packed__)) hdcp_deinit_req {
	uint32_t commandid;
	uint32_t ctxhandle;
};


struct __attribute__ ((__packed__)) hdcp_deinit_rsp {
	uint32_t status;
	uint32_t commandid;
};


struct __attribute__ ((__packed__)) hdcp_rcvd_msg_req {
	uint32_t commandid;
	uint32_t ctxhandle;
	uint32_t msglen;
	uint8_t  msg[MAX_RX_MESSAGE_SIZE];
};


struct __attribute__ ((__packed__)) hdcp_rcvd_msg_rsp {
	uint32_t status;
	uint32_t commandid;
	uint32_t state;
	uint32_t timeout;
	uint32_t flag;
	uint32_t msglen;
	uint8_t  msg[MAX_TX_MESSAGE_SIZE];
};


struct __attribute__ ((__packed__)) hdcp_set_hw_key_req {
	uint32_t commandid;
	uint32_t ctxhandle;
};


struct __attribute__ ((__packed__)) hdcp_set_hw_key_rsp {
	uint32_t status;
	uint32_t commandid;
};


struct __attribute__ ((__packed__)) hdcp_send_timeout_req {
	uint32_t commandid;
	uint32_t ctxhandle;
};


struct __attribute__ ((__packed__)) hdcp_send_timeout_rsp {
	uint32_t status;
	uint32_t commandid;
	uint32_t timeout;
	uint32_t msglen;
	uint8_t  message[MAX_TX_MESSAGE_SIZE];
};

struct __attribute__ ((__packed__)) hdcp_query_stream_type_req {
	uint32_t commandid;
	uint32_t ctxhandle;
};

struct __attribute__ ((__packed__)) hdcp_query_stream_type_rsp {
	uint32_t status;
	uint32_t commandid;
	uint32_t timeout;
	uint32_t msglen;
	uint8_t  msg[MAX_TX_MESSAGE_SIZE];
};

struct __attribute__ ((__packed__)) hdcp_set_stream_type_req {
	uint32_t commandid;
	uint32_t ctxhandle;
	uint8_t  streamtype;
};

struct __attribute__ ((__packed__)) hdcp_set_stream_type_rsp {
	uint32_t status;
	uint32_t commandid;
	uint32_t timeout;
	uint32_t msglen;
	uint8_t  message[MAX_TX_MESSAGE_SIZE];
};


struct __attribute__ ((__packed__)) hdcp_update_srm_req {
	uint32_t commandid;
	uint32_t ctxhandle;
	uint32_t srmoffset;
	uint32_t srmlength;
};


struct __attribute__ ((__packed__)) hdcp_update_srm_rsp {
	uint32_t status;
	uint32_t commandid;
};


struct __attribute__ ((__packed__)) hdcp_get_topology_req {
	uint32_t commandid;
	uint32_t ctxhandle;
};


struct __attribute__ ((__packed__)) hdcp_get_topology_rsp {
	uint32_t status;
	uint32_t commandid;
	struct topology_info topologyinfo;
};


struct __attribute__ ((__packed__)) rxvr_info_struct {
	uint8_t rcvrCert[522];
	uint8_t rrx[BITS_64_IN_BYTES];
	uint8_t rxcaps[RXCAPS_SIZE];
	bool repeater;
};


struct __attribute__ ((__packed__)) repeater_info_struct {
	uint8_t RxInfo[RXINFO_SIZE];
	uint8_t seq_num_V[SEQ_NUM_V_SIZE];
	bool seq_num_V_Rollover_flag;
	uint8_t ReceiverIDList[MAX_RCVR_ID_LIST_SIZE];
	uint32_t ReceiverIDListLen;
};

struct __attribute__ ((__packed__)) hdcp1_set_enc_req {
	uint32_t commandid;
	uint32_t enable;
};

struct __attribute__ ((__packed__)) hdcp1_set_enc_rsp {
	uint32_t commandid;
	uint32_t ret;
};

/*
 * struct hdcp_lib_handle - handle for hdcp client
 * @qseecom_handle - for sending commands to qseecom
 * @listener_buf - buffer containing message shared with the client
 * @msglen - size message in the buffer
 * @tz_ctxhandle - context handle shared with tz
 * @hdcp_timeout - timeout in msecs shared for hdcp messages
 * @client_ctx - client context maintained by hdmi
 * @client_ops - handle to call APIs exposed by hdcp client
 * @timeout_lock - this lock protects hdcp_timeout field
 * @msg_lock - this lock protects the message buffer
 */
struct hdcp_lib_handle {
	unsigned char *listener_buf;
	uint32_t msglen;
	uint32_t tz_ctxhandle;
	uint32_t hdcp_timeout;
	uint32_t timeout_left;
	bool no_stored_km_flag;
	bool feature_supported;
	void *client_ctx;
	struct hdcp_client_ops *client_ops;
	struct mutex msg_lock;
	struct mutex wakeup_mutex;
	enum hdcp_state hdcp_state;
	enum hdcp_lib_wakeup_cmd wakeup_cmd;
	bool repeater_flag;
	bool update_stream;
	bool tethered;
	struct qseecom_handle *qseecom_handle;
	int last_msg_sent;
	char *last_msg_recvd_buf;
	uint32_t last_msg_recvd_len;
	atomic_t hdcp_off;

	struct task_struct *thread;
	struct completion topo_wait;

	struct kthread_worker worker;
	struct kthread_work wk_init;
	struct kthread_work wk_msg_sent;
	struct kthread_work wk_msg_recvd;
	struct kthread_work wk_timeout;
	struct kthread_work wk_clean;
	struct kthread_work wk_topology;
	struct kthread_work wk_stream;
};

struct hdcp_lib_message_map {
	int msg_id;
	const char *msg_name;
};

static void hdcp_lib_clean(struct hdcp_lib_handle *handle);
static void hdcp_lib_init(struct hdcp_lib_handle *handle);
static void hdcp_lib_msg_sent(struct hdcp_lib_handle *handle);
static void hdcp_lib_msg_recvd(struct hdcp_lib_handle *handle);
static void hdcp_lib_timeout(struct hdcp_lib_handle *handle);
static void hdcp_lib_stream(struct hdcp_lib_handle *handle);

static struct qseecom_handle *hdcp1_handle;
static bool hdcp1_supported = true;

static const char *hdcp_lib_message_name(int msg_id)
{
	/*
	 * Message ID map. The first number indicates the message number
	 * assigned to the message by the HDCP 2.2 spec. This is also the first
	 * byte of every HDCP 2.2 authentication protocol message.
	 */
	static struct hdcp_lib_message_map hdcp_lib_msg_map[] = {
		{2, "AKE_INIT"},
		{3, "AKE_SEND_CERT"},
		{4, "AKE_NO_STORED_KM"},
		{5, "AKE_STORED_KM"},
		{7, "AKE_SEND_H_PRIME"},
		{8, "AKE_SEND_PAIRING_INFO"},
		{9, "LC_INIT"},
		{10, "LC_SEND_L_PRIME"},
		{11, "SKE_SEND_EKS"},
		{12, "REPEATER_AUTH_SEND_RECEIVERID_LIST"},
		{15, "REPEATER_AUTH_SEND_ACK"},
		{16, "REPEATER_AUTH_STREAM_MANAGE"},
		{17, "REPEATER_AUTH_STREAM_READY"},
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(hdcp_lib_msg_map); i++) {
		if (msg_id == hdcp_lib_msg_map[i].msg_id)
			return hdcp_lib_msg_map[i].msg_name;
	}
	return "UNKNOWN";
}

static inline void hdcp_lib_wakeup_client(struct hdcp_lib_handle *handle,
	struct hdmi_hdcp_wakeup_data *data)
{
	int rc = 0;

	if (handle && handle->client_ops && handle->client_ops->wakeup &&
		data && (data->cmd != HDMI_HDCP_WKUP_CMD_INVALID)) {
		rc = handle->client_ops->wakeup(data);
		if (rc)
			pr_err("error sending %s to client\n",
				hdmi_hdcp_cmd_to_str(data->cmd));
	}
}

static inline void hdcp_lib_send_message(struct hdcp_lib_handle *handle)
{
	struct hdmi_hdcp_wakeup_data cdata = {HDMI_HDCP_WKUP_CMD_SEND_MESSAGE};

	cdata.context = handle->client_ctx;
	cdata.send_msg_buf = handle->listener_buf;
	cdata.send_msg_len = handle->msglen;
	cdata.timeout = handle->hdcp_timeout;

	hdcp_lib_wakeup_client(handle, &cdata);
}

static int hdcp_lib_enable_encryption(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdcp_set_hw_key_req *req_buf;
	struct hdcp_set_hw_key_rsp *rsp_buf;

	if (!handle || !handle->qseecom_handle ||
		!handle->qseecom_handle->sbuf) {
		pr_err("invalid handle\n");
		rc = -EINVAL;
		goto error;
	}

	/*
	 * wait at least 200ms before enabling encryption
	 * as per hdcp2p2 sepcifications.
	 */
	msleep(SLEEP_SET_HW_KEY_MS);

	req_buf = (struct hdcp_set_hw_key_req *)(
		handle->qseecom_handle->sbuf);
	req_buf->commandid = HDCP_TXMTR_SET_HW_KEY;
	req_buf->ctxhandle = handle->tz_ctxhandle;

	rsp_buf = (struct hdcp_set_hw_key_rsp *)(
		handle->qseecom_handle->sbuf + QSEECOM_ALIGN(
		sizeof(struct hdcp_set_hw_key_req)));

	rc = qseecom_send_command(handle->qseecom_handle, req_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_set_hw_key_req)),
			rsp_buf, QSEECOM_ALIGN(sizeof(
			struct hdcp_set_hw_key_rsp)));

	if ((rc < 0) || (rsp_buf->status < 0)) {
		pr_err("qseecom cmd failed with err = %d status = %d\n",
			rc, rsp_buf->status);
		rc = -EINVAL;
		goto error;
	}

	/* reached an authenticated state */
	handle->hdcp_state |= HDCP_STATE_AUTHENTICATED;

	pr_debug("success\n");
	return 0;
error:
	if (handle && !atomic_read(&handle->hdcp_off))
		HDCP_LIB_EXECUTE(clean);

	return rc;
}

static int hdcp_lib_library_load(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdcp_init_req *req_buf;
	struct hdcp_init_rsp *rsp_buf;

	if (!handle) {
		pr_err("invalid input\n");
		goto exit;
	}

	if (handle->hdcp_state & HDCP_STATE_APP_LOADED) {
		pr_err("library already loaded\n");
		return rc;
	}

	/*
	 * allocating resource for qseecom handle
	 * the app is not loaded here
	 */
	rc = qseecom_start_app(&(handle->qseecom_handle),
			TZAPP_NAME, QSEECOM_SBUFF_SIZE);
	if (rc) {
		pr_err("qseecom_start_app failed %d\n", rc);
		goto exit;
	}

	pr_debug("qseecom_start_app success\n");

	/* now load the app by sending hdcp_lib_init */
	req_buf = (struct hdcp_init_req *)handle->qseecom_handle->sbuf;
	req_buf->commandid = HDCP_LIB_INIT;
	rsp_buf = (struct hdcp_init_rsp *)(handle->qseecom_handle->
		sbuf + QSEECOM_ALIGN(sizeof(struct hdcp_init_req)));

	rc = qseecom_send_command(handle->qseecom_handle,
		req_buf, QSEECOM_ALIGN(sizeof(struct hdcp_init_req)),
		rsp_buf, QSEECOM_ALIGN(sizeof(struct hdcp_init_rsp)));

	if (rc < 0) {
		pr_err("qseecom cmd failed err = %d\n", rc);
		goto exit;
	}

	pr_debug("success\n");

	handle->hdcp_state |= HDCP_STATE_APP_LOADED;
exit:
	return rc;
}

static int hdcp_lib_library_unload(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdcp_deinit_req *req_buf;
	struct hdcp_deinit_rsp *rsp_buf;

	if (!handle || !handle->qseecom_handle ||
		!handle->qseecom_handle->sbuf) {
		pr_err("invalid handle\n");
		rc = -EINVAL;
		goto exit;
	}

	if (!(handle->hdcp_state & HDCP_STATE_APP_LOADED)) {
		pr_err("library not loaded\n");
		return rc;
	}

	/* unloading app by sending hdcp_lib_deinit cmd */
	req_buf = (struct hdcp_deinit_req *)handle->
			qseecom_handle->sbuf;
	req_buf->commandid = HDCP_LIB_DEINIT;
	rsp_buf = (struct hdcp_deinit_rsp *)(handle->qseecom_handle->
		sbuf + QSEECOM_ALIGN(sizeof(struct hdcp_deinit_req)));

	rc = qseecom_send_command(handle->qseecom_handle,
		req_buf, QSEECOM_ALIGN(sizeof(struct hdcp_deinit_req)),
		rsp_buf, QSEECOM_ALIGN(sizeof(struct hdcp_deinit_rsp)));

	if (rc < 0) {
		pr_err("qseecom cmd failed err = %d\n", rc);
		goto exit;
	}

	/* deallocate the resources for qseecom handle */
	rc = qseecom_shutdown_app(&handle->qseecom_handle);
	if (rc) {
		pr_err("qseecom_shutdown_app failed err: %d\n", rc);
		goto exit;
	}

	handle->hdcp_state &= ~HDCP_STATE_APP_LOADED;
	pr_debug("success\n");
exit:
	return rc;
}

static int hdcp_lib_txmtr_init(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdcp_init_req *req_buf;
	struct hdcp_init_rsp *rsp_buf;

	if (!handle || !handle->qseecom_handle ||
		!handle->qseecom_handle->sbuf) {
		pr_err("invalid handle\n");
		rc = -EINVAL;
		goto exit;
	}

	if (!(handle->hdcp_state & HDCP_STATE_APP_LOADED)) {
		pr_err("app not loaded\n");
		goto exit;
	}

	if (handle->hdcp_state & HDCP_STATE_TXMTR_INIT) {
		pr_err("txmtr already initialized\n");
		goto exit;
	}

	/* send HDCP_Txmtr_Init command to TZ */
	req_buf = (struct hdcp_init_req *)handle->
			qseecom_handle->sbuf;
	req_buf->commandid = HDCP_TXMTR_INIT;
	rsp_buf = (struct hdcp_init_rsp *)(handle->qseecom_handle->
		sbuf + QSEECOM_ALIGN(sizeof(struct hdcp_init_req)));

	rc = qseecom_send_command(handle->qseecom_handle, req_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_init_req)), rsp_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_init_rsp)));

	if ((rc < 0) || (rsp_buf->status != HDCP_SUCCESS) ||
		(rsp_buf->commandid != HDCP_TXMTR_INIT) ||
		(rsp_buf->msglen <= 0) || (rsp_buf->message == NULL)) {
		pr_err("qseecom cmd failed with err = %d, status = %d\n",
			rc, rsp_buf->status);
		rc = -EINVAL;
		goto exit;
	}

	pr_debug("recvd %s from TZ at %dms\n",
		hdcp_lib_message_name((int)rsp_buf->message[0]),
		jiffies_to_msecs(jiffies));

	/* send the response to HDMI driver */
	memset(handle->listener_buf, 0, MAX_TX_MESSAGE_SIZE);
	memcpy(handle->listener_buf, (unsigned char *)rsp_buf->message,
			rsp_buf->msglen);
	handle->msglen = rsp_buf->msglen;
	handle->hdcp_timeout = rsp_buf->timeout;

	handle->tz_ctxhandle = rsp_buf->ctxhandle;
	handle->hdcp_state |= HDCP_STATE_TXMTR_INIT;

	pr_debug("success\n");
exit:
	return rc;
}

static int hdcp_lib_txmtr_deinit(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdcp_deinit_req *req_buf;
	struct hdcp_deinit_rsp *rsp_buf;

	if (!handle || !handle->qseecom_handle ||
		!handle->qseecom_handle->sbuf) {
		pr_err("invalid handle\n");
		rc = -EINVAL;
		goto exit;
	}

	if (!(handle->hdcp_state & HDCP_STATE_APP_LOADED)) {
		pr_err("app not loaded\n");
		goto exit;
	}

	if (!(handle->hdcp_state & HDCP_STATE_TXMTR_INIT)) {
		/* unload library here */
		pr_err("txmtr not initialized\n");
		goto exit;
	}

	/* send command to TZ */
	req_buf = (struct hdcp_deinit_req *)handle->
			qseecom_handle->sbuf;
	req_buf->commandid = HDCP_TXMTR_DEINIT;
	req_buf->ctxhandle = handle->tz_ctxhandle;
	rsp_buf = (struct hdcp_deinit_rsp *)(handle->
		qseecom_handle->sbuf + QSEECOM_ALIGN(sizeof(
			struct hdcp_deinit_req)));

	rc = qseecom_send_command(handle->qseecom_handle, req_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_deinit_req)), rsp_buf,
			QSEECOM_ALIGN(sizeof(struct hdcp_deinit_rsp)));

	if ((rc < 0) || (rsp_buf->status < 0) ||
			(rsp_buf->commandid != HDCP_TXMTR_DEINIT)) {
		pr_err("qseecom cmd failed with err = %d status = %d\n",
					rc, rsp_buf->status);
		rc = -EINVAL;
		goto exit;
	}

	handle->hdcp_state &= ~HDCP_STATE_TXMTR_INIT;
	pr_debug("success\n");
exit:
	return rc;
}

static void hdcp_lib_stream(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdcp_query_stream_type_req *req_buf;
	struct hdcp_query_stream_type_rsp *rsp_buf;

	if (!handle || !handle->qseecom_handle ||
		!handle->qseecom_handle->sbuf) {
		pr_err("invalid handle\n");
		return;
	}

	if (atomic_read(&handle->hdcp_off)) {
		pr_debug("invalid state, hdcp off\n");
		return;
	}

	if (!handle->repeater_flag) {
		pr_debug("invalid state, not a repeater\n");
		return;
	}

	/* send command to TZ */
	req_buf = (struct hdcp_query_stream_type_req *)handle->
			qseecom_handle->sbuf;
	req_buf->commandid = HDCP_TXMTR_QUERY_STREAM_TYPE;
	req_buf->ctxhandle = handle->tz_ctxhandle;
	rsp_buf = (struct hdcp_query_stream_type_rsp *)(handle->
		qseecom_handle->sbuf + QSEECOM_ALIGN(sizeof(
			struct hdcp_query_stream_type_req)));

	rc = qseecom_send_command(handle->qseecom_handle, req_buf,
	QSEECOM_ALIGN(sizeof(struct hdcp_query_stream_type_req)), rsp_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_query_stream_type_rsp)));

	if ((rc < 0) || (rsp_buf->status < 0) || (rsp_buf->msglen <= 0) ||
		(rsp_buf->commandid != HDCP_TXMTR_QUERY_STREAM_TYPE) ||
				(rsp_buf->msg == NULL)) {
		pr_err("qseecom cmd failed with err=%d status=%d\n",
			rc, rsp_buf->status);
		rc = -EINVAL;
		goto exit;
	}

	pr_debug("message received from TZ: %s\n",
		hdcp_lib_message_name((int)rsp_buf->msg[0]));

	memset(handle->listener_buf, 0, MAX_TX_MESSAGE_SIZE);
	memcpy(handle->listener_buf, (unsigned char *)rsp_buf->msg,
			rsp_buf->msglen);
	handle->hdcp_timeout = rsp_buf->timeout;
	handle->msglen = rsp_buf->msglen;
exit:
	if (!rc && !atomic_read(&handle->hdcp_off))
		hdcp_lib_send_message(handle);
}

static void hdcp_lib_query_stream_work(struct kthread_work *work)
{
	struct hdcp_lib_handle *handle = container_of(work,
		struct hdcp_lib_handle, wk_stream);

	hdcp_lib_stream(handle);
}

static bool hdcp_lib_client_feature_supported(void *phdcpcontext)
{
	int rc = 0;
	bool supported = false;
	struct hdcp_lib_handle *handle = phdcpcontext;

	if (!handle) {
		pr_err("invalid input\n");
		goto exit;
	}

	if (handle->feature_supported) {
		supported = true;
		goto exit;
	}

	rc = hdcp_lib_library_load(handle);
	if (!rc) {
		pr_debug("HDCP2p2 supported\n");
		handle->feature_supported = true;
		hdcp_lib_library_unload(handle);
		supported = true;
	}
exit:
	return supported;
}

static void hdcp_lib_check_worker_status(struct hdcp_lib_handle *handle)
{
	if (!list_empty(&handle->wk_init.node))
		pr_debug("init work queued\n");

	if (handle->worker.current_work == &handle->wk_init)
		pr_debug("init work executing\n");

	if (!list_empty(&handle->wk_msg_sent.node))
		pr_debug("msg_sent work queued\n");

	if (handle->worker.current_work == &handle->wk_msg_sent)
		pr_debug("msg_sent work executing\n");

	if (!list_empty(&handle->wk_msg_recvd.node))
		pr_debug("msg_recvd work queued\n");

	if (handle->worker.current_work == &handle->wk_msg_recvd)
		pr_debug("msg_recvd work executing\n");

	if (!list_empty(&handle->wk_timeout.node))
		pr_debug("timeout work queued\n");

	if (handle->worker.current_work == &handle->wk_timeout)
		pr_debug("timeout work executing\n");

	if (!list_empty(&handle->wk_clean.node))
		pr_debug("clean work queued\n");

	if (handle->worker.current_work == &handle->wk_clean)
		pr_debug("clean work executing\n");

	if (!list_empty(&handle->wk_topology.node))
		pr_debug("topology work queued\n");

	if (handle->worker.current_work == &handle->wk_topology)
		pr_debug("topology work executing\n");

	if (!list_empty(&handle->wk_stream.node))
		pr_debug("stream work queued\n");

	if (handle->worker.current_work == &handle->wk_stream)
		pr_debug("stream work executing\n");
}

static int hdcp_lib_check_valid_state(struct hdcp_lib_handle *handle)
{
	int rc = 0;

	if (!list_empty(&handle->worker.work_list))
		hdcp_lib_check_worker_status(handle);

	if (handle->wakeup_cmd == HDCP_LIB_WKUP_CMD_START) {
		if (!list_empty(&handle->worker.work_list)) {
			rc = -EBUSY;
			goto exit;
		}
	} else {
		if (atomic_read(&handle->hdcp_off)) {
			pr_debug("hdcp2.2 session tearing down\n");
			goto exit;
		}

		if (!(handle->hdcp_state & HDCP_STATE_APP_LOADED)) {
			pr_debug("hdcp 2.2 app not loaded\n");
			goto exit;
		}
	}
exit:
	return rc;
}

static void hdcp_lib_update_exec_type(void *ctx, bool tethered)
{
	struct hdcp_lib_handle *handle = ctx;

	if (!handle)
		return;

	mutex_lock(&handle->wakeup_mutex);

	if (handle->tethered == tethered) {
		pr_debug("exec mode same as %s\n",
			tethered ? "tethered" : "threaded");
	} else {
		handle->tethered = tethered;

		pr_debug("exec mode changed to %s\n",
			tethered ? "tethered" : "threaded");
	}

	mutex_unlock(&handle->wakeup_mutex);
}

static int hdcp_lib_wakeup(struct hdcp_lib_wakeup_data *data)
{
	struct hdcp_lib_handle *handle;
	int rc = 0;

	if (!data)
		return -EINVAL;

	handle = data->context;
	if (!handle)
		return -EINVAL;

	mutex_lock(&handle->wakeup_mutex);

	handle->wakeup_cmd = data->cmd;
	handle->timeout_left = data->timeout;

	pr_debug("%s, timeout left: %dms, tethered %d\n",
		hdcp_lib_cmd_to_str(handle->wakeup_cmd),
		handle->timeout_left, handle->tethered);

	rc = hdcp_lib_check_valid_state(handle);
	if (rc)
		goto exit;

	mutex_lock(&handle->msg_lock);
	if (data->recvd_msg_len) {
		kzfree(handle->last_msg_recvd_buf);

		handle->last_msg_recvd_len = data->recvd_msg_len;
		handle->last_msg_recvd_buf = kzalloc(data->recvd_msg_len,
			GFP_KERNEL);
		if (!handle->last_msg_recvd_buf) {
			rc = -ENOMEM;
			mutex_unlock(&handle->msg_lock);
			goto exit;
		}

		memcpy(handle->last_msg_recvd_buf, data->recvd_msg_buf,
			data->recvd_msg_len);
	}
	mutex_unlock(&handle->msg_lock);

	if (!completion_done(&handle->topo_wait))
		complete_all(&handle->topo_wait);

	switch (handle->wakeup_cmd) {
	case HDCP_LIB_WKUP_CMD_START:
		handle->no_stored_km_flag = 0;
		handle->repeater_flag = false;
		handle->update_stream = false;
		handle->last_msg_sent = 0;
		handle->hdcp_timeout = 0;
		handle->timeout_left = 0;
		atomic_set(&handle->hdcp_off, 0);
		handle->hdcp_state = HDCP_STATE_INIT;

		HDCP_LIB_EXECUTE(init);
		break;
	case HDCP_LIB_WKUP_CMD_STOP:
		atomic_set(&handle->hdcp_off, 1);

		HDCP_LIB_EXECUTE(clean);
		break;
	case HDCP_LIB_WKUP_CMD_MSG_SEND_SUCCESS:
		handle->last_msg_sent = handle->listener_buf[0];

		HDCP_LIB_EXECUTE(msg_sent);
		break;
	case HDCP_LIB_WKUP_CMD_MSG_SEND_FAILED:
	case HDCP_LIB_WKUP_CMD_MSG_RECV_FAILED:
		HDCP_LIB_EXECUTE(clean);
		break;
	case HDCP_LIB_WKUP_CMD_MSG_RECV_SUCCESS:
		HDCP_LIB_EXECUTE(msg_recvd);
		break;
	case HDCP_LIB_WKUP_CMD_MSG_RECV_TIMEOUT:
		HDCP_LIB_EXECUTE(timeout);
		break;
	case HDCP_LIB_WKUP_CMD_QUERY_STREAM_TYPE:
		HDCP_LIB_EXECUTE(stream);
		break;
	default:
		pr_err("invalid wakeup command %d\n", handle->wakeup_cmd);
	}
exit:
	mutex_unlock(&handle->wakeup_mutex);

	return rc;
}

static void hdcp_lib_msg_sent(struct hdcp_lib_handle *handle)
{
	struct hdmi_hdcp_wakeup_data cdata = {HDMI_HDCP_WKUP_CMD_INVALID};

	if (!handle) {
		pr_err("invalid handle\n");
		return;
	}

	cdata.context = handle->client_ctx;

	switch (handle->last_msg_sent) {
	case SKE_SEND_EKS_MESSAGE_ID:
		if (handle->repeater_flag) {
			if (!atomic_read(&handle->hdcp_off))
				queue_kthread_work(&handle->worker,
					&handle->wk_topology);
		}

		if (!hdcp_lib_enable_encryption(handle)) {
			cdata.cmd = HDMI_HDCP_WKUP_CMD_STATUS_SUCCESS;
			hdcp_lib_wakeup_client(handle, &cdata);

			/* poll for link check */
			cdata.cmd = HDMI_HDCP_WKUP_CMD_LINK_POLL;
		} else {
			if (!atomic_read(&handle->hdcp_off))
				HDCP_LIB_EXECUTE(clean);
		}
		break;
	case REPEATER_AUTH_SEND_ACK_MESSAGE_ID:
		pr_debug("Repeater authentication successful\n");

		if (handle->update_stream) {
			HDCP_LIB_EXECUTE(stream);
			handle->update_stream = false;
		} else {
			cdata.cmd = HDMI_HDCP_WKUP_CMD_LINK_POLL;
		}
		break;
	default:
		cdata.cmd = HDMI_HDCP_WKUP_CMD_RECV_MESSAGE;
		cdata.timeout = handle->timeout_left;
	}

	hdcp_lib_wakeup_client(handle, &cdata);
}

static void hdcp_lib_msg_sent_work(struct kthread_work *work)
{
	struct hdcp_lib_handle *handle = container_of(work,
		struct hdcp_lib_handle, wk_msg_sent);

	if (handle->wakeup_cmd != HDCP_LIB_WKUP_CMD_MSG_SEND_SUCCESS) {
		pr_err("invalid wakeup command %d\n", handle->wakeup_cmd);
		return;
	}

	hdcp_lib_msg_sent(handle);
}

static void hdcp_lib_init(struct hdcp_lib_handle *handle)
{
	int rc = 0;

	if (!handle) {
		pr_err("invalid handle\n");
		return;
	}

	if (handle->wakeup_cmd != HDCP_LIB_WKUP_CMD_START) {
		pr_err("invalid wakeup command %d\n", handle->wakeup_cmd);
		return;
	}

	rc = hdcp_lib_library_load(handle);
	if (rc)
		goto exit;

	rc = hdcp_lib_txmtr_init(handle);
	if (rc)
		goto exit;

	hdcp_lib_send_message(handle);

	return;
exit:
	HDCP_LIB_EXECUTE(clean);
}

static void hdcp_lib_init_work(struct kthread_work *work)
{
	struct hdcp_lib_handle *handle = container_of(work,
		struct hdcp_lib_handle, wk_init);

	hdcp_lib_init(handle);
}

static void hdcp_lib_timeout(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdcp_send_timeout_req *req_buf;
	struct hdcp_send_timeout_rsp *rsp_buf;

	if (!handle || !handle->qseecom_handle ||
		!handle->qseecom_handle->sbuf) {
		pr_debug("invalid handle\n");
		return;
	}

	if (atomic_read(&handle->hdcp_off)) {
		pr_debug("invalid state, hdcp off\n");
		return;
	}

	req_buf = (struct hdcp_send_timeout_req *)
		(handle->qseecom_handle->sbuf);
	req_buf->commandid = HDCP_TXMTR_SEND_MESSAGE_TIMEOUT;
	req_buf->ctxhandle = handle->tz_ctxhandle;

	rsp_buf = (struct hdcp_send_timeout_rsp *)(handle->
		qseecom_handle->sbuf + QSEECOM_ALIGN(sizeof(
			struct hdcp_send_timeout_req)));

	rc = qseecom_send_command(handle->qseecom_handle, req_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_send_timeout_req)), rsp_buf,
			QSEECOM_ALIGN(sizeof(struct hdcp_send_timeout_rsp)));

	if ((rc < 0) || (rsp_buf->status != HDCP_SUCCESS)) {
		pr_err("qseecom cmd failed for with err = %d status = %d\n",
			rc, rsp_buf->status);
		rc = -EINVAL;
		goto error;
	}

	if (rsp_buf->commandid == HDCP_TXMTR_SEND_MESSAGE_TIMEOUT) {
		pr_err("HDCP_TXMTR_SEND_MESSAGE_TIMEOUT\n");
		rc = -EINVAL;
		goto error;
	}

	/*
	 * if the response contains LC_Init message
	 * send the message again to TZ
	 */
	if ((rsp_buf->commandid == HDCP_TXMTR_PROCESS_RECEIVED_MESSAGE) &&
		((int)rsp_buf->message[0] == LC_INIT_MESSAGE_ID) &&
			(rsp_buf->msglen == LC_INIT_MESSAGE_SIZE)) {
		if (!atomic_read(&handle->hdcp_off)) {
			/* keep local copy of TZ response */
			memset(handle->listener_buf, 0, MAX_TX_MESSAGE_SIZE);
			memcpy(handle->listener_buf,
				(unsigned char *)rsp_buf->message,
				rsp_buf->msglen);
			handle->hdcp_timeout = rsp_buf->timeout;
			handle->msglen = rsp_buf->msglen;

			hdcp_lib_send_message(handle);
		}
	}

	return;
error:
	if (!atomic_read(&handle->hdcp_off))
		HDCP_LIB_EXECUTE(clean);
}

static void hdcp_lib_manage_timeout_work(struct kthread_work *work)
{
	struct hdcp_lib_handle *handle = container_of(work,
		struct hdcp_lib_handle, wk_timeout);

	hdcp_lib_timeout(handle);
}

static void hdcp_lib_clean(struct hdcp_lib_handle *handle)
{
	struct hdmi_hdcp_wakeup_data cdata = {HDMI_HDCP_WKUP_CMD_INVALID};

	if (!handle) {
		pr_err("invalid input\n");
		return;
	};

	hdcp_lib_txmtr_deinit(handle);
	hdcp_lib_library_unload(handle);

	cdata.context = handle->client_ctx;
	cdata.cmd = HDMI_HDCP_WKUP_CMD_STATUS_FAILED;

	if (!atomic_read(&handle->hdcp_off))
		hdcp_lib_wakeup_client(handle, &cdata);

	atomic_set(&handle->hdcp_off, 1);
}


static void hdcp_lib_cleanup_work(struct kthread_work *work)
{
	struct hdcp_lib_handle *handle = container_of(work,
		struct hdcp_lib_handle, wk_clean);

	hdcp_lib_clean(handle);
}

static void hdcp_lib_msg_recvd(struct hdcp_lib_handle *handle)
{
	int rc = 0;
	struct hdmi_hdcp_wakeup_data cdata = {HDMI_HDCP_WKUP_CMD_INVALID};
	struct hdcp_rcvd_msg_req *req_buf;
	struct hdcp_rcvd_msg_rsp *rsp_buf;
	uint32_t msglen;
	char *msg = NULL;

	if (!handle || !handle->qseecom_handle ||
		!handle->qseecom_handle->sbuf) {
		pr_err("invalid handle\n");
		return;
	}

	if (atomic_read(&handle->hdcp_off)) {
		pr_debug("invalid state, hdcp off\n");
		return;
	}

	cdata.context = handle->client_ctx;

	mutex_lock(&handle->msg_lock);
	msglen = handle->last_msg_recvd_len;

	if (msglen <= 0) {
		pr_err("invalid msg len\n");
		mutex_unlock(&handle->msg_lock);
		rc = -EINVAL;
		goto exit;
	}

	msg = kzalloc(msglen, GFP_KERNEL);
	if (!msg) {
		mutex_unlock(&handle->msg_lock);
		rc = -ENOMEM;
		goto exit;
	}

	memcpy(msg, handle->last_msg_recvd_buf, msglen);

	mutex_unlock(&handle->msg_lock);

	pr_debug("msg received: %s from sink\n",
		hdcp_lib_message_name((int)msg[0]));

	/* send the message to QSEECOM */
	req_buf = (struct hdcp_rcvd_msg_req *)(handle->
			qseecom_handle->sbuf);
	req_buf->commandid = HDCP_TXMTR_PROCESS_RECEIVED_MESSAGE;
	memcpy(req_buf->msg, msg, msglen);
	req_buf->msglen = msglen;
	req_buf->ctxhandle = handle->tz_ctxhandle;

	rsp_buf = (struct hdcp_rcvd_msg_rsp *)(handle->
		qseecom_handle->sbuf + QSEECOM_ALIGN(sizeof(
			struct hdcp_rcvd_msg_req)));

	pr_debug("writing %s to TZ at %dms\n",
		hdcp_lib_message_name((int)msg[0]),
		jiffies_to_msecs(jiffies));

	rc = qseecom_send_command(handle->qseecom_handle, req_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_rcvd_msg_req)), rsp_buf,
		QSEECOM_ALIGN(sizeof(struct hdcp_rcvd_msg_rsp)));

	/* get next message from sink if we receive H PRIME on no store km */
	if ((msg[0] == AKE_SEND_H_PRIME_MESSAGE_ID) &&
			handle->no_stored_km_flag) {
		handle->hdcp_timeout = rsp_buf->timeout;

		cdata.cmd = HDMI_HDCP_WKUP_CMD_RECV_MESSAGE;
		cdata.timeout = handle->hdcp_timeout;

		goto exit;
	}

	if ((msg[0] == REPEATER_AUTH_STREAM_READY_MESSAGE_ID) &&
			(rc == 0) && (rsp_buf->status == 0)) {
		pr_debug("Got Auth_Stream_Ready, nothing sent to rx\n");

		cdata.cmd = HDMI_HDCP_WKUP_CMD_LINK_POLL;
		goto exit;
	}

	if ((rc < 0) || (rsp_buf->status < 0) || (rsp_buf->msglen <= 0) ||
		(rsp_buf->commandid != HDCP_TXMTR_PROCESS_RECEIVED_MESSAGE) ||
				(rsp_buf->msg == NULL)) {
		pr_err("qseecom cmd failed with err=%d status=%d\n",
			rc, rsp_buf->status);
		rc = -EINVAL;
		goto exit;
	}

	pr_debug("recvd %s from TZ at %dms\n",
		hdcp_lib_message_name((int)rsp_buf->msg[0]),
		jiffies_to_msecs(jiffies));

	/* set the flag if response is AKE_No_Stored_km */
	if (((int)rsp_buf->msg[0] == AKE_NO_STORED_KM_MESSAGE_ID)) {
		pr_debug("Setting no_stored_km_flag\n");
		handle->no_stored_km_flag = 1;
	} else {
		handle->no_stored_km_flag = 0;
	}

	/* check if it's a repeater */
	if ((rsp_buf->msg[0] == SKE_SEND_EKS_MESSAGE_ID) &&
			(rsp_buf->msglen == SKE_SEND_EKS_MESSAGE_SIZE)) {
		if ((rsp_buf->flag ==
			HDCP_TXMTR_SUBSTATE_WAITING_FOR_RECIEVERID_LIST) &&
						(rsp_buf->timeout > 0))
			handle->repeater_flag = true;
			handle->update_stream = true;
	}

	memset(handle->listener_buf, 0, MAX_TX_MESSAGE_SIZE);
	memcpy(handle->listener_buf, (unsigned char *)rsp_buf->msg,
			rsp_buf->msglen);
	handle->hdcp_timeout = rsp_buf->timeout;
	handle->msglen = rsp_buf->msglen;

	if (!atomic_read(&handle->hdcp_off)) {
		cdata.cmd = HDMI_HDCP_WKUP_CMD_SEND_MESSAGE;
		cdata.send_msg_buf = handle->listener_buf;
		cdata.send_msg_len = handle->msglen;
		cdata.timeout = handle->hdcp_timeout;
	}

exit:
	kzfree(msg);

	hdcp_lib_wakeup_client(handle, &cdata);

	if (rc && !atomic_read(&handle->hdcp_off))
		HDCP_LIB_EXECUTE(clean);
}

static void hdcp_lib_msg_recvd_work(struct kthread_work *work)
{
	struct hdcp_lib_handle *handle = container_of(work,
		struct hdcp_lib_handle, wk_msg_recvd);

	hdcp_lib_msg_recvd(handle);
}

static void hdcp_lib_topology_work(struct kthread_work *work)
{
	u32 timeout;
	struct hdcp_lib_handle *handle = container_of(work,
		struct hdcp_lib_handle, wk_topology);

	if (!handle) {
		pr_err("invalid input\n");
		return;
	}

	reinit_completion(&handle->topo_wait);
	timeout = wait_for_completion_timeout(&handle->topo_wait, HZ * 3);
	if (!timeout) {
		pr_err("topology receiver id list timeout\n");

		if (!atomic_read(&handle->hdcp_off))
			HDCP_LIB_EXECUTE(clean);
	}
}

bool hdcp1_check_if_supported_load_app(void)
{
	int rc = 0;

	/* start hdcp1 app */
	if (hdcp1_supported && !hdcp1_handle) {
		rc = qseecom_start_app(&hdcp1_handle, HDCP1_APP_NAME,
			QSEECOM_SBUFF_SIZE);
		if (rc) {
			pr_err("qseecom_start_app failed %d\n", rc);
			hdcp1_supported = false;
		}
	}

	pr_debug("hdcp1 app %s loaded\n",
		hdcp1_supported ? "successfully" : "not");

	return hdcp1_supported;
}

/* APIs exposed to all clients */
int hdcp1_set_keys(uint32_t *aksv_msb, uint32_t *aksv_lsb)
{
	int rc = 0;
	struct hdcp1_key_set_req *key_set_req;
	struct hdcp1_key_set_rsp *key_set_rsp;

	if (aksv_msb == NULL || aksv_lsb == NULL)
		return -EINVAL;

	if (!hdcp1_supported || !hdcp1_handle)
		return -EINVAL;

	/* set keys and request aksv */
	key_set_req = (struct hdcp1_key_set_req *)hdcp1_handle->sbuf;
	key_set_req->commandid = HDCP1_SET_KEY_MESSAGE_ID;
	key_set_rsp = (struct hdcp1_key_set_rsp *)(hdcp1_handle->sbuf +
			QSEECOM_ALIGN(sizeof(struct hdcp1_key_set_req)));
	rc = qseecom_send_command(hdcp1_handle,
		key_set_req, QSEECOM_ALIGN(sizeof(struct hdcp1_key_set_req)),
		key_set_rsp, QSEECOM_ALIGN(sizeof(struct hdcp1_key_set_rsp)));

	if (rc < 0) {
		pr_err("qseecom cmd failed err=%d\n", rc);
		return -ENOKEY;
	}

	rc = key_set_rsp->ret;
	if (rc) {
		pr_err("set key cmd failed, rsp=%d\n",
			key_set_rsp->ret);
		return -ENOKEY;
	}

	/* copy bytes into msb and lsb */
	*aksv_msb = key_set_rsp->ksv[0] << 24;
	*aksv_msb |= key_set_rsp->ksv[1] << 16;
	*aksv_msb |= key_set_rsp->ksv[2] << 8;
	*aksv_msb |= key_set_rsp->ksv[3];
	*aksv_lsb = key_set_rsp->ksv[4] << 24;
	*aksv_lsb |= key_set_rsp->ksv[5] << 16;
	*aksv_lsb |= key_set_rsp->ksv[6] << 8;
	*aksv_lsb |= key_set_rsp->ksv[7];

	return 0;
}

int hdcp1_set_enc(bool enable)
{
	int rc = 0;
	struct hdcp1_set_enc_req *set_enc_req;
	struct hdcp1_set_enc_rsp *set_enc_rsp;

	if (!hdcp1_supported || !hdcp1_handle)
		return -EINVAL;

	/* set keys and request aksv */
	set_enc_req = (struct hdcp1_set_enc_req *)hdcp1_handle->sbuf;
	set_enc_req->commandid = HDCP1_SET_ENC_MESSAGE_ID;
	set_enc_req->enable = enable;
	set_enc_rsp = (struct hdcp1_set_enc_rsp *)(hdcp1_handle->sbuf +
			QSEECOM_ALIGN(sizeof(struct hdcp1_set_enc_req)));
	rc = qseecom_send_command(hdcp1_handle,
		set_enc_req, QSEECOM_ALIGN(sizeof(struct hdcp1_set_enc_req)),
		set_enc_rsp, QSEECOM_ALIGN(sizeof(struct hdcp1_set_enc_rsp)));

	if (rc < 0) {
		pr_err("qseecom cmd failed err=%d\n", rc);
		return -EINVAL;
	}

	rc = set_enc_rsp->ret;
	if (rc) {
		pr_err("enc cmd failed, rsp=%d\n",
			set_enc_rsp->ret);
		return -EINVAL;
	}

	pr_debug("success\n");
	return 0;
}

int hdcp_library_register(struct hdcp_register_data *data)
{
	int rc = 0;
	struct hdcp_lib_handle *handle = NULL;

	if (!data) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	if (!data->txmtr_ops) {
		pr_err("invalid input: txmtr context\n");
		return -EINVAL;
	}

	if (!data->client_ops) {
		pr_err("invalid input: client_ops\n");
		return -EINVAL;
	}

	if (!data->hdcp_ctx) {
		pr_err("invalid input: hdcp_ctx\n");
		return -EINVAL;
	}

	/* populate ops to be called by client */
	data->txmtr_ops->feature_supported = hdcp_lib_client_feature_supported;
	data->txmtr_ops->wakeup = hdcp_lib_wakeup;
	data->txmtr_ops->update_exec_type = hdcp_lib_update_exec_type;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle) {
		rc = -ENOMEM;
		goto unlock;
	}

	handle->client_ctx = data->client_ctx;
	handle->client_ops = data->client_ops;
	handle->tethered = data->tethered;

	pr_debug("tethered %d\n", handle->tethered);

	atomic_set(&handle->hdcp_off, 0);

	mutex_init(&handle->msg_lock);
	mutex_init(&handle->wakeup_mutex);

	init_kthread_worker(&handle->worker);

	init_kthread_work(&handle->wk_init,      hdcp_lib_init_work);
	init_kthread_work(&handle->wk_msg_sent,  hdcp_lib_msg_sent_work);
	init_kthread_work(&handle->wk_msg_recvd, hdcp_lib_msg_recvd_work);
	init_kthread_work(&handle->wk_timeout,   hdcp_lib_manage_timeout_work);
	init_kthread_work(&handle->wk_clean,     hdcp_lib_cleanup_work);
	init_kthread_work(&handle->wk_topology,  hdcp_lib_topology_work);
	init_kthread_work(&handle->wk_stream,    hdcp_lib_query_stream_work);

	init_completion(&handle->topo_wait);

	handle->listener_buf = kzalloc(MAX_TX_MESSAGE_SIZE, GFP_KERNEL);
	if (!(handle->listener_buf)) {
		rc = -ENOMEM;
		goto error;
	}

	*data->hdcp_ctx = handle;

	handle->thread = kthread_run(kthread_worker_fn,
		&handle->worker, "hdcp_tz_lib");

	if (IS_ERR(handle->thread)) {
		pr_err("unable to start lib thread\n");
		rc = PTR_ERR(handle->thread);
		handle->thread = NULL;
		goto error;
	}

	return 0;
error:
	kzfree(handle->listener_buf);
	handle->listener_buf = NULL;
	kzfree(handle);
	handle = NULL;
unlock:
	return rc;
}

void hdcp_library_deregister(void *phdcpcontext)
{
	struct hdcp_lib_handle *handle = phdcpcontext;

	if (!handle)
		return;

	kthread_stop(handle->thread);

	kzfree(handle->qseecom_handle);
	kzfree(handle->last_msg_recvd_buf);

	mutex_destroy(&handle->wakeup_mutex);

	kzfree(handle->listener_buf);
	kzfree(handle);
}
