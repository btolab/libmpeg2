// fix event handling. wait for unmap before close display.

/* 
 * video_out_x11.c, X11 interface
 *
 *
 * Copyright (C) 1996, MPEG Software Simulation Group. All Rights Reserved. 
 *
 * Hacked into mpeg2dec by
 * 
 * Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * 15 & 16 bpp support added by Franck Sicard <Franck.Sicard@solsoft.fr>
 *
 * Xv image suuport by Gerd Knorr <kraxel@goldbach.in-berlin.de>
 */

#include "config.h"

#ifdef LIBVO_X11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>	// strerror
#include <errno.h>	// errno
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <inttypes.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

#include "video_out.h"
#include "video_out_internal.h"
#include "yuv2rgb.h"


#ifdef LIBVO_XSHM
/* since it doesn't seem to be defined on some platforms */
int XShmGetEventBase(Display*);
Bool XShmQueryExtension (Display *);
#endif


static struct x11_priv_s {
/* local data */
    unsigned char *ImageData;
    int image_width;
    int image_height;

/* X11 related variables */
    Display *display;
    Window window;
    GC gc;
    XVisualInfo vinfo;
    XImage *ximage;
    int depth, bpp;
    int X_already_started;	// = 0

    // XSHM
    XShmSegmentInfo Shminfo; // num_buffers
} x11_priv;

static int x11_open (void)
{
    int screen;
    XGCValues xgcv;
    Colormap theCmap;
    XSetWindowAttributes xswa;
    unsigned long xswamask;
    struct x11_priv_s *priv = &x11_priv;
    XWindowAttributes attribs;

    if (priv->X_already_started)
	return -1;

    priv->display = XOpenDisplay (NULL);
    if (! (priv->display)) {
	fprintf (stderr, "Can not open display");
	return -1;
    }

    XGetWindowAttributes (priv->display, DefaultRootWindow(priv->display), &attribs);

    priv->depth = attribs.depth;

    screen = DefaultScreen (priv->display);

    /*
     *
     * depth in X11 terminology land is the number of bits used to
     * actually represent the colour.
     *
     * bpp in X11 land means how many bits in the frame buffer per
     * pixel. 
     *
     * ex. 15 bit color is 15 bit depth and 16 bpp. Also 24 bit
     *     color is 24 bit depth, but can be 24 bpp or 32 bpp.
     */

    switch (priv->depth) {
    case 15:
    case 16:
    case 24:
    case 32:
	break;

    default:
	/* The root window may be 8bit but there might still be
	 * visuals with other bit depths. For example this is the
	 * case on Sun/Solaris machines.
	 */
	priv->depth = 24;
	break;
    }

    XMatchVisualInfo (priv->display, screen, priv->depth, TrueColor, &priv->vinfo);

    theCmap   = XCreateColormap(priv->display, RootWindow(priv->display,screen), priv->vinfo.visual, AllocNone);

    xswa.background_pixel = 0;
    xswa.border_pixel     = 1;
    xswa.colormap         = theCmap;
    xswa.event_mask = 0;
    xswamask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

    priv->window =
	XCreateWindow (priv->display,
		       RootWindow (priv->display,screen),
		       0, 0, 320, 200,
		       4, priv->depth, CopyFromParent, priv->vinfo.visual,
		       xswamask, &xswa);

    // Map window
    XMapWindow (priv->display, priv->window);

    XFlush (priv->display);
    XSync (priv->display, False);

    priv->gc = XCreateGC (priv->display, priv->window, 0L, &xgcv);

    if (XShmQueryExtension (priv->display)) {
	printf ("Using MIT Shared memory extension");
    } else {
	printf ("no shm\n");
	exit (1);
    }

    priv->X_already_started = 1;
    return 0;
}

static int _xshm_create (XShmSegmentInfo *Shminfo, int size)
{
    struct x11_priv_s *priv = &x11_priv;
	
    Shminfo->shmid = shmget (IPC_PRIVATE, size, IPC_CREAT | 0777);

    if (Shminfo->shmid < 0) {
	fprintf (stderr, "Shared memory error, disabling (seg id error: %s)", strerror (errno));
	return -1;
    }
	
    Shminfo->shmaddr = (char *) shmat(Shminfo->shmid, 0, 0);

    if (Shminfo->shmaddr == ((char *) -1)) {
	if (Shminfo->shmaddr != ((char *) -1))
	    shmdt(Shminfo->shmaddr);
	fprintf (stderr, "Shared memory error, disabling (address error)");
	return -1;
    }
		
    Shminfo->readOnly = False;
    XShmAttach(priv->display, Shminfo);

    return 0;
}

static void _xshm_destroy (XShmSegmentInfo *Shminfo)
{
    struct x11_priv_s *priv = &x11_priv;
	
    XShmDetach (priv->display, Shminfo);
    shmdt (Shminfo->shmaddr);
    shmctl (Shminfo->shmid, IPC_RMID, 0);
}

/**
 * connect to server, create and map window,
 * allocate colors and (shared) memory
 **/

static int x11_setup (vo_output_video_attr_t * vo_attr)
{
    int width, height;
    int mode;
    struct x11_priv_s *priv = &x11_priv;

    width = vo_attr->width;
    height = vo_attr->height;

    x11_open ();

    priv->image_width = width;
    priv->image_height = height;

    XResizeWindow (priv->display, priv->window, priv->image_width, priv->image_height);

    priv->ximage = XShmCreateImage (priv->display, priv->vinfo.visual, priv->depth, ZPixmap, NULL, &priv->Shminfo, width, priv->image_height);

    // If no go, then revert to normal Xlib calls.
    if (!priv->ximage) {
	fprintf (stderr, "Shared memory error, disabling (Ximage error)");
	exit (1);
    }

    if ((_xshm_create (&priv->Shminfo, priv->ximage->bytes_per_line * priv->ximage->height))) {
	XDestroyImage(priv->ximage);
	printf ("shm error\n");
	exit (1);
    }
	
    priv->ximage->data = priv->Shminfo.shmaddr;
    priv->ImageData = (unsigned char *) priv->ximage->data;

    priv->bpp = priv->ximage->bits_per_pixel;

    // If we have blue in the lowest bit then obviously RGB 
    mode = ((priv->ximage->blue_mask & 0x01)) ? MODE_RGB : MODE_BGR;

#ifdef WORDS_BIGENDIAN 
    if (priv->ximage->byte_order != MSBFirst) {
#else
    if (priv->ximage->byte_order != LSBFirst) {
#endif
	fprintf (stderr, "No support fon non-native XImage byte order");
	return -1;
    }

    yuv2rgb_init ((priv->depth == 24) ? priv->bpp : priv->depth, mode);
    return 0;
}

static int x11_close(void *plugin) 
{
    struct x11_priv_s *priv = &x11_priv;

    printf ("Closing video plugin");

    if (priv->Shminfo.shmaddr) {
	_xshm_destroy(&priv->Shminfo);
    }

    if (priv->ximage)
	XDestroyImage (priv->ximage);

    if (priv->window)
	XDestroyWindow (priv->display, priv->window);
    XCloseDisplay (priv->display);
    priv->X_already_started = 0;

    return 0;
}

static void x11_flip_page (void)
{
    struct x11_priv_s *priv = &x11_priv;

    XShmPutImage (priv->display, priv->window, priv->gc, priv->ximage, 
		  0, 0, 0, 0, priv->ximage->width, priv->ximage->height, False); 
    XFlush (priv->display);
}

static int x11_draw_slice (uint8_t *src[], int slice_num)
{
    struct x11_priv_s *priv = &x11_priv;
    uint8_t *dst;

    dst = priv->ImageData + priv->image_width * 16 * (priv->bpp/8) * slice_num;

    yuv2rgb (dst, src[0], src[1], src[2], 
	     priv->image_width, 16, 
	     priv->image_width*(priv->bpp/8), priv->image_width, priv->image_width/2 );

    return 0;
}

static int x11_draw_frame (frame_t *frame)
{
    struct x11_priv_s *priv = &x11_priv;

    yuv2rgb(priv->ImageData, frame->base[0], frame->base[1], frame->base[2],
	    priv->image_width, priv->image_height, 
	    priv->image_width*(priv->bpp/8), priv->image_width, priv->image_width/2 );

    return 0; 
}

static frame_t* x11_allocate_image_buffer (int width, int height, uint32_t format)
{
    return libvo_common_alloc (width, height);
}

void x11_free_image_buffer (frame_t* frame)
{
    libvo_common_free (frame);
}

LIBVO_EXTERN (x11,"x11")

#endif
