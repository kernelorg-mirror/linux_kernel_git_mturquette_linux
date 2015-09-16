#undef TRACE_SYSTEM
#define TRACE_SYSTEM idleforce

#if !defined(_TRACE_IDLEFORCE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_IDLEFORCE_H
#include <linux/tracepoint.h>

TRACE_EVENT(idle_thread_start,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_thread_stop,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_wait_start,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_wait_end,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
		),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_start,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_wakeup,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_end,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_wait_wake_next,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

TRACE_EVENT(idle_wake_next,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d", __entry->cpu)
);

#endif

/* This part must be outside protection */
#include <trace/define_trace.h>
