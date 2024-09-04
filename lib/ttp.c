/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2024 Ralf Ramsauer */

#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/ttp.h>
#include <linux/uaccess.h>
#include <linux/time_namespace.h>

/* ordentlich hochschrauben, wenn gebraucht */
#define MAX_EVENTS 30000

/* wayne */
#define MAX_INPUT_SIZE 31

struct event {
	unsigned int id;
	uint64_t abstime;
};

/* per-cpu event storage */
struct ttp_storage {
	uint64_t eventcount;
	struct event *events;
};

static DEFINE_SPINLOCK(ttp_lock);

static bool ttp_enabled;

/* Array of per-cpu event storages */
static unsigned int ttp_cpus;
static struct ttp_storage *storage;

/* clock selector */
static clockid_t ttp_clock;

/* emit a timed trace point */
void ttp_emit(unsigned int id)
{
	struct ttp_storage *stor;
	struct timespec64 tp;
	unsigned int cpu_id;
	struct event ev;

	if (!ttp_enabled)
		return;

	switch (ttp_clock) {
		case CLOCK_REALTIME:
			ktime_get_real_ts64(&tp);
			break;

		case CLOCK_MONOTONIC:
			ktime_get_ts64(&tp);
			timens_add_monotonic(&tp);
			break;

		default:
			return;
	}

	ev.id = id;
	ev.abstime = tp.tv_sec * NSEC_PER_SEC + tp.tv_nsec;
	cpu_id = smp_processor_id();

	if (cpu_id >= ttp_cpus) {
		printk("ttp: FATAL\n");
		return;
	}

	stor = storage + cpu_id;
	if (stor->eventcount >= MAX_EVENTS) {
		printk("ttp: FATAL - Max Events reached\n");
		return;
	}

	stor->events[stor->eventcount++] = ev;
}

struct fpos {
	unsigned int cpu;
	uint64_t event;
};

static int ttp_open(struct inode *inode, struct file *file)
{
	struct fpos *fpos;

	fpos = kzalloc(sizeof(*fpos), GFP_KERNEL);
	if (!fpos)
		return -ENOMEM;

	file->private_data = fpos;

	return 0;
}

static int ttp_release(struct inode *inode, struct file *file)
{
	struct fpos *fpos;

	fpos = file->private_data;
	if (fpos)
		kfree(fpos);

	return 0;
}

static ssize_t
ttp_read(struct file *file, char __user *out, size_t size, loff_t *off)
{
	struct ttp_storage *stor;
	struct fpos *fpos;
	struct event *ev;
	char tmp[128];
	int ret;

	fpos = file->private_data;

	if (!storage)
		return -ENOMEM;

	if (ttp_enabled)
		return -EBUSY;

	if (size < sizeof(tmp))
		return -ENOSPC;

again:
	if (fpos->cpu == ttp_cpus)
		return 0;

	stor = storage + fpos->cpu;
	if (fpos->event == stor->eventcount) {
		fpos->event = 0;
		fpos->cpu++;
		goto again;
	}

	stor = storage + fpos->cpu;
	ev = stor->events + fpos->event;
	ret = snprintf(tmp, sizeof(tmp), "%u; %u; %llu\n", ev->id, fpos->cpu, ev->abstime);
	ret = copy_to_user(out, tmp, ret);

	fpos->event++;

	return ret;
}

static ssize_t
ttp_write(struct file *file, const char __user *in, size_t len, loff_t *off)
{
	char input[MAX_INPUT_SIZE + 1];
	unsigned int i;
	ssize_t ret;

	if (!storage)
		return -ENOMEM;

	if (len > MAX_INPUT_SIZE)
		len = MAX_INPUT_SIZE;

	if (copy_from_user(input, in, len))
		return -EFAULT;
	input[len] = 0;

	strim(input);

	spin_lock(&ttp_lock);
	if (strncmp(input, "start", len) == 0) {
		if (ttp_enabled) {
			ret = -EBUSY;
			goto unlock_out;
		}

		ttp_enabled = true;
		printk("ttp: Armed\n");
	} else if (strncmp(input, "stop", len) == 0) {
		ttp_enabled = false;
		printk("ttp: Stopped\n");
	} else if (strncmp(input, "reset", len) == 0) {
		if (ttp_enabled) {
			ret = -EINVAL;
			goto unlock_out;
		}

		for (i = 0; i < ttp_cpus; i++)
			storage[i].eventcount = 0;
		printk("ttp: Reset event storage\n");
	} else if (strncmp(input, "0", len) == 0) {
		if (ttp_enabled) {
			ret = -EBUSY;
			goto unlock_out;
		}
		ttp_clock = CLOCK_REALTIME;
		printk("ttp: using CLOCK_REALTIME\n");
	} else if (strncmp(input, "1", len) == 0) {
		if (ttp_enabled) {
			ret = -EBUSY;
			goto unlock_out;
		}
		ttp_clock = CLOCK_MONOTONIC;
		printk("ttp: using CLOCK_MONOTONIC\n");
	} else {
		ret = -EINVAL;
		goto unlock_out;
	}

	ret = len;

unlock_out:
	spin_unlock(&ttp_lock);

	return ret;
}

static const struct file_operations ttp_fops = {
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
	.open = ttp_open,
	.release = ttp_release,
	.read = ttp_read,
	.write = ttp_write,
};

static struct miscdevice ttp_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ttp",
	.fops = &ttp_fops,
};

static int __init ttp_init(void)
{
	struct ttp_storage *stor;
	unsigned int i;
	int err;

	ttp_cpus = cpumask_weight(cpu_present_mask);
	printk("ttp: allocating space for %u CPUs\n", ttp_cpus);
	storage = kzalloc(ttp_cpus * sizeof(*storage), GFP_KERNEL);
	if (!storage)
		return -ENOMEM;

	for (i = 0; i < ttp_cpus; i++) {
		stor = storage + i;
		stor->events = kzalloc(MAX_EVENTS * sizeof(*storage->events), GFP_KERNEL);
		if (!stor->events)
			return -ENOMEM; // fixme - misses unrolling
	}

	err = misc_register(&ttp_misc_dev);
	if (err)
		return err;

	return 0;
}

module_init(ttp_init);

static void __exit ttp_exit(void)
{
	struct ttp_storage *stor;
	unsigned int i;

	misc_deregister(&ttp_misc_dev);

	if (!storage)
		return;

	for (i = 0; i < ttp_cpus; i++) {
		stor = storage + i;
		kfree(stor->events);
	}
	kfree(storage);
}
module_exit(ttp_exit);
