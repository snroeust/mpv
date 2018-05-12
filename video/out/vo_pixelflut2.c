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

#define MAX_RENDER_THREADS   1000
#define TX_BUFFER_BLOCKS     12000
#define TX_BUFFER_BLOCK_SIZE 4096

struct priv;

struct pixel{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct write_thread{
    struct priv* vo;
    
    pthread_t pthread;
    int id;
    
    int block;
    int pos;
    
    int socket;
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
    
    char tx_buffer[TX_BUFFER_BLOCKS][TX_BUFFER_BLOCK_SIZE];
    int  tx_len[TX_BUFFER_BLOCKS];
    
    int num_draw_blocks;
    int current_draw_block;
    pthread_rwlock_t lock;
    
    int flip; //Requests threads to stop writing the current frame
    int quit; //Requests threads to exit
    
    int num_threads;
    struct write_thread* threads[MAX_RENDER_THREADS];
};

typedef struct point{
    int x;
    int y;
} point_t;


static struct write_thread* draw_thread_create(struct priv* vo, int id);
static void* draw_thread(void* arg);
static int draw_thread_connect(struct write_thread* thread);
static int write_thread_write(struct write_thread* thread, char* buffer, int len);
static void convert_frame(struct priv* p);


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
    
    //Flip frame buffers
    if (p->last) talloc_free(p->last);
    p->last = p->current;
    p->current = new;
    
    p->flip = 1; //Tell threads to stop writing and release the lock, so we can convert a new frame
    printf("Flip...\n");
//     pthread_rwlock_wrlock(&p->lock);
    printf("Convert...\n");
    convert_frame(p);
//     pthread_rwlock_unlock(&p->lock);
    printf("Done\n");
    p->flip = 0;
}

/***
 * @brief Convert frame buffer to ascii PX command string
 */
static void convert_frame(struct priv* p){
    int line_step = p->current->stride[0];
    uint8_t* img_data = p->current->planes[0];
    
    uint8_t* last_img_data = p->last ? p->last->planes[0] : 0;
    
    p->num_draw_blocks = 0;
    int current_block = 0;
    char* d = p->tx_buffer[current_block];
    size_t len = 0;
        
    for (int y = 0; y < p->current->h; y += 1){
        for (int x = 0; x < p->current->w; x++){
            uint8_t* px = &img_data[(y * line_step) + (x * 3)];
            uint8_t* last_px = last_img_data ? &last_img_data[(y * line_step) + (x * 3)] : 0;
            if (p->cfg_full_redraw || ((last_px == 0) || (p->current->stride[0] != p->last->stride[0]) || (p->current->h != p->last->h)
                || (abs(px[0] - last_px[0]) + abs(px[1] - last_px[1]) + abs(px[2] - last_px[2]) ) > 2)){
                
                size_t l= 0;
                if ((p->cfg_colorkey < 0) || (abs( (p->cfg_colorkey & 0xFF) - px[0]) + abs( ((p->cfg_colorkey >> 8) & 0xFF) - px[1]) + abs( ((p->cfg_colorkey >> 16) & 0xFF) - px[2]) ) > 25){
                    point_t t = {p->offset_x + x, p->offset_y + y};
                    
                    if (p->cfg_grayscale_optimize && (px[0] == px[1]) && (px[1] == px[2])){ //Grayscale optimize
                        l = sprintf(d, "PX %i %i %02x\n", t.x, t.y, px[0]);
                    } else {
                        l = sprintf(d, "PX %i %i %02x%02x%02x\n", t.x, t.y, px[0], px[1], px[2]);
                    }
                    
                } else {
                    l = sprintf(d, "PX %i %i 000000\n", p->offset_x + x, p->offset_y + y);
                }
                d+=l;
                len +=l;
                if (len > TX_BUFFER_BLOCK_SIZE-125){
                    p->tx_len[current_block] = len;
                    current_block++;
                    len = 0;
                    if (current_block >= TX_BUFFER_BLOCKS) {fprintf(stderr, "Image too large for tx buffer\n"); break;}
                    d = p->tx_buffer[current_block];
                }
            }
        }
        if (current_block >= TX_BUFFER_BLOCKS) break;
    }
    
    if (len){
        p->tx_len[current_block] = len;
        current_block++;
    }
    printf("Convert blocks: %i\n", current_block);
    p->num_draw_blocks = current_block;
}

static void flip_page(struct vo *vo){
    
}

static int get_next_draw_block(struct write_thread* thread){
    pthread_rwlock_wrlock(&thread->vo->lock);
    if (thread->vo->num_draw_blocks <= 0) {pthread_rwlock_unlock(&thread->vo->lock); return -1;} //No data yet
    int block = thread->vo->current_draw_block++;
    if (thread->vo->current_draw_block >= thread->vo->num_draw_blocks) thread->vo->current_draw_block = 0;
    pthread_rwlock_unlock(&thread->vo->lock);
    printf("Thread %i fetched block %i (size %i)\n", thread->id, block, thread->vo->tx_len[block]);
    return block;
}


static struct write_thread* draw_thread_create(struct priv* vo, int id){
    struct write_thread* thread = malloc(sizeof(struct write_thread));
    thread->id = id;
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

    fprintf(stderr, "Thread %i: Running...\n",thread->id);
    
    double pts = -100;
    
    struct priv* vo = thread->vo;
    
    while (vo->quit == 0) {
        if ((thread->socket < 0) && (draw_thread_connect(thread) == 0)) sleep(1); //Connect until succesful or exit
        if (thread->socket < 0) continue; //Not connected
        if (vo->current == 0) {usleep(100); continue;} //No frame to draw
        if ((vo->cfg_full_redraw == 0) && (vo->current->pts == pts)) {usleep(10); continue;}
        
        int block = get_next_draw_block(thread);
        if (block < 0) {usleep(10); continue;}
        
        pts = vo->current->pts;
        
        pthread_rwlock_rdlock(&vo->lock);
        
        if (write_thread_write(thread, vo->tx_buffer[block], vo->tx_len[block]) != 0){
            fprintf(stderr, "Thread %i: Write failed\n",thread->id);
            close(thread->socket);
            thread->socket = -1;
        }
        pthread_rwlock_unlock(&vo->lock);
    }
    
    if (thread->socket >= 0) close(thread->socket);
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


static int write_thread_write(struct write_thread* thread, char* buffer, int len){
    char* b = buffer;
    int timeout = 100*100; //100ms
    int error = 0;
    struct timespec tstart;
    clock_gettime(CLOCK_MONOTONIC , &tstart);
    
    while (len && (error == 0) && (thread->vo->cfg_full_frames || !thread->vo->flip) && (thread->vo->quit == 0)){
        clock_gettime(CLOCK_MONOTONIC , &tstart);
        
        int ret = write(thread->socket, b, len);
        
        struct timespec tend;
        clock_gettime(CLOCK_MONOTONIC , &tend);
        long ms = (tend.tv_sec - tstart.tv_sec) * 1000L + (tend.tv_nsec / 1000000L - tstart.tv_nsec / 1000000L);
        fprintf(stderr, "Thread %i: Blocked for %lu ns\n", thread->id, ms);
    
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
    if (!p->hostname) return -1;
    p->last = 0;
    p->current = 0;
    
    if (pthread_rwlock_init(&p->lock, NULL)){
        perror("Lock create failed");
        return 1;
    }
    
    for (int i = 0; i < p->num_threads; i++){
        p->threads[i] = draw_thread_create(p, i);
    }
    
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_pixelflut2 =
{
    .description = "Transmit video to Pixelflut canvas server",
    .name = "pixelflut2",
    .untimed = false,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_STRING("hostname", hostname,        0),
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
