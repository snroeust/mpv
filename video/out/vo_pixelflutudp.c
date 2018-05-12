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
    
    int fd;
};

struct header {
    uint16_t x;
    uint16_t y;
    uint16_t width;
};

#define BUFFER_SIZE 65535
static char buffer[BUFFER_SIZE];

static void draw_image(struct vo *vo, mp_image_t *in){
    struct priv *p = vo->priv;
    
    int widthStep = in->stride[0];
    uint8_t* img_data = in->planes[0];
    int lines_per_datagram = BUFFER_SIZE / in->w / 3;

    struct header head;
    head.x = p->offset_x;
    head.width = in->w;

    for (int y = 0; y < in->h; y+=lines_per_datagram){
        head.y = y + p->offset_y;
        if ((in->h - y) < lines_per_datagram) lines_per_datagram = in->h - y;
        
        memcpy(buffer, &head, sizeof(head));
        for (int y2 = 0; y2 < lines_per_datagram; y2++){
            memcpy(buffer + sizeof(head) + (in->w * 3 * y2) , img_data + (widthStep * (y+y2)), (in->w * 3));
        }
        send(p->fd, buffer, sizeof(head) + (in->w * 3 * lines_per_datagram), 0);
    }
    
    talloc_free(in);
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
    if (!p->port) p->port = 1234;
    
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

const struct vo_driver video_out_pixelflutudp =
{
    .description = "Transmit video to UDP Pixelflut canvas server",
    .name = "pixelflutudp",
    .untimed = false,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_STRING("hostname", hostname, 0),
        OPT_INT("x", offset_x, 0),
        OPT_INT("y", offset_y, 0),
        OPT_INT("port", port, 0, OPTDEF_INT(1234)),
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
