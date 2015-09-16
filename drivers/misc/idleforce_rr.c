/*
 * drivers/cpuidle/idleforce_rr.c - Cyclic idle injection
 *
 * Based in thermal/intel_poweclamp.c
 *
 * Copyright (c) 2015, Baylibre.
 * Copyright (c) 2012, Intel Corporation.
 *
 * Authors:
 *     Neil Armstrong <narmstrong@baylibre.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sched/rt.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <asm/proc-fns.h>
#ifdef CONFIG_SMP
#include <linux/smp.h>
#endif

#define CREATE_TRACE_POINTS
#include <trace/events/idleforce.h>

/* Default delay values */
#define DEFAULT_WAIT_DELAY_MS	100
#define DEFAULT_IDLE_DELAY_MS	10

enum idle_mode_e {
	IDLE_MODE_RR = 0,
};

enum idle_state_e {
	T_DO_SLEEP = 0,
	T_WAIT_PREV,
	T_WAIT_SLOT
};

/* sysfs entry */
static struct kobject *sysfs_dir;

/* Startup status */
static bool idling;

/* Current Mode */
static unsigned idle_mode = IDLE_MODE_RR;

/* Current delay values */
static unsigned cur_wait_delay = DEFAULT_WAIT_DELAY_MS;
static unsigned cur_idle_delay = DEFAULT_IDLE_DELAY_MS;

/* Kthread handles */
static struct task_struct * __percpu *idleforce_rr_thread;
static unsigned long *cpu_idling_mask;  /* bit map for tracking per cpu
					 * idling thread
					 */

/* Kthreads contexts */
static struct idleforce_rr_task_s {
	long first;
	long int prev_cpu;
	long int next_cpu;
	unsigned do_sleep;
	wait_queue_head_t queue;
} __percpu *idleforce_rr_task;

/* Main per-cpu Kthread */
static int idleforce_rr_kthread(void *arg)
{
	int cpunr = (unsigned long)arg;
	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
	};
	struct idleforce_rr_task_s *t =
		per_cpu_ptr((void *)idleforce_rr_task, cpunr);
	struct idleforce_rr_task_s *t_next = NULL,
				   *t_prev = NULL;
	unsigned jiffies_wait, jiffies_idle;
	unsigned state;

	if (t->first)
		state = T_WAIT_SLOT; /* give time to all threads to startup */
	else
		state = T_WAIT_PREV;

	if (t->prev_cpu < 0) {
		pr_err("invalid prev %li\n", t->prev_cpu);
		return 0;
	}

	if (t->next_cpu < 0) {
		pr_err("invalid next %li\n", t->next_cpu);
		return 0;
	}

	if (msecs_to_jiffies(cur_wait_delay))
		jiffies_wait = msecs_to_jiffies(cur_wait_delay);
	else
		jiffies_wait = 1;

	if (msecs_to_jiffies(cur_idle_delay))
		jiffies_idle = msecs_to_jiffies(cur_idle_delay);
	else
		jiffies_idle = 1;

	pr_info("live on cpu %d first=%li prev=%li next=%li wait=%dms(%dj) idle=%dms(%dj)\n",
			cpunr, t->first,
			t->prev_cpu, t->next_cpu,
			cur_wait_delay, jiffies_wait,
			cur_idle_delay, jiffies_idle);

	if (t->prev_cpu >= 0)
		t_prev = per_cpu_ptr((void *)idleforce_rr_task,
					    t->prev_cpu);

	if (t->next_cpu >= 0)
		t_next = per_cpu_ptr((void *)idleforce_rr_task,
					    t->next_cpu);

	trace_idle_thread_start(cpunr);

	set_bit(cpunr, cpu_idling_mask);
	set_freezable();
	sched_setscheduler(current, SCHED_FIFO, &param);

	while (true == idling && !kthread_should_stop()) {

		try_to_freeze();

		switch (state) {
		case T_DO_SLEEP:
		{
			t->do_sleep = 1;

			trace_idle_wait_wake_next(cpunr);

			wake_up_interruptible(&t_next->queue);

			preempt_disable();

			tick_nohz_idle_enter();

			trace_idle_start(cpunr);
			while (t->do_sleep && idling &&
					!kthread_should_stop()) {
				cpu_do_idle();
				trace_idle_wakeup(cpunr);
			}

			tick_nohz_idle_exit();
			preempt_enable();

			state = T_WAIT_PREV;
		} break;

		case T_WAIT_PREV:
		{
			wait_event_interruptible(t->queue, (t_prev->do_sleep ||
							!idling ||
							kthread_should_stop()));

			if (!idling || kthread_should_stop())
				break;

			trace_idle_wait_start(cpunr);

			schedule_timeout_interruptible(jiffies_idle);

			t_prev->do_sleep = 0;

			trace_idle_wait_end(cpunr);

			smp_send_reschedule(t->prev_cpu);

			if (t->first)
				state = T_WAIT_SLOT;
			else
				state = T_DO_SLEEP;
		} break;

		case T_WAIT_SLOT:
		{
			trace_idle_wait_start(cpunr);

			schedule_timeout_interruptible(jiffies_wait);

			trace_idle_wait_end(cpunr);

			state = T_DO_SLEEP;
		} break;

		default:
			goto thread_end;
		}
	}

thread_end:
	clear_bit(cpunr, cpu_idling_mask);

	trace_idle_thread_stop(cpunr);

	return 0;
}

static int start_idleforce(void)
{
	unsigned long cpu;
	struct task_struct *thread;

	static int long cpu_last = -1;
	static int long cpu_first = -1;

	if (idling)
		return -EINVAL;

	if (idle_mode != IDLE_MODE_RR)
		return -EINVAL;

	idling = true;

	/* create one thread per online cpu */
	for_each_online_cpu(cpu) {
		static int long cpu_prev = -1;
		struct task_struct **p =
			per_cpu_ptr(idleforce_rr_thread, cpu);
		struct idleforce_rr_task_s *t =
			per_cpu_ptr((void *)idleforce_rr_task, cpu);

		t->prev_cpu = cpu_prev;
		t->next_cpu = -1;

		/* fill next cpu */
		if (cpu_prev >= 0) {
			struct idleforce_rr_task_s *t_prev =
				per_cpu_ptr(idleforce_rr_task, cpu_prev);
			t_prev->next_cpu = cpu;
		}

		init_waitqueue_head(&t->queue);
		t->do_sleep = 0;

		thread = kthread_create_on_node(idleforce_rr_kthread,
				(void *) cpu,
				cpu_to_node(cpu),
				"idleforce/%ld", cpu);
		if (unlikely(IS_ERR(thread))) {
			pr_err("failed to create kthread\n");
			return -1;
		}
		*p = thread;
		pr_info("kthread created cpu=%ld\n", cpu);

		if (cpu_first < 0)
			cpu_first = cpu;

		cpu_prev = cpu;
		cpu_last = cpu;
	}

	if (cpu_first >= 0) {
		struct idleforce_rr_task_s *t =
			per_cpu_ptr((void *)idleforce_rr_task, cpu_first);
		t->prev_cpu = cpu_last;
		t->first = true;
	}

	if (cpu_last >= 0) {
		struct idleforce_rr_task_s *t =
			per_cpu_ptr((void *)idleforce_rr_task, cpu_last);
		t->next_cpu = cpu_first;
	}

	/* start one thread per online cpu */
	for_each_online_cpu(cpu) {
		struct task_struct **p =
			per_cpu_ptr(idleforce_rr_thread, cpu);
		struct idleforce_rr_task_s *t =
			per_cpu_ptr((void *)idleforce_rr_task, cpu);

		/* bind to cpu here */
		kthread_bind(*p, cpu);
		wake_up_process(*p);
		pr_info("kthread started cpu=%ld prev=%li next=%li\n",
				cpu, t->prev_cpu, t->next_cpu);
	}

	return 0;
}

static void end_idleforce(void)
{
	int i;
	struct task_struct *thread;
	struct idleforce_rr_task_s *t;

	if (!idling)
		return;
	idling = false;
	/*
	 * make idling visible to other cpus and give per cpu idling threads
	 * sometime to exit, or gets killed later.
	 */
	smp_mb();
	msleep(20);
	if (bitmap_weight(cpu_idling_mask, num_possible_cpus())) {
		for_each_set_bit(i, cpu_idling_mask, num_possible_cpus()) {
			pr_info("idling thread for cpu %d alive, kill\n", i);
			thread = *per_cpu_ptr(idleforce_rr_thread, i);
			t = per_cpu_ptr((void *)idleforce_rr_task, i);
			wake_up_interruptible(&t->queue);
			smp_send_reschedule(i);
			kthread_stop(thread);
			pr_info("kthread stopped\n");
		}
	}
}

static ssize_t idleforce_rr_mode_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "mode: %d\n", idle_mode);
}

static ssize_t idleforce_rr_mode_store(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	if (count < 1)
		return count;

	if (idling) {
		pr_err("stop idling before changing mode\n");
		return -EINVAL;
	}

	if (buf[0] == '0') {
		idle_mode = IDLE_MODE_RR;
	} else {
		pr_err("invalid idling mode '%c'\n", buf[0] == '1');
		return -EINVAL;
	}

	return count;
}

static ssize_t idleforce_rr_delay_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "wait: %d\nidle: %d\n",
			cur_wait_delay, cur_idle_delay);
}

static ssize_t idleforce_rr_delay_store(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	unsigned wait = 0, idle = 0;

	if (idling) {
		pr_err("stop idling before changin delays\n");
		return -EINVAL;
	}

	if (sscanf(buf, "%u %u", &wait, &idle) != 2) {
		pr_err("delay format error : '<wait> <idle>'\n");
		return -EINVAL;
	}

	if (!wait || !idle) {
		pr_err("delay value error : > 0\n");
		return -EINVAL;
	}

	cur_wait_delay = wait;
	cur_idle_delay = idle;

	pr_info("new delays wait=%d idle=%d\n",
			cur_wait_delay, cur_idle_delay);

	return count;
}

static ssize_t idleforce_rr_ctrl_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "state: %d\n", idling);
}

static ssize_t idleforce_rr_ctrl_store(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	if (count < 1)
		return count;

	if (buf[0] == '1') {
		pr_info("Starting idleforce\n");
		start_idleforce();
	} else if (buf[0] == '0') {
		pr_info("Stopping idleforce\n");
		end_idleforce();
	}

	return count;
}

static struct kobj_attribute idleforce_rr_ctrl_attribute =
	__ATTR(ctrl, 0664, idleforce_rr_ctrl_show, idleforce_rr_ctrl_store);
static struct kobj_attribute idleforce_rr_delay_attribute =
	__ATTR(delay, 0664, idleforce_rr_delay_show, idleforce_rr_delay_store);
static struct kobj_attribute idleforce_rr_mode_attribute =
	__ATTR(mode, 0664, idleforce_rr_mode_show, idleforce_rr_mode_store);

static struct attribute *idleforce_rr_attrs[] = {
	&idleforce_rr_ctrl_attribute.attr,
	&idleforce_rr_delay_attribute.attr,
	&idleforce_rr_mode_attribute.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct attribute_group idleforce_rr_attr_group = {
	.attrs = idleforce_rr_attrs,
};

static inline int idleforce_rr_create_sysfs_files(void)
{
	int retval = 0;

	sysfs_dir = kobject_create_and_add("idleforce_rr", kernel_kobj);
	if (!sysfs_dir)
		return -ENOMEM;

	retval = sysfs_create_group(sysfs_dir, &idleforce_rr_attr_group);
	if (retval)
		kobject_put(sysfs_dir);

	return retval;
}

static int __init idleforce_rr_init(void)
{
	int retval = 0;
	int bitmap_size;

	bitmap_size = BITS_TO_LONGS(num_possible_cpus()) * sizeof(long);
	cpu_idling_mask = kzalloc(bitmap_size, GFP_KERNEL);
	if (!cpu_idling_mask)
		return -ENOMEM;

	idleforce_rr_thread = alloc_percpu(struct task_struct *);
	if (!idleforce_rr_thread) {
		retval = -ENOMEM;
		goto exit_thread;
	}

	idleforce_rr_task = alloc_percpu(struct idleforce_rr_task_s);
	if (!idleforce_rr_task) {
		retval = -ENOMEM;
		goto exit_free;
	}

	retval = idleforce_rr_create_sysfs_files();
	if (retval < 0)
		goto exit_thread_task;

	return 0;

exit_thread_task:
	free_percpu(idleforce_rr_task);
exit_thread:
	free_percpu(idleforce_rr_thread);
exit_free:
	kfree(cpu_idling_mask);
	return retval;
}
module_init(idleforce_rr_init);

static void __exit idleforce_rr_exit(void)
{
	if (sysfs_dir)
		kobject_put(sysfs_dir);

	end_idleforce();

	free_percpu(idleforce_rr_thread);
	free_percpu(idleforce_rr_task);
	kfree(cpu_idling_mask);
}
module_exit(idleforce_rr_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Round Robin Cycle Idle Core force");
