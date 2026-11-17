/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_capture.h"
#include <string.h>

#define ONE ((uint64_t)1 << 32)

int pw_audio_capture_init(PwAudioCapture *capture,const PwAudioMixFormat *format,
                          uint8_t *frames,uint32_t capacity,uint32_t source_rate)
{
    if(!capture || !frames || !capacity)return PW_ERR_PRECONDITION;
    int status=pw_audio_mix_format_check(format);
    if(status!=PW_OK)return status;
    if(source_rate<PW_AUDIO_MIX_MIN_RATE || source_rate>PW_AUDIO_MIX_MAX_RATE)return PW_ERR_UNSUPPORTED;
    memset(capture,0,sizeof(*capture));
    capture->format=*format;capture->frames=frames;capture->capacity=capacity;
    capture->source_rate=source_rate;
    /* rounded up, so a whole ratio (48 kHz into 16 kHz) lands on its samples */
    capture->step=(((uint64_t)source_rate<<32)+format->rate-1)/format->rate;
    for(unsigned channel=0;channel<PW_AUDIO_MIX_MAX_CHANNELS;channel++)capture->gain[channel]=1;
    pw_audio_capture_reset(capture);
    return PW_OK;
}

void pw_audio_capture_reset(PwAudioCapture *capture)
{
    if(!capture)return;
    capture->offset=capture->held=0;capture->written=capture->dropped=0;
    capture->phase=ONE;     /* the first stream frame reads one sample */
    capture->previous=capture->current=0;
}

int pw_audio_capture_set_gain(PwAudioCapture *capture,const float *gain,uint32_t count)
{
    if(!capture || !gain || count!=capture->format.channels)return PW_ERR_PRECONDITION;
    for(uint32_t channel=0;channel<count;channel++) {
        float value=gain[channel];
        capture->gain[channel]=value>1?1:value>0?value:0;
    }
    return PW_OK;
}

/* Full scale is 2^(bits-1), as the microphone's 16 bits are, so 16-bit
 * samples pass unchanged; round half away from zero, within [low, high]. */
static int64_t scaled(float value,double scale,int64_t low,int64_t high)
{
    double v=(double)value*scale;
    int64_t r=(int64_t)(v>=0?v+0.5:v-0.5);
    return r<low?low:r>high?high:r;
}

static void encode(uint8_t *at,float value,const PwAudioMixFormat *format)
{
    int64_t v;
    value=value>1?1:value<-1?-1:value;
    if(format->encoding==PW_AUDIO_MIX_FLOAT){memcpy(at,&value,sizeof(value));return;}
    switch(format->bits) {
    case 8:at[0]=(uint8_t)(scaled(value,128,-128,127)+128);return;
    case 16:
        v=scaled(value,32768,-32768,32767);
        at[0]=(uint8_t)v;at[1]=(uint8_t)(v>>8);return;
    case 24:
        v=scaled(value,8388608,-8388608,8388607);
        at[0]=(uint8_t)v;at[1]=(uint8_t)(v>>8);at[2]=(uint8_t)(v>>16);return;
    default:
        v=scaled(value,2147483648.0,-2147483647-1LL,2147483647);
        at[0]=(uint8_t)v;at[1]=(uint8_t)(v>>8);at[2]=(uint8_t)(v>>16);at[3]=(uint8_t)(v>>24);return;
    }
}

/* One stream frame at the back of the ring; a full ring loses its oldest. */
static void append(PwAudioCapture *capture,float value)
{
    const PwAudioMixFormat *format=&capture->format;
    if(capture->held==capture->capacity) {
        if(++capture->offset==capture->capacity)capture->offset=0;
        capture->held--;capture->dropped++;
    }
    uint32_t at=capture->offset+capture->held;
    if(at>=capture->capacity)at-=capture->capacity;
    uint8_t *frame=capture->frames+(size_t)at*format->block_align;
    for(unsigned channel=0;channel<format->channels;channel++)
        encode(frame+channel*(format->bits/8u),value*capture->gain[channel],format);
    capture->held++;capture->written++;
}

uint32_t pw_audio_capture_put(PwAudioCapture *capture,const int16_t *samples,uint32_t count)
{
    uint32_t used=0,added=0;
    if(!capture)return 0;
    for(;;) {
        while(capture->phase>=ONE) {
            if(used==count)return added;
            capture->previous=capture->current;
            capture->current=samples?samples[used]*(1.0f/32768):0;
            used++;capture->phase-=ONE;
        }
        float t=(float)(uint32_t)capture->phase*(1.0f/4294967296.0f);
        append(capture,capture->previous+(capture->current-capture->previous)*t);
        added++;
        capture->phase+=capture->step;
    }
}

void pw_audio_capture_take(PwAudioCapture *capture,uint32_t frames)
{
    if(!capture)return;
    if(frames>capture->held)frames=capture->held;
    capture->offset=(uint32_t)(((uint64_t)capture->offset+frames)%capture->capacity);
    capture->held-=frames;
}
