/*
 *
 * mga_vid.c
 *
 * Copyright (C) 1999 Aaron Holtzman
 * 
 * Module skeleton based on gutted agpgart module by Jeff Hartmann 
 * <slicer@ionet.net>
 *
 * Matrox MGA G200/G400 YUV Video Interface module Version 0.1.0
 * 
 * BES == Back End Scaler
 * 
 * This software has been released under the terms of the GNU Public
 * license. See http://www.gnu.org/copyleft/gpl.html for details.
 */

//It's entirely possible this major conflicts with something else
/* mknod /dev/mga_vid c 178 0 */

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/videodev.h>

#include "mga_vid.h"

#ifdef CONFIG_MTRR 
#include <asm/mtrr.h>
#endif

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>

#define TRUE 1
#define FALSE 0

#define MGA_VID_MAJOR 178

#define MGA_VIDMEM_SIZE 16

#ifndef PCI_DEVICE_ID_MATROX_G200_PCI 
#define PCI_DEVICE_ID_MATROX_G200_PCI 0x0520
#endif

#ifndef PCI_DEVICE_ID_MATROX_G200_AGP 
#define PCI_DEVICE_ID_MATROX_G200_AGP 0x0521
#endif

#ifndef PCI_DEVICE_ID_MATROX_G400 
#define PCI_DEVICE_ID_MATROX_G400 0x0525
#endif

MODULE_AUTHOR("Aaron Holtzman <aholtzma@engr.uvic.ca>");


typedef struct bes_registers_s
{
	//BES Control
	uint_32 besctl;
	//BES Global control
	uint_32 besglobctl;
	//Luma control (brightness and contrast)
	uint_32 beslumactl;
	//Line pitch
	uint_32 bespitch;

	//Buffer A-1 Chroma 3 plane org
	uint_32 besa1c3org;
	//Buffer A-1 Chroma org
	uint_32 besa1corg;
	//Buffer A-1 Luma org
	uint_32 besa1org;

	//Buffer A-2 Chroma 3 plane org
	uint_32 besa2c3org;
	//Buffer A-2 Chroma org
	uint_32 besa2corg;
	//Buffer A-2 Luma org
	uint_32 besa2org;

	//Buffer B-1 Chroma 3 plane org
	uint_32 besb1c3org;
	//Buffer B-1 Chroma org
	uint_32 besb1corg;
	//Buffer B-1 Luma org
	uint_32 besb1org;

	//Buffer B-2 Chroma 3 plane org
	uint_32 besb2c3org;
	//Buffer B-2 Chroma org
	uint_32 besb2corg;
	//Buffer B-2 Luma org
	uint_32 besb2org;

	//BES Horizontal coord
	uint_32 beshcoord;
	//BES Horizontal inverse scaling [5.14]
	uint_32 beshiscal;
	//BES Horizontal source start [10.14] (for scaling)
	uint_32 beshsrcst;
	//BES Horizontal source ending [10.14] (for scaling) 
	uint_32 beshsrcend;
	//BES Horizontal source last 
	uint_32 beshsrclst;

	
	//BES Vertical coord
	uint_32 besvcoord;
	//BES Vertical inverse scaling [5.14]
	uint_32 besviscal;
	//BES Field 1 vertical source last position
	uint_32 besv1srclst;
	//BES Field 1 weight start
	uint_32 besv1wght;
	//BES Field 2 vertical source last position
	uint_32 besv2srclst;
	//BES Field 2 weight start
	uint_32 besv2wght;

} bes_registers_t;

static bes_registers_t regs;
static uint_32 mga_vid_in_use = 0;
static uint_32 is_g400 = 0;
static uint_32 vid_src_ready = 0;
static uint_32 vid_overlay_on = 0;

static uint_8 *mga_mmio_base = 0;
static uint_32 mga_mem_base = 0; 
static uint_32 mga_src_base = 0;

static struct pci_dev *pci_dev;

static struct video_window mga_win;
static mga_vid_config_t mga_config; 

//All register offsets are converted to word aligned offsets (32 bit)
//because we want all our register accesses to be 32 bits
#define VCOUNT      0x1e20

#define PALWTADD      0x3c00 // Index register for X_DATAREG port
#define X_DATAREG     0x3c0a

#define XMULCTRL      0x19
#define BPP_8         0x00
#define BPP_15        0x01
#define BPP_16        0x02
#define BPP_24        0x03
#define BPP_32_DIR    0x04
#define BPP_32_PAL    0x07

#define XCOLMSK       0x40
#define X_COLKEY      0x42
#define XKEYOPMODE    0x51
#define XCOLMSK0RED   0x52
#define XCOLMSK0GREEN 0x53
#define XCOLMSK0BLUE  0x54
#define XCOLKEY0RED   0x55
#define XCOLKEY0GREEN 0x56
#define XCOLKEY0BLUE  0x57

// Backend Scaler registers
#define BESCTL      0x3d20
#define BESGLOBCTL  0x3dc0
#define BESLUMACTL  0x3d40
#define BESPITCH    0x3d24
#define BESA1C3ORG  0x3d60
#define BESA1CORG   0x3d10
#define BESA1ORG    0x3d00
#define BESHCOORD   0x3d28
#define BESHISCAL   0x3d30
#define BESHSRCEND  0x3d3C
#define BESHSRCLST  0x3d50
#define BESHSRCST   0x3d38
#define BESV1WGHT   0x3d48
#define BESV2WGHT   0x3d4c
#define BESV1SRCLST 0x3d54
#define BESV2SRCLST 0x3d58
#define BESVISCAL   0x3d34
#define BESVCOORD   0x3d2c
#define BESSTATUS   0x3dc4

static void mga_vid_write_regs(void)
{
	//Make sure internal registers don't get updated until we're done
	writel( (readl(mga_mmio_base + VCOUNT)-1)<<16,
			mga_mmio_base + BESGLOBCTL);

	// color or coordinate keying
	writeb( XKEYOPMODE, mga_mmio_base + PALWTADD);
	writeb( mga_config.colkey_on, mga_mmio_base + X_DATAREG);
	if ( mga_config.colkey_on ) 
	{
		uint_32 r=0, g=0, b=0;

		writeb( XMULCTRL, mga_mmio_base + PALWTADD);
		switch (readb (mga_mmio_base + X_DATAREG)) 
		{
			case BPP_8:
				/* Need to look up the color index, just using
														 color 0 for now. */
			break;

			case BPP_15:
				r = mga_config.colkey_red   >> 3;
				g = mga_config.colkey_green >> 3;
				b = mga_config.colkey_blue  >> 3;
			break;

			case BPP_16:
				r = mga_config.colkey_red   >> 3;
				g = mga_config.colkey_green >> 2;
				b = mga_config.colkey_blue  >> 3;
			break;

			case BPP_24:
			case BPP_32_DIR:
			case BPP_32_PAL:
				r = mga_config.colkey_red;
				g = mga_config.colkey_green;
				b = mga_config.colkey_blue;
			break;
		}

		// Disable color keying on alpha channel 
		writeb( XCOLMSK, mga_mmio_base + PALWTADD);
		writeb( 0x00, mga_mmio_base + X_DATAREG);
		writeb( X_COLKEY, mga_mmio_base + PALWTADD);
		writeb( 0x00, mga_mmio_base + X_DATAREG);

		// Set up color key registers
		writeb( XCOLKEY0RED, mga_mmio_base + PALWTADD);
		writeb( r, mga_mmio_base + X_DATAREG);
		writeb( XCOLKEY0GREEN, mga_mmio_base + PALWTADD);
		writeb( g, mga_mmio_base + X_DATAREG);
		writeb( XCOLKEY0BLUE, mga_mmio_base + PALWTADD);
		writeb( b, mga_mmio_base + X_DATAREG);

		// Set up color key mask registers
		writeb( XCOLMSK0RED, mga_mmio_base + PALWTADD);
		writeb( 0xff, mga_mmio_base + X_DATAREG);
		writeb( XCOLMSK0GREEN, mga_mmio_base + PALWTADD);
		writeb( 0xff, mga_mmio_base + X_DATAREG);
		writeb( XCOLMSK0BLUE, mga_mmio_base + PALWTADD);
		writeb( 0xff, mga_mmio_base + X_DATAREG);
	}

	// Backend Scaler
	writel( regs.besctl,      mga_mmio_base + BESCTL); 
	if(is_g400)
		writel( regs.beslumactl,  mga_mmio_base + BESLUMACTL); 
	writel( regs.bespitch,    mga_mmio_base + BESPITCH); 

	writel( regs.besa1org,    mga_mmio_base + BESA1ORG);
	writel( regs.besa1corg,   mga_mmio_base + BESA1CORG);
	if(is_g400)
		writel( regs.besa1c3org,  mga_mmio_base + BESA1C3ORG);

	writel( regs.beshcoord,   mga_mmio_base + BESHCOORD);
	writel( regs.beshiscal,   mga_mmio_base + BESHISCAL);
	writel( regs.beshsrcst,   mga_mmio_base + BESHSRCST);
	writel( regs.beshsrcend,  mga_mmio_base + BESHSRCEND);
	writel( regs.beshsrclst,  mga_mmio_base + BESHSRCLST);
	
	writel( regs.besvcoord,   mga_mmio_base + BESVCOORD);
	writel( regs.besviscal,   mga_mmio_base + BESVISCAL);
	writel( regs.besv1srclst, mga_mmio_base + BESV1SRCLST);
	writel( regs.besv1wght,   mga_mmio_base + BESV1WGHT);
	
	//update the registers somewhere between 1 and 2 frames from now.
	writel( regs.besglobctl + ((readl(mga_mmio_base + VCOUNT)+2)<<16),
			mga_mmio_base + BESGLOBCTL);

	printk("mga_vid: wrote BES registers\n");
	printk("mga_vid: BESCTL = 0x%08x\n",
			readl(mga_mmio_base + BESCTL));
	printk("mga_vid: BESGLOBCTL = 0x%08x\n",
			readl(mga_mmio_base + BESGLOBCTL));
	printk("mga_vid: BESSTATUS= 0x%08x\n",
			readl(mga_mmio_base + BESSTATUS));
	
	//FIXME remove
	printk("besa1org = %08lx\n",regs.besa1org);
	printk("besa1corg= %08lx\n",regs.besa1corg);
	printk("besa1c3org= %08lx\n",regs.besa1c3org);
	//FIXME remove
}

static int mga_vid_set_config(mga_vid_config_t *config)
{
	int x, y, sw, sh, dw, dh;
	int besleft, bestop, ifactor, ofsleft, ofstop, baseadrofs, weight, weights;
	x = config->x_org;
	y = config->y_org;
	sw = config->src_width;
	sh = config->src_height;
	dw = config->dest_width;
	dh = config->dest_height;

	printk("mga_vid: Setting up a %dx%d+%d+%d video window (src %dx%d)\n",
	       dw, dh, x, y, sw, sh);

	//FIXME check that window is valid and inside desktop
	
	//FIXME figure out a better way to allocate memory on card
	//allocate 2 megs
	//mga_src_base = mga_mem_base + (MGA_VIDMEM_SIZE-2) * 0x100000;
	mga_src_base = (MGA_VIDMEM_SIZE-2) * 0x100000;

	
	//Setup the BES registers for a three plane 4:2:0 video source 
	
	//BES enabled, even start polarity, filtering enabled, chroma upsampling
	//enabled, 420 mode enabled, dither enabled, mirror disabled, b/w
	//disabled, blanking enabled, software field select, buffer a1 displayed
	regs.besctl = 1 + (1<<10) + (1<<11) + (1<<16) + (1<<17) + (1<<18); 

	if(is_g400)
	{
		//zoom disabled, zoom filter disabled, 420 3 plane format, proc amp
		//disabled, rgb mode disabled 
		regs.besglobctl = (1<<5);
	}
	else
	{
		//zoom disabled, zoom filter disabled, Cb samples in 0246, Cr
		//in 1357, BES register update on besvcnt
		regs.besglobctl = 0;
	}


	//Disable contrast and brightness control
	regs.besglobctl = (1<<5) + (1<<7);
	regs.beslumactl = (0x7f << 16) + (0x80<<0);
	regs.beslumactl = 0x80<<0;

	//Setup destination window boundaries
	besleft = x > 0 ? x : 0;
	bestop = y > 0 ? y : 0;
	regs.beshcoord = (besleft<<16) + (x + dw-1);
	regs.besvcoord = (bestop<<16) + (y + dh-1);
	
	//Setup source dimensions
	regs.beshsrclst  = (sw - 1) << 16;
	regs.bespitch = (sw + 31) & ~31 ; 
	
	//Setup horizontal scaling
	ifactor = ((sw-1)<<14)/(dw-1);
	ofsleft = besleft - x;
		
	regs.beshiscal = ifactor<<2;
	regs.beshsrcst = (ofsleft*ifactor)<<2;
	regs.beshsrcend = regs.beshsrcst + (((dw - ofsleft - 1) * ifactor) << 2);
	
	//Setup vertical scaling
	ifactor = ((sh-1)<<14)/(dh-1);
	ofstop = bestop - y;

	regs.besviscal = ifactor<<2;

	baseadrofs = ((ofstop*regs.besviscal)>>16)*regs.bespitch;
	regs.besa1org = (uint_32) mga_src_base + baseadrofs;

	if (is_g400) 
		baseadrofs = (((ofstop*regs.besviscal)/4)>>16)*regs.bespitch;
	else 
		baseadrofs = (((ofstop*regs.besviscal)/2)>>16)*regs.bespitch;

	regs.besa1corg = (uint_32) mga_src_base + regs.bespitch * sh + baseadrofs;
	regs.besa1c3org = regs.besa1corg + ((regs.bespitch * sh) / 4);

	weight = ofstop * (regs.besviscal >> 2);
	weights = weight < 0 ? 1 : 0;
	regs.besv1wght = (weights << 16) + ((weight & 0x3FFF) << 2);
	regs.besv1srclst = sh - 1 - (((ofstop * regs.besviscal) >> 16) & 0x03FF);

	mga_vid_write_regs();
	return 0;
}


static int mga_vid_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{

	switch(cmd) 
	{
		case MGA_VID_CONFIG:
			//FIXME remove
			printk("vcount = %d\n",readl(mga_mmio_base + VCOUNT));
			printk("mga_mmio_base = %p\n",mga_mmio_base);
			printk("mga_mem_base = %08lx\n",mga_mem_base);
			//FIXME remove

			printk("mga_vid: Received configuration\n");
			if(copy_from_user(&mga_config,(mga_vid_config_t*) arg,sizeof(mga_vid_config_t)))
			{
				printk("mga_vid: failed copy from userspace\n");
				return(-EFAULT);
			}
			if (is_g400) 
			  mga_config.card_type = MGA_G400;
			else
			  mga_config.card_type = MGA_G200;
			if (copy_to_user((mga_vid_config_t *) arg, &mga_config, sizeof(mga_vid_config_t)))
			  {
			    printk("mga_vid: failed copy to userspace\n");
			    return(-EFAULT);
			  }
			return mga_vid_set_config(&mga_config);	
		break;

		case MGA_VID_ON:
			printk("mga_vid: Video ON\n");
			vid_src_ready = 1;
			if(vid_overlay_on)
			{
				regs.besctl |= 1;
				mga_vid_write_regs();
			}
		break;

		case MGA_VID_OFF:
			printk("mga_vid: Video OFF\n");
			vid_src_ready = 0;   
			regs.besctl &= ~1;
			mga_vid_write_regs();
			break;
			
	        default:
			printk("mga_vid: Invalid ioctl\n");
			return (-EINVAL);
	}
       
	return 0;
}


static int mga_vid_find_card(void)
{
	struct pci_dev *dev = NULL;

	if((dev = pci_find_device(PCI_VENDOR_ID_MATROX, PCI_DEVICE_ID_MATROX_G400, NULL)))
	{
		is_g400 = 1;
		printk("mga_vid: Found MGA G400\n");
	}
	else if((dev = pci_find_device(PCI_VENDOR_ID_MATROX, PCI_DEVICE_ID_MATROX_G200_AGP, NULL)))
	{
		is_g400 = 0;
		printk("mga_vid: Found MGA G200 AGP\n");
	}
	else if((dev = pci_find_device(PCI_VENDOR_ID_MATROX, PCI_DEVICE_ID_MATROX_G200_PCI, NULL)))
	{
		is_g400 = 0;
		printk("mga_vid: Found MGA G200 PCI\n");
	}
	else
	{
		printk("mga_vid: No supported cards found\n");
		return FALSE;   
	}

	pci_dev = dev;
	
#if LINUX_VERSION_CODE >= 0x020300
	mga_mmio_base = ioremap_nocache(dev->resource[1].start,0x4000);
	mga_mem_base =  dev->resource[0].start;
#else
	mga_mmio_base = ioremap_nocache(dev->base_address[1] & PCI_BASE_ADDRESS_MEM_MASK,0x4000);
	mga_mem_base =  dev->base_address[0] & PCI_BASE_ADDRESS_MEM_MASK;
#endif
	printk("MMIO at 0x%p\n", mga_mmio_base);
	printk("Frame at 0x%08lX\n", mga_mem_base);
	
	return TRUE;
}


static ssize_t mga_vid_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	return -EINVAL;
}

static ssize_t mga_vid_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	return -EINVAL;
}

static int mga_vid_mmap(struct file *file, struct vm_area_struct *vma)
{

	printk("mga_vid: mapping video memory into userspace\n");
	if(remap_page_range(vma->vm_start, mga_mem_base + (MGA_VIDMEM_SIZE-2) * 0x100000,
		 vma->vm_end - vma->vm_start, vma->vm_page_prot)) 
	{
		printk("mga_vid: error mapping video memory\n");
		return(-EAGAIN);
	}

	return(0);
}

static int mga_vid_release(struct inode *inode, struct file *file)
{
	//Close the window just in case
	vid_src_ready = 0;   
	regs.besctl &= ~1;
	mga_vid_write_regs();
	mga_vid_in_use = 0;

	//FIXME put back in!
	//MOD_DEC_USE_COUNT;
	return 0;
}

static long long mga_vid_lseek(struct file *file, long long offset, int origin)
{
	return -ESPIPE;
}					 

static int mga_vid_open(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);

	if(minor != 0)
	 return(-ENXIO);

	if(mga_vid_in_use == 1) 
		return(-EBUSY);

	mga_vid_in_use = 1;
	//FIXME turn me back on!
	//MOD_INC_USE_COUNT;
	return(0);
}

static struct file_operations mga_vid_fops =
{
	mga_vid_lseek,
	mga_vid_read,
	mga_vid_write,
	NULL,
	NULL,
	mga_vid_ioctl,
	mga_vid_mmap,
	mga_vid_open,
	NULL,
	mga_vid_release
};


static long mga_v4l_read(struct video_device *v, char *buf, unsigned long count, 
	int noblock)
{
	return -EINVAL;
}

static long mga_v4l_write(struct video_device *v, const char *buf, unsigned long count, int noblock)
{
	return -EINVAL;
}

static int mga_v4l_open(struct video_device *dev, int mode)
{
	MOD_INC_USE_COUNT;
	return 0;
}

static void mga_v4l_close(struct video_device *dev)
{
	regs.besctl &= ~1;
	mga_vid_write_regs();
	vid_overlay_on = 0;
	MOD_DEC_USE_COUNT;
	return;
}

static int mga_v4l_init_done(struct video_device *dev)
{
	return 0;
}

static int mga_v4l_ioctl(struct video_device *dev, unsigned int cmd, void *arg)
{
	switch(cmd)
	{
		case VIDIOCGCAP:
		{
			struct video_capability b;
			strcpy(b.name, "Matrox G200/400");
			b.type = VID_TYPE_SCALES|VID_TYPE_OVERLAY|VID_TYPE_CHROMAKEY;
			b.channels = 0;
			b.audios = 0;
			b.maxwidth = 1024;	/* GUESS ?? */
			b.maxheight = 768;
			b.minwidth = 32;
			b.minheight = 16;	/* GUESS ?? */
			if(copy_to_user(arg,&b,sizeof(b)))
				return -EFAULT;
			return 0;
		}
		case VIDIOCGPICT:
		{
			/*
			 *	Default values.. if we can change this we
			 *	can add the feature later
			 */
			struct video_picture vp;
			vp.brightness = 0x8000;
			vp.hue = 0x8000;
			vp.colour = 0x8000;
			vp.whiteness = 0x8000;
			vp.depth = 8;
			/* Format is a guess */
			vp.palette = VIDEO_PALETTE_YUV420P;
			if(copy_to_user(arg, &vp, sizeof(vp)))
				return -EFAULT;
			return 0;
		}
		case VIDIOCSPICT:
		{
			return -EINVAL;
		}
		case VIDIOCSWIN:
		{
			struct video_window vw;
			if(copy_from_user(&vw, arg, sizeof(vw)))
				return -EFAULT;
			if(vw.x <0 || vw.y <0 || vw.width < 32 
				|| vw.height < 16)
				return -EINVAL;
			memcpy(&mga_win, &vw, sizeof(mga_win));

			mga_config.x_org = vw.x;
			mga_config.y_org = vw.y;
			mga_config.dest_width = vw.width;
			mga_config.dest_height = vw.height;

			/* 
			 * May have to add 
			 *
			 * #define VIDEO_WINDOW_CHROMAKEY 16 
			 *
			 * to <linux/videodev.h> 
			 */

			//add it here for now
			#define VIDEO_WINDOW_CHROMAKEY 16 

			if (vw.flags & VIDEO_WINDOW_CHROMAKEY)
				mga_config.colkey_on = 1;
			else 
				mga_config.colkey_on = 0;

			mga_config.colkey_red   = (vw.chromakey >> 24) & 0xFF;
			mga_config.colkey_green = (vw.chromakey >> 16) & 0xFF;
			mga_config.colkey_blue  = (vw.chromakey >> 8)  & 0xFF;
			mga_vid_set_config(&mga_config);
			return 0;
				
		}
		case VIDIOCGWIN:
		{
			if(copy_to_user(arg, &mga_win, sizeof(mga_win)))
				return -EFAULT;
			return 0;
		}
		case VIDIOCCAPTURE:
		{
			int v;
			if(copy_from_user(&v, arg, sizeof(v)))
				return -EFAULT;
			vid_overlay_on = v;
			if(vid_overlay_on && vid_src_ready)
			{
				regs.besctl |= 1;
				mga_vid_write_regs();
			}
			else
			{
				regs.besctl &= ~1;
				mga_vid_write_regs();
			}
			return 0;
		}
		default:
			return -ENOIOCTLCMD;
	}
}

static struct video_device mga_v4l_dev =
{
	"Matrox G200/G400",
	VID_TYPE_CAPTURE,
	VID_HARDWARE_BT848,		/* This is a lie for now */
	mga_v4l_open,
	mga_v4l_close,
	mga_v4l_read,
	mga_v4l_write,
	NULL,
	mga_v4l_ioctl,
	NULL,
	mga_v4l_init_done,
	NULL,
	0,
	0
};



/* 
 * Main Initialization Function 
 */


static int mga_vid_initialize(void)
{
	mga_vid_in_use = 0;

	printk( "Matrox MGA G200/G400 YUV Video interface v0.01 (c) Aaron Holtzman \n");
	if(register_chrdev(MGA_VID_MAJOR, "mga_vid", &mga_vid_fops))
	{
		printk("mga_vid: unable to get major: %d\n", MGA_VID_MAJOR);
		return -EIO;
	}

	if (!mga_vid_find_card())
	{
		printk("mga_vid: no supported devices found\n");
		unregister_chrdev(MGA_VID_MAJOR, "mga_vid");
		return -EINVAL;
	}
	
	if (video_register_device(&mga_v4l_dev, VFL_TYPE_GRABBER)<0)
	{
		printk("mga_vid: unable to register.\n");
		unregister_chrdev(MGA_VID_MAJOR, "mga_vid");
		if(mga_mmio_base)
			iounmap(mga_mmio_base);
		mga_mmio_base = 0;
		return -EINVAL;
	}

	return(0);
}

int init_module(void)
{
   return mga_vid_initialize();
}

void cleanup_module(void)
{
	video_unregister_device(&mga_v4l_dev);
	if(mga_mmio_base)
		iounmap(mga_mmio_base);

	//FIXME turn off BES
	printk("mga_vid: Cleaning up module\n");
	unregister_chrdev(MGA_VID_MAJOR, "mga_vid");
}

