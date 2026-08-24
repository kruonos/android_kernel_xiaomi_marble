/*
 * Copyright (c) 2011-2020 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: wlan_hdd_sysfs_monitor_mode_channel.c
 *
 * Implementation for creating sysfs file monitor_mode_channel
 */

#include <wlan_hdd_includes.h>
#include "osif_vdev_sync.h"
#include <wlan_hdd_sysfs.h>
#include <wlan_hdd_sysfs_monitor_mode_channel.h>

static u32 mon_chan_store_stage;
static u32 mon_chan_store_ret;

#define MON_CHAN_STAGE(n) WRITE_ONCE(mon_chan_store_stage, (n))

static int mon_chan_store_stage_get(char *buf, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buf, PAGE_SIZE, "stage=%u ret=%u\n",
			 mon_chan_store_stage, mon_chan_store_ret);
}

static const struct kernel_param_ops mon_chan_store_stage_ops = {
	.get = mon_chan_store_stage_get,
};

module_param_cb(mon_chan_store_stage, &mon_chan_store_stage_ops, NULL,
		S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(mon_chan_store_stage,
		 "Monitor channel sysfs store failure stage debug");

static ssize_t
__hdd_sysfs_monitor_mode_channel_store(struct net_device *net_dev,
				       char const *buf, size_t count)
{
	struct hdd_adapter *adapter = netdev_priv(net_dev);
	char buf_local[MAX_SYSFS_USER_COMMAND_SIZE_LENGTH + 1];
	struct hdd_context *hdd_ctx;
	char *sptr, *token;
	uint32_t val1, val2;
	int ret;

	MON_CHAN_STAGE(9);

	if (hdd_validate_adapter(adapter)) {
		pr_err("mon_chan: adapter invalid\n");
		MON_CHAN_STAGE(2);
		return -EINVAL;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret != 0) {
		pr_err("mon_chan: ctx invalid ret %d\n", ret);
		MON_CHAN_STAGE(3);
		return ret;
	}

	if (!wlan_hdd_validate_modules_state(hdd_ctx)) {
		pr_err("mon_chan: modules state invalid\n");
		MON_CHAN_STAGE(4);
		return -EINVAL;
	}

	ret = hdd_sysfs_validate_and_copy_buf(buf_local, sizeof(buf_local),
					      buf, count);

	if (ret) {
		pr_err("mon_chan: copy buf invalid ret %d\n", ret);
		MON_CHAN_STAGE(5);
		return ret;
	}

	sptr = buf_local;
	hdd_debug("set_mon_chan: count %zu buf_local:(%s) net_devname %s",
		  count, buf_local, net_dev->name);

	/* Get val1 */
	token = strsep(&sptr, " ");
	if (!token) {
		MON_CHAN_STAGE(6);
		return -EINVAL;
	}
	if (kstrtou32(token, 0, &val1)) {
		pr_err("mon_chan: bad val1\n");
		MON_CHAN_STAGE(6);
		return -EINVAL;
	}

	/* Get val2 */
	token = strsep(&sptr, " ");
	if (!token) {
		MON_CHAN_STAGE(7);
		return -EINVAL;
	}
	if (kstrtou32(token, 0, &val2)) {
		pr_err("mon_chan: bad val2\n");
		MON_CHAN_STAGE(7);
		return -EINVAL;
	}

	if (val1 > 256)
		ret = wlan_hdd_set_mon_chan(adapter, val1, val2);
	else
		ret = wlan_hdd_set_mon_chan(adapter,
					    wlan_reg_legacy_chan_to_freq(
							hdd_ctx->pdev, val1),
					    val2);

	WRITE_ONCE(mon_chan_store_ret, ret);
	pr_err("mon_chan: set_mon_chan ret %d (count %zu)\n", ret, count);
	MON_CHAN_STAGE(ret ? 10 : 11);

	return count;
}

static ssize_t
hdd_sysfs_monitor_mode_channel_store(struct device *dev,
				     struct device_attribute *attr,
				     char const *buf, size_t count)
{
	struct net_device *net_dev = container_of(dev, struct net_device, dev);
	struct osif_vdev_sync *vdev_sync;
	ssize_t errno_size;

	MON_CHAN_STAGE(1);

	errno_size = osif_vdev_sync_op_start(net_dev, &vdev_sync);
	if (errno_size) {
		pr_err("mon_chan: vdev sync start failed %zd\n", errno_size);
		MON_CHAN_STAGE(8);
		return errno_size;
	}

	errno_size = __hdd_sysfs_monitor_mode_channel_store(net_dev,
							    buf, count);

	osif_vdev_sync_op_stop(vdev_sync);

	return errno_size;
}

static DEVICE_ATTR(monitor_mode_channel, 0220,
		   NULL, hdd_sysfs_monitor_mode_channel_store);

int hdd_sysfs_monitor_mode_channel_create(struct hdd_adapter *adapter)
{
	int error;

	error = device_create_file(&adapter->dev->dev,
				   &dev_attr_monitor_mode_channel);
	if (error)
		hdd_err("could not create monitor_mode_channel sysfs file");

	return error;
}

void hdd_sysfs_monitor_mode_channel_destroy(struct hdd_adapter *adapter)
{
	device_remove_file(&adapter->dev->dev, &dev_attr_monitor_mode_channel);
}
