/* 
 *    display_mga_vid.c
 *
 *	Copyright (C) Aaron Holtzman - Aug 1999
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

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "debug.h"
#include "mpeg2_internal.h"
#include "drivers/mga_vid.h"
#include "display.h"


mga_vid_config_t config;
uint_8 *vid_data;

void 
display_frame(uint_8 *src[])
{
	uint_8 *dest,*y,*cr,*cb;
	uint_32 bespitch,h;

	y  = src[0];
	cb = src[1];
	cr = src[2];
	dest = vid_data;
	bespitch = (config.src_width + 31) & ~31;

	for(h=0; h < config.src_height; h++) 
	{
		memcpy(dest, y, config.src_width);
		y += config.src_width;
		dest += bespitch;
	}

	for(h=0; h < config.src_height/2; h++) 
	{
		memcpy(dest, cb, config.src_width/2);
		cb += config.src_width/2;
		dest += bespitch/2;
	}

	for(h=0; h < config.src_height/2; h++) 
	{
		memcpy(dest, cr, config.src_width/2);
		cr += config.src_width/2;
		dest += bespitch/2;
	}
}


void 
display_init(uint_32 width, uint_32 height)
{
	int f;
	uint_32 frame_size;

	f = open("/dev/mga_vid",O_RDWR);

	if(f == -1)
	{
		fprintf(stderr,"Couldn't open driver\n");
		exit(1);
	}

	config.src_width = width;
	config.src_height= height;
	config.dest_width = width;
	config.dest_height= height;
	config.x_org= 10;
	config.y_org= 10;

	if (ioctl(f,MGA_VID_CONFIG,&config))
	{
		perror("Error in config ioctl");
	}
	ioctl(f,MGA_VID_ON,0);

	frame_size = ((width + 31) & ~31) * height + (((width + 31) & ~31) * height) / 2;
	vid_data = (char*)mmap(0,frame_size,PROT_WRITE,MAP_SHARED,f,0);

	//clear the buffer
	memset(vid_data,0x80,frame_size);

	dprintf("(display) mga_vid initialized %p\n",vid_data);
}
