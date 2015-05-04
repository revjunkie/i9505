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

struct rev_tune
{
unsigned int active;
unsigned int shift_all;
unsigned int shift_one;
unsigned int shift_threshold;
unsigned int shift_all_threshold;
unsigned int down_shift;
unsigned int downshift_threshold;
unsigned int sample_time;
unsigned int min_cpu;
unsigned int max_cpu;
unsigned int down_diff;
unsigned int shift_diff;
unsigned int shift_diff_all;
} rev = {
	.active = 1,
	.shift_all = 98,
	.shift_one = 60,
	.shift_threshold = 2,
	.shift_all_threshold = 1,
	.down_shift = 30,
	.downshift_threshold = 10,
	.sample_time = (HZ / 5),
	.min_cpu = 1,
	.max_cpu = 4,
};

struct cpu_info
{
unsigned int cur;
u64 prev_cpu_idle;
u64 prev_cpu_wall;
unsigned int load;
};

static DEFINE_PER_CPU(struct cpu_info, rev_info);
static DEFINE_MUTEX(hotplug_lock);

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
	rev.shift_diff_all = 0;
}

static inline void hotplug_all(void)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) 
		if (!cpu_online(cpu) && num_online_cpus() < rev.max_cpu) 
			cpu_up(cpu);
			REV_INFO("onlining %d cpus\n", cpu);
	
	reset_counter();
}

static inline void hotplug_one(void)
{
	unsigned int cpu;
	
	cpu = cpumask_next_zero(0, cpu_online_mask);
		if (cpu < nr_cpu_ids)
			cpu_up(cpu);		
			REV_INFO("online CPU %d\n", cpu);
			
	reset_counter();
}

static inline void unplug_one(void)
{
	unsigned int i, cpu = 0, i_state = 0;
	struct cpu_info *idle_info;
	
	for (i = 1; i < rev.max_cpu; i++) {
		if (!cpu_online(i))
			continue;
			idle_info = &per_cpu(rev_info, i);
			idle_info->cur = idle_cpu(i);
			REV_INFO("cpu %u idle state %d\n", i, idle_info->cur);
			if (i_state == 0) {
				cpu = i;
				i_state = idle_info->cur;
				continue;
			}	
			if (idle_info->cur > i_state) {
				cpu = i;
				i_state = idle_info->cur;
		}
	}
	if (cpu != 0 && i_state > 0) { 
		cpu_down(cpu);
		REV_INFO("offline cpu %d\n", cpu);
	}
	reset_counter();
}

static void  __cpuinit hotplug_decision_work(struct work_struct *work)
{
	unsigned int online_cpus, down_load, up_load, load;
	unsigned int i, total_load = 0;
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);
	
	mutex_lock(&hotplug_lock);
	
	for_each_online_cpu(i) {
		struct cpu_info *tmp_info;
		u64 cur_wall_time, cur_idle_time;
		unsigned int idle_time, wall_time;
		tmp_info = &per_cpu(rev_info, i);
		cur_idle_time = get_cpu_idle_time_us(i, &cur_wall_time);
		idle_time = (unsigned int) (cur_idle_time - tmp_info->prev_cpu_idle);
		tmp_info->prev_cpu_idle = cur_idle_time;
		wall_time = (unsigned int) (cur_wall_time - tmp_info->prev_cpu_wall);
		tmp_info->prev_cpu_wall = cur_wall_time;
		if (wall_time < idle_time)
			break;
		tmp_info->load = 100 * (wall_time - idle_time) / wall_time;
		total_load += tmp_info->load;
		}
	
	online_cpus = num_online_cpus();
	load = (total_load * policy->cur / policy->cpuinfo.max_freq) / online_cpus; 
		REV_INFO("load is %d online cpus: %d\n", load, online_cpus);
	up_load = online_cpus > 1 ? rev.shift_one : 20;
	down_load = online_cpus > 2 ? rev.down_shift : 5;
	
		if (load > up_load && online_cpus < rev.max_cpu) {
				++rev.shift_diff;
				REV_INFO("shift_diff is %d\n", rev.shift_diff);
			if (rev.down_diff) {
				rev.down_diff = 0;
				REV_INFO("down_diff reset to %d\n", rev.down_diff);
				}
			if (load > rev.shift_all) {
				++rev.shift_diff_all;
				REV_INFO("shift_diff_all is %d\n", rev.shift_diff_all);
				if (rev.shift_diff_all > rev.shift_all_threshold) 		
					hotplug_all();		
			} else if (rev.shift_diff > rev.shift_threshold) 
					hotplug_one();		
									
		} else {	
			rev.shift_diff = 0;
			rev.shift_diff_all = 0;
			if (load < down_load && online_cpus > rev.min_cpu) {
				++rev.down_diff;
				REV_INFO("down_diff is %d down_load is %d\n", rev.down_diff, down_load);
				if (rev.down_diff > rev.downshift_threshold) 
					unplug_one();
				}
		}	
	queue_delayed_work_on(0, hotplug_wq, &hotplug_work, rev.sample_time);
	mutex_unlock(&hotplug_lock);
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
show_one(shift_all_threshold, shift_all_threshold);
show_one(down_shift, down_shift);
show_one(downshift_threshold, downshift_threshold);
show_one(sample_time, sample_time);
show_one(min_cpu, min_cpu);
show_one(max_cpu, max_cpu);

static ssize_t __ref store_active(struct kobject *a, struct attribute *b, const char *buf, size_t count)
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
			hotplug_all();
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
store_one(shift_all_threshold, shift_all_threshold);
store_one(down_shift, down_shift);
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
	&shift_all_threshold.attr,
	&down_shift.attr,
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
	
static int __init rev_hotplug_init(void)
{
	int ret;

	hotplug_wq = alloc_workqueue("hotplug_decision_work",
				WQ_HIGHPRI, 0);	

	INIT_DELAYED_WORK(&hotplug_work, hotplug_decision_work);
	if (rev.active)
	schedule_delayed_work_on(0, &hotplug_work, HZ * 20);

	rev_kobject = kobject_create_and_add("rev_hotplug", kernel_kobj);
	if (rev_kobject) {
	ret = sysfs_create_group(rev_kobject, &rev_hotplug_group);
	if (ret) {
		ret = -EINVAL;
		goto err;
		}
	}

	return 0;
	
err:
	return ret;
}

late_initcall(rev_hotplug_init);
