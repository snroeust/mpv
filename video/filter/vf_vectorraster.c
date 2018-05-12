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
#include "options/options.h"

#include "video/mp_image.h"
#include "vf.h"

#include "video/out/vo.h"

#include "options/m_option.h"

static struct vf_priv_s {
    int cfg_width, cfg_height;
} const vf_priv_dflt = {
    800, 600,
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

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi){
    
    struct vf_priv_s *priv = vf->priv;
    struct mp_image *out_image = mp_image_alloc(IMGFMT_RGB0, priv->cfg_width, priv->cfg_height);
    if (!out_image) return NULL;
    if (!mp_image_make_writeable(out_image)) return NULL;
    
    uint8_t* src = mpi->planes[0];
    unsigned long in_stepwidth = (mp_image_plane_w(mpi, 0) * mpi->fmt.bpp[0] + 7) / 8;    
    vector_t* dst = (vector_t*) out_image->planes[0];
    unsigned long out_stepwidth = (mp_image_plane_w(out_image, 0) * out_image->fmt.bpp[0] + 7) / 8;    

    unsigned long max_length = out_image->w * out_image->h;
    unsigned long total_length = 0;
    
    for (unsigned long y = 0; y < scan_height; y++){
        for (unsigned long x = 0; x < scan_width; x++){
            unsigned long sx = (y & 1) ? (scan_width - 1 - x) : x; //Avoid scan back after each line by changing scan direction
            unsigned long brightness = src[in_stepwidth * (mpi->h - 1 - (y * mpi->h / scan_height)) + (sx * mpi->w / scan_width)];
            if (sx == 0 || (sx == (scan_width-1))) brightness = 0xF0; //Ensure leftmost pixel is drawn, to force the beam to scan the entire line and don't start in the middle
            total_length += BRIGHTNESS2LENGTH(brightness);
        }
    }
    
    if (total_length > 0){
        
        for (unsigned long y = 0; y < scan_height; y++){
            
            for (unsigned long x = 0; x < scan_width; x++){
                unsigned long sx = (y & 1) ? (scan_width - 1 - x) : x; //Avoid scan back after each line by changing scan direction
                unsigned long brightness = src[in_stepwidth * (mpi->h - 1 - (y * mpi->h / scan_height)) + (sx * mpi->w / scan_width)];
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
    
    talloc_free(mpi);
    return out_image;
}

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
    //Grayscale input
    if (fmt == IMGFMT_Y8){
        return vf_next_query_format(vf, IMGFMT_RGB0); //RGB output goes to next filter/output in chain
    } else {
        return 0;
    }
}

static int reconfig(struct vf_instance *vf, struct mp_image_params *in,
                    struct mp_image_params *out)
{
    struct vf_priv_s *priv = vf->priv;
    
    *out = *in;
    out->imgfmt = IMGFMT_RGB0; //Set RGB output format
    out->w = priv->cfg_width;
    out->h = priv->cfg_height;
    out->d_w = out->w;
    out->d_h = out->h;
    
    mp_image_params_guess_csp(out);

    return 0;
}


static int vf_open(vf_instance_t *vf){
    vf->filter       = filter;
    vf->reconfig     = reconfig;
    vf->query_format = query_format;
    return 1;
}


#define OPT_BASE_STRUCT struct vf_priv_s
static const m_option_t vf_opts_fields[] = {
    OPT_INT("width",  cfg_width,  0, .min=1),
    OPT_INT("height", cfg_height, 0, .min=1),
    {0}
};

const vf_info_t vf_info_vectorraster = {
    .description   = "raster output on vector display",
    .name          = "vectorraster",
    .open          = vf_open,
    .priv_size     = sizeof(struct vf_priv_s),
    .priv_defaults = &vf_priv_dflt,
    .options       = vf_opts_fields,
};
