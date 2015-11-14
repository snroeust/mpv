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

#include <cv.h>

static struct vf_priv_s {
    double cfg_threshold1, cfg_threshold2;
    int cfg_aperture;
} const vf_priv_dflt = {
    128.0, 130.0,
    3
};

static void mp_to_ocv_image(IplImage* out, struct mp_image* in)
{
    assert(in->num_planes == 1);
    out->nSize = sizeof(IplImage);
    out->ID = 0;
    out->roi = 0;
    out->maskROI = 0;
    out->origin = 0; //top-left origin
    out->dataOrder = 0; //Interleaved pixel data: R,G,B,R,G,B,...
    out->imageId = 0;
    out->tileInfo = 0;
    out->align = 4;
    
    out->depth = IPL_DEPTH_8U; //8 Bits per color channel, unsigned
    out->height = in->h;
    out->width = in->w;
    out->imageData = in->planes[0];
    out->imageDataOrigin = in->planes[0];
    out->nChannels = in->fmt.bpp[0] / 8; //Bytes per pixel
    out->widthStep = (mp_image_plane_w(in, 0) * in->fmt.bpp[0] + 7) / 8;
    out->imageSize = out->widthStep * out->height;
}

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi)
{
    struct vf_priv_s *priv = vf->priv;
    struct mp_image *out_image = mp_image_alloc(IMGFMT_Y8, mpi->w, mpi->h);
    if (!out_image) return NULL;
    if (!mp_image_make_writeable(out_image)) return NULL;
    
    IplImage ocv_in;
    IplImage ocv_edges;
    mp_to_ocv_image(&ocv_in, mpi);
    mp_to_ocv_image(&ocv_edges, out_image);
    cvCanny(&ocv_in, &ocv_edges, priv->cfg_threshold1, priv->cfg_threshold2, priv->cfg_aperture);
    
    out_image->pts = mpi->pts;
    
    talloc_free(mpi);
    return out_image;
}

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
    //RGB input
    if (fmt == IMGFMT_RGB24){
        return vf_next_query_format(vf, IMGFMT_Y8); //Grayscale output goes to next filter/output in chain
    } else {
        return 0;
    }
}

static int reconfig(struct vf_instance *vf, struct mp_image_params *in,
                    struct mp_image_params *out)
{
    *out = *in;
    out->imgfmt = IMGFMT_Y8; //Set Grayscale output format
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
    OPT_DOUBLE("t1", cfg_threshold1, 0),
    OPT_DOUBLE("t2", cfg_threshold2, 0),
    OPT_INT("aperture", cfg_aperture, 0, .min = 3),
    {0}
};

const vf_info_t vf_info_canny = {
    .description   = "canny edge detection",
    .name          = "canny",
    .open          = vf_open,
    .priv_size     = sizeof(struct vf_priv_s),
    .priv_defaults = &vf_priv_dflt,
    .options       = vf_opts_fields,
};
