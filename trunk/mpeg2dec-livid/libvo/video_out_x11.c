// make sure shm is not reused while the server uses it
// get rid of memcpy in xv/xvshm

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
#include <string.h>	// memcmp, strcmp
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <inttypes.h>

#ifdef LIBVO_XSHM
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <sys/socket.h>	// getsockname, getpeername
#include <netinet/in.h>	// struct sockaddr_in
/* since it doesn't seem to be defined on some platforms */
int XShmGetEventBase (Display *);
#endif

#ifdef LIBVO_XV
#include <X11/extensions/Xvlib.h>
#define FOURCC_YV12 0x32315659
#endif

#include "video_out.h"
#include "video_out_internal.h"
#include "yuv2rgb.h"


static struct x11_priv_s {
/* local data */
    uint8_t * imagedata;
    int width;
    int height;
    int stride;

/* X11 related variables */
    Display * display;
    Window window;
    GC gc;
    XVisualInfo vinfo;
    XImage * ximage;

#ifdef LIBVO_XSHM
    XShmSegmentInfo shminfo; // num_buffers
#endif

#ifdef LIBVO_XV
    XvPortID port;
    XvImage * xvimage;
#endif
} x11_priv;

static int x11_get_visual_info (void)
{
    struct x11_priv_s * priv = &x11_priv;
    XVisualInfo visualTemplate;
    XVisualInfo * XvisualInfoTable;
    XVisualInfo * XvisualInfo;
    int number;
    int i;

    // list truecolor visuals for the default screen
    visualTemplate.class = TrueColor;
    visualTemplate.screen = DefaultScreen (priv->display);
    XvisualInfoTable =
	XGetVisualInfo (priv->display,
			VisualScreenMask | VisualClassMask, &visualTemplate,
			&number);
    if (XvisualInfoTable == NULL)
	return 1;

    // find the visual with the highest depth
    XvisualInfo = XvisualInfoTable;
    for (i = 1; i < number; i++)
	if (XvisualInfoTable[i].depth > XvisualInfo->depth)
	    XvisualInfo = XvisualInfoTable + i;

    priv->vinfo = *XvisualInfo;
    XFree (XvisualInfoTable);
    return 0;
}

static void x11_create_window (int width, int height)
{
    struct x11_priv_s * priv = &x11_priv;
    XSetWindowAttributes attr;

    attr.background_pixmap = None;
    attr.backing_store = NotUseful;
    attr.event_mask = 0;
    priv->window =
	XCreateWindow (priv->display, DefaultRootWindow (priv->display),
		       0 /* x */, 0 /* y */, width, height,
		       0 /* border_width */, priv->vinfo.depth,
		       InputOutput, priv->vinfo.visual,
		       CWBackPixmap | CWBackingStore | CWEventMask, &attr);
}

static void x11_create_gc (void)
{
    struct x11_priv_s * priv = &x11_priv;
    XGCValues gcValues;

    priv->gc = XCreateGC (priv->display, priv->window, 0, &gcValues);
}

static int x11_create_image (int width, int height)
{
    struct x11_priv_s * priv = &x11_priv;

    priv->ximage = XCreateImage (priv->display,
				 priv->vinfo.visual, priv->vinfo.depth,
				 ZPixmap, 0, NULL /* data */,
				 width, height, 16, 0);
    if (priv->ximage == NULL)
	return 1;

    priv->ximage->data =
	malloc (priv->ximage->bytes_per_line * priv->ximage->height);
    return 0;
}

static int x11_yuv2rgb_init (void)
{
    struct x11_priv_s * priv = &x11_priv;
    int mode;

    // If we have blue in the lowest bit then "obviously" RGB
    // (walken : the guy who wrote this convention never heard of endianness ?)
    mode = ((priv->ximage->blue_mask & 0x01)) ? MODE_RGB : MODE_BGR;

#ifdef WORDS_BIGENDIAN 
    if (priv->ximage->byte_order != MSBFirst)
	return 1;
#else
    if (priv->ximage->byte_order != LSBFirst)
	return 1;
#endif

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

    yuv2rgb_init (((priv->vinfo.depth == 24) ?
		   priv->ximage->bits_per_pixel : priv->vinfo.depth), mode);

    return 0;
}

static int x11_common_setup (int width, int height,
			     int (* create_image) (int, int))
{
    struct x11_priv_s * priv = &x11_priv;

    if (x11_get_visual_info ()) {
	fprintf (stderr, "No truecolor visual\n");
	return 1;
    }

    x11_create_window (width, height);
    x11_create_gc ();

    if (create_image (width, height)) {
	fprintf (stderr, "Cannot create ximage\n");
	return 1;
    }

    if (x11_yuv2rgb_init ()) {
	fprintf (stderr, "No support for non-native byte order\n");
	return 1;
    }

    // FIXME set WM_DELETE_WINDOW protocol ? to avoid shm leaks

    XMapWindow (priv->display, priv->window);

    priv->width = width;
    priv->height = height;
    priv->imagedata = (unsigned char *) priv->ximage->data;
    priv->stride = priv->ximage->bytes_per_line;
    return 0;
}

static void common_close (void)
{
    struct x11_priv_s * priv = &x11_priv;

    if (priv->window) {
	XFreeGC (priv->display, priv->gc);
	XDestroyWindow (priv->display, priv->window);
    }
    if (priv->display)
	XCloseDisplay (priv->display);
}

static int x11_setup (vo_output_video_attr_t * vo_attr)
{
    struct x11_priv_s * priv = &x11_priv;
    int width, height;

    width = vo_attr->width;
    height = vo_attr->height;

    priv->display = XOpenDisplay (NULL);
    if (! (priv->display)) {
	fprintf (stderr, "Can not open display\n");
	return 1;
    }

    return x11_common_setup (width, height, x11_create_image);
}

static int x11_close (void * dummy)
{
    struct x11_priv_s * priv = &x11_priv;

    XDestroyImage (priv->ximage);
    common_close ();
    return 0;
}

static void x11_flip_page (void)
{
    struct x11_priv_s * priv = &x11_priv;

    XPutImage (priv->display, priv->window, priv->gc, priv->ximage, 
	       0, 0, 0, 0, priv->width, priv->height);
    XFlush (priv->display);
}

static int x11_draw_slice (uint8_t *src[], int slice_num)
{
    struct x11_priv_s * priv = &x11_priv;

    yuv2rgb (priv->imagedata + priv->stride * 16 * slice_num,
	     src[0], src[1], src[2], priv->width, 16,
	     priv->stride, priv->width, priv->width >> 1);

    return 0;
}

static int x11_draw_frame (frame_t *frame)
{
    struct x11_priv_s * priv = &x11_priv;

    yuv2rgb (priv->imagedata, frame->base[0], frame->base[1], frame->base[2],
	     priv->width, priv->height,
	     priv->stride, priv->width, priv->width >> 1);

    return 0;
}

static frame_t * x11_allocate_image_buffer (int width, int height,
					    uint32_t format)
{
    return libvo_common_alloc (width, height);
}

void x11_free_image_buffer (frame_t* frame)
{
    libvo_common_free (frame);
}

vo_output_video_t video_out_x11 = {
    name: "x11",
    setup: x11_setup,
    close: x11_close,
    flip_page: x11_flip_page,
    draw_slice: x11_draw_slice,
    draw_frame: x11_draw_frame,
    allocate_image_buffer: x11_allocate_image_buffer,
    free_image_buffer: x11_free_image_buffer
};

#ifdef LIBVO_XSHM
static int x11_check_local (void)
{
    struct x11_priv_s * priv = &x11_priv;
    int fd;
    struct sockaddr_in me;
    struct sockaddr_in peer;
    int len;

    fd = ConnectionNumber (priv->display);

    len = sizeof (me);
    if (getsockname (fd, &me, &len))
	return 1;	// should not happen, assume remote display

    if (me.sin_family == PF_UNIX)
	return 0;	// display is local, using unix domain socket

    if (me.sin_family != PF_INET)
	return 1;	// unknown protocol, assume remote display

    len = sizeof (peer);
    if (getpeername (fd, &peer, &len))
	return 1;	// should not happen, assume remote display

    if (peer.sin_family != PF_INET)
	return 1;	// should not happen, assume remote display

    if (memcmp (&(me.sin_addr), &(peer.sin_addr), sizeof(me.sin_addr)))
	return 1;	// display is remote, using tcp/ip socket

    return 0;		// display is local, using tcp/ip socket
}

static int xshm_check_extension (void)
{
    struct x11_priv_s * priv = &x11_priv;
    int major;
    int minor;
    Bool pixmaps;

    if (XShmQueryVersion (priv->display, &major, &minor, &pixmaps) == 0)
	return 1;

    if ((major < 1) || ((major == 1) && (minor < 1)))
	return 1;

    return 0;
}

static int xshm_create_shm (int size)
{
    struct x11_priv_s * priv = &x11_priv;

    priv->shminfo.shmid = shmget (IPC_PRIVATE, size, IPC_CREAT | 0777);
    if (priv->shminfo.shmid == -1)
	return 1;

    priv->shminfo.shmaddr = shmat (priv->shminfo.shmid, 0, 0);
    if (priv->shminfo.shmaddr == (char *)-1)
	return 1;

    priv->shminfo.readOnly = True;
    if (! (XShmAttach (priv->display, &(priv->shminfo))))
	return 1;

    return 0;
}

static void xshm_destroy_shm (void)
{
    struct x11_priv_s * priv = &x11_priv;

    if (priv->shminfo.shmaddr != (char *)-1) {
	XShmDetach (priv->display, &(priv->shminfo));
	shmdt (priv->shminfo.shmaddr);
    }
    if (priv->shminfo.shmid != -1)
	shmctl (priv->shminfo.shmid, IPC_RMID, 0);
}

static int xshm_create_image (int width, int height)
{
    struct x11_priv_s * priv = &x11_priv;

    priv->ximage = XShmCreateImage (priv->display,
				    priv->vinfo.visual, priv->vinfo.depth,
				    ZPixmap, NULL /* data */,
				    &(priv->shminfo), width, height);
    if (priv->ximage == NULL)
	return 1;

    if (xshm_create_shm (priv->ximage->bytes_per_line * priv->ximage->height))
	return 1;

    priv->ximage->data = priv->shminfo.shmaddr;
    return 0;
}

static int xshm_setup (vo_output_video_attr_t * vo_attr)
{
    struct x11_priv_s * priv = &x11_priv;
    int width, height;

    width = vo_attr->width;
    height = vo_attr->height;

    priv->display = XOpenDisplay (NULL);
    if (! (priv->display)) {
	fprintf (stderr, "Can not open display\n");
	return 1;
    }

    if (x11_check_local ()) {
	fprintf (stderr, "Can not use xshm on a remote display\n");
	return 1;
    }

    if (xshm_check_extension ()) {
	fprintf (stderr, "No xshm extension\n");
	return 1;
    }

    return x11_common_setup (width, height, xshm_create_image);
}

static int xshm_close (void * dummy)
{
    struct x11_priv_s * priv = &x11_priv;

    if (priv->ximage) {
	xshm_destroy_shm ();
	XDestroyImage (priv->ximage);
    }
    common_close ();
    return 0;
}

static void xshm_flip_page (void)
{
    struct x11_priv_s * priv = &x11_priv;

    XShmPutImage (priv->display, priv->window, priv->gc, priv->ximage, 
		  0, 0, 0, 0, priv->width, priv->height, False);
    XFlush (priv->display);
}

vo_output_video_t video_out_xshm = {
    name: "xshm",
    setup: xshm_setup,
    close: xshm_close,
    flip_page: xshm_flip_page,
    draw_slice: x11_draw_slice,
    draw_frame: x11_draw_frame,
    allocate_image_buffer: x11_allocate_image_buffer,
    free_image_buffer: x11_free_image_buffer
};
#endif

#ifdef LIBVO_XV
static int xv_check_extension (void)
{
    struct x11_priv_s * priv = &x11_priv;
    unsigned int version;
    unsigned int release;
    unsigned int dummy;

    if (XvQueryExtension (priv->display, &version, &release,
			  &dummy, &dummy, &dummy) != Success)
	return 1;

    if ((version < 2) || ((version == 2) && (release < 2)))
	return 1;

    return 0;
}

static int xv_check_yv12 (XvPortID port)
{
    struct x11_priv_s * priv = &x11_priv;
    XvImageFormatValues * formatValues;
    int formats;
    int i;

    formatValues = XvListImageFormats (priv->display, port, &formats);
    for (i = 0; i < formats; i++)
	if ((formatValues[i].id == FOURCC_YV12) &&
	    (! (strcmp (formatValues[i].guid, "YV12")))) {
	    XFree (formatValues);
	    return 0;
	}
    XFree (formatValues);
    return 1;
}

static int xv_get_port (void)
{
    struct x11_priv_s * priv = &x11_priv;
    int adaptors;
    int i;
    unsigned long j;
    XvAdaptorInfo * adaptorInfo;

    XvQueryAdaptors (priv->display, priv->window, &adaptors, &adaptorInfo);

    for (i = 0; i < adaptors; i++)
	if (adaptorInfo[i].type & XvImageMask)
	    for (j = 0; j < adaptorInfo[i].num_ports; j++)
		if (! (xv_check_yv12 (adaptorInfo[i].base_id + j))) {
		    priv->port = adaptorInfo[i].base_id + j;
		    XvFreeAdaptorInfo (adaptorInfo);
		    return 0;
		}

    XvFreeAdaptorInfo (adaptorInfo);
    return 1;
}

static int xv_create_image (int width, int height)
{
    struct x11_priv_s * priv = &x11_priv;

    priv->xvimage = XvCreateImage (priv->display, priv->port, FOURCC_YV12,
				   NULL /* data */, width, height);
    if (priv->xvimage == NULL)
	return 1;

    priv->xvimage->data =
	malloc (priv->xvimage->data_size);
    return 0;
}

static int xv_common_setup (int width, int height,
			    int (* create_image) (int, int))
{
    struct x11_priv_s * priv = &x11_priv;

    if (xv_check_extension ()) {
	fprintf (stderr, "No xv extension\n");
	return 1;
    }

    if (x11_get_visual_info ()) {
	fprintf (stderr, "No truecolor visual\n");
	return 1;
    }

    x11_create_window (width, height);
    x11_create_gc ();

    if (xv_get_port ()) {
	fprintf (stderr, "Cannot find xv port\n");
	return 1;
    }

    if (create_image (width, height)) {
	fprintf (stderr, "Cannot create xvimage\n");
	return 1;
    }

    // FIXME set WM_DELETE_WINDOW protocol ? to avoid shm leaks

    XMapWindow (priv->display, priv->window);

    priv->width = width;
    priv->height = height;
    return 0;
}

static int xv_setup (vo_output_video_attr_t * vo_attr)
{
    struct x11_priv_s * priv = &x11_priv;
    int width, height;

    width = vo_attr->width;
    height = vo_attr->height;

    priv->display = XOpenDisplay (NULL);
    if (! (priv->display)) {
	fprintf (stderr, "Can not open display\n");
	return 1;
    }

    return xv_common_setup (width, height, xv_create_image);
}

static int xv_close (void * dummy)
{
    struct x11_priv_s * priv = &x11_priv;

    XFree (priv->xvimage);
    common_close ();
    return 0;
}

static void xv_flip_page (void)
{
    struct x11_priv_s * priv = &x11_priv;

    XvPutImage (priv->display, priv->port, priv->window, priv->gc,
		priv->xvimage, 0, 0, priv->width, priv->height,
		0, 0, priv->width, priv->height);
    XFlush (priv->display);
}

static int xv_draw_slice (uint8_t *src[], int slice_num)
{
    struct x11_priv_s * priv = &x11_priv;

    memcpy (priv->xvimage->data + priv->width * 16 * slice_num,
	    src[0], priv->width * 16);
    memcpy (priv->xvimage->data + priv->width * (priv->height + 4 * slice_num),
	    src[2], priv->width * 4);
    memcpy (priv->xvimage->data +
	    priv->width * (priv->height * 5 / 4 + 4 * slice_num),
	    src[1], priv->width * 4);

    return 0;
}

static int xv_draw_frame (frame_t *frame)
{
    struct x11_priv_s * priv = &x11_priv;

    memcpy (priv->xvimage->data, frame->base[0], priv->width * priv->height);
    memcpy (priv->xvimage->data + priv->width * priv->height,
	    frame->base[2], priv->width * priv->height / 4);
    memcpy (priv->xvimage->data + priv->width * priv->height * 5 / 4,
	    frame->base[1], priv->width * priv->height / 4);

    return 0;
}

vo_output_video_t video_out_xv = {
    name: "xv",
    setup: xv_setup,
    close: xv_close,
    flip_page: xv_flip_page,
    draw_slice: xv_draw_slice,
    draw_frame: xv_draw_frame,
    allocate_image_buffer: x11_allocate_image_buffer,
    free_image_buffer: x11_free_image_buffer
};
#endif

#ifdef LIBVO_XVSHM
static int xvshm_create_image (int width, int height)
{
    struct x11_priv_s * priv = &x11_priv;

    priv->xvimage = XvShmCreateImage (priv->display, priv->port, FOURCC_YV12,
				      NULL /* data */, width, height,
				      &(priv->shminfo));
    if (priv->xvimage == NULL)
	return 1;

    if (xshm_create_shm (priv->xvimage->data_size))
	return 1;

    priv->xvimage->data = priv->shminfo.shmaddr;
    return 0;
}

static int xvshm_setup (vo_output_video_attr_t * vo_attr)
{
    struct x11_priv_s * priv = &x11_priv;
    int width, height;

    width = vo_attr->width;
    height = vo_attr->height;

    priv->display = XOpenDisplay (NULL);
    if (! (priv->display)) {
	fprintf (stderr, "Can not open display\n");
	return 1;
    }

    if (x11_check_local ()) {
	fprintf (stderr, "Can not use xshm on a remote display\n");
	return 1;
    }

    if (xshm_check_extension ()) {
	fprintf (stderr, "No xshm extension\n");
	return 1;
    }

    return xv_common_setup (width, height, xvshm_create_image);
}

static int xvshm_close (void * dummy)
{
    struct x11_priv_s * priv = &x11_priv;

    if (priv->xvimage) {
	xshm_destroy_shm ();
	XFree (priv->xvimage);
    }
    common_close ();
    return 0;
}

static void xvshm_flip_page (void)
{
    struct x11_priv_s * priv = &x11_priv;

    XvShmPutImage (priv->display, priv->port, priv->window, priv->gc,
		   priv->xvimage, 0, 0, priv->width, priv->height,
		   0, 0, priv->width, priv->height, False);
    XFlush (priv->display);
}

vo_output_video_t video_out_xvshm = {
    name: "xvshm",
    setup: xvshm_setup,
    close: xvshm_close,
    flip_page: xvshm_flip_page,
    draw_slice: xv_draw_slice,
    draw_frame: xv_draw_frame,
    allocate_image_buffer: x11_allocate_image_buffer,
    free_image_buffer: x11_free_image_buffer
};
#endif

#endif
