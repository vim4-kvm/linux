/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM wave5_buffers
#if !defined(_TRACE_WAVE5_BUFFERS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_WAVE5_BUFFERS_H
#include <linux/tracepoint.h>
TRACE_EVENT(wave5_buffers_buffer,

	TP_PROTO(const void *allocator, u64 phys, int phase),
	TP_ARGS(allocator, phys, phase),
	TP_STRUCT__entry(__field(const void *, allocator) __field(u64, phys) __field(int, phase)),
	TP_fast_assign(
		__entry->allocator = allocator;
		__entry->phys = phys;
		__entry->phase = phase;
	),
	TP_printk("allocator=%p phys=%llu phase=%d",
		  __entry->allocator, __entry->phys, __entry->phase)
);
#endif
#include <trace/define_trace.h>
