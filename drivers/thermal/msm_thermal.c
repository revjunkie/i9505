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
#include <mach/cpufreq.h>

#define OVERTEMP 	88

static int enabled;
static struct msm_thermal_data msm_thermal_info;
static uint32_t limited_max_freq = MSM_CPUFREQ_NO_LIMIT;
static struct delayed_work check_temp_work;
static int limit_idx;

static struct cpufreq_frequency_table *table;

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

	limit_idx = i - 1;
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
		pr_info("%s: Limiting cpu%d max frequency to %d\n",
 				KBUILD_MODNAME, cpu, max_freq);
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

static void __ref do_freq_control(long temp)
{
	int cpu = 0;
	int th_freq;
	uint32_t max_freq = limited_max_freq;

	if (temp > msm_thermal_info.limit_temp_degC) {
		th_freq = limit_idx - msm_thermal_info.freq_step;
		max_freq = table[th_freq].frequency;
		if (temp > msm_thermal_info.limit_temp_degC + 6) {
			th_freq = limit_idx - (msm_thermal_info.freq_step * 2);
			max_freq = table[th_freq].frequency;
			if (temp > msm_thermal_info.limit_temp_degC + 14) {
				th_freq = limit_idx - (msm_thermal_info.freq_step * 3);
				max_freq = table[th_freq].frequency;
				if (temp > OVERTEMP)
					max_freq = table[2].frequency;
				}
		}
	} else if (temp < msm_thermal_info.limit_temp_degC -
		 msm_thermal_info.temp_hysteresis_degC) {
			max_freq = MSM_CPUFREQ_NO_LIMIT;
	}
	
	if (max_freq == limited_max_freq)
		return;
	else
		pr_info("%s: Threshold reached! %liC\n",
				KBUILD_MODNAME, temp);

	/* Update new limits */

	for_each_possible_cpu(cpu) {
		update_cpu_max_freq(cpu, max_freq);
	}
}

static void check_temp(struct work_struct *work)
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

	do_freq_control(temp);

reschedule:
	if (enabled)
		schedule_delayed_work(&check_temp_work,
				msecs_to_jiffies(msm_thermal_info.poll_ms));
}

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

#define show_one(_name, variable) \
static ssize_t show_##_name (struct kobject *kobj, \
	struct kobj_attribute *attr, char *buf) \
{ \
	return snprintf(buf, PAGE_SIZE, "%u\n", msm_thermal_info.variable); \
}

show_one(limit_temp, limit_temp_degC)
show_one(poll_ms, poll_ms)
show_one(temp_hysteresis, temp_hysteresis_degC)
show_one(freq_step, freq_step)

#define store_one(_name, variable) \
static ssize_t store_##_name(struct kobject *kobj, \
	struct kobj_attribute *attr, const char *buf, size_t count) \
{ \
	unsigned int input;				\
	int ret;						\
    	ret = sscanf(buf, "%u", &input);	\
    	if (ret != 1)					\
    	return -EINVAL;				\
								\
	msm_thermal_info.variable = input; 				\
								\
	return count;					\
}

store_one(limit_temp, limit_temp_degC)
store_one(poll_ms, poll_ms)
store_one(temp_hysteresis, temp_hysteresis_degC)
store_one(freq_step, freq_step)

static __refdata struct kobj_attribute limit_temp =
__ATTR(temp_threshold, 0644, show_limit_temp, store_limit_temp);

static __refdata struct kobj_attribute poll_ms =
__ATTR(poll_ms, 0644, show_poll_ms, store_poll_ms);

static __refdata struct kobj_attribute temp_hysteresis =
__ATTR(temp_hysteresis, 0644, show_temp_hysteresis, store_temp_hysteresis);

static __refdata struct kobj_attribute freq_step =
__ATTR(freq_step, 0644, show_freq_step, store_freq_step);

static __refdata struct attribute *cc_attrs[] = {
	&limit_temp.attr,
	&poll_ms.attr,
	&temp_hysteresis.attr,
	&freq_step.attr,
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

	cc_kobj = kobject_create_and_add("thermal_settings", module_kobj);
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
	BUG_ON(pdata->sensor_id >= TSENS_MAX_SENSORS);
	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));

	enabled = 1;
	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	schedule_delayed_work(&check_temp_work, 0);

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
	WARN_ON(data.sensor_id >= TSENS_MAX_SENSORS);

	key = "qcom,poll-ms";
	ret = of_property_read_u32(node, key, &data.poll_ms);
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

fail:
	if (ret)
		pr_err("%s: Failed reading node=%s, key=%s\n",
		       __func__, node->full_name, key);
	else
		ret = msm_thermal_init(&data);

	return ret;
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
};

int __init msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

int __init msm_thermal_late_init(void)
{
	return msm_thermal_add_cc_nodes();
}
module_init(msm_thermal_late_init);
