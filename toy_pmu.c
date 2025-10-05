// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/hrtimer.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/smp.h>
#include <linux/interrupt.h>

#define DRV_NAME "toy_pmu"
#define PMU_NAME "toy"

/* config[7:0] encodes the toy event id; we only support 0x1 = ticks */
#define TOY_EVENT_TICKS 0x1

struct toy_cpu_ctx {
	local64_t counter;          /* per-CPU monotonically increasing ticks */
	struct hrtimer timer;       /* 1ms periodic timer while active > 0 */
	atomic_t   active;          /* number of active perf events on this CPU */
};

static DEFINE_PER_CPU(struct toy_cpu_ctx, toy_cpu);
static struct pmu toy_pmu;

/* ---------- per-CPU timer ---------- */

static enum hrtimer_restart toy_hrtimer_cb(struct hrtimer *t)
{
	struct toy_cpu_ctx *c = container_of(t, struct toy_cpu_ctx, timer);

	if (atomic_read(&c->active) > 0) {
		local64_inc(&c->counter);
		hrtimer_forward_now(&c->timer, ktime_set(0, NSEC_PER_MSEC));
		return HRTIMER_RESTART;
	}
	return HRTIMER_NORESTART;
}

static void toy_cpu_start(void)
{
	struct toy_cpu_ctx *c = this_cpu_ptr(&toy_cpu);

	if (atomic_inc_return(&c->active) == 1)
		hrtimer_start(&c->timer, ktime_set(0, NSEC_PER_MSEC),
			      HRTIMER_MODE_REL_PINNED);
}

static void toy_cpu_stop(void)
{
	struct toy_cpu_ctx *c = this_cpu_ptr(&toy_cpu);

	if (atomic_dec_return(&c->active) == 0)
		hrtimer_cancel(&c->timer);
}

/* ---------- perf PMU plumbing ---------- */

static void toy_event_update(struct perf_event *event)
{
	u64 now;

	/* read this CPU's tick counter */
	now = local64_read(&get_cpu_var(toy_cpu).counter);
	put_cpu_var(toy_cpu);

	/* compute delta using hw.prev_count as our previous snapshot */
	{
		u64 prev = local64_read(&event->hw.prev_count);
		u64 delta = now - prev;

		local64_set(&event->hw.prev_count, now);
		local64_add(delta, &event->count);
	}
}

static int toy_event_init(struct perf_event *event)
{
	u64 cfg;

	if (event->attr.type != toy_pmu.type)
		return -ENOENT;

	/* counting only (no sampling) */
	if (event->attr.sample_period)
		return -EINVAL;

	/* we accept both task and CPU events */
	cfg = event->attr.config & 0xFFULL;
	if (cfg != TOY_EVENT_TICKS)
		return -EINVAL;

	/* no per-event allocations needed */
	return 0;
}

static void toy_event_start(struct perf_event *event, int flags)
{
	u64 start = local64_read(&get_cpu_var(toy_cpu).counter);
	put_cpu_var(toy_cpu);

	local64_set(&event->hw.prev_count, start);
	perf_event_update_userpage(event);

	event->hw.state = 0;

	local_irq_disable();
	toy_cpu_start();
	local_irq_enable();
}

static void toy_event_stop(struct perf_event *event, int flags)
{
	if (!(event->hw.state & PERF_HES_STOPPED)) {
		toy_event_update(event);
		event->hw.state |= PERF_HES_STOPPED;

		local_irq_disable();
		toy_cpu_stop();
		local_irq_enable();
	}
}

static int toy_event_add(struct perf_event *event, int flags)
{
	local64_set(&event->count, 0);
	event->hw.state = PERF_HES_STOPPED;

	if (flags & PERF_EF_START)
		toy_event_start(event, flags);

	return 0;
}

static void toy_event_del(struct perf_event *event, int flags)
{
	toy_event_stop(event, flags);
}

static void toy_event_read(struct perf_event *event)
{
	toy_event_update(event);
}

/* ---------- sysfs: events & format ---------- */

PMU_EVENT_ATTR_STRING(ticks, attr_ticks, "event=0x1");

static struct attribute *toy_events_attrs[] = {
	&attr_ticks.attr.attr, /* events/ticks */
	NULL,
};
static const struct attribute_group toy_events_group = {
	.name = "events",
	.attrs = toy_events_attrs,
};

PMU_FORMAT_ATTR(event, "config:0-7");

static struct attribute *toy_format_attrs[] = {
	&format_attr_event.attr, /* format/event */
	NULL,
};
static const struct attribute_group toy_format_group = {
	.name = "format",
	.attrs = toy_format_attrs,
};

static const struct attribute_group *toy_attr_groups[] = {
	&toy_events_group,
	&toy_format_group,
	NULL,
};

/* ---------- module init/exit ---------- */

static int __init toy_pmu_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct toy_cpu_ctx *c = per_cpu_ptr(&toy_cpu, cpu);
		local64_set(&c->counter, 0);
		atomic_set(&c->active, 0);
		hrtimer_init(&c->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		c->timer.function = toy_hrtimer_cb;
	}

	memset(&toy_pmu, 0, sizeof(toy_pmu));
	toy_pmu.module       = THIS_MODULE;
	toy_pmu.capabilities  = PERF_PMU_CAP_NO_EXCLUDE;
	toy_pmu.task_ctx_nr   = perf_invalid_context;
	toy_pmu.attr_groups   = toy_attr_groups;
	toy_pmu.event_init    = toy_event_init;
	toy_pmu.add           = toy_event_add;   /* int (*)() */
	toy_pmu.del           = toy_event_del;
	toy_pmu.start         = toy_event_start;
	toy_pmu.stop          = toy_event_stop;
	toy_pmu.read          = toy_event_read;

	if (perf_pmu_register(&toy_pmu, PMU_NAME, -1)) {
		pr_err(DRV_NAME ": perf_pmu_register failed\n");
		return -ENODEV;
	}
	pr_info(DRV_NAME ": registered PMU '%s' (type=%d)\n", PMU_NAME, toy_pmu.type);
	return 0;
}

static void __exit toy_pmu_exit(void)
{
	int cpu;

	perf_pmu_unregister(&toy_pmu);

	for_each_possible_cpu(cpu) {
		struct toy_cpu_ctx *c = per_cpu_ptr(&toy_cpu, cpu);
		hrtimer_cancel(&c->timer);
	}
	pr_info(DRV_NAME ": unregistered\n");
}

MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Toy PMU exposing toy/ticks/");
MODULE_LICENSE("GPL");
module_init(toy_pmu_init);
module_exit(toy_pmu_exit);

