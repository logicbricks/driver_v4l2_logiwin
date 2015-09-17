/*
 * Xylon logiWIN IP core frame grabber driver test application
 * Designed for use with Xylon FG 2.0
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

#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xylonfb.h"


#define BUFFERS_NUM	3

#define LOGIWIN_DRIVER_OLD 0
#define LOGIWIN_DRIVER_NEW 1
#define LOGIWIN_DRIVER LOGIWIN_DRIVER_NEW

#if LOGIWIN_DRIVER == LOGIWIN_DRIVER_OLD
#define LOGIWIN_IOCTL_FRAME_INT		_IO ('V', BASE_VIDIOC_PRIVATE)
#define LOGIWIN_IOCTL_RESOLUTION_INT	_IO ('V', (BASE_VIDIOC_PRIVATE + 1))
#define LOGIWIN_IOCTL_RESOLUTION_GET	\
	_IOR('V', (BASE_VIDIOC_PRIVATE + 2), struct v4l2_rect)
#define LOGIWIN_IOCTL_SWIZZLE		\
	_IOW('V', (BASE_VIDIOC_PRIVATE + 3), bool)
#define LOGIWIN_IOCTL_SYNC_POLARITY	\
	_IOW('V', (BASE_VIDIOC_PRIVATE + 4), unsigned int)
#define LOGIWIN_IOCTL_ALPHA		\
	_IOW('V', (BASE_VIDIOC_PRIVATE + 5), unsigned int)
#define LOGIWIN_IOCTL_FRAMES_SKIP	\
	_IOR('V', (BASE_VIDIOC_PRIVATE + 6), unsigned int)
#define LOGIWIN_IOCTL_FRAME_PHYS_ADDRESS	\
	_IOR('V', (BASE_VIDIOC_PRIVATE + 7), unsigned long)
#endif

#if LOGIWIN_DRIVER == LOGIWIN_DRIVER_NEW
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
#endif

struct video_device
{
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;

	struct v4l2_buffer buf;
	struct v4l2_capability capability;
	struct v4l2_cropcap cropcap;
	struct v4l2_format format;
	struct v4l2_framebuffer fb;
	struct v4l2_requestbuffers rb;
	struct v4l2_rect resolution;

	char fgdev[16];
	char fbdev[16];
	void *mem[BUFFERS_NUM];

	unsigned char *pfb;
	unsigned char *framebuffer;

	int fbfd;
	int fgfd;

	int in_width;
	int in_height;
	int width;
	int height;
	int bpp;
	int bpl;

	float fps;

	unsigned long buffer_phys;
	unsigned int alpha;
	unsigned int sync_pol;
	bool console_off;
	bool run_thread_position;
	bool run_thread_scale_crop;
	bool streaming;
};

enum thread_id
{
	THREAD_POSITION,
	THREAD_SCALE_CROP,
	THREADS_NUM
};

//static pthread_mutex_t tmutex[THREADS_NUM];
static volatile bool capture_loop;
static volatile bool overlay_loop;
static volatile bool scale_crop_loop;

static unsigned char dummy_buff[1024*4*50];

static int video_enable(struct video_device *vd)
{
	int ret, type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(vd->fgfd, VIDIOC_STREAMON, &type);
	if (ret < 0)
	{
		perror("Unable to start capture");
		return ret;
	}

	vd->streaming = TRUE;

	return 0;
}

static int video_disable(struct video_device *vd)
{
	int ret, type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(vd->fgfd, VIDIOC_STREAMOFF, &type);
	if (ret < 0)
	{
		perror("Unable to stop capture");
		return ret;
	}

	vd->streaming = FALSE;

	return 0;
}

static int queue_buffers(struct video_device *vd)
{
	int i, ret;

	for (i = 0; i < vd->rb.count; ++i)
	{
		memset(&vd->buf, 0, sizeof(struct v4l2_buffer));
		vd->buf.index = i;
		vd->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vd->buf.memory = V4L2_MEMORY_MMAP;
		ret = ioctl(vd->fgfd, VIDIOC_QBUF, &vd->buf);
		if (ret < 0)
		{
			perror("Unable to queue buffer");
			return errno;
		}
	}

	return 0;
}

static int grab_frame(struct video_device *vd)
{
	int ret, skip;

	if (!vd->streaming)
		if (video_enable(vd))
			goto err;

	memset(&vd->buf, 0, sizeof(struct v4l2_buffer));
	vd->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vd->buf.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(vd->fgfd, VIDIOC_DQBUF, &vd->buf);
	if (ret < 0)
	{
		perror("Unable to dequeue buffer");
		goto err;
	}

	vd->framebuffer = (unsigned char *)vd->mem[vd->buf.index];

	ret = ioctl(vd->fgfd, VIDIOC_QBUF, &vd->buf);
	if (ret < 0)
	{
		perror("Unable to requeue buffer");
		goto err;
	}

	return 0;

err:
	skip = 0;
	if (ioctl(vd->fgfd, LOGIWIN_IOCTL_FRAMES_SKIP, &skip))
	{
		perror("Unable to get skipped frames cnt");
	}
	printf("Skipped frames: %d\n", skip);

	return -1;
}

static int videodevice_init(struct video_device *vd)
{
	struct xylonfb_layer_geometry layer_geometry;
	struct v4l2_crop crop;
	int ret = 0;
	int i;

	if ((vd->fgfd = open(vd->fgdev, O_RDWR)) == -1)
	{
		perror("XylonFG device does not exist! Exit.");
		exit(1);
	}

	memset(&vd->capability, 0, sizeof(struct v4l2_capability));
	ret = ioctl(vd->fgfd, VIDIOC_QUERYCAP, &vd->capability);
	if (ret < 0)
	{
		printf("%s unable to query device\n", vd->fgdev);
		goto error_exit;
	}

	if ((vd->capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
	{
		printf("device %s video capture not supported\n",
			vd->fgdev);
		goto error_exit;
	}

	vd->fbfd = open(vd->fbdev, O_RDWR);
	if (vd->fbfd < 0)
	{
		printf("xylonFB device does not exist! Exit.\n");
		goto error_exit;
	}

	if (ioctl(vd->fbfd, FBIOGET_FSCREENINFO, &vd->finfo))
	{
		printf("Read fb fix info failed\n");
		goto error_exit;
	}
	if (ioctl(vd->fbfd, FBIOGET_VSCREENINFO, &vd->vinfo))
	{
		printf("Read fb var info failed\n");
		goto error_exit;
	}

	vd->width = vd->vinfo.xres;
	vd->height = vd->vinfo.yres;

	memset(&layer_geometry.x, 0, sizeof(struct xylonfb_layer_geometry));
	layer_geometry.width = vd->vinfo.xres;
	layer_geometry.height = vd->vinfo.yres;
	layer_geometry.set = TRUE;
	ret = ioctl(vd->fbfd, XYLONFB_LAYER_GEOMETRY, &layer_geometry);
	if (ret)
		perror("Unable to set layer size");

	memset(&vd->format, 0, sizeof(struct v4l2_format));
	vd->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(vd->fgfd, VIDIOC_G_FMT, &vd->format);
	if (ret < 0)
	{
		perror("Unable to get format");
		goto error_exit;
	}
	vd->format.fmt.pix.width = vd->vinfo.xres;
	vd->format.fmt.pix.height = vd->vinfo.yres;
	vd->format.fmt.pix.field = V4L2_FIELD_NONE;
	ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &vd->format);
	if (ret < 0)
	{
		perror("Unable to set format");
		goto error_exit;
	}

	printf("\tFrame size: %dx%d\n", vd->width, vd->height);

	memset(&vd->cropcap, 0, sizeof(struct v4l2_cropcap));
	vd->cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(vd->fgfd, VIDIOC_CROPCAP, &vd->cropcap) < 0)
	{
		perror("Unable to get crop capabilities");
		goto error_exit;
	}

	memset(&crop, 0, sizeof(struct v4l2_crop));
	crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(vd->fgfd, VIDIOC_G_CROP, &crop);
	if (ret < 0)
	{
		perror("Unable to get crop rectangle");
		goto error_exit;
	}
	crop.c.left = 0;
	crop.c.top = 0;
	if (vd->in_width > 0 && vd->in_height > 0)
	{
		crop.c.width = vd->in_width;
		crop.c.height = vd->in_height;
	}
	else
	{
		crop.c.width = vd->width;
		crop.c.height = vd->height;
	}
	ret = ioctl(vd->fgfd, VIDIOC_S_CROP, &crop);
	if (ret < 0)
	{
		perror("Unable to set crop rectangle");
		goto error_exit;
	}

	if (vd->run_thread_position)
		return 0;

	memset(&vd->rb, 0, sizeof(struct v4l2_requestbuffers));
	vd->rb.count = BUFFERS_NUM;
	vd->rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vd->rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(vd->fgfd, VIDIOC_REQBUFS, &vd->rb);
	if (ret < 0)
	{
		perror("Unable to get buffers");
		goto error_exit;
	}

	for (i = 0; i < vd->rb.count; i++)
	{
		memset(&vd->buf, 0, sizeof(struct v4l2_buffer));
		vd->buf.index = i;
		vd->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		vd->buf.memory = V4L2_MEMORY_MMAP;
		ret = ioctl(vd->fgfd, VIDIOC_QUERYBUF, &vd->buf);
		if (ret < 0)
		{
			perror("Unable to query buffer");
			goto error_exit;
		}
		printf("V4L2 FB%d:\n%u bytes at device memory offset %u,",
			i, vd->buf.length, vd->buf.m.offset);
		vd->mem[i] = mmap(0, vd->buf.length, PROT_WRITE, MAP_SHARED,
			vd->fgfd, vd->buf.m.offset);
		if (vd->mem[i] == MAP_FAILED)
		{
			perror("Unable to map buffer");
			goto error_exit;
		}
		printf(" mapped to %p\n", vd->mem[i]);
	}

	if (queue_buffers(vd))
		goto error_exit;

	return 0;

error_exit:
	return -1;
}

static void close_v4l2(struct video_device *vd)
{
	if (vd->streaming)
		video_disable(vd);

	vd->framebuffer = NULL;
}

static int check_video_device(struct video_device *vd)
{
	int ret;

	if (vd == NULL)
		return -1;

	puts("Check device:");
	printf("\tDevice path: %s\n", vd->fgdev);
	if ((vd->fgfd = open(vd->fgdev, O_RDWR)) == -1)
	{
		perror("Error opening V4L interface");
		return -1;
	}
	memset(&vd->capability, 0, sizeof(struct v4l2_capability));
	ret = ioctl(vd->fgfd, VIDIOC_QUERYCAP, &vd->capability);
	if (ret < 0)
	{
		printf("Error opening device %s: unable to query device.\n",
			vd->fgdev);
		goto error_exit;
	}
	if ((vd->capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
	{
		printf("Error opening device %s: video capture not supported.\n",
			vd->fgdev);
	}
	if (!(vd->capability.capabilities & V4L2_CAP_STREAMING))
	{
		printf("\t%s does not support streaming\n", vd->fgdev);
	}
	if (!(vd->capability.capabilities & V4L2_CAP_READWRITE))
	{
		printf("\t%s does not support read/write\n", vd->fgdev);
	}

error_exit:
	close(vd->fgfd);

	return 0;
}

static int wait_input_resolution_change(struct video_device *vd)
{
	int ret;

	if (vd == NULL)
		return -1;

	puts("Check input resolution:");

	if ((vd->fgfd = open(vd->fgdev, O_RDWR)) == -1)
	{
		perror("XylonFG device does not exist! Exit.");
		exit(1);
	}

	memset(&vd->rb, 0, sizeof(struct v4l2_requestbuffers));
	vd->rb.count = 1;
	vd->rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vd->rb.memory = V4L2_MEMORY_MMAP;
	ret = ioctl(vd->fgfd, VIDIOC_REQBUFS, &vd->rb);
	if (ret < 0)
	{
		perror("Unable to get buffer");
		return -1;
	}

	if (queue_buffers(vd))
		return -1;

	video_enable(vd);

	usleep(100000);

	video_disable(vd);

	if (ioctl(vd->fgfd, LOGIWIN_IOCTL_RESOLUTION_GET, &vd->resolution))
	{
		perror("Unable to get input resolution");
	}
	else
	{
		printf("\t%d pixels x %d lines\n",
			vd->resolution.width, vd->resolution.height);
	}

	close(vd->fgfd);

	return 0;
}

static int init_videodevice(struct video_device *vd, int width, int height,
			    float fps, int format)
{
	if (vd == NULL)
		return -1;
	if (width == 0 || height == 0)
		return -1;

	puts("Device initialization:");
	printf("\tDevice path: %s\n", vd->fgdev);

	if (width > 0 && height > 0)
	{
		vd->in_width = width;
		vd->in_height = height;
	}
	vd->fps = fps;

	if (videodevice_init(vd) < 0)
	{
		puts("Failed! Exit.");
		goto error;
	}

	return 0;

error:
	close(vd->fgfd);

	return -1;
}

void app_exit(int sig)
{
	capture_loop = FALSE;
	overlay_loop = FALSE;
	scale_crop_loop = FALSE;
}

static void sw_buffer_switch(struct video_device *vd, int div_y)
{
	static unsigned int buff_state = 0;

	if (!vd)
	{
		buff_state = 0;
		return;
	}

	if (ioctl(vd->fgfd, LOGIWIN_IOCTL_FRAME_INT, NULL))
	{
		perror("Unable to get frame interrupt");
	}

	switch (buff_state)
	{
	case 0:
		vd->vinfo.yoffset = 0;
		buff_state = 1;
		break;
	case 1:
		vd->vinfo.yoffset = vd->vinfo.yres / div_y;
		buff_state = 2;
		break;
	case 2:
		vd->vinfo.yoffset = vd->vinfo.yres * 2 / div_y;
		buff_state = 0;
		break;
	}
	ioctl(vd->fbfd, FBIOPAN_DISPLAY, &vd->vinfo);
}

void *thread_position(void *arg)
{
	struct v4l2_format format;
	struct video_device *vd = arg;
	unsigned int overlay;
	int ret, i, j;
#if 0
	int loop_state;
#endif

	memset(&format, 0, sizeof(struct v4l2_format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(vd->fgfd, VIDIOC_G_FMT, &format);
	if (ret < 0)
	{
		perror("Unable to get capture format");
		goto thread_position_exit;
	}
#define OVERLAY_IMAGE_FULL 1
#if OVERLAY_IMAGE_FULL
	format.fmt.pix.width = vd->width;
	format.fmt.pix.height = vd->height;
#endif
#define OVERLAY_IMAGE_SCALED 0
#if OVERLAY_IMAGE_SCALED
	/* Scale image to quarter of size */
	format.fmt.pix.width = vd->width / 2;
	format.fmt.pix.height = vd->height / 2;
#endif
	ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
	if (ret < 0)
	{
		perror("Unable to set capture format");
		goto thread_position_exit;
	}

	memset(&format, 0, sizeof(struct v4l2_format));
	format.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
	ret = ioctl(vd->fgfd, VIDIOC_G_FMT, &format);
	if (ret < 0)
	{
		perror("Unable to get overlay format");
		goto thread_position_exit;
	}

	if (ioctl(vd->fgfd, VIDIOC_G_FBUF, &vd->fb))
	{
		perror("Unable to get V4L2 framebuffer");
		goto thread_position_exit;
	}
	switch (vd->fb.fmt.pixelformat)
	{
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_YUYV:
		vd->bpp = 16;
		break;
	case V4L2_PIX_FMT_RGB32:
	default:
		vd->bpp = 32;
		break;
	}
	vd->bpl = vd->fb.fmt.bytesperline;

	printf("V4L2 FB:\n"
		"\tResolution:\t%ux%u\n"
		"\tSize:\t\t%u bytes\n"
		"\tBytes Per Line:\t%u\n"
		"\tBits Per Pixel:\t%u\n"
		"\tPixel Format:\t%c%c%c%c\n"
		"\tField:\t\t%u\n"
		"\tCapability:\t0x%X\n"
		"\tFlags:\t\t0x%X\n"
		"\tColorspace:\t%X\n",
		vd->fb.fmt.width, vd->fb.fmt.height,
		vd->fb.fmt.sizeimage,
		vd->fb.fmt.bytesperline, vd->bpp,
		(char)vd->fb.fmt.pixelformat,
		(char)((vd->fb.fmt.pixelformat & 0xFF00) >> 8),
		(char)((vd->fb.fmt.pixelformat & 0xFF0000) >> 16),
		(char)((vd->fb.fmt.pixelformat & 0xFF000000) >> 24),
		vd->fb.fmt.field, vd->fb.capability, vd->fb.flags,
		vd->fb.fmt.colorspace);

	memset(vd->pfb, 0, (vd->finfo.line_length * vd->vinfo.yres_virtual));

	vd->fb.base = (void *)vd->finfo.smem_start;
	if (ioctl(vd->fgfd, VIDIOC_S_FBUF, &vd->fb))
	{
		perror("Unable to set V4L2 framebuffer");
		goto thread_position_exit;
	}

	overlay = 1;
	ret = ioctl(vd->fgfd, VIDIOC_OVERLAY, &overlay);
	if (ret < 0)
	{
		perror("Unable to start overlay");
		goto thread_position_exit;
	}
	vd->streaming = TRUE;

	overlay_loop = TRUE;
	sw_buffer_switch(NULL, 0);

	while (overlay_loop)
	{
#if 0
		loop_state = 0;

		for (i = 0; i < vd->height / 2; i += 2)
		{
			format.fmt.win.w.left = i;
			format.fmt.win.w.top = i;
			format.fmt.win.w.width =
				vd->width - format.fmt.win.w.left;
			format.fmt.win.w.height =
				vd->height - format.fmt.win.w.top;
			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			printf("Output position: %d,%d : %d,%d\n",
				format.fmt.win.w.left,
				format.fmt.win.w.top,
				format.fmt.win.w.width,
				format.fmt.win.w.height);

			sw_buffer_switch(vd, 1);
		}

		for (i = 0; ;)
		{
			format.fmt.win.w.left = i;
			format.fmt.win.w.top = i;
			format.fmt.win.w.width =
				vd->width - format.fmt.win.w.left;
			format.fmt.win.w.height =
				vd->height - format.fmt.win.w.top;
			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			printf("Output position: %d,%d : %d,%d\n",
				format.fmt.win.w.left,
				format.fmt.win.w.top,
				format.fmt.win.w.width,
				format.fmt.win.w.height);

			sw_buffer_switch(vd, 1);

			if ((loop_state == 0) && (i >= vd->height / 2))
			{
				loop_state = 1;
			}
			else if ((loop_state == 1) && (i <= 0))
			{
				loop_state = 0;
				break;
			}

			if (loop_state == 0)
				i += 2;
			else
				i -= 2;
		}

		for (i = 0, j = 0; ;)
		{
			switch (loop_state)
			{
			case 0:
				format.fmt.win.w.left = i;
				i += 5;
				if (i >= vd->width / 2)
					loop_state = 1;
				break;
			case 1:
				format.fmt.win.w.top = j;
				j += 5;
				if (j >= vd->height / 2)
					loop_state = 2;
				break;
			case 2:
				format.fmt.win.w.top = j;
				j -= 5;
				if (j < 0)
					loop_state = 3;
				break;
			case 3:
				format.fmt.win.w.left = i;
				i -= 5;
				if (i < 0)
					loop_state = 0;
				break;
			}
			format.fmt.win.w.width = vd->width;
			format.fmt.win.w.height = vd->height;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			printf("Output position: %d,%d : %d,%d\n",
				format.fmt.win.w.left,
				format.fmt.win.w.top,
				format.fmt.win.w.width,
				format.fmt.win.w.height);

			sw_buffer_switch(vd, 1);
		}

		format.fmt.win.w.left = 0;
		format.fmt.win.w.top = 0;
		for (i = vd->width, j = vd->height; ;)
		{
			switch (loop_state)
			{
			case 0:
				format.fmt.win.w.width = i;
				i -= 5;
				if (i < vd->width / 2)
					loop_state = 1;
				break;
			case 1:
				format.fmt.win.w.height = j;
				j -= 5;
				if (j < vd->height / 2)
					loop_state = 2;
				break;
			case 2:
				format.fmt.win.w.height = j;
				j += 5;
				if (j >= vd->height)
					loop_state = 3;
				break;
			case 3:
				format.fmt.win.w.width = i;
				i += 5;
				if (i >= vd->width)
					loop_state = 0;
				break;
			}

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			printf("Output position: %d,%d : %d,%d\n",
				format.fmt.win.w.left,
				format.fmt.win.w.top,
				format.fmt.win.w.width,
				format.fmt.win.w.height);

			sw_buffer_switch(vd, 1);
		}
#endif
#if OVERLAY_IMAGE_FULL
		/* Move cropped image around screen */
		format.fmt.win.w.left = 0;
		format.fmt.win.w.top = 0;
		for (i = vd->width, j = vd->height; ;)
		{
			if (i > vd->width / 2)
			{
				i -= 4;
				format.fmt.win.w.width = i;
			}
			if (j > vd->height / 2)
			{
				j -= 4;
				format.fmt.win.w.height = j;
			}

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 1);

			if ((i <= vd->width / 2) && (j <= vd->height / 2))
				break;
		}

		if(!overlay_loop)
			break;
		for (i = 0; i < vd->width / 2;)
		{
			i += 4;
			format.fmt.win.w.left = i;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 1);
		}

		if(!overlay_loop)
			break;
		for (j = 0; j < vd->height / 2;)
		{
			j += 4;
			format.fmt.win.w.top = j;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 1);
		}

		if(!overlay_loop)
			break;
		for (; i > 0;)
		{
			i -= 4;
			format.fmt.win.w.left = i;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 1);
		}

		if(!overlay_loop)
			break;
		for (; j > 0;)
		{
			j -= 4;
			format.fmt.win.w.top = j;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 1);
		}

		if(!overlay_loop)
			break;
		for (i = vd->width / 2, j = vd->height / 2; ;)
		{
			if (i < vd->width)
			{
				i += 4;
				format.fmt.win.w.width = i;
			}
			if (j < vd->height)
			{
				j += 4;
				format.fmt.win.w.height = j;
			}

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 1);

			if ((i >= vd->width) && (j >= vd->height))
				break;
		}
		if(!overlay_loop)
			break;
#endif
#if OVERLAY_IMAGE_SCALED
		/* Move scaled image around screen */
		for (i = 0; i < vd->width / 2;)
		{
			i += 5;
			format.fmt.win.w.left = i;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 2);
		}

		for (j = 0; j < vd->height / 2;)
		{
			j += 5;
			format.fmt.win.w.top = j;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 2);
		}

		for (; i > 0;)
		{
			i -= 5;
			format.fmt.win.w.left = i;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 2);
		}

		for (; j > 0;)
		{
			j -= 5;
			format.fmt.win.w.top = j;

			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set overlay format");

			sw_buffer_switch(vd, 2);
		}
#endif
	}

	overlay = 0;
	ret = ioctl(vd->fgfd, VIDIOC_OVERLAY, &overlay);
	if (ret < 0)
	{
		perror("Unable to stop overlay");
	}
	vd->streaming = FALSE;

thread_position_exit:
	puts("Position thread terminated.");
	pthread_exit(NULL);
}

void *thread_scale_crop(void *arg)
{
	struct v4l2_crop crop;
	struct v4l2_format format;
	struct xylonfb_layer_geometry layer_geometry;
	struct video_device *vd = arg;
	int width, height, scale_width, scale_height;
	int ret;
	unsigned char crop_state;
	bool scale, down;

	memset(&format, 0, sizeof(struct v4l2_format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(vd->fgfd, VIDIOC_G_FMT, &format);
	if (ret < 0)
	{
		perror("Unable to get format");
		goto thread_scale_crop_exit;
	}

	memset(&crop, 0, sizeof(crop));
	crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(vd->fgfd, VIDIOC_G_CROP, &crop);
	if (ret < 0)
	{
		perror("Unable to get crop rectangle");
		goto thread_scale_crop_exit;
	}

	width = vd->width;
	height = vd->height;
	scale_width = width / 4;
	scale_height = height / 4;

	scale = 1;
	down = 1;
	crop_state = 0;
	scale_crop_loop = TRUE;
	while (scale_crop_loop)
	{
		//pthread_mutex_lock(&tmutex[THREAD_SCALE_CROP]);

		if (scale)
		{
			if ((width > vd->width) || (height > vd->height))
			{
				width = vd->width;
				height = vd->height;
			}

			if (down)
			{
				if ((height - scale_height) > 0)
				{
					width -= scale_width;
					height -= scale_height;
				}
				else
					down = 0;
			}
			else // up
			{
				if ((height + scale_height) < vd->height)
				{
					width += scale_width;
					height += scale_height;
				}
				else
				{
					width = vd->width;
					height = vd->height;
					down = 1;
					scale = 0;
					crop_state = 0;
				}
			}
			format.fmt.pix.width = width;
			format.fmt.pix.height = height;
			ret = ioctl(vd->fgfd, VIDIOC_S_FMT, &format);
			if (ret < 0)
				perror("Unable to set format");
			memset(&layer_geometry.x, 0,
			       sizeof(struct xylonfb_layer_geometry));
			layer_geometry.width = width;
			layer_geometry.height = height;
			layer_geometry.set = TRUE;
			ret = ioctl(vd->fbfd, XYLONFB_LAYER_GEOMETRY,
				    &layer_geometry);
			if (ret)
				perror("Unable to set layer size");

			printf("Scale output: W %d H %d\n", width, height);
			usleep(500000);
		}
		else // crop
		{
			switch (crop_state)
			{
				case 0:
					if (vd->in_width > 0 && vd->in_height > 0)
					{
						width = vd->in_width;
						height = vd->in_height;
					}
					else
					{
						width = vd->width;
						height = vd->height;
					}
					crop.c.left = 400;
					crop.c.top = 200;
					crop.c.width = width - crop.c.left;
					crop.c.height = height - crop.c.top;
					crop_state = 1;
					break;
				case 1:
					crop.c.left = width / 4;
					crop.c.top = height / 4;
					crop.c.width = width / 2;
					crop.c.height = height / 2;
					crop_state = 2;
					break;
				case 2:
					crop.c.left = width / 2;
					crop.c.top = height / 2;
					crop.c.width = width / 2;
					crop.c.height = height / 2;
					crop_state = 3;
					break;
				case 3:
					crop.c.left = 0;
					crop.c.top = 0;
					crop.c.width = width;
					crop.c.height = height;
					scale = 1;
					break;
			}
			ret = ioctl(vd->fgfd, VIDIOC_S_CROP, &crop);
			if (ret < 0)
				perror("Unable to set crop");

			printf("Crop output: X %d Y %d W %d H %d\n",
				crop.c.left, crop.c.top,
				crop.c.width, crop.c.height);
			sleep(2);
		}

		//pthread_mutex_unlock(&tmutex[THREAD_SCALE_CROP]);
	}

thread_scale_crop_exit:
	puts("Scale-Crop thread terminated.");
	pthread_exit(NULL);
}

static void configure_fbdev(struct video_device *vd, bool exit)
{
#define LOGICVC_BASE_LAYER_CTRL_REG_ADDR 0x138
	struct xylonfb_hw_access hw_access;
	struct xylonfb_layer_color layer_color;
	unsigned int layer_id;
	int fd;

	if (vd->console_off && !exit)
	{
		fd = open("/dev/fb0", O_RDWR);
		if (fd > 0)
		{
			if (ioctl(fd, XYLONFB_LAYER_IDX, &layer_id))
			{
				perror("IOCTL Error");
				return;
			}

			hw_access.offset = LOGICVC_BASE_LAYER_CTRL_REG_ADDR +
					   (layer_id * 0x80);
			hw_access.set = FALSE;
			if (ioctl(fd, XYLONFB_HW_ACCESS, &hw_access))
			{
				perror("IOCTL Error");
			}
			else
			{
				hw_access.value &= ~0x01;
				hw_access.set = TRUE;
				if (ioctl(fd, XYLONFB_HW_ACCESS, &hw_access))
				{
					perror("IOCTL Error");
				}
			}

			close(fd);
		}
		else
		{
			perror("Error opening /dev/fb0\n");
		}
	}
	else if (vd->console_off && exit)
	{
		fd = open("/dev/fb0", O_RDWR);
		if (fd > 0)
		{
			if (ioctl(fd, XYLONFB_LAYER_IDX, &layer_id))
			{
				perror("IOCTL Error");
				return;
			}

			hw_access.offset = LOGICVC_BASE_LAYER_CTRL_REG_ADDR +
					   (layer_id * 0x80);
			hw_access.set = FALSE;
			if (ioctl(fd, XYLONFB_HW_ACCESS, &hw_access))
			{
				perror("IOCTL Error");
			}
			else
			{
				hw_access.value |= 0x01;
				hw_access.set = TRUE;
				if (ioctl(fd, XYLONFB_HW_ACCESS, &hw_access))
					perror("IOCTL Error");
			}

			close(fd);
		}
		else
		{
			perror("Error opening /dev/fb0\n");
		}
	}

	if (!exit)
	{
		//http://www.mikekohn.net/file_formats/yuv_rgb_converter.php
		//layer_color.raw_rgb = 0xFF1DFF6B; //RGB to YUV blue
		layer_color.raw_rgb = 0xFF0000FF; //Blue
		layer_color.use_raw = 1;
		if (ioctl(vd->fbfd, XYLONFB_BACKGROUND_COLOR, &layer_color))
		{
			perror("IOCTL Error");
		}
	}
}

int main(int argc, char *argv[])
{
	struct video_device *vdev;
	const char *fgdev = NULL;
	const char *fbdev = NULL;
	unsigned char *src, *dst;
	char *separateur, *sizestring;
	pthread_t thread[THREADS_NUM];
	pthread_attr_t attr;
	int i, w, h, fbsize;
	int width, height, sw_buff;
	float fps = 60.0;
	bool hw_buff, console_off, swizzle;
	bool run_thread_position, run_thread_scale_crop;

	width = height = -1;
	hw_buff = console_off = swizzle = FALSE;
	run_thread_position = run_thread_scale_crop = FALSE;

	signal(SIGINT, app_exit);

	for (i = 1; i < argc; i++)
	{
		/* skip bad arguments */
		if (argv[i] == NULL || *argv[i] == 0 || *argv[i] != '-')
		{
			continue;
		}
		if (strcmp(argv[i], "-d") == 0)
		{
			if (i + 1 >= argc)
			{
				puts("No parameter specified with -d, aborting.");
				exit(1);
			}
			fgdev = strdup(argv[i + 1]);
		}
		if (strcmp(argv[i], "-o") == 0)
		{
			if (i + 1 >= argc)
			{
				puts("No parameter specified with -o, aborting.");
				exit(1);
			}
			fbdev = strdup(argv[i + 1]);
		}
		if (strcmp(argv[i], "-s") == 0)
		{
			if (i + 1 >= argc)
			{
				puts("No parameter specified with -s, aborting.");
				exit(1);
			}

			sizestring = strdup(argv[i + 1]);

			width = strtoul(sizestring, &separateur, 10);
			if (*separateur != 'x')
			{
				puts("Error in size use -s widthxheight");
				exit(1);
			}
			else
			{
				++separateur;
				height = strtoul(separateur, &separateur, 10);
				if (*separateur != 0)
					puts("Wrong separator?");
			}
		}
		if (strcmp(argv[i], "-conoff") == 0)
		{
			console_off = TRUE;
		}
		if (strcmp(argv[i], "-pos") == 0)
		{
			if (!run_thread_scale_crop)
				run_thread_position = TRUE;
		}
		if (strcmp(argv[i], "-sc") == 0)
		{
			if (!run_thread_position)
				run_thread_scale_crop = TRUE;
		}
		if (strcmp(argv[i], "-swizz") == 0)
		{
			swizzle = TRUE;
		}
		if (strcmp(argv[i], "-hw") == 0)
		{
			hw_buff = TRUE;
		}
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0)
		{
			puts("usage: fgtest [...]");
			puts("-h      print help");
			puts("-d      /dev/videoX use videoX device");
			puts("-o      /dev/fbX use fbX device");
			puts("-s      use specified input size (WxH)");
			puts("-conoff disable fb console layer");
			puts("-pos    enable position demo");
			puts("-sc     enable scale/crop demo");
			puts("-swizz  enable hw swizzle");
			puts("-hw     use hardware video buffer switching");
			exit(0);
		}
	}

	if (fgdev == NULL || *fgdev == 0)
	{
		fgdev = "/dev/video0";
	}
	if (fbdev == NULL || *fbdev == 0)
	{
		fbdev = "/dev/fb0";
	}

	vdev = (struct video_device *)calloc(1, sizeof(struct video_device));

	vdev->console_off = console_off;
	vdev->run_thread_position = run_thread_position;
	vdev->run_thread_scale_crop = run_thread_scale_crop;

	strcpy(vdev->fgdev, fgdev);
	strcpy(vdev->fbdev, fbdev);

	if (check_video_device(vdev) < 0)
		goto out1;
	puts("");

	wait_input_resolution_change(vdev);

	if (init_videodevice(vdev, width, height, fps, V4L2_PIX_FMT_RGB32) < 0)
		goto out2;
	puts("");
	printf("FB:\n"
		"\tID:\t\t%s\n"
		"\tResolution:\t%ux%u\n"
		"\tSize:\t\t%ux%u (%u bytes)\n"
		"\tBytes Per Line:\t%u\n"
		"\tBits Per Pixel:\t%u\n",
		vdev->finfo.id,
		vdev->vinfo.xres, vdev->vinfo.yres,
		vdev->vinfo.xres_virtual, vdev->vinfo.yres_virtual,
		(unsigned int)vdev->finfo.smem_len,
		(unsigned int)vdev->finfo.line_length,
		vdev->vinfo.bits_per_pixel);

	fbsize = vdev->finfo.line_length * vdev->vinfo.yres_virtual;
	vdev->pfb = (unsigned char *)mmap(NULL, fbsize,
					  (PROT_READ | PROT_WRITE),
					  MAP_SHARED, vdev->fbfd, 0);
	if ((int)vdev->pfb == -1)
	{
		printf("fb device memory mapping failed!\n");
		goto out2;
	}
	memset((void *)vdev->pfb, 0, vdev->finfo.smem_len);

	switch (vdev->format.fmt.pix.pixelformat)
	{
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_YUYV:
		vdev->bpp = 16;
		break;
	case V4L2_PIX_FMT_RGB32:
	default:
		vdev->bpp = 32;
		break;
	}
	vdev->bpl = vdev->format.fmt.pix.bytesperline;

	printf("V4L2 FB:\n"
		"\tResolution:\t%ux%u\n"
		"\tSize:\t\t%u bytes\n"
		"\tBytes Per Line:\t%u\n"
		"\tBits Per Pixel:\t%u\n"
		"\tPixel Format:\t%c%c%c%c\n"
		"\tField:\t\t%u\n"
		"\tCapability:\t0x%X\n"
		"\tFlags:\t\t0x%X\n"
		"\tColorspace:\t%X\n",
		vdev->format.fmt.pix.width, vdev->format.fmt.pix.height,
		vdev->format.fmt.pix.sizeimage,
		vdev->format.fmt.pix.bytesperline, vdev->bpp,
		(char)vdev->format.fmt.pix.pixelformat,
		(char)((vdev->format.fmt.pix.pixelformat & 0xFF00) >> 8),
		(char)((vdev->format.fmt.pix.pixelformat & 0xFF0000) >> 16),
		(char)((vdev->format.fmt.pix.pixelformat & 0xFF000000) >> 24),
		vdev->format.fmt.pix.field, vdev->fb.capability, vdev->fb.flags,
		vdev->format.fmt.pix.colorspace);

	sw_buff = 0;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	for (i = 0; i < THREADS_NUM; i++)
	{
		switch (i)
		{
		case THREAD_POSITION:
			if (!run_thread_position)
				break;

			if (pthread_create(&thread[i], &attr,
					   thread_position, (void *)vdev))
			{
				perror("Thread \"Position\" create failed");
			}
			break;
		case THREAD_SCALE_CROP:
			if (!run_thread_scale_crop)
				break;

			if (pthread_create(&thread[i], &attr,
					   thread_scale_crop, (void *)vdev))
			{
				perror("Thread \"Scale-Crop\" create failed\n");
			}
			break;
		}
	}

	pthread_attr_destroy(&attr);

	configure_fbdev(vdev, FALSE);

	capture_loop = TRUE;

	if (swizzle && ioctl(vdev->fgfd, LOGIWIN_IOCTL_SWIZZLE, &swizzle))
	{
		perror("Unable to enable swizzle");
	}

//	vdev->sync_pol = V4L2_DV_VSYNC_POS_POL | V4L2_DV_HSYNC_POS_POL;
//	if (ioctl(vdev->fgfd, LOGIWIN_IOCTL_SYNC_POLARITY, &vdev->sync_pol))
//	{
//		perror("Unable to set sync polarity");
//	}
//	vdev->alpha = 0x80;
//	if (ioctl(vdev->fgfd, LOGIWIN_IOCTL_ALPHA, &vdev->alpha))
//	{
//		perror("Unable to set alpha");
//	}

	puts("\nFrame grabbing started...");
	while (capture_loop)
	{
		if (hw_buff)
		{
			if (!vdev->streaming)
				video_enable(vdev);
		}
		else
		{
			if (vdev->run_thread_position)
			{
				sleep(1);
				continue;
			}

			if (grab_frame(vdev) < 0)
			{
				continue;
			}
			src = (unsigned char *)vdev->framebuffer;

			if(++sw_buff > 2)
				sw_buff = 0;

			vdev->vinfo.yoffset = vdev->vinfo.yres * sw_buff;

			dst = vdev->pfb +
			      (vdev->vinfo.yoffset * vdev->finfo.line_length);
			w = vdev->width * (vdev->bpp / 8);
			h = vdev->height;
			for (i = 0; i < h; i++)
			{
				memcpy(dst, src, w);
				src += vdev->bpl;
				dst += vdev->finfo.line_length;
			}

			/* flush CPU cache */
			memcpy(dummy_buff + 1, dummy_buff, sizeof(dummy_buff) - 1);

//			vdev->buffer_phys = 0;
//			if (ioctl(vdev->fgfd, LOGIWIN_IOCTL_FRAME_PHYS_ADDRESS,
//				  &vdev->buffer_phys))
//			{
//				perror("Unable to get buffer phys address");
//			}
//			printf("PHYS 0x%lX\n", vdev->buffer_phys);
			ioctl(vdev->fbfd, FBIOPAN_DISPLAY, &vdev->vinfo);
		}
	}
	puts("Frame grabbing stopped.");

	if (vdev->run_thread_position)
		pthread_join(thread[THREAD_POSITION], NULL);

	if (vdev->run_thread_scale_crop)
		pthread_join(thread[THREAD_SCALE_CROP], NULL);

	memset(vdev->pfb, 0, (vdev->finfo.line_length * vdev->vinfo.yres_virtual));

	configure_fbdev(vdev, TRUE);

	if (vdev->pfb)
		munmap((void *)vdev->pfb, fbsize);
out2:
	if (vdev->fbfd)
		close(vdev->fbfd);
out1:
	close_v4l2(vdev);
	free(vdev);

	puts("Cleanup done, exit.\n");

	return 0;
}
