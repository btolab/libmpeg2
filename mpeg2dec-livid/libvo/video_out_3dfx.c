/* 
 *    video_out_3dfx.c
 *
 *	Copyright (C) Colin Cross Apr 2000
 *
 *  This file heavily based off of video_out_mga.c of Aaron Holtzman's
 *  mpeg2dec
 *	
 *  mpeg2dec is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  mpeg2dec is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "video_out.h"
#include "video_out_internal.h"

LIBVO_EXTERN(3dfx)

#ifdef HAVE_3DFX
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <wchar.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/extensions/xf86dga.h>
#include <X11/Xutil.h>

//#define LOG(x) syslog(LOG_USER | LOG_DEBUG,x)
#define LOG(x)

#include "drivers/3dfx.h"

static vo_info_t vo_info = 
{
	"3dfx (/dev/3dfx)",
	"3dfx",
	"Colin Cross <colin@MIT.EDU>",
	""
};

static uint32_t is_fullscreen = 1;

static uint32_t vidwidth;
static uint32_t vidheight;

static uint32_t screenwidth;
static uint32_t screenheight;
static uint32_t screendepth = 2; //Only 16bpp supported right now

static uint32_t dispwidth = 1280; // You can change these to whatever you want
static uint32_t dispheight = 720; // 16:9 screen ratio??
static uint32_t dispx;
static uint32_t dispy;

static uint32_t *vidpage0;
static uint32_t *vidpage1;
static uint32_t *vidpage2;

static uint32_t vidpage0offset;
static uint32_t vidpage1offset;
static uint32_t vidpage2offset;

// Current pointer into framebuffer where display is located
static uint32_t targetoffset;

static uint32_t page_space;

static voodoo_io_reg *reg_IO;
static voodoo_2d_reg *reg_2d;
static voodoo_yuv_reg *reg_YUV;
static voodoo_yuv_fb *fb_YUV;

static uint32_t *memBase0, *memBase1;
static uint32_t baseAddr0, baseAddr1;


/* X11 related variables */
static Display *display;
static Window mywindow;
static int bpp;
static XWindowAttributes attribs;
static int X_already_started = 0;


static void 
restore(void) 
{
	//reg_IO->vidDesktopStartAddr = vidpage0offset;
	XF86DGADirectVideo(display,0,0);
}

static void 
sighup(int foo) 
{
	//reg_IO->vidDesktopStartAddr = vidpage0offset;
	XF86DGADirectVideo(display,0,0);
	exit(0);
}

static void 
restore_regs(voodoo_2d_reg *regs) 
{
	reg_2d->commandExtra = regs->commandExtra;
	reg_2d->clip0Min = regs->clip0Min;
	reg_2d->clip0Max = regs->clip0Max;

	reg_2d->srcBaseAddr = regs->srcBaseAddr;
	reg_2d->srcXY = regs->srcXY;
	reg_2d->srcFormat = regs->srcFormat;
	reg_2d->srcSize = regs->srcSize;

	reg_2d->dstBaseAddr = regs->dstBaseAddr;
	reg_2d->dstXY = regs->dstXY;
	reg_2d->dstFormat = regs->dstFormat;

	reg_2d->dstSize = regs->dstSize;
	reg_2d->command = 0;
}

static uint32_t 
create_window(Display *display) 
{
	int screen;
	unsigned int fg, bg;
	char *hello = "I hate X11";
	XSizeHints hint;
	XVisualInfo vinfo;
	XEvent xev;

	Colormap theCmap;
	XSetWindowAttributes xswa;
	unsigned long xswamask;

	if (X_already_started)
		return -1;

	screen = DefaultScreen(display);

	hint.x = 0;
	hint.y = 10;
	hint.width = dispwidth;
	hint.height = dispheight;
	hint.flags = PPosition | PSize;

	bg = WhitePixel(display, screen);
	fg = BlackPixel(display, screen);

	XGetWindowAttributes(display, DefaultRootWindow(display), &attribs);
	bpp = attribs.depth;
	if (bpp != 16) 
	{
		fprintf(stderr,"Only 16bpp supported!");
		exit(-1);
	}

	XMatchVisualInfo(display,screen,bpp,TrueColor,&vinfo);
	printf("visual id is  %lx\n",vinfo.visualid);

	theCmap = XCreateColormap(display, RootWindow(display,screen),
			vinfo.visual, AllocNone);

	xswa.background_pixel = 0;
	xswa.border_pixel     = 1;
	xswa.colormap         = theCmap;
	xswamask = CWBackPixel | CWBorderPixel |CWColormap;


	mywindow = XCreateWindow(display, RootWindow(display,screen),
				 hint.x, hint.y, hint.width, hint.height, 4, bpp,CopyFromParent,vinfo.visual,xswamask,&xswa);

	XSelectInput(display, mywindow, StructureNotifyMask);

	/* Tell other applications about this window */

	XSetStandardProperties(display, mywindow, hello, hello, None, NULL, 0, &hint);

	/* Map window. */

	XMapWindow(display, mywindow);

	/* Wait for map. */
	do 
	{
		XNextEvent(display, &xev);
	}
	while (xev.type != MapNotify || xev.xmap.event != mywindow);

	XSelectInput(display, mywindow, NoEventMask);

	XFlush(display);
	XSync(display, False);

	X_already_started++;
	return 0;
}

static void 
dump_yuv_planar(uint32_t *y, uint32_t *u, uint32_t *v, uint32_t to, uint32_t width, uint32_t height) 
{
	// YUV conversion works like this:
	//
	//		 We write the Y, U, and V planes separately into 3dfx YUV Planar memory
	//		 region.  The nice chip then takes these and packs them into the YUYV
	//		 format in the regular frame buffer, starting at yuvBaseAddr, page 2 here.
	//		 Then we tell the 3dfx to do a Screen to Screen Stretch BLT to copy all 
	//		 of the data on page 2 onto page 1, converting it to 16 bpp RGB as it goes.
	//		 The result is a nice image on page 1 ready for display. 

	uint32_t j;
	uint32_t y_imax, uv_imax, jmax;

	reg_YUV->yuvBaseAddr = to;
	reg_YUV->yuvStride = screenwidth*2;

	LOG("video_out_3dfx: starting planar dump\n");
	jmax = height>>1; // vidheight/2, height of U and V planes
	y_imax = width; // Y plane is twice as wide as U and V planes
	uv_imax = width>>1;  // vidwidth/2/4, width of U and V planes in 32-bit words

	for (j=0;j<jmax;j++) 
	{
		//XXX this should be hand-rolled 32 bit memcpy for safeness.
		memcpy(fb_YUV->U + (uint32_t) VOODOO_YUV_STRIDE*  j       ,((uint8_t*) u) + uv_imax*  j       , uv_imax);
		memcpy(fb_YUV->V + (uint32_t) VOODOO_YUV_STRIDE*  j       ,((uint8_t*) v) + uv_imax*  j       , uv_imax);
		memcpy(fb_YUV->Y + (uint32_t) VOODOO_YUV_STRIDE* (j<<1)   ,((uint8_t*) y) + y_imax * (j<<1)   , y_imax);
		memcpy(fb_YUV->Y + (uint32_t) VOODOO_YUV_STRIDE*((j<<1)+1),((uint8_t*) y) + y_imax *((j<<1)+1), y_imax);
	}
  LOG("video_out_3dfx: done planar dump\n");
}

static void 
screen_to_screen_stretch_blt(uint32_t to, uint32_t from, uint32_t width, uint32_t height) 
{
	//FIXME - this function should be called by a show_frame function that
	//        uses a series of blts to show only those areas not covered
	//        by another window
	voodoo_2d_reg saved_regs;

	LOG("video_out_3dfx: saving registers\n");
	// Save VGA regs (so X kinda works when we're done)
	saved_regs = *reg_2d;

	/* The following lines set up the screen to screen stretch blt from page2 to
		 page 1
	*/

	LOG("video_out_3dfx: setting blt registers\n");
	reg_2d->commandExtra = 4; //disable colorkeying, enable wait for v-refresh (0100b)
	reg_2d->clip0Min = 0;
	reg_2d->clip0Max = 0xFFFFFFFF; //no clipping

	reg_2d->srcBaseAddr = from;
	reg_2d->srcXY = 0;
	reg_2d->srcFormat = screenwidth*2 | VOODOO_BLT_FORMAT_YUYV; // | 1<<21;
	reg_2d->srcSize = vidwidth | (vidheight << 16);

	reg_2d->dstBaseAddr = to;
	reg_2d->dstXY = 0;
	reg_2d->dstFormat = screenwidth*2 | VOODOO_BLT_FORMAT_16;

	reg_2d->dstSize = width | (height << 16);

	LOG("video_out_3dfx: starting blt\n");
	// Executes screen to screen stretch blt
	reg_2d->command = 2 | 1<<8 | 0xCC<<24;

	LOG("video_out_3dfx: restoring regs\n");
	restore_regs(&saved_regs);

	LOG("video_out_3dfx: done blt\n");
}

static void 
update_target(void) 
{
	uint32_t xp, yp, w, h, b, d;
	Window root;

	XGetGeometry(display,mywindow,&root,&xp,&yp,&w,&h,&b,&d);
	XTranslateCoordinates(display,mywindow,root,0,0,&xp,&yp,&root);
	dispx = (uint32_t) xp;
	dispy = (uint32_t) yp;
	dispwidth = (uint32_t) w;
	dispheight = (uint32_t) h;

	if (is_fullscreen) 
		targetoffset = vidpage0offset + (screenheight - dispheight)/2*screenwidth*screendepth + (screenwidth-dispwidth)/2*screendepth;
	else 
		targetoffset = vidpage0offset + (dispy*screenwidth + dispx)*screendepth;
}

static uint32_t 
init(uint32_t width, uint32_t height, uint32_t fullscreen, char *title, uint32_t format) 
{
	int fd;
	char *name = ":0.0";
	pioData data;
	uint32_t retval;

	if(getenv("DISPLAY"))
		name = getenv("DISPLAY");
	display = XOpenDisplay(name);

	screenwidth = XDisplayWidth(display,0);
	screenheight = XDisplayHeight(display,0);

	page_space = screenwidth*screenheight*screendepth;
	vidpage0offset = 0;
	vidpage1offset = page_space;  // Use third and fourth pages
	vidpage2offset = page_space*2;

	signal(SIGALRM,sighup);
	//alarm(120);

	// Open driver device
	if ( (fd = open("/dev/3dfx",O_RDWR) ) == -1) 
	{
		fprintf(stderr,"Couldn't open /dev/3dfx\n");
		exit(1);
	}

	// Store sizes for later
	vidwidth = width;
	vidheight = height;

	is_fullscreen = fullscreen = 0;
	if (!is_fullscreen) 
		create_window(display);

	// Ask 3dfx driver for base memory address 0
	data.port = 0x10; // PCI_BASE_ADDRESS_0_LINUX;
	data.size = 4;
	data.value = &baseAddr0;
	data.device = 0;
	if ((retval = ioctl(fd,_IOC(_IOC_READ,'3',3,0),&data)) < 0) 
	{
		printf("Error: %d\n",retval);
		//return -1;
	}

	// Ask 3dfx driver for base memory address 1
	data.port = 0x14; // PCI_BASE_ADDRESS_1_LINUX;
	data.size = 4;
	data.value = &baseAddr1;
	data.device = 0;
	if ((retval = ioctl(fd,_IOC(_IOC_READ,'3',3,0),&data)) < 0) 
	{
		printf("Error: %d\n",retval);
		//return -1;
	}

	// Map all 3dfx memory areas
	memBase0 = mmap(0,0x1000000,PROT_READ | PROT_WRITE,MAP_SHARED,fd,baseAddr0);
	memBase1 = mmap(0,3*page_space,PROT_READ | PROT_WRITE,MAP_SHARED,fd,baseAddr1);
	if (memBase0 == (uint32_t *) 0xFFFFFFFF || memBase1 == (uint32_t *) 0xFFFFFFFF) 
	{
		printf("Couldn't map 3dfx memory areas: %p,%p,%d\n", 
		 memBase0,memBase1,errno);
	}  

	// Set up global pointers
	reg_IO  = (void *)memBase0 + VOODOO_IO_REG_OFFSET;
	reg_2d  = (void *)memBase0 + VOODOO_2D_REG_OFFSET;
	reg_YUV = (void *)memBase0 + VOODOO_YUV_REG_OFFSET;
	fb_YUV  = (void *)memBase0 + VOODOO_YUV_PLANE_OFFSET;

	vidpage0 = (void *)memBase1 + (unsigned long int)vidpage0offset;
	vidpage1 = (void *)memBase1 + (unsigned long int)vidpage1offset;
	vidpage2 = (void *)memBase1 + (unsigned long int)vidpage2offset;

	// Clear pages 1,2,3 
	// leave page 0, that belongs to X.
	// So does part of 1.  Oops.
	memset(vidpage1,0x00,page_space);
	memset(vidpage2,0x00,page_space);

	if (is_fullscreen) 
		memset(vidpage0,0x00,page_space);


#ifndef VOODOO_DEBUG
	// Show page 0 (unblanked)
	reg_IO->vidDesktopStartAddr = vidpage0offset;

	/* Stop X from messing with my video registers!
		 Find a better way to do this?
		 Currently I use DGA to tell XF86 to not screw with registers, but I can't really use it
		 to do FB stuff because I need to know the absolute FB position and offset FB position
		 to feed to BLT command 
	*/
	//XF86DGADirectVideo(display,0,XF86DGADirectGraphics); //| XF86DGADirectMouse | XF86DGADirectKeyb);
#endif

	/* fd is deliberately not closed - if it were, mmaps might be released??? */

	atexit(restore);

	printf("(display) 3dfx initialized %p\n",memBase1);
	return 0;
}

static const vo_info_t*
get_info(void)
{
	return &vo_info;
}

static uint32_t 
draw_frame(uint8_t *src[]) 
{
	LOG("video_out_3dfx: starting display_frame\n");

	// Put packed data onto page 2
	dump_yuv_planar((uint32_t *)src[0],(uint32_t *)src[1],(uint32_t *)src[2],
			vidpage2offset,vidwidth,vidheight);

	LOG("video_out_3dfx: done display_frame\n");
	return 0;
}

static uint32_t 
draw_slice(uint8_t *src[], uint32_t slice_num) 
{
	uint32_t target;

	target = vidpage2offset + (screenwidth*2 * 16*slice_num);
	dump_yuv_planar((uint32_t *)src[0],(uint32_t *)src[1],(uint32_t *)src[2],target,vidwidth,16);
	return 0;
}

static void 
flip_page(void) 
{
	//FIXME - update_target() should be called by event handler when window
	//        is resized or moved
	update_target();
	LOG("video_out_3dfx: calling blt function\n");
	screen_to_screen_stretch_blt(targetoffset, vidpage2offset, dispwidth, dispheight);
}

static vo_image_buffer_t* 
allocate_image_buffer()
{
	//use the generic fallback
	return allocate_image_buffer_common(vidheight,vidwidth,0x32315659);
}

static void	
free_image_buffer(vo_image_buffer_t* image)
{
	//use the generic fallback
	free_image_buffer_common(image);
}

#else /* HAVE_3DFX */

LIBVO_DUMMY_FUNCTIONS(3dfx);

#endif
