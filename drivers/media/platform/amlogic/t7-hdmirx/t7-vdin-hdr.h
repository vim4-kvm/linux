/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 * PQ table and T7 HDR2 programming derived from the vendor set_hdr2_v0.c.
 * Gain/sRGB tables match tools/hdr-curve.sh tables 203 1000 in vim4-kvm.
 * Program only while the MIF is stopped, before granting hardware buffer use.
 */
static const u32 t7_hdr_pq[143] = {
	59392, 66560, 94208, 110592, 121984, 132160, 138816, 146432,
	151264, 156096, 161440, 165568, 168768, 172224, 175952, 179968,
	182240, 200680, 215102, 226400, 235271, 244625, 250984, 258029,
	264312, 269323, 275208, 280295, 284260, 288817, 294028, 297434,
	300794, 304586, 308851, 312465, 315139, 318120, 321437, 325119,
	328439, 330693, 333181, 335922, 338938, 342251, 344974, 346965,
	349143, 351524, 354124, 356960, 360050, 361931, 363762, 365751,
	367912, 370258, 372802, 375559, 377689, 379306, 381056, 382948,
	384994, 387204, 389591, 392167, 394081, 395581, 397197, 398940,
	400818, 402840, 405018, 407363, 409743, 411100, 412561, 414132,
	415820, 417636, 419588, 421685, 423939, 426172, 427472, 428869,
	430370, 431980, 433710, 435567, 437561, 439701, 441999, 443416,
	444740, 446160, 447685, 449321, 451078, 452962, 454986, 457157,
	459120, 460370, 461713, 463154, 464701, 466363, 468146, 470061,
	472118, 474326, 475917, 477191, 478560, 480031, 481611, 483309,
	485135, 487098, 489208, 491477, 492719, 494032, 495444, 496965,
	498601, 500363, 502261, 504304, 506506, 508391, 509670, 511049,
	512536, 514140, 515870, 517739, 519756, 521934, 524287,
};
static const u32 t7_hdr_gain[149] = {
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3153, 3153,
	3153, 3153, 3153, 3153, 3153, 3153, 3145, 2932,
	2704, 2503, 2328, 2175, 2040, 1815, 1635, 1487,
	1363, 1259, 1169, 1091, 1023, 910, 819, 745,
	683, 630, 585, 546, 512, 455, 410, 372,
	341, 315, 293, 273, 256, 228, 205, 186,
	171, 158, 146, 137, 128, 120, 114, 108,
	102, 98, 93, 89, 85, 82, 79, 76,
	73, 71, 68, 66, 64,
};
static const u32 t7_hdr_srgb[149] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 3, 3, 4, 5, 6,
	6, 8, 10, 11, 13, 15, 16, 18,
	19, 21, 23, 24, 26, 29, 32, 36,
	39, 42, 45, 48, 52, 58, 65, 71,
	78, 84, 90, 97, 103, 116, 129, 142,
	155, 168, 180, 192, 203, 225, 245, 264,
	282, 299, 316, 332, 347, 376, 403, 428,
	452, 475, 497, 518, 538, 577, 613, 647,
	679, 710, 739, 767, 794, 845, 894, 939,
	982, 1023, 1062, 1099, 1136, 1204, 1268, 1329,
	1386, 1441, 1493, 1543, 1591, 1683, 1768, 1849,
	1926, 1998, 2068, 2135, 2199, 2321, 2436, 2543,
	2646, 2743, 2836, 2925, 3011, 3094, 3174, 3252,
	3327, 3400, 3471, 3540, 3607, 3673, 3737, 3800,
	3861, 3921, 3980, 4038, 4095,
};

static void t7_vdin_hdr_reload(struct t7_vdin *vdin)
{
	unsigned int i;

	t7_vdin_write(vdin, 0x12a6, 0);
	for (i = 0; i < 148; i += 2)
		t7_vdin_write(vdin, 0x12a7, vdin->hdr_gain[i] | (vdin->hdr_gain[i + 1] << 16));
	t7_vdin_write(vdin, 0x12a7, vdin->hdr_gain[148]);
	t7_vdin_write(vdin, 0x12a0, 0);
	for (i = 0; i < 148; i += 2)
		t7_vdin_write(vdin, 0x12a1,
			      (vdin->hdr_oetf[i] >> 2) | ((vdin->hdr_oetf[i + 1] >> 2) << 16));
	t7_vdin_write(vdin, 0x12a1, vdin->hdr_oetf[148] >> 2);
	vdin->hdr_curve_pending = false;
}

static void t7_vdin_hdr_matrix(struct t7_vdin *vdin, u32 base, const int *coef)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		t7_vdin_write(vdin, base + i,
			      ((u32)coef[2 * i] << 16) | (coef[2 * i + 1] & 0x1fff));
	t7_vdin_write(vdin, base + 4, coef[8] & 0x1fff);
}

/* HDR10 limited BT.2020 YCbCr -> linear RGB -> BT.709 -> SDR NV12.
 * T7 uses the floating PQ EOTF and 33-bit gamut mode. The output remains
 * limited BT.709, so applications must never label these buffers as PQ.
 */
static void t7_vdin_hdr_configure(struct t7_vdin *vdin)
{
	static const int input[9] = {1196, 0, 1724, 1196, -193, -668, 1196, 2200, 0};
	static const int output[9] = {186, 627, 63, -103, -345, 448, 448, -407, -41};
	static const int gamut[9] = {425, -150, -18, -31, 290, -2, -4, -25, 286};
	unsigned int i;

	t7_vdin_hdr_matrix(vdin, 0x1282, input);
	t7_vdin_write(vdin, 0x128a, 0);
	t7_vdin_write(vdin, 0x128b, 0);
	t7_vdin_write(vdin, 0x128c, ((u32)-64 << 16) | ((-512) & 0xfff));
	t7_vdin_write(vdin, 0x128d, (-512) & 0xfff);
	t7_vdin_hdr_matrix(vdin, 0x128e, output);
	t7_vdin_write(vdin, 0x1296, 0x00400200);
	t7_vdin_write(vdin, 0x1297, 512);
	t7_vdin_write(vdin, 0x1298, 0);
	t7_vdin_write(vdin, 0x1299, 0);
	t7_vdin_write(vdin, 0x12b0, 0x18);
	for (i = 0; i < 4; i++)
		t7_vdin_write(vdin, 0x12b1 + i,
			      ((gamut[2 * i + 1] & 0xffff) << 16) |
			      (gamut[2 * i] & 0xffff));
	t7_vdin_write(vdin, 0x12b5, gamut[8]);
	t7_vdin_write(vdin, 0x12a8, 1);
	t7_vdin_write(vdin, 0x12a9, 0x04000400);
	t7_vdin_write(vdin, 0x12aa, 0x0a6a0400);
	t7_vdin_write(vdin, 0x12ab, 0);
	t7_vdin_write(vdin, 0x12ac, 0);
	t7_vdin_write(vdin, 0x12ad, 0);
	t7_vdin_write(vdin, 0x12ae, 0x04000400);
	t7_vdin_write(vdin, 0x12af, 1024);
	t7_vdin_write(vdin, 0x129e, 0);
	for (i = 0; i < 143; i++)
		t7_vdin_write(vdin, 0x129f, t7_hdr_pq[i]);
	t7_vdin_hdr_reload(vdin);
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_IN, 1);
	t7_vdin_write(vdin, VDIN_HDR_MATRIX_OUT, 1);
	t7_vdin_write(vdin, VDIN_HDR_CTRL, BIT(15) | BIT(14) | BIT(13) |
		      BIT(6) | BIT(4) | BIT(3) | BIT(2) | BIT(1));
	t7_vdin_write(vdin, VDIN_MATRIX_CTRL, 1);
}

