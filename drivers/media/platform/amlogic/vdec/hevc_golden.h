/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AML_HEVC_GOLDEN_H
#define AML_HEVC_GOLDEN_H

#include <linux/types.h>

void aml_hevc_golden_phase(unsigned int phase);
void aml_hevc_golden_event(char op, u32 reg, u32 value);
void aml_hevc_golden_access(char op, u32 reg, u32 value);

#endif
