/*
 * idct_alpha.c
 * Copyright (C) 2002 Falk Hueffner <falk@debian.org>
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307	 USA
 */

#include "config.h"

#ifdef ARCH_ALPHA

#include <inttypes.h>
#include <stdlib.h>

#include "alpha_asm.h"

#define W1 2841 /* 2048*sqrt (2)*cos (1*pi/16) */
#define W2 2676 /* 2048*sqrt (2)*cos (2*pi/16) */
#define W3 2408 /* 2048*sqrt (2)*cos (3*pi/16) */
#define W4 2048 /* 2048*sqrt (2)*cos (4*pi/16) */
#define W5 1609 /* 2048*sqrt (2)*cos (5*pi/16) */
#define W6 1108 /* 2048*sqrt (2)*cos (6*pi/16) */
#define W7 565	/* 2048*sqrt (2)*cos (7*pi/16) */
#define ROW_SHIFT 8
#define COL_SHIFT 17

static uint8_t clip_lut[1024];
#define CLIP(i) ((clip_lut+384)[(i)])

#ifdef __alpha_ev6__
#define SKIP_ZERO_TEST 1
#else
#define SKIP_ZERO_TEST 0
#endif

/* 0: all entries 0, 1: only first entry nonzero, 2: otherwise	*/
static inline int idct_row(int16_t *row)
{
    int_fast32_t a0, a1, a2, a3, b0, b1, b2, b3, t;
    uint64_t l, r;
    l = ldq(row);
    r = ldq(row + 4);

    if (l == 0 && r == 0)
	return 0;

    a0 = W4 * sextw(l) + (1 << (ROW_SHIFT - 1));

    if (((l & ~0xffffUL) | r) == 0) {
	a0 >>= ROW_SHIFT;
	a0 = (uint16_t) a0;
	a0 |= a0 << 16;
	a0 |= a0 << 32;
	
	stq(a0, row);
	stq(a0, row + 4);
	return 1;
    }

    a1 = a0;
    a2 = a0;
    a3 = a0;

    t = extwl(l, 4);		/* row[2] */
    if (SKIP_ZERO_TEST || t != 0) {
	t = sextw(t);
	a0 += W2 * t;
	a1 += W6 * t;
	a2 -= W6 * t;
	a3 -= W2 * t;
    }

    t = extwl(r, 0);		/* row[4] */
    if (SKIP_ZERO_TEST || t != 0) {
	t = sextw(t);
	a0 += W4 * t;
	a1 -= W4 * t;
	a2 -= W4 * t;
	a3 += W4 * t;
    }

    t = extwl(r, 4);		/* row[6] */
    if (SKIP_ZERO_TEST || t != 0) {
	t = sextw(t);
	a0 += W6 * t;
	a1 -= W2 * t;
	a2 += W2 * t;
	a3 -= W6 * t;
    }

    t = extwl(l, 2);		/* row[1] */
    if (SKIP_ZERO_TEST || t != 0) {
	t = sextw(t);
	b0 = W1 * t;
	b1 = W3 * t;
	b2 = W5 * t;
	b3 = W7 * t;
    } else {
	b0 = 0;
	b1 = 0;
	b2 = 0;
	b3 = 0;
    }

    t = extwl(l, 6);		/* row[3] */
    if (SKIP_ZERO_TEST || t != 0) {
	t = sextw(t);
	b0 += W3 * t;
	b1 -= W7 * t;
	b2 -= W1 * t;
	b3 -= W5 * t;
    }

    
    t = extwl(r, 2);		/* row[5] */
    if (SKIP_ZERO_TEST || t != 0) {
	t = sextw(t);
	b0 += W5 * t;
	b1 -= W1 * t;
	b2 += W7 * t;
	b3 += W3 * t;
    }

    t = extwl(r, 6);		/* row[7] */
    if (SKIP_ZERO_TEST || t != 0) {
	t = sextw(t);
	b0 += W7 * t;
	b1 -= W5 * t;
	b2 += W3 * t;
	b3 -= W1 * t;
    }

    row[0] = (a0 + b0) >> ROW_SHIFT;
    row[1] = (a1 + b1) >> ROW_SHIFT;
    row[2] = (a2 + b2) >> ROW_SHIFT;
    row[3] = (a3 + b3) >> ROW_SHIFT;
    row[4] = (a3 - b3) >> ROW_SHIFT;
    row[5] = (a2 - b2) >> ROW_SHIFT;
    row[6] = (a1 - b1) >> ROW_SHIFT;
    row[7] = (a0 - b0) >> ROW_SHIFT;

    return 2;
}

static inline void idct_col(int16_t *col)
{
    int_fast32_t a0, a1, a2, a3, b0, b1, b2, b3;

    col[0] += (1 << (COL_SHIFT - 1)) / W4;

    a0 = W4 * col[8 * 0];
    a1 = W4 * col[8 * 0];
    a2 = W4 * col[8 * 0];
    a3 = W4 * col[8 * 0];

    if (SKIP_ZERO_TEST || col[8 * 2]) {
	a0 += W2 * col[8 * 2];
	a1 += W6 * col[8 * 2];
	a2 -= W6 * col[8 * 2];
	a3 -= W2 * col[8 * 2];
    }

    if (SKIP_ZERO_TEST || col[8 * 4]) {
	a0 += W4 * col[8 * 4];
	a1 -= W4 * col[8 * 4];
	a2 -= W4 * col[8 * 4];
	a3 += W4 * col[8 * 4];
    }

    if (SKIP_ZERO_TEST || col[8 * 6]) {
	a0 += W6 * col[8 * 6];
	a1 -= W2 * col[8 * 6];
	a2 += W2 * col[8 * 6];
	a3 -= W6 * col[8 * 6];
    }

    if (SKIP_ZERO_TEST || col[8 * 1]) {
	b0 = W1 * col[8 * 1];
	b1 = W3 * col[8 * 1];
	b2 = W5 * col[8 * 1];
	b3 = W7 * col[8 * 1];
    } else {
	b0 = 0;
	b1 = 0;
	b2 = 0;
	b3 = 0;
    }

    if (SKIP_ZERO_TEST || col[8 * 3]) {
	b0 += W3 * col[8 * 3];
	b1 -= W7 * col[8 * 3];
	b2 -= W1 * col[8 * 3];
	b3 -= W5 * col[8 * 3];
    }

    if (SKIP_ZERO_TEST || col[8 * 5]) {
	b0 += W5 * col[8 * 5];
	b1 -= W1 * col[8 * 5];
	b2 += W7 * col[8 * 5];
	b3 += W3 * col[8 * 5];
    }

    if (SKIP_ZERO_TEST || col[8 * 7]) {
	b0 += W7 * col[8 * 7];
	b1 -= W5 * col[8 * 7];
	b2 += W3 * col[8 * 7];
	b3 -= W1 * col[8 * 7];
    }

    col[8 * 0] = (a0 + b0) >> COL_SHIFT;
    col[8 * 7] = (a0 - b0) >> COL_SHIFT;
    col[8 * 1] = (a1 + b1) >> COL_SHIFT;
    col[8 * 6] = (a1 - b1) >> COL_SHIFT;
    col[8 * 2] = (a2 + b2) >> COL_SHIFT;
    col[8 * 5] = (a2 - b2) >> COL_SHIFT;
    col[8 * 3] = (a3 + b3) >> COL_SHIFT;
    col[8 * 4] = (a3 - b3) >> COL_SHIFT;
}

void mpeg2_idct_copy_alpha (int16_t *restrict block, uint8_t *restrict dest,
			    const int stride)
{
    int i;

    for (i = 0; i < 8; i++)
	idct_row (block + 8 * i);

    for (i = 0; i < 8; i++)
	idct_col (block + i);

    if (HAVE_MVI()) {
	int i = 8;
	uint64_t clampmask = zap(-1, 0xaa); /* 0x00ff00ff00ff00ff */

	ASM_ACCEPT_MVI;
	do {
	    uint64_t shorts0, shorts1;

	    shorts0 = ldq(block);
	    shorts0 = maxsw4(shorts0, 0);
	    shorts0 = minsw4(shorts0, clampmask);
	    stl(pkwb(shorts0), dest);

	    shorts1 = ldq(block + 4);
	    shorts1 = maxsw4(shorts1, 0);
	    shorts1 = minsw4(shorts1, clampmask);
	    stl(pkwb(shorts1), dest + 4);

	    stq(0, block);
	    stq(0, block + 4);

	    dest += stride;
	    block += 8;
	} while (--i);
	
    } else {
	i = 8;
	do {
	    dest[0] = CLIP (block[0]);
	    dest[1] = CLIP (block[1]);
	    dest[2] = CLIP (block[2]);
	    dest[3] = CLIP (block[3]);
	    dest[4] = CLIP (block[4]);
	    dest[5] = CLIP (block[5]);
	    dest[6] = CLIP (block[6]);
	    dest[7] = CLIP (block[7]);

	    stq(0, block);
	    stq(0, block + 4);

	    dest += stride;
	    block += 8;
	} while (--i);
    }
}

void mpeg2_idct_add_alpha (const int last, int16_t *restrict block,
			   uint8_t *restrict dest, const int stride)
{
    int i;

    if (last != 129 || (block[0] & 7) == 4) {
	for (i = 0; i < 8; i++)
	    idct_row (block + 8 * i);
	for (i = 0; i < 8; i++)
	    idct_col (block + i);

	if (HAVE_MVI()) {
	    uint64_t clampmask = zap(-1, 0xaa); /* 0x00ff00ff00ff00ff */
	    uint64_t signmask  = zap(-1, 0x33);
	    signmask ^= signmask >> 1;	/* 0x8000800080008000 */

	    ASM_ACCEPT_MVI;
	    do {
		uint64_t shorts0, pix0, signs0;
		uint64_t shorts1, pix1, signs1;

		shorts0 = ldq(block);
		shorts1 = ldq(block + 4);

		pix0	= unpkbw(ldl(dest));
		/* Signed subword add (MMX paddw).  */
		signs0	= shorts0 & signmask;
		shorts0 &= ~signmask;
		shorts0 += pix0;
		shorts0 ^= signs0;
		/* Clamp. */
		shorts0 = maxsw4(shorts0, 0);
		shorts0 = minsw4(shorts0, clampmask);	

		/* Next 4.  */
		pix1	= unpkbw(ldl(dest + 4));
		signs1	= shorts1 & signmask;
		shorts1 &= ~signmask;
		shorts1 += pix1;
		shorts1 ^= signs1;
		shorts1 = maxsw4(shorts1, 0);
		shorts1 = minsw4(shorts1, clampmask);

		stl(pkwb(shorts0), dest);
		stl(pkwb(shorts1), dest + 4);
		stq(0, block);
		stq(0, block + 4);

		dest += stride;
		block += 8;
	    } while (--i);
	} else {
	    do {
		dest[0] = CLIP (block[0] + dest[0]);
		dest[1] = CLIP (block[1] + dest[1]);
		dest[2] = CLIP (block[2] + dest[2]);
		dest[3] = CLIP (block[3] + dest[3]);
		dest[4] = CLIP (block[4] + dest[4]);
		dest[5] = CLIP (block[5] + dest[5]);
		dest[6] = CLIP (block[6] + dest[6]);
		dest[7] = CLIP (block[7] + dest[7]);

		stq(0, block);
		stq(0, block + 4);

		dest += stride;
		block += 8;
	    } while (--i);
	}
    } else {
	int DC = (block[0] + 4) >> 3;

	block[0] = block[63] = 0;

	if (DC == 0)
	    return;

	if (HAVE_MVI()) {
	    uint64_t p0, p1, p2, p3, p4, p5, p6, p7;
	    uint64_t DCs = BYTE_VEC(abs(DC));

	    p0 = ldq(dest + 0 * stride);
	    p1 = ldq(dest + 1 * stride);
	    p2 = ldq(dest + 2 * stride);
	    p3 = ldq(dest + 3 * stride);
	    p4 = ldq(dest + 4 * stride);
	    p5 = ldq(dest + 5 * stride);
	    p6 = ldq(dest + 6 * stride);
	    p7 = ldq(dest + 7 * stride);

	    if (DC > 0) {
		p0 += minub8(DCs, ~p0);
		p1 += minub8(DCs, ~p1);
		p2 += minub8(DCs, ~p2);
		p3 += minub8(DCs, ~p3);
		p4 += minub8(DCs, ~p4);
		p5 += minub8(DCs, ~p5);
		p6 += minub8(DCs, ~p6);
		p7 += minub8(DCs, ~p7);
	    } else {
		p0 -= minub8(DCs, p0);
		p1 -= minub8(DCs, p1);
		p2 -= minub8(DCs, p2);
		p3 -= minub8(DCs, p3);
		p4 -= minub8(DCs, p4);
		p5 -= minub8(DCs, p5);
		p6 -= minub8(DCs, p6);
		p7 -= minub8(DCs, p7);
	    }

	    stq(p0, dest + 0 * stride);
	    stq(p1, dest + 1 * stride);
	    stq(p2, dest + 2 * stride);
	    stq(p3, dest + 3 * stride);
	    stq(p4, dest + 4 * stride);
	    stq(p5, dest + 5 * stride);
	    stq(p6, dest + 6 * stride);
	    stq(p7, dest + 7 * stride);
	} else {
	    i = 8;
	    do {
		dest[0] = CLIP (DC + dest[0]);
		dest[1] = CLIP (DC + dest[1]);
		dest[2] = CLIP (DC + dest[2]);
		dest[3] = CLIP (DC + dest[3]);
		dest[4] = CLIP (DC + dest[4]);
		dest[5] = CLIP (DC + dest[5]);
		dest[6] = CLIP (DC + dest[6]);
		dest[7] = CLIP (DC + dest[7]);
		dest += stride;
	    } while (--i);
	}
    }
}

void mpeg2_idct_alpha_init(void)
{
    int i;

    for (i = -384; i < 640; i++)
	clip_lut[i + 384] = (i < 0) ? 0 : ((i > 255) ? 255 : i);
}

#endif /* ARCH_ALPHA */
