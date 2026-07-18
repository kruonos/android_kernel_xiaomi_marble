// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2013-2021 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ******************************************************************************/
/*
 * Copyright (C) 2010 Trusted Logic S.A.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "nfc_common.h"

#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>

#define NFC_TRACE_DEFAULT_MAX_LEN 64
#define NFC_TRACE_RECORD_DATA_LEN 256
#define NFC_TRACE_RECORD_COUNT 512
#define NFC_TRACE_PROC_NAME "qti_nfc_trace"
#define NFC_TRACE_DEV_NAME "qti_nfc_trace"
#define NCI_RF_MGMT_GID 0x01
#define NCI_RF_FIELD_INFO_NTF_GID (NCI_MSG_NTF | NCI_RF_MGMT_GID)
#define NCI_RF_FIELD_INFO_NTF_OID 0x07
#define NCI_MT_MASK 0xE0
#define NCI_DATA_HDR_LEN 2

struct nfc_i2c_trace_record {
	u64 seq;
	u64 ts_ns;
	pid_t pid;
	char comm[TASK_COMM_LEN];
	char dir[16];
	int ret;
	size_t len;
	size_t dump_len;
	bool truncated;
	u8 data[NFC_TRACE_RECORD_DATA_LEN];
};

static DEFINE_MUTEX(nfc_i2c_trace_lock);
static struct nfc_i2c_trace_record nfc_i2c_trace_records[NFC_TRACE_RECORD_COUNT];
static u64 nfc_i2c_trace_next_seq;
static u64 nfc_i2c_trace_dropped;
static unsigned int nfc_i2c_trace_start;
static unsigned int nfc_i2c_trace_count;
static struct proc_dir_entry *nfc_i2c_trace_proc;
static struct miscdevice nfc_i2c_trace_miscdev;
static bool nfc_i2c_trace_misc_registered;

static bool nfc_i2c_trace_capture = true;
module_param_named(trace_capture, nfc_i2c_trace_capture, bool, 0644);
MODULE_PARM_DESC(trace_capture,
	"Capture raw QTI NFC I2C/NCI frames in /proc/qti_nfc_trace (default: enabled)");

static bool nfc_i2c_trace;
module_param_named(trace, nfc_i2c_trace, bool, 0644);
MODULE_PARM_DESC(trace,
	"Also trace raw QTI NFC I2C/NCI frames to dmesg (default: disabled)");

static uint nfc_i2c_trace_max_len = NFC_TRACE_DEFAULT_MAX_LEN;
module_param_named(trace_max_len, nfc_i2c_trace_max_len, uint, 0644);
MODULE_PARM_DESC(trace_max_len,
	"Maximum bytes per NFC I2C/NCI frame to dump when trace=1");

/* Debug-only RF diagnostics exposed for internal NFC antenna/NCI testing. */
static ssize_t rf_field_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct nfc_dev *nfc_dev = i2c_get_clientdata(to_i2c_client(dev));

	if (!nfc_dev)
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%u\n", nfc_dev->rf_field ? 1 : 0);
}

static DEVICE_ATTR(rf_field, 0444, rf_field_show, NULL);

static const char *nfc_i2c_ese_state_name(enum nfc_ese_diag_state state)
{
	switch (state) {
	case NFC_ESE_STATE_ON:
		return "on";
	case NFC_ESE_STATE_RESET:
		return "reset";
	case NFC_ESE_STATE_OFF:
	default:
		return "off";
	}
}

static ssize_t ese_state_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct nfc_dev *nfc_dev = i2c_get_clientdata(to_i2c_client(dev));

	if (!nfc_dev)
		return sysfs_emit(buf, "off\n");

	return sysfs_emit(buf, "%s\n",
		nfc_i2c_ese_state_name(nfc_dev->ese_state));
}

static DEVICE_ATTR(ese_state, 0444, ese_state_show, NULL);

static int nfc_i2c_create_diag_sysfs(struct i2c_client *client,
				     struct nfc_dev *nfc_dev)
{
	int ret;

	ret = device_create_file(&client->dev, &dev_attr_rf_field);
	if (ret)
		return ret;

	ret = device_create_file(&client->dev, &dev_attr_ese_state);
	if (ret) {
		device_remove_file(&client->dev, &dev_attr_rf_field);
		return ret;
	}

	nfc_dev->diag_sysfs_created = true;
	return 0;
}

static void nfc_i2c_remove_diag_sysfs(struct i2c_client *client,
				      struct nfc_dev *nfc_dev)
{
	if (!nfc_dev || !nfc_dev->diag_sysfs_created)
		return;

	device_remove_file(&client->dev, &dev_attr_ese_state);
	device_remove_file(&client->dev, &dev_attr_rf_field);
	nfc_dev->diag_sysfs_created = false;
}

static void nfc_i2c_trace_clear(void)
{
	mutex_lock(&nfc_i2c_trace_lock);
	nfc_i2c_trace_start = 0;
	nfc_i2c_trace_count = 0;
	nfc_i2c_trace_dropped = 0;
	mutex_unlock(&nfc_i2c_trace_lock);
}

static void nfc_i2c_trace_store(const char *dir, int ret, const u8 *buf,
					 size_t len, size_t dump_len)
{
	struct nfc_i2c_trace_record *record;
	unsigned int index;

	if (!nfc_i2c_trace_capture && !nfc_i2c_trace)
		return;

	dump_len = min_t(size_t, dump_len, NFC_TRACE_RECORD_DATA_LEN);

	mutex_lock(&nfc_i2c_trace_lock);
	if (nfc_i2c_trace_count < NFC_TRACE_RECORD_COUNT) {
		index = (nfc_i2c_trace_start + nfc_i2c_trace_count) %
			NFC_TRACE_RECORD_COUNT;
		nfc_i2c_trace_count++;
	} else {
		index = nfc_i2c_trace_start;
		nfc_i2c_trace_start = (nfc_i2c_trace_start + 1) %
			NFC_TRACE_RECORD_COUNT;
		nfc_i2c_trace_dropped++;
	}

	record = &nfc_i2c_trace_records[index];
	memset(record, 0, sizeof(*record));
	record->seq = nfc_i2c_trace_next_seq++;
	record->ts_ns = ktime_get_ns();
	record->pid = task_pid_nr(current);
	get_task_comm(record->comm, current);
	strscpy(record->dir, dir, sizeof(record->dir));
	record->ret = ret;
	record->len = len;
	record->dump_len = dump_len;
	record->truncated = len > dump_len;
	if (buf && dump_len)
		memcpy(record->data, buf, dump_len);
	mutex_unlock(&nfc_i2c_trace_lock);
}

static int nfc_i2c_trace_proc_show(struct seq_file *m, void *v)
{
	unsigned int i, j;

	seq_puts(m, "# qti_nfc_trace\n");
	seq_printf(m,
		"# capture=%u dmesg=%u max_len=%u record_data_max=%u records=%u/%u dropped=%llu next_seq=%llu\n",
		nfc_i2c_trace_capture, nfc_i2c_trace,
		nfc_i2c_trace_max_len, NFC_TRACE_RECORD_DATA_LEN,
		nfc_i2c_trace_count, NFC_TRACE_RECORD_COUNT, nfc_i2c_trace_dropped,
		nfc_i2c_trace_next_seq);
	seq_puts(m, "# write commands: on | off | clear | reset | max <bytes> | log <0|1>\n");
	seq_puts(m, "# fields: seq ts_ns pid comm dir ret len dump_len flags hex\n");

	mutex_lock(&nfc_i2c_trace_lock);
	for (i = 0; i < nfc_i2c_trace_count; i++) {
		const struct nfc_i2c_trace_record *record;
		unsigned int index = (nfc_i2c_trace_start + i) %
			NFC_TRACE_RECORD_COUNT;

		record = &nfc_i2c_trace_records[index];
		seq_printf(m, "%llu %llu %d %s %s %d %zu %zu %c ",
			record->seq, record->ts_ns, record->pid, record->comm,
			record->dir, record->ret, record->len, record->dump_len,
			record->truncated ? 'T' : '-');
		for (j = 0; j < record->dump_len; j++)
			seq_printf(m, "%02x", record->data[j]);
		seq_putc(m, '\n');
	}
	mutex_unlock(&nfc_i2c_trace_lock);

	return 0;
}

static int nfc_i2c_trace_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, nfc_i2c_trace_proc_show, NULL);
}

static ssize_t nfc_i2c_trace_proc_write(struct file *file,
					       const char __user *buf, size_t count,
					       loff_t *ppos)
{
	char cmd[64];
	char *arg;
	unsigned int value;
	size_t len = min_t(size_t, count, sizeof(cmd) - 1);

	if (copy_from_user(cmd, buf, len))
		return -EFAULT;

	cmd[len] = '\0';
	arg = strim(cmd);

	if (sysfs_streq(arg, "on") || sysfs_streq(arg, "1") ||
	    sysfs_streq(arg, "enable")) {
		nfc_i2c_trace_capture = true;
		return count;
	}

	if (sysfs_streq(arg, "off") || sysfs_streq(arg, "0") ||
	    sysfs_streq(arg, "disable")) {
		nfc_i2c_trace_capture = false;
		return count;
	}

	if (sysfs_streq(arg, "clear")) {
		nfc_i2c_trace_clear();
		return count;
	}

	if (sysfs_streq(arg, "reset")) {
		nfc_i2c_trace_clear();
		nfc_i2c_trace_capture = true;
		nfc_i2c_trace = false;
		nfc_i2c_trace_max_len = NFC_TRACE_DEFAULT_MAX_LEN;
		return count;
	}

	if (sscanf(arg, "max %u", &value) == 1) {
		if (!value)
			return -EINVAL;
		nfc_i2c_trace_max_len = min_t(uint, value,
						   NFC_TRACE_RECORD_DATA_LEN);
		return count;
	}

	if (sscanf(arg, "log %u", &value) == 1) {
		nfc_i2c_trace = !!value;
		return count;
	}

	return -EINVAL;
}

static const struct proc_ops nfc_i2c_trace_proc_ops = {
	.proc_open = nfc_i2c_trace_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = nfc_i2c_trace_proc_write,
};

static int nfc_i2c_trace_dev_open(struct inode *inode, struct file *file)
{
	return single_open(file, nfc_i2c_trace_proc_show, NULL);
}

static const struct file_operations nfc_i2c_trace_dev_fops = {
	.owner = THIS_MODULE,
	.open = nfc_i2c_trace_dev_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = nfc_i2c_trace_proc_write,
};

static void nfc_i2c_trace_fill_user_record(
		struct qti_nfc_trace_user_record *dst,
		const struct nfc_i2c_trace_record *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->seq = src->seq;
	dst->ts_ns = src->ts_ns;
	dst->pid = src->pid;
	dst->ret = src->ret;
	dst->len = src->len;
	dst->dump_len = src->dump_len;
	dst->truncated = src->truncated;
	strscpy(dst->comm, src->comm, sizeof(dst->comm));
	strscpy(dst->dir, src->dir, sizeof(dst->dir));
	if (src->dump_len)
		memcpy(dst->data, src->data,
		       min_t(size_t, src->dump_len, sizeof(dst->data)));
}

long qti_nfc_trace_ioctl(unsigned int cmd, unsigned long arg)
{
	struct qti_nfc_trace_info info;
	struct qti_nfc_trace_read_record req;
	unsigned int index;
	unsigned int value;
	long ret = 0;

	switch (cmd) {
	case QTI_NFC_TRACE_GET_INFO:
		memset(&info, 0, sizeof(info));
		mutex_lock(&nfc_i2c_trace_lock);
		info.record_count = nfc_i2c_trace_count;
		info.record_capacity = NFC_TRACE_RECORD_COUNT;
		info.record_data_len = NFC_TRACE_RECORD_DATA_LEN;
		info.max_len = nfc_i2c_trace_max_len;
		info.dropped = nfc_i2c_trace_dropped;
		info.next_seq = nfc_i2c_trace_next_seq;
		info.capture = nfc_i2c_trace_capture;
		info.dmesg = nfc_i2c_trace;
		mutex_unlock(&nfc_i2c_trace_lock);

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	case QTI_NFC_TRACE_READ_RECORD:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		mutex_lock(&nfc_i2c_trace_lock);
		if (req.index >= nfc_i2c_trace_count) {
			ret = -ENOENT;
		} else {
			index = (nfc_i2c_trace_start + req.index) %
				NFC_TRACE_RECORD_COUNT;
			nfc_i2c_trace_fill_user_record(&req.record,
					&nfc_i2c_trace_records[index]);
		}
		mutex_unlock(&nfc_i2c_trace_lock);
		if (ret)
			return ret;

		if (copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;
		return 0;

	case QTI_NFC_TRACE_CLEAR:
		nfc_i2c_trace_clear();
		return 0;

	case QTI_NFC_TRACE_SET_CAPTURE:
		nfc_i2c_trace_capture = !!arg;
		return 0;

	case QTI_NFC_TRACE_SET_MAX_LEN:
		value = min_t(unsigned long, arg, NFC_TRACE_RECORD_DATA_LEN);
		if (!value)
			return -EINVAL;
		nfc_i2c_trace_max_len = value;
		return 0;

	case QTI_NFC_TRACE_SET_DMESG:
		nfc_i2c_trace = !!arg;
		return 0;

	default:
		return -ENOIOCTLCMD;
	}
}

static void nfc_i2c_trace_frame(struct nfc_dev *nfc_dev, const char *dir,
					const u8 *buf, size_t len)
{
	size_t dump_len;

	if ((!nfc_i2c_trace_capture && !nfc_i2c_trace) || !buf || !len)
		return;

	dump_len = min_t(size_t, len, nfc_i2c_trace_max_len);
	dump_len = min_t(size_t, dump_len, NFC_TRACE_RECORD_DATA_LEN);
	nfc_i2c_trace_store(dir, len, buf, len, dump_len);

	if (!nfc_i2c_trace)
		return;

	NFCLOG_IPC(nfc_dev, false, "trace %s len %zu dump %zu", dir, len,
		     dump_len);
	pr_info("qti-nfc trace %s len=%zu dump=%zu%s\n", dir, len, dump_len,
		len > dump_len ? " truncated" : "");

	if (dump_len)
		print_hex_dump(KERN_INFO, "qti-nfc trace: ", DUMP_PREFIX_OFFSET,
			       16, 1, buf, dump_len, false);
}

static void nfc_i2c_trace_status(const char *dir, int ret)
{
	if (!nfc_i2c_trace_capture && !nfc_i2c_trace)
		return;

	nfc_i2c_trace_store(dir, ret, NULL, 0, 0);

	if (!nfc_i2c_trace)
		return;

	pr_info("qti-nfc trace %s ret=%d\n", dir, ret);
}

static void nfc_i2c_update_rf_field(struct nfc_dev *nfc_dev, const u8 *buf,
				    int len)
{
	bool field_on;
	u8 payload_len;

	if (!nfc_dev || !buf || len < (NCI_HDR_LEN + 1))
		return;

	payload_len = buf[NCI_PAYLOAD_LEN_IDX];
	if (buf[0] != NCI_RF_FIELD_INFO_NTF_GID ||
	    buf[1] != NCI_RF_FIELD_INFO_NTF_OID ||
	    payload_len != 1 || NCI_HDR_LEN + payload_len > len)
		return;

	field_on = !!buf[NCI_PAYLOAD_IDX];
	nfc_dev->rf_field = field_on;

	if (nfc_dev->nfc_device)
		dev_dbg(nfc_dev->nfc_device, "RF field state %u\n", field_on);
	else if (nfc_dev->i2c_dev.client)
		dev_dbg(&nfc_dev->i2c_dev.client->dev,
			"RF field state %u\n", field_on);
}

static void nfc_i2c_reset_rx_reassembly_locked(struct nfc_dev *nfc_dev)
{
	memset(&nfc_dev->rx_reassembly, 0,
	       sizeof(nfc_dev->rx_reassembly));
}

void nfc_i2c_reset_rx_reassembly(struct nfc_dev *nfc_dev)
{
	if (!nfc_dev)
		return;

	mutex_lock(&nfc_dev->rx_reassembly_mutex);
	nfc_i2c_reset_rx_reassembly_locked(nfc_dev);
	mutex_unlock(&nfc_dev->rx_reassembly_mutex);
}

static u8 nfc_i2c_nci_header_len(u8 first_byte)
{
	switch (first_byte & NCI_MT_MASK) {
	case 0x00:
		return NCI_DATA_HDR_LEN;
	case NCI_MSG_CMD:
	case NCI_MSG_RSP:
	case NCI_MSG_NTF:
		return NCI_HDR_LEN;
	default:
		return 0;
	}
}

static void nfc_i2c_process_rx_chunk(struct nfc_dev *nfc_dev,
				     const u8 *buf, size_t len)
{
	struct nfc_rx_reassembly *frame;
	size_t copy_len;
	size_t offset = 0;
	u8 payload_len;

	if (!nfc_dev || !buf || !len)
		return;

	mutex_lock(&nfc_dev->rx_reassembly_mutex);
	frame = &nfc_dev->rx_reassembly;
	if (nfc_dev->nfc_state != NFC_STATE_NCI ||
	    (gpio_is_valid(nfc_dev->configs.gpio.dwl_req) &&
	     gpio_get_value(nfc_dev->configs.gpio.dwl_req))) {
		nfc_i2c_reset_rx_reassembly_locked(nfc_dev);
		goto out;
	}

	while (offset < len) {
		if (!frame->header_len) {
			if (buf[offset] == 0xff) {
				offset++;
				continue;
			}
			frame->header_len = nfc_i2c_nci_header_len(buf[offset]);
			if (!frame->header_len) {
				pr_debug("dropping invalid NCI RX chunk at offset %zu\n",
					 offset);
				nfc_i2c_reset_rx_reassembly_locked(nfc_dev);
				break;
			}
		}

		if (frame->len < frame->header_len) {
			copy_len = min_t(size_t,
					frame->header_len - frame->len,
					len - offset);
			memcpy(&frame->data[frame->len], &buf[offset], copy_len);
			frame->len += copy_len;
			offset += copy_len;
			if (frame->len < frame->header_len)
				continue;

			payload_len = frame->data[frame->header_len - 1];
			frame->expected = frame->header_len + payload_len;
			if (frame->expected > sizeof(frame->data)) {
				pr_debug("dropping oversized NCI RX frame len %u\n",
					 frame->expected);
				nfc_i2c_reset_rx_reassembly_locked(nfc_dev);
				break;
			}
		}

		if (frame->len < frame->expected) {
			copy_len = min_t(size_t, frame->expected - frame->len,
					len - offset);
			memcpy(&frame->data[frame->len], &buf[offset], copy_len);
			frame->len += copy_len;
			offset += copy_len;
		}

		if (frame->len == frame->expected) {
			nfc_i2c_trace_frame(nfc_dev, "RX_NCI", frame->data,
					    frame->len);
			nfc_i2c_update_rf_field(nfc_dev, frame->data, frame->len);
			nfc_i2c_reset_rx_reassembly_locked(nfc_dev);
		}
	}

out:
	mutex_unlock(&nfc_dev->rx_reassembly_mutex);
}

/**
 * i2c_disable_irq()
 *
 * Check if interrupt is disabled or not
 * and disable interrupt
 *
 * Return: int
 */
int i2c_disable_irq(struct nfc_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->i2c_dev.irq_enabled_lock, flags);
	if (dev->i2c_dev.irq_enabled) {
		disable_irq_nosync(dev->i2c_dev.client->irq);
		dev->i2c_dev.irq_enabled = false;
	}
	spin_unlock_irqrestore(&dev->i2c_dev.irq_enabled_lock, flags);

	return 0;
}

/**
 * i2c_enable_irq()
 *
 * Check if interrupt is enabled or not
 * and enable interrupt
 *
 * Return: int
 */
int i2c_enable_irq(struct nfc_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->i2c_dev.irq_enabled_lock, flags);
	if (!dev->i2c_dev.irq_enabled) {
		dev->i2c_dev.irq_enabled = true;
		enable_irq(dev->i2c_dev.client->irq);
	}
	spin_unlock_irqrestore(&dev->i2c_dev.irq_enabled_lock, flags);

	return 0;
}

static irqreturn_t i2c_irq_handler(int irq, void *dev_id)
{
	struct nfc_dev *nfc_dev = dev_id;
	struct i2c_dev *i2c_dev = &nfc_dev->i2c_dev;

	if (device_may_wakeup(&i2c_dev->client->dev))
		pm_wakeup_event(&i2c_dev->client->dev, WAKEUP_SRC_TIMEOUT);

	i2c_disable_irq(nfc_dev);
	wake_up(&nfc_dev->read_wq);

	return IRQ_HANDLED;
}

int i2c_read(struct nfc_dev *nfc_dev, char *buf, size_t count, int timeout)
{
	int ret = 0;
	struct i2c_dev *i2c_dev = &nfc_dev->i2c_dev;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	uint16_t i = 0;
	uint16_t disp_len = GET_IPCLOG_MAX_PKT_LEN(count);

	pr_debug("%s : reading %zu bytes.\n", __func__, count);

	if (timeout > NCI_CMD_RSP_TIMEOUT)
		timeout = NCI_CMD_RSP_TIMEOUT;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (!gpio_get_value(nfc_gpio->irq)) {
		while (1) {
			ret = 0;
			if (!i2c_dev->irq_enabled) {
				i2c_dev->irq_enabled = true;
				enable_irq(i2c_dev->client->irq);
			}
			if (!gpio_get_value(nfc_gpio->irq)) {
				if (timeout) {
					ret = wait_event_interruptible_timeout(nfc_dev->read_wq,
						 !i2c_dev->irq_enabled, msecs_to_jiffies(timeout));

					if (ret <= 0) {
						pr_err("%s timeout/error in read\n", __func__);
						goto err;
					}
				} else {
					ret = wait_event_interruptible(nfc_dev->read_wq,
						!i2c_dev->irq_enabled);
					if (ret) {
						pr_debug("%s unexpeted wakeup of read wq, try wait again.\n", __func__);
						goto err;
					}
				}
			}
			i2c_disable_irq(nfc_dev);

			if (gpio_get_value(nfc_gpio->irq))
				break;
			if (!gpio_get_value(nfc_gpio->ven)) {
				pr_info("%s: releasing read\n", __func__);
				ret = -EIO;
				goto err;
			}
			/*
			 * NFC service wanted to close the driver so,
			 * release the calling reader thread asap.
			 *
			 * This can happen in case of nfc node close call from
			 * eSE HAL in that case the NFC HAL reader thread
			 * will again call read system call
			 */
			if (nfc_dev->release_read) {
				pr_debug("%s: releasing read\n", __func__);
				return 0;
			}
			pr_warn("%s: spurious interrupt detected\n", __func__);
		}
	}

	memset(buf, 0x00, count);
	/* Read data */
	ret = i2c_master_recv(nfc_dev->i2c_dev.client, buf, count);
	NFCLOG_IPC(nfc_dev, false, "%s of %zu bytes, ret %d", __func__, count,
								ret);
	if (ret <= 0) {
		nfc_i2c_trace_status("RX", ret);
		pr_err("%s: returned %d\n", __func__, ret);
		goto err;
	}
	nfc_i2c_trace_frame(nfc_dev, "RX", buf, ret);
	nfc_i2c_process_rx_chunk(nfc_dev, (u8 *)buf, ret);

	for (i = 0; i < disp_len; i++)
		NFCLOG_IPC(nfc_dev, false, " %02x", buf[i]);

	/* check if it's response of cold reset command
	 * NFC HAL process shouldn't receive this data as
	 * command was esepowermanager
	 */
	if (nfc_dev->cold_reset.rsp_pending && nfc_dev->cold_reset.cmd_buf
		&& (buf[0] == PROP_NCI_RSP_GID)
		&& (buf[1] == nfc_dev->cold_reset.cmd_buf[1])) {
		read_cold_reset_rsp(nfc_dev, buf);
		nfc_dev->cold_reset.rsp_pending = false;
		wake_up_interruptible(&nfc_dev->cold_reset.read_wq);
		/*
		 * NFC process doesn't know about cold reset command
		 * being sent as it was initiated by eSE process
		 * we shouldn't return any data to NFC process
		 */
		return 0;
	}

err:
	return ret;
}

int i2c_write(struct nfc_dev *nfc_dev, const char *buf, size_t count,
	      int max_retry_cnt)
{
	int ret = -EINVAL;
	int retry_cnt;
	uint16_t i = 0;
	uint16_t disp_len = GET_IPCLOG_MAX_PKT_LEN(count);

	if (count > MAX_DL_BUFFER_SIZE)
		count = MAX_DL_BUFFER_SIZE;

	pr_debug("%s : writing %zu bytes.\n", __func__, count);

	NFCLOG_IPC(nfc_dev, false, "%s sending %zu B", __func__, count);

	nfc_i2c_trace_frame(nfc_dev, "TX", buf, count);

	for (i = 0; i < disp_len; i++)
		NFCLOG_IPC(nfc_dev, false, " %02x", buf[i]);

	for (retry_cnt = 1; retry_cnt <= max_retry_cnt; retry_cnt++) {

		ret = i2c_master_send(nfc_dev->i2c_dev.client, buf, count);
		NFCLOG_IPC(nfc_dev, false, "%s ret %d", __func__, ret);
		nfc_i2c_trace_status("TX", ret);
		if (ret <= 0) {
			pr_warn("%s: write failed ret %d, Maybe in Standby Mode - Retry(%d)\n",
				__func__, ret, retry_cnt);
			usleep_range(WRITE_RETRY_WAIT_TIME_USEC,
				     WRITE_RETRY_WAIT_TIME_USEC + 100);
		} else if (ret != count) {
			pr_err("%s: failed to write %d\n", __func__, ret);
			ret = -EIO;
		} else if (ret == count)
			break;
	}
	return ret;
}

ssize_t nfc_i2c_dev_read(struct file *filp, char __user *buf,
			 size_t count, loff_t *offset)
{
	int ret = 0;
	struct nfc_dev *nfc_dev = (struct nfc_dev *)filp->private_data;

	count = min_t(size_t, count, MAX_BUFFER_SIZE);

	mutex_lock(&nfc_dev->read_mutex);
	if (filp->f_flags & O_NONBLOCK) {
		ret = i2c_master_recv(nfc_dev->i2c_dev.client, nfc_dev->read_kbuf, count);
		pr_debug("%s: NONBLOCK read ret = %d\n", __func__, ret);
		if (ret > 0) {
			nfc_i2c_trace_frame(nfc_dev, "RX_NONBLOCK",
					    nfc_dev->read_kbuf, ret);
			nfc_i2c_process_rx_chunk(nfc_dev, nfc_dev->read_kbuf,
					     ret);
		} else {
			nfc_i2c_trace_status("RX_NONBLOCK", ret);
		}
	} else {
		ret = i2c_read(nfc_dev, nfc_dev->read_kbuf, count, 0);
	}
	if (ret > 0) {
		if (copy_to_user(buf, nfc_dev->read_kbuf, ret)) {
			pr_warn("%s: failed to copy to user space\n", __func__);
			ret = -EFAULT;
		}
	}
	mutex_unlock(&nfc_dev->read_mutex);
	return ret;
}

ssize_t nfc_i2c_dev_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *offset)
{
	int ret;
	struct nfc_dev *nfc_dev = (struct nfc_dev *)filp->private_data;

	if (count > MAX_DL_BUFFER_SIZE)
		count = MAX_DL_BUFFER_SIZE;

	if (!nfc_dev)
		return -ENODEV;

	mutex_lock(&nfc_dev->write_mutex);
	if (copy_from_user(nfc_dev->write_kbuf, buf, count)) {
		pr_err("%s : failed to copy from user space\n", __func__);
		mutex_unlock(&nfc_dev->write_mutex);
		return -EFAULT;
	}
	ret = i2c_write(nfc_dev, nfc_dev->write_kbuf, count, NO_RETRY);
	mutex_unlock(&nfc_dev->write_mutex);
	return ret;
}

static const struct file_operations nfc_i2c_dev_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.read = nfc_i2c_dev_read,
	.write = nfc_i2c_dev_write,
	.open = nfc_dev_open,
	.flush = nfc_dev_flush,
	.release = nfc_dev_close,
	.unlocked_ioctl = nfc_dev_ioctl,
};

int nfc_i2c_dev_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = 0;
	struct nfc_dev *nfc_dev = NULL;
	struct i2c_dev *i2c_dev = NULL;
	struct platform_configs nfc_configs;
	struct platform_gpio *nfc_gpio = &nfc_configs.gpio;

	pr_info("%s: enter\n", __func__);

	//retrieve details of gpios from dt

	ret = nfc_parse_dt(&client->dev, &nfc_configs, PLATFORM_IF_I2C);
	if (ret) {
		pr_err("%s : failed to parse dt\n", __func__);
		goto err;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s : need I2C_FUNC_I2C\n", __func__);
		ret = -ENODEV;
		goto err;
	}
	nfc_dev = kzalloc(sizeof(struct nfc_dev), GFP_KERNEL);
	if (nfc_dev == NULL) {
		ret = -ENOMEM;
		goto err;
	}
	nfc_dev->read_kbuf = kzalloc(MAX_BUFFER_SIZE, GFP_DMA | GFP_KERNEL);
	if (!nfc_dev->read_kbuf) {
		ret = -ENOMEM;
		goto err_free_nfc_dev;
	}
	nfc_dev->write_kbuf = kzalloc(MAX_DL_BUFFER_SIZE, GFP_DMA | GFP_KERNEL);
	if (!nfc_dev->write_kbuf) {
		ret = -ENOMEM;
		goto err_free_read_kbuf;
	}
	nfc_dev->interface = PLATFORM_IF_I2C;
	nfc_dev->nfc_state = NFC_STATE_NCI;
	nfc_dev->i2c_dev.client = client;
	i2c_dev = &nfc_dev->i2c_dev;
	nfc_dev->nfc_read = i2c_read;
	nfc_dev->nfc_write = i2c_write;
	nfc_dev->nfc_enable_intr = i2c_enable_irq;
	nfc_dev->nfc_disable_intr = i2c_disable_irq;
	nfc_dev->fw_major_version = 0;
	nfc_dev->rf_field = false;
	nfc_dev->ese_state = NFC_ESE_STATE_OFF;
	ret = configure_gpio(nfc_gpio->ven, GPIO_OUTPUT);
	if (ret) {
		pr_err("%s: unable to request nfc reset gpio [%d]\n",
			__func__, nfc_gpio->ven);
		goto err_free_write_kbuf;
	}
	ret = configure_gpio(nfc_gpio->irq, GPIO_IRQ);
	if (ret <= 0) {
		pr_err("%s: unable to request nfc irq gpio [%d]\n",
			__func__, nfc_gpio->irq);
		goto err_free_ven;
	}
	client->irq = ret;
	ret = configure_gpio(nfc_gpio->dwl_req, GPIO_OUTPUT);
	if (ret) {
		pr_err("%s: unable to request nfc firm downl gpio [%d]\n",
			__func__, nfc_gpio->dwl_req);
		//not returning failure here as dwl gpio is a optional gpio for sn220
	}

	ret = configure_gpio(nfc_gpio->clkreq, GPIO_INPUT);
	if (ret) {
		pr_err("%s: unable to request nfc clkreq gpio [%d]\n",
		       __func__, nfc_gpio->clkreq);
		goto err_free_dwl_req;
	}

	/*copy the retrieved gpio details from DT */
	memcpy(&nfc_dev->configs, &nfc_configs, sizeof(struct platform_configs));

	/* init mutex and queues */
	init_waitqueue_head(&nfc_dev->read_wq);
	mutex_init(&nfc_dev->read_mutex);
	mutex_init(&nfc_dev->write_mutex);
	mutex_init(&nfc_dev->rx_reassembly_mutex);
	mutex_init(&nfc_dev->dev_ref_mutex);
	spin_lock_init(&i2c_dev->irq_enabled_lock);
	ret = nfc_misc_register(nfc_dev, &nfc_i2c_dev_fops, DEV_COUNT,
				NFC_CHAR_DEV_NAME, CLASS_NAME);
	if (ret) {
		pr_err("%s: nfc_misc_register failed\n", __func__);
		goto err_mutex_destroy;
	}
	/* interrupt initializations */
	pr_info("%s : requesting IRQ %d\n", __func__, client->irq);
	i2c_dev->irq_enabled = true;
	ret = request_irq(client->irq, i2c_irq_handler,
			  IRQF_TRIGGER_HIGH, client->name, nfc_dev);
	if (ret) {
		pr_err("%s: request_irq failed\n", __func__);
		goto err_nfc_misc_unregister;
	}
	i2c_disable_irq(nfc_dev);
	i2c_set_clientdata(client, nfc_dev);

	ret = nfc_i2c_create_diag_sysfs(client, nfc_dev);
	if (ret)
		dev_dbg(&client->dev, "diag sysfs create failed ret %d\n", ret);

	ret = nfc_ldo_config(&client->dev, nfc_dev);
	if (ret) {
		pr_err("LDO config failed\n");
		goto err_ldo_config_failed;
	}

	ret = nfcc_hw_check(nfc_dev);
	if (ret || nfc_dev->nfc_state == NFC_STATE_UNKNOWN) {
		pr_err("nfc hw check failed ret %d\n", ret);
		goto err_nfcc_hw_check;
	}

	device_init_wakeup(&client->dev, true);
	i2c_dev->irq_wake_up = false;
	nfc_dev->is_ese_session_active = false;

	pr_info("%s success\n", __func__);
	return 0;

err_nfcc_hw_check:
	if (nfc_dev->reg) {
		nfc_ldo_unvote(nfc_dev);
		regulator_put(nfc_dev->reg);
	}
err_ldo_config_failed:
	nfc_i2c_remove_diag_sysfs(client, nfc_dev);
	free_irq(client->irq, nfc_dev);
err_nfc_misc_unregister:
	nfc_misc_unregister(nfc_dev, DEV_COUNT);
err_mutex_destroy:
	mutex_destroy(&nfc_dev->dev_ref_mutex);
	mutex_destroy(&nfc_dev->rx_reassembly_mutex);
	mutex_destroy(&nfc_dev->read_mutex);
	mutex_destroy(&nfc_dev->write_mutex);
	gpio_free(nfc_gpio->clkreq);
err_free_dwl_req:
	if (gpio_is_valid(nfc_gpio->dwl_req))
		gpio_free(nfc_gpio->dwl_req);
	gpio_free(nfc_gpio->irq);
err_free_ven:
	gpio_free(nfc_gpio->ven);
err_free_write_kbuf:
	kfree(nfc_dev->write_kbuf);
err_free_read_kbuf:
	kfree(nfc_dev->read_kbuf);
err_free_nfc_dev:
	kfree(nfc_dev);
err:
	pr_err("%s: failed\n", __func__);
	return ret;
}

int nfc_i2c_dev_remove(struct i2c_client *client)
{
	int ret = 0;
	struct nfc_dev *nfc_dev = NULL;

	pr_info("%s: remove device\n", __func__);
	nfc_dev = i2c_get_clientdata(client);
	if (!nfc_dev) {
		pr_err("%s: device doesn't exist anymore\n", __func__);
		ret = -ENODEV;
		return ret;
	}

	if (nfc_dev->dev_ref_count > 0) {
		pr_err("%s: device already in use\n", __func__);
		return -EBUSY;
	}

	gpio_set_value(nfc_dev->configs.gpio.ven, 0);
	// HW dependent delay before LDO goes into LPM mode
	usleep_range(10000, 10100);
	if (nfc_dev->reg) {
		nfc_ldo_unvote(nfc_dev);
		regulator_put(nfc_dev->reg);
	}

	nfc_i2c_remove_diag_sysfs(client, nfc_dev);
	device_init_wakeup(&client->dev, false);
	free_irq(client->irq, nfc_dev);
	nfc_misc_unregister(nfc_dev, DEV_COUNT);
	mutex_destroy(&nfc_dev->dev_ref_mutex);
	mutex_destroy(&nfc_dev->rx_reassembly_mutex);
	mutex_destroy(&nfc_dev->read_mutex);
	mutex_destroy(&nfc_dev->write_mutex);

	if (gpio_is_valid(nfc_dev->configs.gpio.clkreq))
		gpio_free(nfc_dev->configs.gpio.clkreq);

	if (gpio_is_valid(nfc_dev->configs.gpio.dwl_req))
		gpio_free(nfc_dev->configs.gpio.dwl_req);

	if (gpio_is_valid(nfc_dev->configs.gpio.irq))
		gpio_free(nfc_dev->configs.gpio.irq);

	if (gpio_is_valid(nfc_dev->configs.gpio.ven))
		gpio_free(nfc_dev->configs.gpio.ven);

	kfree(nfc_dev->read_kbuf);
	kfree(nfc_dev->write_kbuf);
	kfree(nfc_dev);
	return ret;
}

int nfc_i2c_dev_suspend(struct device *device)
{
	struct i2c_client *client = to_i2c_client(device);
	struct nfc_dev *nfc_dev = i2c_get_clientdata(client);
	struct i2c_dev *i2c_dev = NULL;

	if (!nfc_dev) {
		pr_err("%s: device doesn't exist anymore\n", __func__);
		return -ENODEV;
	}

	i2c_dev = &nfc_dev->i2c_dev;

	NFCLOG_IPC(nfc_dev, false, "%s: irq_enabled = %d", __func__,
							i2c_dev->irq_enabled);

	if (device_may_wakeup(&client->dev) && i2c_dev->irq_enabled) {
		if (!enable_irq_wake(client->irq))
			i2c_dev->irq_wake_up = true;
	}
	return 0;
}

int nfc_i2c_dev_resume(struct device *device)
{
	struct i2c_client *client = to_i2c_client(device);
	struct nfc_dev *nfc_dev = i2c_get_clientdata(client);
	struct i2c_dev *i2c_dev = NULL;

	if (!nfc_dev) {
		pr_err("%s: device doesn't exist anymore\n", __func__);
		return -ENODEV;
	}

	i2c_dev = &nfc_dev->i2c_dev;

	NFCLOG_IPC(nfc_dev, false, "%s: irq_wake_up = %d", __func__,
							i2c_dev->irq_wake_up);

	if (device_may_wakeup(&client->dev) && i2c_dev->irq_wake_up) {
		if (!disable_irq_wake(client->irq))
			i2c_dev->irq_wake_up = false;
	}
	return 0;
}

static const struct i2c_device_id nfc_i2c_dev_id[] = {
	{NFC_I2C_DEV_ID, 0},
	{}
};

static const struct of_device_id nfc_i2c_dev_match_table[] = {
	{.compatible = NFC_I2C_DRV_STR,},
	{}
};

static const struct dev_pm_ops nfc_i2c_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(nfc_i2c_dev_suspend, nfc_i2c_dev_resume)
};

static struct i2c_driver nfc_i2c_dev_driver = {
	.id_table = nfc_i2c_dev_id,
	.probe = nfc_i2c_dev_probe,
	.remove = nfc_i2c_dev_remove,
	.driver = {
		.name = NFC_I2C_DRV_STR,
		.pm = &nfc_i2c_dev_pm_ops,
		.of_match_table = nfc_i2c_dev_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

MODULE_DEVICE_TABLE(of, nfc_i2c_dev_match_table);

static int __init nfc_i2c_dev_init(void)
{
	int ret = 0;
	int misc_ret;

	pr_info("loading NFC I2C driver\n");
	ret = i2c_add_driver(&nfc_i2c_dev_driver);
	if (ret != 0)
		pr_err("NFC I2C add driver error ret %d\n", ret);
	else {
		nfc_i2c_trace_miscdev.minor = MISC_DYNAMIC_MINOR;
		nfc_i2c_trace_miscdev.name = NFC_TRACE_DEV_NAME;
		nfc_i2c_trace_miscdev.fops = &nfc_i2c_trace_dev_fops;
		nfc_i2c_trace_miscdev.mode = 0600;
		misc_ret = misc_register(&nfc_i2c_trace_miscdev);
		if (misc_ret)
			pr_warn("failed to register /dev/%s ret %d\n",
				NFC_TRACE_DEV_NAME, misc_ret);
		else
			nfc_i2c_trace_misc_registered = true;

		nfc_i2c_trace_proc = proc_create(NFC_TRACE_PROC_NAME, 0600, NULL,
						       &nfc_i2c_trace_proc_ops);
		if (!nfc_i2c_trace_proc)
			pr_warn("failed to create /proc/%s\n", NFC_TRACE_PROC_NAME);
	}
	return ret;
}

module_init(nfc_i2c_dev_init);

static void __exit nfc_i2c_dev_exit(void)
{
	pr_info("Unloading NFC I2C driver\n");
	if (nfc_i2c_trace_misc_registered)
		misc_deregister(&nfc_i2c_trace_miscdev);
	if (nfc_i2c_trace_proc)
		proc_remove(nfc_i2c_trace_proc);
	i2c_del_driver(&nfc_i2c_dev_driver);
}

module_exit(nfc_i2c_dev_exit);

MODULE_DESCRIPTION("QTI NFC I2C driver");
MODULE_LICENSE("GPL v2");
