/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "common/msg.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/options.h"
#include "video/img_format.h"

#include "video/mp_image.h"
#include "video/mp_image_pool.h"

#include "video/out/vo.h"

#include "options/m_option.h"

struct vf_vectorraster_opts {
    int cfg_width;
    int cfg_height;
};

struct vf_priv_s {
    struct vf_vectorraster_opts *opts;
    struct mp_image_pool *pool;
};

typedef union vector{
    struct{
        uint8_t x;
        uint8_t y;
        uint8_t z;
        uint8_t padding;
    };
    uint32_t pattern;
} vector_t;

static const unsigned long scan_width   = 512;
static const unsigned long scan_height  = 256;

#define BRIGHTNESS2LENGTH(x) ((x+1)*(x+1)*(x+1)) //Increased

static void vf_vectorraster_process(struct mp_filter *vf){
    
    struct vf_priv_s *priv = vf->priv;
    
    
    if (!mp_pin_can_transfer_data(vf->ppins[1], vf->ppins[0]))
        return;
    
    struct mp_frame frame = mp_pin_out_read(vf->ppins[0]);
    
    if (mp_frame_is_signaling(frame)) {
        mp_pin_in_write(vf->ppins[1], frame);
        return;
    }
    if (frame.type != MP_FRAME_VIDEO) {
        MP_ERR(vf, "unsupported frame type\n");
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(vf);
        return;
    }

    struct mp_image *mpi = frame.data;
   
    //struct mp_image *out_image = mp_image_alloc(IMGFMT_RGB0, priv->cfg_width, priv->cfg_height);
    struct mp_image *out_image = mp_image_pool_get(priv->pool, IMGFMT_RGB0, priv->opts->cfg_width, priv->opts->cfg_height);
    if (!out_image){
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(vf);
        return;
    }
    mp_image_make_writeable(out_image);
        
    uint8_t* src = mpi->planes[0];
    int in_stepwidth = mpi->stride[0];
    unsigned int in_height = mp_image_plane_h(mpi, 0);
    vector_t* dst = (vector_t*) out_image->planes[0];
    unsigned long out_stepwidth = out_image->stride[0];  
    
        
    unsigned long max_length = out_image->w * out_image->h;
    unsigned long total_length = 0;
    
    for (unsigned long y = 0; y < scan_height; y++){
        for (unsigned long x = 0; x < scan_width; x++){
            unsigned long sx = (y & 1) ? (scan_width - 1 - x) : x; //Avoid scan back after each line by changing scan direction
            unsigned long brightness = src[in_stepwidth * (in_height - 1 - (y * in_height / scan_height)) + (sx * mpi->w * (mpi->fmt.bpp[0] / 8) / scan_width)];
            if (sx == 0 || (sx == (scan_width-1))) brightness = 0xF0; //Ensure leftmost pixel is drawn, to force the beam to scan the entire line and don't start in the middle
            total_length += BRIGHTNESS2LENGTH(brightness);
        }
    }
    
    if (total_length > 0){
        
        for (unsigned long y = 0; y < scan_height; y++){
            
            for (unsigned long x = 0; x < scan_width; x++){
                unsigned long sx = (y & 1) ? (scan_width - 1 - x) : x; //Avoid scan back after each line by changing scan direction
                unsigned long brightness = src[in_stepwidth * (in_height - 1 - (y * in_height / scan_height)) + (sx * mpi->w * (mpi->fmt.bpp[0] / 8) / scan_width)];
                if (sx == 0 || (sx == (scan_width-1))) brightness = 0xF0; //Ensure leftmost pixel is drawn, to force the beam to scan the entire line and don't start in the middle
                unsigned long length = BRIGHTNESS2LENGTH(brightness) * max_length / total_length;
                
                //Temporal dithering 50%/50%
                vector_t v[2] = {{{sx/2,y,0xFF}}, {{((sx & 1) == 0) || (sx == (scan_width -1)) ? sx/2 : sx/2+1,y,0xFF}}};
                for (long i = 0; i < length; i++){
                    if ((void*)dst > (void*)(out_image->planes[0] + (out_stepwidth * out_image->h))){
                        printf("overflow im\n");
                        break;
                    }
                    *dst = v[(((unsigned long)((uint8_t*)dst-out_image->planes[0]))/(sizeof(void*))) &1];
                    dst++;
                }
            }
        }
    }
        
    // Fill unused pixels
    if ((void*)dst < (void*)(out_image->planes[0] + (out_stepwidth * out_image->h))){
        memset((char*)dst, 0x0, (out_stepwidth * out_image->h) - ((char*)dst - (char*)out_image->planes[0]));
    }
    
    out_image->pts = mpi->pts;
    
    mp_frame_unref(&frame);
    frame = (struct mp_frame){MP_FRAME_VIDEO, out_image};
    mp_pin_in_write(vf->ppins[1], frame);
}


static const struct mp_filter_info vf_vectorraster_filter = {
    .name = "vectorraster",
    .process = vf_vectorraster_process,
    .priv_size = sizeof(struct vf_priv_s),
};

static struct mp_filter *vf_vectorraster_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_vectorraster_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct vf_priv_s *priv = f->priv;
    priv->opts = talloc_steal(priv, options);
    priv->pool = mp_image_pool_new(priv);

    return f;
}


#define OPT_BASE_STRUCT struct vf_vectorraster_opts
static const m_option_t vf_opts_fields[] = {
    OPT_INTRANGE("width",  cfg_width,  0, 0, 4096, OPTDEF_INT(800)),
    OPT_INTRANGE("height", cfg_height, 0, 0, 4096, OPTDEF_INT(600)),
    {0}
};

const struct mp_user_filter_entry vf_vectorraster = {
    .desc = {
        .description   = "raster output on vector display",
        .name          = "vectorraster",
        .priv_size     = sizeof(struct vf_vectorraster_opts),
        .options       = vf_opts_fields,
    },
    .create        = vf_vectorraster_create,
};
