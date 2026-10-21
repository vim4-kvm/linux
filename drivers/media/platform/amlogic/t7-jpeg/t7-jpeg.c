// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2017 Amlogic, Inc. All rights reserved. */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

#include "jpeg-tables.h"

/* DOS register numbers are word addresses within the fe320000 window. */
#define ASSIST_MMC_CTRL1 0x1002
#define MFDIN_CTRL 0x1009
#define MFDIN_CANV 0x100b
#define MFDIN_STRIDE 0x100c
#define MFDIN_Y 0x100d
#define MFDIN_DCFG 0x100e
#define MFDIN_SCMD 0x100f
#define MFDIN_SIZE 0x1010
#define MFDIN_ENDIAN 0x1011
#define MFDIN_U 0x101f
#define MFDIN_V 0x1020
#define MFDIN_UV_STRIDE 0x1021
#define ASSIST_INT0 0x1025
#define ASSIST_INT1 0x1026
#define ASSIST_INT3 0x1028
#define MBOX_IRQ 0x1078
#define MBOX_CLEAR 0x1079
#define MBOX_MASK 0x107a
#define MPSR 0x1301
#define CPSR 0x1321
#define STATUS 0x1ac0
#define MCU_XY 0x1ac1
#define VB_START 0x1d10
#define VB_END 0x1d11
#define VB_WRITE 0x1d12
#define VB_READ 0x1d14
#define VB_CONTROL 0x1d16
#define VB_MEM_CONTROL 0x1d17
#define TOTAL_BYTES 0x1d1a
#define VLC_INT_CONTROL 0x1d30
#define PIC_SIZE 0x1d31
#define PIC_POSITION 0x1d33
#define VLC_JPEG_CONTROL 0x1d58
#define HUFFMAN_ADDR 0x1d5a
#define HUFFMAN_DATA 0x1d5b
#define MB_CONTROL 0x1f15
#define QDCT_JPEG_CONTROL 0x1f25
#define X_START_END 0x1f26
#define Y_START_END 0x1f27
#define QUANT_ADDR 0x1f28
#define QUANT_DATA 0x1f29
#define DOS_GCLK_EN0 0x3f01
#define DOS_SW_RESET1 0x3f07
#define DOS_MEM_PD_HCODEC 0x3f32

#define JPEG_IOCTL(n) _IOW('J', (n), u32)
#define JPEG_DONE 4
#define INPUT_SIZE PAGE_ALIGN(4096 * 2160 * 3 / 2)
#define HEADER_OFFSET INPUT_SIZE
#define HEADER_SIZE PAGE_SIZE
#define OUTPUT_OFFSET (HEADER_OFFSET + HEADER_SIZE)
#define OUTPUT_SIZE SZ_16M
#define MAPPING_SIZE (OUTPUT_OFFSET + OUTPUT_SIZE)
#define ENGINE_RESET (BIT(2) | BIT(6) | BIT(7) | BIT(8) | BIT(14) | BIT(16) | BIT(17))

struct t7_jpeg {
	struct device *dev;
	void __iomem *regs;
	struct clk_bulk_data *clocks;
	int num_clocks, irq;
	struct miscdevice misc;
	struct mutex lock;
	wait_queue_head_t wait;
	bool opened, powered, failed;
	u32 stage, bytes;
};

struct t7_jpeg_file {
	struct t7_jpeg *jpeg;
	void *mapping;
	dma_addr_t dma;
	struct dma_buf *input;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	u32 header_bytes;
	bool running;
};

static u32 rd(struct t7_jpeg *j, u32 reg)
{
	return readl(j->regs + reg * 4);
}

static void wr(struct t7_jpeg *j, u32 reg, u32 value)
{
	writel(value, j->regs + reg * 4);
}

static void reset_engine(struct t7_jpeg *j, u32 mask)
{
	int i;

	for (i = 0; i < 3; i++)
		rd(j, DOS_SW_RESET1);
	wr(j, DOS_SW_RESET1, mask);
	wr(j, DOS_SW_RESET1, 0);
	for (i = 0; i < 3; i++)
		rd(j, DOS_SW_RESET1);
}

static irqreturn_t jpeg_irq(int irq, void *data)
{
	struct t7_jpeg *j = data;
	u32 stage;

	if (!READ_ONCE(j->powered) || !rd(j, MBOX_IRQ))
		return IRQ_NONE;
	wr(j, MBOX_CLEAR, 1);
	stage = rd(j, STATUS);
	if (stage == JPEG_DONE) {
		WRITE_ONCE(j->bytes, rd(j, TOTAL_BYTES));
		/* Publish byte count before poll observes completion. */
		smp_store_release(&j->stage, stage);
		wake_up_interruptible(&j->wait);
	}
	return IRQ_HANDLED;
}

static int power_on(struct t7_jpeg *j)
{
	struct arm_smccc_res result;
	int ret;

	dev_dbg(j->dev, "enabling clocks\n");
	ret = clk_bulk_prepare_enable(j->num_clocks, j->clocks);
	if (ret)
		return ret;
	dev_dbg(j->dev, "powering HCODEC domain on\n");
	ret = pm_runtime_resume_and_get(j->dev);
	if (ret < 0) {
		clk_bulk_disable_unprepare(j->num_clocks, j->clocks);
		return ret;
	}
	WRITE_ONCE(j->powered, true);
	dev_dbg(j->dev, "domain on, core clock %lu Hz\n", clk_get_rate(j->clocks[2].clk));
	wr(j, DOS_MEM_PD_HCODEC, 0);
	wr(j, DOS_GCLK_EN0, rd(j, DOS_GCLK_EN0) | GENMASK(26, 12));
	wr(j, ASSIST_MMC_CTRL1, 0x32);
	wr(j, MPSR, 0);
	wr(j, CPSR, 0);
	/* T7 boot firmware owns the signed JPEG microcode. Firmware index 21,
	 * HCODEC core 3 are the secure video-loader ABI, independent of Linux. */
	dev_dbg(j->dev, "loading secure JPEG firmware\n");
	arm_smccc_smc(ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32,
				       ARM_SMCCC_OWNER_TRUSTED_OS, 15),
		      21, 3, 0, 0, 0, 0, 0, &result);
	if ((s32)result.a0) {
		dev_err(j->dev, "secure JPEG firmware load failed: %#lx\n", result.a0);
		return -EIO;
	}
	wr(j, MBOX_MASK, 0);
	wr(j, MBOX_CLEAR, 1);
	dev_dbg(j->dev, "JPEG firmware ready\n");
	return 0;
}

static int power_off(struct t7_jpeg *j)
{
	int ret;

	if (!j->powered)
		return 0;
	wr(j, MBOX_MASK, 0);
	wr(j, MPSR, 0);
	wr(j, CPSR, 0);
	reset_engine(j, ENGINE_RESET | BIT(12) | BIT(11));
	WRITE_ONCE(j->powered, false);
	synchronize_irq(j->irq);
	/* Complete the secure power-off before disabling its functional clocks.
	 * On failure retain every DMA allocation/import rather than recycle it. */
	ret = pm_runtime_put_sync_suspend(j->dev);
	if (ret < 0) {
		j->failed = true;
		dev_crit(j->dev, "HCODEC power-off failed; DMA quarantined: %d\n", ret);
		return ret;
	}
	clk_bulk_disable_unprepare(j->num_clocks, j->clocks);
	return 0;
}

static void release_input(struct t7_jpeg_file *f)
{
	if (!f->input)
		return;
	dma_buf_unmap_attachment_unlocked(f->attachment, f->sgt, DMA_TO_DEVICE);
	dma_buf_detach(f->input, f->attachment);
	dma_buf_put(f->input);
	f->sgt = NULL;
	f->attachment = NULL;
	f->input = NULL;
}

static int import_input(struct t7_jpeg_file *f, int fd)
{
	struct t7_jpeg *j = f->jpeg;
	struct dma_buf *input;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct scatterlist *sg;
	dma_addr_t end;
	unsigned int i;
	int ret;

	if (f->input || f->running)
		return -EBUSY;
	input = dma_buf_get(fd);
	if (IS_ERR(input))
		return PTR_ERR(input);
	ret = dma_resv_wait_timeout(input->resv, DMA_RESV_USAGE_WRITE, true, HZ);
	if (ret <= 0) {
		ret = ret ? ret : -ETIMEDOUT;
		goto put;
	}
	attach = dma_buf_attach(input, j->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto put;
	}
	sgt = dma_buf_map_attachment_unlocked(attach, DMA_TO_DEVICE);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto detach;
	}
	end = sg_dma_address(sgt->sgl);
	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (sg_dma_address(sg) != end) {
			ret = -EINVAL;
			goto unmap;
		}
		end += sg_dma_len(sg);
	}
	if (end > (1ULL << 32)) {
		ret = -ERANGE;
		goto unmap;
	}
	f->input = input;
	f->attachment = attach;
	f->sgt = sgt;
	return 0;
unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_TO_DEVICE);
detach:
	dma_buf_detach(input, attach);
put:
	dma_buf_put(input);
	return ret;
}

static const u8 zigzag_order[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static u8 *put16(u8 *p, u16 value)
{
	*p++ = value >> 8;
	*p++ = value;
	return p;
}

static u32 make_header(u8 *header, u32 width, u32 height, u16 quant[2][64])
{
	u8 *p = header;
	int table, i;

	p = put16(p, 0xffd8);
	p = put16(p, 0xffdb);
	p = put16(p, 132);
	for (table = 0; table < 2; table++) {
		*p++ = table;
		for (i = 0; i < 64; i++)
			*p++ = quant[table][zigzag_order[i]];
	}
	p = put16(p, 0xffc4);
	p = put16(p, 418);
	for (table = 0; table < 2; table++) {
		*p++ = table;
		memcpy(p, jpeg_huffman_dc[table], 28);
		p += 28;
	}
	for (table = 0; table < 2; table++) {
		*p++ = 0x10 | table;
		memcpy(p, jpeg_huffman_ac[table], 178);
		p += 178;
	}
	p = put16(p, 0xffc0);
	p = put16(p, 17);
	*p++ = 8;
	p = put16(p, height);
	p = put16(p, width);
	*p++ = 3;
	for (table = 0; table < 3; table++) {
		*p++ = table;
		*p++ = table ? 0x11 : 0x22;
		*p++ = !!table;
	}
	/* The firmware bitstream begins at a 64-bit boundary. Fill bytes belong
	 * before SOS, never inside the entropy-coded scan. */
	while ((p - header + 14) & 7)
		*p++ = 0xff;
	p = put16(p, 0xffda);
	p = put16(p, 12);
	*p++ = 3;
	for (table = 0; table < 3; table++) {
		*p++ = table;
		*p++ = table ? 0x11 : 0;
	}
	*p++ = 0;
	*p++ = 63;
	*p++ = 0;
	return p - header;
}

static void write_huffman(struct t7_jpeg *j, const u8 *table, bool ac)
{
	u32 lut[162] = {}, code = 0;
	unsigned int length, i, pos = 16, address, value;

	for (length = 1; length <= 16; length++) {
		for (i = 0; i < table[length - 1]; i++) {
			value = table[pos++];
			if (!ac)
				address = value;
			else if (!value)
				address = 0;
			else if (value == 0xf0)
				address = 161;
			else
				address = 1 + 16 * ((value & 15) - 1) + (value >> 4);
			lut[address] = ((length - 1) << 16) | code++;
		}
		code <<= 1;
	}
	for (i = 0; i < (ac ? 162 : 12); i++)
		wr(j, HUFFMAN_DATA, lut[i]);
}

static void configure_jpeg(struct t7_jpeg_file *f, const u32 cmd[30])
{
	struct t7_jpeg *j = f->jpeg;
	u32 width = cmd[3], height = cmd[4], quality = cmd[7];
	u32 scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	u16 quant[2][64];
	int table, i;
	u32 lo, hi;

	for (table = 0; table < 2; table++)
		for (i = 0; i < 64; i++)
			quant[table][i] = clamp((jpeg_quant[table][i] * scale + 50) / 100, 1u, 255u);
	f->header_bytes = make_header(f->mapping + HEADER_OFFSET, width, height, quant);
	wr(j, PIC_SIZE, width | height << 16);
	wr(j, PIC_POSITION, 3 | BIT(4));
	wr(j, X_START_END, (width - 1) << 16);
	wr(j, Y_START_END, (height - 1) << 16);
	wr(j, QUANT_ADDR, 0);
	for (table = 0; table < 2; table++)
		for (i = 0; i < 64; i += 2) {
			lo = min(DIV_ROUND_CLOSEST(65536u, quant[table][i]), 65535u);
			hi = min(DIV_ROUND_CLOSEST(65536u, quant[table][i + 1]), 65535u);
			wr(j, QUANT_DATA, lo | hi << 16);
		}
	wr(j, HUFFMAN_ADDR, 0);
	write_huffman(j, jpeg_huffman_dc[0], false);
	write_huffman(j, jpeg_huffman_dc[1], false);
	wr(j, HUFFMAN_ADDR, 24);
	write_huffman(j, jpeg_huffman_ac[0], true);
	write_huffman(j, jpeg_huffman_ac[1], true);
	/* 4:2:0: four luma blocks, one block for each chroma component. */
	wr(j, QDCT_JPEG_CONTROL, BIT(16) | BIT(15) | BIT(12) | BIT(11) |
		BIT(8) | BIT(6) | BIT(5) | BIT(4) | BIT(0));
	wr(j, VLC_JPEG_CONTROL, BIT(29) | BIT(28) | BIT(24) | BIT(21) |
		BIT(20) | BIT(16) | (3 << 9) | BIT(8));
	wr(j, MB_CONTROL, BIT(9) | BIT(0));
	wr(j, MB_CONTROL, BIT(13) | BIT(12));
	wr(j, TOTAL_BYTES, 0);
	wr(j, VLC_INT_CONTROL, 0);
	wr(j, STATUS, 0);
	wr(j, MCU_XY, 0xffffffff);
	wr(j, ASSIST_INT0, 0x15);
	wr(j, ASSIST_INT1, 8);
	wr(j, ASSIST_INT3, 0x14);
}

static int start_frame(struct t7_jpeg_file *f, const u32 cmd[30])
{
	struct t7_jpeg *j = f->jpeg;
	u32 width = cmd[3], height = cmd[4], stride = cmd[11];
	u64 uv_offset = (u64)stride * cmd[14];
	u64 required = uv_offset + (u64)stride * height / 2;
	dma_addr_t input = f->input ? sg_dma_address(f->sgt->sgl) : f->dma;
	u64 capacity = f->input ? f->input->size : INPUT_SIZE;

	if (!j->powered)
		return -EPIPE;
	if (f->running)
		return -EBUSY;
	/* Only linear NV12/NV21 with a single bounded allocation is supported.
	 * No user-supplied physical address or canvas identifier reaches DMA. */
	if (cmd[0] || (cmd[1] != 2 && cmd[1] != 3) || cmd[2] != 4 ||
	    width < 16 || width > 4096 || (width & 15) ||
	    height < 16 || height > 2160 || (height & 1) ||
	    !cmd[7] || cmd[7] > 100 || stride < width || stride > 65535 ||
	    stride != cmd[12] || stride != cmd[13] || cmd[14] < height ||
	    required > capacity || required > cmd[5] || cmd[5] > capacity ||
	    input + required > (1ULL << 32))
		return -EINVAL;
	reset_engine(j, ENGINE_RESET);
	wr(j, MFDIN_SIZE, width << 16 | height);
	wr(j, MFDIN_CTRL, cmd[1] | BIT(13) | (2 << 29));
	wr(j, MFDIN_CANV, BIT(28) | BIT(26) | BIT(24));
	wr(j, MFDIN_STRIDE, stride);
	wr(j, MFDIN_Y, input);
	wr(j, MFDIN_U, input + uv_offset);
	wr(j, MFDIN_V, input + uv_offset);
	wr(j, MFDIN_UV_STRIDE, stride << 16 | stride);
	wr(j, MFDIN_DCFG, (rd(j, MFDIN_DCFG) & 0x3ff) | BIT(16));
	wr(j, MFDIN_ENDIAN, 7 | 6 << 3 | 5 << 6 | 4 << 9 | 3 << 12 | 2 << 15 | 1 << 18);
	wr(j, MFDIN_SCMD, rd(j, MFDIN_SCMD) | BIT(28));
	configure_jpeg(f, cmd);
	wr(j, VB_MEM_CONTROL, BIT(31) | 0x3f << 24 | 0x20 << 16 | 2);
	wr(j, VB_START, f->dma + OUTPUT_OFFSET);
	wr(j, VB_WRITE, f->dma + OUTPUT_OFFSET);
	wr(j, VB_READ, f->dma + OUTPUT_OFFSET);
	wr(j, VB_END, f->dma + MAPPING_SIZE - 1);
	wr(j, VB_CONTROL, 1);
	wr(j, VB_CONTROL, 7 << 3 | BIT(1));
	wr(j, MBOX_CLEAR, 1);
	WRITE_ONCE(j->bytes, 0);
	WRITE_ONCE(j->stage, 0);
	f->running = true;
	wr(j, MBOX_MASK, 0xffffffff);
	reset_engine(j, BIT(12) | BIT(11));
	/* Header/input writes must be visible before the firmware starts DMA. */
	dma_wmb();
	wr(j, MPSR, 1);
	return 0;
}

static long jpeg_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct t7_jpeg_file *f = file->private_data;
	struct t7_jpeg *j = f->jpeg;
	void __user *user = (void __user *)arg;
	u32 data[30] = {}, stage;
	int fd, ret = 0;

	if (mutex_lock_interruptible(&j->lock))
		return -ERESTARTSYS;
	if (j->failed) {
		ret = -EIO;
		goto out;
	}
	stage = smp_load_acquire(&j->stage);
	switch (command) {
	case JPEG_IOCTL(0):
		data[0] = MAPPING_SIZE;
		data[1] = 0;
		data[2] = INPUT_SIZE;
		data[3] = HEADER_OFFSET;
		data[4] = HEADER_SIZE;
		data[5] = OUTPUT_OFFSET;
		data[6] = OUTPUT_SIZE;
		ret = copy_to_user(user, data, 7 * sizeof(u32)) ? -EFAULT : 0;
		break;
	case JPEG_IOCTL(1):
		if (j->powered) {
			ret = -EBUSY;
			break;
		}
		ret = power_on(j);
		if (ret)
			power_off(j);
		break;
	case JPEG_IOCTL(2):
		if (copy_from_user(data, user, sizeof(data)))
			ret = -EFAULT;
		else
			ret = start_frame(f, data);
		break;
	case JPEG_IOCTL(3):
		ret = put_user(stage, (u32 __user *)user);
		break;
	case JPEG_IOCTL(4):
		if (stage != JPEG_DONE || !j->bytes || j->bytes >= OUTPUT_SIZE) {
			ret = -EIO;
			break;
		}
		dma_rmb();
		data[0] = f->header_bytes;
		data[1] = j->bytes;
		ret = copy_to_user(user, data, 2 * sizeof(u32)) ? -EFAULT : 0;
		/* The firmware's DONE handshake ends input reads. Reset the input
		 * engine before its DMA mapping can be released/reused. */
		wr(j, MBOX_MASK, 0);
		wr(j, MPSR, 0);
		wr(j, CPSR, 0);
		reset_engine(j, ENGINE_RESET);
		synchronize_irq(j->irq);
		f->running = false;
		break;
	case JPEG_IOCTL(7):
		if (get_user(fd, (int __user *)user))
			ret = -EFAULT;
		else
			ret = import_input(f, fd);
		break;
	case JPEG_IOCTL(8):
		if (f->running)
			ret = -EBUSY;
		else
			release_input(f);
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&j->lock);
	return ret;
}

static __poll_t jpeg_poll(struct file *file, poll_table *wait)
{
	struct t7_jpeg_file *f = file->private_data;
	struct t7_jpeg *j = f->jpeg;

	poll_wait(file, &j->wait, wait);
	if (READ_ONCE(j->failed))
		return EPOLLERR;
	return smp_load_acquire(&j->stage) == JPEG_DONE ? EPOLLIN | EPOLLRDNORM : 0;
}

static int jpeg_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct t7_jpeg_file *f = file->private_data;

	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != MAPPING_SIZE)
		return -EINVAL;
	return dma_mmap_wc(f->jpeg->dev, vma, f->mapping, f->dma, MAPPING_SIZE);
}

static int jpeg_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct t7_jpeg *j = container_of(misc, struct t7_jpeg, misc);
	struct t7_jpeg_file *f;
	int ret = 0;

	mutex_lock(&j->lock);
	if (j->failed || j->opened) {
		ret = j->failed ? -EIO : -EBUSY;
		goto out;
	}
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		ret = -ENOMEM;
		goto out;
	}
	f->mapping = dma_alloc_wc(j->dev, MAPPING_SIZE, &f->dma, GFP_KERNEL);
	if (!f->mapping) {
		kfree(f);
		ret = -ENOMEM;
		goto out;
	}
	f->jpeg = j;
	j->stage = 0;
	j->bytes = 0;
	j->opened = true;
	file->private_data = f;
out:
	mutex_unlock(&j->lock);
	return ret;
}

static int jpeg_release(struct inode *inode, struct file *file)
{
	struct t7_jpeg_file *f = file->private_data;
	struct t7_jpeg *j = f->jpeg;

	mutex_lock(&j->lock);
	if (j->failed || power_off(j)) {
		/* Prevent module removal while quarantined DMA still has an owner. */
		__module_get(THIS_MODULE);
		mutex_unlock(&j->lock);
		return 0;
	}
	release_input(f);
	dma_free_wc(j->dev, MAPPING_SIZE, f->mapping, f->dma);
	kfree(f);
	j->opened = false;
	mutex_unlock(&j->lock);
	return 0;
}

static const struct file_operations jpeg_fops = {
	.owner = THIS_MODULE,
	.open = jpeg_open,
	.release = jpeg_release,
	.unlocked_ioctl = jpeg_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.mmap = jpeg_mmap,
	.poll = jpeg_poll,
};

static int jpeg_probe(struct platform_device *pdev)
{
	struct t7_jpeg *j;
	int ret;

	j = devm_kzalloc(&pdev->dev, sizeof(*j), GFP_KERNEL);
	if (!j)
		return -ENOMEM;
	j->dev = &pdev->dev;
	j->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(j->regs))
		return PTR_ERR(j->regs);
	ret = dma_set_mask_and_coherent(j->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = devm_clk_bulk_get_all(j->dev, &j->clocks);
	if (ret < 0)
		return ret;
	j->num_clocks = ret;
	if (ret != 3)
		return -EINVAL;
	mutex_init(&j->lock);
	init_waitqueue_head(&j->wait);
	j->irq = platform_get_irq(pdev, 0);
	if (j->irq < 0)
		return j->irq;
	ret = devm_request_irq(j->dev, j->irq, jpeg_irq, 0, "t7-jpeg", j);
	if (ret)
		return ret;
	pm_runtime_enable(j->dev);
	j->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "jpegenc",
		.fops = &jpeg_fops,
		.parent = j->dev,
	};
	platform_set_drvdata(pdev, j);
	ret = misc_register(&j->misc);
	if (ret)
		pm_runtime_disable(j->dev);
	return ret;
}

static void jpeg_remove(struct platform_device *pdev)
{
	struct t7_jpeg *j = platform_get_drvdata(pdev);

	misc_deregister(&j->misc);
	pm_runtime_disable(j->dev);
}

static const struct of_device_id jpeg_match[] = {
	{ .compatible = "amlogic,t7-jpeg-encoder" },
	{}
};
MODULE_DEVICE_TABLE(of, jpeg_match);

static struct platform_driver jpeg_driver = {
	.probe = jpeg_probe,
	.remove = jpeg_remove,
	.driver = {
		.name = "t7-jpeg",
		.of_match_table = jpeg_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(jpeg_driver);

MODULE_DESCRIPTION("Amlogic T7 HCODEC JPEG encoder");
MODULE_AUTHOR("simon.zheng <simon.zheng@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
