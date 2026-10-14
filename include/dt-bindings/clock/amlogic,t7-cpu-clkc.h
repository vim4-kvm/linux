/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Amlogic T7 CPU clock controller
 */

#ifndef __T7_CPU_CLKC_H
#define __T7_CPU_CLKC_H

/* A73 cluster: sys0_pll */
#define CLKID_A73_PLL_DCO	0
#define CLKID_A73_PLL		1
#define CLKID_A73_DYN		2
#define CLKID_A73_CPU		3

/* A53 cluster: sys1_pll */
#define CLKID_A53_PLL_DCO	4
#define CLKID_A53_PLL		5
#define CLKID_A53_DYN		6
#define CLKID_A53_CPU		7

#endif /* __T7_CPU_CLKC_H */
