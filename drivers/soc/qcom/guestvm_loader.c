// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/completion.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/gunyah/gh_rm_drv.h>
#include <linux/cpu.h>
#include <linux/of_address.h>
#include <linux/qcom_scm.h>
#include <linux/firmware.h>
#include <linux/soc/qcom/mdt_loader.h>

#include <soc/qcom/subsystem_notif.h>
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/secure_buffer.h>

#define MAX_LEN 256
#define DEFAULT_UNISO_TIMEOUT_MS 12000
#define GUESTVM_READY_TIMEOUT_MS 30000
#define CPUSYS_AUTOSTART_DELAY_MS 10000
#define NUM_RESERVED_CPUS 2
#define GUESTVM_DIAG_STAGE_LEN 48

static bool guestvm_loader_diag = true;
module_param_named(diag, guestvm_loader_diag, bool, 0644);
MODULE_PARM_DESC(diag, "Enable verbose GuestVM PAS/PIL boot diagnostics");

static bool guestvm_cleanup_on_failure = true;
module_param_named(cleanup_on_failure, guestvm_cleanup_on_failure, bool, 0644);
MODULE_PARM_DESC(cleanup_on_failure,
		 "Try VM_RESET/VM_DEALLOCATE after a VMID was allocated but VM load failed");

static bool guestvm_cpusys_autostart_enabled = true;
module_param_named(cpusys_autostart, guestvm_cpusys_autostart_enabled, bool, 0644);
MODULE_PARM_DESC(cpusys_autostart,
		 "Start cpusys_vm from the GuestVM loader after module probe");

#define guestvm_diag(priv, fmt, ...) \
	do { \
		if (guestvm_loader_diag) \
			dev_info((priv)->dev, "diag: " fmt, ##__VA_ARGS__); \
	} while (0)

const static struct {
	enum gh_vm_names val;
	const char *str;
} conversion[] = {
	{GH_PRIMARY_VM, "pvm"},
	{GH_TRUSTED_VM, "trustedvm"},
	{GH_CPUSYS_VM, "cpusys_vm"},
	{GH_OEM_VM, "oem_vm"},
};

static struct kobj_type guestvm_kobj_type = {
	.sysfs_ops = &kobj_sysfs_ops,
};

struct gh_sec_ext_region {
	phys_addr_t ext_phys;
	ssize_t ext_size;
	u32 ext_label;
	gh_memparcel_handle_t ext_mem_handle;
};

struct guestvm_loader_private {
	struct notifier_block guestvm_nb;
	struct completion vm_start;
	struct kobject vm_loader_kobj;
	struct device *dev;
	ktime_t request_vm_start_time;
	char vm_name[MAX_LEN];
	bool vm_loaded;
	bool vmid_allocated;
	bool iso_needed;
	int pas_id;
	int vmid;
	u8 vm_status;
	u8 os_status;
	u16 app_status;
	struct timer_list guestvm_cpu_isolate_timer;
	struct completion isolation_done;
	struct work_struct unisolation_work;
	struct delayed_work cpusys_autostart_work;
	struct mutex vm_boot_lock;
	cpumask_t guestvm_isolated_cpus;
	cpumask_t guestvm_reserve_cpus;
	u32 guestvm_unisolate_timeout;
	struct gh_sec_ext_region ext_region;
	bool ext_region_supported;
	bool cpusys_autostart_scheduled;
	int cpusys_autostart_ret;
	u32 boot_seq;
	int last_ret;
	int last_error;
	char last_stage[GUESTVM_DIAG_STAGE_LEN];
	char failure_stage[GUESTVM_DIAG_STAGE_LEN];
	s64 loaded_us;
	s64 ready_us;
	s64 started_us;
	s64 booted_us;
};

static const char *guestvm_name_str(enum gh_vm_names vm_name)
{
	switch (vm_name) {
	case GH_SELF_VM:
		return "self";
	case GH_PRIMARY_VM:
		return "pvm";
	case GH_TRUSTED_VM:
		return "trustedvm";
	case GH_CPUSYS_VM:
		return "cpusys_vm";
	case GH_OEM_VM:
		return "oem_vm";
	default:
		return "unknown";
	}
}

static const char *guestvm_vm_status_str(u8 status)
{
	switch (status) {
	case GH_RM_VM_STATUS_NO_STATE:
		return "NO_STATE";
	case GH_RM_VM_STATUS_INIT:
		return "INIT";
	case GH_RM_VM_STATUS_READY:
		return "READY";
	case GH_RM_VM_STATUS_RUNNING:
		return "RUNNING";
	case GH_RM_VM_STATUS_PAUSED:
		return "PAUSED";
	case GH_RM_VM_STATUS_INIT_FAILED:
		return "INIT_FAILED";
	case GH_RM_VM_STATUS_EXITED:
		return "EXITED";
	case GH_RM_VM_STATUS_RESETTING:
		return "RESETTING";
	case GH_RM_VM_STATUS_RESET:
		return "RESET";
	default:
		return "UNKNOWN";
	}
}

static const char *guestvm_os_status_str(u8 status)
{
	switch (status) {
	case GH_RM_OS_STATUS_NONE:
		return "NONE";
	case GH_RM_OS_STATUS_EARLY_BOOT:
		return "EARLY_BOOT";
	case GH_RM_OS_STATUS_BOOT:
		return "BOOT";
	case GH_RM_OS_STATUS_INIT:
		return "INIT";
	case GH_RM_OS_STATUS_RUN:
		return "RUN";
	default:
		return "UNKNOWN";
	}
}

static void guestvm_set_stage(struct guestvm_loader_private *priv,
				      const char *stage, int ret)
{
	strlcpy(priv->last_stage, stage, sizeof(priv->last_stage));
	priv->last_ret = ret;
	if (ret && !priv->last_error) {
		priv->last_error = ret;
		strlcpy(priv->failure_stage, stage, sizeof(priv->failure_stage));
	}
}

static void guestvm_cleanup_allocated_vmid(struct guestvm_loader_private *priv,
					   const char *reason)
{
	int ret;

	guestvm_set_stage(priv, "cleanup_vm_reset", 0);
	ret = gh_rm_vm_reset(priv->vmid);
	guestvm_diag(priv, "cleanup after %s: gh_rm_vm_reset vmid=%d ret=%d\n",
		reason, priv->vmid, ret);

	guestvm_set_stage(priv, "cleanup_vm_dealloc", 0);
	ret = gh_rm_vm_dealloc_vmid(priv->vmid);
	guestvm_diag(priv,
		"cleanup after %s: gh_rm_vm_dealloc_vmid vmid=%d ret=%d\n",
		reason, priv->vmid, ret);
	if (ret)
		guestvm_set_stage(priv, "cleanup_vm_dealloc_failed", ret);
	else
		priv->vmid_allocated = false;
}

static void guestvm_isolate_cpu(struct guestvm_loader_private *priv)
{
	int cpu, ret;

	for_each_cpu_and(cpu, &priv->guestvm_reserve_cpus, cpu_online_mask) {
		ret = remove_cpu(cpu);
		if (ret) {
			pr_err("fail to offline CPU%d. ret=%d\n", cpu, ret);
			continue;
		}
		pr_info("%s: offlined cpu : %d\n", __func__, cpu);
		cpumask_set_cpu(cpu, &priv->guestvm_isolated_cpus);
	}
}

static void guestvm_unisolate_cpu(struct guestvm_loader_private *priv)
{
	int i, ret;

	for_each_cpu(i, &priv->guestvm_isolated_cpus) {
		ret = add_cpu(i);
		if (ret) {
			pr_err("fail to online CPU%d. ret=%d\n", i, ret);
			continue;
		}
		pr_info("%s: onlined cpu : %d\n", __func__, i);

		cpumask_clear_cpu(i, &priv->guestvm_isolated_cpus);
	}

	del_timer(&priv->guestvm_cpu_isolate_timer);
}

static void guestvm_unisolate_work(struct work_struct *work)
{
	struct guestvm_loader_private *priv;

	priv = container_of(work, struct guestvm_loader_private, unisolation_work);

	if (wait_for_completion_interruptible(&priv->isolation_done))
		pr_err("%s: CPU unisolation is interrupted\n", __func__);

	guestvm_unisolate_cpu(priv);
}

static void guestvm_timer_callback(struct timer_list *t)
{
	struct guestvm_loader_private *priv;

	priv = container_of(t, struct guestvm_loader_private,
			    guestvm_cpu_isolate_timer);

	pr_err("%s: expired: VM app status not set\n", __func__);
	complete(&priv->isolation_done);
}

static inline enum gh_vm_names get_gh_vm_name(const char *str)
{
	int vmid;

	for (vmid = 0; vmid < ARRAY_SIZE(conversion); ++vmid) {
		if (!strcmp(str, conversion[vmid].str))
			return conversion[vmid].val;
	}
	return GH_VM_MAX;
}

static int vm_provide_ext_region(struct guestvm_loader_private *priv)
{
	gh_vmid_t self_vmid;
	u32 src_vmlist[1];
	int src_perms[1] = { PERM_READ | PERM_WRITE | PERM_EXEC };
	int dst_vmlist[1] = { priv->vmid };
	int dst_perms[1] = { PERM_READ };
	struct gh_acl_desc *acl;
	struct gh_sgl_desc *sgl;
	int ret;

	guestvm_set_stage(priv, "ext_region_get_self_vmid", 0);
	ret = gh_rm_get_vmid(GH_PRIMARY_VM, &self_vmid);
	if (ret)
		return ret;
	src_vmlist[0] = self_vmid;
	guestvm_diag(priv,
		"ext-region begin vm=%s vmid=%d self_vmid=%u phys=%pa size=%zd label=0x%x\n",
		priv->vm_name, priv->vmid, self_vmid,
		&priv->ext_region.ext_phys, priv->ext_region.ext_size,
		priv->ext_region.ext_label);

	acl = kzalloc(offsetof(struct gh_acl_desc, acl_entries[1]), GFP_KERNEL);
	if (!acl) {
		ret = -ENOMEM;
		return ret;
	}
	sgl = kzalloc(offsetof(struct gh_sgl_desc, sgl_entries[1]), GFP_KERNEL);
	if (!sgl) {
		ret = -ENOMEM;
		goto err_sgl;
	}
	guestvm_set_stage(priv, "ext_region_hyp_assign_to_vm", 0);
	ret = hyp_assign_phys(priv->ext_region.ext_phys,
			      priv->ext_region.ext_size, src_vmlist, 1,
			      dst_vmlist, dst_perms, 1);
	guestvm_diag(priv, "ext-region hyp_assign_to_vm ret=%d\n", ret);

	if (ret) {
		dev_err(priv->dev,
			"%s: hyp_assign_phys failed for addr=%pa size=%zd err=%d\n",
			__func__, &priv->ext_region.ext_phys,
			priv->ext_region.ext_size, ret);
		goto err_hyp_assign;
	}

	acl->n_acl_entries = 1;
	acl->acl_entries[0].vmid = (u16)priv->vmid;
	acl->acl_entries[0].perms = GH_RM_ACL_R;

	sgl->n_sgl_entries = 1;
	sgl->sgl_entries[0].ipa_base = priv->ext_region.ext_phys;
	sgl->sgl_entries[0].size = priv->ext_region.ext_size;

	guestvm_set_stage(priv, "ext_region_mem_lend", 0);
	ret = gh_rm_mem_lend(GH_RM_MEM_TYPE_NORMAL, 0,
			     priv->ext_region.ext_label, acl, sgl, NULL,
			     &priv->ext_region.ext_mem_handle);
	guestvm_diag(priv,
		"ext-region mem_lend ret=%d handle=0x%x label=0x%x\n",
		ret, priv->ext_region.ext_mem_handle,
		priv->ext_region.ext_label);
	if (ret) {
		dev_err(priv->dev, "%s: Sharing memory failed %d\n",
			__func__, ret);
		/* Attempt to assign resource back to HLOS */
		hyp_assign_phys(priv->ext_region.ext_phys,
				priv->ext_region.ext_size, dst_vmlist, 1,
				src_vmlist, src_perms, 1);
	}

err_hyp_assign:
	kfree(sgl);
err_sgl:
	kfree(acl);

	return ret;
}

static void vm_reclaim_ext_region(struct guestvm_loader_private *priv)
{
	gh_vmid_t self_vmid;

	int dst_perms[1] = { PERM_READ | PERM_WRITE | PERM_EXEC };
	int src_vmlist[1];
	u32 dst_vmlist[1];
	int ret;

	guestvm_set_stage(priv, "ext_region_reclaim_get_self_vmid", 0);
	ret = gh_rm_get_vmid(GH_PRIMARY_VM, &self_vmid);
	if (ret) {
		dev_err(priv->dev, "Failed to get VMID\n");
		return;
	}
	guestvm_diag(priv,
		"ext-region reclaim begin vm=%s vmid=%d self_vmid=%u handle=0x%x phys=%pa size=%zd\n",
		priv->vm_name, priv->vmid, self_vmid,
		priv->ext_region.ext_mem_handle, &priv->ext_region.ext_phys,
		priv->ext_region.ext_size);

	src_vmlist[0] = priv->vmid;
	dst_vmlist[0] = self_vmid;

	guestvm_set_stage(priv, "ext_region_mem_reclaim", 0);
	ret = gh_rm_mem_reclaim(priv->ext_region.ext_mem_handle, 0);
	guestvm_diag(priv, "ext-region mem_reclaim ret=%d handle=0x%x\n",
		ret, priv->ext_region.ext_mem_handle);
	if (ret)
		dev_err(priv->dev, "ext_region reclaim failed\n");

	guestvm_set_stage(priv, "ext_region_hyp_assign_to_hlos", 0);
	ret = hyp_assign_phys(priv->ext_region.ext_phys,
			      priv->ext_region.ext_size, src_vmlist, 1,
			      dst_vmlist, dst_perms, 1);
	guestvm_diag(priv, "ext-region hyp_assign_to_hlos ret=%d\n", ret);
	if (ret)
		dev_err(priv->dev, "ext_region assign failed\n");
}

static int guestvm_loader_nb_handler(struct notifier_block *this,
					unsigned long cmd, void *data)
{
	struct guestvm_loader_private *priv;
	struct gh_rm_notif_vm_status_payload *vm_status_payload = data;
	u8 vm_status = vm_status_payload->vm_status;
	u8 os_status = vm_status_payload->os_status;
	u16 app_status = vm_status_payload->app_status;
	s64 delta;
	ktime_t now;
	int ret;

	priv = container_of(this, struct guestvm_loader_private, guestvm_nb);

	if (cmd != GH_RM_NOTIF_VM_STATUS)
		return NOTIFY_DONE;

	if (priv->vmid != vm_status_payload->vmid) {
		dev_warn(priv->dev, "Expected a notification from vmid = %d, but received one from vmid = %d\n",
				priv->vmid, vm_status_payload->vmid);
		return NOTIFY_DONE;
	}

	now = ktime_get();
	delta = ktime_to_us(ktime_sub(now, priv->request_vm_start_time));
	guestvm_diag(priv,
		"notif cmd=0x%lx vmid=%u vm_status=%u/%s os_status=%u/%s app_status=%u delta_us=%lld\n",
		cmd, vm_status_payload->vmid, vm_status,
		guestvm_vm_status_str(vm_status), os_status,
		guestvm_os_status_str(os_status), app_status, delta);

	/*
	 * Listen to STATUS_READY or STATUS_RUNNING notifications from RM.
	 * These notifications come from RM after PIL loading the VM images.
	 * Query GET_HYP_RESOURCES to populate other entities such as MessageQ
	 * and DBL.
	 */
	switch (vm_status) {
	case GH_RM_VM_STATUS_READY:
		priv->vm_status = GH_RM_VM_STATUS_READY;
		priv->ready_us = delta;
		guestvm_set_stage(priv, "notif_ready_populate_hyp_res", 0);
		ret = gh_rm_populate_hyp_res(vm_status_payload->vmid, priv->vm_name);
		guestvm_diag(priv,
			"populate_hyp_res vmid=%u vm=%s ret=%d\n",
			vm_status_payload->vmid, priv->vm_name, ret);
		if (ret < 0) {
			dev_err(priv->dev, "Failed to get hyp resources for vmid = %d ret = %d\n",
				vm_status_payload->vmid, ret);
			complete_all(&priv->vm_start);
			return NOTIFY_DONE;
		}
		guestvm_set_stage(priv, "notif_ready_get_vm_id_info", 0);
		ret = gh_rm_get_vm_id_info(priv->vmid);
		guestvm_diag(priv, "get_vm_id_info vmid=%d ret=%d\n",
			priv->vmid, ret);
		if (ret < 0)
			dev_err(priv->dev, "Couldn't obtain VM ID info.\n");

		complete_all(&priv->vm_start);
		break;
	case GH_RM_VM_STATUS_RUNNING:
		if (priv->vm_status != GH_RM_VM_STATUS_RUNNING) {
			priv->vm_status = GH_RM_VM_STATUS_RUNNING;
			priv->started_us = delta;
			dev_info(priv->dev, "VM(%d) started running in %lld us\n",
					vm_status_payload->vmid, delta);
		}
		break;
	case GH_RM_VM_STATUS_RESET:
		if (priv->ext_region_supported)
			vm_reclaim_ext_region(priv);
		break;
	default:
		dev_err(priv->dev, "Unknown notification receieved for vmid = %d vm_status = %d\n",
				vm_status_payload->vmid, vm_status);
	}

	if (priv->os_status != os_status) {
		if (os_status == GH_RM_OS_STATUS_BOOT) {
			priv->os_status = os_status;
			priv->booted_us = delta;
			dev_info(priv->dev, "VM(%d) booted in %lld us\n",
					    priv->vmid, delta);
		}
	}

	if (priv->app_status != app_status) {
		if (app_status == GH_RM_APP_STATUS_TUI_SERVICE_BOOT) {
			priv->app_status = app_status;
			dev_info(priv->dev, "Unisolating VM(%d) cpus after %lld us: VM app status = %d\n",
					    priv->vmid, delta, app_status);
			complete(&priv->isolation_done);
		}
	}

	return NOTIFY_DONE;
}

static int vm_load(struct guestvm_loader_private *priv)
{
	const struct firmware *fw;
	char fw_name[32];
	struct device_node *node;
	struct resource res;
	phys_addr_t phys;
	ssize_t size;
	void *virt;
	int ret;
	struct device *dev = priv->dev;

	guestvm_set_stage(priv, "vm_load_parse_ext_region", 0);
	node = of_parse_phandle(dev->of_node, "ext-region", 0);
	if (node) {
		priv->ext_region_supported = true;
		ret = of_address_to_resource(node, 0, &res);
		if (ret) {
			dev_err(dev,
				"error %d getting \"ext-region\" resource\n",
				ret);
			return -EINVAL;
		}

		priv->ext_region.ext_phys = res.start;
		priv->ext_region.ext_size = (size_t)resource_size(&res);
		ret = of_property_read_u32(dev->of_node, "ext-label",
					   &priv->ext_region.ext_label);
		if (ret) {
			dev_err(dev, "DT error getting \"ext-label\": %d\n",
				ret);
			return -EINVAL;
		}
		guestvm_diag(priv,
			"vm_load ext-region phys=%pa size=%zd label=0x%x\n",
			&priv->ext_region.ext_phys,
			priv->ext_region.ext_size,
			priv->ext_region.ext_label);
	}

	guestvm_set_stage(priv, "vm_load_parse_memory_region", 0);
	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(dev, "DT error getting \"memory-region\" property\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret) {
		dev_err(dev, "error %d getting \"memory-region\" resource\n",
			ret);
		return ret;
	}

	scnprintf(fw_name, ARRAY_SIZE(fw_name), "%s.mdt", priv->vm_name);

	ret = of_property_read_u32(dev->of_node, "qcom,pas-id", &priv->pas_id);
	if (ret) {
		dev_err(dev, "error %d getting pas-id\n", ret);
		return ret;
	}

	ret = request_firmware(&fw, fw_name, dev);
	if (ret) {
		dev_err(dev, "error %d requesting \"%s\"\n", ret, fw_name);
		return ret;
	}

	phys = res.start;
	size = (size_t)resource_size(&res);
	guestvm_diag(priv,
		"vm_load memory-region phys=%pa size=%zd firmware=%s pas_id=%d\n",
		&phys, size, fw_name, priv->pas_id);
	guestvm_set_stage(priv, "vm_load_memremap", 0);
	virt = memremap(phys, size, MEMREMAP_WC);
	if (!virt) {
		dev_err(dev, "unable to remap firmware memory\n");
		ret = -ENOMEM;
		goto release_firmware;
	}

	guestvm_diag(priv,
		"qcom_mdt_load begin firmware=%s fw_size=%zu pas_id=%d phys=%pa size=%zd\n",
		fw_name, fw->size, priv->pas_id, &phys, size);
	guestvm_set_stage(priv, "qcom_mdt_load", 0);
	ret = qcom_mdt_load(dev, fw, fw_name, priv->pas_id, virt, phys, size, NULL);
	guestvm_diag(priv, "qcom_mdt_load done firmware=%s ret=%d\n",
		fw_name, ret);
	if (ret) {
		dev_err(dev, "error %d loading \"%s\"\n", ret, fw_name);
		goto mem_unmap;
	}

	if (priv->ext_region_supported) {
		guestvm_set_stage(priv, "vm_load_provide_ext_region", 0);
		ret = vm_provide_ext_region(priv);
		if (ret) {
			dev_err(priv->dev,
				"Failed to provide memory for ext-region to vm: %s, %d\n",
				priv->vm_name, ret);
			goto mem_unmap;
		}
	}

	guestvm_diag(priv, "qcom_scm_pas_auth_and_reset begin pas_id=%d\n",
		priv->pas_id);
	guestvm_set_stage(priv, "qcom_scm_pas_auth_and_reset", 0);
	ret = qcom_scm_pas_auth_and_reset(priv->pas_id);
	guestvm_diag(priv, "qcom_scm_pas_auth_and_reset done pas_id=%d ret=%d\n",
		priv->pas_id, ret);
	if (ret)
		dev_err(dev, "error %d authenticating \"%s\"\n", ret, fw_name);
	guestvm_set_stage(priv, "vm_load_done", ret);

mem_unmap:
	memunmap(virt);
release_firmware:
	release_firmware(fw);
	return ret;
}

static void guestvm_cleanup_failed_boot(struct guestvm_loader_private *priv,
					const char *reason)
{
	int ret;

	if (!guestvm_cleanup_on_failure) {
		guestvm_diag(priv,
			"cleanup after %s skipped cleanup_on_failure=0 vmid=%d\n",
			reason, priv->vmid);
		return;
	}

	if (priv->vm_loaded) {
		guestvm_set_stage(priv, "cleanup_pas_shutdown", 0);
		ret = qcom_scm_pas_shutdown(priv->pas_id);
		guestvm_diag(priv,
			"cleanup after %s: qcom_scm_pas_shutdown pas_id=%d ret=%d\n",
			reason, priv->pas_id, ret);
		if (ret)
			guestvm_set_stage(priv, "cleanup_pas_shutdown_failed", ret);
		priv->vm_loaded = false;
	}

	if (priv->vmid_allocated)
		guestvm_cleanup_allocated_vmid(priv, reason);

	priv->vm_status = GH_RM_VM_STATUS_NO_STATE;
}

static int guestvm_loader_start_vm(struct guestvm_loader_private *priv)
{
	int ret = 0;
	ktime_t now;
	s64 delta;
	enum gh_vm_names vm_name_val;

	mutex_lock(&priv->vm_boot_lock);
	if (priv->vm_loaded || priv->vmid_allocated) {
		dev_err(priv->dev, "VM load has already been started\n");
		ret = -EBUSY;
		goto unlock;
	}

	priv->boot_seq++;
	priv->request_vm_start_time = ktime_get();
	priv->last_ret = 0;
	priv->last_error = 0;
	priv->failure_stage[0] = '\0';
	priv->loaded_us = 0;
	priv->ready_us = 0;
	priv->started_us = 0;
	priv->booted_us = 0;
	reinit_completion(&priv->vm_start);

	priv->vm_status = GH_RM_VM_STATUS_INIT;
	vm_name_val = get_gh_vm_name(priv->vm_name);
	guestvm_set_stage(priv, "gh_rm_vm_alloc_vmid", 0);
	guestvm_diag(priv,
		"boot_seq=%u begin vm=%s enum=%d/%s dt_vmid=%d iso_needed=%d\n",
		priv->boot_seq, priv->vm_name, vm_name_val,
		guestvm_name_str(vm_name_val), priv->vmid, priv->iso_needed);
	ret = gh_rm_vm_alloc_vmid(vm_name_val, &priv->vmid);
	guestvm_diag(priv, "gh_rm_vm_alloc_vmid done ret=%d vmid=%d\n",
		ret, priv->vmid);
	guestvm_set_stage(priv, "gh_rm_vm_alloc_vmid_done", ret);
	if (ret < 0) {
		dev_err(priv->dev, "Couldn't allocate VMID.\n");
		goto start_failed;
	}
	priv->vmid_allocated = true;

	guestvm_set_stage(priv, "vm_load", 0);
	ret = vm_load(priv);
	guestvm_diag(priv, "vm_load done ret=%d vmid=%d\n", ret, priv->vmid);
	if (ret) {
		dev_err(priv->dev, "vm_load failed with error %d\n", ret);
		guestvm_set_stage(priv, "vm_load_failed", ret);
		guestvm_cleanup_failed_boot(priv, "vm_load_failure");
		goto unlock;
	}
	priv->vm_loaded = true;
	now = ktime_get();
	delta = ktime_to_us(ktime_sub(now, priv->request_vm_start_time));
	priv->loaded_us = delta;
	dev_info(priv->dev, "VM(%d) loaded in %lld us\n", priv->vmid, delta);

	guestvm_set_stage(priv, "wait_for_ready", 0);
	if (!wait_for_completion_timeout(&priv->vm_start,
			msecs_to_jiffies(GUESTVM_READY_TIMEOUT_MS))) {
		ret = -ETIMEDOUT;
		dev_err(priv->dev, "VM ready notification timed out\n");
		guestvm_set_stage(priv, "wait_for_ready_timeout", ret);
		guestvm_cleanup_failed_boot(priv, "ready_timeout");
		goto unlock;
	}
	guestvm_diag(priv,
		"wait_for_ready done vmid=%d vm_status=%u/%s ready_us=%lld\n",
		priv->vmid, priv->vm_status,
		guestvm_vm_status_str(priv->vm_status), priv->ready_us);

	if (priv->iso_needed) {
		INIT_WORK(&priv->unisolation_work, guestvm_unisolate_work);
		schedule_work(&priv->unisolation_work);
		guestvm_isolate_cpu(priv);
		mod_timer(&priv->guestvm_cpu_isolate_timer, jiffies +
				msecs_to_jiffies(priv->guestvm_unisolate_timeout));
	}

	guestvm_set_stage(priv, "gh_rm_vm_start", 0);
	guestvm_diag(priv, "gh_rm_vm_start begin vmid=%d\n", priv->vmid);
	ret = gh_rm_vm_start(priv->vmid);
	guestvm_diag(priv, "gh_rm_vm_start done vmid=%d ret=%d\n", priv->vmid,
		ret);
	guestvm_set_stage(priv, "gh_rm_vm_start_done", ret);
	if (ret) {
		dev_err(priv->dev, "VM start failed for vmid = %d ret = %d\n",
			priv->vmid, ret);
		guestvm_cleanup_failed_boot(priv, "vm_start_failure");
	}
	goto unlock;

start_failed:
	priv->vm_status = GH_RM_VM_STATUS_NO_STATE;
unlock:
	mutex_unlock(&priv->vm_boot_lock);
	return ret;
}

static void guestvm_cpusys_autostart(struct work_struct *work)
{
	struct guestvm_loader_private *priv;

	priv = container_of(to_delayed_work(work),
			    struct guestvm_loader_private, cpusys_autostart_work);
	priv->cpusys_autostart_ret = guestvm_loader_start_vm(priv);
	guestvm_diag(priv, "cpusys autostart completed ret=%d\n",
		priv->cpusys_autostart_ret);
}

static ssize_t guestvm_loader_start(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct guestvm_loader_private *priv;
	bool boot;
	int ret;

	ret = kstrtobool(buf, &boot);
	if (ret)
		return -EINVAL;
	if (!boot)
		return count;

	priv = container_of(kobj, struct guestvm_loader_private,
				vm_loader_kobj);
	ret = guestvm_loader_start_vm(priv);
	return ret ? ret : count;
}

static ssize_t guestvm_loader_status(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	struct guestvm_loader_private *priv;

	priv = container_of(kobj, struct guestvm_loader_private,
				vm_loader_kobj);

	return scnprintf(buf, PAGE_SIZE,
		"vm_name=%s\n"
		"vmid=%d\n"
		"pas_id=%d\n"
		"boot_seq=%u\n"
		"vm_loaded=%d\n"
		"vmid_allocated=%d\n"
		"cpusys_autostart_scheduled=%d\n"
		"cpusys_autostart_ret=%d\n"
		"vm_status=%u/%s\n"
		"os_status=%u/%s\n"
		"app_status=%u\n"
		"iso_needed=%d\n"
		"ext_region_supported=%d\n"
		"ext_phys=%pa\n"
		"ext_size=%zd\n"
		"ext_label=0x%x\n"
		"ext_handle=0x%x\n"
		"last_stage=%s\n"
		"last_ret=%d\n"
		"last_error=%d\n"
		"failure_stage=%s\n"
		"loaded_us=%lld\n"
		"ready_us=%lld\n"
		"started_us=%lld\n"
		"booted_us=%lld\n",
		priv->vm_name, priv->vmid, priv->pas_id, priv->boot_seq,
		priv->vm_loaded, priv->vmid_allocated,
		priv->cpusys_autostart_scheduled, priv->cpusys_autostart_ret,
		priv->vm_status,
		guestvm_vm_status_str(priv->vm_status), priv->os_status,
		guestvm_os_status_str(priv->os_status), priv->app_status,
		priv->iso_needed, priv->ext_region_supported,
		&priv->ext_region.ext_phys, priv->ext_region.ext_size,
		priv->ext_region.ext_label, priv->ext_region.ext_mem_handle,
		priv->last_stage, priv->last_ret, priv->last_error,
		priv->failure_stage,
		priv->loaded_us, priv->ready_us, priv->started_us,
		priv->booted_us);
}

static struct kobj_attribute guestvm_loader_attribute =
__ATTR(boot_guestvm, 0220, NULL, guestvm_loader_start);
static struct kobj_attribute guestvm_loader_status_attribute =
__ATTR(status, 0444, guestvm_loader_status, NULL);

static struct attribute *attrs[] = {
	&guestvm_loader_attribute.attr,
	&guestvm_loader_status_attribute.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int guestvm_loader_probe(struct platform_device *pdev)
{
	struct guestvm_loader_private *priv = NULL;
	const char *sub_sys;
	int ret = 0, i, reserve_cpus_len;
	u32 reserve_cpus[NUM_RESERVED_CPUS] = {0};

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->dev = &pdev->dev;
	mutex_init(&priv->vm_boot_lock);
	INIT_DELAYED_WORK(&priv->cpusys_autostart_work,
			  guestvm_cpusys_autostart);

	ret = of_property_read_string(pdev->dev.of_node, "qcom,firmware-name",
				      &sub_sys);
	if (ret)
		return -EINVAL;
	strlcpy(priv->vm_name, sub_sys, sizeof(priv->vm_name));

	ret = kobject_init_and_add(&priv->vm_loader_kobj, &guestvm_kobj_type,
			kernel_kobj, "%s_%s", "load_guestvm", priv->vm_name);

	if (ret) {
		dev_err(&pdev->dev, "sysfs create and add failed\n");
		ret = -ENOMEM;
		goto error_return;
	}

	ret = sysfs_create_group(&priv->vm_loader_kobj, &attr_group);
	if (ret) {
		dev_err(&pdev->dev, "sysfs create group failed %d\n", ret);
		goto error_return;
	}

	init_completion(&priv->vm_start);
	init_completion(&priv->isolation_done);
	priv->guestvm_nb.notifier_call = guestvm_loader_nb_handler;
	priv->guestvm_nb.priority = 1;
	ret = gh_rm_register_notifier(&priv->guestvm_nb);
	if (ret)
		return ret;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,vmid",
							&priv->vmid);
	if (ret)
		dev_err(&pdev->dev, "Unable to get vmid from DT, ret=%d\n", ret);
	guestvm_set_stage(priv, "probe", ret);
	guestvm_diag(priv, "probe vm=%s dt_vmid=%d ret=%d\n",
		priv->vm_name, priv->vmid, ret);

	priv->iso_needed = of_property_read_bool(pdev->dev.of_node,
							"qcom,isolate-cpus");
	if (!priv->iso_needed) {
		pr_err("%s: no isolation needed for %s\n", __func__, priv->vm_name);
		goto no_iso;
	}

	reserve_cpus_len = of_property_read_variable_u32_array(
					pdev->dev.of_node,
					"qcom,reserved-cpus",
					reserve_cpus, 0, NUM_RESERVED_CPUS);
	for (i = 0; i < reserve_cpus_len; i++)
		if (reserve_cpus[i] < num_possible_cpus())
			cpumask_set_cpu(reserve_cpus[i], &priv->guestvm_reserve_cpus);

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,unisolate-timeout-ms",
				&priv->guestvm_unisolate_timeout);
	if (ret) {
		pr_warn("%s: no unisolate timeout specified\n", __func__);
		priv->guestvm_unisolate_timeout = DEFAULT_UNISO_TIMEOUT_MS;
	}
	timer_setup(&priv->guestvm_cpu_isolate_timer, guestvm_timer_callback, 0);

no_iso:
	priv->vm_status = GH_RM_VM_STATUS_NO_STATE;
	if (guestvm_cpusys_autostart_enabled &&
	    get_gh_vm_name(priv->vm_name) == GH_CPUSYS_VM) {
		priv->cpusys_autostart_scheduled = true;
		schedule_delayed_work(&priv->cpusys_autostart_work,
			msecs_to_jiffies(CPUSYS_AUTOSTART_DELAY_MS));
		guestvm_diag(priv,
			"scheduled cpusys autostart after %d ms\n",
			CPUSYS_AUTOSTART_DELAY_MS);
	}
	return 0;

error_return:
	if (kobject_name(&priv->vm_loader_kobj) != NULL) {
		kobject_del(&priv->vm_loader_kobj);
		kobject_put(&priv->vm_loader_kobj);

		memset(&priv->vm_loader_kobj, 0, sizeof(priv->vm_loader_kobj));
	}

	return ret;
}

static int guestvm_loader_remove(struct platform_device *pdev)
{
	struct guestvm_loader_private *priv = platform_get_drvdata(pdev);
	int ret;

	cancel_delayed_work_sync(&priv->cpusys_autostart_work);

	ret = gh_rm_unregister_notifier(&priv->guestvm_nb);
	if (ret)
		dev_warn(priv->dev, "Failed to unregister RM notifier ret=%d\n",
			 ret);
	else
		guestvm_diag(priv, "unregistered RM notifier\n");

	if (priv->vm_loaded) {
		qcom_scm_pas_shutdown(priv->pas_id);
		priv->vm_loaded = false;
		init_completion(&priv->vm_start);
	}

	if (kobject_name(&priv->vm_loader_kobj) != NULL) {
		kobject_del(&priv->vm_loader_kobj);
		kobject_put(&priv->vm_loader_kobj);

		memset(&priv->vm_loader_kobj, 0, sizeof(priv->vm_loader_kobj));
	}

	return 0;
}

static const struct of_device_id guestvm_loader_match_table[] = {
	{ .compatible = "qcom,guestvm-loader" },
	{},
};

static struct platform_driver guestvm_loader_driver = {
	.probe = guestvm_loader_probe,
	.remove = guestvm_loader_remove,
	.driver = {
		.name = "qcom_guestvm_loader",
		.of_match_table = guestvm_loader_match_table,
	},
};

module_platform_driver(guestvm_loader_driver);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. GuestVM loader");
MODULE_LICENSE("GPL v2");
