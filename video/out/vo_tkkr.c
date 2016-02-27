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

#include <libswscale/swscale.h>

#include "config.h"
#include "misc/bstr.h"
#include "osdep/io.h"
#include "talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "video/out/vo.h"
#include "video/csputils.h"
#include "video/mp_image.h"
#include "video/fmt-conversion.h"
#include "video/sws_utils.h"
#include "sub/osd.h"
#include "options/m_option.h"

#define IMAGE_WIDTH 17
#define IMAGE_HEIGHT 10
#define IMAGE_SIZE (IMAGE_WIDTH * IMAGE_HEIGHT * 3)

typedef struct artnet_header{
    uint8_t magic[8];
    uint16_t opcode;
    uint16_t version;
    uint8_t seq;
    uint8_t physical;
    uint16_t universe;
    uint16_t data_length;
} __attribute__ ((packed)) artnet_header_t;

typedef struct artnet_message{
    artnet_header_t header;
    uint8_t data[IMAGE_SIZE];
} __attribute__ ((packed)) artnet_message_t;


struct priv {
    char *hostname;
    unsigned int port;
    struct sockaddr_in dest_addr;
    
    int fd;
    artnet_message_t msg;
};

//WS2101 leds are very non-linear. Bringing things down to 5 bit colordepth
const uint8_t led_lookup[32] = {
    0, 1, 2, 2, 2, 3, 3, 4, 5, 6, 7, 8, 10, 11, 13, 16, 19, 23,
    27, 32, 38, 45, 54, 64, 76, 91, 108, 128, 152, 181, 215, 255
};

static void draw_image(struct vo *vo, mp_image_t *in){
    struct priv *p = vo->priv;
    //Initialize Art-Net message
    memset(p->msg.data, 0, IMAGE_SIZE);
    memcpy(p->msg.header.magic, "Art-Net\0", 8);
    p->msg.header.opcode = 0x5000;
    p->msg.header.version = htons(14);
    p->msg.header.physical = 0;
    p->msg.header.universe = 0;
    p->msg.header.data_length = htons(IMAGE_SIZE);
    p->msg.header.seq++;
    
    int nChannels = in->fmt.bpp[0] / 8; //Bytes per pixel
    
    int widthStep = in->stride[0];
    int width = in->w;
    if (width > IMAGE_WIDTH) width = IMAGE_WIDTH;
    int height = in->h;
    if (height > IMAGE_HEIGHT) height = IMAGE_HEIGHT;
    
    for (int x = 0; x < width; x++){
        for (int y = 0; y < height; y++){
            unsigned int snake_pos = (x*IMAGE_HEIGHT + ((x & 1) ? (IMAGE_HEIGHT - y-1) : y )) * 3;
            
            uint8_t* dst = &p->msg.data[snake_pos];
            uint8_t* src = &in->planes[0][(y * widthStep) + (x*nChannels)];
            dst[0] = led_lookup[src[0] * 32 / 256];
            dst[1] = led_lookup[src[1] * 32 / 256];
            dst[2] = led_lookup[src[2] * 32 / 256];
            
        }
        
    }
}

static void flip_page(struct vo *vo){
    struct priv *p = vo->priv;
    if (sendto(p->fd, &(p->msg), sizeof(artnet_message_t), 0, &p->dest_addr, sizeof(p->dest_addr)) < 0){
        perror("Sendto failed");
    }
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

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->hostname) return -1;
    if (!p->port) p->port = 6454;
    
    p->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    printf("Opened socket %i\n", p->fd);
    if (p->fd < 0) return -1;
    
    memset((char *) &p->dest_addr, 0, sizeof(struct sockaddr_in)); 
    p->dest_addr.sin_family = AF_INET;
    p->dest_addr.sin_port = htons(p->port);
    if (inet_aton(p->hostname, &p->dest_addr.sin_addr)==0) return -1;
    
    p->msg.header.seq = rand();
    
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_tkkr =
{
    .description = "Transmit video to TkkrLab Pixelmatrix",
    .name = "tkkr",
    .untimed = false,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        OPT_STRING("hostname", hostname, 0),
//         OPT_INT("port", port, 0, OPTDEF_INT(6454)),
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
