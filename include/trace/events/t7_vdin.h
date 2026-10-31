/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM t7_vdin
#if !defined(_TRACE_T7_VDIN_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_T7_VDIN_H
#include <linux/tracepoint.h>
TRACE_EVENT(t7_vdin_buffer,

	TP_PROTO(u64 gen, u64 ticket, unsigned int index,
		 const void *allocator, u64 phys, int phase),
	TP_ARGS(gen, ticket, index, allocator, phys, phase),
	TP_STRUCT__entry(
		__field(u64, gen)
		__field(u64, ticket)
		__field(unsigned int, index)
		__field(const void *, allocator)
		__field(u64, phys)
		__field(int, phase)
	),
	TP_fast_assign(
		__entry->gen = gen;
		__entry->ticket = ticket;
		__entry->index = index;
		__entry->allocator = allocator;
		__entry->phys = phys;
		__entry->phase = phase;
	),
	TP_printk("gen=%llu ticket=%llu index=%u allocator=%p phys=%llu phase=%d",
		  __entry->gen, __entry->ticket, __entry->index,
		  __entry->allocator, __entry->phys, __entry->phase)
);
#endif
#include <trace/define_trace.h>
