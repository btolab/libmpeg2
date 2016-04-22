/*
 * video_out_sdl.c
 *
 * Copyright (C) 2000-2003 Ryan C. Gordon <icculus@lokigames.com> and
 *                         Dominik Schnitzer <aeneas@linuxvideo.org>
 *
 * SDL info, source, and binaries can be found at http://www.libsdl.org/
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
 * You should have received a copy of the GNU General Public License along
 * with mpeg2dec; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#ifdef LIBVO_SDL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <inttypes.h>

#include "video_out.h"
#include "vo_internal.h"

typedef struct {
    vo_instance_t vo;
    int width;
    int height;
    int chroma_width;
    int chroma_height;
    SDL_Window * window;
    SDL_Texture * texture;
    SDL_Renderer * renderer;
} sdl_instance_t;

static void sdl_draw_frame (vo_instance_t * _instance,
                            uint8_t * const * buf, void * id)
{
    sdl_instance_t * instance = (sdl_instance_t *) _instance;
    SDL_Event event;

    while (SDL_PollEvent (&event))
        if (event.type == SDL_WINDOWEVENT_RESIZED)
            SDL_SetWindowSize(instance->window, event.window.data1, event.window.data2);
    SDL_UpdateYUVTexture(instance->texture, NULL,
        buf[0], instance->width,
        buf[1], instance->chroma_width,
        buf[2], instance->chroma_width);
    SDL_RenderClear(instance->renderer);
    SDL_RenderCopy(instance->renderer, instance->texture, NULL, NULL);
    SDL_RenderPresent(instance->renderer);
}

static void sdl_close (vo_instance_t * _instance)
{
    sdl_instance_t * instance = (sdl_instance_t *) _instance;

    SDL_DestroyTexture (instance->texture);
    SDL_DestroyRenderer (instance->renderer);
    SDL_QuitSubSystem (SDL_INIT_VIDEO);
}

static int sdl_setup (vo_instance_t * _instance, unsigned int width,
                      unsigned int height, unsigned int chroma_width,
                      unsigned int chroma_height, vo_setup_result_t * result)
{
    sdl_instance_t * instance;

    instance = (sdl_instance_t *) _instance;

    instance->width = width;
    instance->height = height;
    instance->chroma_width = chroma_width;
    instance->chroma_height = chroma_height;

    instance->window = SDL_CreateWindow("SDL",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              width, height,
                              SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
    if (! (instance->window)) {
        fprintf (stderr, "sdl could not create window: %s\n", SDL_GetError());
        return 1;
    }

    instance->renderer = SDL_CreateRenderer(instance->window, -1, 0);

    instance->texture = SDL_CreateTexture (instance->renderer,
            SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
            instance->width, instance->height);

    result->convert = NULL;
    return 0;
}

vo_instance_t * vo_sdl_open (void)
{
    sdl_instance_t * instance;

    instance = (sdl_instance_t *) malloc (sizeof (sdl_instance_t));
    if (instance == NULL)
        return NULL;

    instance->vo.setup = sdl_setup;
    instance->vo.setup_fbuf = NULL;
    instance->vo.set_fbuf = NULL;
    instance->vo.start_fbuf = NULL;
    instance->vo.discard = NULL;
    instance->vo.draw = sdl_draw_frame;
    instance->vo.close = sdl_close;

    if (SDL_Init (SDL_INIT_VIDEO)) {
        fprintf (stderr, "sdl video initialization failed.\n");
        return NULL;
    }

    return (vo_instance_t *) instance;
}
#endif
