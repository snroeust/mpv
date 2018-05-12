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

/*
 * -- Naming --
 * Contour:   List of adjacent points as outputed by cvFindContours
 * Point:     Point in a contour
 * Distance:  Distance between two points (typically end of one contour to begging of the next)
 * Time:      (unscaled) Scanout time of the Point (input image values, 0-255), contour (sum of point times), or travel delay time (between contours)
 * Length:    Intensity of the contour scaled to make all countours fill output bitmap size.
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "config.h"
#include "common/msg.h"
#include "options/options.h"

#include "video/mp_image.h"
#include "vf.h"

#include "video/out/vo.h"

#include "options/m_option.h"

#include <cv.h>


static struct vf_priv_s {
    int cfg_width, cfg_height;
    double cfg_move_scale;
    double cfg_blank_scale;
    double cfg_min_length;
    int cfg_sort;
    int cfg_dither;
} const vf_priv_dflt = {
    800, 600,
    0,
    0,
    3,
    1
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

//static unsigned long jump_to(vector_t** path, IplImage* ocv_in, unsigned long total_time , struct vf_priv_s* config, CvPoint* location);
static unsigned long calculate_move_time(CvSeq* contour, CvPoint* current_point, double move_speed);
static unsigned long calculate_contour_time(IplImage* image, CvSeq* contour, CvPoint* current_point, double move_speed);
static vector_t* add_point(struct vf_priv_s* priv, vector_t* p, IplImage* image , unsigned long length, CvPoint* point, unsigned int z);

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

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi_in)
{
    struct vf_priv_s* priv = vf->priv;
    struct mp_image* mpi_out = mp_image_alloc(IMGFMT_RGB0, priv->cfg_width, priv->cfg_height);
    if (!mpi_out) return NULL;
    if (!mp_image_make_writeable(mpi_out)) return NULL;
    struct mp_image* mpi_work = mp_image_new_copy(mpi_in); //cvFindContours modifies the input, take a copy because we don't own the input image.
    
    IplImage ocv_in;
    IplImage ocv_work;
    IplImage ocv_out;
    mp_to_ocv_image(&ocv_in,   mpi_in);
    mp_to_ocv_image(&ocv_work, mpi_work);
    mp_to_ocv_image(&ocv_out,  mpi_out);
    
    unsigned int max_time = ocv_out.width * ocv_out.height;
    vector_t* dst = (vector_t*)ocv_out.imageData;
    
    CvMemStorage *storage = cvCreateMemStorage(0);
    CvSeq* contours;
    cvFindContours(&ocv_work, storage, &contours, sizeof(CvContour), CV_RETR_LIST, CV_CHAIN_APPROX_NONE, cvPoint(0, 0));
    
//     if(priv->cfg_sort && contours){
//         CvMemStorage *storage_sort = cvCreateMemStorage(0);
//         CvSeq* contours_sort = cvCreateSeq(1117343756, 128, sizeof(CvSeq*), storage_sort);
//         CvSeq* c = 0;
//         CvPoint* last_point = 0;
//         
//         do {
//             CvSeq* min_move_seq = 0;
//             unsigned int min_move_time = 0xFFFFFFFF;
//         
//             CvSeq* d = contours;
//             
//             do {
//                 if (d != c){
//                     int move_time = calculate_move_time(d, last_point, 1);
//                     if (move_time < min_move_time) min_move_seq = d;
//                 }
//             } while ((d = d->h_next));
//             
//             if (min_move_seq){
//                 cvSeqPush(contours_sort, min_move_seq);
//                 last_point = CV_GET_SEQ_ELEM(CvPoint, c, min_move_seq->total - 1);
//             }
//             
//         } while ((c = d->h_next));
//         
//         cvReleaseMemStorage(&storage);
//         storage = storage_sort;
//         contours = contours_sort;
//     }
    
    unsigned long total_time = 0;
    
    CvPoint* last_point = 0;

    if (contours){
        //Count the number of vectors to draw
        CvSeq* c = contours;
        do {
            int num_points = c->total;
            if (num_points >= priv->cfg_min_length){
                total_time += calculate_contour_time(&ocv_in,c, last_point, priv->cfg_move_scale);
                last_point =  CV_GET_SEQ_ELEM(CvPoint, c, -1);
            }
        } while ((c = c->h_next));
    }

    if ((total_time > 0)){
        float scale = (float)max_time / (float)total_time;
        float remain = 0;
        
        CvSeq* c = contours;
        do {
            int num_points = c->total;           
            
            if (num_points >= priv->cfg_min_length){
                if (priv->cfg_move_scale){
                    //Beam move/fill
                    CvPoint* first_point = CV_GET_SEQ_ELEM(CvPoint, c, 0);
                    unsigned long move_points = calculate_move_time(c, last_point, priv->cfg_move_scale) * scale;
                    unsigned long on_points   = move_points * priv->cfg_blank_scale;
                    unsigned long off_points  = move_points - on_points;
                    dst = add_point(priv, dst, &ocv_in, off_points, first_point, 0x00);
                    dst = add_point(priv, dst, &ocv_in, on_points,  first_point, 0xFF);
                }
                
                for (int i = 0; i < num_points; i++){
                    CvPoint* point = CV_GET_SEQ_ELEM(CvPoint, c, i);
                    uint8_t brightness = (uint8_t)ocv_in.imageData[point->x + point->y * ocv_in.widthStep];
                    //Can only draw full vectors, accumulate rounding errors and draw one additional vector when > 1
                    float pscale = scale * brightness;
                    int iscale = pscale + remain;
                    remain += pscale - iscale;

                    dst = add_point(priv, dst, &ocv_in, iscale,  point, 0xFF);
                }
                last_point = CV_GET_SEQ_ELEM(CvPoint, c, -1);
            }
        } while ((c = c->h_next));
    }
    // Fill unused pixels
//     printf("Fill: %li\n", ocv_out.imageSize - ((char*)dst - ocv_out.imageData));
    memset((char*)dst, 0x0,  ocv_out.imageSize - ((char*)dst - ocv_out.imageData));
    
    mpi_out->pts = mpi_in->pts;
    
    cvReleaseMemStorage(&storage);
    talloc_free(mpi_in);
    talloc_free(mpi_work);
    return mpi_out;
}

static vector_t* add_point(struct vf_priv_s* priv, vector_t* dst, IplImage* src_image , unsigned long length, CvPoint* point, unsigned int z){
    if (priv->cfg_dither){
        const unsigned int width = 512;
        const unsigned int height = 512;
        
        z  = ~z;
        
        unsigned int x;
        unsigned int y;
        
        if (point){
            x = (point->x * width / src_image->width); //Scale to full "color" depth
            y = (point->y * height / src_image->height);
        } else {
            x = 0;
            y = 0;
        }
        
        for (unsigned long j = 0; j < length; j++){
            vector_t v = {.x = x/2, .y = y/2, .z = z};
            
            if ((x & 1) && (j & 1) && (x < 512)) v.x++;
            if ((y & 1) && (j & 1) && (y < 512)) v.y++;
            v.y = 256 - v.y;
            
            dst->pattern = v.pattern;
            dst++;
        }
    } else {
        vector_t v;
        v.z = z;
        if (point){
            v.x = (point->x * 256 / src_image->width); //Scale to full "color" depth
            v.y = 255 - (point->y * 256 / src_image->height);
        } else {
            v.x = 0;
            v.y = 255;
        }
        
        for (unsigned long j = 0; j < length; j++){
            dst->pattern = v.pattern;
            dst++;
        }
    }
    return dst;
}

/**
 * Calculate the (unscaled) scanout time to move the beam to the begin of the given countour from the current starting point.
 */
static unsigned long calculate_move_time(CvSeq* contour, CvPoint* current_point, double move_speed){
    if (move_speed){
        double move_distance;
        
        //Beam move time
        CvPoint* first_point = CV_GET_SEQ_ELEM(CvPoint, contour, 0);
        
        if (current_point){ //Move from current beam point
            move_distance = sqrt((current_point->x - first_point->x)*(current_point->x - first_point->x) + (current_point->y - first_point->y)*(current_point->y - first_point->y));
        } else { //Move from (0,0) (h/vblank)
            move_distance = sqrt((first_point->x)*(first_point->x) + (first_point->y)*(first_point->y));
        }
        return move_distance * move_speed;
    } else {
        return 0;
    }
}

/**
 * Calculate the (unscaled) scanout time to draw the countour from current starting point.
 */
static unsigned long calculate_contour_time(IplImage* src_image, CvSeq* contour, CvPoint* current_point, double move_speed){
    unsigned long contour_time = calculate_move_time(contour, current_point, move_speed);
    
    //Count Contour points
    for (int i = 0; i < contour->total; i++){
        CvPoint* point = CV_GET_SEQ_ELEM(CvPoint, contour, i);
        //Draw length (^= draw time) proportional to brightness
        contour_time += (uint8_t)src_image->imageData[point->x + point->y * src_image->widthStep];
    }
    return contour_time;
}



//-------------------------------- MPV Functions -------------------------------

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
    OPT_INT(   "width",      cfg_width,       0, .min=1),
    OPT_INT(   "height",     cfg_height,      0, .min=1),
    OPT_DOUBLE("move",       cfg_move_scale,  0, .min=0, .max=1),
    OPT_DOUBLE("blank",      cfg_blank_scale, 0, .min=0, .max=1),
    OPT_DOUBLE("min_length", cfg_min_length,  0, .min = 0),
    OPT_INT(   "dither",     cfg_dither,      0, .min = 0, .max=1, OPTDEF_INT(1)),
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
