/*
 * decode.c
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <stdio.h>
#include <string.h>	// memcpy/memset, try to remove
#include <stdlib.h>
#include <inttypes.h>

#include "output_video.h"
#include "mpeg2.h"
#include "mpeg2_internal.h"
#include "mm_accel.h"
#include "attributes.h"
#include "mmx.h"


mpeg2_config_t config;


void mpeg2_init (mpeg2dec_t * mpeg2dec, vo_output_video_t * output,
		 void * user_data) 
{
    
    //intialize the decoder state 
    mpeg2dec->shift = 0;
    mpeg2dec->is_display_initialized = 0;
    mpeg2dec->is_sequence_needed=1;
    mpeg2dec->drop_flag=0;
    mpeg2dec->drop_frame=0;
    mpeg2dec->in_slice=0;

    // copy output structure into local mpeg2dec structure
    // FIXME this is ugly
    if (output != NULL) {
	mpeg2dec->output=(vo_output_video_t *) 
	    malloc (sizeof (vo_output_video_t));
	memcpy(mpeg2dec->output,output,sizeof (vo_output_video_t));
    }
    
    // opaque user pointer (is passed to the output)
    mpeg2dec->user_data=user_data;
    
    mpeg2dec->chunk_buffer=(uint8_t *) malloc(224 * 1024 + 4);
    mpeg2dec->chunk_ptr=mpeg2dec->chunk_buffer;
    mpeg2dec->code=0xff;

    mpeg2dec->picture= (picture_t*) malloc(sizeof(picture_t));
    memset(mpeg2dec->picture,0,sizeof(picture_t));

    config.flags = mm_accel () | MM_ACCEL_MLIB;

    // initialize supstructures
    header_state_init (mpeg2dec->picture);
    idct_init ();
    motion_comp_init ();
}

static void decode_allocate_image_buffers (mpeg2dec_t * mpeg2dec)
{
    picture_t * picture;
    img_buf_t ** frame;
    img_buf_t * (* allocate) (int, int, uint32_t);
    int frame_size;

    picture = mpeg2dec->picture;
    frame = mpeg2dec->frame;
    allocate = mpeg2dec->output->allocate_image_buffer;

    frame_size = (mpeg2dec->picture->coded_picture_width *
		  mpeg2dec->picture->coded_picture_height);

    // allocate images in YV12 format
    frame[0] = allocate (picture->coded_picture_width,
			 picture->coded_picture_height,
			 0x32315659);
    picture->throwaway_frame[0] = frame[0]->base;
    picture->throwaway_frame[1] = frame[0]->base + frame_size * 5 / 4;
    picture->throwaway_frame[2] = frame[0]->base + frame_size;

    frame[1] = allocate (picture->coded_picture_width, 
			 picture->coded_picture_height, 
			 0x32315659);
    picture->backward_reference_frame[0] = frame[1]->base;
    picture->backward_reference_frame[1] = frame[1]->base + frame_size * 5 / 4;
    picture->backward_reference_frame[2] = frame[1]->base + frame_size;
    
    frame[2] = allocate (picture->coded_picture_width, 
			 picture->coded_picture_height, 
			 0x32315659);
    picture->forward_reference_frame[0] = frame[2]->base;
    picture->forward_reference_frame[1] = frame[2]->base + frame_size * 5 / 4;
    picture->forward_reference_frame[2] = frame[2]->base + frame_size;
}


static void decode_reorder_frames (mpeg2dec_t * mpeg2dec)
{
    picture_t * picture;

    picture = mpeg2dec->picture;

    if (picture->picture_coding_type != B_TYPE) {

	//reuse the soon to be outdated forward reference frame
	picture->current_frame[0] = picture->forward_reference_frame[0];
	picture->current_frame[1] = picture->forward_reference_frame[1];
	picture->current_frame[2] = picture->forward_reference_frame[2];

	//make the backward reference frame the new forward reference frame
	picture->forward_reference_frame[0] =
	    picture->backward_reference_frame[0];
	picture->forward_reference_frame[1] =
	    picture->backward_reference_frame[1];
	picture->forward_reference_frame[2] =
	    picture->backward_reference_frame[2];

	picture->backward_reference_frame[0] = picture->current_frame[0];
	picture->backward_reference_frame[1] = picture->current_frame[1];
	picture->backward_reference_frame[2] = picture->current_frame[2];

    } else {

	picture->current_frame[0] = picture->throwaway_frame[0];
	picture->current_frame[1] = picture->throwaway_frame[1];
	picture->current_frame[2] = picture->throwaway_frame[2];

    }
}


static int parse_chunk (mpeg2dec_t * mpeg2dec, int code, uint8_t * buffer)
{
    picture_t * picture;
    int is_frame_done;

    /* wait for sequence_header_code */
    if (mpeg2dec->is_sequence_needed && (code != 0xb3))
	return 0;

    stats_header (code, buffer);

    picture = mpeg2dec->picture;
    is_frame_done = mpeg2dec->in_slice && ((!code) || (code >= 0xb0));

    if (is_frame_done) {
	mpeg2dec->in_slice = 0;

	if (!(mpeg2dec->drop_frame)) {
	    if (((HACK_MODE == 2) || (picture->mpeg1)) &&
		((picture->picture_structure == FRAME_PICTURE) ||
		 (picture->second_field))) {
		if (picture->picture_coding_type == B_TYPE)
		    mpeg2dec->output->draw_frame (picture->throwaway_frame);
		else
		    mpeg2dec->output->draw_frame (picture->forward_reference_frame);
	    }
	    mpeg2dec->output->flip_page ();
#ifdef ARCH_X86
	    if (config.flags & MM_ACCEL_X86_MMX)
		emms ();
#endif
	}
    }

    switch (code) {
    case 0x00:	/* picture_start_code */
	if (header_process_picture_header (picture, buffer)) {
	    printf ("bad picture header\n");
	    exit (1);
	}
	mpeg2dec->drop_frame =
	    mpeg2dec->drop_flag && (picture->picture_coding_type == B_TYPE);
	break;

    case 0xb3:	/* sequence_header_code */
	if (header_process_sequence_header (picture, buffer)) {
	    printf ("bad sequence header\n");
	    exit (1);
	}
	mpeg2dec->is_sequence_needed = 0;
	break;

    case 0xb5:	/* extension_start_code */
	if (header_process_extension (picture, buffer)) {
	    printf ("bad extension\n");
	    exit (1);
	}
	break;

    default:
	if (code >= 0xb9)
	    printf ("stream not demultiplexed ?\n");

	if (code >= 0xb0)
	    break;

	if (!(mpeg2dec->in_slice)) {
	    mpeg2dec->in_slice = 1;

	    if (!(mpeg2dec->is_display_initialized)) {
		vo_output_video_attr_t attr;

		attr.width = picture->coded_picture_width;
		attr.height = picture->coded_picture_height;
		attr.fullscreen = 0;
		attr.title = NULL;

		if (mpeg2dec->output->setup (&attr)) {
		    printf ("display init failed\n");
		    exit (1);
		}

		decode_allocate_image_buffers (mpeg2dec);
		decode_reorder_frames (mpeg2dec);
		mpeg2dec->is_display_initialized = 1;
	    } else if (!(picture->second_field) )
		decode_reorder_frames (mpeg2dec);
	}

	if (!(mpeg2dec->drop_frame)) {
	    slice_process (picture, code, buffer);

	    if ((HACK_MODE < 2) && (!(picture->mpeg1))) {
		uint8_t * foo[3];
		uint8_t ** bar;
		int offset;

		if (picture->picture_coding_type == B_TYPE)
		    bar = picture->throwaway_frame;
		else
		    bar = picture->forward_reference_frame;

		offset = (code-1) * 4 * picture->coded_picture_width;
		if ((! HACK_MODE) && (picture->picture_coding_type == B_TYPE))
		    offset = 0;

		foo[0] = bar[0] + 4 * offset;
		foo[1] = bar[1] + offset;
		foo[2] = bar[2] + offset;

		mpeg2dec->output->draw_slice (foo, code-1);
	    }

#ifdef ARCH_X86
	    if (config.flags & MM_ACCEL_X86_MMX)
		emms ();
#endif
	}
    }

    return is_frame_done;
}

int mpeg2_decode_data (mpeg2dec_t * mpeg2dec, uint8_t * current, uint8_t * end)
{
    uint32_t shift;
    uint8_t * chunk_ptr;
    uint8_t byte;
    int ret = 0;

    shift = mpeg2dec->shift;
    chunk_ptr = mpeg2dec->chunk_ptr;

    while (current != end) {
	while (1) {
	    byte = *current++;
	    if (shift != 0x00000100) {
		*chunk_ptr++ = byte;
		shift = (shift | byte) << 8;
		if (current != end)
		    continue;
		mpeg2dec->chunk_ptr = chunk_ptr;
		mpeg2dec->shift = shift;
		return ret;
	    }
	    break;
	}

	/* found start_code following chunk */

	ret += parse_chunk (mpeg2dec, mpeg2dec->code, mpeg2dec->chunk_buffer);

	/* done with header or slice, prepare for next one */

	mpeg2dec->code = byte;
	chunk_ptr = mpeg2dec->chunk_buffer;
	shift = 0xffffff00;
    }
    mpeg2dec->chunk_ptr = chunk_ptr;
    mpeg2dec->shift = shift;
    return ret;
}

void mpeg2_close (mpeg2dec_t * mpeg2dec)
{
    static uint8_t finalizer[] = {0,0,1,0};

    mpeg2_decode_data (mpeg2dec, finalizer, finalizer+4);

    if (mpeg2dec->is_display_initialized)
	mpeg2dec->output->draw_frame (mpeg2dec->picture->backward_reference_frame);

    mpeg2dec->output->free_image_buffer (mpeg2dec->frame[0]);
    mpeg2dec->output->free_image_buffer (mpeg2dec->frame[1]);
    mpeg2dec->output->free_image_buffer (mpeg2dec->frame[2]);
}

void mpeg2_drop (mpeg2dec_t * mpeg2dec, int flag)
{
    mpeg2dec->drop_flag = flag;
}

void mpeg2_output_init (mpeg2dec_t * mpeg2dec,int flag)
{
    mpeg2dec->is_display_initialized = flag;
}
