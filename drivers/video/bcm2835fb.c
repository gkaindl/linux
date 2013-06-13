/*
 * Copyright (C) 2010 Broadcom
 * Copyright (C) 2013 Lubomir Rintel
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Broadcom simple framebuffer driver
 *
 * This file is derived from cirrusfb.c
 * Copyright 1999-2001 Jeff Garzik <jgarzik@pobox.com>
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/mailbox.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>

#define MBOX_CHAN_FB	1 /* for use by the frame buffer */
#define VC_PHYS		0x40000000
#define TO_VC_PHYS(a)	(0x40000000 | (a))
#define FROM_VC_PHYS(a)	(0x3fffffff & (a))

/* this data structure describes each frame buffer device we find */
struct fbinfo_s {
	u32 xres, yres, xres_virtual, yres_virtual;
	u32 pitch, bpp;
	u32 xoffset, yoffset;
	u32 base;
	u32 screen_size;
	u16 cmap[256];
} __packed;

struct bcm2835_fb {
	struct fb_info fb;
	struct fbinfo_s *info;
	struct device *mbox;
	dma_addr_t dma;
	u32 cmap[16];
};
#define to_bcm2835_fb(info)	container_of(info, struct bcm2835_fb, fb)

static int bcm2835_fb_set_bitfields(struct fb_var_screeninfo *var)
{
	memset(&var->transp, 0, sizeof(var->transp));

	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;

	switch (var->bits_per_pixel) {
	case 1:
	case 2:
	case 4:
	case 8:
		var->red.length = var->bits_per_pixel;
		var->red.offset = 0;
		var->green.length = var->bits_per_pixel;
		var->green.offset = 0;
		var->blue.length = var->bits_per_pixel;
		var->blue.offset = 0;
		break;
	case 16:
		var->red.length = 5;
		var->blue.length = 5;
		/*
		 * Green length can be 5 or 6 depending whether
		 * we're operating in RGB555 or RGB565 mode.
		 */
		if (var->green.length != 5 && var->green.length != 6)
			var->green.length = 6;
		break;
	case 24:
		var->red.length = 8;
		var->blue.length = 8;
		var->green.length = 8;
		break;
	case 32:
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->transp.length = 8;
		break;
	default:
		return -EINVAL;
		break;
	}

	/*
	 * >= 16bpp displays have separate colour component bitfields
	 * encoded in the pixel data.  Calculate their position from
	 * the bitfield length defined above.
	 */
	if (var->bits_per_pixel >= 24) {
		var->red.offset = 0;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;
		var->transp.offset = var->blue.offset + var->blue.length;
	} else if (var->bits_per_pixel >= 16) {
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
		var->transp.offset = var->red.offset + var->red.length;
	}

	return 0;
}

static int bcm2835_fb_check_var(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	int yres;

	if (!var->bits_per_pixel)
		var->bits_per_pixel = 16;

	if (bcm2835_fb_set_bitfields(var) != 0) {
		dev_err(info->dev, "invalid bits_per_pixel %d\n",
					     var->bits_per_pixel);
		return -EINVAL;
	}

	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	/* use highest possible virtual resolution */
	if (var->yres_virtual == -1) {
		var->yres_virtual = 480;

		dev_err(info->dev, "resolution set to maximum of %dx%d\n",
				     var->xres_virtual, var->yres_virtual);
	}
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;

	if (var->xoffset < 0)
		var->xoffset = 0;
	if (var->yoffset < 0)
		var->yoffset = 0;

	/* truncate xoffset and yoffset to maximum if too high */
	if (var->xoffset > var->xres_virtual - var->xres)
		var->xoffset = var->xres_virtual - var->xres - 1;
	if (var->yoffset > var->yres_virtual - var->yres)
		var->yoffset = var->yres_virtual - var->yres - 1;

	yres = var->yres;
	if (var->vmode & FB_VMODE_DOUBLE)
		yres *= 2;
	else if (var->vmode & FB_VMODE_INTERLACED)
		yres = (yres + 1) / 2;

	if (yres > 1200) {
		dev_err(info->dev, "VerticalTotal >= 1200\n");
		return -EINVAL;
	}

	return 0;
}

static int bcm2835_fb_set_par(struct fb_info *info)
{
	uint32_t val = -1;
	struct bcm2835_fb *fb = to_bcm2835_fb(info);
	int ret;

	struct fbinfo_s *fbinfo = fb->info;
	fbinfo->xres = info->var.xres;
	fbinfo->yres = info->var.yres;
	fbinfo->xres_virtual = info->var.xres_virtual;
	fbinfo->yres_virtual = info->var.yres_virtual;
	fbinfo->bpp = info->var.bits_per_pixel;
	fbinfo->xoffset = info->var.xoffset;
	fbinfo->yoffset = info->var.yoffset;
	fbinfo->base = 0x0;	/* filled in by VC */
	fbinfo->pitch = 0x0;	/* filled in by VC */

	/* ensure last write to fbinfo is visible to GPU */
	wmb();
	ret = bcm2835_mbox_io(fb->mbox, MBOX_CHAN_FB, TO_VC_PHYS(fb->dma), &val);
	rmb();
	if (ret != 0)
		return ret;

	if (val) {
		dev_err(info->dev, "Query for video memory failed\n");
		return -EIO;
	}

	fb->fb.fix.line_length = fbinfo->pitch;

	if (info->var.bits_per_pixel <= 8)
		fb->fb.fix.visual = FB_VISUAL_PSEUDOCOLOR;
	else
		fb->fb.fix.visual = FB_VISUAL_TRUECOLOR;

	fb->fb.fix.smem_start = FROM_VC_PHYS(fbinfo->base);
	fb->fb.fix.smem_len = fbinfo->pitch * fbinfo->yres_virtual;
	fb->fb.screen_size = fbinfo->screen_size;
	if (fb->fb.screen_base)
		iounmap(fb->fb.screen_base);
	fb->fb.screen_base = (void *)ioremap_wc(
			fb->fb.fix.smem_start,
			fb->fb.screen_size);
	if (!fb->fb.screen_base) {
		dev_err(info->dev, "Could not ioremap video memory\n");
		return -EIO;
	}

	return 0;
}

static inline u32 convert_bitfield(int val, struct fb_bitfield *bf)
{
	unsigned int mask = (1 << bf->length) - 1;

	return (val >> (16 - bf->length) & mask) << bf->offset;
}


static int bcm2835_fb_setcolreg(unsigned int regno, unsigned int red,
				unsigned int green, unsigned int blue,
				unsigned int transp, struct fb_info *info)
{
	struct bcm2835_fb *fb = to_bcm2835_fb(info);
	if (info->screen_base == NULL)
		return 1;

	if (fb->fb.var.bits_per_pixel <= 8) {
		if (regno < 256) {
			/* blue [0:4], green [5:10], red [11:15] */
			fb->info->cmap[regno] = ((red   >> (11)) & 0x1f) << 11 |
					    ((green >> (10)) & 0x3f) << 5 |
					    ((blue  >> (11)) & 0x1f) << 0;
		}
		/* Hack: we need to tell GPU the palette has changed, but
		 * currently bcm2835_fb_set_par takes noticable time when
		 * called for every (256) colour. So just call it for what
		 * looks like the last colour in a list for now. */
		if (regno == 15 || regno == 255)
			bcm2835_fb_set_par(info);
	} else if (regno < 16) {
		fb->cmap[regno] = convert_bitfield(transp, &fb->fb.var.transp) |
		    convert_bitfield(blue, &fb->fb.var.blue) |
		    convert_bitfield(green, &fb->fb.var.green) |
		    convert_bitfield(red, &fb->fb.var.red);
	}
	return regno > 255;
}

static int bcm2835_fb_blank(int blank_mode, struct fb_info *info)
{
	return -1;
}

static void bcm2835_fb_fillrect(struct fb_info *info,
				const struct fb_fillrect *rect)
{
	if (info->screen_base == NULL)
		return;
	cfb_fillrect(info, rect);
}

static void bcm2835_fb_copyarea(struct fb_info *info,
				const struct fb_copyarea *region)
{
	if (info->screen_base == NULL)
		return;
	cfb_copyarea(info, region);
}

static void bcm2835_fb_imageblit(struct fb_info *info,
				 const struct fb_image *image)
{
	if (info->screen_base == NULL)
		return;
	cfb_imageblit(info, image);
}

static struct fb_ops bcm2835_fb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = bcm2835_fb_check_var,
	.fb_set_par = bcm2835_fb_set_par,
	.fb_setcolreg = bcm2835_fb_setcolreg,
	.fb_blank = bcm2835_fb_blank,
	.fb_fillrect = bcm2835_fb_fillrect,
	.fb_copyarea = bcm2835_fb_copyarea,
	.fb_imageblit = bcm2835_fb_imageblit,
};

static int fbwidth = 640;	/* module parameter */
static int fbheight = 480;	/* module parameter */
static int fbdepth = 16;	/* module parameter */

static int bcm2835_fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *mbox;
	struct bcm2835_fb *fb;
	dma_addr_t dma;
	void *mem;
	int ret;

	fb = devm_kzalloc(dev, sizeof(struct bcm2835_fb), GFP_KERNEL);
	if (!fb) {
		dev_err(dev, "could not allocate new bcm2835_fb struct\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, fb);

	ret = bcm2835_mbox_init(&mbox);
	if (ret != 0)
		return ret;
	fb->mbox = mbox;

	mem = dmam_alloc_coherent(dev, PAGE_ALIGN(sizeof(*fb->info)),
						&dma, GFP_KERNEL);
	if (!mem) {
		dev_err(dev, "unable to allocate fbinfo buffer\n");
		return -ENOMEM;
	}

	fb->info = (struct fbinfo_s *)mem;
	fb->dma = dma;

	fb->fb.fbops = &bcm2835_fb_ops;
	fb->fb.flags = FBINFO_FLAG_DEFAULT;
	fb->fb.pseudo_palette = fb->cmap;

	/* This is limited to 16 characters when displayed by X startup */
	strcpy(fb->fb.fix.id, "BCM2835 FB");

	fb->fb.fix.type = FB_TYPE_PACKED_PIXELS;
	fb->fb.fix.type_aux = 0;
	fb->fb.fix.xpanstep = 0;
	fb->fb.fix.ypanstep = 0;
	fb->fb.fix.ywrapstep = 0;
	fb->fb.fix.accel = FB_ACCEL_NONE;

	fb->fb.var.xres = fbwidth;
	fb->fb.var.yres = fbheight;
	fb->fb.var.xres_virtual = fbwidth;
	fb->fb.var.yres_virtual = fbheight;
	fb->fb.var.bits_per_pixel = fbdepth;
	fb->fb.var.vmode = FB_VMODE_NONINTERLACED;
	fb->fb.var.activate = FB_ACTIVATE_NOW;
	fb->fb.var.nonstd = 0;
	fb->fb.var.height = -1;		/* height of picture in mm    */
	fb->fb.var.width = -1;		/* width of picture in mm    */
	fb->fb.var.accel_flags = 0;

	fb->fb.monspecs.hfmin = 0;
	fb->fb.monspecs.hfmax = 100000;
	fb->fb.monspecs.vfmin = 0;
	fb->fb.monspecs.vfmax = 400;
	fb->fb.monspecs.dclkmin = 1000000;
	fb->fb.monspecs.dclkmax = 100000000;

	bcm2835_fb_set_bitfields(&fb->fb.var);

	/* Allocate colormap.  */
	fb_set_var(&fb->fb, &fb->fb.var);

	ret = register_framebuffer(&fb->fb);
	if (ret) {
		dev_err(dev, "could not register a framebuffer device\n");
		return ret;
	}

	dev_info(dev, "Broadcom BCM2835 framebuffer");
	return 0;
}

static int bcm2835_fb_remove(struct platform_device *dev)
{
	struct bcm2835_fb *fb = platform_get_drvdata(dev);

	if (fb->fb.screen_base)
		iounmap(fb->fb.screen_base);
	unregister_framebuffer(&fb->fb);

	return 0;
}

static const struct of_device_id bcm2835_fb_of_match[] = {
	{ .compatible = "brcm,bcm2835-fb", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_fb_of_match);

static struct platform_driver bcm2835_fb_driver = {
	.probe = bcm2835_fb_probe,
	.remove = bcm2835_fb_remove,
	.driver = {
		.name = "bcm2835-fb",
		.owner = THIS_MODULE,
		.of_match_table = bcm2835_fb_of_match,
	},
};
module_platform_driver(bcm2835_fb_driver);

module_param(fbwidth, int, 0644);
module_param(fbheight, int, 0644);
module_param(fbdepth, int, 0644);

MODULE_PARM_DESC(fbwidth, "Width of ARM Framebuffer");
MODULE_PARM_DESC(fbheight, "Height of ARM Framebuffer");
MODULE_PARM_DESC(fbdepth, "Bit depth of ARM Framebuffer");

MODULE_AUTHOR("Lubomir Rintel");
MODULE_DESCRIPTION("BCM2835 framebuffer driver");
MODULE_LICENSE("GPL");
