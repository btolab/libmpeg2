/*
 * mpeg2dec.c
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#ifdef HAVE_IO_H
#include <fcntl.h>
#include <io.h>
#endif
#if defined(HAVE_SYS_TIME_H) && defined(HAVE_GETTIMEOFDAY)
#include <sys/time.h>
#include <signal.h>
#endif
#include <inttypes.h>

#include "video_out.h"
#include "mpeg2.h"
#include "mm_accel.h"

#define BUFFER_SIZE 4096
static uint8_t buffer[BUFFER_SIZE];
static FILE * in_file;
static int demux_track = 0;
static int demux_pid = 0;
static int disable_accel = 0;
static mpeg2dec_t * mpeg2dec;
static vo_open_t * output_open = NULL;
static vo_instance_t * output;

#if defined(HAVE_SYS_TIME_H) && defined(HAVE_GETTIMEOFDAY) 

static void print_fps (int final);

static RETSIGTYPE signal_handler (int sig)
{
    print_fps (1);
    signal (sig, SIG_DFL);
    raise (sig);
}

static void print_fps (int final)
{
    static uint32_t frame_counter = 0;
    static struct timeval tv_beg, tv_start;
    static int total_elapsed;
    static int last_count = 0;
    struct timeval tv_end;
    float fps, tfps;
    int frames, elapsed;

    gettimeofday (&tv_end, NULL);

    if (!frame_counter) {
	tv_start = tv_beg = tv_end;
	signal (SIGINT, signal_handler);
    }

    elapsed = (tv_end.tv_sec - tv_beg.tv_sec) * 100 +
	(tv_end.tv_usec - tv_beg.tv_usec) / 10000;
    total_elapsed = (tv_end.tv_sec - tv_start.tv_sec) * 100 +
	(tv_end.tv_usec - tv_start.tv_usec) / 10000;

    if (final) {
	if (total_elapsed)
	    tfps = frame_counter * 100.0 / total_elapsed;
	else
	    tfps = 0;

	fprintf (stderr,"\n%d frames decoded in %.2f seconds (%.2f fps)\n",
		 frame_counter, total_elapsed / 100.0, tfps);

	return;
    }

    frame_counter++;

    if (elapsed < 50)	/* only display every 0.50 seconds */
	return;

    tv_beg = tv_end;
    frames = frame_counter - last_count;

    fps = frames * 100.0 / elapsed;
    tfps = frame_counter * 100.0 / total_elapsed;

    fprintf (stderr, "%d frames in %.2f sec (%.2f fps), "
	     "%d last %.2f sec (%.2f fps)\033[K\r", frame_counter,
	     total_elapsed / 100.0, tfps, frames, elapsed / 100.0, fps);

    last_count = frame_counter;
}

#else /* !HAVE_SYS_TIME_H || !HAVE_GETTIMEOFDAY */

static void print_fps (int final)
{
}

#endif

static void print_usage (char ** argv)
{
    int i;
    vo_driver_t * drivers;

    fprintf (stderr,
	     "usage: %s [-o <mode>] [-s [<track>]] [-t <pid>] [-c] <file>\n"
	     "\t-s\tuse program stream demultiplexer, "
	     "track 0-15 or 0xe0-0xef\n"
	     "\t-t\tuse transport stream demultiplexer, pid 0x10-0x1ffe\n"
	     "\t-c\tuse c implementation, disables all accelerations\n"
	     "\t-o\tvideo output mode\n", argv[0]);

    drivers = vo_drivers ();
    for (i = 0; drivers[i].name; i++)
	fprintf (stderr, "\t\t\t%s\n", drivers[i].name);

    exit (1);
}

static void handle_args (int argc, char ** argv)
{
    int c;
    vo_driver_t * drivers;
    int i;
    char * s;

    drivers = vo_drivers ();
    while ((c = getopt (argc, argv, "s::t:co:")) != -1)
	switch (c) {
	case 'o':
	    for (i = 0; drivers[i].name != NULL; i++)
		if (strcmp (drivers[i].name, optarg) == 0)
		    output_open = drivers[i].open;
	    if (output_open == NULL) {
		fprintf (stderr, "Invalid video driver: %s\n", optarg);
		print_usage (argv);
	    }
	    break;

	case 's':
	    demux_track = 0xe0;
	    if (optarg != NULL) {
		demux_track = strtol (optarg, &s, 16);
		if (demux_track < 0xe0)
		    demux_track += 0xe0;
		if ((demux_track < 0xe0) || (demux_track > 0xef) || (*s)) {
		    fprintf (stderr, "Invalid track number: %s\n", optarg);
		    print_usage (argv);
		}
	    }
	    break;

	case 't':
	    demux_pid = strtol (optarg, &s, 16);
	    if ((demux_pid < 0x10) || (demux_pid > 0x1ffe) || (*s)) {
		fprintf (stderr, "Invalid pid: %s\n", optarg);
		print_usage (argv);
	    }
	    break;

	case 'c':
	    disable_accel = 1;
	    break;

	default:
	    print_usage (argv);
	}

    /* -o not specified, use a default driver */
    if (output_open == NULL)
	output_open = drivers[0].open;

    if (optind < argc) {
	in_file = fopen (argv[optind], "rb");
	if (!in_file) {
	    fprintf (stderr, "%s - couldnt open file %s\n", strerror (errno),
		     argv[optind]);
	    exit (1);
	}
    } else
	in_file = stdin;
}

void set_buf (uint8_t * buf[3], void * id)
{
    mpeg2_set_buf (mpeg2dec, buf, id);
}

static void decode_mpeg2 (uint8_t * current, uint8_t * end)
{
    mpeg2_info_t * info;
    picture_t * picture;
    int state, flags;

    info = mpeg2_info (mpeg2dec);
    while (1) {
	state = mpeg2_buffer (mpeg2dec, &current, end);
	switch (state) {
	case -1:
	    return;
	case STATE_SEQUENCE:
	    if (vo_setup (output, info->sequence->width,
			  info->sequence->height)) {
		fprintf (stderr, "display setup failed\n");
		exit (1);
	    }
	    vo_set_frame (output, VO_PREDICTION_FLAG | VO_BOTH_FIELDS);
	    vo_set_frame (output, VO_PREDICTION_FLAG | VO_BOTH_FIELDS);
	    break;
	case STATE_PICTURE:
	    picture = info->current_picture;
	    flags = ((picture->nb_fields > 1) ? VO_BOTH_FIELDS :
		     ((picture->flags & PIC_FLAG_TOP_FIELD_FIRST) ?
		      VO_TOP_FIELD : VO_BOTTOM_FIELD));
	    if ((picture->flags & PIC_MASK_CODING_TYPE) !=
		PIC_FLAG_CODING_TYPE_B)
		flags |= VO_PREDICTION_FLAG;
	    vo_set_frame (output, flags);
	    break;
	case STATE_PICTURE_2ND:
	    picture = info->current_picture_2nd;
	    flags = ((picture->flags & PIC_FLAG_TOP_FIELD_FIRST) ?
		     VO_TOP_FIELD : VO_BOTTOM_FIELD);
	    vo_field (info->current_fbuf->id, flags);
	    break;
	case STATE_SLICE:
	case STATE_END:
	    vo_draw (info->display_fbuf->id);
	    print_fps (0);
	    break;
	}
    }
}

#define DEMUX_PAYLOAD_START 1
static int demux (uint8_t * buf, uint8_t * end, int flags)
{
    static int mpeg1_skip_table[16] = {
	0, 0, 4, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    /*
     * the demuxer keeps some state between calls:
     * if "state" = DEMUX_HEADER, then "head_buf" contains the first
     *     "bytes" bytes from some header.
     * if "state" == DEMUX_DATA, then we need to copy "bytes" bytes
     *     of ES data before the next header.
     * if "state" == DEMUX_SKIP, then we need to skip "bytes" bytes
     *     of data before the next header.
     *
     * NEEDBYTES makes sure we have the requested number of bytes for a
     * header. If we dont, it copies what we have into head_buf and returns,
     * so that when we come back with more data we finish decoding this header.
     *
     * DONEBYTES updates "buf" to point after the header we just parsed.
     */

#define DEMUX_HEADER 0
#define DEMUX_DATA 1
#define DEMUX_SKIP 2
    static int state = DEMUX_SKIP;
    static int state_bytes = 0;
    static uint8_t head_buf[264];

    uint8_t * header;
    int bytes;
    int len;

#define NEEDBYTES(x)						\
    do {							\
	int missing;						\
								\
	missing = (x) - bytes;					\
	if (missing > 0) {					\
	    if (header == head_buf) {				\
		if (missing <= end - buf) {			\
		    memcpy (header + bytes, buf, missing);	\
		    buf += missing;				\
		    bytes = (x);				\
		} else {					\
		    memcpy (header + bytes, buf, end - buf);	\
		    state_bytes = bytes + end - buf;		\
		    return 0;					\
		}						\
	    } else {						\
		memcpy (head_buf, header, bytes);		\
		state = DEMUX_HEADER;				\
		state_bytes = bytes;				\
		return 0;					\
	    }							\
	}							\
    } while (0)

#define DONEBYTES(x)		\
    do {			\
	if (header != head_buf)	\
	    buf = header + (x);	\
    } while (0)

    if (flags & DEMUX_PAYLOAD_START)
	goto payload_start;
    switch (state) {
    case DEMUX_HEADER:
	if (state_bytes > 0) {
	    header = head_buf;
	    bytes = state_bytes;
	    goto continue_header;
	}
	break;
    case DEMUX_DATA:
	if (demux_pid || (state_bytes > end - buf)) {
	    decode_mpeg2 (buf, end);
	    state_bytes -= end - buf;
	    return 0;
	}
	decode_mpeg2 (buf, buf + state_bytes);
	buf += state_bytes;
	break;
    case DEMUX_SKIP:
	if (demux_pid || (state_bytes > end - buf)) {
	    state_bytes -= end - buf;
	    return 0;
	}
	buf += state_bytes;
	break;
    }

    while (1) {
	if (demux_pid) {
	    state = DEMUX_SKIP;
	    return 0;
	}
    payload_start:
	header = buf;
	bytes = end - buf;
    continue_header:
	NEEDBYTES (4);
	if (header[0] || header[1] || (header[2] != 1)) {
	    if (demux_pid) {
		state = DEMUX_SKIP;
		return 0;
	    } else if (header != head_buf) {
		buf++;
		goto payload_start;
	    } else {
		header[0] = header[1];
		header[1] = header[2];
		header[2] = header[3];
		bytes = 3;
		goto continue_header;
	    }
	}
	if (demux_pid) {
	    if ((header[3] >= 0xe0) && (header[3] <= 0xef))
		goto pes;
	    fprintf (stderr, "bad stream id %x\n", header[3]);
	    exit (1);
	}
	switch (header[3]) {
	case 0xb9:	/* program end code */
	    /* DONEBYTES (4); */
	    /* break;         */
	    return 1;
	case 0xba:	/* pack header */
	    NEEDBYTES (12);
	    if ((header[4] & 0xc0) == 0x40) {	/* mpeg2 */
		NEEDBYTES (14);
		len = 14 + (header[13] & 7);
		NEEDBYTES (len);
		DONEBYTES (len);
		/* header points to the mpeg2 pack header */
	    } else if ((header[4] & 0xf0) == 0x20) {	/* mpeg1 */
		DONEBYTES (12);
		/* header points to the mpeg1 pack header */
	    } else {
		fprintf (stderr, "weird pack header\n");
		exit (1);
	    }
	    break;
	default:
	    if (header[3] == demux_track) {
	    pes:
		NEEDBYTES (7);
		if ((header[6] & 0xc0) == 0x80) {	/* mpeg2 */
		    NEEDBYTES (9);
		    len = 9 + header[8];
		    NEEDBYTES (len);
		    /* header points to the mpeg2 pes header */
		    if (header[7] & 0x80) {
			uint32_t pts;

			pts = (((buf[9] >> 1) << 30) |
			       (buf[10] << 22) | ((buf[11] >> 1) << 15) |
			       (buf[12] << 7) | (buf[13] >> 1));
			mpeg2_pts (mpeg2dec, pts);
		    }
		} else {	/* mpeg1 */
		    int len_skip;
		    uint8_t * ptsbuf;

		    len = 7;
		    while (header[len - 1] == 0xff) {
			len++;
			NEEDBYTES (len);
			if (len == 23) {
			    fprintf (stderr, "too much stuffing\n");
			    break;
			}
		    }
		    if ((header[len - 1] & 0xc0) == 0x40) {
			len += 2;
			NEEDBYTES (len);
		    }
		    len_skip = len;
		    len += mpeg1_skip_table[header[len - 1] >> 4];
		    NEEDBYTES (len);
		    /* header points to the mpeg1 pes header */
		    ptsbuf = header + len_skip;
		    if (ptsbuf[-1] & 0x20) {
			uint32_t pts;

			pts = (((ptsbuf[-1] >> 1) << 30) |
			       (ptsbuf[0] << 22) | ((ptsbuf[1] >> 1) << 15) |
			       (ptsbuf[2] << 7) | (ptsbuf[3] >> 1));
			mpeg2_pts (mpeg2dec, pts);
		    }
		}
		DONEBYTES (len);
		bytes = 6 + (header[4] << 8) + header[5] - len;
		if (demux_pid || (bytes > end - buf)) {
		    decode_mpeg2 (buf, end);
		    state = DEMUX_DATA;
		    state_bytes = bytes - (end - buf);
		    return 0;
		} else if (bytes > 0) {
		    decode_mpeg2 (buf, buf + bytes);
		    buf += bytes;
		}
	    } else if (header[3] < 0xb9) {
		fprintf (stderr,
			 "looks like a video stream, not system stream\n");
		DONEBYTES (4);
	    } else {
		NEEDBYTES (6);
		DONEBYTES (6);
		bytes = (header[4] << 8) + header[5];
		if (bytes > end - buf) {
		    state = DEMUX_SKIP;
		    state_bytes = bytes - (end - buf);
		    return 0;
		}
		buf += bytes;
	    }
	}
    }
}

static void ps_loop (void)
{
    uint8_t * end;

    do {
	end = buffer + fread (buffer, 1, BUFFER_SIZE, in_file);
	if (demux (buffer, end, 0))
	    break;	/* hit program_end_code */
    } while (end == buffer + BUFFER_SIZE);
}

static void ts_loop (void)
{
#define PACKETS (BUFFER_SIZE / 188)
    uint8_t * buf;
    uint8_t * data;
    uint8_t * end;
    int packets;
    int i;
    int pid;

    do {
	packets = fread (buffer, 188, PACKETS, in_file);
	for (i = 0; i < packets; i++) {
	    buf = buffer + i * 188;
	    end = buf + 188;
	    if (buf[0] != 0x47) {
		fprintf (stderr, "bad sync byte\n");
		exit (1);
	    }
	    pid = ((buf[1] << 8) + buf[2]) & 0x1fff;
	    if (pid != demux_pid)
		continue;
	    data = buf + 4;
	    if (buf[3] & 0x20) {	/* buf contains an adaptation field */
		data = buf + 5 + buf[4];
		if (data > end)
		    continue;
	    }
	    if (buf[3] & 0x10)
		demux (data, end, (buf[1] & 0x40) ? DEMUX_PAYLOAD_START : 0);
	}
    } while (packets == PACKETS);
}

static void es_loop (void)
{
    uint8_t * end;

    do {
	end = buffer + fread (buffer, 1, BUFFER_SIZE, in_file);
	decode_mpeg2 (buffer, end);
    } while (end == buffer + BUFFER_SIZE);
}

int main (int argc, char ** argv)
{
    uint32_t accel;

#ifdef HAVE_IO_H
    setmode (fileno (stdout), O_BINARY);
#endif

    fprintf (stderr, PACKAGE"-"VERSION
	     " - by Michel Lespinasse <walken@zoy.org> and Aaron Holtzman\n");

    handle_args (argc, argv);

    accel = disable_accel ? 0 : (mm_accel () | MM_ACCEL_MLIB);

    vo_accel (accel);
    output = vo_open (output_open);
    if (output == NULL) {
	fprintf (stderr, "Can not open output\n");
	return 1;
    }
    mpeg2dec = mpeg2_init (accel);
    if (mpeg2dec == NULL)
	exit (1);

    if (demux_pid)
	ts_loop ();
    else if (demux_track)
	ps_loop ();
    else
	es_loop ();

    mpeg2_close (mpeg2dec);
    vo_close (output);
    print_fps (1);
    return 0;
}
