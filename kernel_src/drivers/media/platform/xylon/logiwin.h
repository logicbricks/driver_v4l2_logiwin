/*
 * Xylon logiWIN frame grabber core header
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

#ifndef __LOGIWIN_H__
#define __LOGIWIN_H__

/* Resolution constants */
#define MAX_VMEM_STRIDE		2048
#define MAX_IN_HRES		2048
#define MAX_IN_VRES		2048
#define MAX_OUT_HRES		2048
#define MAX_OUT_VRES		2048

/* Scale step constant */
#define SCALE_STEP		(1 << 16)

/* Interrupt Register Bits */
#define LOGIWIN_INT_FRAME_START	0x1
#define LOGIWIN_INT_RESOLUTION	0x2
#define LOGIWIN_INT_ALL		(LOGIWIN_INT_FRAME_START | \
				 LOGIWIN_INT_RESOLUTION)

/* logiWIN Operation Flag */
enum logiwin_operation_flag {
	LOGIWIN_OP_FLAG_DISABLE,
	LOGIWIN_OP_FLAG_ENABLE
};

/* logiWIN Operation */
enum logiwin_operation {
	LOGIWIN_OP_ENABLE,
	LOGIWIN_OP_EVEN_FIELD_VBUFF_SWITCH,
	LOGIWIN_OP_STENCIL_MASK,
	LOGIWIN_OP_CPU_VBUFF_SWITCH,
	LOGIWIN_OP_FRAME_STORED_STOP,
	LOGIWIN_OP_SWIZZLE
};

/* logiWIN Input Format */
enum logiwin_format_video_input {
	LOGIWIN_FORMAT_INPUT_DVI,
	LOGIWIN_FORMAT_INPUT_ITU,
	LOGIWIN_FORMAT_INPUT_RGB
};

/* logiWIN Output Format */
enum logiwin_format_video_output {
	LOGIWIN_FORMAT_OUTPUT_RGB565,
	LOGIWIN_FORMAT_OUTPUT_ARGB8888,
	LOGIWIN_FORMAT_OUTPUT_YUYV
};

/* logiWIN Frame Rate */
enum logiwin_frame_rate {
	LOGIWIN_FRAME_RATE_FULL,
	LOGIWIN_FRAME_RATE_75,
	LOGIWIN_FRAME_RATE_50,
	LOGIWIN_FRAME_RATE_25
};

/* logiWIN Rectangle Type */
enum logiwin_rectangle_type {
	LOGIWIN_RECTANGLE_BOUNDS,
	LOGIWIN_RECTANGLE_CROP,
	LOGIWIN_RECTANGLE_OUT
};

/*
 * logiWIN rectangle
 * @x:	upper left x
 * @y:	upper left y
 * @w:	width in pixels
 * @h:	height in lines
 */
struct logiwin_rectangle {
	unsigned int left;
	unsigned int top;
	unsigned int width;
	unsigned int height;
};

/*
 * logiWIN output
 * @ul_x:	up left x
 * @ul_y:	up left y
 * @dr_x:	down right x
 * @dr_y:	down right y
 */
struct logiwin_output {
	u32 ul_x;
	u32 ul_y;
	u32 dr_x;
	u32 dr_y;
};

/*
 * logiWIN data
 * @base:			Registers base
 * @in:				Input rectangle
 * @bounds:			Input bounds rectangle
 * @crop:			Input crop rectangle
 * @ctrl:			Control register
 * @int_mask:			Interrupt mask value
 * @out_align_mask:		Output byte alignment mask
 * @out:			Output rectangle
 * @output:			Output rectangle coordinates
 * @out_hres:			Calculated output horizontal resolution after
 				scale/crop
 * @out_vres:			Calculated output vertical resolution after
 				scale/crop
 * @hscale_step:		Horizontal scale step
 * @vscale_step:		Vertical scale step
 * @start_x:			First interpolated pixel horizontal distance
 * @start_y:			First interpolated pixel vertical distance
 * @scale_shift:		Number of bits to shift scale/start values
 				before writing to register
 * @out_align:			Output byte alignment (UL_X and DR_X should be
 				aligned to that number)
 * @input_format:		Input format (DVI, ITU, RGB)
 * @brightness:			Defines output image brightness in range
 				0 - 100 (percent)
 * @contrast:			Defines output image contrast in range
 				0 - 100 (percent)
 * @saturation:			Defines output image color saturation in range
 				0 - 100 (percent)
 * @hue:			Defines output image hue in range -30° to 30°
 * @channel_id:			Active channel id
 * @weave_deinterlace:		Weave deinterlace ("bob" deinterlace by default)
 * hw_access:			Register access flag, if not set, registers
 *				are not written
*/
struct logiwin_parameters {
	void __iomem *base;
	struct logiwin_rectangle in;
	struct logiwin_rectangle bounds;
	struct logiwin_rectangle crop;
	struct logiwin_rectangle out;
	struct logiwin_output output;
	u32 ctrl;
	u32 int_mask;
	u32 out_align_mask;
	unsigned int out_hres;
	unsigned int out_vres;
	unsigned int hscale_step;
	unsigned int vscale_step;
	unsigned int start_x;
	unsigned int start_y;
	unsigned int scale_shift;
	unsigned int out_align;
	enum logiwin_format_video_input input_format;
	int brightness;
	int contrast;
	int saturation;
	int hue;
	u8 alpha;
	u8 channel_id;
	bool weave_deinterlace;
	bool hw_access;
};

/* Control functions */
void logiwin_operation(struct logiwin_parameters *lw_par,
		       enum logiwin_operation op,
		       enum logiwin_operation_flag op_flag);
void logiwin_weave_deinterlace(struct logiwin_parameters *lw,
			       bool weave_deinterlace);
void logiwin_select_input_ch(struct logiwin_parameters *lw, unsigned int ch);
void logiwin_set_frame_rate(struct logiwin_parameters *lw,
			    enum logiwin_frame_rate frame_rate);
void logiwin_sync_polarity(struct logiwin_parameters *lw_par,
			   unsigned int ch, bool hsync_inv, bool vsync_inv);

void logiwin_get_resolution(struct logiwin_parameters *lw,
			    u32 *hres, u32 *vres);

void logiwin_set_memory_offset(struct logiwin_parameters *lw,
			       u32 even_ptr, u32 odd_ptr);
void logiwin_set_pixel_alpha(struct logiwin_parameters *lw, u32 alpha);

void logiwin_set_brightness(struct logiwin_parameters *lw, int brightness);
void logiwin_set_contrast(struct logiwin_parameters *lw, int contrast);
void logiwin_set_saturation(struct logiwin_parameters *lw, int saturation);
void logiwin_set_hue(struct logiwin_parameters *lw, int hue);

void logiwin_write_mask_stencil(struct logiwin_parameters *lw,
				unsigned int *mask_buffer,
				unsigned int offset, unsigned int length);

void logiwin_update_registers(struct logiwin_parameters *lw);

/* Interrupt functions */
void logiwin_int(struct logiwin_parameters *lw, u32 mask, bool enable);
u32 logiwin_int_mask_get(struct logiwin_parameters *lw);
u32 logiwin_int_stat_get(struct logiwin_parameters *lw);
void logiwin_int_stat_clear(struct logiwin_parameters *lw, u32 mask);

/* Frame grab functions */
void logiwin_set_start_scale(struct logiwin_parameters *lw);
void logiwin_get_scale_steps(struct logiwin_parameters *lw,
			     unsigned int *scale_x, unsigned int *scale_y);
void logiwin_set_scale_steps(struct logiwin_parameters *lw,
			     unsigned int scale_x, unsigned int scale_y);
int logiwin_set_scale(struct logiwin_parameters *lw);

void logiwin_get_rect_parameters(struct logiwin_parameters *lw_par,
				 unsigned int *left, unsigned int *top,
				 unsigned int *width, unsigned int *height,
				 enum logiwin_rectangle_type type);
void logiwin_set_rect_parameters(struct logiwin_parameters *lw_par,
				 unsigned int left, unsigned int top,
				 unsigned int width, unsigned int height,
				 enum logiwin_rectangle_type type);

#endif /* __LOGWIN_H__ */
