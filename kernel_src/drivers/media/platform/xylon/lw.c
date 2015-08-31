/*
 * Xylon logiWIN frame grabber Open Firmware driver
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

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "logiwin.h"

#define INFO		1
#define CORE		2
#define DEBUG_LEVEL	CORE

#define DEBUG
#ifdef DEBUG
#define LW_DBG(level, format, ...)					    \
	do {								    \
		if (level >= DEBUG_LEVEL)				    \
			pr_info("%s "format"\n", __func__, ## __VA_ARGS__); \
	} while (0)
#else
#define LW_DBG(format, ...) do { } while (0)
#endif

#define DRIVER_NAME			"logiwin"
#define DEVICE_NAME			"logiWIN"
#define LOGIWIN_DRIVER_DESCRIPTION	"Xylon logiWIN frame grabber driver"
#define LOGIWIN_DRIVER_VERSION		"1.0"

#define LOGIWIN_DMA_BUFFERS		3
#define LOGIWIN_KERNEL_VERSION		3

#define LOGIWIN_FLAG_UPDATE_REGISTERS		(1 << 0)
#define LOGIWIN_FLAG_BUFFERS_AVAILABLE		(1 << 1)
#define LOGIWIN_FLAG_BUFFERS_REALLOCATE		(1 << 2)
#define LOGIWIN_FLAG_BUFFERS_DESTROY		(1 << 3)
#define LOGIWIN_FLAG_DEINTERLACE		(1 << 4)
#define LOGIWIN_FLAG_RESOLUTION_CHANGE		(1 << 5)
#define LOGIWIN_FLAG_RESOLUTION			(1 << 6)
#define LOGIWIN_FLAG_DEVICE_IN_USE		(1 << 7)
#define LOGIWIN_FLAG_OVERLAY_BUFFER_SWITCH	(1 << 8)
#define LOGIWIN_FLAG_HW_BUFFER_SWITCH		(1 << 9)

#define LOGIWIN_IOCTL_FRAME_INT		_IO('V', BASE_VIDIOC_PRIVATE)
#define LOGIWIN_IOCTL_RESOLUTION_INT	_IO('V', (BASE_VIDIOC_PRIVATE + 1))
#define LOGIWIN_IOCTL_OVERLAY_BUFFER_SWITCH	\
	_IOW('V', (BASE_VIDIOC_PRIVATE + 2), bool)
#define LOGIWIN_IOCTL_RESOLUTION_GET	\
	_IOR('V', (BASE_VIDIOC_PRIVATE + 3), struct v4l2_rect)
#define LOGIWIN_IOCTL_SWIZZLE		\
	_IOW('V', (BASE_VIDIOC_PRIVATE + 4), bool)
#define LOGIWIN_IOCTL_SYNC_POLARITY	\
	_IOW('V', (BASE_VIDIOC_PRIVATE + 5), unsigned int)
#define LOGIWIN_IOCTL_ALPHA		\
	_IOW('V', (BASE_VIDIOC_PRIVATE + 6), unsigned int)
#define LOGIWIN_IOCTL_FRAMES_SKIP	\
	_IOR('V', (BASE_VIDIOC_PRIVATE + 7), unsigned int)
#define LOGIWIN_IOCTL_FRAME_PHYS_ADDRESS	\
	_IOR('V', (BASE_VIDIOC_PRIVATE + 8), unsigned long)

enum logiwin_stream_state {
	STREAM_OFF,
	CAPTURE_STREAM_ON,
	OVERLAY_STREAM_ON
};

enum logiwin_frame_state {
	FRAME_UNUSED,
	FRAME_QUEUED,
	FRAME_DEQUEUED,
	FRAME_DONE
};

struct logiwin_frame {
	struct list_head frame;
	struct v4l2_buffer buf;
	enum logiwin_frame_state state;
	u32 buff_addr;
	atomic_t vma_refcnt;
};

struct logiwin_video_norm {
	v4l2_std_id norm;
	char *name;
	u16 width;
	u16 height;
};

struct logiwin_dma {
	dma_addr_t pa;
	void *va;
};

struct logiwin_buffer {
	struct logiwin_dma address[LOGIWIN_DMA_BUFFERS];
	unsigned int size;
	unsigned int id;
};

struct logiwin_config {
	u32 vmem_addr_start;
	u32 vmem_addr_end;
	u32 input_hres;
	u32 input_vres;
	u32 input_num;
	u32 input_format;
	u32 output_format;
	u32 output_hres;
	u32 output_vres;
	u32 out_align;
	u32 scale_fraction_bits;
	bool hw_buff_switch;
};

struct logiwin_hw {
	dma_addr_t reg_pbase;
	dma_addr_t vmem_pbase;

	void __iomem *reg_base;
#ifdef LOGIWIN_MMAP_VMEM
	void __iomem *vmem_base;
#endif
	unsigned long vmem_size;

	unsigned int bpp;
	int irq;
};

struct logiwin {
	struct device *dev;

	struct logiwin_config lw_cfg;
	struct logiwin_hw lw_hw;
	struct logiwin_parameters lw_par;

	struct logiwin_buffer capture;
	struct logiwin_buffer overlay;

	struct logiwin_frame *frame;
	unsigned int frames;
	unsigned int frames_queue;
	unsigned int frames_skip;
	unsigned int frame_seq;

	struct list_head inqueue;
	struct list_head outqueue;

	struct video_device video_dev;
	struct v4l2_device v4l2_dev;

	struct v4l2_cropcap cropcap;
	struct v4l2_pix_format pix_format;
	struct v4l2_rect crop;
	struct v4l2_window window;

	struct logiwin_video_norm video_norm;

	struct mutex fops_lock;
	struct mutex ioctl_lock;

	spinlock_t irq_lock;

	struct tasklet_struct tasklet;

	wait_queue_head_t wait_buff_switch;
	wait_queue_head_t wait_frame;
	wait_queue_head_t wait_resolution;

	atomic_t wait_buff_switch_refcnt;
	atomic_t wait_resolution_refcnt;

	enum logiwin_stream_state stream_state;

	u32 flags;
};

static const char logiwin_formats[][22] = {
	{"5:6:5, packed, RGB"},
	{"8:8:8:8, packed, ARGB"},
	{"4:2:2, packed, YUYV"}
};

static void logiwin_set_video_norm(struct logiwin_video_norm *video_norm,
				   int width, int height)
{
	LW_DBG(INFO, "");

	video_norm->norm = V4L2_STD_UNKNOWN;
	video_norm->name = "CUSTOM";
	video_norm->width = width;
	video_norm->height = height;
}

static int logiwin_update(struct logiwin *lw)
{
	u32 h, v;

	LW_DBG(INFO, "");

	if (lw->flags & LOGIWIN_FLAG_RESOLUTION_CHANGE) {
		logiwin_get_resolution(&lw->lw_par, &h, &v);
		if ((h > 0) && (h <= MAX_IN_HRES) &&
		    (v > 0) && (v <= MAX_IN_VRES)) {
			lw->lw_par.bounds.left = 0;
			lw->lw_par.bounds.top = 0;
			lw->lw_par.bounds.width = h;
			lw->lw_par.bounds.height = v;

			lw->lw_par.crop = lw->lw_par.bounds;

			logiwin_set_scale(&lw->lw_par);

			lw->flags |= LOGIWIN_FLAG_RESOLUTION;
			wake_up_interruptible(&lw->wait_resolution);
		}

		lw->flags &= ~LOGIWIN_FLAG_RESOLUTION_CHANGE;
	}

	logiwin_update_registers(&lw->lw_par);

	return 0;
}

static unsigned int logiwin_get_buf(struct logiwin *lw)
{
	unsigned long flags;
	unsigned int curr_id;
	int queued;

	if (lw->stream_state == CAPTURE_STREAM_ON) {
		curr_id = lw->capture.id;

		spin_lock_irqsave(&lw->irq_lock, flags);
		queued = lw->frames_queue;
		spin_unlock_irqrestore(&lw->irq_lock, flags);

		if (queued == 0)
			return curr_id;

		do {
			if (lw->capture.id < (lw->frames - 1))
				lw->capture.id++;
			else
				lw->capture.id = 0;

			if (lw->frame[lw->capture.id].state == FRAME_QUEUED)
				break;

			queued--;
		} while (queued > 0);

		return queued ? lw->capture.id : curr_id;
	} else if (lw->stream_state == OVERLAY_STREAM_ON) {
		if (lw->overlay.id < (lw->frames - 1))
			lw->overlay.id++;
		else
			lw->overlay.id = 0;

		return lw->overlay.id;
	} else {
		return 0;
	}
}

static void logiwin_enable(struct logiwin *lw,
			   enum logiwin_stream_state stream_state)
{
	dma_addr_t pa;
	u32 int_mask;

	if (stream_state == CAPTURE_STREAM_ON) {
		lw->capture.id = lw->frames - 1;
		lw->capture.id = logiwin_get_buf(lw);
		pa = lw->capture.address[lw->capture.id].pa;
	} else if (stream_state == OVERLAY_STREAM_ON) {
		pa = lw->overlay.address[0].pa;
		lw->flags |= LOGIWIN_FLAG_OVERLAY_BUFFER_SWITCH;
	}

	logiwin_update_registers(&lw->lw_par);

	logiwin_set_memory_offset(&lw->lw_par, pa, pa);

	int_mask = LOGIWIN_INT_RESOLUTION;
	if (!(lw->flags & LOGIWIN_FLAG_HW_BUFFER_SWITCH))
		int_mask |= LOGIWIN_INT_FRAME_START;
	logiwin_int_stat_clear(&lw->lw_par, 0);
	logiwin_int(&lw->lw_par, int_mask, true);

	lw->frames_skip = 0;
	lw->frame_seq = 0;

	lw->stream_state = stream_state;

	logiwin_operation(&lw->lw_par, LOGIWIN_OP_ENABLE,
			  LOGIWIN_OP_FLAG_ENABLE);
}

static void logiwin_disable(struct logiwin *lw)
{
	logiwin_operation(&lw->lw_par, LOGIWIN_OP_ENABLE,
			  LOGIWIN_OP_FLAG_DISABLE);

	logiwin_int(&lw->lw_par, LOGIWIN_INT_ALL, false);

	lw->stream_state = STREAM_OFF;

	lw->flags &= ~LOGIWIN_FLAG_OVERLAY_BUFFER_SWITCH;

	wake_up_interruptible(&lw->wait_buff_switch);
	wake_up(&lw->wait_frame);
	wake_up_interruptible(&lw->wait_resolution);
}

static void logiwin_empty_queues(struct logiwin *lw)
{
	unsigned long flags;
	int i;

	LW_DBG(INFO, "");

	spin_lock_irqsave(&lw->irq_lock, flags);

	INIT_LIST_HEAD(&lw->inqueue);
	INIT_LIST_HEAD(&lw->outqueue);

	for (i = 0; i < lw->frames; i++) {
		lw->frame[i].state = FRAME_DEQUEUED;
		lw->frame[i].buf.bytesused = 0;
		lw->frame[i].buf.sequence = 0;
	}

	lw->frames_queue = 0;

	spin_unlock_irqrestore(&lw->irq_lock, flags);
}

static unsigned int logiwin_request_buffers(struct logiwin *lw,
					    unsigned int count)
{
	int i;

	LW_DBG(INFO, "");

	if (count > LOGIWIN_DMA_BUFFERS) {
		count = LOGIWIN_DMA_BUFFERS;
		dev_warn(lw->dev, "buffer count set to %d\n",
			 LOGIWIN_DMA_BUFFERS);
	}

	lw->capture.id = 0;
	lw->frames = 0;

	lw->capture.size = lw->pix_format.sizeimage;

	if (lw->lw_hw.vmem_pbase) {
		for (i = 0; i < count; i++) {
			lw->capture.address[i].pa =
				lw->lw_hw.vmem_pbase + (i * lw->capture.size);
#ifdef LOGIWIN_MMAP_VMEM
			lw->capture.address[i].va =
				lw->lw_hw.vmem_base + (i * lw->capture.size);
#endif
			lw->frames++;
		}
	} else {
		/*
		 * create "count" buffers, or try to create at least one buffer
		 */
		for (i = 0; i < count; i++) {
			lw->capture.address[i].va =
				dma_alloc_coherent(lw->dev,
						   lw->capture.size,
						   &lw->capture.address[i].pa,
						   GFP_KERNEL);
			LW_DBG(INFO, "PA 0x%X : VA 0x%X",
				lw->capture.address[i].pa,
				(unsigned int)lw->capture.address[i].va);
			if (lw->capture.address[i].va == NULL)
				break;
			lw->frames++;
		}
		LW_DBG(INFO, "DMA frame buffers created %d", lw->frames);
		if (lw->frames == 0)
			return 0;
	}

	lw->frame = kzalloc((sizeof(struct logiwin_frame) * lw->frames),
			     GFP_KERNEL);
	if (lw->frame == NULL)
		goto error_handle;

	for (i = 0; i < lw->frames; i++) {
		lw->frame[i].state = FRAME_UNUSED;
		atomic_set(&lw->frame[i].vma_refcnt, 0);
		lw->frame[i].buff_addr = lw->capture.address[i].pa;
		lw->frame[i].buf.index = i;
		if (lw->lw_hw.vmem_pbase)
			lw->frame[i].buf.m.offset = i * lw->capture.size;
		else
			lw->frame[i].buf.m.offset = lw->capture.address[i].pa;
		lw->frame[i].buf.length = lw->capture.size;
		lw->frame[i].buf.bytesused = 0;
		lw->frame[i].buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		lw->frame[i].buf.sequence = 0;
		lw->frame[i].buf.field = V4L2_FIELD_NONE;
		lw->frame[i].buf.memory = V4L2_MEMORY_MMAP;
		lw->frame[i].buf.flags = 0;
	}

	lw->frames_queue = 0;

	lw->flags |= LOGIWIN_FLAG_BUFFERS_AVAILABLE;

	return lw->frames;

error_handle:
	if (!lw->lw_hw.vmem_pbase)
		for (i = lw->frames - 1; i >= 0; i--)
			dma_free_coherent(lw->dev,
					  lw->capture.size,
					  lw->capture.address[i].va,
					  lw->capture.address[i].pa);

	return -ENOMEM;
}

static void logiwin_release_buffers(struct logiwin *lw)
{
	int i;

	LW_DBG(INFO, "");

	kfree(lw->frame);
	lw->frame = NULL;
	if (!lw->lw_hw.vmem_pbase)
		for (i = lw->frames - 1; i >= 0; i--)
			dma_free_coherent(lw->dev,
					  lw->capture.size,
					  lw->capture.address[i].va,
					  lw->capture.address[i].pa);

	lw->frames = 0;
	lw->frames_queue = 0;

	lw->flags &= ~LOGIWIN_FLAG_BUFFERS_AVAILABLE;
}

static void logiwin_release_overlay_buffers(struct logiwin *lw)
{
	int i;

	LW_DBG(INFO, "");

	for (i = 0; i < lw->frames; i++)
		lw->overlay.address[i].pa = 0;
	lw->overlay.size = 0;

	lw->frames = 0;
}

static int vidioc_querycap(struct file *file, void *fh,
			   struct v4l2_capability *cap)
{
	LW_DBG(INFO, "");

	strlcpy((char *)cap->card, DEVICE_NAME, sizeof(cap->card));
	strlcpy((char *)cap->driver, DRIVER_NAME, sizeof(cap->card));
	cap->bus_info[0] = '\0';
	cap->version = LOGIWIN_KERNEL_VERSION;
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OVERLAY |
			    V4L2_CAP_STREAMING;

	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *fh,
				   struct v4l2_fmtdesc *fmt)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if ((fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) ||
	    (fmt->index > ARRAY_SIZE(logiwin_formats)))
		return -EINVAL;

	strlcpy((char *)fmt->description, &logiwin_formats[fmt->index][0],
		sizeof(fmt->description));
	fmt->pixelformat = lw->pix_format.pixelformat;

	return 0;
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	f->fmt.pix = lw->pix_format;

	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct logiwin *lw = fh;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	unsigned int dummy = 0;

	LW_DBG(INFO, "");

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if ((pix->field == V4L2_FIELD_NONE) || (pix->field == V4L2_FIELD_ANY))
		lw->flags &= ~LOGIWIN_FLAG_DEINTERLACE;
	else if (pix->field == V4L2_FIELD_INTERLACED)
		lw->flags |= LOGIWIN_FLAG_DEINTERLACE;
	else
		return -EINVAL;

	if (pix->width > lw->lw_cfg.output_hres)
		pix->width = lw->lw_cfg.output_hres;
	if (pix->height > lw->lw_cfg.output_vres)
		pix->height = lw->lw_cfg.output_vres;
	pix->pixelformat = lw->lw_cfg.output_format;
	pix->bytesperline = lw->pix_format.bytesperline;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = lw->pix_format.colorspace;

	logiwin_set_rect_parameters(&lw->lw_par, 0, 0, pix->width, pix->height,
				    LOGIWIN_RECTANGLE_OUT);
	if (logiwin_set_scale(&lw->lw_par))
		return -EINVAL;

	lw->flags |= LOGIWIN_FLAG_UPDATE_REGISTERS;

	logiwin_get_rect_parameters(&lw->lw_par, &dummy, &dummy,
				    &pix->width, &pix->height,
				    LOGIWIN_RECTANGLE_OUT);
	lw->pix_format = *pix;

	logiwin_set_video_norm(&lw->video_norm, pix->width, pix->height);

	if (lw->flags & LOGIWIN_FLAG_HW_BUFFER_SWITCH)
		tasklet_schedule(&lw->tasklet);

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	struct logiwin *lw = fh;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	LW_DBG(INFO, "");

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if ((f->fmt.pix.field != V4L2_FIELD_NONE) ||
	    (f->fmt.pix.field != V4L2_FIELD_ANY) ||
	    (f->fmt.pix.field != V4L2_FIELD_INTERLACED))
		return -EINVAL;

	if (pix->width > lw->lw_cfg.output_hres)
		pix->width = lw->lw_cfg.output_hres;
	if (pix->height > lw->lw_cfg.output_vres)
		pix->height = lw->lw_cfg.output_vres;
	pix->pixelformat = lw->lw_cfg.output_format;
	pix->bytesperline = lw->pix_format.bytesperline;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = lw->pix_format.colorspace;

	return 0;
}

static int vidioc_enum_fmt_vid_overlay(struct file *file, void *fh,
				       struct v4l2_fmtdesc *fmt)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if ((fmt->type != V4L2_BUF_TYPE_VIDEO_OVERLAY) ||
	    (fmt->index > ARRAY_SIZE(logiwin_formats)))
		return -EINVAL;

	strlcpy((char *)fmt->description, &logiwin_formats[fmt->index][0],
		sizeof(fmt->description));
	fmt->pixelformat = lw->pix_format.pixelformat;

	return 0;
}

static int vidioc_g_fmt_vid_overlay(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (f->type != V4L2_BUF_TYPE_VIDEO_OVERLAY)
		return -EINVAL;

	lw->window.w.width = lw->video_norm.width;
	lw->window.w.height = lw->video_norm.height;
	lw->window.field = V4L2_FIELD_NONE;
	lw->window.global_alpha = lw->lw_par.alpha;

	f->fmt.win = lw->window;

	return 0;
}

static int vidioc_s_fmt_vid_overlay(struct file *file, void *fh,
				    struct v4l2_format *f)
{
	struct logiwin *lw = fh;
	struct v4l2_window *win = &f->fmt.win;

	LW_DBG(INFO, "");

	if (f->type != V4L2_BUF_TYPE_VIDEO_OVERLAY)
		return -EINVAL;

	if ((win->field == V4L2_FIELD_NONE) || (win->field == V4L2_FIELD_ANY))
		lw->flags &= ~LOGIWIN_FLAG_DEINTERLACE;
	else if (win->field == V4L2_FIELD_INTERLACED)
		lw->flags |= LOGIWIN_FLAG_DEINTERLACE;
	else
		return -EINVAL;

	logiwin_set_pixel_alpha(&lw->lw_par, win->global_alpha);

	logiwin_set_rect_parameters(&lw->lw_par, win->w.left, win->w.top,
				    win->w.width, win->w.height,
				    LOGIWIN_RECTANGLE_OUT);

	lw->flags |= LOGIWIN_FLAG_UPDATE_REGISTERS;

	logiwin_get_rect_parameters(&lw->lw_par, &win->w.left, &win->w.top,
				    &win->w.width, &win->w.height,
				    LOGIWIN_RECTANGLE_OUT);
	lw->window = *win;

	if (lw->flags & LOGIWIN_FLAG_HW_BUFFER_SWITCH)
		tasklet_schedule(&lw->tasklet);

	return 0;
}

static int vidioc_try_fmt_vid_overlay(struct file *file, void *fh,
				      struct v4l2_format *f)
{
	struct logiwin *lw = fh;
	struct v4l2_window *win = &f->fmt.win;

	LW_DBG(INFO, "");

	if (f->type != V4L2_BUF_TYPE_VIDEO_OVERLAY)
		return -EINVAL;

	if ((win->field == V4L2_FIELD_NONE) || (win->field == V4L2_FIELD_ANY))
		lw->flags &= ~LOGIWIN_FLAG_DEINTERLACE;
	else if (win->field == V4L2_FIELD_INTERLACED)
		lw->flags |= LOGIWIN_FLAG_DEINTERLACE;
	else
		return -EINVAL;

	if (win->w.left < 0)
		win->w.left = 0;
	if (win->w.top < 0)
		win->w.top = 0;
	if (win->w.width > lw->lw_cfg.output_hres)
		win->w.width = lw->lw_cfg.output_hres;
	if (win->w.height > lw->lw_cfg.output_vres)
		win->w.height = lw->lw_cfg.output_vres;

	return 0;
}

static int vidioc_g_std(struct file *file, void *fh, v4l2_std_id *norm)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	*norm = lw->video_norm.norm;

	return 0;
}

static int vidioc_s_std(struct file *file, void *fh, v4l2_std_id norm)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (norm != lw->video_norm.norm)
		return -EINVAL;

	return 0;
}

static int vidioc_enum_input(struct file *file, void *fh,
			     struct v4l2_input *inp)
{
	struct logiwin *lw = fh;
	char str[32] = "logiWIN input ";

	LW_DBG(INFO, "");

	if (inp->index != 0)
		return -EINVAL;

	switch (lw->lw_par.input_format) {
	case LOGIWIN_FORMAT_INPUT_DVI:
		strcat(str, "DVI");
		break;
	case LOGIWIN_FORMAT_INPUT_ITU:
		strcat(str, "ITU");
		break;
	case LOGIWIN_FORMAT_INPUT_RGB:
		strcat(str, "RGB");
		break;
	default:
		strcat(str, "unknown");
		break;
	}

	strlcpy((char *)inp->name, str, sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->std = V4L2_STD_ALL;

	return 0;
}

static int vidioc_g_input(struct file *file, void *fh, unsigned int *channel)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	*channel = lw->lw_par.channel_id;

	return 0;
}

static int vidioc_s_input(struct file *file, void *fh, unsigned int channel)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if ((channel < 0) && (channel > 1))
		return -EINVAL;

	logiwin_select_input_ch(&lw->lw_par, channel);

	return 0;
}

static int vidioc_g_parm(struct file *file, void *fh,
			 struct v4l2_streamparm *sp)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	sp->parm.capture.capability = 0;
	sp->parm.capture.capturemode = 0;
	sp->parm.capture.extendedmode = 0;
	sp->parm.capture.readbuffers = lw->frames;

	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if ((fsize->index != 0) ||
	    (fsize->pixel_format != lw->pix_format.pixelformat))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = lw->video_norm.width;
	fsize->discrete.height = lw->video_norm.height;

	return 0;
}

static int vidioc_reqbufs(struct file *file, void *fh,
			  struct v4l2_requestbuffers *rb)
{
	struct logiwin *lw = fh;
	int i;

	LW_DBG(INFO, "");

	if (rb->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    rb->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	if (rb->count == 0 && lw->frames) {
		lw->flags |= LOGIWIN_FLAG_BUFFERS_DESTROY;
		return 0;
	} else if (rb->count && rb->count != lw->frames) {
		lw->flags |= LOGIWIN_FLAG_BUFFERS_REALLOCATE;
		for (i = 0; i < lw->frames; i++)
			while (lw->frame[i].state != FRAME_UNUSED)
				mdelay(100);
		logiwin_release_buffers(lw);
		rb->count = logiwin_request_buffers(lw, rb->count);
	} else if (rb->count && lw->frames == 0) {
		rb->count = logiwin_request_buffers(lw, rb->count);
	}

	if (rb->count == 0)
		return -ENOMEM;

	logiwin_empty_queues(lw);

	return 0;
}

static int vidioc_querybuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->index >= lw->frames)
		return -EINVAL;

	if (!(lw->flags & LOGIWIN_FLAG_BUFFERS_AVAILABLE))
		return -ENOMEM;

	*b = lw->frame[b->index].buf;

	if (atomic_read(&lw->frame[b->index].vma_refcnt))
		b->flags |= V4L2_BUF_FLAG_MAPPED;
	if (lw->frame[b->index].state == FRAME_DONE)
		b->flags |= V4L2_BUF_FLAG_DONE;
	else if (lw->frame[b->index].state == FRAME_QUEUED)
		b->flags |= V4L2_BUF_FLAG_QUEUED;

	return 0;
}

static int vidioc_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct logiwin *lw = fh;
	unsigned long flags;

	LW_DBG(INFO, "");

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || b->index >= lw->frames)
		return -EINVAL;

	if (!(lw->flags & LOGIWIN_FLAG_BUFFERS_AVAILABLE))
		return -ENOMEM;

	if (b->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	if (lw->frame[b->index].state != FRAME_DEQUEUED)
		return -EAGAIN;

	spin_lock_irqsave(&lw->irq_lock, flags);

	list_add_tail(&lw->frame[b->index].frame, &lw->inqueue);
	lw->frame[b->index].state = FRAME_QUEUED;

	if (atomic_read(&lw->frame[b->index].vma_refcnt))
		b->flags |= V4L2_BUF_FLAG_MAPPED;
	b->flags |= V4L2_BUF_FLAG_QUEUED;
	b->flags &= ~V4L2_BUF_FLAG_DONE;

	lw->frames_queue++;

	spin_unlock_irqrestore(&lw->irq_lock, flags);

	return 0;
}

static int vidioc_dqbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct logiwin *lw = fh;
	struct logiwin_frame *frame;
	int ret = 0;
	unsigned long flags;

	LW_DBG(INFO, "");

	if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (!(lw->flags & LOGIWIN_FLAG_BUFFERS_AVAILABLE) ||
	    b->index >= lw->frames)
		return -ENOMEM;

	if (lw->stream_state == STREAM_OFF)
		return -ENODEV;

	if (list_empty(&lw->outqueue)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_timeout(lw->wait_frame,
					 !list_empty(&lw->outqueue),
					 60*HZ/1000);
		if (ret == 0)
			ret = -ETIMEDOUT;
		if (lw->stream_state == STREAM_OFF)
			ret = -ENODEV;
		if (ret < 0)
			return ret;
	}

	spin_lock_irqsave(&lw->irq_lock, flags);

	frame = list_entry(lw->outqueue.next, struct logiwin_frame, frame);
	list_del(lw->outqueue.next);

	*b = frame->buf;
	if (lw->flags & LOGIWIN_FLAG_BUFFERS_AVAILABLE) {
		frame->state = FRAME_DEQUEUED;

		if (atomic_read(&frame->vma_refcnt))
			b->flags |= V4L2_BUF_FLAG_MAPPED;
		b->flags |= V4L2_BUF_FLAG_DONE;
		b->flags &= ~V4L2_BUF_FLAG_QUEUED;
	} else if (lw->flags & (LOGIWIN_FLAG_BUFFERS_DESTROY |
		   LOGIWIN_FLAG_BUFFERS_REALLOCATE)) {
		frame->state = FRAME_UNUSED;

		b->flags &= ~(V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_QUEUED);
	}

	spin_unlock_irqrestore(&lw->irq_lock, flags);

	return 0;
}

static int vidioc_cropcap(struct file *file, void *fh,
			  struct v4l2_cropcap *cropcap)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (cropcap->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	cropcap->bounds = lw->cropcap.bounds;
	LW_DBG(INFO, "bounds = { .left=%i, .top=%i, .width=%i, .height=%i }",
	       cropcap->bounds.left, cropcap->bounds.top,
	       cropcap->bounds.width, cropcap->bounds.height);

	cropcap->defrect = lw->cropcap.defrect;
	LW_DBG(INFO, "defrect = { .left=%i, .top=%i, .width=%i, .height=%i }",
	       cropcap->defrect.left, cropcap->defrect.top,
	       cropcap->defrect.width, cropcap->defrect.height);

	cropcap->pixelaspect.numerator = 1;
	cropcap->pixelaspect.denominator = 1;

	return 0;
}

static int vidioc_g_crop(struct file *file, void *fh, struct v4l2_crop *crop)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	crop->c = lw->crop;

	return 0;
}

static int vidioc_s_crop(struct file *file, void *fh,
			 const struct v4l2_crop *crop)
{
	struct logiwin *lw = fh;
	const struct v4l2_rect *c = &crop->c;

	LW_DBG(INFO, "");

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	logiwin_set_rect_parameters(&lw->lw_par,
				    c->left, c->top, c->width, c->height,
				    LOGIWIN_RECTANGLE_CROP);

	if (logiwin_set_scale(&lw->lw_par))
		return -EINVAL;

	lw->flags |= LOGIWIN_FLAG_UPDATE_REGISTERS;

	logiwin_get_rect_parameters(&lw->lw_par,
				    &lw->crop.left, &lw->crop.top,
				    &lw->crop.width, &lw->crop.height,
				    LOGIWIN_RECTANGLE_CROP);

	if (lw->flags & LOGIWIN_FLAG_HW_BUFFER_SWITCH)
		tasklet_schedule(&lw->tasklet);

	return 0;
}

static int vidioc_overlay(struct file *file, void *fh, unsigned int on)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (on == 0) {
		if (lw->stream_state != OVERLAY_STREAM_ON)
			return -EBUSY;

		logiwin_disable(lw);

		logiwin_release_overlay_buffers(lw);
	} else if (on == 1) {
		if (lw->frames != LOGIWIN_DMA_BUFFERS ||
		    lw->overlay.address[0].pa == 0)
			return -ENOMEM;

		if (lw->stream_state != STREAM_OFF)
			return -EBUSY;

		logiwin_enable(lw, OVERLAY_STREAM_ON);
	} else {
		return -EINVAL;
	}

	return 0;
}

static int vidioc_g_fbuf(struct file *file, void *fh,
			 struct v4l2_framebuffer *fb)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if ((lw->stream_state != STREAM_OFF) &&
	    (lw->stream_state != OVERLAY_STREAM_ON))
		return -EBUSY;

	fb->capability = 0;
	fb->flags = (V4L2_FBUF_FLAG_PRIMARY | V4L2_FBUF_FLAG_OVERLAY |
		     V4L2_FBUF_FLAG_GLOBAL_ALPHA);
	fb->base = (void *)lw->overlay.address[0].pa;
	fb->fmt.width = lw->pix_format.width;
	fb->fmt.height = lw->pix_format.height;
	fb->fmt.pixelformat = lw->pix_format.pixelformat;
	fb->fmt.field = lw->pix_format.field;
	fb->fmt.bytesperline = lw->pix_format.bytesperline;
	fb->fmt.sizeimage = lw->pix_format.sizeimage;
	fb->fmt.colorspace = lw->pix_format.colorspace;
	fb->fmt.priv = 0;

	return 0;
}

static int vidioc_s_fbuf(struct file *file, void *fh,
			 const struct v4l2_framebuffer *fb)
{
	struct logiwin *lw = fh;
	int i;

	LW_DBG(INFO, "");

	if (!capable(CAP_SYS_ADMIN) && !capable(CAP_SYS_RAWIO))
		return -EPERM;

	if (fb->capability != 0)
		return -EINVAL;

	if (lw->stream_state != STREAM_OFF)
		return -EBUSY;

	lw->frames = LOGIWIN_DMA_BUFFERS;

	for (i = 0; i < lw->frames; i++)
		lw->overlay.address[i].pa =
			(dma_addr_t)(fb->base + (i * fb->fmt.sizeimage));

	lw->overlay.size = fb->fmt.sizeimage;
	lw->overlay.id = 0;

	return 0;
}

static int vidioc_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (!(lw->flags & LOGIWIN_FLAG_BUFFERS_AVAILABLE) ||
	    lw->frames_queue == 0)
		return -ENOMEM;

	if (lw->stream_state != STREAM_OFF)
		return -EBUSY;

	logiwin_enable(lw, CAPTURE_STREAM_ON);

	return 0;
}

static int vidioc_streamoff(struct file *file, void *fh,
			    enum v4l2_buf_type type)
{
	struct logiwin *lw = fh;

	LW_DBG(INFO, "");

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (lw->stream_state != CAPTURE_STREAM_ON)
		return -EBUSY;

	logiwin_disable(lw);

	logiwin_empty_queues(lw);

	if (lw->flags & LOGIWIN_FLAG_BUFFERS_DESTROY) {
		logiwin_release_buffers(lw);
		lw->flags &= ~LOGIWIN_FLAG_BUFFERS_DESTROY;
	}

	return 0;
}

static long logiwin_ioctl(struct file *file, void *fh, bool valid_prio,
			  unsigned int cmd, void *arg)
{
	struct logiwin *lw = fh;
	int ret = 0;
	unsigned int id;
	union locked_ioctl {
		u32 alpha;
		u32 sync_pol;
		bool enable;
	} lio;

	LW_DBG(INFO, "");

	switch (cmd) {
	case LOGIWIN_IOCTL_FRAME_INT:
		if (atomic_read(&lw->wait_buff_switch_refcnt)) {
			ret = -EAGAIN;
			break;
		}

		atomic_inc(&lw->wait_buff_switch_refcnt);

		id = lw->overlay.id;
		ret = wait_event_interruptible_timeout(lw->wait_buff_switch,
						       (id != lw->overlay.id),
						       HZ);
		if (ret == 0)
			ret = -ETIMEDOUT;
		else if (ret > 0)
			ret = 0;

		atomic_dec(&lw->wait_buff_switch_refcnt);
		break;

	case LOGIWIN_IOCTL_RESOLUTION_INT:
		if (atomic_read(&lw->wait_resolution_refcnt)) {
			ret = -EAGAIN;
			break;
		}

		atomic_inc(&lw->wait_resolution_refcnt);

		ret = wait_event_interruptible(lw->wait_resolution,
					       (lw->flags &
					       LOGIWIN_FLAG_RESOLUTION));
		lw->flags &= ~LOGIWIN_FLAG_RESOLUTION;

		atomic_dec(&lw->wait_resolution_refcnt);
		break;

	case LOGIWIN_IOCTL_OVERLAY_BUFFER_SWITCH:
		mutex_lock(&lw->ioctl_lock);
		lio.enable = *((bool *)arg);
		if (lio.enable)
			lw->flags |= LOGIWIN_FLAG_OVERLAY_BUFFER_SWITCH;
		else
			lw->flags &= ~LOGIWIN_FLAG_OVERLAY_BUFFER_SWITCH;
		mutex_unlock(&lw->ioctl_lock);
		break;

	case LOGIWIN_IOCTL_RESOLUTION_GET:
		memcpy(arg, &lw->lw_par.bounds, sizeof(struct v4l2_rect));
		break;

	case LOGIWIN_IOCTL_SWIZZLE:
		mutex_lock(&lw->ioctl_lock);
		lio.enable = *((bool *)arg);
		if (lio.enable)
			logiwin_operation(&lw->lw_par, LOGIWIN_OP_SWIZZLE,
					  LOGIWIN_OP_FLAG_ENABLE);
		else
			logiwin_operation(&lw->lw_par, LOGIWIN_OP_SWIZZLE,
					  LOGIWIN_OP_FLAG_DISABLE);
		mutex_unlock(&lw->ioctl_lock);
		break;

	case LOGIWIN_IOCTL_SYNC_POLARITY:
		mutex_lock(&lw->ioctl_lock);
		lio.sync_pol = *((u32 *)arg);
		logiwin_sync_polarity(&lw->lw_par,
				      lw->lw_par.channel_id,
				      lio.sync_pol & V4L2_DV_HSYNC_POS_POL,
				      lio.sync_pol & V4L2_DV_VSYNC_POS_POL);
		mutex_unlock(&lw->ioctl_lock);
		break;

	case LOGIWIN_IOCTL_ALPHA:
		mutex_lock(&lw->ioctl_lock);
		lio.alpha = *((u32 *)arg);
		logiwin_set_pixel_alpha(&lw->lw_par, lio.alpha);
		mutex_unlock(&lw->ioctl_lock);
		break;

	case LOGIWIN_IOCTL_FRAMES_SKIP:
		*((unsigned int *)arg) = lw->frames_skip;
		break;

	case LOGIWIN_IOCTL_FRAME_PHYS_ADDRESS:
		*((unsigned long *)arg) =
			lw->capture.address[lw->capture.id].pa;
		break;

	default:
		dev_err(lw->dev, "unknown IOCTL 0x%x: dir: %x, type: %x,"
			"nr: %x, size: %x\n", cmd,
			_IOC_DIR(cmd), _IOC_TYPE(cmd),
			_IOC_NR(cmd), _IOC_SIZE(cmd));
		ret = -EINVAL;
	}

	return ret;
}

static const struct v4l2_ioctl_ops logiwin_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = vidioc_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = vidioc_try_fmt_vid_cap,
	.vidioc_enum_fmt_vid_overlay = vidioc_enum_fmt_vid_overlay,
	.vidioc_g_fmt_vid_overlay = vidioc_g_fmt_vid_overlay,
	.vidioc_s_fmt_vid_overlay = vidioc_s_fmt_vid_overlay,
	.vidioc_try_fmt_vid_overlay = vidioc_try_fmt_vid_overlay,
	.vidioc_g_std = vidioc_g_std,
	.vidioc_s_std = vidioc_s_std,
	.vidioc_enum_input = vidioc_enum_input,
	.vidioc_g_input = vidioc_g_input,
	.vidioc_s_input = vidioc_s_input,
	.vidioc_g_parm = vidioc_g_parm,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,
	.vidioc_reqbufs = vidioc_reqbufs,
	.vidioc_querybuf = vidioc_querybuf,
	.vidioc_qbuf = vidioc_qbuf,
	.vidioc_dqbuf = vidioc_dqbuf,
	.vidioc_cropcap = vidioc_cropcap,
	.vidioc_g_crop = vidioc_g_crop,
	.vidioc_s_crop = vidioc_s_crop,
	.vidioc_overlay = vidioc_overlay,
	.vidioc_g_fbuf = vidioc_g_fbuf,
	.vidioc_s_fbuf = vidioc_s_fbuf,
	.vidioc_streamon = vidioc_streamon,
	.vidioc_streamoff = vidioc_streamoff,
	.vidioc_default = logiwin_ioctl
};

static void logiwin_init_params(struct logiwin *lw)
{
	LW_DBG(INFO, "");

	lw->lw_par.base = lw->lw_hw.reg_base;

	lw->lw_par.in.left = 0;
	lw->lw_par.in.top = 0;
	lw->lw_par.in.width = lw->lw_cfg.input_hres;
	lw->lw_par.in.height = lw->lw_cfg.input_vres;

	lw->lw_par.bounds = lw->lw_par.in;

	lw->lw_par.crop = lw->lw_par.bounds;

	lw->lw_par.out.left = 0;
	lw->lw_par.out.top = 0;
	lw->lw_par.out.width = lw->lw_cfg.output_hres;
	lw->lw_par.out.height = lw->lw_cfg.output_vres;

	lw->lw_par.output.dr_x = 0;
	lw->lw_par.output.dr_y = 0;
	lw->lw_par.output.ul_x = lw->lw_cfg.output_hres;
	lw->lw_par.output.ul_y = lw->lw_cfg.output_vres;

	lw->lw_par.out_hres = lw->lw_cfg.output_hres;
	lw->lw_par.out_vres = lw->lw_cfg.output_vres;

	lw->lw_par.hscale_step = SCALE_STEP;
	lw->lw_par.vscale_step = SCALE_STEP;

	lw->lw_par.scale_shift = 16 - lw->lw_cfg.scale_fraction_bits;

	lw->lw_par.out_align = lw->lw_cfg.out_align;
	lw->lw_par.out_align_mask = ~(lw->lw_par.out_align - 1);

	lw->lw_par.int_mask = 0xFFFF;

	lw->lw_par.input_format = lw->lw_cfg.input_format;

	lw->lw_par.brightness = 0;
	lw->lw_par.contrast = 0;
	lw->lw_par.saturation = 0;
	lw->lw_par.hue = 0;

	if (lw->lw_cfg.hw_buff_switch)
		lw->flags |= LOGIWIN_FLAG_HW_BUFFER_SWITCH;

	logiwin_set_video_norm(&lw->video_norm,
			       lw->lw_par.out_hres, lw->lw_par.out_vres);
}

static int logiwin_startup_config(struct logiwin *lw, bool weave_deinterlace)
{
	LW_DBG(INFO, "");

	logiwin_init_params(lw);

	lw->lw_par.hw_access = true;

	logiwin_set_pixel_alpha(&lw->lw_par, 0xFF);

	if (weave_deinterlace)
		logiwin_weave_deinterlace(&lw->lw_par, weave_deinterlace);

	if (logiwin_set_scale(&lw->lw_par))
		return -EINVAL;

	lw->flags &= ~LOGIWIN_FLAG_UPDATE_REGISTERS;
	lw->flags |= LOGIWIN_FLAG_DEVICE_IN_USE;

	return 0;
}

static int logiwin_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct logiwin *lw = video_get_drvdata(vdev);
	int ret;

	LW_DBG(INFO, "");

	if (lw->flags & LOGIWIN_FLAG_DEVICE_IN_USE)
		return -EBUSY;

	mutex_lock(&lw->fops_lock);

	lw->pix_format.width = lw->video_norm.width;
	lw->pix_format.height = lw->video_norm.height;
	lw->pix_format.pixelformat = lw->lw_cfg.output_format;
	lw->pix_format.field = V4L2_FIELD_NONE;
	lw->pix_format.bytesperline = lw->lw_cfg.output_hres *
				      (lw->lw_hw.bpp / 8);
	lw->pix_format.sizeimage = lw->pix_format.bytesperline *
				   lw->pix_format.height;
	lw->pix_format.colorspace = V4L2_COLORSPACE_SRGB;
	lw->pix_format.priv = 0;

	ret = logiwin_startup_config(lw, false);

	memcpy(&lw->cropcap.bounds, &lw->lw_par.bounds,
	       sizeof(struct v4l2_rect));
	memcpy(&lw->cropcap.defrect, &lw->lw_par.bounds,
	       sizeof(struct v4l2_rect));
	memcpy(&lw->crop, &lw->lw_par.crop, sizeof(struct v4l2_rect));

	lw->window.w.left = 0;
	lw->window.w.top = 0;
	lw->window.w.width = lw->video_norm.width;
	lw->window.w.height = lw->video_norm.height;
	lw->window.field = V4L2_FIELD_NONE;
	lw->window.global_alpha = lw->lw_par.alpha;

	file->private_data = lw;

	mutex_unlock(&lw->fops_lock);

	return ret;
}

static int logiwin_close(struct file *file)
{
	struct logiwin *lw = file->private_data;

	LW_DBG(INFO, "");

	mutex_lock(&lw->fops_lock);

	if (lw->stream_state != STREAM_OFF)
		logiwin_disable(lw);

	if (lw->flags & LOGIWIN_FLAG_BUFFERS_AVAILABLE) {
		mdelay(HZ / 10);
		logiwin_release_buffers(lw);
	}
	lw->flags &= ~LOGIWIN_FLAG_DEVICE_IN_USE;

	lw->lw_par.hw_access = false;

	mutex_unlock(&lw->fops_lock);

	return 0;
}

static void logiwin_vm_open(struct vm_area_struct *vma)
{
	struct logiwin_frame *frame = vma->vm_private_data;

	LW_DBG(INFO, "");

	atomic_inc(&frame->vma_refcnt);
}

static void logiwin_vm_close(struct vm_area_struct *vma)
{
	struct logiwin_frame *frame = vma->vm_private_data;

	LW_DBG(INFO, "");

	atomic_dec(&frame->vma_refcnt);
}

static struct vm_operations_struct logiwin_vm_ops = {
	.open = logiwin_vm_open,
	.close = logiwin_vm_close,
};

static int logiwin_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct logiwin *lw = file->private_data;
	unsigned long vm_flags = VM_WRITE | VM_SHARED;
	unsigned long size;
	int ret = -EINVAL;
	int i;

	LW_DBG(INFO, "");

	if (!(lw->flags & LOGIWIN_FLAG_BUFFERS_AVAILABLE))
		return -ENOMEM;

	if (mutex_lock_interruptible(&lw->fops_lock))
		return -ERESTARTSYS;

	size = vma->vm_end - vma->vm_start;
	if (size != PAGE_ALIGN(lw->frame[0].buf.length)) {
		dev_err(lw->dev, "failed size page align\n");
		goto error_unlock;
	}

	for (i = 0; i < lw->frames; i++)
		if ((lw->frame[i].buf.m.offset >> PAGE_SHIFT) == vma->vm_pgoff)
			break;

	if (i == lw->frames) {
		dev_err(lw->dev, "invalid mapping address, out of range\n");
		goto error_unlock;
	}

	if ((vma->vm_flags & vm_flags) != vm_flags) {
		dev_err(lw->dev, "failed mapping protection\n");
		goto error_unlock;
	}

	ret = remap_pfn_range(vma, vma->vm_start,
			      (lw->frame[i].buff_addr >> PAGE_SHIFT),
			      size, vma->vm_page_prot);
	if (ret) {
		dev_err(lw->dev, "failed address mapping \n");
		goto error_unlock;
	}

	vma->vm_ops = &logiwin_vm_ops;
	vma->vm_private_data = &lw->frame[i];

	logiwin_vm_open(vma);

error_unlock:
	mutex_unlock(&lw->fops_lock);

	return ret;
}

static const struct v4l2_file_operations logiwin_fops = {
	.owner = THIS_MODULE,
	.open = logiwin_open,
	.release = logiwin_close,
	.unlocked_ioctl = video_ioctl2,
	.mmap = logiwin_mmap,
};

static const struct video_device logiwin_template = {
	.name = DEVICE_NAME,
	.fops = &logiwin_fops,
	.ioctl_ops = &logiwin_ioctl_ops,
	.release = video_device_release_empty,
	.minor = -1
};

static void logiwin_tasklet(unsigned long arg)
{
	struct logiwin *lw = (struct logiwin *)arg;

	LW_DBG(INFO, "");

	if (lw->flags & LOGIWIN_FLAG_UPDATE_REGISTERS) {
		logiwin_update(lw);
		lw->flags &= ~LOGIWIN_FLAG_UPDATE_REGISTERS;
	}
}

static int logiwin_handle_buffer(struct logiwin *lw)
{
	struct logiwin_frame *frame;
	unsigned long flags;
	int ret = -ENOMEM;

	LW_DBG(INFO, "");

	spin_lock_irqsave(&lw->irq_lock, flags);

	if (!list_empty(&lw->inqueue)) {
		frame = list_entry(lw->inqueue.next,
				   struct logiwin_frame, frame);

		do_gettimeofday(&frame->buf.timestamp);
		frame->buf.bytesused = frame->buf.length;
		frame->buf.sequence = lw->frame_seq;
		frame->state = FRAME_DONE;

		list_move_tail(&frame->frame, &lw->outqueue);

		lw->frames_queue--;

		wake_up(&lw->wait_frame);

		ret = 0;
	}

	spin_unlock_irqrestore(&lw->irq_lock, flags);

	return ret;
}

static irqreturn_t logiwin_isr(int irq, void *pdev)
{
	struct logiwin *lw = (struct logiwin *)pdev;
	u32 isr = logiwin_int_stat_get(&lw->lw_par);
	struct logiwin_dma *address;
	unsigned int id;
	bool next_buff = true;

	LW_DBG(INFO, "");

	logiwin_int_stat_clear(&lw->lw_par, isr);

	if (isr & LOGIWIN_INT_RESOLUTION)
		lw->flags |= (LOGIWIN_FLAG_UPDATE_REGISTERS |
			      LOGIWIN_FLAG_RESOLUTION_CHANGE);

	if (!(lw->flags & LOGIWIN_FLAG_HW_BUFFER_SWITCH) &&
	    (isr & LOGIWIN_INT_FRAME_START)) {
		if (lw->stream_state == CAPTURE_STREAM_ON) {
			address = lw->capture.address;
			id = logiwin_get_buf(lw);
			if (logiwin_handle_buffer(lw)) {
				next_buff = false;
				lw->frames_skip++;
			}
		} else if (lw->stream_state == OVERLAY_STREAM_ON) {
			if (lw->flags & LOGIWIN_FLAG_OVERLAY_BUFFER_SWITCH) {
				address = lw->overlay.address;
				id = logiwin_get_buf(lw);
			} else {
				next_buff = false;
			}
		}
		lw->frame_seq++;

		if (next_buff && lw->frames > 1) {
			if (address[id].pa)
				logiwin_set_memory_offset(&lw->lw_par,
							  address[id].pa,
							  address[id].pa);
		}

		if (lw->stream_state == OVERLAY_STREAM_ON)
			wake_up_interruptible(&lw->wait_buff_switch);
	}

	tasklet_schedule(&lw->tasklet);

	return IRQ_HANDLED;
}

static int logiwin_get_config(struct platform_device *pdev,
			      struct logiwin_config *lw_cfg)
{
	struct device_node *dn = pdev->dev.of_node;
	const char *s;
	int ret;

	LW_DBG(INFO, "");

	if (!of_property_read_u32_array(dn, "vmem-address",
					&lw_cfg->vmem_addr_start, 2))
		lw_cfg->vmem_addr_end += lw_cfg->vmem_addr_start;

	ret = of_property_read_u32(dn, "input-num", &lw_cfg->input_num);
	if (ret)
		goto logiwin_get_config_error;

	ret = of_property_read_string(dn, "input-format", &s);
	if (ret) {
		goto logiwin_get_config_error;
	} else {
		if (!strcmp(s, "dvi"))
			lw_cfg->input_format = LOGIWIN_FORMAT_INPUT_DVI;
		else if (!strcmp(s, "itu"))
			lw_cfg->input_format = LOGIWIN_FORMAT_INPUT_ITU;
		else if (!strcmp(s, "rgb"))
			lw_cfg->input_format = LOGIWIN_FORMAT_INPUT_RGB;
	}

	ret = of_property_read_u32_array(dn, "input-resolution",
					 &lw_cfg->input_hres, 2);
	if (ret)
		goto logiwin_get_config_error;

	ret = of_property_read_string(dn, "output-format", &s);
	if (ret) {
		goto logiwin_get_config_error;
	} else {
		if (!strcmp(s, "rgb565"))
			lw_cfg->output_format = V4L2_PIX_FMT_RGB565;
		else if (!strcmp(s, "argb8888"))
			lw_cfg->output_format = V4L2_PIX_FMT_RGB32;
		else if (!strcmp(s, "yuyv"))
			lw_cfg->output_format = V4L2_PIX_FMT_YUYV;
	}

	ret = of_property_read_u32_array(dn, "output-resolution",
					 &lw_cfg->output_hres, 2);
	if (ret)
		goto logiwin_get_config_error;

	ret = of_property_read_u32(dn, "output-byte-align", &lw_cfg->out_align);
	if (ret)
		goto logiwin_get_config_error;

	ret = of_property_read_u32(dn, "scale-fraction-bits",
				   &lw_cfg->scale_fraction_bits);
	if (ret)
		goto logiwin_get_config_error;

	if (of_property_read_bool(dn, "hw-buffer-switch"))
		lw_cfg->hw_buff_switch = true;

	return 0;

logiwin_get_config_error:
	return ret;
}

static int logiwin_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *iomem;
	struct logiwin *lw;
	struct logiwin_config *lw_cfg;
	struct logiwin_hw *lw_hw;
	int ret;

	LW_DBG(INFO, "");

	lw = devm_kzalloc(dev, sizeof(*lw), GFP_KERNEL);
	if (!lw) {
		ret = -ENOMEM;
		goto error_handle;
	}

	lw->dev = dev;

	lw_cfg = &lw->lw_cfg;
	lw_hw = &lw->lw_hw;

	ret = logiwin_get_config(pdev, lw_cfg);
	if (ret)
		goto error_handle;

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!iomem) {
		ret = -EINVAL;
		goto error_handle;
	}
	lw_hw->reg_pbase = iomem->start;
	lw_hw->reg_base = devm_ioremap_nocache(dev, iomem->start,
					       resource_size(iomem));
	if (!lw_hw->reg_base) {
		dev_err(dev, "failed ioremap\n");
		ret = -ENOMEM;
		goto error_handle;
	}

	lw_hw->irq = platform_get_irq(pdev, 0);
	if (lw_hw->irq < 0) {
		dev_err(dev, "failed get irq\n");
		ret = lw_hw->irq;
		goto error_handle;
	} else {
		ret = devm_request_irq(dev, lw_hw->irq, logiwin_isr,
				       IRQF_TRIGGER_HIGH, DRIVER_NAME, lw);
		if (ret) {
			dev_err(dev, "failed request irq\n");
			goto error_handle;
		}
	}

	switch (lw_cfg->output_format) {
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_YUYV:
		lw_hw->bpp = 16;
		break;
	case V4L2_PIX_FMT_RGB32:
		lw_hw->bpp = 32;
		break;
	default:
		dev_err(dev, "invalid output format\n");
		break;
	}

	if (lw_cfg->vmem_addr_start) {
		lw_hw->vmem_pbase = lw_cfg->vmem_addr_start;
		lw_hw->vmem_size = lw_cfg->output_hres * lw_cfg->output_vres *
				   (lw_hw->bpp / 8);
		if ((lw_cfg->vmem_addr_end - lw_cfg->vmem_addr_start) <
		    lw_hw->vmem_size) {
			dev_err(dev, "invalid vmem size\n");
		}
#ifdef LOGIWIN_MMAP_VMEM
		else {
			lw_hw->vmem_base = devm_ioremap(dev, lw_hw->vmem_pbase,
							lw_hw->vmem_size);
		}
		if (!lw_hw->vmem_base) {
			dev_err(dev, "failed vmem ioremap\n");
			ret = -ENOMEM;
			goto error_handle;
		}
#endif
	}

	lw->video_dev = logiwin_template;

	strlcpy(lw->v4l2_dev.name, DRIVER_NAME, sizeof(lw->v4l2_dev.name));

	ret = v4l2_device_register(NULL, &lw->v4l2_dev);
	if (ret) {
		dev_err(dev, "failed register v4l2 device\n");
		goto error_handle;
	}

	lw->video_dev.v4l2_dev = &lw->v4l2_dev;

	ret = video_register_device(&lw->video_dev, VFL_TYPE_GRABBER, -1);
	if (ret) {
		dev_err(dev, "failed register video device\n");
		goto error_handle;
	} else {
		dev_info(dev, "video device registered\n");
	}

	spin_lock_init(&lw->irq_lock);

	mutex_init(&lw->fops_lock);
	mutex_init(&lw->ioctl_lock);

	tasklet_init(&lw->tasklet, logiwin_tasklet, (unsigned long)lw);

	init_waitqueue_head(&lw->wait_buff_switch);
	init_waitqueue_head(&lw->wait_frame);
	init_waitqueue_head(&lw->wait_resolution);

	atomic_set(&lw->wait_buff_switch_refcnt, 0);
	atomic_set(&lw->wait_resolution_refcnt, 0);

	video_set_drvdata(&lw->video_dev, lw);

	platform_set_drvdata(pdev, lw);

	return 0;

error_handle:
	video_unregister_device(&lw->video_dev);

	return ret;
}

static int __exit logiwin_remove(struct platform_device *pdev)
{
	struct logiwin *lw = platform_get_drvdata(pdev);

	LW_DBG(INFO, "");

	tasklet_disable(&lw->tasklet);
	tasklet_kill(&lw->tasklet);

	video_unregister_device(&lw->video_dev);

	return 0;
}

static const struct of_device_id logiwin_of_match[] = {
	{ .compatible = "xylon,logiwin-4.00.a" },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, logiwin_of_match);

static struct platform_driver logiwin_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = logiwin_of_match,
	},
	.probe = logiwin_probe,
	.remove = logiwin_remove,
};
module_platform_driver(logiwin_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(LOGIWIN_DRIVER_DESCRIPTION);
MODULE_VERSION(LOGIWIN_DRIVER_VERSION);
