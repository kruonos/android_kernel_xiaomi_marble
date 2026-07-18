// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2019-2021 NXP
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

#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include "nfc_common.h"

#define NCI_RF_MGMT_GID			(0x01)
#define NCI_RF_GET_ROUTING_OID		(0x02)
#define NCI_RF_GET_ROUTING_CMD_GID	(NCI_MSG_CMD | NCI_RF_MGMT_GID)
#define NCI_RF_GET_ROUTING_RSP_GID	(NCI_MSG_RSP | NCI_RF_MGMT_GID)
#define NCI_RF_GET_ROUTING_NTF_GID	(NCI_MSG_NTF | NCI_RF_MGMT_GID)

#define NCI_ROUTING_TYPE_TECH		(0x00)
#define NCI_ROUTING_TYPE_PROTOCOL	(0x01)
#define NCI_ROUTING_TYPE_AID		(0x02)
#define NCI_ROUTING_TYPE_SYSTEM_CODE	(0x03)
#define NCI_ROUTING_QUALIFIER_TYPE_MASK	(0x0F)
#define NCI_ROUTING_MORE_MAX		(0x01)
#define NFC_ROUTING_TYPE_AID		(0x01)
#define NFC_ROUTING_TYPE_PROTOCOL	(0x02)
#define NFC_ROUTING_TYPE_TECH		(0x03)
#define NFC_ROUTING_MAX_NTF_FRAMES	(32)

int nfc_parse_dt(struct device *dev, struct platform_configs *nfc_configs,
		 uint8_t interface)
{
	int ret;
	struct device_node *np = dev->of_node;
	struct platform_gpio *nfc_gpio = &nfc_configs->gpio;
	struct platform_ldo *ldo = &nfc_configs->ldo;

	if (!np) {
		pr_err("nfc of_node NULL\n");
		return -EINVAL;
	}

	nfc_gpio->irq = -EINVAL;
	nfc_gpio->dwl_req = -EINVAL;
	nfc_gpio->ven = -EINVAL;
	nfc_gpio->clkreq = -EINVAL;

	/* required for i2c based chips only */
	if (interface == PLATFORM_IF_I2C) {
		nfc_gpio->irq = of_get_named_gpio(np, DTS_IRQ_GPIO_STR, 0);
		if ((!gpio_is_valid(nfc_gpio->irq))) {
			pr_err("nfc irq gpio invalid %d\n", nfc_gpio->irq);
			return -EINVAL;
		}
		pr_info("%s: irq %d\n", __func__, nfc_gpio->irq);
	}
	nfc_gpio->ven = of_get_named_gpio(np, DTS_VEN_GPIO_STR, 0);
	if ((!gpio_is_valid(nfc_gpio->ven))) {
		pr_err("nfc ven gpio invalid %d\n", nfc_gpio->ven);
		return -EINVAL;
	}

	nfc_gpio->dwl_req = of_get_named_gpio(np, DTS_FWDN_GPIO_STR, 0);

	/* not returning failure for dwl gpio as it is optional for sn220 */
	if ((!gpio_is_valid(nfc_gpio->dwl_req)))
		pr_warn("nfc dwl_req gpio invalid %d\n", nfc_gpio->dwl_req);

	nfc_gpio->clkreq = of_get_named_gpio(np, DTS_CLKREQ_GPIO_STR, 0);
	if (!gpio_is_valid(nfc_gpio->clkreq)) {
		dev_err(dev, "clkreq gpio invalid %d\n", nfc_gpio->dwl_req);
		return -EINVAL;
	}

	pr_info("%s: ven %d, dwl req %d, clkreq %d\n", __func__,
		nfc_gpio->ven, nfc_gpio->dwl_req, nfc_gpio->clkreq);

	// optional property
	ret = of_property_read_u32_array(np, NFC_LDO_VOL_DT_NAME,
			(u32 *) ldo->vdd_levels,
			ARRAY_SIZE(ldo->vdd_levels));
	if (ret) {
		dev_err(dev, "error reading NFC VDDIO min and max value\n");
		// set default as per datasheet
		ldo->vdd_levels[0] = NFC_VDDIO_MIN;
		ldo->vdd_levels[1] = NFC_VDDIO_MAX;
	}

	// optional property
	ret = of_property_read_u32(np, NFC_LDO_CUR_DT_NAME, &ldo->max_current);
	if (ret) {
		dev_err(dev, "error reading NFC current value\n");
		// set default as per datasheet
		ldo->max_current = NFC_CURRENT_MAX;
	}

	return 0;
}

/**
 * nfc_ldo_vote()
 * @nfc_dev: NFC device containing regulator handle
 *
 * LDO voting based on voltage and current entries in DT
 *
 * Return: 0 on success and -ve on failure
 */
int nfc_ldo_vote(struct nfc_dev *nfc_dev)
{
	int ret;

	ret =  regulator_set_voltage(nfc_dev->reg,
			nfc_dev->configs.ldo.vdd_levels[0],
			nfc_dev->configs.ldo.vdd_levels[1]);
	if (ret < 0) {
		pr_err("%s: set voltage failed\n", __func__);
		return ret;
	}

	/* pass expected current from NFC in uA */
	ret = regulator_set_load(nfc_dev->reg, nfc_dev->configs.ldo.max_current);
	if (ret < 0) {
		pr_err("%s: set load failed\n", __func__);
		return ret;
	}

	ret = regulator_enable(nfc_dev->reg);
	if (ret < 0)
		pr_err("%s: regulator_enable failed\n", __func__);
	else
		nfc_dev->is_vreg_enabled = true;
	return ret;
}

/**
 * nfc_ldo_config()
 * @dev: device instance to read DT entry
 * @nfc_dev: NFC device containing regulator handle
 *
 * Configure LDO if entry is present in DT file otherwise
 * return with success as it's optional
 *
 * Return: 0 on success and -ve on failure
 */
int nfc_ldo_config(struct device *dev, struct nfc_dev *nfc_dev)
{
	int ret;

	if (of_get_property(dev->of_node, NFC_LDO_SUPPLY_NAME, NULL)) {
		// Get the regulator handle
		nfc_dev->reg = regulator_get(dev, NFC_LDO_SUPPLY_DT_NAME);
		if (IS_ERR(nfc_dev->reg)) {
			ret = PTR_ERR(nfc_dev->reg);
			nfc_dev->reg = NULL;
			pr_err("%s: regulator_get failed, ret = %d\n",
				__func__, ret);
			return ret;
		}
	} else {
		nfc_dev->reg = NULL;
		pr_err("%s: regulator entry not present\n", __func__);
		// return success as it's optional to configure LDO
		return 0;
	}

	// LDO config supported by platform DT
	ret = nfc_ldo_vote(nfc_dev);
	if (ret < 0) {
		pr_err("%s: LDO voting failed, ret = %d\n", __func__, ret);
		regulator_put(nfc_dev->reg);
	}
	return ret;
}

/**
 * nfc_ldo_unvote()
 * @nfc_dev: NFC device containing regulator handle
 *
 * set voltage and load to zero and disable regulator
 *
 * Return: 0 on success and -ve on failure
 */
int nfc_ldo_unvote(struct nfc_dev *nfc_dev)
{
	int ret;

	if (!nfc_dev->is_vreg_enabled) {
		pr_err("%s: regulator already disabled\n", __func__);
		return -EINVAL;
	}

	ret = regulator_disable(nfc_dev->reg);
	if (ret < 0) {
		pr_err("%s: regulator_disable failed\n", __func__);
		return ret;
	}
	nfc_dev->is_vreg_enabled = false;

	ret =  regulator_set_voltage(nfc_dev->reg, 0, NFC_VDDIO_MAX);
	if (ret < 0) {
		pr_err("%s: set voltage failed\n", __func__);
		return ret;
	}

	ret = regulator_set_load(nfc_dev->reg, 0);
	if (ret < 0)
		pr_err("%s: set load failed\n", __func__);
	return ret;
}

void set_valid_gpio(int gpio, int value)
{
	if (gpio_is_valid(gpio)) {
		pr_debug("%s gpio %d value %d\n", __func__, gpio, value);
		gpio_set_value(gpio, value);
		/* hardware dependent delay */
		usleep_range(NFC_GPIO_SET_WAIT_TIME_USEC,
			     NFC_GPIO_SET_WAIT_TIME_USEC + 100);
	}
}

int get_valid_gpio(int gpio)
{
	int value = -EINVAL;

	if (gpio_is_valid(gpio)) {
		value = gpio_get_value(gpio);
		pr_debug("%s gpio %d value %d\n", __func__, gpio, value);
	}
	return value;
}

void gpio_set_ven(struct nfc_dev *nfc_dev, int value)
{
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;

	if (gpio_get_value(nfc_gpio->ven) != value) {
		pr_debug("%s: value %d\n", __func__, value);

		gpio_set_value(nfc_gpio->ven, value);
		/* hardware dependent delay */
		if(value == 0)
		{
			usleep_range(2*NFC_GPIO_SET_WAIT_TIME_USEC,
			     2*NFC_GPIO_SET_WAIT_TIME_USEC + 100);
		} else {
			usleep_range(NFC_GPIO_SET_WAIT_TIME_USEC,
			     NFC_GPIO_SET_WAIT_TIME_USEC + 100);
		}
	}
}

int configure_gpio(unsigned int gpio, int flag)
{
	int ret;

	pr_debug("%s: nfc gpio [%d] flag [%01x]\n", __func__, gpio, flag);

	if (gpio_is_valid(gpio)) {
		ret = gpio_request(gpio, "nfc_gpio");
		if (ret) {
			pr_err("%s: unable to request nfc gpio [%d]\n",
			       __func__, gpio);
			return ret;
		}
		/* set direction and value for output pin */
		if (flag & GPIO_OUTPUT) {
			ret = gpio_direction_output(gpio, (GPIO_HIGH & flag));
			pr_debug("nfc o/p gpio %d level %d\n", gpio, gpio_get_value(gpio));
		} else {
			ret = gpio_direction_input(gpio);
			pr_debug("nfc i/p gpio %d\n", gpio);
		}

		if (ret) {
			pr_err
			    ("%s: unable to set direction for nfc gpio [%d]\n",
			     __func__, gpio);
			gpio_free(gpio);
			return ret;
		}
		// Consider value as control for input IRQ pin
		if (flag & GPIO_IRQ) {
			ret = gpio_to_irq(gpio);
			if (ret < 0) {
				pr_err("%s: unable to set irq for nfc gpio [%d]\n",
				     __func__, gpio);
				gpio_free(gpio);
				return ret;
			}
			pr_debug
			    ("%s: gpio_to_irq successful [%d]\n",
			     __func__, gpio);
			return ret;
		}
	} else {
		pr_err("%s: invalid gpio\n", __func__);
		ret = -EINVAL;
	}
	return ret;
}

void nfc_misc_unregister(struct nfc_dev *nfc_dev, int count)
{
	pr_debug("%s: entry\n", __func__);

	kfree(nfc_dev->kbuf);
	device_destroy(nfc_dev->nfc_class, nfc_dev->devno);
	cdev_del(&nfc_dev->c_dev);
	class_destroy(nfc_dev->nfc_class);
	unregister_chrdev_region(nfc_dev->devno, count);
	if (nfc_dev->ipcl)
		ipc_log_context_destroy(nfc_dev->ipcl);
}

int nfc_misc_register(struct nfc_dev *nfc_dev,
		      const struct file_operations *nfc_fops, int count,
		      char *devname, char *classname)
{
	int ret = 0;

	ret = alloc_chrdev_region(&nfc_dev->devno, 0, count, devname);
	if (ret < 0) {
		pr_err("%s: failed to alloc chrdev region ret %d\n",
			__func__, ret);
		return ret;
	}
	nfc_dev->nfc_class = class_create(THIS_MODULE, classname);
	if (IS_ERR(nfc_dev->nfc_class)) {
		ret = PTR_ERR(nfc_dev->nfc_class);
		pr_err("%s: failed to register device class ret %d\n",
			__func__, ret);
		unregister_chrdev_region(nfc_dev->devno, count);
		return ret;
	}
	cdev_init(&nfc_dev->c_dev, nfc_fops);
	ret = cdev_add(&nfc_dev->c_dev, nfc_dev->devno, count);
	if (ret < 0) {
		pr_err("%s: failed to add cdev ret %d\n", __func__, ret);
		class_destroy(nfc_dev->nfc_class);
		unregister_chrdev_region(nfc_dev->devno, count);
		return ret;
	}
	nfc_dev->nfc_device = device_create(nfc_dev->nfc_class, NULL,
					    nfc_dev->devno, nfc_dev, devname);
	if (IS_ERR(nfc_dev->nfc_device)) {
		ret = PTR_ERR(nfc_dev->nfc_device);
		pr_err("%s: failed to create the device ret %d\n",
			__func__, ret);
		cdev_del(&nfc_dev->c_dev);
		class_destroy(nfc_dev->nfc_class);
		unregister_chrdev_region(nfc_dev->devno, count);
		return ret;
	}

	nfc_dev->ipcl = ipc_log_context_create(NUM_OF_IPC_LOG_PAGES,
						dev_name(nfc_dev->nfc_device), 0);

	nfc_dev->kbuflen = MAX_BUFFER_SIZE;
	nfc_dev->kbuf = kzalloc(MAX_BUFFER_SIZE, GFP_KERNEL | GFP_DMA);
	if (!nfc_dev->kbuf) {
		nfc_misc_unregister(nfc_dev, count);
		return -ENOMEM;
	}

	nfc_dev->cold_reset.rsp_pending = false;
	nfc_dev->cold_reset.is_nfc_enabled = false;
	nfc_dev->cold_reset.is_crp_en = false;
	nfc_dev->cold_reset.last_src_ese_prot = ESE_COLD_RESET_ORIGIN_NONE;
	nfc_dev->rf_field = false;
	nfc_dev->ese_state = NFC_ESE_STATE_OFF;

	init_waitqueue_head(&nfc_dev->cold_reset.read_wq);

	return 0;
}

/*
 * Power management of the eSE
 * eSE and NFCC both are powered using VEN gpio,
 * VEN HIGH - eSE and NFCC both are powered on
 * VEN LOW - eSE and NFCC both are power down
 */
int nfc_ese_pwr(struct nfc_dev *nfc_dev, unsigned long arg)
{
	int ret = 0;
	pr_info("%s : enter, arg=%lu\n", __func__, arg);
	if (arg == ESE_POWER_ON) {
		/*
		 * Let's store the NFC VEN pin state
		 * will check stored value in case of eSE power off request,
		 * to find out if NFC MW also sent request to set VEN HIGH
		 * VEN state will remain HIGH if NFC is enabled otherwise
		 * it will be set as LOW
		 */
		nfc_dev->nfc_ven_enabled = gpio_get_value(nfc_dev->configs.gpio.ven);
		if (!nfc_dev->nfc_ven_enabled) {
			pr_debug("eSE HAL service setting ven HIGH\n");
			gpio_set_ven(nfc_dev, 1);
		} else {
			pr_debug("ven already HIGH\n");
		}
		nfc_dev->is_ese_session_active = true;
		nfc_dev->ese_state = NFC_ESE_STATE_ON;
	} else if (arg == ESE_POWER_OFF) {
		if (!nfc_dev->nfc_ven_enabled) {
			pr_debug("NFC not enabled, disabling ven\n");
			gpio_set_ven(nfc_dev, 0);
		} else {
			pr_debug("keep ven high as NFC is enabled\n");
		}
		nfc_dev->is_ese_session_active = false;
		nfc_dev->ese_state = NFC_ESE_STATE_OFF;
	} else if (arg == ESE_POWER_STATE) {
		/* get VEN gpio state for eSE, as eSE also enabled through same GPIO */
		ret = gpio_get_value(nfc_dev->configs.gpio.ven);
	} else {
		pr_err("%s bad arg %lu\n", __func__, arg);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

/*
 * nfc_ioctl_power_states() - power control
 * @nfc_dev:    nfc device data structure
 * @arg:    mode that we want to move to
 *
 * Device power control. Depending on the arg value, device moves to
 * different states, refer common.h for args
 *
 * Return: -ENOIOCTLCMD if arg is not supported, 0 if Success(or no issue)
 * and error ret code otherwise
 */
static int nfc_ioctl_power_states(struct nfc_dev *nfc_dev, unsigned long arg)
{
	int ret = 0;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;
	pr_info("%s : enter, arg=%lu \n", __func__, arg);
	if (arg == NFC_POWER_OFF || arg == NFC_POWER_ON ||
	    arg == NFC_FW_DWL_VEN_TOGGLE || arg == NFC_FW_DWL_HIGH ||
	    arg == NFC_VEN_FORCED_HARD_RESET || arg == NFC_FW_DWL_LOW)
		nfc_i2c_reset_rx_reassembly(nfc_dev);

	if (arg == NFC_POWER_OFF) {
		/*
		 * We are attempting a hardware reset so let us disable
		 * interrupts to avoid spurious notifications to upper
		 * layers.
		 */
		nfc_dev->nfc_disable_intr(nfc_dev);
		set_valid_gpio(nfc_gpio->dwl_req, 0);
		gpio_set_ven(nfc_dev, 0);
		nfc_dev->nfc_ven_enabled = false;
		nfc_dev->rf_field = false;

	} else if (arg == NFC_POWER_ON) {
		nfc_dev->nfc_enable_intr(nfc_dev);
		set_valid_gpio(nfc_gpio->dwl_req, 0);

		gpio_set_ven(nfc_dev, 1);
		nfc_dev->nfc_ven_enabled = true;

	} else if (arg == NFC_FW_DWL_VEN_TOGGLE) {
		/*
		 * We are switching to download Mode, toggle the enable pin
		 * in order to set the NFCC in the new mode
		 */
		nfc_dev->nfc_disable_intr(nfc_dev);
		set_valid_gpio(nfc_gpio->dwl_req, 1);
		nfc_dev->nfc_state = NFC_STATE_FW_DWL;
		gpio_set_ven(nfc_dev, 0);
		gpio_set_ven(nfc_dev, 1);
		nfc_dev->nfc_enable_intr(nfc_dev);
	} else if (arg == NFC_FW_DWL_HIGH) {
		/*
		 * Setting firmware download gpio to HIGH
		 * before FW download start
		 */
		pr_info("set fw gpio high\n");
		set_valid_gpio(nfc_gpio->dwl_req, 1);
		nfc_dev->nfc_state = NFC_STATE_FW_DWL;

	} else if (arg == NFC_VEN_FORCED_HARD_RESET) {
		nfc_dev->nfc_disable_intr(nfc_dev);
		gpio_set_ven(nfc_dev, 0);
		gpio_set_ven(nfc_dev, 1);
		nfc_dev->nfc_enable_intr(nfc_dev);
		pr_info("%s VEN forced reset done\n", __func__);

	} else if (arg == NFC_FW_DWL_LOW) {
		/*
		 * Setting firmware download gpio to LOW
		 * FW download finished
		 */
		pr_info("set fw gpio LOW\n");
		set_valid_gpio(nfc_gpio->dwl_req, 0);
		nfc_dev->nfc_state = NFC_STATE_NCI;

	} else if (arg == NFC_ENABLE) {
		/*
		 * Setting flag true when NFC is enabled
		 */
		nfc_dev->cold_reset.is_nfc_enabled = true;
	} else if (arg == NFC_DISABLE) {
		/*
		 * Setting flag true when NFC is disabled
		 */
		nfc_dev->cold_reset.is_nfc_enabled = false;
	}  else {
		pr_err("%s bad arg %lu\n", __func__, arg);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

/*
 * Inside nfc_ioctl_nfcc_info
 *
 * @brief   nfc_ioctl_nfcc_info
 *
 * Check the NFC Chipset and firmware version details
 */
unsigned int nfc_ioctl_nfcc_info(struct file *filp, unsigned long arg)
{
	unsigned int r = 0;
	struct nfc_dev *nfc_dev = filp->private_data;

	r = nfc_dev->nqx_info.i;
	pr_debug("nfc : %s r = 0x%x\n", __func__, r);

	return r;
}

static int nfc_diag_parse_timeout(__s32 timeout_ms, int *timeout)
{
	if (timeout_ms < -1)
		return -EINVAL;

	if (timeout_ms == -1)
		*timeout = 0;
	else if (timeout_ms == 0)
		*timeout = NCI_CMD_RSP_TIMEOUT;
	else
		*timeout = timeout_ms;

	return 0;
}

static int nfc_diag_trim_nci_frame(const __u8 *rsp, __u32 read_len,
					  __u32 rsp_max, __u32 *rsp_len)
{
	__u32 frame_len;

	if (read_len < NCI_HDR_LEN)
		return -EIO;

	frame_len = NCI_HDR_LEN + rsp[NCI_PAYLOAD_LEN_IDX];
	if (frame_len > read_len || frame_len > rsp_max)
		return -EMSGSIZE;

	*rsp_len = frame_len;
	return 0;
}

/* Raw NCI diagnostic passthrough for internal RF/NCI development tooling. */
static int nfc_diag_nci_xfer(struct nfc_dev *nfc_dev, const __u8 *cmd,
				     __u32 cmd_len, __u8 *rsp, __u32 *rsp_len,
				     int timeout)
{
	int ret;

	if (!cmd || !rsp || !rsp_len || !cmd_len || !*rsp_len)
		return -EINVAL;

	ret = validate_nfc_state_nci(nfc_dev);
	if (ret)
		return ret;

	if (!mutex_trylock(&nfc_dev->read_mutex))
		return -EBUSY;

	if (!mutex_trylock(&nfc_dev->write_mutex)) {
		mutex_unlock(&nfc_dev->read_mutex);
		return -EBUSY;
	}

	dev_dbg(nfc_dev->nfc_device,
		"debug raw nci xfer cmd_len %u rsp_max %u timeout %d\n",
		cmd_len, *rsp_len, timeout);

	ret = nfc_dev->nfc_write(nfc_dev, (const char *)cmd, cmd_len, NO_RETRY);
	if (ret != (int)cmd_len) {
		if (ret >= 0)
			ret = -EIO;
		goto out;
	}

	ret = nfc_dev->nfc_read(nfc_dev, (char *)rsp, *rsp_len, timeout);
	if (ret == 0)
		ret = -ETIMEDOUT;
	if (ret < 0)
		goto out;

	ret = nfc_diag_trim_nci_frame(rsp, ret, *rsp_len, rsp_len);
	if (ret)
		goto out;

	ret = 0;

out:
	mutex_unlock(&nfc_dev->write_mutex);
	mutex_unlock(&nfc_dev->read_mutex);
	return ret;
}

static int nfc_diag_acquire(struct file *pfile)
{
	struct nfc_dev *nfc_dev = pfile->private_data;
	int ret = 0;

	mutex_lock(&nfc_dev->dev_ref_mutex);
	if (nfc_dev->diag_owner == pfile)
		goto out;
	if (nfc_dev->diag_owner || nfc_dev->dev_ref_count != 1) {
		ret = -EBUSY;
		goto out;
	}

	nfc_dev->diag_owner = pfile;
out:
	mutex_unlock(&nfc_dev->dev_ref_mutex);
	return ret;
}

static int nfc_diag_release(struct file *pfile)
{
	struct nfc_dev *nfc_dev = pfile->private_data;
	int ret = 0;

	mutex_lock(&nfc_dev->dev_ref_mutex);
	if (!nfc_dev->diag_owner)
		goto out;
	if (nfc_dev->diag_owner != pfile) {
		ret = -EPERM;
		goto out;
	}

	nfc_dev->diag_owner = NULL;
out:
	mutex_unlock(&nfc_dev->dev_ref_mutex);
	return ret;
}

static int nfc_send_raw_nci_ioctl(struct nfc_dev *nfc_dev, unsigned long arg)
{
	int ret;
	int timeout;
	__u32 rsp_len;
	struct nfc_raw_nci_arg raw_arg;

	if (!arg)
		return -EINVAL;

	if (copy_from_user(&raw_arg, (void __user *)arg, sizeof(raw_arg)))
		return -EFAULT;

	if (raw_arg.cmd_len < NCI_HDR_LEN ||
	    raw_arg.cmd_len > sizeof(raw_arg.cmd))
		return -EINVAL;
	if (raw_arg.rsp_len > sizeof(raw_arg.rsp))
		return -EINVAL;

	rsp_len = raw_arg.rsp_len ? raw_arg.rsp_len : sizeof(raw_arg.rsp);
	if (rsp_len < NCI_HDR_LEN)
		return -EINVAL;

	ret = nfc_diag_parse_timeout(raw_arg.timeout_ms, &timeout);
	if (ret)
		return ret;

	memset(raw_arg.rsp, 0x00, sizeof(raw_arg.rsp));
	ret = nfc_diag_nci_xfer(nfc_dev, raw_arg.cmd, raw_arg.cmd_len,
				 raw_arg.rsp, &rsp_len, timeout);
	if (ret)
		return ret;

	raw_arg.rsp_len = rsp_len;
	if (copy_to_user((void __user *)arg, &raw_arg, sizeof(raw_arg)))
		return -EFAULT;

	return 0;
}

static void nfc_routing_add_entry(struct nfc_routing_info *info,
				  __u8 qualifier, const __u8 *data, __u8 len)
{
	__u8 aid_len;
	__u8 nci_type = qualifier & NCI_ROUTING_QUALIFIER_TYPE_MASK;
	struct nfc_routing_entry *entry;

	if (len < 2 || info->num_entries >= NFC_MAX_ROUTING_ENTRIES)
		return;

	entry = &info->entries[info->num_entries];
	memset(entry, 0x00, sizeof(*entry));
	entry->destination = data[0];

	switch (nci_type) {
	case NCI_ROUTING_TYPE_AID:
		entry->type = NFC_ROUTING_TYPE_AID;
		aid_len = min_t(__u8, len - 2, sizeof(entry->aid));
		memcpy(entry->aid, &data[2], aid_len);
		entry->aid_len = aid_len;
		break;
	case NCI_ROUTING_TYPE_PROTOCOL:
		if (len != 3)
			return;
		entry->type = NFC_ROUTING_TYPE_PROTOCOL;
		entry->protocol = data[2];
		break;
	case NCI_ROUTING_TYPE_TECH:
		if (len != 3)
			return;
		entry->type = NFC_ROUTING_TYPE_TECH;
		entry->tech = data[2];
		break;
	case NCI_ROUTING_TYPE_SYSTEM_CODE:
		/* The v1 diagnostic ABI has no system-code field. */
		return;
	default:
		return;
	}

	info->num_entries++;
}

static int nfc_parse_routing_ntf(const __u8 *rsp, __u32 rsp_len,
				 struct nfc_routing_info *info, bool *more)
{
	__u8 count;
	__u8 qualifier;
	__u8 len;
	__u8 i;
	__u32 payload_len;
	__u32 pos = 2;
	const __u8 *payload;

	if (!more || rsp_len < (NCI_HDR_LEN + 2) ||
	    rsp[0] != NCI_RF_GET_ROUTING_NTF_GID ||
	    rsp[1] != NCI_RF_GET_ROUTING_OID)
		return -EPROTO;

	payload_len = rsp[NCI_PAYLOAD_LEN_IDX];
	if (payload_len > rsp_len - NCI_HDR_LEN || payload_len < 2)
		return -EPROTO;

	payload = &rsp[NCI_PAYLOAD_IDX];
	if (payload[0] > NCI_ROUTING_MORE_MAX)
		return -EPROTO;
	*more = payload[0] != 0;
	count = payload[1];

	for (i = 0; i < count; i++) {
		if (pos + 2 > payload_len)
			return -EPROTO;
		qualifier = payload[pos++];
		len = payload[pos++];
		if (len > payload_len - pos)
			return -EPROTO;
		nfc_routing_add_entry(info, qualifier, &payload[pos], len);
		pos += len;
	}

	return pos == payload_len ? 0 : -EPROTO;
}

static int nfc_parse_routing_rsp(const __u8 *rsp, __u32 rsp_len,
				 struct nfc_routing_info *info)
{
	__u32 payload_len;
	const __u8 *payload;

	info->status = 0xFF;

	if (rsp_len <= NCI_HDR_LEN || rsp[0] != NCI_RF_GET_ROUTING_RSP_GID ||
	    rsp[1] != NCI_RF_GET_ROUTING_OID)
		return -EPROTO;

	payload_len = min_t(__u32, rsp[NCI_PAYLOAD_LEN_IDX],
				    rsp_len - NCI_HDR_LEN);
	if (!payload_len)
		return -EPROTO;

	payload = &rsp[NCI_HDR_LEN];
	info->status = payload[0];
	return 0;
}

static void nfc_routing_store_raw(struct nfc_routing_info *info,
				  const __u8 *frame, __u32 frame_len)
{
	__u32 available = sizeof(info->raw_rsp) - info->raw_rsp_len;
	__u32 copy_len = min(frame_len, available);

	if (!copy_len)
		return;
	memcpy(&info->raw_rsp[info->raw_rsp_len], frame, copy_len);
	info->raw_rsp_len += copy_len;
}

static int nfc_get_routing_xfer(struct nfc_dev *nfc_dev, const __u8 *cmd,
				__u32 cmd_len, int timeout,
				struct nfc_routing_info *info)
{
	__u8 rsp[NFC_RAW_NCI_MAX_LEN];
	__u32 rsp_len;
	bool more;
	int frame_count;
	int ret;

	ret = validate_nfc_state_nci(nfc_dev);
	if (ret)
		return ret;
	if (!mutex_trylock(&nfc_dev->read_mutex))
		return -EBUSY;
	if (!mutex_trylock(&nfc_dev->write_mutex)) {
		mutex_unlock(&nfc_dev->read_mutex);
		return -EBUSY;
	}

	ret = nfc_dev->nfc_write(nfc_dev, (const char *)cmd, cmd_len, NO_RETRY);
	if (ret != (int)cmd_len) {
		if (ret >= 0)
			ret = -EIO;
		goto out;
	}

	rsp_len = sizeof(rsp);
	ret = nfc_dev->nfc_read(nfc_dev, (char *)rsp, rsp_len, timeout);
	if (!ret) {
		ret = -ETIMEDOUT;
		goto out;
	}
	if (ret < 0)
		goto out;
	rsp_len = ret;
	ret = nfc_diag_trim_nci_frame(rsp, rsp_len, sizeof(rsp), &rsp_len);
	if (ret)
		goto out;
	nfc_routing_store_raw(info, rsp, rsp_len);
	ret = nfc_parse_routing_rsp(rsp, rsp_len, info);
	if (ret || info->status)
		goto out;

	more = true;
	for (frame_count = 0; more && frame_count < NFC_ROUTING_MAX_NTF_FRAMES;
	     frame_count++) {
		rsp_len = sizeof(rsp);
		ret = nfc_dev->nfc_read(nfc_dev, (char *)rsp, rsp_len, timeout);
		if (!ret) {
			ret = -ETIMEDOUT;
			goto out;
		}
		if (ret < 0)
			goto out;
		rsp_len = ret;
		ret = nfc_diag_trim_nci_frame(rsp, rsp_len, sizeof(rsp), &rsp_len);
		if (ret)
			goto out;
		nfc_routing_store_raw(info, rsp, rsp_len);
		ret = nfc_parse_routing_ntf(rsp, rsp_len, info, &more);
		if (ret)
			goto out;
	}

	if (more)
		ret = -EOVERFLOW;
	else
		dev_dbg(nfc_dev->nfc_device,
			"RF_GET_ROUTING parsed %u entries from %d notifications\n",
			info->num_entries, frame_count);

out:
	mutex_unlock(&nfc_dev->write_mutex);
	mutex_unlock(&nfc_dev->read_mutex);
	return ret;
}

static int nfc_get_routing_ioctl(struct nfc_dev *nfc_dev, unsigned long arg)
{
	int ret;
	int timeout;
	__s32 timeout_ms;
	__u8 cmd[NCI_HDR_LEN] = {
		NCI_RF_GET_ROUTING_CMD_GID,
		NCI_RF_GET_ROUTING_OID,
		0x00,
	};
	struct nfc_routing_info info;

	if (!arg)
		return -EINVAL;

	if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
		return -EFAULT;

	ret = nfc_diag_parse_timeout(info.timeout_ms, &timeout);
	if (ret)
		return ret;
	timeout_ms = info.timeout_ms;

	memset(&info, 0x00, sizeof(info));
	info.timeout_ms = timeout_ms;
	info.status = 0xFF;

	ret = nfc_get_routing_xfer(nfc_dev, cmd, sizeof(cmd), timeout, &info);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

/** @brief   IOCTL function  to be used to set or get data from upper layer.
 *
 *  @param   pfile  fil node for opened device.
 *  @cmd     IOCTL type from upper layer.
 *  @arg     IOCTL arg from upper layer.
 *
 *  @return 0 on success, error code for failures.
 */
long nfc_dev_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct nfc_dev *nfc_dev = pfile->private_data;

	if (!nfc_dev)
		return -ENODEV;

	pr_debug("%s cmd = %x arg = %zx\n", __func__, cmd, arg);

	switch (cmd) {
	case NFC_SET_PWR:
		ret = nfc_ioctl_power_states(nfc_dev, arg);
		break;
	case ESE_SET_PWR:
		ret = nfc_ese_pwr(nfc_dev, arg);
		break;
	case ESE_GET_PWR:
		ret = nfc_ese_pwr(nfc_dev, ESE_POWER_STATE);
		break;
	case NFCC_GET_INFO:
		ret = nfc_ioctl_nfcc_info(pfile, arg);
		break;
	case NFC_GET_PLATFORM_TYPE:
		ret = nfc_dev->interface;
		break;
	case ESE_COLD_RESET: {
		enum nfc_ese_diag_state prev_ese_state = nfc_dev->ese_state;

		pr_debug("nfc ese cold reset ioctl\n");
		nfc_dev->ese_state = NFC_ESE_STATE_RESET;
		ret = ese_cold_reset_ioctl(nfc_dev, arg);
		nfc_dev->ese_state = prev_ese_state;
		break;
	}
	case NFC_GET_IRQ_STATE:
		ret = gpio_get_value(nfc_dev->configs.gpio.irq);
		break;
	case NFC_SEND_RAW_NCI:
		ret = nfc_diag_acquire(pfile);
		if (!ret)
			ret = nfc_send_raw_nci_ioctl(nfc_dev, arg);
		break;
	case NFC_GET_ROUTING:
		ret = nfc_diag_acquire(pfile);
		if (!ret)
			ret = nfc_get_routing_ioctl(nfc_dev, arg);
		break;
	case NFC_DIAG_ACQUIRE:
		ret = nfc_diag_acquire(pfile);
		break;
	case NFC_DIAG_RELEASE:
		ret = nfc_diag_release(pfile);
		break;
	case QTI_NFC_TRACE_GET_INFO:
	case QTI_NFC_TRACE_READ_RECORD:
	case QTI_NFC_TRACE_CLEAR:
	case QTI_NFC_TRACE_SET_CAPTURE:
	case QTI_NFC_TRACE_SET_MAX_LEN:
	case QTI_NFC_TRACE_SET_DMESG:
		ret = qti_nfc_trace_ioctl(cmd, arg);
		break;
	default:
		pr_err("%s Unsupported ioctl cmd 0x%x, arg %lu\n",
						__func__, cmd, arg);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

int nfc_dev_open(struct inode *inode, struct file *filp)
{
	struct nfc_dev *nfc_dev = NULL;
	nfc_dev = container_of(inode->i_cdev, struct nfc_dev, c_dev);

	if (!nfc_dev)
		return -ENODEV;

	pr_debug("%s: %d, %d\n", __func__, imajor(inode), iminor(inode));
	mutex_lock(&nfc_dev->dev_ref_mutex);
	if (nfc_dev->diag_owner) {
		mutex_unlock(&nfc_dev->dev_ref_mutex);
		return -EBUSY;
	}

	/* Set flag to block freezer fake signal if not set already.
	 * Without this Signal being set, Driver is trying to do a read
	 * which is causing the delay in moving to Hibernate Mode.
	 */
	if (!(current->flags & PF_NOFREEZE)) {
		current->flags |= PF_NOFREEZE;
		pr_debug("%s: current->flags 0x%x.\n", __func__, current->flags);
	}

	filp->private_data = nfc_dev;

	if (nfc_dev->dev_ref_count == 0) {
		set_valid_gpio(nfc_dev->configs.gpio.dwl_req, 0);
		nfc_dev->nfc_enable_intr(nfc_dev);
	}
	nfc_dev->dev_ref_count = nfc_dev->dev_ref_count + 1;

	mutex_unlock(&nfc_dev->dev_ref_mutex);

	return 0;
}

int nfc_dev_flush(struct file *pfile, fl_owner_t id)
{
	struct nfc_dev *nfc_dev = pfile->private_data;

	if (!nfc_dev)
		return -ENODEV;
	/*
	 * release blocked user thread waiting for pending read during close
	 */
	if (!mutex_trylock(&nfc_dev->read_mutex)) {
		nfc_dev->release_read = true;
		nfc_dev->nfc_disable_intr(nfc_dev);
		wake_up(&nfc_dev->read_wq);
		pr_debug("%s: waiting for release of blocked read\n", __func__);
		mutex_lock(&nfc_dev->read_mutex);
		nfc_dev->release_read = false;
	} else {
		pr_debug("%s: read thread already released\n", __func__);
	}
	nfc_i2c_reset_rx_reassembly(nfc_dev);
	mutex_unlock(&nfc_dev->read_mutex);
	return 0;
}

int nfc_dev_close(struct inode *inode, struct file *filp)
{
	struct nfc_dev *nfc_dev = NULL;
	nfc_dev = container_of(inode->i_cdev, struct nfc_dev, c_dev);

	if (!nfc_dev)
		return -ENODEV;

	pr_debug("%s: %d, %d\n", __func__, imajor(inode), iminor(inode));

	/* unset the flag to restore to previous state */
	if (current->flags & PF_NOFREEZE) {
		current->flags &= ~PF_NOFREEZE;
		pr_debug("%s: current->flags 0x%x.\n", __func__, current->flags);
	}

	mutex_lock(&nfc_dev->dev_ref_mutex);
	if (nfc_dev->diag_owner == filp)
		nfc_dev->diag_owner = NULL;

	if (nfc_dev->dev_ref_count == 1) {
		nfc_dev->nfc_disable_intr(nfc_dev);
		set_valid_gpio(nfc_dev->configs.gpio.dwl_req, 0);
	}

	if (nfc_dev->dev_ref_count > 0)
		nfc_dev->dev_ref_count = nfc_dev->dev_ref_count - 1;

	filp->private_data = NULL;

	mutex_unlock(&nfc_dev->dev_ref_mutex);

	return 0;
}

int is_nfc_data_available_for_read(struct nfc_dev *nfc_dev)
{
	int ret;

	nfc_dev->nfc_enable_intr(nfc_dev);

	ret = wait_event_interruptible_timeout(nfc_dev->read_wq,
			!nfc_dev->i2c_dev.irq_enabled,
			msecs_to_jiffies(MAX_IRQ_WAIT_TIME));
	return ret;
}

/**
 * get_nfcc_chip_type_dl() - get chip type in fw download command;
 * @nfc_dev:    nfc device data structure
 *
 * Perform get version command and determine chip
 * type from response.
 *
 * @Return:  enum chip_types value
 *
 */
static enum chip_types get_nfcc_chip_type_dl(struct nfc_dev *nfc_dev)
{
	int ret = 0;
	uint8_t *cmd = nfc_dev->write_kbuf;
	uint8_t *rsp = nfc_dev->read_kbuf;
	enum chip_types chip_type = CHIP_UNKNOWN;

	*cmd++ = DL_CMD;
	*cmd++ = DL_GET_VERSION_CMD_PAYLOAD_LEN;
	*cmd++ = DL_GET_VERSION_CMD_ID;
	*cmd++ = DL_PAYLOAD_BYTE_ZERO;
	*cmd++ = DL_PAYLOAD_BYTE_ZERO;
	*cmd++ = DL_PAYLOAD_BYTE_ZERO;
	*cmd++ = DL_GET_VERSION_CMD_CRC_1;
	*cmd++ = DL_GET_VERSION_CMD_CRC_2;

	pr_debug("%s:Sending GET_VERSION cmd of size = %d\n", __func__, DL_GET_VERSION_CMD_LEN);
	ret = nfc_dev->nfc_write(nfc_dev, nfc_dev->write_kbuf, DL_GET_VERSION_CMD_LEN,
									MAX_RETRY_COUNT);
	if (ret <= 0) {
		pr_err("%s: - nfc get version cmd error ret %d\n", __func__, ret);
		goto err;
	}
	memset(rsp, 0x00, DL_GET_VERSION_RSP_LEN_2);
	pr_debug("%s:Reading response of GET_VERSION cmd\n", __func__);
	ret = nfc_dev->nfc_read(nfc_dev, rsp, DL_GET_VERSION_RSP_LEN_2, NCI_CMD_RSP_TIMEOUT);
	if (ret <= 0) {
		pr_err("%s: - nfc get version rsp error ret %d\n", __func__, ret);
		goto err;
	}
	if (rsp[0] == FW_MSG_CMD_RSP && ret >= DL_GET_VERSION_RSP_LEN_2) {

		nfc_dev->fw_major_version = rsp[FW_MAJOR_VER_OFFSET];

		if (rsp[FW_ROM_CODE_VER_OFFSET] == SN1XX_ROM_VER &&
			rsp[FW_MAJOR_VER_OFFSET] == SN1xx_MAJOR_VER)
			chip_type = CHIP_SN1XX;
		else if (rsp[FW_ROM_CODE_VER_OFFSET] == SN220_ROM_VER &&
			rsp[FW_MAJOR_VER_OFFSET] == SN220_MAJOR_VER)
			chip_type = CHIP_SN220;

		pr_debug("%s:NFC Chip Type 0x%02x Rom Version 0x%02x FW Minor 0x%02x Major 0x%02x\n",
			__func__, rsp[GET_VERSION_RSP_CHIP_TYPE_OFFSET],
					rsp[FW_ROM_CODE_VER_OFFSET],
					rsp[GET_VERSION_RSP_MINOR_VERSION_OFFSET],
					rsp[FW_MAJOR_VER_OFFSET]);

		nfc_dev->nqx_info.info.chip_type = rsp[GET_VERSION_RSP_CHIP_TYPE_OFFSET];
		nfc_dev->nqx_info.info.rom_version = rsp[FW_ROM_CODE_VER_OFFSET];
		nfc_dev->nqx_info.info.fw_minor = rsp[GET_VERSION_RSP_MINOR_VERSION_OFFSET];
		nfc_dev->nqx_info.info.fw_major = rsp[FW_MAJOR_VER_OFFSET];
	}
err:
	return chip_type;
}

/**
 * get_nfcc_session_state_dl() - gets the session state
 * @nfc_dev:    nfc device data structure
 *
 * Performs get session command and determine
 * the nfcc state based on session status.
 *
 * @Return     nfcc state based on session status.
 *             NFC_STATE_FW_TEARED if sessionis not closed
 *             NFC_STATE_FW_DWL if session closed
 *             NFC_STATE_UNKNOWN in error cases.
 */
enum nfc_state_flags get_nfcc_session_state_dl(struct nfc_dev *nfc_dev)
{
	int ret = 0;
	uint8_t *cmd = nfc_dev->write_kbuf;
	uint8_t *rsp = nfc_dev->read_kbuf;
	enum nfc_state_flags nfc_state = NFC_STATE_UNKNOWN;

	*cmd++ = DL_CMD;
	*cmd++ = DL_GET_SESSION_STATE_CMD_PAYLOAD_LEN;
	*cmd++ = DL_GET_SESSION_CMD_ID;
	*cmd++ = DL_PAYLOAD_BYTE_ZERO;
	*cmd++ = DL_PAYLOAD_BYTE_ZERO;
	*cmd++ = DL_PAYLOAD_BYTE_ZERO;
	*cmd++ = DL_GET_SESSION_CMD_CRC_1;
	*cmd++ = DL_GET_SESSION_CMD_CRC_2;

	pr_debug("%s:Sending GET_SESSION_STATE cmd of size = %d\n", __func__,
						DL_GET_SESSION_STATE_CMD_LEN);
	ret = nfc_dev->nfc_write(nfc_dev, nfc_dev->write_kbuf, DL_GET_SESSION_STATE_CMD_LEN,
						MAX_RETRY_COUNT);
	if (ret <= 0) {
		pr_err("%s: - nfc get session cmd error ret %d\n", __func__, ret);
		goto err;
	}
	memset(rsp, 0x00, DL_GET_SESSION_STATE_RSP_LEN);
	pr_debug("%s:Reading response of GET_SESSION_STATE cmd\n", __func__);
	ret = nfc_dev->nfc_read(nfc_dev, rsp, DL_GET_SESSION_STATE_RSP_LEN, NCI_CMD_RSP_TIMEOUT);
	if (ret <= 0) {
		pr_err("%s: - nfc get session rsp error ret %d\n", __func__, ret);
		goto err;
	}
	if (rsp[0] != FW_MSG_CMD_RSP) {
		pr_err("%s: - nfc invalid get session state rsp\n", __func__);
		goto err;
	}
	pr_debug("Response bytes are %02x%02x%02x%02x%02x%02x%02x%02x\n",
		rsp[0], rsp[1], rsp[2], rsp[3], rsp[4], rsp[5], rsp[6], rsp[7]);
	/*verify fw in non-teared state */
	if (rsp[GET_SESSION_STS_OFF] != NFCC_SESSION_STS_CLOSED) {
		pr_err("%s NFCC booted in FW teared state\n", __func__);
		nfc_state = NFC_STATE_FW_TEARED;
	} else {
		pr_info("%s NFCC booted in FW DN mode\n", __func__);
		nfc_state = NFC_STATE_FW_DWL;
	}
err:
	return nfc_state;
}

/**
 * get_nfcc_chip_type() - get nfcc chip type in nci mode.
 * @nfc_dev:   nfc device data structure.
 *
 * Function to perform nci core reset and extract
 * chip type from the response.
 *
 * @Return:  enum chip_types value
 *
 */
static enum chip_types get_nfcc_chip_type(struct nfc_dev *nfc_dev)
{
	int ret = 0;
	uint8_t major_version = 0;
	uint8_t rom_version = 0;
	uint8_t *cmd = nfc_dev->write_kbuf;
	uint8_t *rsp = nfc_dev->read_kbuf;
	enum chip_types chip_type = CHIP_UNKNOWN;

	*cmd++ = NCI_MSG_CMD;
	*cmd++ = NCI_CORE_RESET_CMD_OID;
	*cmd++ = NCI_CORE_RESET_CMD_PAYLOAD_LEN;
	*cmd++ = NCI_CORE_RESET_KEEP_CONFIG;

	pr_debug("%s:Sending NCI Core Reset cmd of size = %d\n", __func__, NCI_RESET_CMD_LEN);
	ret = nfc_dev->nfc_write(nfc_dev, nfc_dev->write_kbuf, NCI_RESET_CMD_LEN, NO_RETRY);
	if (ret <= 0) {
		pr_err("%s: - nfc nci core reset cmd error ret %d\n", __func__, ret);
		goto err;
	}

	/* to flush out debug NTF this delay is required */
	usleep_range(NCI_RESET_RESP_READ_DELAY, NCI_RESET_RESP_READ_DELAY + 100);
	nfc_dev->nfc_enable_intr(nfc_dev);

	memset(rsp, 0x00, NCI_RESET_RSP_LEN);
	pr_debug("%s:Reading NCI Core Reset rsp\n", __func__);
	ret = nfc_dev->nfc_read(nfc_dev, rsp, NCI_RESET_RSP_LEN, NCI_CMD_RSP_TIMEOUT);
	if (ret <= 0) {
		pr_err("%s: - nfc nci core reset rsp error ret %d\n", __func__, ret);
		goto err_disable_intr;
	}

	pr_debug(" %s: nci core reset response 0x%02x%02x%02x%02x\n",
		__func__, rsp[0], rsp[1], rsp[2], rsp[3]);
	if (rsp[0] != NCI_MSG_RSP) {
		/* reset response failed response*/
		pr_err("%s invalid nci core reset response\n", __func__);
		goto err_disable_intr;
	}

	memset(rsp, 0x00, NCI_RESET_NTF_LEN);
	/* read nci rest response ntf */
	ret = nfc_dev->nfc_read(nfc_dev, rsp, NCI_RESET_NTF_LEN, NCI_CMD_RSP_TIMEOUT);
	if (ret <= 0) {
		pr_err("%s - nfc nci rest rsp ntf error status %d\n", __func__, ret);
		goto err_disable_intr;
	}

	if (ret < NCI_HDR_LEN || rsp[0] != NCI_MSG_NTF ||
	    rsp[NCI_PAYLOAD_LEN_IDX] < NFC_CHIP_TYPE_OFF ||
	    NCI_HDR_LEN + rsp[NCI_PAYLOAD_LEN_IDX] > ret) {
		pr_err("%s invalid nci core reset notification\n", __func__);
		goto err_disable_intr;
	}

	if (rsp[0] == NCI_MSG_NTF) {
		/* read version info from NCI Reset Notification */
		rom_version = rsp[NCI_HDR_LEN + rsp[NCI_PAYLOAD_LEN_IDX] - 3];
		major_version = rsp[NCI_HDR_LEN + rsp[NCI_PAYLOAD_LEN_IDX] - 2];
		/* determine chip type based on version info */
		if (rom_version == SN1XX_ROM_VER && major_version == SN1xx_MAJOR_VER)
			chip_type = CHIP_SN1XX;
		else if (rom_version == SN220_ROM_VER && major_version == SN220_MAJOR_VER)
			chip_type = CHIP_SN220;
		pr_info(" %s:NCI  Core Reset ntf 0x%02x%02x%02x%02x\n",
			__func__, rsp[0], rsp[1], rsp[2], rsp[3]);

		nfc_dev->nqx_info.info.chip_type = rsp[NCI_HDR_LEN + rsp[NCI_PAYLOAD_LEN_IDX] -
									NFC_CHIP_TYPE_OFF];
		nfc_dev->nqx_info.info.rom_version = rom_version;
		nfc_dev->nqx_info.info.fw_major = major_version;
		nfc_dev->nqx_info.info.fw_minor = rsp[NCI_HDR_LEN + rsp[NCI_PAYLOAD_LEN_IDX] -
									NFC_FW_MINOR_OFF];
	}
err_disable_intr:
	nfc_dev->nfc_disable_intr(nfc_dev);
err:
	return chip_type;
}

/**
 * validate_download_gpio() - validate download gpio.
 * @nfc_dev: nfc_dev device data structure.
 * @chip_type: chip type of the platform.
 *
 * Validates dwnld gpio should configured for supported and
 * should not be configured for unsupported platform.
 *
 * @Return:  true if gpio validation successful ortherwise
 *           false if validation fails.
 */
static bool validate_download_gpio(struct nfc_dev *nfc_dev, enum chip_types chip_type)
{
	bool status = false;
	struct platform_gpio *nfc_gpio;

	if (nfc_dev == NULL) {
		pr_err("%s nfc devices structure is null\n", __func__);
		return status;
	}
	nfc_gpio = &nfc_dev->configs.gpio;
	if (chip_type == CHIP_SN1XX) {
		/* gpio should be configured for SN1xx */
		status = gpio_is_valid(nfc_gpio->dwl_req);
	} else if (chip_type == CHIP_SN220) {
		/* gpio should not be configured for SN220 */
		set_valid_gpio(nfc_gpio->dwl_req, 0);
		gpio_free(nfc_gpio->dwl_req);
		nfc_gpio->dwl_req = -EINVAL;
		status = true;
	}
	return status;
}

/* Check for availability of NFC controller hardware */
int nfcc_hw_check(struct nfc_dev *nfc_dev)
{
	int ret = 0;
	enum nfc_state_flags nfc_state = NFC_STATE_UNKNOWN;
	enum chip_types chip_type = CHIP_UNKNOWN;
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;

	/*get fw version in nci mode*/
	gpio_set_ven(nfc_dev, 1);
	gpio_set_ven(nfc_dev, 0);
	gpio_set_ven(nfc_dev, 1);
	chip_type = get_nfcc_chip_type(nfc_dev);

	/*get fw version in fw dwl mode*/
	if (chip_type == CHIP_UNKNOWN) {
		nfc_dev->nfc_enable_intr(nfc_dev);
		/*Chip is unknown, initially assume with fw dwl pin enabled*/
		set_valid_gpio(nfc_gpio->dwl_req, 1);
		gpio_set_ven(nfc_dev, 0);
		gpio_set_ven(nfc_dev, 1);
		chip_type = get_nfcc_chip_type_dl(nfc_dev);
		/*get the state of nfcc normal/teared in fw dwl mode*/
	} else {
		nfc_state = NFC_STATE_NCI;
	}

	/*validate gpio config required as per the chip*/
	if (!validate_download_gpio(nfc_dev, chip_type)) {
		pr_info("%s gpio validation fail\n", __func__);
		ret = -ENXIO;
		goto err;
	}

	/*check whether the NFCC is in FW DN or Teared state*/
	if (nfc_state != NFC_STATE_NCI)
		nfc_state = get_nfcc_session_state_dl(nfc_dev);

	/*nfcc state specific operations */
	switch (nfc_state) {
	case NFC_STATE_FW_TEARED:
		pr_warn("%s: - NFCC FW Teared State\n", __func__);
	case NFC_STATE_FW_DWL:
	case NFC_STATE_NCI:
		break;
	case NFC_STATE_UNKNOWN:
	default:
		ret = -ENXIO;
		pr_err("%s: - NFCC HW not available\n", __func__);
		goto err;
	}
	nfc_dev->nfc_state = nfc_state;
err:
	nfc_dev->nfc_disable_intr(nfc_dev);
	set_valid_gpio(nfc_gpio->dwl_req, 0);
	gpio_set_ven(nfc_dev, 0);
	gpio_set_ven(nfc_dev, 1);
	nfc_dev->nfc_ven_enabled = true;
	return ret;
}

int validate_nfc_state_nci(struct nfc_dev *nfc_dev)
{
	struct platform_gpio *nfc_gpio = &nfc_dev->configs.gpio;

	if (!gpio_get_value(nfc_gpio->ven)) {
		pr_err("VEN LOW - NFCC powered off\n");
		return -ENODEV;
	}
	if (get_valid_gpio(nfc_gpio->dwl_req) == 1) {
		pr_err("FW download in-progress\n");
		return -EBUSY;
	}
	if (nfc_dev->nfc_state != NFC_STATE_NCI) {
		pr_err("FW download state\n");
		return -EBUSY;
	}
	return 0;
}
