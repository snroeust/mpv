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
#include <math.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h> 
#include <unistd.h>
#include <netinet/in.h>

#include <pthread.h>

#include <libswscale/swscale.h>

#include "config.h"
#include "misc/bstr.h"
#include "osdep/io.h"
#include "common/common.h"
#include "common/msg.h"
#include "video/out/vo.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/fmt-conversion.h"
#include "video/sws_utils.h"
#include "sub/osd.h"
#include "options/m_option.h"

#define MAX_RENDER_THREADS 1024

struct priv;

struct write_thread{
    struct priv* vo;
    
    pthread_t pthread;
    int id;
    int field_y;
    int field_step;
    
    char tx_buffer[40960];
    int socket;
    pthread_mutex_t frame_mutex;
};

struct priv {
    char *hostname;
    int port;
    
    int32_t cfg_colorkey;
    int cfg_grayscale_optimize;
    int cfg_full_frames; //Always draw full frames, drop new ones until complete (default, use for video)
    int cfg_full_redraw; //Always write pixels even if not changed since last frame
    
    int offset_x;
    int offset_y;
    
    mp_image_t* current;
    mp_image_t* last;
    
    int frame_drawn;
    int flip; //Requests threads to stop writing the current frame
    int quit; //Requests threads to exit
    
    int num_threads;
    struct write_thread* threads[MAX_RENDER_THREADS];
};

typedef struct point{
    int x;
    int y;
} point_t;


static struct write_thread* draw_thread_create(struct priv* vo, int id, int field_y, int field_step);
static void* draw_thread(void* arg);
static int draw_thread_connect(struct write_thread* thread);
static int draw_thread_draw_frame(struct write_thread* thread);
static int write_thread_write(struct write_thread* thread, char* buffer, int len);



static inline size_t itoa10(char* s, unsigned int i){
    if (i == 0) {*s = '0'; return 1;}
    int len = 0;
    
    unsigned int tmp = i;
    while (tmp){
         tmp /= 10;
         len++;
    }
    
    s+=len-1;
    
    while (i){
        unsigned int d = i / 10;
        unsigned int r = i - (d * 10);
        
        *s = '0' + r;
        i = d;
        
        s--;
    }
    return len;
}

static void draw_image(struct vo *vo, mp_image_t *new){
    struct priv* p = (struct priv*)vo->priv;
    
    p->flip = 1; //Tell threads not to start rendering the current frame again.
    
//     fprintf(stderr, "Draw: Flip...\n");
    for (int i = 0; i < p->num_threads; i++){ //Wait for all threads to finish rendering the current frame
        pthread_mutex_lock(&p->threads[i]->frame_mutex);
    }
    
    //Flip frame buffers
    if (p->frame_drawn){ //update last frame only if the current frame was actually drawn, otherwise we mess up the reference for drawing only changed pixels.
        if (p->last) talloc_free(p->last);
        p->last = p->current;
    } else {
        talloc_free(p->current);
    }
    p->current = new;
    p->frame_drawn = 0;

//     fprintf(stderr, "Draw: Fliped\n");
    
    //Restart render threads
    p->flip = 0;
    for (int i = 0; i < p->num_threads; i++){
        pthread_mutex_unlock(&p->threads[i]->frame_mutex);
    }
    
//     fprintf(stderr, "Draw: Resumed\n");
}


static void flip_page(struct vo *vo){
    
}

static struct write_thread* draw_thread_create(struct priv* vo, int id, int field_y, int field_step){
    struct write_thread* thread = malloc(sizeof(struct write_thread));
    thread->id = id;
    thread->field_y = field_y;
    thread->field_step = field_step;
    thread->socket = -1;
    thread->vo = vo;
    if (pthread_create(&thread->pthread, 0, draw_thread, thread) == 0){
        fprintf(stderr, "Thread %i: Created\n", id);
        return thread; //Ok
    } else { //Error
        fprintf(stderr, "Thread %i: Faild to create\n", id);
        free(thread);
        return 0;
    }
}

static void* draw_thread(void* arg){
    struct write_thread* thread = (struct write_thread*)arg;
    pthread_mutex_init(&thread->frame_mutex, 0);

    fprintf(stderr, "Thread %i: Running...\n",thread->id);
    
//    double pts = -100;
    
    while (thread->vo->quit == 0) {
        if ((thread->socket < 0) && (draw_thread_connect(thread) == 0)) sleep(1); //Connect until succesful or exit
        if (thread->socket < 0) continue; //Not connected
        if (thread->vo->current == 0) {usleep(100); continue;} //No frame to draw
        if (thread->vo->flip) {usleep(10); continue;} //Wait for new frame from vo thread
//         if ((thread->vo->cfg_full_redraw == 0) && (thread->vo->current->pts == pts)) {usleep(10); continue;}
        
//        pts = thread->vo->current->pts;
        
        pthread_mutex_lock(&thread->frame_mutex);
        if (draw_thread_draw_frame(thread) == 0){
            fprintf(stderr, "Thread %i: Write failed\n",thread->id);
            close(thread->socket);
            thread->socket = -1;
        } else {
            thread->vo->frame_drawn = 1;
        }
        pthread_mutex_unlock(&thread->frame_mutex);
    }
    
    pthread_mutex_destroy(&thread->frame_mutex);
    if (thread->socket >= 0 )close(thread->socket);
    thread->socket = -1;
    return 0;
}

static int draw_thread_connect(struct write_thread* thread){
    if (thread->socket >= 0) close(thread->socket);
    thread->socket = -1;
    
    struct sockaddr_in dest_addr;
    
    int fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, IPPROTO_TCP);
    fprintf(stderr, "Thread %i: Opened socket %i\n", thread->id, fd);
    if (fd < 0) return 0;
    
    memset((char *) &dest_addr, 0, sizeof(struct sockaddr_in)); 
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(thread->vo->port);
    if (inet_aton(thread->vo->hostname, &dest_addr.sin_addr)==0) return -1;
    
    int ret = EINPROGRESS;
    
    while (ret == EINPROGRESS){
        ret = 0;
        if (connect(fd, &dest_addr, sizeof(struct sockaddr_in)) < 0) ret = errno;
    }
    if (ret < 0){
        fprintf(stderr, "Thread %i:", thread->id);
        perror("Connect failed");
        return 0;
    }
    fprintf(stderr, "Thread %i: Connected\n", thread->id);
    
    thread->socket = fd;
    return 1;
}

static int draw_thread_draw_frame(struct write_thread* thread){
    struct priv *p = thread->vo;
    
    
    int line_step = p->current->stride[0];
    uint8_t* img_data = p->current->planes[0];
    
    uint8_t* last_img_data = p->last ? p->last->planes[0] : 0;
    
//     fprintf(stderr, "Thread %i, Last %p\n", thread->id, p->last);
    
    char* d = thread->tx_buffer;
    size_t len = 0;
    
    for (int y = thread->field_y; y < p->current->h; y+=thread->field_step){
        if ((thread->vo->cfg_full_frames == 0) && thread->vo->flip) return 1;
        for (int x = 0; x < p->current->w; x++){
            uint8_t* px = &img_data[(y * line_step) + (x * 3)];
            uint8_t* last_px = last_img_data ? &last_img_data[(y * line_step) + (x * 3)] : 0;
            if (p->cfg_full_redraw || ((last_px == 0) || (p->current->stride[0] != p->last->stride[0]) || (p->current->h != p->last->h)
                || (abs((int)px[0] - (int)last_px[0]) + abs((int)px[1] - (int)last_px[1]) + abs((int)px[2] - (int)last_px[2]) ) > 4)
            /*|| (rand() < (RAND_MAX/10))*/){
                
                size_t l= 0;
                //if (x == 0 && y == 0) printf("%i %i %i %i\n", (p->cfg_colorkey & 0xFF) , (int)px[0], (p->cfg_colorkey & 0xFF) - (int)px[0], abs( (p->cfg_colorkey & 0xFF) - (int)px[0]));
                if ((p->cfg_colorkey < 0) || (abs( (p->cfg_colorkey & 0xFF) - (int)px[0]) + abs( ((p->cfg_colorkey >> 8) & 0xFF) - (int)px[1]) + abs( ((p->cfg_colorkey >> 16) & 0xFF) - (int)px[2]) ) > 3){
                    point_t t = {p->offset_x + x, p->offset_y + y};
                    
                    if (p->cfg_grayscale_optimize && (px[0] == px[1]) && (px[1] == px[2])){ //Grayscale optimize
                        l = sprintf(d, "PX %i %i %02x\n", t.x, t.y, px[0]);
                    } else {
                        l = sprintf(d, "PX %i %i %02x%02x%02x\n", p->offset_x + x, p->offset_y + y, px[0], px[1], px[2]);
                    }
                    
                } else {
//                    l = sprintf(d, "PX %i %i 000000\n", p->offset_x + x, p->offset_y + y);
                }
                d+=l;
                len +=l;
                if (len > 4000){
//                     fprintf(stderr, "Thread %i: Flush...\n", thread->id);
                    int ok = (write_thread_write(thread, thread->tx_buffer, len) >= 0);
//                     fprintf(stderr, "Thread %i: Flushed\n", thread->id);
                    if (!ok) return 0;                    
                    len = 0;
                    d = thread->tx_buffer;
                }
            }
        }
    }
    
//     fprintf(stderr, "Thread %i: Flush...\n", thread->id);
    int ok = (write_thread_write(thread, thread->tx_buffer, len) >= 0);
//     fprintf(stderr, "Thread %i: Flushed/Done\n", thread->id);
    
    return ok;
}

static int write_thread_write(struct write_thread* thread, char* buffer, int len){
    char* b = buffer;
    int timeout = 100*1000; //1000ms
    int error = 0;
    while (len && (error == 0) && (thread->vo->cfg_full_frames || !thread->vo->flip) && (thread->vo->quit == 0)){
        int ret = write(thread->socket, b, len);
        if (ret >= 0){
            b += ret;
            len -= ret;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK){
                fprintf(stderr, "Thread %i:", thread->id);
                perror("Write error");
                error = 1;
            } else { //Give more time
                usleep(100);
                timeout--;
                if (timeout == 0){
                    error = 1;
                    fprintf(stderr, "Thread %i: Write timeout\n", thread->id);
                }
            }
        }
    }
    if (error){
        return -1;
    }
    
    return len;
}           


static int query_format(struct vo *vo, int fmt){
    if (fmt == IMGFMT_RGB24) return 1;
    return 0;
}


static int reconfig(struct vo *vo, struct mp_image_params *params){
    return 0;
}

static void uninit(struct vo *vo){
    struct priv *p = vo->priv;
    p->quit = 1;
    
    for (int i = 0; i < p->num_threads; i++){
        pthread_join(p->threads[i]->pthread, 0);
        free(p->threads[i]);
    }
}

static int preinit(struct vo *vo){
    
    struct priv *p = vo->priv;
    if (!p->hostname){
        printf("Pixeflut server not specified!");
        return -1;
    }
    printf("Pixeflut server: %s\n", p->hostname);
    printf("Colorkey: %06x\n", p->cfg_colorkey);
    p->last = 0;
    p->current = 0;
    
    for (int i = 0; i < p->num_threads; i++){
        p->threads[i] = draw_thread_create(p, i, i , p->num_threads);
    }
    
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_pixelflut =
{
    .description = "Transmit video to Pixelflut canvas server",
    .name = "pixelflut",
    .untimed = false,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_STRING("server", hostname,        0),
        OPT_INT("x",           offset_x,        0),
        OPT_INT("y",           offset_y,        0),
        OPT_INT("colorkey",    cfg_colorkey,    0, OPTDEF_INT(-1)),
        OPT_INT("grayscale",   cfg_grayscale_optimize, 0),
        OPT_INT("port",        port,            0, OPTDEF_INT(1234)),
        OPT_INT("threads",     num_threads,     0, OPTDEF_INT(1)),
        OPT_INT("fullframe",   cfg_full_frames, 0, OPTDEF_INT(1)),
        OPT_INT("fullredraw",  cfg_full_redraw, 0, OPTDEF_INT(0)),
        {0},
    },
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_image = draw_image,
    .flip_page = flip_page,
    .uninit = uninit,
};
