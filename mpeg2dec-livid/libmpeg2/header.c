/*
 * slice.c
 *
 * Copyright (C) Aaron Holtzman <aholtzma@ess.engr.uvic.ca> - Nov 1999
 *
 * Decodes an MPEG-2 video stream.
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 *	
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with GNU Make; see the file COPYING. If not, write to
 * the Free Software Foundation, 
 *
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "config.h"
#include "mpeg2.h"
#include "mpeg2_internal.h"

#include "header.h"

// default intra quant matrix, in zig-zag order
static uint8_t default_intra_quantizer_matrix[64] ALIGN_16_BYTE = {
    8,
    16, 16,
    19, 16, 19,
    22, 22, 22, 22,
    22, 22, 26, 24, 26,
    27, 27, 27, 26, 26, 26,
    26, 27, 27, 27, 29, 29, 29,
    34, 34, 34, 29, 29, 29, 27, 27,
    29, 29, 32, 32, 34, 34, 37,
    38, 37, 35, 35, 34, 35,
    38, 38, 40, 40, 40,
    48, 48, 46, 46,
    56, 56, 58,
    69, 69,
    83
};

#ifdef __i386__
static uint8_t scan_norm_mmx[64] ALIGN_16_BYTE = { 
    // MMX Zig-Zag scan pattern (transposed) 
     0, 8, 1, 2, 9,16,24,17,
    10, 3, 4,11,18,25,32,40,
    33,26,19,12, 5, 6,13,20,
    27,34,41,48,56,49,42,35,
    28,21,14, 7,15,22,29,36,
    43,50,57,58,51,44,37,30,
    23,31,38,45,52,59,60,53,
    46,39,47,54,61,62,55,63
};

static uint8_t scan_alt_mmx[64] ALIGN_16_BYTE = 
{ 
    // Alternate scan pattern (transposed)
     0, 1, 2, 3, 8, 9,16,17,
    10,11, 4, 5, 6, 7,15,14,
    13,12,19,18,24,25,32,33,
    26,27,20,21,22,23,28,29,
    30,31,34,35,40,41,48,49,
    42,43,36,37,38,39,44,45,
    46,47,50,51,56,57,58,59,
    52,53,54,55,60,61,62,63,
};
#endif

static uint8_t scan_norm[64] ALIGN_16_BYTE =
{ 
    // Zig-Zag scan pattern
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

static uint8_t scan_alt[64] ALIGN_16_BYTE =
{ 
    // Alternate scan pattern
    0,8,16,24,1,9,2,10,17,25,32,40,48,56,57,49,
    41,33,26,18,3,11,4,12,19,27,34,42,50,58,35,43,
    51,59,20,28,5,13,6,14,21,29,36,44,52,60,37,45,
    53,61,22,30,7,15,23,31,38,46,54,62,39,47,55,63
};

void header_state_init (picture_t * picture)
{
    //FIXME we should set pointers to the real scan matrices here (mmx vs
    //normal) instead of the ifdefs in header_process_picture_coding_extension

#ifdef __i386__
    if (config.flags & MPEG2_MMX_ENABLE)
	picture->scan = scan_norm_mmx;
    else
#endif
	picture->scan = scan_norm;
}

void header_process_sequence_header (picture_t * picture, uint8_t * buffer)
{
    unsigned int h_size;
    unsigned int v_size;
    int i;
    uint8_t * scan;

    //if ((buffer[6] & 0x20) != 0x20)
    //return 1;	// missing marker_bit

    v_size = (buffer[0] << 16) | (buffer[1] << 8) | buffer[2];

    h_size = ((v_size >> 12) + 15) & ~15;
    v_size = ((v_size & 0xfff) + 15) & ~15;

    //if ((h_size > 720) || (v_size > 576))
    //return 1;	// MP@ML size restrictions

    //XXX this needs field fixups
    picture->coded_picture_width = h_size;
    picture->coded_picture_height = v_size;
    picture->last_mba = ((h_size * v_size) >> 8) - 1;

    // this is not used by the decoder
    picture->aspect_ratio_information = buffer[3] >> 4;
    picture->frame_rate_code = buffer[3] & 15;

#ifdef __i386__
    if (config.flags & MPEG2_MMX_ENABLE)
	scan = scan_norm_mmx;
    else
#endif
	scan = scan_norm;

    if (buffer[7] & 2) {
	for (i = 0; i < 64; i++)
	    picture->intra_quantizer_matrix[scan[i]] =
		(buffer[i+7] << 7) | (buffer[i+8] >> 1);
	buffer += 64;
    } else {
	for (i = 0; i < 64; i++)
	    picture->intra_quantizer_matrix[scan[i]] =
		default_intra_quantizer_matrix [i];
    }

    if (buffer[7] & 1) {
	for (i = 0; i < 64; i++)
	    picture->non_intra_quantizer_matrix[scan[i]] =
		buffer[i+8];
    } else {
	for (i = 0; i < 64; i++)
	    picture->non_intra_quantizer_matrix[i] = 16;
    }

    // MPEG1 - for testing only
    picture->mpeg1 = 1;
    picture->intra_dc_precision = 0;
    picture->frame_pred_frame_dct = 1;
    picture->q_scale_type = 0;
    picture->concealment_motion_vectors = 0;
    //picture->alternate_scan = 0;
 
    //return 0;
}

static int header_process_sequence_extension (picture_t * picture,
					      uint8_t * buffer)
{
    // check chroma format, size extensions, marker bit
    if (((buffer[1] & 0x07) != 0x02) || (buffer[2] & 0xe0) ||
	((buffer[3] & 0x01) != 0x01))
	return 1;

    // this is not used by the decoder
    picture->progressive_sequence = (buffer[1] >> 3) & 1;

    // MPEG1 - for testing only
    picture->mpeg1 = 0;

    return 0;
}

static int header_process_picture_coding_extension (picture_t * picture, uint8_t * buffer)
{
    if ((buffer[2] & 3) != 3)
	return 1;	// not a frame picture

    //pre subtract 1 for use later in compute_motion_vector
    picture->f_code[0][0] = (buffer[0] & 15) - 1;
    picture->f_code[0][1] = (buffer[1] >> 4) - 1;
    picture->f_code[1][0] = (buffer[1] & 15) - 1;
    picture->f_code[1][1] = (buffer[2] >> 4) - 1;

    picture->intra_dc_precision = (buffer[2] >> 2) & 3;
    picture->frame_pred_frame_dct = (buffer[3] >> 6) & 1;
    picture->concealment_motion_vectors = (buffer[3] >> 5) & 1;
    picture->q_scale_type = (buffer[3] >> 4) & 1;
    picture->intra_vlc_format = (buffer[3] >> 3) & 1;

    if (buffer[3] & 4) {	// alternate_scan
#ifdef __i386__
	if (config.flags & MPEG2_MMX_ENABLE)
	    picture->scan = scan_alt_mmx;
	else
#endif
	    picture->scan = scan_alt;
    } else {
#ifdef __i386__
	if (config.flags & MPEG2_MMX_ENABLE)
	    picture->scan = scan_norm_mmx;
	else
#endif
	    picture->scan = scan_norm;
    }

    // these are not used by the decoder
    picture->top_field_first = buffer[3] >> 7;
    picture->repeat_first_field = (buffer[3] >> 1) & 1;
    picture->progressive_frame = buffer[4] >> 7;

    return 0;
}

void header_process_extension (picture_t * picture, uint8_t * buffer)
{
    switch (buffer[0] & 0xf0) {
    case 0x10:	// sequence extension
	header_process_sequence_extension (picture, buffer);
	break;

    case 0x80:	// picture coding extension
	header_process_picture_coding_extension (picture, buffer);
	break;
    }
}

void header_process_picture_header (picture_t *picture, uint8_t * buffer)
{
    picture->picture_coding_type = (buffer [1] >> 3) & 7;

    // forward_f_code and backward_f_code - used in mpeg1 only
    picture->f_code[0][0] = picture->f_code[0][1] =
	(((buffer[3] << 1) | (buffer[4] >> 7)) & 7) - 1;
    picture->f_code[1][0] = picture->f_code[1][1] =
	((buffer[4] >> 3) & 7) - 1;

    //return 0;
}
