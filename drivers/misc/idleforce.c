/*
 * drivers/cpuidle/idleforce.c - Cyclic idle injection
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

/* sysfs entry */
static struct kobject *sysfs_dir;

/* Startup status */
static bool idling;

/* Current delay values */
static unsigned cur_wait_delay = DEFAULT_WAIT_DELAY_MS;
static unsigned cur_idle_delay = DEFAULT_IDLE_DELAY_MS;

/* Kthread handles */
static struct task_struct * __percpu *idleforce_thread;
static unsigned long *cpu_idling_mask;  /* bit map for tracking per cpu
					 * idling thread
					 */

/* Kthreads contexts */
static struct idleforce_task_s {
	long int prev_cpu;
	long int next_cpu;
	unsigned do_sleep;
	wait_queue_head_t queue;
} __percpu *idleforce_task;

/* Timer callback */
static void noop_timer(unsigned long foo)
{
	/* empty... just the fact that we get the interrupt wakes us up */
}

/* Main per-cpu Kthread */
static int idleforce_kthread(void *arg)
{
	int cpunr = (unsigned long)arg;
	DEFINE_TIMER(wakeup_timer, noop_timer, 0, 0);
	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
	};
	struct idleforce_task_s *t =
		per_cpu_ptr((void *)idleforce_task, cpunr);
	unsigned jiffies_wait, jiffies_idle;

	if (msecs_to_jiffies(cur_wait_delay))
		jiffies_wait = msecs_to_jiffies(cur_wait_delay);
	else
		jiffies_wait = 1;

	if (msecs_to_jiffies(cur_idle_delay))
		jiffies_idle = msecs_to_jiffies(cur_idle_delay);
	else
		jiffies_idle = 1;

	pr_info("live on cpu %d prev=%li next=%li wait=%dms(%dj) idle=%dms(%dj)\n",
			cpunr, t->prev_cpu, t->next_cpu,
			cur_wait_delay, jiffies_wait,
			cur_idle_delay, jiffies_idle);

	trace_idle_thread_start(cpunr);

	set_bit(cpunr, cpu_idling_mask);
	set_freezable();
	init_timer_on_stack(&wakeup_timer);
	sched_setscheduler(current, SCHED_FIFO, &param);

	while (true == idling && !kthread_should_stop()) {
		unsigned long target_jiffies;

		try_to_freeze();

		trace_idle_wait_start(cpunr);

		if (t->prev_cpu < 0) {
			/* wait wakeup delay */
			schedule_timeout_interruptible(jiffies_wait);
		} else {
			/* wake up from previous core kthread */
			wait_event_interruptible(t->queue, t->do_sleep);
		}

		trace_idle_wait_end(cpunr);

		/* Wake up next core to prepare WFI */
		if (t->next_cpu >= 0) {
			struct idleforce_task_s *t_next =
				per_cpu_ptr((void *)idleforce_task,
					    t->next_cpu);

			t_next->do_sleep = 1;
			trace_idle_wait_wake_next(cpunr);
			wake_up_interruptible(&t_next->queue);
		}

		/* The first Core waits for timer for a wakeup */
		if (t->prev_cpu < 0) {
			/* set idle wakeup time */
			target_jiffies = jiffies + jiffies_idle;

			mod_timer(&wakeup_timer, target_jiffies);

			/*
			 * stop tick sched during idle time,
			 * interrupts are still
			 * allowed. thus jiffies are updated properly.
			 */
			preempt_disable();

			/* mwait until target jiffies is reached */
			trace_idle_start(cpunr);
			while (time_before(jiffies, target_jiffies)) {
				cpu_do_idle();
				trace_idle_wakeup(cpunr);
			}
			preempt_enable();
		}
		/* The other cores waits an IPI Wakeup message to wake up */
		else {
			preempt_disable();
			/* Set in nohz to only have timer on first core */
			tick_nohz_idle_enter();

			trace_idle_start(cpunr);
			while (t->do_sleep) {
				cpu_do_idle();
				trace_idle_wakeup(cpunr);
			}

			tick_nohz_idle_exit();
			preempt_enable();
		}

		trace_idle_end(cpunr);

#ifdef CONFIG_SMP
		/* wake up next cpu */
		if (t->next_cpu >= 0) {
			struct idleforce_task_s *t_next =
				per_cpu_ptr((void *)idleforce_task,
					    t->next_cpu);

			t_next->do_sleep = 0;

			trace_idle_wake_next(cpunr);

			/* wake up next core kthread */
			smp_send_reschedule(t->next_cpu);
		}
#endif
	}
	del_timer_sync(&wakeup_timer);
	clear_bit(cpunr, cpu_idling_mask);

	trace_idle_thread_stop(cpunr);

	return 0;
}

static int start_idleforce(void)
{
	unsigned long cpu;
	struct task_struct *thread;

	if (idling)
		return -EINVAL;
	idling = true;

	/* create one thread per online cpu */
	for_each_online_cpu(cpu) {
		static int long cpu_prev = -1;
		struct task_struct **p =
			per_cpu_ptr(idleforce_thread, cpu);
		struct idleforce_task_s *t =
			per_cpu_ptr((void *)idleforce_task, cpu);

		t->prev_cpu = cpu_prev;
		t->next_cpu = -1;

		/* fill next cpu */
		if (cpu_prev >= 0) {
			struct idleforce_task_s *t_prev =
				per_cpu_ptr(idleforce_task, cpu_prev);
			t_prev->next_cpu = cpu;
		}

		init_waitqueue_head(&t->queue);
		t->do_sleep = 0;

		thread = kthread_create_on_node(idleforce_kthread,
				(void *) cpu,
				cpu_to_node(cpu),
				"idleforce/%ld", cpu);
		if (unlikely(IS_ERR(thread))) {
			pr_err("failed to create kthread\n");
			return -1;
		}
		*p = thread;
		pr_info("kthread created cpu=%ld\n", cpu);

		cpu_prev = cpu;
	}

	/* start one thread per online cpu */
	for_each_online_cpu(cpu) {
		struct task_struct **p =
			per_cpu_ptr(idleforce_thread, cpu);
		struct idleforce_task_s *t =
			per_cpu_ptr((void *)idleforce_task, cpu);

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
			thread = *per_cpu_ptr(idleforce_thread, i);
			kthread_stop(thread);
			pr_info("kthread stopped\n");
		}
	}
}

static ssize_t idleforce_delay_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "wait: %d\nidle: %d\n",
			cur_wait_delay, cur_idle_delay);
}

static ssize_t idleforce_delay_store(struct kobject *kobj,
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

static ssize_t idleforce_ctrl_show(struct kobject *kobj,
			struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "state: %d\n", idling);
}

static ssize_t idleforce_ctrl_store(struct kobject *kobj,
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

static struct kobj_attribute idleforce_ctrl_attribute =
	__ATTR(ctrl, 0664, idleforce_ctrl_show, idleforce_ctrl_store);
static struct kobj_attribute idleforce_delay_attribute =
	__ATTR(delay, 0664, idleforce_delay_show, idleforce_delay_store);

static struct attribute *idleforce_attrs[] = {
	&idleforce_ctrl_attribute.attr,
	&idleforce_delay_attribute.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct attribute_group idleforce_attr_group = {
	.attrs = idleforce_attrs,
};

static inline int idleforce_create_sysfs_files(void)
{
	int retval = 0;

	sysfs_dir = kobject_create_and_add("idleforce", kernel_kobj);
	if (!sysfs_dir)
		return -ENOMEM;

	retval = sysfs_create_group(sysfs_dir, &idleforce_attr_group);
	if (retval)
		kobject_put(sysfs_dir);

	return retval;
}

static int __init idleforce_init(void)
{
	int retval = 0;
	int bitmap_size;

	bitmap_size = BITS_TO_LONGS(num_possible_cpus()) * sizeof(long);
	cpu_idling_mask = kzalloc(bitmap_size, GFP_KERNEL);
	if (!cpu_idling_mask)
		return -ENOMEM;

	idleforce_thread = alloc_percpu(struct task_struct *);
	if (!idleforce_thread) {
		retval = -ENOMEM;
		goto exit_thread;
	}

	idleforce_task = alloc_percpu(struct idleforce_task_s);
	if (!idleforce_task) {
		retval = -ENOMEM;
		goto exit_free;
	}

	retval = idleforce_create_sysfs_files();
	if (retval < 0)
		goto exit_thread_task;

	return 0;

exit_thread_task:
	free_percpu(idleforce_task);
exit_thread:
	free_percpu(idleforce_thread);
exit_free:
	kfree(cpu_idling_mask);
	return retval;
}
module_init(idleforce_init);

static void __exit idleforce_exit(void)
{
	if (sysfs_dir)
		kobject_put(sysfs_dir);

	end_idleforce();

	free_percpu(idleforce_thread);
	free_percpu(idleforce_task);
	kfree(cpu_idling_mask);
}
module_exit(idleforce_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Cycle Idle Core force");
