// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/completion.h>
#include <linux/gunyah.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/auxiliary_bus.h>
#include <linux/gunyah/gh_errno.h>
#include <linux/gunyah/gh_rm_drv.h>

#include <asm/gunyah.h>

#include "rsc_mgr.h"
#include "vm_mgr.h"

static bool enable_rm_probe;
module_param_named(enable_rm_probe, enable_rm_probe, bool, 0600);
MODULE_PARM_DESC(enable_rm_probe,
		 "Allow experimental AVF Resource Manager probe; disabled by default to avoid legacy RM conflicts");

static bool enable_legacy_rm_bridge = true;
module_param_named(enable_legacy_rm_bridge, enable_legacy_rm_bridge, bool, 0600);
MODULE_PARM_DESC(enable_legacy_rm_bridge,
		 "Register /dev/gunyah backed by the existing Qualcomm gh_rm_drv transport");

static bool trace_rm_lifecycle;
module_param_named(trace_rm_lifecycle, trace_rm_lifecycle, bool, 0600);
MODULE_PARM_DESC(trace_rm_lifecycle,
		 "Trace experimental AVF Resource Manager lifecycle RPCs through the legacy bridge");

static uint legacy_rm_timeout_ms = 1000;
module_param_named(legacy_rm_timeout_ms, legacy_rm_timeout_ms, uint, 0600);
MODULE_PARM_DESC(legacy_rm_timeout_ms,
		 "Timeout for experimental legacy gh_rm_drv bridge RPCs in milliseconds; 0 uses the legacy infinite wait");

/* clang-format off */
#define RM_RPC_API_VERSION_MASK		GENMASK(3, 0)
#define RM_RPC_HEADER_WORDS_MASK	GENMASK(7, 4)
#define RM_RPC_API_VERSION		FIELD_PREP(RM_RPC_API_VERSION_MASK, 1)
#define RM_RPC_HEADER_WORDS		FIELD_PREP(RM_RPC_HEADER_WORDS_MASK, \
						(sizeof(struct gunyah_rm_rpc_hdr) / sizeof(u32)))
#define RM_RPC_API			(RM_RPC_API_VERSION | RM_RPC_HEADER_WORDS)

#define RM_RPC_TYPE_CONTINUATION	0x0
#define RM_RPC_TYPE_REQUEST		0x1
#define RM_RPC_TYPE_REPLY		0x2
#define RM_RPC_TYPE_NOTIF		0x3
#define RM_RPC_TYPE_MASK		GENMASK(1, 0)

#define GUNYAH_RM_MAX_NUM_FRAGMENTS		62
#define RM_RPC_FRAGMENTS_MASK		GENMASK(7, 2)
/* clang-format on */

struct gunyah_rm_rpc_hdr {
	u8 api;
	u8 type;
	__le16 seq;
	__le32 msg_id;
} __packed;

struct gunyah_rm_rpc_reply_hdr {
	struct gunyah_rm_rpc_hdr hdr;
	__le32 err_code; /* GUNYAH_RM_ERROR_* */
} __packed;

struct gunyah_rm_mem_wire_header {
	u8 mem_type;
	u8 _padding0;
	u8 flags;
	u8 _padding1;
	__le32 label;
} __packed;

struct gunyah_rm_mem_wire_acl_section {
	__le16 n_entries;
	__le16 _padding;
	struct gunyah_rm_mem_acl_entry entries[];
} __packed;

struct gunyah_rm_mem_wire_mem_section {
	__le16 n_entries;
	__le16 _padding;
	struct gunyah_rm_mem_entry entries[];
} __packed;

#define GUNYAH_RM_MSGQ_MSG_SIZE 240
#define GUNYAH_RM_PAYLOAD_SIZE \
	(GUNYAH_RM_MSGQ_MSG_SIZE - sizeof(struct gunyah_rm_rpc_hdr))

/* RM Error codes */
enum gunyah_rm_error {
	/* clang-format off */
	GUNYAH_RM_ERROR_OK			= 0x0,
	GUNYAH_RM_ERROR_UNIMPLEMENTED		= 0xFFFFFFFF,
	GUNYAH_RM_ERROR_NOMEM			= 0x1,
	GUNYAH_RM_ERROR_NORESOURCE		= 0x2,
	GUNYAH_RM_ERROR_DENIED			= 0x3,
	GUNYAH_RM_ERROR_INVALID			= 0x4,
	GUNYAH_RM_ERROR_BUSY			= 0x5,
	GUNYAH_RM_ERROR_ARGUMENT_INVALID	= 0x6,
	GUNYAH_RM_ERROR_HANDLE_INVALID		= 0x7,
	GUNYAH_RM_ERROR_VALIDATE_FAILED		= 0x8,
	GUNYAH_RM_ERROR_MAP_FAILED		= 0x9,
	GUNYAH_RM_ERROR_MEM_INVALID		= 0xA,
	GUNYAH_RM_ERROR_MEM_INUSE		= 0xB,
	GUNYAH_RM_ERROR_MEM_RELEASED		= 0xC,
	GUNYAH_RM_ERROR_VMID_INVALID		= 0xD,
	GUNYAH_RM_ERROR_LOOKUP_FAILED		= 0xE,
	GUNYAH_RM_ERROR_IRQ_INVALID		= 0xF,
	GUNYAH_RM_ERROR_IRQ_INUSE		= 0x10,
	GUNYAH_RM_ERROR_IRQ_RELEASED		= 0x11,
	/* clang-format on */
};

/**
 * struct gunyah_rm_message - Represents a complete message from resource manager
 * @payload: Combined payload of all the fragments (msg headers stripped off).
 * @size: Size of the payload received so far.
 * @msg_id: Message ID from the header.
 * @type: RM_RPC_TYPE_REPLY or RM_RPC_TYPE_NOTIF.
 * @num_fragments: total number of fragments expected to be received.
 * @fragments_received: fragments received so far.
 * @reply: Fields used for request/reply sequences
 */
struct gunyah_rm_message {
	void *payload;
	size_t size;
	u32 msg_id;
	u8 type;

	u8 num_fragments;
	u8 fragments_received;

	/**
	 * @ret: Linux return code, there was an error processing message
	 * @seq: Sequence ID for the main message.
	 * @rm_error: For request/reply sequences with standard replies
	 * @seq_done: Signals caller that the RM reply has been received
	 */
	struct {
		int ret;
		u16 seq;
		enum gunyah_rm_error rm_error;
		struct completion seq_done;
	} reply;
};

/**
 * struct gunyah_rm - private data for communicating w/Gunyah resource manager
 * @dev: pointer to RM platform device
 * @tx_ghrsc: message queue resource to TX to RM
 * @rx_ghrsc: message queue resource to RX from RM
 * @active_rx_message: ongoing gunyah_rm_message for which we're receiving fragments
 * @call_xarray: xarray to allocate & lookup sequence IDs for Request/Response flows
 * @next_seq: next ID to allocate (for xa_alloc_cyclic)
 * @recv_msg: cached allocation for Rx messages
 * @send_msg: cached allocation for Tx messages. Must hold @send_lock to manipulate.
 * @send_lock: synchronization to allow only one request to be sent at a time
 * @send_ready: completed when we know Tx message queue can take more messages
 * @nh: notifier chain for clients interested in RM notification messages
 * @miscdev: /dev/gunyah
 * @parent_fwnode: Parent IRQ fwnode to translate Gunyah hwirqs to Linux irqs
 */
struct gunyah_rm {
	struct device *dev;
	struct gunyah_resource tx_ghrsc;
	struct gunyah_resource rx_ghrsc;
	struct gunyah_rm_message *active_rx_message;

	struct xarray call_xarray;
	u32 next_seq;

	unsigned char recv_msg[GUNYAH_RM_MSGQ_MSG_SIZE];
	unsigned char send_msg[GUNYAH_RM_MSGQ_MSG_SIZE];
	struct mutex send_lock;
	struct completion send_ready;
	struct blocking_notifier_head nh;

	struct auxiliary_device adev;
	struct miscdevice miscdev;
	struct fwnode_handle *parent_fwnode;
	struct notifier_block legacy_nb;
	bool legacy_bridge;
};

static struct gunyah_rm *legacy_bridge_rm;

static const char *gunyah_rm_lifecycle_msg_name(u32 message_id)
{
	switch (message_id) {
	case 0x51000012:
		return "MEM_LEND";
	case 0x51000013:
		return "MEM_SHARE";
	case 0x51000015:
		return "MEM_RECLAIM";
	case 0x51000018:
		return "MEM_APPEND";
	case 0x56000001:
		return "VM_ALLOC_VMID";
	case 0x56000002:
		return "VM_DEALLOC_VMID";
	case 0x56000004:
		return "VM_START";
	case 0x56000005:
		return "VM_STOP";
	case 0x56000006:
		return "VM_RESET";
	case 0x56000009:
		return "VM_CONFIG_IMAGE";
	case 0x5600000B:
		return "VM_INIT";
	case 0x56000020:
		return "VM_GET_HYP_RESOURCES";
	case 0x56000024:
		return "VM_GET_VMID";
	case 0x56000031:
		return "VM_SET_BOOT_CONTEXT";
	case 0x56000032:
		return "VM_SET_FIRMWARE_MEM";
	case 0x56000033:
		return "VM_SET_DEMAND_PAGING";
	case 0x56000034:
		return "VM_SET_ADDRESS_LAYOUT";
	default:
		return NULL;
	}
}

static const char *gunyah_legacy_error_name(int error)
{
	switch (error) {
	case GH_ERROR_OK:
		return "GH_ERROR_OK";
	case GH_ERROR_UNIMPLEMENTED:
		return "GH_ERROR_UNIMPLEMENTED";
	case GH_ERROR_ARG_INVAL:
		return "GH_ERROR_ARG_INVAL";
	case GH_ERROR_ARG_SIZE:
		return "GH_ERROR_ARG_SIZE";
	case GH_ERROR_ARG_ALIGN:
		return "GH_ERROR_ARG_ALIGN";
	case GH_ERROR_NOMEM:
		return "GH_ERROR_NOMEM";
	case GH_ERROR_ADDR_OVFL:
		return "GH_ERROR_ADDR_OVFL";
	case GH_ERROR_ADDR_UNFL:
		return "GH_ERROR_ADDR_UNFL";
	case GH_ERROR_ADDR_INVAL:
		return "GH_ERROR_ADDR_INVAL";
	case GH_ERROR_DENIED:
		return "GH_ERROR_DENIED";
	case GH_ERROR_BUSY:
		return "GH_ERROR_BUSY";
	case GH_ERROR_IDLE:
		return "GH_ERROR_IDLE";
	case GH_ERROR_IRQ_BOUND:
		return "GH_ERROR_IRQ_BOUND";
	case GH_ERROR_IRQ_UNBOUND:
		return "GH_ERROR_IRQ_UNBOUND";
	case GH_ERROR_CSPACE_CAP_NULL:
		return "GH_ERROR_CSPACE_CAP_NULL";
	case GH_ERROR_CSPACE_CAP_REVOKED:
		return "GH_ERROR_CSPACE_CAP_REVOKED";
	case GH_ERROR_CSPACE_WRONG_OBJ_TYPE:
		return "GH_ERROR_CSPACE_WRONG_OBJ_TYPE";
	case GH_ERROR_CSPACE_INSUF_RIGHTS:
		return "GH_ERROR_CSPACE_INSUF_RIGHTS";
	case GH_ERROR_CSPACE_FULL:
		return "GH_ERROR_CSPACE_FULL";
	case GH_ERROR_MSGQUEUE_EMPTY:
		return "GH_ERROR_MSGQUEUE_EMPTY";
	case GH_ERROR_MSGQUEUE_FULL:
		return "GH_ERROR_MSGQUEUE_FULL";
	default:
		return "GH_ERROR_UNKNOWN";
	}
}

static bool gunyah_rm_is_mem_lifecycle_msg(u32 message_id)
{
	return message_id == 0x51000012 || message_id == 0x51000013;
}

static void gunyah_rm_log_mem_payload(struct device *dev, u32 message_id,
				      const void *req_buf, size_t req_buf_size,
				      const char *reason)
{
	const struct gunyah_rm_mem_wire_header *hdr;
	const struct gunyah_rm_mem_wire_acl_section *acl;
	const struct gunyah_rm_mem_wire_mem_section *mem;
	const u8 *buf = req_buf;
	size_t offset, acl_size, mem_size, i, shown;
	u16 n_acl, n_mem;
	u32 attr_entries = 0;
	__le32 attr_le;
	const char *name = gunyah_rm_lifecycle_msg_name(message_id);

	if (!gunyah_rm_is_mem_lifecycle_msg(message_id))
		return;

	if (!req_buf || req_buf_size < sizeof(*hdr)) {
		dev_info(dev, "%s payload %s malformed: req=%zu\n",
			 name ?: "MEM", reason, req_buf_size);
		return;
	}

	hdr = req_buf;
	offset = sizeof(*hdr);
	if (req_buf_size - offset < sizeof(*acl)) {
		dev_info(dev,
			 "%s payload %s short before ACL: req=%zu mem_type=%u flags=0x%x label=%u\n",
			 name ?: "MEM", reason, req_buf_size, hdr->mem_type,
			 hdr->flags, le32_to_cpu(hdr->label));
		return;
	}

	acl = (const void *)(buf + offset);
	n_acl = le16_to_cpu(acl->n_entries);
	if (n_acl > (req_buf_size - offset - sizeof(*acl)) /
		    sizeof(*acl->entries)) {
		dev_info(dev,
			 "%s payload %s truncated ACL: req=%zu n_acl=%u\n",
			 name ?: "MEM", reason, req_buf_size, n_acl);
		return;
	}
	acl_size = sizeof(*acl) + n_acl * sizeof(*acl->entries);
	offset += acl_size;

	if (req_buf_size - offset < sizeof(*mem)) {
		dev_info(dev,
			 "%s payload %s short before mem entries: req=%zu n_acl=%u\n",
			 name ?: "MEM", reason, req_buf_size, n_acl);
		return;
	}

	mem = (const void *)(buf + offset);
	n_mem = le16_to_cpu(mem->n_entries);
	if (n_mem > (req_buf_size - offset - sizeof(*mem)) /
		    sizeof(*mem->entries)) {
		dev_info(dev,
			 "%s payload %s truncated mem entries: req=%zu n_acl=%u n_mem=%u\n",
			 name ?: "MEM", reason, req_buf_size, n_acl, n_mem);
		return;
	}
	mem_size = sizeof(*mem) + n_mem * sizeof(*mem->entries);
	offset += mem_size;

	if (req_buf_size - offset >= sizeof(attr_le)) {
		memcpy(&attr_le, buf + offset, sizeof(attr_le));
		attr_entries = le32_to_cpu(attr_le);
	}

	dev_info(dev,
		 "%s payload %s: req=%zu mem_type=%u flags=0x%x label=%u acl_entries=%u mem_entries=%u attr_entries=%u\n",
		 name ?: "MEM", reason, req_buf_size, hdr->mem_type, hdr->flags,
		 le32_to_cpu(hdr->label), n_acl, n_mem, attr_entries);

	shown = min_t(size_t, n_acl, 4);
	for (i = 0; i < shown; i++)
		dev_info(dev, "%s acl[%zu]: vmid=%u perms=0x%x\n",
			 name ?: "MEM", i, le16_to_cpu(acl->entries[i].vmid),
			 acl->entries[i].perms);
	if (n_acl > shown)
		dev_info(dev, "%s acl: ... %u total entries\n",
			 name ?: "MEM", n_acl);

	shown = min_t(size_t, n_mem, 4);
	for (i = 0; i < shown; i++)
		dev_info(dev, "%s mem[%zu]: phys=0x%llx size=0x%llx\n",
			 name ?: "MEM", i,
			 (unsigned long long)le64_to_cpu(mem->entries[i].phys_addr),
			 (unsigned long long)le64_to_cpu(mem->entries[i].size));
	if (n_mem > shown) {
		i = n_mem - 1;
		dev_info(dev,
			 "%s mem: ... %u total entries; last[%zu] phys=0x%llx size=0x%llx\n",
			 name ?: "MEM", n_mem, i,
			 (unsigned long long)le64_to_cpu(mem->entries[i].phys_addr),
			 (unsigned long long)le64_to_cpu(mem->entries[i].size));
	}
}

/**
 * gunyah_rm_error_remap() - Remap Gunyah resource manager errors into a Linux error code
 * @rm_error: "Standard" return value from Gunyah resource manager
 */
static inline int gunyah_rm_error_remap(enum gunyah_rm_error rm_error)
{
	switch (rm_error) {
	case GUNYAH_RM_ERROR_OK:
		return 0;
	case GUNYAH_RM_ERROR_UNIMPLEMENTED:
		return -EOPNOTSUPP;
	case GUNYAH_RM_ERROR_NOMEM:
		return -ENOMEM;
	case GUNYAH_RM_ERROR_NORESOURCE:
		return -ENODEV;
	case GUNYAH_RM_ERROR_DENIED:
		return -EPERM;
	case GUNYAH_RM_ERROR_BUSY:
		return -EBUSY;
	case GUNYAH_RM_ERROR_INVALID:
	case GUNYAH_RM_ERROR_ARGUMENT_INVALID:
	case GUNYAH_RM_ERROR_HANDLE_INVALID:
	case GUNYAH_RM_ERROR_VALIDATE_FAILED:
	case GUNYAH_RM_ERROR_MAP_FAILED:
	case GUNYAH_RM_ERROR_MEM_INVALID:
	case GUNYAH_RM_ERROR_MEM_INUSE:
	case GUNYAH_RM_ERROR_MEM_RELEASED:
	case GUNYAH_RM_ERROR_VMID_INVALID:
	case GUNYAH_RM_ERROR_LOOKUP_FAILED:
	case GUNYAH_RM_ERROR_IRQ_INVALID:
	case GUNYAH_RM_ERROR_IRQ_INUSE:
	case GUNYAH_RM_ERROR_IRQ_RELEASED:
		return -EINVAL;
	default:
		return -EBADMSG;
	}
}

struct gunyah_resource *
gunyah_rm_alloc_resource(struct gunyah_rm *rm,
			 struct gunyah_rm_hyp_resource *hyp_resource)
{
	struct gunyah_resource *ghrsc;
	int ret;

	ghrsc = kzalloc(sizeof(*ghrsc), GFP_KERNEL);
	if (!ghrsc)
		return NULL;

	ghrsc->type = hyp_resource->type;
	ghrsc->capid = le64_to_cpu(hyp_resource->cap_id);
	ghrsc->irq = IRQ_NOTCONNECTED;
	ghrsc->rm_label = le32_to_cpu(hyp_resource->resource_label);
	if (hyp_resource->virq) {
		u32 virq = le32_to_cpu(hyp_resource->virq);

		if (rm->legacy_bridge) {
			ret = gh_rm_virq_to_irq(virq, IRQ_TYPE_EDGE_RISING);
			if (ret <= 0) {
				dev_err(rm->dev,
					"Failed to translate legacy RM interrupt for resource %d label: %d virq: %u err: %d\n",
					ghrsc->type, ghrsc->rm_label, virq, ret);
				kfree(ghrsc);
				return NULL;
			}

			ghrsc->irq = ret;
			return ghrsc;
		} else {
			struct irq_fwspec fwspec;

			fwspec.fwnode = rm->parent_fwnode;
			ret = arch_gunyah_fill_irq_fwspec_params(virq, &fwspec);
			if (ret) {
				dev_err(rm->dev,
					"Failed to translate interrupt for resource %d label: %d: %d\n",
					ghrsc->type, ghrsc->rm_label, ret);
				kfree(ghrsc);
				return NULL;
			}

			ret = irq_create_fwspec_mapping(&fwspec);
			if (ret <= 0) {
				dev_err(rm->dev,
					"Failed to allocate interrupt for resource %d label: %d: %d\n",
					ghrsc->type, ghrsc->rm_label, ret);
				kfree(ghrsc);
				return NULL;
			}
			ghrsc->irq = ret;
		}
	}

	return ghrsc;
}

void gunyah_rm_free_resource(struct gunyah_resource *ghrsc)
{
	if (ghrsc->irq > 0)
		irq_dispose_mapping(ghrsc->irq);
	kfree(ghrsc);
}

static int gunyah_rm_init_message_payload(struct gunyah_rm_message *message,
					  const void *msg, size_t hdr_size,
					  size_t msg_size)
{
	const struct gunyah_rm_rpc_hdr *hdr = msg;
	size_t max_buf_size, payload_size;

	if (msg_size < hdr_size)
		return -EINVAL;

	payload_size = msg_size - hdr_size;

	message->num_fragments = FIELD_GET(RM_RPC_FRAGMENTS_MASK, hdr->type);
	message->fragments_received = 0;

	/* There's not going to be any payload, no need to allocate buffer. */
	if (!payload_size && !message->num_fragments)
		return 0;

	if (message->num_fragments > GUNYAH_RM_MAX_NUM_FRAGMENTS)
		return -EINVAL;

	max_buf_size = payload_size +
		       (message->num_fragments * GUNYAH_RM_PAYLOAD_SIZE);

	message->payload = kzalloc(max_buf_size, GFP_KERNEL);
	if (!message->payload)
		return -ENOMEM;

	memcpy(message->payload, msg + hdr_size, payload_size);
	message->size = payload_size;
	return 0;
}

static void gunyah_rm_abort_message(struct gunyah_rm *rm)
{
	kfree(rm->active_rx_message->payload);

	switch (rm->active_rx_message->type) {
	case RM_RPC_TYPE_REPLY:
		rm->active_rx_message->reply.ret = -EIO;
		complete(&rm->active_rx_message->reply.seq_done);
		break;
	case RM_RPC_TYPE_NOTIF:
		fallthrough;
	default:
		kfree(rm->active_rx_message);
	}

	rm->active_rx_message = NULL;
}

static inline void gunyah_rm_try_complete_message(struct gunyah_rm *rm)
{
	struct gunyah_rm_message *message = rm->active_rx_message;

	if (!message || message->fragments_received != message->num_fragments)
		return;

	switch (message->type) {
	case RM_RPC_TYPE_REPLY:
		complete(&message->reply.seq_done);
		break;
	case RM_RPC_TYPE_NOTIF:
		blocking_notifier_call_chain(&rm->nh, message->msg_id,
					     message->payload);

		kfree(message->payload);
		kfree(message);
		break;
	default:
		dev_err_ratelimited(rm->dev,
				    "Invalid message type (%u) received\n",
				    message->type);
		gunyah_rm_abort_message(rm);
		break;
	}

	rm->active_rx_message = NULL;
}

static void gunyah_rm_process_notif(struct gunyah_rm *rm, const void *msg,
				    size_t msg_size)
{
	const struct gunyah_rm_rpc_hdr *hdr = msg;
	struct gunyah_rm_message *message;
	int ret;

	if (rm->active_rx_message) {
		dev_err(rm->dev,
			"Unexpected new notification, still processing an active message");
		gunyah_rm_abort_message(rm);
	}

	message = kzalloc(sizeof(*message), GFP_KERNEL);
	if (!message)
		return;

	message->type = RM_RPC_TYPE_NOTIF;
	message->msg_id = le32_to_cpu(hdr->msg_id);

	ret = gunyah_rm_init_message_payload(message, msg, sizeof(*hdr),
					     msg_size);
	if (ret) {
		dev_err(rm->dev,
			"Failed to initialize message for notification: %d\n",
			ret);
		kfree(message);
		return;
	}

	rm->active_rx_message = message;

	gunyah_rm_try_complete_message(rm);
}

static void gunyah_rm_process_reply(struct gunyah_rm *rm, const void *msg,
				    size_t msg_size)
{
	const struct gunyah_rm_rpc_reply_hdr *reply_hdr = msg;
	struct gunyah_rm_message *message;
	u16 seq_id;

	seq_id = le16_to_cpu(reply_hdr->hdr.seq);
	message = xa_load(&rm->call_xarray, seq_id);

	if (!message || message->msg_id != le32_to_cpu(reply_hdr->hdr.msg_id))
		return;

	if (rm->active_rx_message) {
		dev_err(rm->dev,
			"Unexpected new reply, still processing an active message");
		gunyah_rm_abort_message(rm);
	}

	if (gunyah_rm_init_message_payload(message, msg, sizeof(*reply_hdr),
					   msg_size)) {
		dev_err(rm->dev,
			"Failed to alloc message buffer for sequence %d\n",
			seq_id);
		/* Send message complete and error the client. */
		message->reply.ret = -ENOMEM;
		complete(&message->reply.seq_done);
		return;
	}

	message->reply.rm_error = le32_to_cpu(reply_hdr->err_code);
	rm->active_rx_message = message;

	gunyah_rm_try_complete_message(rm);
}

static void gunyah_rm_process_cont(struct gunyah_rm *rm,
				   struct gunyah_rm_message *message,
				   const void *msg, size_t msg_size)
{
	const struct gunyah_rm_rpc_hdr *hdr = msg;
	size_t payload_size = msg_size - sizeof(*hdr);

	if (!rm->active_rx_message)
		return;

	/*
	 * hdr->fragments and hdr->msg_id preserves the value from first reply
	 * or notif message. To detect mishandling, check it's still intact.
	 */
	if (message->msg_id != le32_to_cpu(hdr->msg_id) ||
	    message->num_fragments !=
		    FIELD_GET(RM_RPC_FRAGMENTS_MASK, hdr->type)) {
		gunyah_rm_abort_message(rm);
		return;
	}

	memcpy(message->payload + message->size, msg + sizeof(*hdr),
	       payload_size);
	message->size += payload_size;
	message->fragments_received++;

	gunyah_rm_try_complete_message(rm);
}

static irqreturn_t gunyah_rm_rx(int irq, void *data)
{
	enum gunyah_error gunyah_error;
	struct gunyah_rm_rpc_hdr *hdr;
	struct gunyah_rm *rm = data;
	void *msg = &rm->recv_msg[0];
	size_t len;
	bool ready;

	do {
		gunyah_error = gunyah_hypercall_msgq_recv(rm->rx_ghrsc.capid,
							  msg,
							  sizeof(rm->recv_msg),
							  &len, &ready);
		if (gunyah_error != GUNYAH_ERROR_OK) {
			if (gunyah_error != GUNYAH_ERROR_MSGQUEUE_EMPTY)
				dev_warn(rm->dev,
					 "Failed to receive data: %d\n",
					 gunyah_error);
			return IRQ_HANDLED;
		}

		if (len < sizeof(*hdr)) {
			dev_err_ratelimited(
				rm->dev,
				"Too small message received. size=%ld\n", len);
			continue;
		}

		hdr = msg;
		if (hdr->api != RM_RPC_API) {
			dev_err(rm->dev, "Unknown RM RPC API version: %x\n",
				hdr->api);
			return IRQ_HANDLED;
		}

		switch (FIELD_GET(RM_RPC_TYPE_MASK, hdr->type)) {
		case RM_RPC_TYPE_NOTIF:
			gunyah_rm_process_notif(rm, msg, len);
			break;
		case RM_RPC_TYPE_REPLY:
			gunyah_rm_process_reply(rm, msg, len);
			break;
		case RM_RPC_TYPE_CONTINUATION:
			gunyah_rm_process_cont(rm, rm->active_rx_message, msg,
					       len);
			break;
		default:
			dev_err(rm->dev,
				"Invalid message type (%lu) received\n",
				FIELD_GET(RM_RPC_TYPE_MASK, hdr->type));
			return IRQ_HANDLED;
		}
	} while (ready);

	return IRQ_HANDLED;
}

static irqreturn_t gunyah_rm_tx(int irq, void *data)
{
	struct gunyah_rm *rm = data;

	complete(&rm->send_ready);

	return IRQ_HANDLED;
}

static int gunyah_rm_msgq_send(struct gunyah_rm *rm, size_t size, bool push)
{
	const u64 tx_flags = push ? GUNYAH_HYPERCALL_MSGQ_TX_FLAGS_PUSH : 0;
	enum gunyah_error gunyah_error;
	void *data = &rm->send_msg[0];
	bool ready;

	lockdep_assert_held(&rm->send_lock);

again:
	wait_for_completion(&rm->send_ready);
	gunyah_error = gunyah_hypercall_msgq_send(rm->tx_ghrsc.capid, size,
						  data, tx_flags, &ready);

	/* Should never happen because Linux properly tracks the ready-state of the msgq */
	if (WARN_ON(gunyah_error == GUNYAH_ERROR_MSGQUEUE_FULL))
		goto again;

	if (ready)
		complete(&rm->send_ready);

	return gunyah_error_remap(gunyah_error);
}

static int gunyah_rm_send_request(struct gunyah_rm *rm, u32 message_id,
				  const void *req_buf, size_t req_buf_size,
				  struct gunyah_rm_message *message)
{
	size_t buf_size_remaining = req_buf_size;
	const void *req_buf_curr = req_buf;
	struct gunyah_rm_rpc_hdr *hdr =
		(struct gunyah_rm_rpc_hdr *)&rm->send_msg[0];
	struct gunyah_rm_rpc_hdr hdr_template;
	void *payload = hdr + 1;
	u32 cont_fragments = 0;
	size_t payload_size;
	bool push;
	int ret;

	if (req_buf_size >
	    GUNYAH_RM_MAX_NUM_FRAGMENTS * GUNYAH_RM_PAYLOAD_SIZE) {
		dev_warn(
			rm->dev,
			"Limit (%lu bytes) exceeded for the maximum message size: %lu\n",
			GUNYAH_RM_MAX_NUM_FRAGMENTS * GUNYAH_RM_PAYLOAD_SIZE,
			req_buf_size);
		dump_stack();
		return -E2BIG;
	}

	if (req_buf_size)
		cont_fragments = (req_buf_size - 1) / GUNYAH_RM_PAYLOAD_SIZE;

	hdr_template.api = RM_RPC_API;
	hdr_template.type = FIELD_PREP(RM_RPC_TYPE_MASK, RM_RPC_TYPE_REQUEST) |
			    FIELD_PREP(RM_RPC_FRAGMENTS_MASK, cont_fragments);
	hdr_template.seq = cpu_to_le16(message->reply.seq);
	hdr_template.msg_id = cpu_to_le32(message_id);

	do {
		*hdr = hdr_template;

		/* Copy payload */
		payload_size = min(buf_size_remaining, GUNYAH_RM_PAYLOAD_SIZE);
		memcpy(payload, req_buf_curr, payload_size);
		req_buf_curr += payload_size;
		buf_size_remaining -= payload_size;

		/* Only the last message should have push flag set */
		push = !buf_size_remaining;
		ret = gunyah_rm_msgq_send(rm, sizeof(*hdr) + payload_size,
					  push);
		if (ret)
			break;

		hdr_template.type =
			FIELD_PREP(RM_RPC_TYPE_MASK, RM_RPC_TYPE_CONTINUATION) |
			FIELD_PREP(RM_RPC_FRAGMENTS_MASK, cont_fragments);
	} while (buf_size_remaining);

	return ret;
}

/**
 * gunyah_rm_call: Achieve request-response type communication with RPC
 * @rm: Pointer to Gunyah resource manager internal data
 * @message_id: The RM RPC message-id
 * @req_buf: Request buffer that contains the payload
 * @req_buf_size: Total size of the payload
 * @resp_buf: Pointer to a response buffer
 * @resp_buf_size: Size of the response buffer
 *
 * Make a request to the Resource Manager and wait for reply back. For a successful
 * response, the function returns the payload. The size of the payload is set in
 * resp_buf_size. The resp_buf must be freed by the caller when 0 is returned
 * and resp_buf_size != 0.
 *
 * req_buf should be not NULL for req_buf_size >0. If req_buf_size == 0,
 * req_buf *can* be NULL and no additional payload is sent.
 *
 * Context: Process context. Will sleep waiting for reply.
 * Return: 0 on success. <0 if error.
 */
static int gunyah_rm_legacy_call(struct gunyah_rm *rm, u32 message_id,
				 const void *req_buf, size_t req_buf_size,
				 void **resp_buf, size_t *resp_buf_size)
{
	const char *trace_name = gunyah_rm_lifecycle_msg_name(message_id);
	size_t size = 0;
	int rm_error = 0;
	void *resp;
	long ret;

	if (trace_rm_lifecycle && trace_name)
		dev_info(rm->dev,
			 "rm_lifecycle request %s msg=%08x req=%zu wants_resp=%u\n",
			 trace_name, message_id, req_buf_size, resp_buf ? 1 : 0);
	if (trace_rm_lifecycle && gunyah_rm_is_mem_lifecycle_msg(message_id))
		gunyah_rm_log_mem_payload(rm->dev, message_id, req_buf,
					  req_buf_size, "request");

	resp = gh_rm_call_raw_timeout(message_id, (void *)req_buf, req_buf_size,
				       &size, &rm_error, legacy_rm_timeout_ms);
	if (IS_ERR(resp)) {
		ret = PTR_ERR(resp);
		if (trace_rm_lifecycle && trace_name)
			dev_info(rm->dev,
				 "rm_lifecycle reply %s msg=%08x ret=%ld rm_error=%d(%s) resp=%zu timeout_ms=%u\n",
				 trace_name, message_id, ret, rm_error,
				 gunyah_legacy_error_name(rm_error), size,
				 legacy_rm_timeout_ms);
		if (!trace_rm_lifecycle && gunyah_rm_is_mem_lifecycle_msg(message_id))
			gunyah_rm_log_mem_payload(rm->dev, message_id, req_buf,
						  req_buf_size, "rejected");
		dev_warn(rm->dev,
			 "legacy RM rejected message %08x. RM error: %d(%s) Linux error: %ld timeout_ms: %u\n",
			 message_id, rm_error, gunyah_legacy_error_name(rm_error),
			 ret, legacy_rm_timeout_ms);
		return ret;
	}

	if (trace_rm_lifecycle && trace_name)
		dev_info(rm->dev,
			 "rm_lifecycle reply %s msg=%08x ret=0 rm_error=%d(%s) resp=%zu\n",
			 trace_name, message_id, rm_error,
			 gunyah_legacy_error_name(rm_error), size);

	if (resp_buf_size)
		*resp_buf_size = size;

	if (resp_buf && size) {
		*resp_buf = resp;
	} else {
		if (resp_buf)
			*resp_buf = NULL;
		kfree(resp);
	}

	return 0;
}

int gunyah_rm_call(struct gunyah_rm *rm, u32 message_id, const void *req_buf,
		   size_t req_buf_size, void **resp_buf, size_t *resp_buf_size)
{
	struct gunyah_rm_message message = { 0 };
	u32 seq_id;
	int ret;

	/* message_id 0 is reserved. req_buf_size implies req_buf is not NULL */
	if (!rm || !message_id || (!req_buf && req_buf_size))
		return -EINVAL;

	if (rm->legacy_bridge)
		return gunyah_rm_legacy_call(rm, message_id, req_buf, req_buf_size,
					      resp_buf, resp_buf_size);

	message.type = RM_RPC_TYPE_REPLY;
	message.msg_id = message_id;

	message.reply.seq_done =
		COMPLETION_INITIALIZER_ONSTACK(message.reply.seq_done);

	/* Allocate a new seq number for this message */
	ret = xa_alloc_cyclic(&rm->call_xarray, &seq_id, &message, xa_limit_16b,
			      &rm->next_seq, GFP_KERNEL);
	if (ret < 0)
		return ret;
	message.reply.seq = lower_16_bits(seq_id);

	mutex_lock(&rm->send_lock);
	/* Send the request to the Resource Manager */
	ret = gunyah_rm_send_request(rm, message_id, req_buf, req_buf_size,
				     &message);
	if (ret < 0) {
		dev_warn(rm->dev, "Failed to send request. Error: %d\n", ret);
		goto out;
	}

	/*
	 * Wait for response. Uninterruptible because rollback based on what RM did to VM
	 * requires us to know how RM handled the call.
	 */
	wait_for_completion(&message.reply.seq_done);

	/* Check for internal (kernel) error waiting for the response */
	if (message.reply.ret) {
		ret = message.reply.ret;
		goto out;
	}

	/* Got a response, did resource manager give us an error? */
	if (message.reply.rm_error != GUNYAH_RM_ERROR_OK) {
		dev_warn(rm->dev, "RM rejected message %08x. Error: %d\n",
			 message_id, message.reply.rm_error);
		ret = gunyah_rm_error_remap(message.reply.rm_error);
		kfree(message.payload);
		goto out;
	}

	/* Everything looks good, return the payload */
	if (resp_buf_size)
		*resp_buf_size = message.size;

	if (message.size && resp_buf) {
		*resp_buf = message.payload;
	} else {
		/* kfree in case RM sent us multiple fragments but never any data in
		 * those fragments. We would've allocated memory for it, but message.size == 0
		 */
		kfree(message.payload);
	}

out:
	mutex_unlock(&rm->send_lock);
	xa_erase(&rm->call_xarray, message.reply.seq);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_rm_call);

int gunyah_rm_notifier_register(struct gunyah_rm *rm, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&rm->nh, nb);
}
EXPORT_SYMBOL_GPL(gunyah_rm_notifier_register);

int gunyah_rm_notifier_unregister(struct gunyah_rm *rm,
				  struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&rm->nh, nb);
}
EXPORT_SYMBOL_GPL(gunyah_rm_notifier_unregister);

struct device *gunyah_rm_get(struct gunyah_rm *rm)
{
	return get_device(rm->miscdev.this_device);
}
EXPORT_SYMBOL_GPL(gunyah_rm_get);

void gunyah_rm_put(struct gunyah_rm *rm)
{
	put_device(rm->miscdev.this_device);
}
EXPORT_SYMBOL_GPL(gunyah_rm_put);

static long gunyah_dev_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct miscdevice *miscdev = filp->private_data;
	struct gunyah_rm *rm = container_of(miscdev, struct gunyah_rm, miscdev);

	return gunyah_dev_vm_mgr_ioctl(rm, cmd, arg);
}

static const struct file_operations gunyah_dev_fops = {
	/* clang-format off */
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= gunyah_dev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
	/* clang-format on */
};

static int gunyah_rm_legacy_notifier_call(struct notifier_block *nb,
					  unsigned long action, void *data)
{
	struct gunyah_rm *rm = container_of(nb, struct gunyah_rm, legacy_nb);

	blocking_notifier_call_chain(&rm->nh, action, data);
	return NOTIFY_OK;
}

static int gunyah_rm_register_legacy_bridge(void)
{
	struct gunyah_rm *rm;
	int ret;

	rm = kzalloc(sizeof(*rm), GFP_KERNEL);
	if (!rm)
		return -ENOMEM;

	mutex_init(&rm->send_lock);
	init_completion(&rm->send_ready);
	BLOCKING_INIT_NOTIFIER_HEAD(&rm->nh);
	xa_init_flags(&rm->call_xarray, XA_FLAGS_ALLOC);
	rm->legacy_bridge = true;

	rm->miscdev.name = "gunyah";
	rm->miscdev.minor = MISC_DYNAMIC_MINOR;
	rm->miscdev.fops = &gunyah_dev_fops;

	ret = misc_register(&rm->miscdev);
	if (ret) {
		pr_err("gunyah_avf: failed to register legacy-backed /dev/gunyah: %d\n",
		       ret);
		goto err_free_rm;
	}

	rm->dev = rm->miscdev.this_device;
	rm->legacy_nb.notifier_call = gunyah_rm_legacy_notifier_call;
	ret = gh_rm_register_notifier(&rm->legacy_nb);
	if (ret) {
		dev_err(rm->dev,
			"failed to register legacy RM notifier bridge: %d\n", ret);
		goto err_deregister_misc;
	}

	legacy_bridge_rm = rm;
	dev_info(rm->dev, "registered experimental legacy-backed /dev/gunyah\n");
	return 0;

err_deregister_misc:
	misc_deregister(&rm->miscdev);
err_free_rm:
	xa_destroy(&rm->call_xarray);
	kfree(rm);
	return ret;
}

static void gunyah_rm_unregister_legacy_bridge(void)
{
	struct gunyah_rm *rm = legacy_bridge_rm;

	if (!rm)
		return;

	legacy_bridge_rm = NULL;
	gh_rm_unregister_notifier(&rm->legacy_nb);
	misc_deregister(&rm->miscdev);
	xa_destroy(&rm->call_xarray);
	kfree(rm);
}

static int gunyah_platform_probe_capability(struct platform_device *pdev,
					    int idx,
					    struct gunyah_resource *ghrsc)
{
	int ret;

	ghrsc->irq = platform_get_irq(pdev, idx);
	if (ghrsc->irq < 0) {
		dev_err(&pdev->dev, "Failed to get %s irq: %d\n",
			idx ? "rx" : "tx", ghrsc->irq);
		return ghrsc->irq;
	}

	ret = of_property_read_u64_index(pdev->dev.of_node, "reg", idx,
					 &ghrsc->capid);
	if (ret) {
		dev_err(&pdev->dev, "Failed to get %s capid: %d\n",
			idx ? "rx" : "tx", ret);
		return ret;
	}

	return 0;
}

static int gunyah_rm_probe_tx_msgq(struct gunyah_rm *rm,
				   struct platform_device *pdev)
{
	int ret;

	rm->tx_ghrsc.type = GUNYAH_RESOURCE_TYPE_MSGQ_TX;
	ret = gunyah_platform_probe_capability(pdev, 0, &rm->tx_ghrsc);
	if (ret)
		return ret;

	enable_irq_wake(rm->tx_ghrsc.irq);

	return devm_request_irq(rm->dev, rm->tx_ghrsc.irq, gunyah_rm_tx, 0,
				"gunyah_rm_tx", rm);
}

static int gunyah_rm_probe_rx_msgq(struct gunyah_rm *rm,
				   struct platform_device *pdev)
{
	int ret;

	rm->rx_ghrsc.type = GUNYAH_RESOURCE_TYPE_MSGQ_RX;
	ret = gunyah_platform_probe_capability(pdev, 1, &rm->rx_ghrsc);
	if (ret)
		return ret;

	enable_irq_wake(rm->rx_ghrsc.irq);

	return devm_request_threaded_irq(rm->dev, rm->rx_ghrsc.irq, NULL,
					 gunyah_rm_rx, IRQF_ONESHOT,
					 "gunyah_rm_rx", rm);
}

static void gunyah_adev_release(struct device *dev)
{
	/* no-op */
}

static int gunyah_adev_init(struct gunyah_rm *rm, const char *name)
{
	struct auxiliary_device *adev = &rm->adev;
	int ret = 0;

	adev->name = name;
	adev->dev.parent = rm->dev;
	adev->dev.release = gunyah_adev_release;
	ret = auxiliary_device_init(adev);
	if (ret)
		return ret;

	ret = auxiliary_device_add(adev);
	if (ret) {
		auxiliary_device_uninit(adev);
		return ret;
	}

	return ret;
}

static int gunyah_rm_probe(struct platform_device *pdev)
{
	struct device_node *parent_irq_node;
	struct gunyah_rm *rm;
	int ret;

	if (!enable_rm_probe)
		return -ENODEV;

	rm = devm_kzalloc(&pdev->dev, sizeof(*rm), GFP_KERNEL);
	if (!rm)
		return -ENOMEM;

	platform_set_drvdata(pdev, rm);
	rm->dev = &pdev->dev;

	mutex_init(&rm->send_lock);
	init_completion(&rm->send_ready);
	BLOCKING_INIT_NOTIFIER_HEAD(&rm->nh);
	xa_init_flags(&rm->call_xarray, XA_FLAGS_ALLOC);

	device_init_wakeup(&pdev->dev, true);

	ret = gunyah_rm_probe_tx_msgq(rm, pdev);
	if (ret)
		return ret;
	/* assume RM is ready to receive messages from us */
	complete(&rm->send_ready);

	ret = gunyah_rm_probe_rx_msgq(rm, pdev);
	if (ret)
		return ret;

	parent_irq_node = of_irq_find_parent(pdev->dev.of_node);
	if (!parent_irq_node) {
		dev_err(&pdev->dev,
			"Failed to find interrupt parent of resource manager\n");
		return -ENODEV;
	}

	rm->parent_fwnode = of_node_to_fwnode(parent_irq_node);
	if (!rm->parent_fwnode) {
		dev_err(&pdev->dev,
			"Failed to find interrupt parent domain of resource manager\n");
		return -ENODEV;
	}

	rm->miscdev.parent = &pdev->dev;
	rm->miscdev.name = "gunyah";
	rm->miscdev.minor = MISC_DYNAMIC_MINOR;
	rm->miscdev.fops = &gunyah_dev_fops;

	ret = misc_register(&rm->miscdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register gunyah misc device\n");
		return ret;
	} else {
		ret = gunyah_adev_init(rm, "gh_rm_core");
		if (ret) {
			dev_err(&pdev->dev, "Failed to add gh_rm_core device\n");
			return ret;
		}
	}
	return 0;
}

static int gunyah_rm_remove(struct platform_device *pdev)
{
	struct gunyah_rm *rm = platform_get_drvdata(pdev);

	misc_deregister(&rm->miscdev);
	return 0;
}

static const struct of_device_id gunyah_rm_of_match[] = {
	{ .compatible = "gunyah-resource-manager" },
	{}
};
MODULE_DEVICE_TABLE(of, gunyah_rm_of_match);

static struct platform_driver gunyah_rm_driver = {
	.probe = gunyah_rm_probe,
	.remove = gunyah_rm_remove,
	.driver = {
		.name = "gunyah_rsc_mgr",
		.of_match_table = gunyah_rm_of_match,
	},
};

static int __init gunyah_rm_driver_init(void)
{
	int ret;

	if (enable_rm_probe && enable_legacy_rm_bridge) {
		pr_err("gunyah_avf: enable_rm_probe and enable_legacy_rm_bridge are mutually exclusive\n");
		return -EINVAL;
	}

	if (enable_rm_probe) {
		ret = platform_driver_register(&gunyah_rm_driver);
		if (ret)
			return ret;
	}

	if (enable_legacy_rm_bridge) {
		ret = gunyah_rm_register_legacy_bridge();
		if (ret) {
			if (enable_rm_probe)
				platform_driver_unregister(&gunyah_rm_driver);
			return ret;
		}
	}

	return 0;
}

static void __exit gunyah_rm_driver_exit(void)
{
	gunyah_rm_unregister_legacy_bridge();
	if (enable_rm_probe)
		platform_driver_unregister(&gunyah_rm_driver);
}

module_init(gunyah_rm_driver_init);
module_exit(gunyah_rm_driver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah Resource Manager Driver");
