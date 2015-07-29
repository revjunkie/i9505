/* Copyright (c) 2015, Raj Ibrahim <rajibrahim@rocketmail.com>. All rights reserved.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/tick.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

struct rev_tune
{
	unsigned int active;
	unsigned int shift_all;
	unsigned int shift_one;
	unsigned int shift_threshold;
	unsigned int downshift_threshold;
	unsigned int sample_time;
	unsigned int min_cpu;
	unsigned int max_cpu;
	unsigned int down_diff;
	unsigned int shift_diff;
} rev = {
	.active = 1,
	.shift_all = 98,
	.shift_one = 60,
	.shift_threshold = 2,
	.downshift_threshold = 10,
	.sample_time = (HZ / 5),
	.min_cpu = 1,
	.max_cpu = 4,
};

struct cpu_info
{
	u64 prev_cpu_idle;
	u64 prev_cpu_wall;
	unsigned int load_at_freq;
};

static DEFINE_PER_CPU(struct cpu_info, rev_info);

static unsigned int debug = 0;
module_param(debug, uint, 0644);

#define REV_INFO(msg...)		\
do { 					\
	if (debug)			\
		pr_info(msg);		\
} while (0)

static struct delayed_work hotplug_work;
static struct workqueue_struct *hotplug_wq;

static void reset_counter(void)
{
	rev.down_diff = 0;
	rev.shift_diff = 0;
}

static void __ref plug_cpu(int max_cpu)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		if (num_online_cpus() == max_cpu)
			break;
		if (!cpu_online(cpu))
			cpu_up(cpu);
			REV_INFO("CPU %u online\n", cpu);
	}
	reset_counter();
}

static void unplug_cpu(void)
{
	unsigned int cpu, idle;

	for (cpu = CONFIG_NR_CPUS; cpu > 0; cpu--) {
		if (!cpu_online(cpu))
			continue;
			idle = idle_cpu(cpu);
			REV_INFO("CPU %u idle state %d\n", cpu, idle);
			if (idle > 0) {
				cpu_down(cpu);
				REV_INFO("Offline cpu %d\n", cpu);
				break;
			}
	}
	reset_counter();
}

static unsigned int get_load(unsigned int cpu)
{
	unsigned int wall_time, idle_time, load;
	u64 cur_wall_time, cur_idle_time;
	struct cpu_info *pcpu = &per_cpu(rev_info, cpu);
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return 0;
	cpufreq_cpu_put(policy);
	cur_idle_time = get_cpu_idle_time_us(cpu, &cur_wall_time);
	wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
	pcpu->prev_cpu_wall = cur_wall_time;
	idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
	pcpu->prev_cpu_idle = cur_idle_time;
	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;
	load = 100 * (wall_time - idle_time) / wall_time;
	pcpu->load_at_freq = (load * policy->cur) / policy->cpuinfo.max_freq;
		REV_INFO("CPU%u: usage: %d cur: %d max: %d\n", cpu, load,
				policy->cur, policy->cpuinfo.max_freq);

	return 0;
}

static void  __ref hotplug_decision_work(struct work_struct *work)
{
	unsigned int online_cpus, load, up_load, cpu;
	unsigned int total_load = 0;
	struct cpu_info *pcpu;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		pcpu = &per_cpu(rev_info, cpu);
		get_load(cpu);
		total_load += pcpu->load_at_freq;
		}
	put_online_cpus();
	online_cpus = num_online_cpus();
	load = total_load / online_cpus;
	up_load = online_cpus > 1 ? rev.shift_one : 30;

		if (load > up_load && online_cpus < rev.max_cpu) {
			++rev.shift_diff;
			rev.down_diff = 0;
			if (rev.shift_diff > rev.shift_threshold) {
				if (load > rev.shift_all && online_cpus > 1)
					plug_cpu(rev.max_cpu);			
				else 
					plug_cpu(online_cpus + 1);
				}	
		} else {
			rev.shift_diff = 0;
			if (online_cpus > rev.min_cpu)
				++rev.down_diff;
				if (rev.down_diff > rev.downshift_threshold)
					unplug_cpu();
	}
	queue_delayed_work_on(0, hotplug_wq, &hotplug_work, rev.sample_time);
	REV_INFO("rev_hotplug - Load: %d Online CPUs: %d SD: %d DD: %d\n",
				load, online_cpus,  rev.shift_diff, rev.down_diff);
}

/**************SYSFS*******************/
struct kobject *rev_kobject;

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", rev.object);			\
}
show_one(active, active);
show_one(shift_one, shift_one);
show_one(shift_all, shift_all);
show_one(shift_threshold, shift_threshold);
show_one(downshift_threshold, downshift_threshold);
show_one(sample_time, sample_time);
show_one(min_cpu, min_cpu);
show_one(max_cpu, max_cpu);

static ssize_t __ref store_active(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	rev.active = input > 1 ? 1 : input;
		if (rev.active) {
		queue_delayed_work_on(0, hotplug_wq, &hotplug_work, rev.sample_time);
		} else {
			plug_cpu(CONFIG_NR_CPUS);
			flush_workqueue(hotplug_wq);
			cancel_delayed_work_sync(&hotplug_work);
		}
	return count;
}
define_one_global_rw(active);

#define store_one(file_name, object)					\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	rev.object = input;						\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one(shift_one, shift_one);
store_one(shift_all, shift_all);
store_one(shift_threshold, shift_threshold);
store_one(downshift_threshold, downshift_threshold);
store_one(sample_time, sample_time);
store_one(min_cpu, min_cpu);
store_one(max_cpu, max_cpu);

static struct attribute *rev_hotplug_attributes[] =
{
	&active.attr,
	&shift_one.attr,
	&shift_all.attr,
	&shift_threshold.attr,
	&downshift_threshold.attr,
	&sample_time.attr,
	&min_cpu.attr,
	&max_cpu.attr,
	NULL
};

static struct attribute_group rev_hotplug_group =
{
	.attrs  = rev_hotplug_attributes,
	.name = "tune",
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void rev_hotplug_early_suspend(struct early_suspend *handler)
{
	unsigned int cpu;
	if (rev.active) 
	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
			cpu_down(cpu);
	}
	pr_info("rev_hotplug: early suspend\n");
}

static void __ref rev_hotplug_late_resume(struct early_suspend *handler)
{
	if (rev.active) {
	pr_info("rev_hotplug: late resume\n");
	plug_cpu(rev.max_cpu);
	}
}

static struct early_suspend rev_hotplug_suspend = {
	.suspend = rev_hotplug_early_suspend,
	.resume = rev_hotplug_late_resume,
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static int __init rev_hotplug_init(void)
{
	int ret;

	hotplug_wq = alloc_workqueue("hotplug_decision_work",
				WQ_HIGHPRI,  1);

	INIT_DELAYED_WORK(&hotplug_work, hotplug_decision_work);
	if (rev.active)
	schedule_delayed_work_on(0, &hotplug_work, HZ * 30);

	rev_kobject = kobject_create_and_add("rev_hotplug", kernel_kobj);
	if (rev_kobject) {
	ret = sysfs_create_group(rev_kobject, &rev_hotplug_group);
	if (ret) {
		ret = -EINVAL;
		goto err;
		}
	}
#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&rev_hotplug_suspend);
#endif
	return 0;

err:
	return ret;
}

late_initcall(rev_hotplug_init);
