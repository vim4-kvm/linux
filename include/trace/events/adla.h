/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM adla

#if !defined(_ADLAK_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _ADLAK_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(adla_phase,
	    TP_PROTO(const char *phase, u64 elapsed_ns, int result, unsigned long rate),

	TP_ARGS(phase, elapsed_ns, result, rate),

	TP_STRUCT__entry(__string(phase, phase)
		__field(u64, elapsed_ns)
		__field(int, result)
		__field(unsigned long, rate)
	),

	TP_fast_assign(__assign_str(phase);
		__entry->elapsed_ns = elapsed_ns;
		__entry->result = result;
		__entry->rate = rate;
	),

	TP_printk("phase=%s elapsed_ns=%llu result=%d clk_hz=%lu",
		  __get_str(phase), __entry->elapsed_ns,
		  __entry->result, __entry->rate)
);

#endif

#include <trace/define_trace.h>
