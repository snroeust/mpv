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
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h> 
#include <unistd.h>
#include <netinet/in.h>

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


struct priv {
    char *hostname;
    int port;
    struct sockaddr_in dest_addr;
    int32_t cfg_colorkey;
    
    int offset_x;
    int offset_y;
    int delay;
    
    mp_image_t* last;
    
    int fd;
};

struct header {
    uint8_t format;
    uint8_t version;
}  __attribute__((packed));
struct RGB{
    union{
        struct{
            uint8_t r;
            uint8_t g;
            uint8_t b;
        };
        uint8_t color[3];
    };
} __attribute__((packed));

struct pixel{
    uint8_t xl;
    uint8_t xhyl;
    uint8_t yh;
    struct RGB color;
}  __attribute__((packed)); //Format 2 version

#define MAX_SIZE 1120
#define MAX_PIXELS ((MAX_SIZE-(sizeof(struct header)))/(sizeof(struct pixel))) //140

struct message {
    struct header header;
    struct pixel pixel[MAX_PIXELS];
    int count; //Used pixels in message (not sent)
}  __attribute__((packed));

#define MSG_SIZE(msg) (sizeof(struct header)+((msg)->count*sizeof(struct pixel)))


static void draw_image(struct vo *vo, mp_image_t *in){
    struct priv *p = vo->priv;
    useconds_t delay = p->delay;
    
    int line_step     = in->stride[0];
    uint8_t* img_data = in->planes[0];
    
    uint8_t* last_img_data = p->last ? p->last->planes[0] : 0;

    struct message msg = {{2,1}};
    msg.count = 0;
    
    for (int y = 0; y < in->h; y++){
        for (int x = 0; x < in->w; x++){
            struct RGB*   src  = (struct RGB*)&img_data[(y * line_step) + (x * 3)];
            
            int update = 1;
            if (last_img_data){
                struct RGB* last = (struct RGB*)&last_img_data[(y * line_step) + (x * 3)];
                update = (last->r != src->r) || (last->g |= src->g) || (last->b != src->b);
            }
            
            if (update){
                struct pixel* dst = &msg.pixel[msg.count];
                unsigned int dstx = x + p->offset_x;
                unsigned int dsty = y + p->offset_y;
                dst->xl = dstx;
                dst->xhyl = ((dstx >> 8) & 0xF) | ((dsty << 4) & 0xF0);
                dst->yh = dsty >> 8;
                dst->color = *src;
                msg.count++;
                if (msg.count == MAX_PIXELS){
//                  printf("Send: %i,%i %i\n", ((uint8_t*)&msg)[0], ((uint8_t*)&msg)[1] , MSG_SIZE(&msg));
                    send(p->fd, &msg, MSG_SIZE(&msg), 0);
                    msg.count = 0;
                    usleep(delay);
                }
            }
        }
    }
    if (msg.count){ //Send remainder
        send(p->fd, &msg, MSG_SIZE(&msg), 0);
    }
    
    if (p->last) talloc_free(p->last);
    p->last = in;
}

static void flip_page(struct vo *vo){
    
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

    close(p->fd);
    p->fd = -1;
}

static int preinit(struct vo *vo){
    
    struct priv *p = vo->priv;
    if (!p->hostname) return -1;
    if (!p->port) p->port = 5005;
    p->last = 0;
    
    p->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    printf("Opened socket %i sdsdsfd\n", p->fd);
    if (p->fd < 0) return -1;
    
    memset((char *) &p->dest_addr, 0, sizeof(struct sockaddr_in)); 
    p->dest_addr.sin_family = AF_INET;
    p->dest_addr.sin_port = htons(p->port);
    if (inet_aton(p->hostname, &p->dest_addr.sin_addr)==0) return -1;
    
    if (connect(p->fd, &p->dest_addr, sizeof(struct sockaddr_in)) < 0){
        perror("Connect failed");
        return -1;
    }
    
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_pixelfluteth0 =
{
    .description = "Transmit video to eth0 Pixelflut canvas server",
    .name = "pixelfluteth0",
    .untimed = false,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_STRING("hostname", hostname, 0),
        OPT_INT("x", offset_x, 0),
        OPT_INT("y", offset_y, 0),
        OPT_INT("port", port, 0, OPTDEF_INT(5005)),
        OPT_INT("delay", delay, 0),
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
