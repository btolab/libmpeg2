/* 
 *  video_out_null.c
 *
 *	Copyright (C) Aaron Holtzman - June 2000
 *
 *  This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
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

#include "config.h"

#include <stdlib.h>	/* only used for NULL */
#include <inttypes.h>

#include "video_out.h"
#include "video_out_internal.h"

typedef struct null_instance_s {
    vo_instance_t vo;
    int prediction_index;
    vo_frame_t * frame_ptr[3];
    vo_frame_t frame[3];
} null_instance_t;

static void null_draw_frame (vo_frame_t * frame)
{
}

vo_instance_t * vo_null_setup (vo_instance_t * _instance,
			       int width, int height)
{
    null_instance_t * instance;

    instance = (null_instance_t *) _instance;
    if (instance == NULL) {
	instance = malloc (sizeof (null_instance_t));
	if (instance == NULL)
	    return NULL;
    }

    if (libvo_common_alloc_frames ((vo_instance_t *) instance, width, height,
				   sizeof (vo_frame_t),
				   NULL, NULL, null_draw_frame))
	return NULL;

    instance->vo.reinit = vo_null_setup;
    instance->vo.close = libvo_common_free_frames;
    instance->vo.get_frame = libvo_common_get_frame;

    return (vo_instance_t *) instance;
}

static void null_copy_slice (vo_frame_t * frame, uint8_t ** src)
{
}

vo_instance_t * vo_nullslice_setup (vo_instance_t * _instance,
				    int width, int height)
{
    null_instance_t * instance;
    int i;

    instance = (null_instance_t *) vo_null_setup (_instance, width, height);
    if (instance == NULL)
	return NULL;

    instance->vo.reinit = vo_nullslice_setup;
    for (i = 0; i < 3; i++)
	instance->frame[i].copy = null_copy_slice;

    return (vo_instance_t *) instance;
}
