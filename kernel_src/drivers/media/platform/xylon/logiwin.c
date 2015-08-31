/*
 * Xylon logiWIN frame grabber core
 *
 * Copyright (C) 2014 Xylon d.o.o.
 * Author: Davor Joja <davor.joja@logicbricks.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/io.h>
#include <linux/delay.h>

#include "logiwin.h"

/* logiWIN registers */
#define LOGIWIN_REG_STRIDE		8
#define LOGIWIN_DR_X_ROFF		(0  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_DR_Y_ROFF		(1  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_UL_X_ROFF		(2  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_UL_Y_ROFF		(3  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_SCALE_X_ROFF		(4  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_SCALE_Y_ROFF		(5  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_CTRL0_ROFF		(6  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_START_X_ROFF		(7  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_START_Y_ROFF		(8  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_CROP_X_ROFF		(9  * LOGIWIN_REG_STRIDE)
#define LOGIWIN_CROP_Y_ROFF		(10 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_MEM_OFFSET_EVEN_ROFF	(11 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_MEM_OFFSET_ODD_ROFF	(12 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_PIX_ALPHA_ROFF		(13 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_CONTRAST_ROFF		(15 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_SATURATION_ROFF		(16 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_BRIGHTNESS_ROFF		(17 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_COS_HUE_ROFF		(18 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_SIN_HUE_ROFF		(19 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_VBUFF_SWITCH_ROFF	(20 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_INT_STAT_ROFF		(22 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_INT_MASK_ROFF		(23 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_IP_VER_ROFF		(24 * LOGIWIN_REG_STRIDE)
#define LOGIWIN_RESOLUTION_ROFF		(25 * LOGIWIN_REG_STRIDE)

#define LOGIWIN_MASK_BRAM_OFFSET	(0x200 * LOGIWIN_REG_STRIDE)

/* Control Register Bits */
#define LOGIWIN_CTRL_ENABLE			(1 << 0)
#define LOGIWIN_CTRL_EVEN_FIELD_VBUFF_SWITCH	(1 << 2)
#define LOGIWIN_CTRL_WEAVE_DEINTERLACE		(1 << 3)
#define LOGIWIN_CTRL_INPUT_SELECT		(1 << 4)
#define LOGIWIN_CTRL_STENCIL_MASK		(1 << 5)
#define LOGIWIN_CTRL_CPU_VBUFF_SWITCH		(1 << 8)
#define LOGIWIN_CTRL_FRAME_STORE_STOP		(1 << 9)
#define LOGIWIN_CTRL_SWIZZLE			(1 << 11)

#define LOGIWIN_FRAME_RATE_MASK_FULL	0x3F
#define LOGIWIN_FRAME_RATE_MASK_75	0x40
#define LOGIWIN_FRAME_RATE_MASK_50	0x80
#define LOGIWIN_FRAME_RATE_MASK_25	0xC0

#define LOGIWIN_HSYNC_INVERT_CH_0	0x1000
#define LOGIWIN_VSYNC_INVERT_CH_0	0x2000
#define LOGIWIN_HSYNC_INVERT_CH_1	0x4000
#define LOGIWIN_VSYNC_INVERT_CH_1	0x8000

/* Scale step constants */
#define SCALE_STEP_MIN			(1 << 10)
#define SCALE_STEP_MAX			((1 << 20) - 1)
#define SCALE_FRAC_MASK			(SCALE_STEP - 1)

/* Interpolation starting points */
#define START_X				0
#define START_Y				0
#define START_X_HALF			(SCALE_STEP >> 1)
#define START_Y_HALF			(SCALE_STEP >> 1)

/* Register access functions */
static inline unsigned long logiwin_read32(struct logiwin_parameters *lw_par,
					   unsigned int offset)
{
	void __iomem *base = lw_par->base;

	if (lw_par->hw_access)
		return readl(base + offset);
	else
		return 0;
}

static inline void logiwin_write32(struct logiwin_parameters *lw_par,
				   unsigned int offset, unsigned int val)
{
	void __iomem *base = lw_par->base;

	if (lw_par->hw_access)
		writel(val, (base + offset));
}

/**
 * Enable/disable logiWIN operation
 *
 * @lw_par:	logiWIN data
 * @op:		logiWIN operation
 * @op_flag:	logiWIN operation flag
 *
 */
void logiwin_operation(struct logiwin_parameters *lw_par,
		       enum logiwin_operation op,
		       enum logiwin_operation_flag op_flag)
{
	u32 op_mask;
	unsigned int us = 0;

	switch (op) {
	case LOGIWIN_OP_ENABLE:
		op_mask = LOGIWIN_CTRL_ENABLE;
		break;
	case LOGIWIN_OP_EVEN_FIELD_VBUFF_SWITCH:
		op_mask = LOGIWIN_CTRL_EVEN_FIELD_VBUFF_SWITCH;
		break;
	case LOGIWIN_OP_STENCIL_MASK:
		op_mask = LOGIWIN_CTRL_STENCIL_MASK;
		break;
	case LOGIWIN_OP_CPU_VBUFF_SWITCH:
		op_mask = LOGIWIN_CTRL_CPU_VBUFF_SWITCH;
		break;
	case LOGIWIN_OP_FRAME_STORED_STOP:
		op_mask = LOGIWIN_CTRL_FRAME_STORE_STOP;
		us = 10;
		break;
	case LOGIWIN_OP_SWIZZLE:
		op_mask = LOGIWIN_CTRL_SWIZZLE;
		us = 10;
		break;
	}

	if (op_flag == LOGIWIN_OP_FLAG_ENABLE)
		lw_par->ctrl |= op_mask;
	else if (op_flag == LOGIWIN_OP_FLAG_DISABLE)
		lw_par->ctrl &= ~op_mask;

	if (us != 0 && (lw_par->ctrl & LOGIWIN_CTRL_ENABLE)) {
		lw_par->ctrl &= ~LOGIWIN_CTRL_ENABLE;
		logiwin_write32(lw_par, LOGIWIN_CTRL0_ROFF, lw_par->ctrl);

		udelay(us);

		lw_par->ctrl |= LOGIWIN_CTRL_ENABLE;
	}
	logiwin_write32(lw_par, LOGIWIN_CTRL0_ROFF, lw_par->ctrl);
}

/**
 * Enable/disable logiWIN deinterlacing
 *
 * @lw_par:	logiWIN data
 * @enable:	enable/disable flag
 *
 * Note:
 *	false:	"Bob"
 *	true:	"Weave"
 *
 */
void logiwin_weave_deinterlace(struct logiwin_parameters *lw_par,
			       bool weave_deinterlace)
{
	if (lw_par->input_format != LOGIWIN_FORMAT_INPUT_ITU)
		return;

	if (weave_deinterlace) {
		lw_par->ctrl |= LOGIWIN_CTRL_WEAVE_DEINTERLACE;
		lw_par->out.top /= 2;
		lw_par->out.height /= 2;
		lw_par->vscale_step *= 2;
	} else {
		lw_par->ctrl &= ~LOGIWIN_CTRL_WEAVE_DEINTERLACE;
		lw_par->out.top *= 2;
		lw_par->out.height *= 2;
		lw_par->vscale_step = (lw_par->vscale_step / 2) &
				      ((~0) << lw_par->scale_shift);
	}
	lw_par->weave_deinterlace = weave_deinterlace;

	if (lw_par->ctrl & LOGIWIN_CTRL_ENABLE)	{
		lw_par->ctrl &= ~LOGIWIN_CTRL_ENABLE;
		logiwin_write32(lw_par, LOGIWIN_CTRL0_ROFF, lw_par->ctrl);

		udelay(10);

		lw_par->ctrl |= LOGIWIN_CTRL_ENABLE;
	}
	logiwin_write32(lw_par, LOGIWIN_CTRL0_ROFF, lw_par->ctrl);
}

/**
 * Input video channel selection
 *
 * @lw_par:	logiWIN data
 * @ch:		channel ID (0, 1)
 *
 */
void logiwin_select_input_ch(struct logiwin_parameters *lw_par, unsigned int ch)
{
	if (ch == 0)
		lw_par->ctrl &= ~LOGIWIN_CTRL_INPUT_SELECT;
	else if (ch == 1)
		lw_par->ctrl |= LOGIWIN_CTRL_INPUT_SELECT;
	else
		return;

	lw_par->channel_id = ch;
	logiwin_write32(lw_par, LOGIWIN_CTRL0_ROFF, lw_par->ctrl);
}

/**
 * Set logiWIN frame rate
 *
 * @lw_par:	logiWIN data
 * @frame_rate:	frame rate
 *
 * Note:
 *	Frame rate smaller than full instructs logiWIN to store 75%, 50% or 25%
 *	of total frames per second.
 *
 */
void logiwin_set_frame_rate(struct logiwin_parameters *lw_par,
			    enum logiwin_frame_rate frame_rate)
{
	lw_par->ctrl &= LOGIWIN_FRAME_RATE_MASK_FULL;

	switch(frame_rate) {
	case LOGIWIN_FRAME_RATE_75:
		lw_par->ctrl |= LOGIWIN_FRAME_RATE_MASK_75;
		break;
	case LOGIWIN_FRAME_RATE_50:
		lw_par->ctrl |= LOGIWIN_FRAME_RATE_MASK_50;
		break;
	case LOGIWIN_FRAME_RATE_25:
		lw_par->ctrl |= LOGIWIN_FRAME_RATE_MASK_25;
		break;
	case LOGIWIN_FRAME_RATE_FULL:
	default:
		break;
	}

	logiwin_write32(lw_par, LOGIWIN_CTRL0_ROFF, lw_par->ctrl);
}

/**
 *
 * Switch H sync and V sync polarity
 *
 * @lw_par:	logiWIN data
 * @ch:		input channel
 * @hsync_inv:	switch H sync polarity
 * @vsync_inv:	switch V sync polarity
 *
 */
void logiwin_sync_polarity(struct logiwin_parameters *lw_par,
			   unsigned int ch, bool hsync_inv, bool vsync_inv)
{
	if (ch > 1)
		return;

	if (ch == 0) {
		if (hsync_inv)
			lw_par->ctrl |= LOGIWIN_HSYNC_INVERT_CH_0;
		else
			lw_par->ctrl &= ~LOGIWIN_HSYNC_INVERT_CH_0;
		if (vsync_inv)
			lw_par->ctrl |= LOGIWIN_VSYNC_INVERT_CH_0;
		else
			lw_par->ctrl &= ~LOGIWIN_VSYNC_INVERT_CH_0;
	} else if (ch == 1) {
		if (hsync_inv)
			lw_par->ctrl |= LOGIWIN_HSYNC_INVERT_CH_1;
		else
			lw_par->ctrl &= ~LOGIWIN_HSYNC_INVERT_CH_1;
		if (vsync_inv)
			lw_par->ctrl |= LOGIWIN_VSYNC_INVERT_CH_1;
		else
			lw_par->ctrl &= ~LOGIWIN_VSYNC_INVERT_CH_1;
	}

	logiwin_write32(lw_par, LOGIWIN_CTRL0_ROFF, lw_par->ctrl);
}

/**
 * Read logiWIN resolution change register
 *
 * @lw_par:	logiWIN data
 * @hres:	horizontal resolution
 * @vres:	vertical resolution
 *
 */
void logiwin_get_resolution(struct logiwin_parameters *lw_par,
			    u32 *hres, u32 *vres)
{
	u32 res = logiwin_read32(lw_par, LOGIWIN_RESOLUTION_ROFF);

	*hres = res & 0xFFFF;
	*vres = res >> 16;
}

/**
 * Set logiWIN memory offsets
 *
 * @lw_par:	logiWIN data
 * @even_ptr:	memory pointer to even field of the video input stream
 * @odd_ptr:	memory pointer to odd field of the video input stream
 *
 * Note:
 *	For VGA or ITU656 in "bob" deinterlace mode video inputs, only even_ptr
 *	is used.
 *	For ITU656 video input in "weave" deinterlace mode, additionally odd_ptr
 *	is used.
 *	even_ptr defines pointer to the block of video memory where even fields
 *	are stored.
 *	odd_ptr defines pointer to the block of video memory where odd fields
 *	are stored.
 *
 */
void logiwin_set_memory_offset(struct logiwin_parameters *lw_par,
			       u32 even_ptr, u32 odd_ptr)
{
	logiwin_write32(lw_par, LOGIWIN_MEM_OFFSET_EVEN_ROFF, even_ptr);
	logiwin_write32(lw_par, LOGIWIN_MEM_OFFSET_ODD_ROFF, odd_ptr);
}

/**
 * Set logiWIN pixel alpha blending value
 *
 * @lw_par:	logiWIN data
 * @alpha:	pixel alpha value
 *
 */
void logiwin_set_pixel_alpha(struct logiwin_parameters *lw_par, u32 alpha)
{
	lw_par->alpha = (u8)alpha;
	logiwin_write32(lw_par, LOGIWIN_PIX_ALPHA_ROFF, alpha);
}

/**
 * Set output brightness
 *
 * @lw_par:	logiWIN data
 * @brightness:	output brightness in range (-50,50)
 *
 */
void logiwin_set_brightness(struct logiwin_parameters *lw_par, int brightness)
{
	u32 regval;

	if (brightness < -50)
		brightness = -50;
	else if (brightness > 50)
		brightness = 50;

	lw_par->brightness = brightness;

	regval = 32 + ((63 * brightness) / 100);
	logiwin_write32(lw_par, LOGIWIN_BRIGHTNESS_ROFF, regval);
}

/**
 * Set output contrast
 *
 * @lw_par:	logiWIN data
 * @contrast:	output contrast in range (-50,50)
 *
 */
void logiwin_set_contrast(struct logiwin_parameters *lw_par, int contrast)
{
	u32 regval;

	if (contrast < -50)
		contrast = -50;
	else if (contrast > 50)
		contrast = 50;

	lw_par->contrast = contrast;

	regval = 1992 * (contrast + 50) * 2048 / 100000;
	logiwin_write32(lw_par, LOGIWIN_CONTRAST_ROFF, regval);
}

/**
 * Set output saturation
 *
 * @lw_par:	logiWIN data
 * @saturation:	output saturation in range (-50,50)
 *
 */
void logiwin_set_saturation(struct logiwin_parameters *lw_par, int saturation)
{
	u32 regval;

	if (saturation < -50)
		saturation = -50;
	else if (saturation > 50)
		saturation = 50;

	lw_par->saturation = saturation;

	regval = 1992 * (saturation + 50) * 2048 / 100000;

	logiwin_write32(lw_par, LOGIWIN_SATURATION_ROFF, regval);
}

/**
 * Set output hue
 *
 * @lw_par:	logiWIN data
 * @hue:	output hue in range (-30,30)
 *
 */
void logiwin_set_hue(struct logiwin_parameters *lw_par, int hue)
{
	static const unsigned int cos_table[31] = {
		2048, 2047, 2046, 2045, 2043, 2040, 2036, 2032, 2028, 2022,
		2016, 2010, 2003, 1995, 1987, 1978, 1968, 1958, 1947, 1936,
		1924, 1911, 1898, 1885, 1870, 1856, 1840, 1824, 1808, 1791,
		1773
	};
	static const unsigned int sin_table[31] = {
		0,   35,  71,  107, 142, 178, 214, 249, 285, 320,
		355, 390, 425, 460, 495, 530, 564, 598, 632, 666,
		700, 733, 767, 800, 832, 865, 897, 929, 961, 992,
		1024
	};
	u32 reg_cos, reg_sin;

	if (hue < -30)
		hue = -30;
	else if (hue > 30)
		hue = 30;

	lw_par->hue = hue;

	reg_cos = (hue < 0) ?  cos_table[-hue] : cos_table[hue];
	reg_sin = (hue < 0) ? -sin_table[-hue] : sin_table[hue];

	logiwin_write32(lw_par, LOGIWIN_COS_HUE_ROFF, reg_cos);
	logiwin_write32(lw_par, LOGIWIN_SIN_HUE_ROFF, reg_sin);
}

/**
 * Write mask stencil to dedicated BRAM registers
 *
 * @lw_par:		logiWIN data
 * @mask_buffer:	input mask data
 * @offset:		mask offset
 * @length:		input mask data length
 *
 * Note:
 *	Offset and length must be even values.
 *
 */
void logiwin_write_mask_stencil(struct logiwin_parameters *lw_par,
				unsigned int *mask_buffer,
				unsigned int offset, unsigned int length)
{
	unsigned int pos, end, *buff;

	if ((offset >= MAX_VMEM_STRIDE) || (offset + length > MAX_VMEM_STRIDE))
		return;

	pos = LOGIWIN_MASK_BRAM_OFFSET + offset * 2;
	end = pos + length * 2;
	buff = mask_buffer;
	for (; pos < end; pos += 4, buff++)
		logiwin_write32(lw_par, pos, *buff);
}

/**
 * Update logiWIN registers
 *
 * @lw_par:	logiWIN data
 *
 */
void logiwin_update_registers(struct logiwin_parameters *lw_par)
{
	logiwin_write32(lw_par, LOGIWIN_DR_X_ROFF, lw_par->output.dr_x - 1);
	logiwin_write32(lw_par, LOGIWIN_DR_Y_ROFF, lw_par->output.dr_y - 1);
	logiwin_write32(lw_par, LOGIWIN_UL_X_ROFF, lw_par->output.ul_x);
	logiwin_write32(lw_par, LOGIWIN_UL_Y_ROFF, lw_par->output.ul_y);
	logiwin_write32(lw_par, LOGIWIN_SCALE_X_ROFF,
		(lw_par->hscale_step >> lw_par->scale_shift));
	logiwin_write32(lw_par, LOGIWIN_SCALE_Y_ROFF,
		(lw_par->vscale_step >> lw_par->scale_shift));
	logiwin_write32(lw_par, LOGIWIN_START_X_ROFF,
		(lw_par->start_x >> lw_par->scale_shift));
	logiwin_write32(lw_par, LOGIWIN_START_Y_ROFF,
		(lw_par->start_y >> lw_par->scale_shift));
	logiwin_write32(lw_par, LOGIWIN_CROP_X_ROFF, lw_par->crop.left);
	logiwin_write32(lw_par, LOGIWIN_CROP_Y_ROFF, lw_par->crop.top);
}

/**
 * Enable/disable logiWIN interrupt
 *
 * @lw_par:	logiWIN data
 * @mask:	interrupt bit mask
 *
 */
void logiwin_int(struct logiwin_parameters *lw_par, u32 mask, bool enable)
{
	if (enable)
		lw_par->int_mask &= ~mask;
	else
		lw_par->int_mask |= mask;

	logiwin_write32(lw_par, LOGIWIN_INT_MASK_ROFF, lw_par->int_mask);
}

/**
 * Read logiWIN interrupt mask register
 *
 * @lw_par:	logiWIN data
 *
 * Returns interrupt mask register value
 *
 */
u32 logiwin_int_mask_get(struct logiwin_parameters *lw_par)
{
	return lw_par->int_mask;
}

/**
 * Read logiWIN interrupt status register
 *
 * @lw_par:	logiWIN data
 *
 * Returns interrupt status register value
 *
 */
u32 logiwin_int_stat_get(struct logiwin_parameters *lw_par)
{
	return logiwin_read32(lw_par, LOGIWIN_INT_STAT_ROFF);
}

/**
 * Clears logiWIN interrupt status register
 *
 * @lw_par:	logiWIN data
 * @mask:	interrupt bit mask
 *
 */
void logiwin_int_stat_clear(struct logiwin_parameters *lw_par, u32 mask)
{
	u32 isr;

	if (mask)
		isr = mask;
	else
		isr = ~mask;

	logiwin_write32(lw_par, LOGIWIN_INT_STAT_ROFF, isr);
}

/**
 * Set starting points for interpolation depending on scaling step.
 *
 * @lw_par:	logiWIN data
 *
 */
void logiwin_set_start_scale(struct logiwin_parameters *lw_par)
{
	unsigned int hscale_step = lw_par->hscale_step;
	unsigned int vscale_step = lw_par->vscale_step;

	if (hscale_step <= SCALE_STEP)
		lw_par->start_x = START_X;
	else if ((hscale_step & SCALE_FRAC_MASK) == 0)
		lw_par->start_x = START_X_HALF;
	else
		lw_par->start_x = (hscale_step & SCALE_FRAC_MASK) / 2;
	if (vscale_step <= SCALE_STEP)
		lw_par->start_y = START_Y;
	else if ((vscale_step & SCALE_FRAC_MASK) == 0)
		lw_par->start_y = START_Y_HALF;
	else
		lw_par->start_y = (vscale_step & SCALE_FRAC_MASK) / 2;
}

/**
 * Get logiWIN scaling parameters
 *
 * @lw_par:	logiWIN data
 * @scale_x:	horizontal scaling step
 * @scale_y:	vertical scaling step
 *
 */
void logiwin_get_scale_steps(struct logiwin_parameters *lw_par,
			     unsigned int *scale_x, unsigned int *scale_y)
{
	*scale_x = lw_par->hscale_step;
	*scale_y = lw_par->vscale_step;

	if (lw_par->weave_deinterlace)
		*scale_y /= 2;
}

/**
 * Set horizontal and vertical scale parameters
 *
 * @lw_par:	logiWIN data
 * @scale_x:	horizontal scaling step
 * @scale_y:	vertical scaling step
 *
 * Note:
 *	Used to set up scale parameters directly.
 *	When using this function, function logiwin_set_scale is not used.
 *
 *	Scaling steps are formated in 4.6 fraction format.
 *	Maximum downscale factor 16 (scale step equals 16).
 *	Maximum upscale factor 64 (scale step equals 1/64 = 0.015625).
 *
 */
void logiwin_set_scale_steps(struct logiwin_parameters *lw_par,
			     unsigned int scale_x, unsigned int scale_y)
{
	unsigned int scale_step_min = 1 << lw_par->scale_shift;
	unsigned int scale_prec_mask = (~0) << lw_par->scale_shift;

	if (lw_par->weave_deinterlace)
		scale_y *= 2;

	scale_x &= scale_prec_mask;
	scale_y &= scale_prec_mask;

	if (scale_x < scale_step_min)
		scale_x = scale_step_min;
	else if (scale_x > SCALE_STEP_MAX)
		scale_x = SCALE_STEP_MAX;
	if (scale_y < scale_step_min)
		scale_y = scale_step_min;
	else if (scale_y > SCALE_STEP_MAX)
		scale_y = SCALE_STEP_MAX;

	lw_par->hscale_step = scale_x;
	lw_par->vscale_step = scale_y;

	logiwin_set_start_scale(lw_par);
}

/**
 * Sets logiWIN scaling factors.
 *
 * @lw_par:	logiWIN data
 *
 * Note:
 *	Calculates new scaling parameters based on input and output resolution.
 *
 */
int logiwin_set_scale(struct logiwin_parameters *lw_par)
{
	unsigned int hscale_step, vscale_step;

	if ((lw_par->crop.width == 0) || (lw_par->crop.height == 0) ||
	    (lw_par->out.width == 0) || (lw_par->out.height == 0))
		return -1;

	hscale_step = (SCALE_STEP * lw_par->crop.width) / lw_par->out.width;
	vscale_step = (SCALE_STEP * lw_par->crop.height) / lw_par->out.height;

	if (lw_par->weave_deinterlace)
		vscale_step /= 2;

	logiwin_set_scale_steps(lw_par, hscale_step, vscale_step);

	return 0;
}

/**
 * Check and adjust "check" rectangle against "referent" rectangle
 *
 * @check:	rectangle to be checked
 * @referent:	referent rectangle
 *
 */
static void check_rectangle(struct logiwin_rectangle *check,
			    const struct logiwin_rectangle *referent)
{
	unsigned int check_end, referent_end;
	int diff;

	check_end = check->left + check->width;
	referent_end = referent->left + referent->width;
	if (check->left < referent->left)
		check->left = referent->left;
	if (check_end > referent_end)
		check_end = referent_end;
	diff = check_end - check->left;
	check->width = (diff <= 0) ? 0 : diff;

	check_end = check->top + check->height;
	referent_end = referent->top + referent->height;
	if (check->top < referent->top)
		check->top = referent->top;
	if (check_end > referent_end)
		check_end = referent_end;
	diff = check_end - check->top;
	check->height = (diff <= 0) ? 0 : diff;
}

/**
 * Check and adjust output "check" rectangle against output "out" rectangle
 *
 * @check:	rectangle to be checked
 * @out:	out rectangle
 * @output:	output coordinates
 *
 */
static void set_output_rectangle(struct logiwin_rectangle *check,
				 const struct logiwin_rectangle *out,
				 struct logiwin_output *output)
{
	bool ret_flag = false;

	output->ul_x = check->left;
	output->ul_y = check->top;

	if (check->left > out->width) {
		check->left = 0;
		check->width = out->width;
		output->ul_x = 0;
		output->dr_x = out->width;

		ret_flag = true;
	}
	if (check->top > out->height) {
		check->top = 0;
		check->height = out->height;
		output->ul_y = 0;
		output->dr_y = out->height;

		ret_flag = true;
	}
	if (ret_flag)
		return;

	if ((check->left + check->width) > out->width) {
		check->width = out->width - check->left;
		output->dr_x = out->width;
	} else {
		output->dr_x = check->left + check->width;
	}
	if ((check->top + check->height) > out->height) {
		check->height = out->height - check->top;
		output->dr_y = out->height;
	} else {
		output->dr_y = check->top + check->height;
	}
}

/**
 * Get logiWIN rectangle
 *
 * @lw_par:	logiWIN data
 * @left:	rectangle horizontal upper coordinate
 * @top:	rectangle vertical upper coordinate
 * @width:	rectangle width
 * @height:	rectangle height
 * @type:	rectangle type
 *
 *****************************************************************************/
void logiwin_get_rect_parameters(struct logiwin_parameters *lw_par,
				 unsigned int *left, unsigned int *top,
				 unsigned int *width, unsigned int *height,
				 enum logiwin_rectangle_type type)
{
	struct logiwin_rectangle *r;
	int divide = 1;

	switch (type) {
	case LOGIWIN_RECTANGLE_BOUNDS:
		r = &lw_par->bounds;
		break;

	case LOGIWIN_RECTANGLE_CROP:
		r = &lw_par->crop;
		break;

	case LOGIWIN_RECTANGLE_OUT:
		r = &lw_par->out;
		if (lw_par->weave_deinterlace)
			divide = 2;
		break;

	default:
		return;
	}

	if (left)
		*left = r->left;
	if (top)
		*top = r->top / divide;
	if (width)
		*width = r->width;
	if (height)
		*height = r->height / divide;
}

/**
 * Set logiWIN rectangle
 *
 * @lw_par:	logiWIN data
 * @left:	rectangle horizontal upper coordinate
 * @top:	rectangle vertical upper coordinate
 * @width:	rectangle width
 * @height:	rectangle height
 * @type:	rectangle type
 *
 */
void logiwin_set_rect_parameters(struct logiwin_parameters *lw_par,
				 unsigned int left, unsigned int top,
				 unsigned int width, unsigned int height,
				 enum logiwin_rectangle_type type)
{
	struct logiwin_rectangle out;
	u32 out_align_mask = lw_par->out_align_mask;

	if (left % lw_par->out_align)
		left &= out_align_mask;
	if (width % lw_par->out_align)
		width &= out_align_mask;

	switch (type) {
	case LOGIWIN_RECTANGLE_BOUNDS:
		lw_par->bounds.left = left;
		lw_par->bounds.top = top;
		lw_par->bounds.width = width;
		lw_par->bounds.height = height;

		check_rectangle(&lw_par->bounds, &lw_par->in);
		break;

	case LOGIWIN_RECTANGLE_CROP:
		lw_par->crop.left = left;
		lw_par->crop.top = top;
		lw_par->crop.width = width;
		lw_par->crop.height = height;
		break;

	case LOGIWIN_RECTANGLE_OUT:
		lw_par->out.left = left;
		lw_par->out.top = top;
		lw_par->out.width = width;
		lw_par->out.height = height;

		out.left = 0;
		out.top = 0;
		out.width = lw_par->out_hres;
		out.height = lw_par->out_vres;

		set_output_rectangle(&lw_par->out, &out, &lw_par->output);

		if (lw_par->weave_deinterlace) {
			lw_par->out.top /= 2;
			lw_par->out.height /= 2;
		}
		break;
	}
}
