/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved. */
/* T7 ordinary HLG SDR profile, not CUVA. See test_hlg.py for provenance. */
#include "t7-hlg-state.h"
static const u32 t7_hlg_eotf[143] = {
	0, 169296, 202068, 221184, 234837, 246442, 253952, 262485,
	267605, 273408, 279210, 282794, 286720, 290986, 295253, 297728,
	300373, 319488, 333141, 344746, 352256, 360789, 365909, 371712,
	377514, 381098, 385024, 389290, 393557, 396032, 398677, 401493,
	404480, 407637, 410282, 412032, 413866, 415786, 417792, 419882,
	422058, 424320, 426325, 427541, 428800, 430101, 431445, 432832,
	434261, 435733, 437248, 438805, 440405, 442048, 443050, 443914,
	444800, 445706, 446634, 447584, 448554, 449546, 450560, 451594,
	452650, 453728, 454826, 455946, 457088, 458250, 459093, 459696,
	460309, 460933, 461568, 462213, 462869, 463536, 464213, 464911,
	465640, 466401, 467197, 468028, 468896, 469803, 470750, 471740,
	472774, 473854, 474982, 475648, 476264, 476907, 477579, 478281,
	479014, 479780, 480580, 481416, 482289, 483201, 484154, 485150,
	486190, 487276, 488411, 489597, 490835, 491824, 492500, 493206,
	493944, 494714, 495519, 496360, 497238, 498156, 499114, 500115,
	501161, 502254, 503396, 504588, 505834, 507135, 508199, 508909,
	509651, 510426, 511236, 512081, 512965, 513888, 514852, 515859,
	516911, 518010, 519158, 520358, 521611, 522920, 524287,
};

static const u32 t7_hlg_gain[149] = {
	790, 749, 739, 729, 719, 709, 699, 690,
	681, 678, 675, 673, 671, 669, 666, 664,
	662, 659, 657, 655, 653, 651, 648, 646,
	645, 642, 639, 638, 636, 633, 631, 629,
	627, 625, 622, 620, 619, 616, 614, 612,
	610, 608, 606, 604, 602, 601, 600, 598,
	597, 596, 596, 595, 594, 593, 591, 590,
	589, 588, 588, 587, 586, 585, 583, 582,
	581, 580, 580, 579, 578, 577, 576, 575,
	574, 573, 572, 571, 570, 569, 568, 567,
	566, 565, 564, 563, 563, 561, 560, 559,
	558, 557, 556, 556, 555, 554, 553, 552,
	551, 550, 549, 548, 548, 546, 545, 544,
	543, 542, 542, 541, 540, 539, 538, 537,
	536, 535, 534, 533, 533, 532, 530, 529,
	529, 528, 527, 526, 526, 524, 523, 522,
	521, 521, 520, 519, 519, 518, 517, 517,
	516, 516, 515, 515, 514, 514, 514, 513,
	513, 512, 512, 512, 512,
};

/* Product SDR output uses the same sRGB OETF for both HDR input encodings. */
#define t7_hlg_oetf t7_hdr_srgb

static const u32 t7_hlg_cgain[65] = {
	1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
	1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
	1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
	1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
	1023, 1023, 1023, 1023, 1023, 1023, 1023, 1023,
	1023, 1023, 1023, 1023, 1037, 1048, 1059, 1086,
	1096, 1105, 1130, 1138, 1162, 1169, 1192, 1199,
	1221, 1243, 1248, 1269, 1290, 1310, 1314, 1327,
	1343,
};

static const int t7_hlg_gamut[9] = {
	3401, -1204, -149, -255, 2320, -17, -37, -206,
	2291,
};

/* Pair packing matches the 10-bit VDIN OETF and OOTF data ports. */
static void t7_hlg_write_pairs(struct t7_vdin *vdin, u32 address, u32 data,
			       const u32 *table, unsigned int count, u32 shift)
{
	unsigned int i;

	t7_vdin_write(vdin, address, 0);
	for (i = 0; i + 1 < count; i += 2)
		t7_vdin_write(vdin, data,
			      (table[i] >> shift) | ((table[i + 1] >> shift) << 16));
	t7_vdin_write(vdin, data, table[count - 1] >> shift);
}

/* Only ordinary HLG, limited YCbCr BT.2020 input, 10-bit HDR2 processing.
 * The MIF must be stopped. Enable after the complete profile is programmed.
 */
static void t7_vdin_hlg_configure(struct t7_vdin *vdin)
{
	static const int input[9] = {1196, 0, 1724, 1196, -193, -668, 1196, 2200, 0};
	static const int output[9] = {186, 627, 63, -103, -345, 448, 448, -407, -41};
	unsigned int i;

	t7_vdin_hdr_matrix(vdin, 0x1282, input);
	t7_vdin_write(vdin, 0x128a, 0);
	t7_vdin_write(vdin, 0x128b, 0);
	t7_vdin_write(vdin, 0x128c, ((u32)-64 << 16) | ((-512) & 0xfff));
	t7_vdin_write(vdin, 0x128d, (u32)-512);
	t7_vdin_hdr_matrix(vdin, 0x128e, output);
	t7_vdin_write(vdin, 0x1296, 0x00400200);
	t7_vdin_write(vdin, 0x1297, 512);
	t7_vdin_write(vdin, 0x1298, 0);
	t7_vdin_write(vdin, 0x1299, 0);
	/* T7 VDIN precise gamut uses 11 fractional bits and 33-bit OOTF. */
	t7_vdin_write(vdin, 0x12b0, BIT(4) | 11);
	for (i = 0; i < 4; i++)
		t7_vdin_write(vdin, 0x12b1 + i,
			      ((u32)(t7_hlg_gamut[2 * i + 1] & 0xffff) << 16) |
			      (t7_hlg_gamut[2 * i] & 0xffff));
	t7_vdin_write(vdin, 0x12b5, t7_hlg_gamut[8]);
	/* Ordinary HLG selects luminance, not PQ's RGB-maximum adaptive mode.
	 * x/y shifts=10, gain[148]=512 gives normalization shift=9.
	 */
	t7_vdin_write(vdin, 0x12a8, 0);
	t7_vdin_write(vdin, 0x12a9, 0x04000400);
	t7_vdin_write(vdin, 0x12aa, (10 << 24) | (9 << 20) | (10 << 16) | 1024);
	t7_vdin_write(vdin, 0x12ab, 0);
	t7_vdin_write(vdin, 0x12ac, 0);
	t7_vdin_write(vdin, 0x12ad, 0);
	t7_vdin_write(vdin, 0x12ae, (694 << 16) | 269);
	t7_vdin_write(vdin, 0x12af, 61);
	t7_vdin_write(vdin, 0x129e, 0);
	for (i = 0; i < ARRAY_SIZE(t7_hlg_eotf); i++)
		t7_vdin_write(vdin, 0x129f, t7_hlg_eotf[i]);
	t7_hlg_write_pairs(vdin, 0x12a6, 0x12a7, t7_hlg_gain, 149, 0);
	t7_hlg_write_pairs(vdin, 0x12a0, 0x12a1, t7_hlg_oetf, 149, 2);
	t7_hlg_write_pairs(vdin, 0x12a2, 0x12a3, t7_hlg_cgain, 65, 0);
	t7_vdin_write(vdin, 0x12a4, (627 * 4 << 16) | (186 * 4));
	t7_vdin_write(vdin, 0x12a5, (0x400 << 16) | (63 * 4));
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_IN, 1);
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_OUT, 1);
	t7_vdin_write(vdin, VDIN_HDR_CTRL, BIT(15) | BIT(14) | BIT(13) |
		      BIT(6) | BIT(4) | BIT(3) | BIT(2) | BIT(1) | BIT(0));
	t7_vdin_write(vdin, VDIN_MATRIX_CTRL, 1);
}
