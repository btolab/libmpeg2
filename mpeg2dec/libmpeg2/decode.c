/*
 * decode.c
 * Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * See http://libmpeg2.sourceforge.net/ for updates.
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

#include <string.h>	/* memcmp/memset, try to remove */
#include <stdlib.h>
#include <inttypes.h>

#include "mpeg2.h"
#include "mpeg2_internal.h"
#include "convert.h"

#define BUFFER_SIZE (1194 * 1024)

mpeg2dec_t * mpeg2_init (uint32_t mm_accel)
{
    static int do_init = 1;
    mpeg2dec_t * mpeg2dec;

    mpeg2dec = (mpeg2dec_t *) mpeg2_malloc (sizeof (mpeg2dec_t),
					    ALLOC_MPEG2DEC);
    if (mpeg2dec == NULL)
	return NULL;

    if (do_init) {
	do_init = 0;
	mpeg2_cpu_state_init (mm_accel);
	mpeg2_idct_init (mm_accel);
	mpeg2_mc_init (mm_accel);
    }

    memset (mpeg2dec, 0, sizeof (mpeg2dec_t));

    mpeg2dec->chunk_ptr = mpeg2dec->chunk_start = mpeg2dec->chunk_buffer =
	(uint8_t *) mpeg2_malloc (BUFFER_SIZE + 4, ALLOC_CHUNK);

    mpeg2dec->shift = 0xffffff00;
    mpeg2dec->state = STATE_INVALID;
    mpeg2dec->code = 0xb4;
    mpeg2dec->skip = 0;

    /* initialize substructures */
    mpeg2_header_state_init (mpeg2dec);

    return mpeg2dec;
}

const mpeg2_info_t * mpeg2_info (mpeg2dec_t * mpeg2dec)
{
    return &(mpeg2dec->info);
}

static inline int copy_chunk (mpeg2dec_t * mpeg2dec)
{
    uint8_t * current;
    uint32_t shift;
    uint8_t * chunk_ptr;
    uint8_t * limit;
    uint8_t byte;

    current = mpeg2dec->buf_start;
    if (current == mpeg2dec->buf_end)
	return 1;

    shift = mpeg2dec->shift;
    chunk_ptr = mpeg2dec->chunk_ptr;
    limit = current + (mpeg2dec->chunk_buffer + BUFFER_SIZE - chunk_ptr);
    if (limit > mpeg2dec->buf_end)
	limit = mpeg2dec->buf_end;

    do {
	byte = *current++;
	if (shift == 0x00000100)
	    goto startcode;
	shift = (shift | byte) << 8;
	*chunk_ptr++ = byte;
    } while (current < limit);

    mpeg2dec->bytes_since_pts += chunk_ptr - mpeg2dec->chunk_ptr;
    mpeg2dec->shift = shift;
    if (current == mpeg2dec->buf_end) {
	mpeg2dec->chunk_ptr = chunk_ptr;
	return 1;
    } else {
	/* we filled the chunk buffer without finding a start code */
	mpeg2dec->chunk_start = mpeg2dec->chunk_ptr = mpeg2dec->chunk_buffer;
	mpeg2dec->code = 0xb4;	/* sequence_error_code */
	mpeg2dec->buf_start = current;
	return 0;
    }

startcode:
    mpeg2dec->bytes_since_pts += chunk_ptr + 1 - mpeg2dec->chunk_ptr;
    mpeg2dec->chunk_ptr = chunk_ptr + 1;
    mpeg2dec->shift = 0xffffff00;
    mpeg2dec->code = byte;
    if (!byte) {
	if (!mpeg2dec->num_pts)
	    mpeg2dec->pts = 0;	/* none */
	else if (mpeg2dec->bytes_since_pts >= 4) {
	    mpeg2dec->num_pts = 0;
	    mpeg2dec->pts = mpeg2dec->pts_current;
	} else if (mpeg2dec->num_pts > 1) {
	    mpeg2dec->num_pts = 1;
	    mpeg2dec->pts = mpeg2dec->pts_previous;
	} else
	    mpeg2dec->pts = 0;	/* none */
    }
    mpeg2dec->buf_start = current;
    return 0;
}

void mpeg2_buffer (mpeg2dec_t * mpeg2dec, uint8_t * start, uint8_t * end)
{
    mpeg2dec->buf_start = start;
    mpeg2dec->buf_end = end;
}

int mpeg2_parse (mpeg2dec_t * mpeg2dec)
{
    static int (* process_header[]) (mpeg2dec_t * mpeg2dec) = {
	mpeg2_header_picture, mpeg2_header_extension, mpeg2_header_user_data,
	mpeg2_header_sequence, NULL, NULL, NULL, NULL, mpeg2_header_gop
    };
    uint8_t code;

#define RECEIVED(code,state) (((state) << 8) + (code))

    switch (RECEIVED (mpeg2dec->code, mpeg2dec->state)) {
    case RECEIVED (0xb7, STATE_SLICE):
	mpeg2_header_end (mpeg2dec);
	return STATE_END;

    case RECEIVED (0x01, STATE_PICTURE):
    case RECEIVED (0x01, STATE_PICTURE_2ND):
	mpeg2_header_slice (mpeg2dec);

    next_chunk:
	mpeg2dec->chunk_ptr = mpeg2dec->chunk_start;
    default:
	code = mpeg2dec->code;
	if (copy_chunk (mpeg2dec))
	    return -1;
    }

    /* wait for sequence_header_code */
    if (mpeg2dec->state == STATE_INVALID && code != 0xb3)
	goto next_chunk;

    if ((unsigned) (code - 1) < 0xb0 - 1) {
	if (! (mpeg2dec->picture->flags & PIC_FLAG_SKIP))
	    mpeg2_slice (&(mpeg2dec->decoder), code, mpeg2dec->chunk_start);
	if ((unsigned) (mpeg2dec->code - 1) < 0xb0 - 1)
	    goto next_chunk;
    } else
	process_header[code & 0x0b] (mpeg2dec);

    switch (RECEIVED (mpeg2dec->code, mpeg2dec->state)) {

    /* state transition after a sequence header */
    case RECEIVED (0x00, STATE_SEQUENCE):
    case RECEIVED (0xb8, STATE_SEQUENCE):
	mpeg2_header_sequence_finalize (mpeg2dec);

	/*
	 * according to 6.1.1.6, repeat sequence headers should be
	 * identical to the original. However some DVDs dont respect that
	 * and have different bitrates in the repeat sequence headers. So
	 * we'll ignore that in the comparison and still consider these as
	 * repeat sequence headers.
	 */
	mpeg2dec->last_sequence.byte_rate = mpeg2dec->sequence.byte_rate;
	if (!memcmp (&(mpeg2dec->last_sequence), &(mpeg2dec->sequence),
		     sizeof (sequence_t)))
	    mpeg2dec->state = STATE_SEQUENCE_REPEATED;
	break;

    /* other legal state transitions */
    case RECEIVED (0x00, STATE_GOP):
    case RECEIVED (0x00, STATE_SLICE_1ST):
    case RECEIVED (0x00, STATE_SLICE):
    case RECEIVED (0x01, STATE_PICTURE):
    case RECEIVED (0x01, STATE_PICTURE_2ND):
    case RECEIVED (0xb3, STATE_SLICE):
    case RECEIVED (0xb7, STATE_SLICE):
    case RECEIVED (0xb8, STATE_SLICE):
	break;

    /* legal headers within a given state */
    case RECEIVED (0xb2, STATE_SEQUENCE):
    case RECEIVED (0xb2, STATE_GOP):
    case RECEIVED (0xb2, STATE_PICTURE):
    case RECEIVED (0xb2, STATE_PICTURE_2ND):
    case RECEIVED (0xb5, STATE_SEQUENCE):
    case RECEIVED (0xb5, STATE_PICTURE):
    case RECEIVED (0xb5, STATE_PICTURE_2ND):
	goto next_chunk;

    default:
	mpeg2dec->state = STATE_INVALID;
	mpeg2dec->chunk_start = mpeg2dec->chunk_buffer;
	goto next_chunk;
    }

    mpeg2dec->chunk_start = mpeg2dec->chunk_ptr = mpeg2dec->chunk_buffer;
    return mpeg2dec->state;
}

void mpeg2_convert (mpeg2dec_t * mpeg2dec,
		    void (* convert) (int, int, void *,
				      struct convert_init_s *), void * arg)
{
    convert_init_t convert_init;
    int size;

    convert_init.id = NULL;
    convert (mpeg2dec->decoder.width, mpeg2dec->decoder.height, arg,
	     &convert_init);
    if (convert_init.id_size) {
	convert_init.id = mpeg2dec->convert_id =
	    mpeg2_malloc (convert_init.id_size, ALLOC_CONVERT_ID);
	convert (mpeg2dec->decoder.width, mpeg2dec->decoder.height, arg,
		 &convert_init);
    }
    mpeg2dec->convert_size[0] = size = convert_init.buf_size[0];
    mpeg2dec->convert_size[1] = size += convert_init.buf_size[1];
    mpeg2dec->convert_size[2] = size += convert_init.buf_size[2];
    mpeg2dec->convert_start = convert_init.start;
    mpeg2dec->convert_copy = convert_init.copy;

    size = mpeg2dec->decoder.width * mpeg2dec->decoder.height >> 2;
    mpeg2dec->yuv_buf[0][0] = (uint8_t *) mpeg2_malloc (6 * size, ALLOC_YUV);
    mpeg2dec->yuv_buf[0][1] = mpeg2dec->yuv_buf[0][0] + 4 * size;
    mpeg2dec->yuv_buf[0][2] = mpeg2dec->yuv_buf[0][0] + 5 * size;
    mpeg2dec->yuv_buf[1][0] = (uint8_t *) mpeg2_malloc (6 * size, ALLOC_YUV);
    mpeg2dec->yuv_buf[1][1] = mpeg2dec->yuv_buf[1][0] + 4 * size;
    mpeg2dec->yuv_buf[1][2] = mpeg2dec->yuv_buf[1][0] + 5 * size;
    size = mpeg2dec->decoder.width * 8;
    mpeg2dec->yuv_buf[2][0] = (uint8_t *) mpeg2_malloc (6 * size, ALLOC_YUV);
    mpeg2dec->yuv_buf[2][1] = mpeg2dec->yuv_buf[2][0] + 4 * size;
    mpeg2dec->yuv_buf[2][2] = mpeg2dec->yuv_buf[2][0] + 5 * size;
}

void mpeg2_set_buf (mpeg2dec_t * mpeg2dec, uint8_t * buf[3], void * id)
{
    fbuf_t * fbuf;

    if (mpeg2dec->custom_fbuf) {
	mpeg2_set_fbuf (mpeg2dec, mpeg2dec->decoder.coding_type);
	fbuf = mpeg2dec->fbuf[0];
	if (mpeg2dec->state == STATE_SEQUENCE) {
	    mpeg2dec->fbuf[2] = mpeg2dec->fbuf[1];
	    mpeg2dec->fbuf[1] = mpeg2dec->fbuf[0];
	}
    } else
	fbuf = &(mpeg2dec->fbuf_alloc[mpeg2dec->alloc_index++].fbuf);
    fbuf->buf[0] = buf[0];
    fbuf->buf[1] = buf[1];
    fbuf->buf[2] = buf[2];
    fbuf->id = id;
}

void mpeg2_custom_fbuf (mpeg2dec_t * mpeg2dec, int custom_fbuf)
{
    mpeg2dec->custom_fbuf = custom_fbuf;
}

void mpeg2_skip (mpeg2dec_t * mpeg2dec, int skip)
{
    mpeg2dec->skip = skip;
}

void mpeg2_pts (mpeg2dec_t * mpeg2dec, uint32_t pts)
{
    mpeg2dec->pts_previous = mpeg2dec->pts_current;
    mpeg2dec->pts_current = pts;
    mpeg2dec->num_pts++;
    mpeg2dec->bytes_since_pts = 0;
}

void mpeg2_close (mpeg2dec_t * mpeg2dec)
{
    /* static uint8_t finalizer[] = {0,0,1,0xb4}; */

    /* mpeg2_decode_data (mpeg2dec, finalizer, finalizer+4); */

    mpeg2_free (mpeg2dec->chunk_buffer);
}
