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
#include <highgui.h>

static struct vf_priv_s {
    int cfg_width, cfg_height;
    double cfg_min_length;
} const vf_priv_dflt = {
    800, 600,
    3
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
    struct mp_image *out_image = mp_image_alloc(IMGFMT_RGB0, priv->cfg_width, priv->cfg_height);
    if (!out_image) return NULL;
    if (!mp_image_make_writeable(out_image)) return NULL;
    
    IplImage ocv_in;
    IplImage ocv_out;
    mp_to_ocv_image(&ocv_in, mpi);
    mp_to_ocv_image(&ocv_out, out_image);
    
//     unsigned int ivector = 0;
    unsigned int max_vectors = ocv_out.width * ocv_out.height;
    unsigned int num_vectors = 0;
    vector_t* dst = (vector_t*)ocv_out.imageData;
    
    CvMemStorage *storage = cvCreateMemStorage(0);
    CvSeq* contours;
    cvFindContours(&ocv_in, storage, &contours, sizeof(CvContour), CV_RETR_LIST, CV_CHAIN_APPROX_NONE, cvPoint(0, 0));
    
    if (contours){
        //Count the number of vectors to draw
        CvSeq* c = contours;
        do {
            int length = c->total;
            if (length >= priv->cfg_min_length){
                num_vectors+=length;
            }
        } while ((c = c->h_next));
    }
    
    if ((num_vectors > 0) && (num_vectors <= max_vectors)){
        
       float scale = (float)max_vectors / (float)num_vectors;
       float remain = 0;
       vector_t v;
       v.z = 0xFF; //Beam on
       
        CvSeq* c = contours;
        do {
            int length = c->total;
            if (length >= priv->cfg_min_length){
                for (int i = 0; i < length; i++){
                    CvPoint* point = CV_GET_SEQ_ELEM(CvPoint, c, i);
                    //Can only draw full vectors, accumulate rounding errors and draw one additional vector when > 1
                    int iscale = scale + remain;
                    remain += scale - iscale;
                    
                    v.x = (point->x * 0xFF / ocv_in.width); //Scale to full "color" depth
                    v.y = 0xFF - (point->y * 0xFF / ocv_in.height);
                    for (int j = 0; j < iscale; j++){
                        dst->pattern = v.pattern;
                        dst++;
                    }
                }
            }
        } while ((c = c->h_next));
    }
    // Fill unused pixels
    memset((char*)dst, 0x0,  ocv_out.imageSize - ((char*)dst - ocv_out.imageData));
    
    out_image->pts = mpi->pts;
    
    cvReleaseMemStorage(&storage);
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
    OPT_DOUBLE("min_length", cfg_min_length, 0, .min = 0),
    {0}
};

const vf_info_t vf_info_vector = {
    .description   = "vector output",
    .name          = "vector",
    .open          = vf_open,
    .priv_size     = sizeof(struct vf_priv_s),
    .priv_defaults = &vf_priv_dflt,
    .options       = vf_opts_fields,
};
