/*
 * slice.c
 * Copyright (C) 2000-2001 Michel Lespinasse <walken@zoy.org>
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

#include <string.h>
#include <inttypes.h>

#include "video_out.h"
#include "mpeg2_internal.h"
#include "attributes.h"

extern mpeg2_mc_t mpeg2_mc;
extern void (* mpeg2_idct_copy) (int16_t * block, uint8_t * dest, int stride);
extern void (* mpeg2_idct_add) (int16_t * block, uint8_t * dest, int stride);
extern void (* mpeg2_cpu_state_save) (cpu_state_t * state);
extern void (* mpeg2_cpu_state_restore) (cpu_state_t * state);

#include "vlc.h"

static int non_linear_quantizer_scale [] = {
     0,  1,  2,  3,  4,  5,   6,   7,
     8, 10, 12, 14, 16, 18,  20,  22,
    24, 28, 32, 36, 40, 44,  48,  52,
    56, 64, 72, 80, 88, 96, 104, 112
};

static inline int get_macroblock_modes (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int macroblock_modes;
    MBtab * tab;

    switch (picture->picture_coding_type) {
    case I_TYPE:

	tab = MB_I + UBITS (bit_buf, 1);
	DUMPBITS (bit_buf, bits, tab->len);
	macroblock_modes = tab->modes;

	if ((! (picture->frame_pred_frame_dct)) &&
	    (picture->picture_structure == FRAME_PICTURE)) {
	    macroblock_modes |= UBITS (bit_buf, 1) * DCT_TYPE_INTERLACED;
	    DUMPBITS (bit_buf, bits, 1);
	}

	return macroblock_modes;

    case P_TYPE:

	tab = MB_P + UBITS (bit_buf, 5);
	DUMPBITS (bit_buf, bits, tab->len);
	macroblock_modes = tab->modes;

	if (picture->picture_structure != FRAME_PICTURE) {
	    if (macroblock_modes & MACROBLOCK_MOTION_FORWARD) {
		macroblock_modes |= UBITS (bit_buf, 2) * MOTION_TYPE_BASE;
		DUMPBITS (bit_buf, bits, 2);
	    }
	    return macroblock_modes;
	} else if (picture->frame_pred_frame_dct) {
	    if (macroblock_modes & MACROBLOCK_MOTION_FORWARD)
		macroblock_modes |= MC_FRAME;
	    return macroblock_modes;
	} else {
	    if (macroblock_modes & MACROBLOCK_MOTION_FORWARD) {
		macroblock_modes |= UBITS (bit_buf, 2) * MOTION_TYPE_BASE;
		DUMPBITS (bit_buf, bits, 2);
	    }
	    if (macroblock_modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN)) {
		macroblock_modes |= UBITS (bit_buf, 1) * DCT_TYPE_INTERLACED;
		DUMPBITS (bit_buf, bits, 1);
	    }
	    return macroblock_modes;
	}

    case B_TYPE:

	tab = MB_B + UBITS (bit_buf, 6);
	DUMPBITS (bit_buf, bits, tab->len);
	macroblock_modes = tab->modes;

	if (picture->picture_structure != FRAME_PICTURE) {
	    if (! (macroblock_modes & MACROBLOCK_INTRA)) {
		macroblock_modes |= UBITS (bit_buf, 2) * MOTION_TYPE_BASE;
		DUMPBITS (bit_buf, bits, 2);
	    }
	    return macroblock_modes;
	} else if (picture->frame_pred_frame_dct) {
	    /* if (! (macroblock_modes & MACROBLOCK_INTRA)) */
	    macroblock_modes |= MC_FRAME;
	    return macroblock_modes;
	} else {
	    if (macroblock_modes & MACROBLOCK_INTRA)
		goto intra;
	    macroblock_modes |= UBITS (bit_buf, 2) * MOTION_TYPE_BASE;
	    DUMPBITS (bit_buf, bits, 2);
	    if (macroblock_modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN)) {
	    intra:
		macroblock_modes |= UBITS (bit_buf, 1) * DCT_TYPE_INTERLACED;
		DUMPBITS (bit_buf, bits, 1);
	    }
	    return macroblock_modes;
	}

    case D_TYPE:

	DUMPBITS (bit_buf, bits, 1);
	return MACROBLOCK_INTRA;

    default:
	return 0;
    }
#undef bit_buf
#undef bits
#undef bit_ptr
}

static inline int get_quantizer_scale (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)

    int quantizer_scale_code;

    quantizer_scale_code = UBITS (bit_buf, 5);
    DUMPBITS (bit_buf, bits, 5);

    if (picture->q_scale_type)
	return non_linear_quantizer_scale [quantizer_scale_code];
    else
	return quantizer_scale_code << 1;
#undef bit_buf
#undef bits
#undef bit_ptr
}

static inline int get_motion_delta (picture_t * picture, int f_code)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)

    int delta;
    int sign;
    MVtab * tab;

    if (bit_buf & 0x80000000) {
	DUMPBITS (bit_buf, bits, 1);
	return 0;
    } else if (bit_buf >= 0x0c000000) {

	tab = MV_4 + UBITS (bit_buf, 4);
	delta = (tab->delta << f_code) + 1;
	bits += tab->len + f_code + 1;
	bit_buf <<= tab->len;

	sign = SBITS (bit_buf, 1);
	bit_buf <<= 1;

	if (f_code)
	    delta += UBITS (bit_buf, f_code);
	bit_buf <<= f_code;

	return (delta ^ sign) - sign;

    } else {

	tab = MV_10 + UBITS (bit_buf, 10);
	delta = (tab->delta << f_code) + 1;
	bits += tab->len + 1;
	bit_buf <<= tab->len;

	sign = SBITS (bit_buf, 1);
	bit_buf <<= 1;

	if (f_code) {
	    NEEDBITS (bit_buf, bits, bit_ptr);
	    delta += UBITS (bit_buf, f_code);
	    DUMPBITS (bit_buf, bits, f_code);
	}

	return (delta ^ sign) - sign;

    }
#undef bit_buf
#undef bits
#undef bit_ptr
}

static inline int bound_motion_vector (int vector, int f_code)
{
#if 1
    unsigned int limit;
    int sign;

    limit = 16 << f_code;

    if ((unsigned int)(vector + limit) < 2 * limit)
	return vector;
    else {
	sign = ((int32_t)vector) >> 31;
	return vector - ((2 * limit) ^ sign) + sign;
    }
#else
    return ((int32_t)vector << (27 - f_code)) >> (27 - f_code);
#endif
}

static inline int get_dmv (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)

    DMVtab * tab;

    tab = DMV_2 + UBITS (bit_buf, 2);
    DUMPBITS (bit_buf, bits, tab->len);
    return tab->dmv;
#undef bit_buf
#undef bits
#undef bit_ptr
}

static inline int get_coded_block_pattern (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)

    CBPtab * tab;

    NEEDBITS (bit_buf, bits, bit_ptr);

    if (bit_buf >= 0x20000000) {

	tab = CBP_7 + (UBITS (bit_buf, 7) - 16);
	DUMPBITS (bit_buf, bits, tab->len);
	return tab->cbp;

    } else {

	tab = CBP_9 + UBITS (bit_buf, 9);
	DUMPBITS (bit_buf, bits, tab->len);
	return tab->cbp;
    }

#undef bit_buf
#undef bits
#undef bit_ptr
}

static inline int get_luma_dc_dct_diff (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    DCtab * tab;
    int size;
    int dc_diff;

    if (bit_buf < 0xf8000000) {
	tab = DC_lum_5 + UBITS (bit_buf, 5);
	size = tab->size;
	if (size) {
	    bits += tab->len + size;
	    bit_buf <<= tab->len;
	    dc_diff =
		UBITS (bit_buf, size) - UBITS (SBITS (~bit_buf, 1), size);
	    bit_buf <<= size;
	    return dc_diff;
	} else {
	    DUMPBITS (bit_buf, bits, 3);
	    return 0;
	}
    } else {
	tab = DC_long + (UBITS (bit_buf, 9) - 0x1e0);
	size = tab->size;
	DUMPBITS (bit_buf, bits, tab->len);
	NEEDBITS (bit_buf, bits, bit_ptr);
	dc_diff = UBITS (bit_buf, size) - UBITS (SBITS (~bit_buf, 1), size);
	DUMPBITS (bit_buf, bits, size);
	return dc_diff;
    }
#undef bit_buf
#undef bits
#undef bit_ptr
}

static inline int get_chroma_dc_dct_diff (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    DCtab * tab;
    int size;
    int dc_diff;

    if (bit_buf < 0xf8000000) {
	tab = DC_chrom_5 + UBITS (bit_buf, 5);
	size = tab->size;
	if (size) {
	    bits += tab->len + size;
	    bit_buf <<= tab->len;
	    dc_diff =
		UBITS (bit_buf, size) - UBITS (SBITS (~bit_buf, 1), size);
	    bit_buf <<= size;
	    return dc_diff;
	} else {
	    DUMPBITS (bit_buf, bits, 2);
	    return 0;
	}
    } else {
	tab = DC_long + (UBITS (bit_buf, 10) - 0x3e0);
	size = tab->size;
	DUMPBITS (bit_buf, bits, tab->len + 1);
	NEEDBITS (bit_buf, bits, bit_ptr);
	dc_diff = UBITS (bit_buf, size) - UBITS (SBITS (~bit_buf, 1), size);
	DUMPBITS (bit_buf, bits, size);
	return dc_diff;
    }
#undef bit_buf
#undef bits
#undef bit_ptr
}

#define SATURATE(val)			\
do {					\
    if ((uint32_t)(val + 2048) > 4095)	\
	val = (val > 0) ? 2047 : -2048;	\
} while (0)

static void get_intra_block_B14 (picture_t * picture)
{
    int i;
    int j;
    int val;
    uint8_t * scan = picture->scan;
    uint8_t * quant_matrix = picture->intra_quantizer_matrix;
    int quantizer_scale = picture->quantizer_scale;
    int mismatch;
    DCTtab * tab;
    uint32_t bit_buf;
    int bits;
    uint8_t * bit_ptr;
    int16_t * dest;

    dest = picture->DCTblock;
    i = 0;
    mismatch = ~dest[0];

    bit_buf = picture->bitstream_buf;
    bits = picture->bitstream_bits;
    bit_ptr = picture->bitstream_ptr;

    NEEDBITS (bit_buf, bits, bit_ptr);

    while (1) {
	if (bit_buf >= 0x28000000) {

	    tab = DCT_B14AC_5 + (UBITS (bit_buf, 5) - 5);

	    i += tab->run;
	    if (i >= 64)
		break;	/* end of block */

	normal_code:
	    j = scan[i];
	    bit_buf <<= tab->len;
	    bits += tab->len + 1;
	    val = (tab->level * quantizer_scale * quant_matrix[j]) >> 4;

	    /* if (bitstream_get (1)) val = -val; */
	    val = (val ^ SBITS (bit_buf, 1)) - SBITS (bit_buf, 1);

	    SATURATE (val);
	    dest[j] = val;
	    mismatch ^= val;

	    bit_buf <<= 1;
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	} else if (bit_buf >= 0x04000000) {

	    tab = DCT_B14_8 + (UBITS (bit_buf, 8) - 4);

	    i += tab->run;
	    if (i < 64)
		goto normal_code;

	    /* escape code */

	    i += UBITS (bit_buf << 6, 6) - 64;
	    if (i >= 64)
		break;	/* illegal, check needed to avoid buffer overflow */

	    j = scan[i];

	    DUMPBITS (bit_buf, bits, 12);
	    NEEDBITS (bit_buf, bits, bit_ptr);
	    val = (SBITS (bit_buf, 12) *
		   quantizer_scale * quant_matrix[j]) / 16;

	    SATURATE (val);
	    dest[j] = val;
	    mismatch ^= val;

	    DUMPBITS (bit_buf, bits, 12);
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	} else if (bit_buf >= 0x02000000) {
	    tab = DCT_B14_10 + (UBITS (bit_buf, 10) - 8);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00800000) {
	    tab = DCT_13 + (UBITS (bit_buf, 13) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00200000) {
	    tab = DCT_15 + (UBITS (bit_buf, 15) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else {
	    tab = DCT_16 + UBITS (bit_buf, 16);
	    bit_buf <<= 16;
	    GETWORD (bit_buf, bits + 16, bit_ptr);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	}
	break;	/* illegal, check needed to avoid buffer overflow */
    }
    dest[63] ^= mismatch & 1;
    DUMPBITS (bit_buf, bits, 2);	/* dump end of block code */
    picture->bitstream_buf = bit_buf;
    picture->bitstream_bits = bits;
    picture->bitstream_ptr = bit_ptr;
}

static void get_intra_block_B15 (picture_t * picture)
{
    int i;
    int j;
    int val;
    uint8_t * scan = picture->scan;
    uint8_t * quant_matrix = picture->intra_quantizer_matrix;
    int quantizer_scale = picture->quantizer_scale;
    int mismatch;
    DCTtab * tab;
    uint32_t bit_buf;
    int bits;
    uint8_t * bit_ptr;
    int16_t * dest;

    dest = picture->DCTblock;
    i = 0;
    mismatch = ~dest[0];

    bit_buf = picture->bitstream_buf;
    bits = picture->bitstream_bits;
    bit_ptr = picture->bitstream_ptr;

    NEEDBITS (bit_buf, bits, bit_ptr);

    while (1) {
	if (bit_buf >= 0x04000000) {

	    tab = DCT_B15_8 + (UBITS (bit_buf, 8) - 4);

	    i += tab->run;
	    if (i < 64) {

	    normal_code:
		j = scan[i];
		bit_buf <<= tab->len;
		bits += tab->len + 1;
		val = (tab->level * quantizer_scale * quant_matrix[j]) >> 4;

		/* if (bitstream_get (1)) val = -val; */
		val = (val ^ SBITS (bit_buf, 1)) - SBITS (bit_buf, 1);

		SATURATE (val);
		dest[j] = val;
		mismatch ^= val;

		bit_buf <<= 1;
		NEEDBITS (bit_buf, bits, bit_ptr);

		continue;

	    } else {

		/* end of block. I commented out this code because if we */
		/* dont exit here we will still exit at the later test :) */

		/* if (i >= 128) break;	*/	/* end of block */

		/* escape code */

		i += UBITS (bit_buf << 6, 6) - 64;
		if (i >= 64)
		    break;	/* illegal, check against buffer overflow */

		j = scan[i];

		DUMPBITS (bit_buf, bits, 12);
		NEEDBITS (bit_buf, bits, bit_ptr);
		val = (SBITS (bit_buf, 12) *
		       quantizer_scale * quant_matrix[j]) / 16;

		SATURATE (val);
		dest[j] = val;
		mismatch ^= val;

		DUMPBITS (bit_buf, bits, 12);
		NEEDBITS (bit_buf, bits, bit_ptr);

		continue;

	    }
	} else if (bit_buf >= 0x02000000) {
	    tab = DCT_B15_10 + (UBITS (bit_buf, 10) - 8);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00800000) {
	    tab = DCT_13 + (UBITS (bit_buf, 13) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00200000) {
	    tab = DCT_15 + (UBITS (bit_buf, 15) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else {
	    tab = DCT_16 + UBITS (bit_buf, 16);
	    bit_buf <<= 16;
	    GETWORD (bit_buf, bits + 16, bit_ptr);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	}
	break;	/* illegal, check needed to avoid buffer overflow */
    }
    dest[63] ^= mismatch & 1;
    DUMPBITS (bit_buf, bits, 4);	/* dump end of block code */
    picture->bitstream_buf = bit_buf;
    picture->bitstream_bits = bits;
    picture->bitstream_ptr = bit_ptr;
}

static void get_non_intra_block (picture_t * picture)
{
    int i;
    int j;
    int val;
    uint8_t * scan = picture->scan;
    uint8_t * quant_matrix = picture->non_intra_quantizer_matrix;
    int quantizer_scale = picture->quantizer_scale;
    int mismatch;
    DCTtab * tab;
    uint32_t bit_buf;
    int bits;
    uint8_t * bit_ptr;
    int16_t * dest;

    i = -1;
    mismatch = 1;
    dest = picture->DCTblock;

    bit_buf = picture->bitstream_buf;
    bits = picture->bitstream_bits;
    bit_ptr = picture->bitstream_ptr;

    NEEDBITS (bit_buf, bits, bit_ptr);
    if (bit_buf >= 0x28000000) {
	tab = DCT_B14DC_5 + (UBITS (bit_buf, 5) - 5);
	goto entry_1;
    } else
	goto entry_2;

    while (1) {
	if (bit_buf >= 0x28000000) {

	    tab = DCT_B14AC_5 + (UBITS (bit_buf, 5) - 5);

	entry_1:
	    i += tab->run;
	    if (i >= 64)
		break;	/* end of block */

	normal_code:
	    j = scan[i];
	    bit_buf <<= tab->len;
	    bits += tab->len + 1;
	    val = ((2*tab->level+1) * quantizer_scale * quant_matrix[j]) >> 5;

	    /* if (bitstream_get (1)) val = -val; */
	    val = (val ^ SBITS (bit_buf, 1)) - SBITS (bit_buf, 1);

	    SATURATE (val);
	    dest[j] = val;
	    mismatch ^= val;

	    bit_buf <<= 1;
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	}

    entry_2:
	if (bit_buf >= 0x04000000) {

	    tab = DCT_B14_8 + (UBITS (bit_buf, 8) - 4);

	    i += tab->run;
	    if (i < 64)
		goto normal_code;

	    /* escape code */

	    i += UBITS (bit_buf << 6, 6) - 64;
	    if (i >= 64)
		break;	/* illegal, check needed to avoid buffer overflow */

	    j = scan[i];

	    DUMPBITS (bit_buf, bits, 12);
	    NEEDBITS (bit_buf, bits, bit_ptr);
	    val = 2 * (SBITS (bit_buf, 12) + SBITS (bit_buf, 1)) + 1;
	    val = (val * quantizer_scale * quant_matrix[j]) / 32;

	    SATURATE (val);
	    dest[j] = val;
	    mismatch ^= val;

	    DUMPBITS (bit_buf, bits, 12);
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	} else if (bit_buf >= 0x02000000) {
	    tab = DCT_B14_10 + (UBITS (bit_buf, 10) - 8);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00800000) {
	    tab = DCT_13 + (UBITS (bit_buf, 13) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00200000) {
	    tab = DCT_15 + (UBITS (bit_buf, 15) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else {
	    tab = DCT_16 + UBITS (bit_buf, 16);
	    bit_buf <<= 16;
	    GETWORD (bit_buf, bits + 16, bit_ptr);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	}
	break;	/* illegal, check needed to avoid buffer overflow */
    }
    dest[63] ^= mismatch & 1;
    DUMPBITS (bit_buf, bits, 2);	/* dump end of block code */
    picture->bitstream_buf = bit_buf;
    picture->bitstream_bits = bits;
    picture->bitstream_ptr = bit_ptr;
}

static void get_mpeg1_intra_block (picture_t * picture)
{
    int i;
    int j;
    int val;
    uint8_t * scan = picture->scan;
    uint8_t * quant_matrix = picture->intra_quantizer_matrix;
    int quantizer_scale = picture->quantizer_scale;
    DCTtab * tab;
    uint32_t bit_buf;
    int bits;
    uint8_t * bit_ptr;
    int16_t * dest;

    i = 0;
    dest = picture->DCTblock;

    bit_buf = picture->bitstream_buf;
    bits = picture->bitstream_bits;
    bit_ptr = picture->bitstream_ptr;

    NEEDBITS (bit_buf, bits, bit_ptr);

    while (1) {
	if (bit_buf >= 0x28000000) {

	    tab = DCT_B14AC_5 + (UBITS (bit_buf, 5) - 5);

	    i += tab->run;
	    if (i >= 64)
		break;	/* end of block */

	normal_code:
	    j = scan[i];
	    bit_buf <<= tab->len;
	    bits += tab->len + 1;
	    val = (tab->level * quantizer_scale * quant_matrix[j]) >> 4;

	    /* oddification */
	    val = (val - 1) | 1;

	    /* if (bitstream_get (1)) val = -val; */
	    val = (val ^ SBITS (bit_buf, 1)) - SBITS (bit_buf, 1);

	    SATURATE (val);
	    dest[j] = val;

	    bit_buf <<= 1;
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	} else if (bit_buf >= 0x04000000) {

	    tab = DCT_B14_8 + (UBITS (bit_buf, 8) - 4);

	    i += tab->run;
	    if (i < 64)
		goto normal_code;

	    /* escape code */

	    i += UBITS (bit_buf << 6, 6) - 64;
	    if (i >= 64)
		break;	/* illegal, check needed to avoid buffer overflow */

	    j = scan[i];

	    DUMPBITS (bit_buf, bits, 12);
	    NEEDBITS (bit_buf, bits, bit_ptr);
	    val = SBITS (bit_buf, 8);
	    if (! (val & 0x7f)) {
		DUMPBITS (bit_buf, bits, 8);
		val = UBITS (bit_buf, 8) + 2 * val;
	    }
	    val = (val * quantizer_scale * quant_matrix[j]) / 16;

	    /* oddification */
	    val = (val + ~SBITS (val, 1)) | 1;

	    SATURATE (val);
	    dest[j] = val;

	    DUMPBITS (bit_buf, bits, 8);
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	} else if (bit_buf >= 0x02000000) {
	    tab = DCT_B14_10 + (UBITS (bit_buf, 10) - 8);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00800000) {
	    tab = DCT_13 + (UBITS (bit_buf, 13) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00200000) {
	    tab = DCT_15 + (UBITS (bit_buf, 15) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else {
	    tab = DCT_16 + UBITS (bit_buf, 16);
	    bit_buf <<= 16;
	    GETWORD (bit_buf, bits + 16, bit_ptr);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	}
	break;	/* illegal, check needed to avoid buffer overflow */
    }
    DUMPBITS (bit_buf, bits, 2);	/* dump end of block code */
    picture->bitstream_buf = bit_buf;
    picture->bitstream_bits = bits;
    picture->bitstream_ptr = bit_ptr;
}

static void get_mpeg1_non_intra_block (picture_t * picture)
{
    int i;
    int j;
    int val;
    uint8_t * scan = picture->scan;
    uint8_t * quant_matrix = picture->non_intra_quantizer_matrix;
    int quantizer_scale = picture->quantizer_scale;
    DCTtab * tab;
    uint32_t bit_buf;
    int bits;
    uint8_t * bit_ptr;
    int16_t * dest;

    i = -1;
    dest = picture->DCTblock;

    bit_buf = picture->bitstream_buf;
    bits = picture->bitstream_bits;
    bit_ptr = picture->bitstream_ptr;

    NEEDBITS (bit_buf, bits, bit_ptr);
    if (bit_buf >= 0x28000000) {
	tab = DCT_B14DC_5 + (UBITS (bit_buf, 5) - 5);
	goto entry_1;
    } else
	goto entry_2;

    while (1) {
	if (bit_buf >= 0x28000000) {

	    tab = DCT_B14AC_5 + (UBITS (bit_buf, 5) - 5);

	entry_1:
	    i += tab->run;
	    if (i >= 64)
		break;	/* end of block */

	normal_code:
	    j = scan[i];
	    bit_buf <<= tab->len;
	    bits += tab->len + 1;
	    val = ((2*tab->level+1) * quantizer_scale * quant_matrix[j]) >> 5;

	    /* oddification */
	    val = (val - 1) | 1;

	    /* if (bitstream_get (1)) val = -val; */
	    val = (val ^ SBITS (bit_buf, 1)) - SBITS (bit_buf, 1);

	    SATURATE (val);
	    dest[j] = val;

	    bit_buf <<= 1;
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	}

    entry_2:
	if (bit_buf >= 0x04000000) {

	    tab = DCT_B14_8 + (UBITS (bit_buf, 8) - 4);

	    i += tab->run;
	    if (i < 64)
		goto normal_code;

	    /* escape code */

	    i += UBITS (bit_buf << 6, 6) - 64;
	    if (i >= 64)
		break;	/* illegal, check needed to avoid buffer overflow */

	    j = scan[i];

	    DUMPBITS (bit_buf, bits, 12);
	    NEEDBITS (bit_buf, bits, bit_ptr);
	    val = SBITS (bit_buf, 8);
	    if (! (val & 0x7f)) {
		DUMPBITS (bit_buf, bits, 8);
		val = UBITS (bit_buf, 8) + 2 * val;
	    }
	    val = 2 * (val + SBITS (val, 1)) + 1;
	    val = (val * quantizer_scale * quant_matrix[j]) / 32;

	    /* oddification */
	    val = (val + ~SBITS (val, 1)) | 1;

	    SATURATE (val);
	    dest[j] = val;

	    DUMPBITS (bit_buf, bits, 8);
	    NEEDBITS (bit_buf, bits, bit_ptr);

	    continue;

	} else if (bit_buf >= 0x02000000) {
	    tab = DCT_B14_10 + (UBITS (bit_buf, 10) - 8);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00800000) {
	    tab = DCT_13 + (UBITS (bit_buf, 13) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else if (bit_buf >= 0x00200000) {
	    tab = DCT_15 + (UBITS (bit_buf, 15) - 16);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	} else {
	    tab = DCT_16 + UBITS (bit_buf, 16);
	    bit_buf <<= 16;
	    GETWORD (bit_buf, bits + 16, bit_ptr);
	    i += tab->run;
	    if (i < 64)
		goto normal_code;
	}
	break;	/* illegal, check needed to avoid buffer overflow */
    }
    DUMPBITS (bit_buf, bits, 2);	/* dump end of block code */
    picture->bitstream_buf = bit_buf;
    picture->bitstream_bits = bits;
    picture->bitstream_ptr = bit_ptr;
}

static inline void slice_intra_DCT (picture_t * picture, int cc,
				    uint8_t * dest, int stride)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)  
#define bit_ptr (picture->bitstream_ptr)
    NEEDBITS (bit_buf, bits, bit_ptr);
    /* Get the intra DC coefficient and inverse quantize it */
    if (cc == 0)
	picture->dc_dct_pred[0] += get_luma_dc_dct_diff (picture);
    else
	picture->dc_dct_pred[cc] += get_chroma_dc_dct_diff (picture);
    picture->DCTblock[0] =
	picture->dc_dct_pred[cc] << (3 - picture->intra_dc_precision);
    memset (picture->DCTblock + 1, 0, 63 * sizeof (int16_t));

    if (picture->mpeg1) {
	if (picture->picture_coding_type != D_TYPE)
	    get_mpeg1_intra_block (picture);
    } else if (picture->intra_vlc_format)
	get_intra_block_B15 (picture);
    else
	get_intra_block_B14 (picture);
    mpeg2_idct_copy (picture->DCTblock, dest, stride);
#undef bit_buf
#undef bits
#undef bit_ptr
}

static inline void slice_non_intra_DCT (picture_t * picture, uint8_t * dest,
					int stride)
{
    memset (picture->DCTblock, 0, 64 * sizeof (int16_t));
    if (picture->mpeg1)
	get_mpeg1_non_intra_block (picture);
    else
	get_non_intra_block (picture);
    mpeg2_idct_add (picture->DCTblock, dest, stride);
}

#define MOTION_Y(table,offset_x,offset_y,motion_x,motion_y,		\
		 dest,src,offset_dest,offset_src,stride,height)		\
do {									\
    int xy_half;							\
    int total_offset;							\
									\
    xy_half = ((motion_y & 1) << 1) | (motion_x & 1);			\
    total_offset = ((offset_y + (motion_y >> 1)) * stride +		\
		    offset_x + (motion_x >> 1) + (offset_src));		\
    table[xy_half] (dest[0] + offset_x + (offset_dest),			\
		    src[0] + total_offset, stride, height);		\
} while (0)

#define MOTION_UV(table,offset_x,offset_y,motion_x,motion_y,		\
		  dest,src,offset_dest,offset_src,stride,height)	\
do {									\
    int xy_half;							\
    int total_offset;							\
									\
    xy_half = ((motion_y & 1) << 1) | (motion_x & 1);			\
    total_offset = (((offset_y + motion_y) >> 1) * (stride) +		\
		    ((offset_x + motion_x) >> 1) + (offset_src));	\
    table[4+xy_half] (dest[1] + (offset_x >> 1) + (offset_dest),	\
		      src[1] + total_offset, stride, height);		\
    table[4+xy_half] (dest[2] + (offset_x >> 1) + (offset_dest),	\
		      src[2] + total_offset, stride, height);		\
} while (0)

static inline void motion_block (void (** table) (uint8_t *, uint8_t *,
						  int32_t, int32_t),
				 int x_offset, int y_offset, int mb_y_8_offset,
				 int src_field, int dest_field,
				 int x_pred, int y_pred,
				 uint8_t * dest[3], uint8_t * src[3],
				 int stride, int height)
{
    MOTION_Y (table, x_offset, y_offset, x_pred, y_pred, dest, src,
	      dest_field + mb_y_8_offset*8*stride, src_field, stride, height);

    x_pred /= 2;
    y_pred /= 2;
    stride >>= 1;
    height >>= 1;

    MOTION_UV (table, x_offset, y_offset, x_pred, y_pred, dest, src,
	       (dest_field >> 1) + mb_y_8_offset*4*stride, src_field >> 1,
	       stride, height);
}

static void motion_mp1 (picture_t * picture, motion_t * motion,
			void (** table) (uint8_t *, uint8_t *, int, int))
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int motion_x, motion_y;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_x = motion->pmv[0][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[0][0] = motion_x;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_y = motion->pmv[0][1] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_y = bound_motion_vector (motion_y, motion->f_code[0]);
    motion->pmv[0][1] = motion_y;

    motion_x <<= motion->f_code[1];	/* full-pixel MC */
    motion_y <<= motion->f_code[1];

    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0,
		  motion_x, motion_y, picture->dest, motion->ref[0],
		  picture->stride, 16);
#undef bit_buf
#undef bits
#undef bit_ptr
}

static void motion_mp1_reuse (picture_t * picture, motion_t * motion,
			      void (** table) (uint8_t *, uint8_t *, int, int))
{
    int motion_x, motion_y;

    motion_x = motion->pmv[0][0] << motion->f_code[1];	/* full-pixel MC */
    motion_y = motion->pmv[0][1] << motion->f_code[1];

    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0,
		  motion_x, motion_y, picture->dest, motion->ref[0],
		  picture->stride, 16);
}

static void motion_fr_frame (picture_t * picture, motion_t * motion,
			     void (** table) (uint8_t *, uint8_t *, int, int))
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int motion_x, motion_y;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_x = motion->pmv[0][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[1][0] = motion->pmv[0][0] = motion_x;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_y = motion->pmv[0][1] + get_motion_delta (picture,
						     motion->f_code[1]);
    motion_y = bound_motion_vector (motion_y, motion->f_code[1]);
    motion->pmv[1][1] = motion->pmv[0][1] = motion_y;

    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0,
		  motion_x, motion_y, picture->dest, motion->ref[0],
		  picture->stride, 16);
#undef bit_buf
#undef bits
#undef bit_ptr
}

static void motion_fr_field (picture_t * picture, motion_t * motion,
			     void (** table) (uint8_t *, uint8_t *, int, int))
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int motion_x, motion_y;
    int field_select;

    NEEDBITS (bit_buf, bits, bit_ptr);
    field_select = SBITS (bit_buf, 1);
    DUMPBITS (bit_buf, bits, 1);

    motion_x = motion->pmv[0][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[0][0] = motion_x;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_y = (motion->pmv[0][1] >> 1) + get_motion_delta (picture,
							    motion->f_code[1]);
    /* motion_y = bound_motion_vector (motion_y, motion->f_code[1]); */
    motion->pmv[0][1] = motion_y << 1;

    motion_block (table, picture->offset, picture->v_offset >> 1,
		  0, (field_select & picture->stride), 0, motion_x, motion_y,
		  picture->dest, motion->ref[0], picture->stride * 2, 8);

    NEEDBITS (bit_buf, bits, bit_ptr);
    field_select = SBITS (bit_buf, 1);
    DUMPBITS (bit_buf, bits, 1);

    motion_x = motion->pmv[1][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[1][0] = motion_x;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_y = (motion->pmv[1][1] >> 1) + get_motion_delta (picture,
							    motion->f_code[1]);
    /* motion_y = bound_motion_vector (motion_y, motion->f_code[1]); */
    motion->pmv[1][1] = motion_y << 1;

    motion_block (table, picture->offset, picture->v_offset >> 1,
		  0, (field_select & picture->stride), picture->stride,
		  motion_x, motion_y, picture->dest, motion->ref[0],
		  picture->stride * 2, 8);
#undef bit_buf
#undef bits
#undef bit_ptr
}

static void motion_fr_dmv (picture_t * picture, motion_t * motion,
			   void (** table) (uint8_t *, uint8_t *, int, int))
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int motion_x, motion_y;
    int dmv_x, dmv_y;
    int m;
    int other_x, other_y;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_x = motion->pmv[0][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[1][0] = motion->pmv[0][0] = motion_x;
    NEEDBITS (bit_buf, bits, bit_ptr);
    dmv_x = get_dmv (picture);

    motion_y = (motion->pmv[0][1] >> 1) + get_motion_delta (picture,
							    motion->f_code[1]);
    /* motion_y = bound_motion_vector (motion_y, motion->f_code[1]); */
    motion->pmv[1][1] = motion->pmv[0][1] = motion_y << 1;
    dmv_y = get_dmv (picture);

    motion_block (mpeg2_mc.put, picture->offset, picture->v_offset >> 1,
		  0, 0, 0, motion_x, motion_y, picture->dest, motion->ref[0],
		  picture->stride * 2, 8);

    m = picture->top_field_first ? 1 : 3;
    other_x = ((motion_x * m + (motion_x > 0)) >> 1) + dmv_x;
    other_y = ((motion_y * m + (motion_y > 0)) >> 1) + dmv_y - 1;
    motion_block (mpeg2_mc.avg, picture->offset, picture->v_offset >> 1,
		  0, picture->stride, 0, other_x, other_y, picture->dest,
		  motion->ref[0], picture->stride * 2, 8);

    motion_block (mpeg2_mc.put, picture->offset, picture->v_offset >> 1,
		  0, picture->stride, picture->stride, motion_x, motion_y,
		  picture->dest, motion->ref[0], picture->stride * 2, 8);

    m = picture->top_field_first ? 3 : 1;
    other_x = ((motion_x * m + (motion_x > 0)) >> 1) + dmv_x;
    other_y = ((motion_y * m + (motion_y > 0)) >> 1) + dmv_y + 1;
    motion_block (mpeg2_mc.avg, picture->offset, picture->v_offset >> 1,
		  0, 0, picture->stride, other_x, other_y, picture->dest,
		  motion->ref[0], picture->stride * 2, 8);
#undef bit_buf
#undef bits
#undef bit_ptr
}

/* like motion_frame, but reuse previous motion vectors */
static void motion_fr_reuse (picture_t * picture, motion_t * motion,
			     void (** table) (uint8_t *, uint8_t *, int, int))
{
    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0,
		  motion->pmv[0][0], motion->pmv[0][1], picture->dest,
		  motion->ref[0], picture->stride, 16);
}

/* like motion_frame, but use null motion vectors */
static void motion_fr_zero (picture_t * picture, motion_t * motion,
			    void (** table) (uint8_t *, uint8_t *, int, int))
{
    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0, 0, 0,
		  picture->dest, motion->ref[0], picture->stride, 16);
}

/* like motion_frame, but parsing without actual motion compensation */
static void motion_fr_conceal (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int tmp;

    NEEDBITS (bit_buf, bits, bit_ptr);
    tmp = (picture->f_motion.pmv[0][0] +
	   get_motion_delta (picture, picture->f_motion.f_code[0]));
    tmp = bound_motion_vector (tmp, picture->f_motion.f_code[0]);
    picture->f_motion.pmv[1][0] = picture->f_motion.pmv[0][0] = tmp;

    NEEDBITS (bit_buf, bits, bit_ptr);
    tmp = (picture->f_motion.pmv[0][1] +
	   get_motion_delta (picture, picture->f_motion.f_code[1]));
    tmp = bound_motion_vector (tmp, picture->f_motion.f_code[1]);
    picture->f_motion.pmv[1][1] = picture->f_motion.pmv[0][1] = tmp;

    DUMPBITS (bit_buf, bits, 1); /* remove marker_bit */
#undef bit_buf
#undef bits
#undef bit_ptr
}

static void motion_fi_field (picture_t * picture, motion_t * motion,
			     void (** table) (uint8_t *, uint8_t *, int, int))
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int motion_x, motion_y;
    int field_select;

    NEEDBITS (bit_buf, bits, bit_ptr);
    field_select = UBITS (bit_buf, 1);
    DUMPBITS (bit_buf, bits, 1);

    motion_x = motion->pmv[0][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[1][0] = motion->pmv[0][0] = motion_x;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_y = motion->pmv[0][1] + get_motion_delta (picture,
						     motion->f_code[1]);
    motion_y = bound_motion_vector (motion_y, motion->f_code[1]);
    motion->pmv[1][1] = motion->pmv[0][1] = motion_y;

    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0,
		  motion_x, motion_y, picture->dest, motion->ref[field_select],
		  picture->stride, 16);
#undef bit_buf
#undef bits
#undef bit_ptr
}

static void motion_fi_16x8 (picture_t * picture, motion_t * motion,
			    void (** table) (uint8_t *, uint8_t *, int, int))
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int motion_x, motion_y;
    int field_select;

    NEEDBITS (bit_buf, bits, bit_ptr);
    field_select = UBITS (bit_buf, 1);
    DUMPBITS (bit_buf, bits, 1);

    motion_x = motion->pmv[0][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[0][0] = motion_x;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_y = motion->pmv[0][1] + get_motion_delta (picture,
						     motion->f_code[1]);
    motion_y = bound_motion_vector (motion_y, motion->f_code[1]);
    motion->pmv[0][1] = motion_y;

    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0,
		  motion_x, motion_y, picture->dest, motion->ref[field_select],
		  picture->stride, 8);

    NEEDBITS (bit_buf, bits, bit_ptr);
    field_select = UBITS (bit_buf, 1);
    DUMPBITS (bit_buf, bits, 1);

    motion_x = motion->pmv[1][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[1][0] = motion_x;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_y = motion->pmv[1][1] + get_motion_delta (picture,
						     motion->f_code[1]);
    motion_y = bound_motion_vector (motion_y, motion->f_code[1]);
    motion->pmv[1][1] = motion_y;

    motion_block (table, picture->offset, picture->v_offset+8, 1, 0, 0,
		  motion_x, motion_y, picture->dest, motion->ref[field_select],
		  picture->stride, 8);
#undef bit_buf
#undef bits
#undef bit_ptr
}

static void motion_fi_dmv (picture_t * picture, motion_t * motion,
			   void (** table) (uint8_t *, uint8_t *, int, int))
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int motion_x, motion_y;
    int dmv_x, dmv_y;

    NEEDBITS (bit_buf, bits, bit_ptr);
    motion_x = motion->pmv[0][0] + get_motion_delta (picture,
						     motion->f_code[0]);
    motion_x = bound_motion_vector (motion_x, motion->f_code[0]);
    motion->pmv[1][0] = motion->pmv[0][0] = motion_x;
    NEEDBITS (bit_buf, bits, bit_ptr);
    dmv_x = get_dmv (picture);

    motion_y = motion->pmv[0][1] + get_motion_delta (picture,
						     motion->f_code[1]);
    motion_y = bound_motion_vector (motion_y, motion->f_code[1]);
    motion->pmv[1][1] = motion->pmv[0][1] = motion_y;
    dmv_y = get_dmv (picture);

    motion_block (mpeg2_mc.put, picture->offset, picture->v_offset, 0, 0, 0,
		  motion_x, motion_y, picture->dest,
		  motion->ref[picture->current_field], picture->stride, 16);

    motion_x = ((motion_x + (motion_x > 0)) >> 1) + dmv_x;
    motion_y = ((motion_y + (motion_y > 0)) >> 1) + dmv_y +
	2 * picture->current_field - 1;
    motion_block (mpeg2_mc.avg, picture->offset, picture->v_offset, 0, 0, 0,
		  motion_x, motion_y,
		  picture->dest, motion->ref[!picture->current_field], picture->stride, 16);
#undef bit_buf
#undef bits
#undef bit_ptr
}

static void motion_fi_reuse (picture_t * picture, motion_t * motion,
			     void (** table) (uint8_t *, uint8_t *, int, int))
{
    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0,
		  motion->pmv[0][0], motion->pmv[0][1], picture->dest,
		  motion->ref[picture->current_field], picture->stride, 16);
}

static void motion_fi_zero (picture_t * picture, motion_t * motion,
			    void (** table) (uint8_t *, uint8_t *, int, int))
{
    motion_block (table, picture->offset, picture->v_offset, 0, 0, 0, 0, 0,
		  picture->dest, motion->ref[picture->current_field],
		  picture->stride, 16);
}

static void motion_fi_conceal (picture_t * picture)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int tmp;

    NEEDBITS (bit_buf, bits, bit_ptr);
    DUMPBITS (bit_buf, bits, 1); /* remove field_select */

    tmp = (picture->f_motion.pmv[0][0] +
	   get_motion_delta (picture, picture->f_motion.f_code[0]));
    tmp = bound_motion_vector (tmp, picture->f_motion.f_code[0]);
    picture->f_motion.pmv[1][0] = picture->f_motion.pmv[0][0] = tmp;

    NEEDBITS (bit_buf, bits, bit_ptr);
    tmp = (picture->f_motion.pmv[0][1] +
	   get_motion_delta (picture, picture->f_motion.f_code[1]));
    tmp = bound_motion_vector (tmp, picture->f_motion.f_code[1]);
    picture->f_motion.pmv[1][1] = picture->f_motion.pmv[0][1] = tmp;

    DUMPBITS (bit_buf, bits, 1); /* remove marker_bit */
#undef bit_buf
#undef bits
#undef bit_ptr
}

#define MOTION(routine,direction)				\
do {								\
    if ((direction) & MACROBLOCK_MOTION_FORWARD)		\
	routine (picture, &(picture->f_motion), mpeg2_mc.put);	\
    if ((direction) & MACROBLOCK_MOTION_BACKWARD)		\
	routine (picture, &(picture->b_motion),			\
		 ((direction) & MACROBLOCK_MOTION_FORWARD ?	\
		  mpeg2_mc.avg : mpeg2_mc.put));		\
} while (0)

#define NEXT_MACROBLOCK							\
do {									\
    picture->offset += 16;						\
    if (picture->offset == picture->coded_picture_width) {		\
	do { /* just so we can use the break statement */		\
	    if (picture->current_frame->copy) {				\
		picture->current_frame->copy (picture->current_frame,	\
					      picture->dest);		\
		if (picture->picture_coding_type == B_TYPE)		\
		    break;						\
	    }								\
	    picture->dest[0] += 16 * picture->stride;			\
	    picture->dest[1] += 4 * picture->stride;			\
	    picture->dest[2] += 4 * picture->stride;			\
	} while (0);							\
	picture->v_offset += 16;					\
	if (picture->v_offset >= picture->coded_picture_height) {	\
	    if (mpeg2_cpu_state_restore)				\
		mpeg2_cpu_state_restore (&cpu_state);			\
	    return;							\
	}								\
	picture->offset = 0;						\
    }									\
} while (0)

static inline int slice_init (picture_t * picture, int code)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    int offset, stride;
    uint8_t ** forward_ref[2];
    MBAtab * mba;

    stride = picture->coded_picture_width;
    offset = (code - 1) * stride * 4;
    picture->v_offset = (code - 1) * 16;

    forward_ref[0] = picture->forward_reference_frame->base;
    if (picture->picture_structure != FRAME_PICTURE) {
	forward_ref[1] = picture->forward_reference_frame->base;
	offset <<= 1;
	picture->current_field = (picture->picture_structure == BOTTOM_FIELD);
	if ((picture->second_field) &&
	    (picture->picture_coding_type != B_TYPE))
	    forward_ref[picture->picture_structure == TOP_FIELD] =
		picture->current_frame->base;

	picture->f_motion.ref[1][0] = forward_ref[1][0] + stride;
	picture->f_motion.ref[1][1] = forward_ref[1][1] + (stride >> 1);
	picture->f_motion.ref[1][2] = forward_ref[1][2] + (stride >> 1);

	picture->b_motion.ref[1][0] =
	    picture->backward_reference_frame->base[0] + stride;
	picture->b_motion.ref[1][1] =
	    picture->backward_reference_frame->base[1] + (stride >> 1);
	picture->b_motion.ref[1][2] =
	    picture->backward_reference_frame->base[2] + (stride >> 1);
    }

    picture->f_motion.ref[0][0] = forward_ref[0][0];
    picture->f_motion.ref[0][1] = forward_ref[0][1];
    picture->f_motion.ref[0][2] = forward_ref[0][2];

    picture->f_motion.pmv[0][0] = picture->f_motion.pmv[0][1] = 0;
    picture->f_motion.pmv[1][0] = picture->f_motion.pmv[1][1] = 0;

    picture->b_motion.ref[0][0] = picture->backward_reference_frame->base[0];
    picture->b_motion.ref[0][1] = picture->backward_reference_frame->base[1];
    picture->b_motion.ref[0][2] = picture->backward_reference_frame->base[2];

    picture->b_motion.pmv[0][0] = picture->b_motion.pmv[0][1] = 0;
    picture->b_motion.pmv[1][0] = picture->b_motion.pmv[1][1] = 0;

    if ((picture->current_frame->copy) &&
	(picture->picture_coding_type == B_TYPE))
	offset = 0;

    picture->dest[0] = picture->current_frame->base[0] + offset * 4;
    picture->dest[1] = picture->current_frame->base[1] + offset;
    picture->dest[2] = picture->current_frame->base[2] + offset;

    switch (picture->picture_structure) {
    case BOTTOM_FIELD:
	picture->dest[0] += stride;
	picture->dest[1] += stride >> 1;
	picture->dest[2] += stride >> 1;
	/* follow thru */
    case TOP_FIELD:
	stride <<= 1;
    }
    picture->stride = stride;
    picture->uv_stride = stride >> 1;

    picture->dc_dct_pred[0] = picture->dc_dct_pred[1] =
	picture->dc_dct_pred[2] = 1 << (picture->intra_dc_precision + 7);

    picture->quantizer_scale = get_quantizer_scale (picture);

    /* ignore intra_slice and all the extra data */
    while (bit_buf & 0x80000000) {
	DUMPBITS (bit_buf, bits, 9);
	NEEDBITS (bit_buf, bits, bit_ptr);
    }

    /* decode initial macroblock address increment */
    offset = 0;
    while (1) {
	if (bit_buf >= 0x08000000) {
	    mba = MBA_5 + (UBITS (bit_buf, 6) - 2);
	    break;
	} else if (bit_buf >= 0x01800000) {
	    mba = MBA_11 + (UBITS (bit_buf, 12) - 24);
	    break;
	} else switch (UBITS (bit_buf, 12)) {
	case 8:		/* macroblock_escape */
	    offset += 33;
	    DUMPBITS (bit_buf, bits, 11);
	    NEEDBITS (bit_buf, bits, bit_ptr);
	    continue;
	case 15:	/* macroblock_stuffing (MPEG1 only) */
	    bit_buf &= 0xfffff;
	    DUMPBITS (bit_buf, bits, 11);
	    NEEDBITS (bit_buf, bits, bit_ptr);
	    continue;
	default:	/* error */
	    return 1;
	}
    }
    DUMPBITS (bit_buf, bits, mba->len + 1);
    picture->offset = (offset + mba->mba) << 4;

    while (picture->offset - picture->coded_picture_width >= 0) {
	picture->offset -= picture->coded_picture_width;
	if ((picture->current_frame->copy == NULL) ||
	    (picture->picture_coding_type != B_TYPE)) {
	    picture->dest[0] += 16 * stride;
	    picture->dest[1] += 4 * stride;
	    picture->dest[2] += 4 * stride;
	}
	picture->v_offset += 16;
    }
    if (picture->v_offset >= picture->coded_picture_height)
	return 1;

    return 0;
#undef bit_buf
#undef bits
#undef bit_ptr
}

void mpeg2_slice (picture_t * picture, int code, uint8_t * buffer)
{
#define bit_buf (picture->bitstream_buf)
#define bits (picture->bitstream_bits)
#define bit_ptr (picture->bitstream_ptr)
    cpu_state_t cpu_state;

    bitstream_init (picture, buffer);

    if (slice_init (picture, code))
	return;

    if (mpeg2_cpu_state_save)
	mpeg2_cpu_state_save (&cpu_state);

    while (1) {
	int macroblock_modes;
	int mba_inc;
	MBAtab * mba;

	NEEDBITS (bit_buf, bits, bit_ptr);

	macroblock_modes = get_macroblock_modes (picture);

	/* maybe integrate MACROBLOCK_QUANT test into get_macroblock_modes ? */
	if (macroblock_modes & MACROBLOCK_QUANT)
	    picture->quantizer_scale = get_quantizer_scale (picture);

	if (macroblock_modes & MACROBLOCK_INTRA) {

	    int DCT_offset, DCT_stride;
	    int offset;
	    uint8_t * dest_y;

	    if (picture->concealment_motion_vectors) {
		if (picture->picture_structure == FRAME_PICTURE)
		    motion_fr_conceal (picture);
		else
		    motion_fi_conceal (picture);
	    } else {
		picture->f_motion.pmv[0][0] = picture->f_motion.pmv[0][1] = 0;
		picture->f_motion.pmv[1][0] = picture->f_motion.pmv[1][1] = 0;
		picture->b_motion.pmv[0][0] = picture->b_motion.pmv[0][1] = 0;
		picture->b_motion.pmv[1][0] = picture->b_motion.pmv[1][1] = 0;
	    }

	    if (macroblock_modes & DCT_TYPE_INTERLACED) {
		DCT_offset = picture->stride;
		DCT_stride = picture->stride * 2;
	    } else {
		DCT_offset = picture->stride * 8;
		DCT_stride = picture->stride;
	    }

	    offset = picture->offset;
	    dest_y = picture->dest[0] + offset;
	    slice_intra_DCT (picture, 0, dest_y, DCT_stride);
	    slice_intra_DCT (picture, 0, dest_y + 8, DCT_stride);
	    slice_intra_DCT (picture, 0, dest_y + DCT_offset, DCT_stride);
	    slice_intra_DCT (picture, 0, dest_y + DCT_offset + 8, DCT_stride);
	    slice_intra_DCT (picture, 1, picture->dest[1] + (offset >> 1),
			     picture->uv_stride);
	    slice_intra_DCT (picture, 2, picture->dest[2] + (offset >> 1),
			     picture->uv_stride);

	    if (picture->picture_coding_type == D_TYPE) {
		NEEDBITS (bit_buf, bits, bit_ptr);
		DUMPBITS (bit_buf, bits, 1);
	    }
	} else {

	    if (picture->mpeg1) {
		if ((macroblock_modes & MOTION_TYPE_MASK) == MC_FRAME)
		    MOTION (motion_mp1, macroblock_modes);
		else {
		    /* non-intra mb without forward mv in a P picture */
		    picture->f_motion.pmv[0][0] = 0;
		    picture->f_motion.pmv[0][1] = 0;
		    picture->f_motion.pmv[1][0] = 0;
		    picture->f_motion.pmv[1][1] = 0;
		    MOTION (motion_fr_zero, MACROBLOCK_MOTION_FORWARD);
		}
	    } else if (picture->picture_structure == FRAME_PICTURE)
		switch (macroblock_modes & MOTION_TYPE_MASK) {
		case MC_FRAME:
		    MOTION (motion_fr_frame, macroblock_modes);
		    break;

		case MC_FIELD:
		    MOTION (motion_fr_field, macroblock_modes);
		    break;

		case MC_DMV:
		    MOTION (motion_fr_dmv, MACROBLOCK_MOTION_FORWARD);
		    break;

		case 0:
		    /* non-intra mb without forward mv in a P picture */
		    picture->f_motion.pmv[0][0] = 0;
		    picture->f_motion.pmv[0][1] = 0;
		    picture->f_motion.pmv[1][0] = 0;
		    picture->f_motion.pmv[1][1] = 0;
		    MOTION (motion_fr_zero, MACROBLOCK_MOTION_FORWARD);
		    break;
		}
	    else
		switch (macroblock_modes & MOTION_TYPE_MASK) {
		case MC_FIELD:
		    MOTION (motion_fi_field, macroblock_modes);
		    break;

		case MC_16X8:
		    MOTION (motion_fi_16x8, macroblock_modes);
		    break;

		case MC_DMV:
		    MOTION (motion_fi_dmv, MACROBLOCK_MOTION_FORWARD);
		    break;

		case 0:
		    /* non-intra mb without forward mv in a P picture */
		    picture->f_motion.pmv[0][0] = 0;
		    picture->f_motion.pmv[0][1] = 0;
		    picture->f_motion.pmv[1][0] = 0;
		    picture->f_motion.pmv[1][1] = 0;
		    MOTION (motion_fi_zero, MACROBLOCK_MOTION_FORWARD);
		    break;
		}

	    if (macroblock_modes & MACROBLOCK_PATTERN) {
		int coded_block_pattern;
		int DCT_offset, DCT_stride;
		int offset;
		uint8_t * dest_y;

		if (macroblock_modes & DCT_TYPE_INTERLACED) {
		    DCT_offset = picture->stride;
		    DCT_stride = picture->stride * 2;
		} else {
		    DCT_offset = picture->stride * 8;
		    DCT_stride = picture->stride;
		}

		coded_block_pattern = get_coded_block_pattern (picture);

		offset = picture->offset;
		dest_y = picture->dest[0] + offset;
		if (coded_block_pattern & 0x20)
		    slice_non_intra_DCT (picture, dest_y, DCT_stride);
		if (coded_block_pattern & 0x10)
		    slice_non_intra_DCT (picture, dest_y + 8, DCT_stride);
		if (coded_block_pattern & 0x08)
		    slice_non_intra_DCT (picture, dest_y + DCT_offset,
					 DCT_stride);
		if (coded_block_pattern & 0x04)
		    slice_non_intra_DCT (picture, dest_y + DCT_offset + 8,
					 DCT_stride);
		if (coded_block_pattern & 0x2)
		    slice_non_intra_DCT (picture,
					 picture->dest[1] + (offset >> 1),
					 picture->uv_stride);
		if (coded_block_pattern & 0x1)
		    slice_non_intra_DCT (picture,
					 picture->dest[2] + (offset >> 1),
					 picture->uv_stride);
	    }

	    picture->dc_dct_pred[0] = picture->dc_dct_pred[1] =
		picture->dc_dct_pred[2] = 128 << picture->intra_dc_precision;
	}

	NEXT_MACROBLOCK;

	NEEDBITS (bit_buf, bits, bit_ptr);
	mba_inc = 0;
	while (1) {
	    if (bit_buf >= 0x10000000) {
		mba = MBA_5 + (UBITS (bit_buf, 5) - 2);
		break;
	    } else if (bit_buf >= 0x03000000) {
		mba = MBA_11 + (UBITS (bit_buf, 11) - 24);
		break;
	    } else switch (UBITS (bit_buf, 11)) {
	    case 8:		/* macroblock_escape */
		mba_inc += 33;
		/* pass through */
	    case 15:	/* macroblock_stuffing (MPEG1 only) */
		DUMPBITS (bit_buf, bits, 11);
		NEEDBITS (bit_buf, bits, bit_ptr);
		continue;
	    default:	/* end of slice, or error */
		if (mpeg2_cpu_state_restore)
		    mpeg2_cpu_state_restore (&cpu_state);
		return;
	    }
	}
	DUMPBITS (bit_buf, bits, mba->len);
	mba_inc += mba->mba;

	if (mba_inc) {
	    picture->dc_dct_pred[0] = picture->dc_dct_pred[1] =
		picture->dc_dct_pred[2] = 128 << picture->intra_dc_precision;

	    if (picture->picture_coding_type == P_TYPE) {
		picture->f_motion.pmv[0][0] = picture->f_motion.pmv[0][1] = 0;
		picture->f_motion.pmv[1][0] = picture->f_motion.pmv[1][1] = 0;

		do {
		    if (picture->picture_structure == FRAME_PICTURE)
			MOTION (motion_fr_zero, MACROBLOCK_MOTION_FORWARD);
		    else
			MOTION (motion_fi_zero, MACROBLOCK_MOTION_FORWARD);
		    NEXT_MACROBLOCK;
		} while (--mba_inc);
	    } else {
		do {
		    if (picture->mpeg1)
			MOTION (motion_mp1_reuse, macroblock_modes);
		    else if (picture->picture_structure == FRAME_PICTURE)
			MOTION (motion_fr_reuse, macroblock_modes);
		    else
			MOTION (motion_fi_reuse, macroblock_modes);
		    NEXT_MACROBLOCK;
		} while (--mba_inc);
	    }
	}
    }
#undef bit_buf
#undef bits
#undef bit_ptr
}
