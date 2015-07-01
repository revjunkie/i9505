/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <mach/cpufreq.h>
#include <mach/rpm-regulator.h>
#include <mach/rpm-regulator-smd.h>
#include <linux/regulator/consumer.h>

#define MAX_RAILS 5

static struct msm_thermal_data msm_thermal_info;
static uint32_t limited_max_freq = MSM_CPUFREQ_NO_LIMIT;
static struct delayed_work check_temp_work;
static struct delayed_work temp_log_work;
static bool core_control_enabled;
static uint32_t cpus_offlined;
static DEFINE_MUTEX(core_control_mutex);

static int enabled;
static int rails_cnt;
static int limit_idx;
static int limit_idx_low;
static int limit_idx_high;
static int max_tsens_num;
static struct cpufreq_frequency_table *table;
static long current_temp;
static uint32_t usefreq;
static int freq_table_get;
static bool vdd_rstr_enabled;
static bool vdd_rstr_nodes_called;
static bool vdd_rstr_probed;
static DEFINE_MUTEX(vdd_rstr_mutex);

struct rail {
	const char *name;
	uint32_t freq_req;
	uint32_t min_level;
	uint32_t num_levels;
	int32_t curr_level;
	uint32_t levels[3];
	struct kobj_attribute value_attr;
	struct kobj_attribute level_attr;
	struct regulator *reg;
	struct attribute_group attr_gp;
};
static struct rail *rails;

struct vdd_rstr_enable {
	struct kobj_attribute ko_attr;
	uint32_t enabled;
};

#define VDD_RES_RO_ATTRIB(_rail, ko_attr, j, _name) \
	ko_attr.attr.name = __stringify(_name); \
	ko_attr.attr.mode = 444; \
	ko_attr.show = vdd_rstr_reg_##_name##_show; \
	ko_attr.store = NULL; \
	_rail.attr_gp.attrs[j] = &ko_attr.attr;

#define VDD_RES_RW_ATTRIB(_rail, ko_attr, j, _name) \
	ko_attr.attr.name = __stringify(_name); \
	ko_attr.attr.mode = 644; \
	ko_attr.show = vdd_rstr_reg_##_name##_show; \
	ko_attr.store = vdd_rstr_reg_##_name##_store; \
	_rail.attr_gp.attrs[j] = &ko_attr.attr;

#define VDD_RSTR_ENABLE_FROM_ATTRIBS(attr) \
	(container_of(attr, struct vdd_rstr_enable, ko_attr));

#define VDD_RSTR_REG_VALUE_FROM_ATTRIBS(attr) \
	(container_of(attr, struct rail, value_attr));

#define VDD_RSTR_REG_LEVEL_FROM_ATTRIBS(attr) \
	(container_of(attr, struct rail, level_attr));
/* If freq table exists, then we can send freq request */
static int check_freq_table(void)
{
	int ret = 0;
	struct cpufreq_frequency_table *table = NULL;

	table = cpufreq_frequency_get_table(0);
	if (!table) {
		pr_debug("%s: error reading cpufreq table\n", __func__);
		return -EINVAL;
	}
	freq_table_get = 1;

	return ret;
}

static int update_cpu_min_freq_all(uint32_t min)
{
	int cpu = 0;
	int ret = 0;
	struct cpufreq_policy *policy = NULL;

	if (!freq_table_get) {
		ret = check_freq_table();
		if (ret) {
			pr_err("%s:Fail to get freq table\n", __func__);
			return ret;
		}
	}
	/* If min is larger than allowed max */
	if (min != MSM_CPUFREQ_NO_LIMIT &&
			min > table[limit_idx_high].frequency)
		min = table[limit_idx_high].frequency;

	for_each_possible_cpu(cpu) {
		ret = msm_cpufreq_set_freq_limits(cpu, min, limited_max_freq);
		if (ret) {
			pr_err("%s:Fail to set limits for cpu%d\n",
					__func__, cpu);
			return ret;
		}

		if (cpu_online(cpu)) {
			policy = cpufreq_cpu_get(cpu);
			if (!policy)
				continue;
			cpufreq_driver_target(policy, policy->cur,
					CPUFREQ_RELATION_L);
			cpufreq_cpu_put(policy);
		}
	}

	return ret;
}

static int vdd_restriction_apply_freq(struct rail *r, int level)
{
	int ret = 0;

	if (level == r->curr_level)
		return ret;

	/* level = -1: disable, level = 0,1,2..n: enable */
	if (level == -1) {
		ret = update_cpu_min_freq_all(r->min_level);
		if (ret)
			return ret;
		else
			r->curr_level = -1;
	} else if (level >= 0 && level < (r->num_levels)) {
		ret = update_cpu_min_freq_all(r->levels[level]);
		if (ret)
			return ret;
		else
			r->curr_level = level;
	} else {
		pr_err("level input:%d is not within range\n", level);
		return -EINVAL;
	}

	return ret;
}

static int vdd_restriction_apply_voltage(struct rail *r, int level)
{
	int ret = 0;

	if (r->reg == NULL) {
		pr_info("Do not have regulator handle:%s, can't apply vdd\n",
				r->name);
		return -EFAULT;
	}
	if (level == r->curr_level)
		return ret;

	/* level = -1: disable, level = 0,1,2..n: enable */
	if (level == -1) {
		ret = regulator_set_voltage(r->reg, r->min_level,
			r->levels[r->num_levels - 1]);
		if (!ret)
			r->curr_level = -1;
	} else if (level >= 0 && level < (r->num_levels)) {
		ret = regulator_set_voltage(r->reg, r->levels[level],
			r->levels[r->num_levels - 1]);
		if (!ret)
			r->curr_level = level;
	} else {
		pr_err("level input:%d is not within range\n", level);
		return -EINVAL;
	}

	return ret;
}

static int vdd_rstr_en_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct vdd_rstr_enable *en = VDD_RSTR_ENABLE_FROM_ATTRIBS(attr);

	return snprintf(buf, PAGE_SIZE, "%d\n", en->enabled);
}

static ssize_t vdd_rstr_en_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int i = 0;
	uint8_t en_cnt = 0;
	uint8_t dis_cnt = 0;
	uint32_t val = 0;
	struct kernel_param kp;
	struct vdd_rstr_enable *en = VDD_RSTR_ENABLE_FROM_ATTRIBS(attr);

	mutex_lock(&vdd_rstr_mutex);
	kp.arg = &val;
	ret = param_set_bool(buf, &kp);
	if (ret) {
		pr_err("Invalid input %s for enabled\n", buf);
		goto done_vdd_rstr_en;
	}

	if ((val == 0) && (en->enabled == 0))
		goto done_vdd_rstr_en;

	for (i = 0; i < rails_cnt; i++) {
		if (rails[i].freq_req == 1 && freq_table_get)
			ret = vdd_restriction_apply_freq(&rails[i],
					(val) ? 0 : -1);
		else
			ret = vdd_restriction_apply_voltage(&rails[i],
			(val) ? 0 : -1);

		/*
		 * Even if fail to set one rail, still try to set the
		 * others. Continue the loop
		 */
		if (ret)
			pr_err("Set vdd restriction for %s failed\n",
					rails[i].name);
		else {
			if (val)
				en_cnt++;
			else
				dis_cnt++;
		}
	}
	/* As long as one rail is enabled, vdd rstr is enabled */
	if (val && en_cnt)
		en->enabled = 1;
	else if (!val && (dis_cnt == rails_cnt))
		en->enabled = 0;

done_vdd_rstr_en:
	mutex_unlock(&vdd_rstr_mutex);
	return count;
}

static struct vdd_rstr_enable vdd_rstr_en = {
	.ko_attr.attr.name = __stringify(enabled),
	.ko_attr.attr.mode = 644,
	.ko_attr.show = vdd_rstr_en_show,
	.ko_attr.store = vdd_rstr_en_store,
	.enabled = 1,
};

static struct attribute *vdd_rstr_en_attribs[] = {
	&vdd_rstr_en.ko_attr.attr,
	NULL,
};

static struct attribute_group vdd_rstr_en_attribs_gp = {
	.attrs  = vdd_rstr_en_attribs,
};

static int vdd_rstr_reg_value_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int val = 0;
	struct rail *reg = VDD_RSTR_REG_VALUE_FROM_ATTRIBS(attr);
	/* -1:disabled, -2:fail to get regualtor handle */
	if (reg->curr_level < 0)
		val = reg->curr_level;
	else
		val = reg->levels[reg->curr_level];

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static int vdd_rstr_reg_level_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct rail *reg = VDD_RSTR_REG_LEVEL_FROM_ATTRIBS(attr);
	return snprintf(buf, PAGE_SIZE, "%d\n", reg->curr_level);
}

static ssize_t vdd_rstr_reg_level_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;

	struct rail *reg = VDD_RSTR_REG_LEVEL_FROM_ATTRIBS(attr);

	mutex_lock(&vdd_rstr_mutex);
	if (vdd_rstr_en.enabled == 0)
		goto done_store_level;

	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		pr_err("Invalid input %s for level\n", buf);
		goto done_store_level;
	}

	if (val < 0 || val > reg->num_levels - 1) {
		pr_err(" Invalid number %d for level\n", val);
		goto done_store_level;
	}

	if (val != reg->curr_level) {
		if (reg->freq_req == 1 && freq_table_get)
			update_cpu_min_freq_all(reg->levels[val]);
		else {
			ret = vdd_restriction_apply_voltage(reg, val);
			if (ret) {
				pr_err( \
				"Set vdd restriction for regulator %s failed\n",
				reg->name);
				goto done_store_level;
			}
		}
		reg->curr_level = val;
	}

done_store_level:
	mutex_unlock(&vdd_rstr_mutex);
	return count;
}

/* 1:enable, 0:disable */
static int vdd_restriction_apply_all(int en)
{
	int i = 0;
	int en_cnt = 0;
	int dis_cnt = 0;
	int fail_cnt = 0;
	int ret = 0;

	for (i = 0; i < rails_cnt; i++) {
		if (rails[i].freq_req == 1 && freq_table_get)
			ret = vdd_restriction_apply_freq(&rails[i],
					en ? 0 : -1);
		else
			ret = vdd_restriction_apply_voltage(&rails[i],
					en ? 0 : -1);
		if (ret) {
			pr_err("Cannot set voltage for %s", rails[i].name);
			fail_cnt++;
		} else {
			if (en)
				en_cnt++;
			else
				dis_cnt++;
		}
	}

	/* As long as one rail is enabled, vdd rstr is enabled */
	if (en && en_cnt)
		vdd_rstr_en.enabled = 1;
	else if (!en && (dis_cnt == rails_cnt))
		vdd_rstr_en.enabled = 0;

	/*
	 * Check fail_cnt again to make sure all of the rails are applied
	 * restriction successfully or not
	 */
	if (fail_cnt)
		return -EFAULT;
	return ret;
}

static int msm_thermal_get_freq_table(void)
{
	int ret = 0;
	int i = 0;

	table = cpufreq_frequency_get_table(0);
	if (table == NULL) {
		pr_debug("%s: error reading cpufreq table\n", KBUILD_MODNAME);
		ret = -EINVAL;
		goto fail;
	}

	while (table[i].frequency != CPUFREQ_TABLE_END)
		i++;
#if defined (CONFIG_MACH_M2_REFRESHSPR)
	limit_idx_low = 5;
#else
	limit_idx_low = 0;
#endif
	limit_idx_high = limit_idx = i - 1;
	BUG_ON(limit_idx_high <= 0 || limit_idx_high <= limit_idx_low);
fail:
	return ret;
}

static int update_cpu_max_freq(int cpu, uint32_t max_freq)
{
	int ret = 0;

	ret = msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT, max_freq);
	if (ret)
		return ret;

	limited_max_freq = max_freq;
	if (max_freq != MSM_CPUFREQ_NO_LIMIT)
		pr_info("%s: Limiting cpu%d max frequency to %d (TEMP=%ld)\n",
				KBUILD_MODNAME, cpu, max_freq, current_temp);
	else
		pr_info("%s: Max frequency reset for cpu%d\n",
				KBUILD_MODNAME, cpu);

	if (cpu_online(cpu)) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return ret;
		ret = cpufreq_driver_target(policy, policy->cur,
				CPUFREQ_RELATION_H);
		cpufreq_cpu_put(policy);
	}

	return ret;
}

static void __ref do_core_control(long temp)
{
	int i = 0;
	int ret = 0;

	if (!core_control_enabled)
		return;

	/**
	 *  Offline cores starting from the max MPIDR to 1, when above limit,
	 *  The core control mask is non zero and allows the core to be turned
	 *  off.
	 *  The core was not previously offlined by this module
	 *  The core is the next in sequence.
	 *  If the core was online for some reason, even after it was offlined
	 *  by this module, offline it again.
	 *  Online the back on if the temp is below the hysteresis and was
	 *  offlined by this module and not already online.
	 */
	mutex_lock(&core_control_mutex);
	if (msm_thermal_info.core_control_mask &&
		temp >= msm_thermal_info.core_limit_temp_degC) {
		for (i = num_possible_cpus(); i > 0; i--) {
			if (!(msm_thermal_info.core_control_mask & BIT(i)))
				continue;
			if (cpus_offlined & BIT(i) && !cpu_online(i))
				continue;
			pr_info("%s: Set Offline: CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			ret = cpu_down(i);
			if (ret)
				pr_err("%s: Error %d offline core %d\n",
					KBUILD_MODNAME, ret, i);
			cpus_offlined |= BIT(i);
			break;
		}
	} else if (msm_thermal_info.core_control_mask && cpus_offlined &&
		temp <= (msm_thermal_info.core_limit_temp_degC -
			msm_thermal_info.core_temp_hysteresis_degC)) {
		for (i = 0; i < num_possible_cpus(); i++) {
			if (!(cpus_offlined & BIT(i)))
				continue;
			cpus_offlined &= ~BIT(i);
			pr_info("%s: Allow Online CPU%d Temp: %ld\n",
					KBUILD_MODNAME, i, temp);
			/*
			 * If this core is already online, then bring up the
			 * next offlined core.
			 */
			if (cpu_online(i))
				continue;
			ret = cpu_up(i);
			if (ret)
				pr_err("%s: Error %d online core %d\n",
						KBUILD_MODNAME, ret, i);
			break;
		}
	}
	mutex_unlock(&core_control_mutex);
}

static int do_vdd_restriction(void)
{
	struct tsens_device tsens_dev;
	long temp = 0;
	int ret = 0;
	int i = 0;
	int dis_cnt = 0;

	if (!vdd_rstr_enabled)
		return ret;

	if (usefreq && !freq_table_get) {
		if (check_freq_table())
			return ret;
	}

	mutex_lock(&vdd_rstr_mutex);
	for (i = 0; i < max_tsens_num; i++) {
		tsens_dev.sensor_num = i;
		ret = tsens_get_temp(&tsens_dev, &temp);
		if (ret) {
			pr_debug("%s: Unable to read TSENS sensor %d\n",
					__func__, tsens_dev.sensor_num);
			dis_cnt++;
			continue;
		}
		if (temp <=  msm_thermal_info.vdd_rstr_temp_degC) {
			ret = vdd_restriction_apply_all(1);
			if (ret) {
				pr_err( \
				"Enable vdd rstr votlage for all failed\n");
				goto exit;
			}
			goto exit;
		} else if (temp > msm_thermal_info.vdd_rstr_temp_hyst_degC)
			dis_cnt++;
	}
	if (dis_cnt == max_tsens_num) {
		ret = vdd_restriction_apply_all(0);
		if (ret) {
			pr_err("Disable vdd rstr votlage for all failed\n");
			goto exit;
		}
	}
exit:
	mutex_unlock(&vdd_rstr_mutex);
	return ret;
}


static void __ref do_freq_control(long temp)
{
	int ret = 0;

	int cpu = 0;
	uint32_t max_freq = limited_max_freq;

	if (temp >= msm_thermal_info.limit_temp_degC) {
		if (limit_idx == limit_idx_low)
			return;

		limit_idx -= msm_thermal_info.freq_step;
		if (limit_idx < limit_idx_low)
			limit_idx = limit_idx_low;
		max_freq = table[limit_idx].frequency;
	} else if (temp < msm_thermal_info.limit_temp_degC -
		 msm_thermal_info.temp_hysteresis_degC) {
		if (limit_idx == limit_idx_high)
			return;

		limit_idx += msm_thermal_info.freq_step;
		if (limit_idx >= limit_idx_high) {
			limit_idx = limit_idx_high;
			max_freq = MSM_CPUFREQ_NO_LIMIT;
		} else
			max_freq = table[limit_idx].frequency;
	}
	if (max_freq == limited_max_freq)
		return;

	/* Update new limits */
	for_each_possible_cpu(cpu) {
		if (!(msm_thermal_info.freq_control_mask & BIT(cpu)))
			continue;
		ret = update_cpu_max_freq(cpu, max_freq);
		if (ret)
			pr_debug(
			"%s: Unable to limit cpu%d max freq to %d\n",
					KBUILD_MODNAME, cpu, max_freq);
	}

}

static void __cpuinit check_temp(struct work_struct *work)
{
	static int limit_init;
	struct tsens_device tsens_dev;
	long temp = 0;
	int ret = 0;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_debug("%s: Unable to read TSENS sensor %d\n",
				KBUILD_MODNAME, tsens_dev.sensor_num);
		goto reschedule;
	}

	if (!limit_init) {
		ret = msm_thermal_get_freq_table();
		if (ret)
			goto reschedule;
		else
			limit_init = 1;
	}

	do_core_control(temp);
	do_vdd_restriction();
	do_freq_control(temp);

reschedule:
	if (enabled)
		schedule_delayed_work(&check_temp_work,
				msecs_to_jiffies(msm_thermal_info.poll_ms));
}

static void __ref msm_therm_temp_log(struct work_struct *work)
{
    struct tsens_device tsens_dev;
    long temp = 0;
    uint32_t max_sensors = 0;

    if(!(tsens_get_max_sensor_num(&max_sensors)))
    {
          int i ,added = 0;
          char buffer[500];
          for (i = 0 ; i< max_sensors;i++)
          {
               int ret = 0;
               tsens_dev.sensor_num = i;
               tsens_get_temp(&tsens_dev,&temp);
               ret = sprintf(buffer + added , "(%d --- %ld)", i ,temp );
               added += ret;
          }
          pr_info("%s: Debug Temp for Sensors %s",KBUILD_MODNAME,buffer);

    }
    schedule_delayed_work(&temp_log_work, HZ*5);
}

static int __ref msm_thermal_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;

	if (action == CPU_UP_PREPARE || action == CPU_UP_PREPARE_FROZEN) {
		if (core_control_enabled &&
			(msm_thermal_info.core_control_mask & BIT(cpu)) &&
			(cpus_offlined & BIT(cpu))) {
			pr_info(
			"%s: Preventing cpu%d from coming online.\n",
				KBUILD_MODNAME, cpu);
			return NOTIFY_BAD;
		}
	}


	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_thermal_cpu_notifier = {
	.notifier_call = msm_thermal_cpu_callback,
};

/**
 * We will reset the cpu frequencies limits here. The core online/offline
 * status will be carried over to the process stopping the msm_thermal, as
 * we dont want to online a core and bring in the thermal issues.
 */
static void __ref disable_msm_thermal(void)
{
	int cpu = 0;

	/* make sure check_temp is no longer running */
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();

	if (limited_max_freq == MSM_CPUFREQ_NO_LIMIT)
		return;

	for_each_possible_cpu(cpu) {
		update_cpu_max_freq(cpu, MSM_CPUFREQ_NO_LIMIT);
	}
}

static int __ref set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_bool(val, kp);
	if (!enabled)
		disable_msm_thermal();
	else
		pr_info("%s: no action for enabled = %d\n",
				KBUILD_MODNAME, enabled);

	pr_info("%s: enabled = %d\n", KBUILD_MODNAME, enabled);

	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "enforce thermal limit on cpu");


/* Call with core_control_mutex locked */
static int __ref update_offline_cores(int val)
{
	int cpu = 0;
	int ret = 0;

	cpus_offlined = msm_thermal_info.core_control_mask & val;
	if (!core_control_enabled)
		return 0;

	for_each_possible_cpu(cpu) {
		if (!(cpus_offlined & BIT(cpu)))
		       continue;
		if (!cpu_online(cpu))
			continue;
		ret = cpu_down(cpu);
		if (ret)
			pr_err("%s: Unable to offline cpu%d\n",
				KBUILD_MODNAME, cpu);
	}
	return ret;
}

static ssize_t show_cc_enabled(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", core_control_enabled);
}

static ssize_t __ref store_cc_enabled(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	int val = 0;

	mutex_lock(&core_control_mutex);
	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		pr_err("%s: Invalid input %s\n", KBUILD_MODNAME, buf);
		goto done_store_cc;
	}

	if (core_control_enabled == !!val)
		goto done_store_cc;

	core_control_enabled = !!val;
	if (core_control_enabled) {
		pr_info("%s: Core control enabled\n", KBUILD_MODNAME);
		register_cpu_notifier(&msm_thermal_cpu_notifier);
		update_offline_cores(cpus_offlined);
	} else {
		pr_info("%s: Core control disabled\n", KBUILD_MODNAME);
		unregister_cpu_notifier(&msm_thermal_cpu_notifier);
	}

done_store_cc:
	mutex_unlock(&core_control_mutex);
	return count;
}

static ssize_t show_cpus_offlined(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", cpus_offlined);
}

static ssize_t __ref store_cpus_offlined(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	uint32_t val = 0;

	mutex_lock(&core_control_mutex);
	ret = kstrtouint(buf, 10, &val);
	if (ret) {
		pr_err("%s: Invalid input %s\n", KBUILD_MODNAME, buf);
		goto done_cc;
	}

	if (enabled) {
		pr_err("%s: Ignoring request; polling thread is enabled.\n",
				KBUILD_MODNAME);
		goto done_cc;
	}

	if (cpus_offlined == val)
		goto done_cc;

	update_offline_cores(val);
done_cc:
	mutex_unlock(&core_control_mutex);
	return count;
}

static __refdata struct kobj_attribute cc_enabled_attr =
__ATTR(enabled, 0644, show_cc_enabled, store_cc_enabled);

static __refdata struct kobj_attribute cpus_offlined_attr =
__ATTR(cpus_offlined, 0644, show_cpus_offlined, store_cpus_offlined);

static __refdata struct attribute *cc_attrs[] = {
	&cc_enabled_attr.attr,
	&cpus_offlined_attr.attr,
	NULL,
};

static __refdata struct attribute_group cc_attr_group = {
	.attrs = cc_attrs,
};

static __init int msm_thermal_add_cc_nodes(void)
{
	struct kobject *module_kobj = NULL;
	struct kobject *cc_kobj = NULL;
	int ret = 0;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("%s: cannot find kobject for module\n",
			KBUILD_MODNAME);
		ret = -ENOENT;
		goto done_cc_nodes;
	}

	cc_kobj = kobject_create_and_add("core_control", module_kobj);
	if (!cc_kobj) {
		pr_err("%s: cannot create core control kobj\n",
				KBUILD_MODNAME);
		ret = -ENOMEM;
		goto done_cc_nodes;
	}

	ret = sysfs_create_group(cc_kobj, &cc_attr_group);
	if (ret) {
		pr_err("%s: cannot create group\n", KBUILD_MODNAME);
		goto done_cc_nodes;
	}

	return 0;

done_cc_nodes:
	if (cc_kobj)
		kobject_del(cc_kobj);
	return ret;
}

int __devinit msm_thermal_init(struct msm_thermal_data *pdata)
{
	int ret = 0;

	BUG_ON(!pdata);
	tsens_get_max_sensor_num(&max_tsens_num);
	BUG_ON(msm_thermal_info.sensor_id >= max_tsens_num);
	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));

	enabled = 1;
	core_control_enabled = 1;
	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	schedule_delayed_work(&check_temp_work, 0);

	register_cpu_notifier(&msm_thermal_cpu_notifier);

	return ret;
}

static int vdd_restriction_reg_init(struct platform_device *pdev)
{
	int ret = 0;
	int i;

	for (i = 0; i < rails_cnt; i++) {
		if (rails[i].freq_req == 1) {
			usefreq |= BIT(i);
			check_freq_table();
			/* Restrict frequency by default until we have made
			 * our first temp reading */
			if (freq_table_get)
				ret = vdd_restriction_apply_freq(&rails[i], 0);
			else
				pr_info("%s:Defer vdd rstr freq init\n",
						__func__);
		} else {
			rails[i].reg = devm_regulator_get(&pdev->dev,
					rails[i].name);
			if (IS_ERR_OR_NULL(rails[i].reg)) {
				ret = PTR_ERR(rails[i].reg);
				if (ret != -EPROBE_DEFER) {
					pr_err( \
					"%s, could not get regulator: %s\n",
					rails[i].name, __func__);
					rails[i].reg = NULL;
					rails[i].curr_level = -2;
					return ret;
				}
				return ret;
			}
			/* Restrict votlage by default until we have made
			 * our first temp reading */
			ret = vdd_restriction_apply_voltage(&rails[i], 0);
		}
	}

	return ret;
}

static int msm_thermal_add_vdd_rstr_nodes(void)
{
	struct kobject *module_kobj = NULL;
	struct kobject *vdd_rstr_kobj = NULL;
	struct kobject *vdd_rstr_reg_kobj[MAX_RAILS] = {0};
	int rc = 0;
	int i = 0;

	if (!vdd_rstr_probed) {
		vdd_rstr_nodes_called = true;
		return rc;
	}

	if (vdd_rstr_probed && rails_cnt == 0)
		return rc;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("%s: cannot find kobject for module %s\n",
			__func__, KBUILD_MODNAME);
		rc = -ENOENT;
		goto thermal_sysfs_add_exit;
	}

	vdd_rstr_kobj = kobject_create_and_add("vdd_restriction", module_kobj);
	if (!vdd_rstr_kobj) {
		pr_err("%s: cannot create vdd_restriction kobject\n", __func__);
		rc = -ENOMEM;
		goto thermal_sysfs_add_exit;
	}

	rc = sysfs_create_group(vdd_rstr_kobj, &vdd_rstr_en_attribs_gp);
	if (rc) {
		pr_err("%s: cannot create kobject attribute group\n", __func__);
		rc = -ENOMEM;
		goto thermal_sysfs_add_exit;
	}

	for (i = 0; i < rails_cnt; i++) {
		vdd_rstr_reg_kobj[i] = kobject_create_and_add(rails[i].name,
					vdd_rstr_kobj);
		if (!vdd_rstr_reg_kobj[i]) {
			pr_err("%s: cannot create for kobject for %s\n",
					__func__, rails[i].name);
			rc = -ENOMEM;
			goto thermal_sysfs_add_exit;
		}

		rails[i].attr_gp.attrs = kzalloc(sizeof(struct attribute *) * 3,
					GFP_KERNEL);
		if (!rails[i].attr_gp.attrs) {
			rc = -ENOMEM;
			goto thermal_sysfs_add_exit;
		}

		VDD_RES_RW_ATTRIB(rails[i], rails[i].level_attr, 0, level);
		VDD_RES_RO_ATTRIB(rails[i], rails[i].value_attr, 1, value);
		rails[i].attr_gp.attrs[2] = NULL;

		rc = sysfs_create_group(vdd_rstr_reg_kobj[i],
				&rails[i].attr_gp);
		if (rc) {
			pr_err("%s: cannot create attribute group for %s\n",
					__func__, rails[i].name);
			goto thermal_sysfs_add_exit;
		}
	}

	return rc;

thermal_sysfs_add_exit:
	if (rc) {
		for (i = 0; i < rails_cnt; i++) {
			kobject_del(vdd_rstr_reg_kobj[i]);
			kfree(rails[i].attr_gp.attrs);
		}
		if (vdd_rstr_kobj)
			kobject_del(vdd_rstr_kobj);
	}
	return rc;
}

static int probe_vdd_rstr(struct device_node *node,
		struct msm_thermal_data *data, struct platform_device *pdev)
{
	int ret = 0;
	int i = 0;
	int arr_size;
	char *key = NULL;
	struct device_node *child_node = NULL;

	key = "qcom,vdd-restriction-temp";
	ret = of_property_read_u32(node, key, &data->vdd_rstr_temp_degC);
	if (ret)
		goto read_node_fail;

	key = "qcom,vdd-restriction-temp-hysteresis";
	ret = of_property_read_u32(node, key, &data->vdd_rstr_temp_hyst_degC);
	if (ret)
		goto read_node_fail;

	for_each_child_of_node(node, child_node) {
		rails_cnt++;
	}

	if (rails_cnt == 0)
		goto read_node_fail;
	if (rails_cnt >= MAX_RAILS) {
		pr_err("%s: Too many rails.\n", __func__);
		return -EFAULT;
	}

	rails = kzalloc(sizeof(struct rail) * rails_cnt,
				GFP_KERNEL);
	if (!rails) {
		pr_err("%s: Fail to allocate memory for rails.\n", __func__);
		return -ENOMEM;
	}

	i = 0;
	for_each_child_of_node(node, child_node) {
		key = "qcom,vdd-rstr-reg";
		ret = of_property_read_string(child_node, key, &rails[i].name);
		if (ret)
			goto read_node_fail;

		key = "qcom,levels";
		if (!of_get_property(child_node, key, &arr_size))
			goto read_node_fail;
		rails[i].num_levels = arr_size/sizeof(__be32);
		if (rails[i].num_levels >
			sizeof(rails[i].levels)/sizeof(uint32_t)) {
			pr_err("%s: Array size too large\n", __func__);
			return -EFAULT;
		}
		ret = of_property_read_u32_array(child_node, key,
				rails[i].levels, rails[i].num_levels);
		if (ret)
			goto read_node_fail;

		key = "qcom,freq-req";
		rails[i].freq_req = of_property_read_bool(child_node, key);
		if (rails[i].freq_req)
			rails[i].min_level = MSM_CPUFREQ_NO_LIMIT;
		else {
			key = "qcom,min-level";
			ret = of_property_read_u32(child_node, key,
				&rails[i].min_level);
			if (ret)
				goto read_node_fail;
		}

		rails[i].curr_level = 0;
		rails[i].reg = NULL;
		i++;
	}

	if (rails_cnt) {
		ret = vdd_restriction_reg_init(pdev);
		if (ret) {
			pr_info("%s:Failed to get regulators. KTM continues.\n",
				__func__);
			goto read_node_fail;
		}
		vdd_rstr_enabled = true;
	}
read_node_fail:
	vdd_rstr_probed = true;
	if (ret) {
		dev_info(&pdev->dev,
			"%s:Failed reading node=%s, key=%s. KTM continues\n",
			__func__, node->full_name, key);
		kfree(rails);
		rails_cnt = 0;
	}
	if (ret == -EPROBE_DEFER)
		vdd_rstr_probed = false;
	return ret;
}

static int __devinit msm_thermal_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	char *key = NULL;
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;

	memset(&data, 0, sizeof(struct msm_thermal_data));
	key = "qcom,sensor-id";
	ret = of_property_read_u32(node, key, &data.sensor_id);
	if (ret)
		goto fail;

	key = "qcom,poll-ms";
	ret = of_property_read_u32(node, key, &data.poll_ms);
	if (ret)
		goto fail;

	key = "qcom,freq-control-mask";
	ret = of_property_read_u32(node, key, &data.freq_control_mask);
	if (ret)
		goto fail;
		
	key = "qcom,limit-temp";
	ret = of_property_read_u32(node, key, &data.limit_temp_degC);
	if (ret)
		goto fail;

	key = "qcom,temp-hysteresis";
	ret = of_property_read_u32(node, key, &data.temp_hysteresis_degC);
	if (ret)
		goto fail;

	key = "qcom,freq-step";
	ret = of_property_read_u32(node, key, &data.freq_step);
	if (ret)
		goto fail;

	key = "qcom,core-limit-temp";
	ret = of_property_read_u32(node, key, &data.core_limit_temp_degC);
	if (ret)
		goto fail;

	key = "qcom,core-temp-hysteresis";
	ret = of_property_read_u32(node, key, &data.core_temp_hysteresis_degC);
	if (ret)
		goto fail;

	key = "qcom,core-control-mask";
	ret = of_property_read_u32(node, key, &data.core_control_mask);
	if (ret)
		goto fail;
	
	/* Probe optional properties below*/
	ret = probe_vdd_rstr(node, &data, pdev);
	if (ret == -EPROBE_DEFER)
		goto fail;
	/* In case sysfs add nodes get called before probe function.
	 * Need to make sure sysfs node is created again */
	if (vdd_rstr_nodes_called) {
		msm_thermal_add_vdd_rstr_nodes();
		vdd_rstr_nodes_called = false;
	}
fail:
	if (ret)
		pr_err("%s: Failed reading node=%s, key=%s\n",
		       __func__, node->full_name, key);
	else
		ret = msm_thermal_init(&data);

	return ret;
}

static int msm_thermal_dev_exit(struct platform_device * inp_dev)
{
        cancel_delayed_work_sync(&temp_log_work);
        return 0;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.driver = {
		.name = "msm-thermal",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
        .remove = msm_thermal_dev_exit,
};

int __init msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

int __init msm_thermal_late_init(void)
{
        INIT_DELAYED_WORK(&temp_log_work,msm_therm_temp_log);
        schedule_delayed_work(&temp_log_work,HZ*2);
	msm_thermal_add_cc_nodes();
	msm_thermal_add_vdd_rstr_nodes();

	return 0;
}
module_init(msm_thermal_late_init);
