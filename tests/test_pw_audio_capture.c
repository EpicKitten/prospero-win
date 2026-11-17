/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_audio_capture.h"
#include <assert.h>
#include <string.h>

static PwAudioMixFormat pcm(uint32_t rate,uint16_t channels,uint16_t bits)
{
    return (PwAudioMixFormat){rate,channels,bits,(uint16_t)(channels*bits/8u),PW_AUDIO_MIX_PCM,0};
}

static int16_t s16_at(const uint8_t *ring,uint32_t frame,uint32_t channels,uint32_t channel)
{
    const uint8_t *at=ring+(frame*channels+channel)*2u;
    return (int16_t)(uint16_t)(at[0]|at[1]<<8);
}

static void test_init(void)
{
    PwAudioCapture c;uint8_t ring[64];
    PwAudioMixFormat f=pcm(16000,1,16);
    assert(pw_audio_capture_init(NULL,&f,ring,32,16000)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_init(&c,&f,NULL,32,16000)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_init(&c,&f,ring,0,16000)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_init(&c,NULL,ring,32,16000)==PW_ERR_PRECONDITION);
    f=pcm(16000,1,12);assert(pw_audio_capture_init(&c,&f,ring,32,16000)==PW_ERR_UNSUPPORTED);
    f=pcm(16000,1,16);
    assert(pw_audio_capture_init(&c,&f,ring,32,10)==PW_ERR_UNSUPPORTED);
    assert(pw_audio_capture_init(&c,&f,ring,32,16000)==PW_OK);
    assert(c.held==0 && c.written==0 && c.gain[0]==1);
    float g[2]={0.5f,0.5f};
    assert(pw_audio_capture_set_gain(&c,g,2)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_set_gain(&c,g,1)==PW_OK && c.gain[0]==0.5f);
    g[0]=3;assert(pw_audio_capture_set_gain(&c,g,1)==PW_OK && c.gain[0]==1);
    g[0]=-1;assert(pw_audio_capture_set_gain(&c,g,1)==PW_OK && c.gain[0]==0);
}

/* At the microphone's own rate a sample is one frame, unchanged and a
 * sample late (the interpolation's history), copied to every channel. */
static void test_same_rate_stereo(void)
{
    PwAudioCapture c;uint8_t ring[4*16];
    PwAudioMixFormat f=pcm(16000,2,16);
    int16_t in[8]={1000,-2000,3000,-4000,32767,-32768,0,5};
    assert(pw_audio_capture_init(&c,&f,ring,16,16000)==PW_OK);
    assert(pw_audio_capture_put(&c,in,8)==8 && c.held==8 && c.written==8);
    assert(s16_at(ring,0,2,0)==0 && s16_at(ring,0,2,1)==0);
    for(uint32_t i=1;i<8;i++) {
        assert(s16_at(ring,i,2,0)==in[i-1] && s16_at(ring,i,2,1)==in[i-1]);
    }
    /* the grain's last sample comes out with the next one */
    assert(pw_audio_capture_put(&c,NULL,1)==1 && s16_at(ring,8,2,0)==5);
    pw_audio_capture_take(&c,3);assert(c.offset==3 && c.held==6);
    pw_audio_capture_take(&c,100);assert(c.held==0 && c.offset==9);
}

/* 16 kHz into 48 kHz: three frames a sample, interpolated between them. */
static void test_upsample(void)
{
    PwAudioCapture c;uint8_t ring[2*64];
    PwAudioMixFormat f=pcm(48000,1,16);
    int16_t in[4]={0,3000,6000,9000};
    assert(pw_audio_capture_init(&c,&f,ring,64,16000)==PW_OK);
    assert(pw_audio_capture_put(&c,in,4)==12 && c.held==12);
    for(uint32_t i=0;i<12;i++) {
        int want=(int)i*1000-3000;
        if(want<0)want=0;               /* before the first sample: its history is silence */
        int got=s16_at(ring,i,1,0);
        assert(got>=want-2 && got<=want+2);
    }
    /* a grain split in two calls gives the same frames as one call */
    PwAudioCapture d;uint8_t ring2[2*64];
    assert(pw_audio_capture_init(&d,&f,ring2,64,16000)==PW_OK);
    assert(pw_audio_capture_put(&d,in,1)+pw_audio_capture_put(&d,in+1,3)==12);
    assert(!memcmp(ring,ring2,12*2));
}

/* 48 kHz into 16 kHz: a frame every three samples. */
static void test_downsample(void)
{
    PwAudioCapture c;uint8_t ring[2*16];
    PwAudioMixFormat f=pcm(16000,1,16);
    int16_t in[12];
    for(int i=0;i<12;i++)in[i]=(int16_t)(i*100);
    assert(pw_audio_capture_init(&c,&f,ring,16,48000)==PW_OK);
    assert(pw_audio_capture_put(&c,in,12)==4);
    assert(s16_at(ring,0,1,0)==0 && s16_at(ring,1,1,0)==200 && s16_at(ring,3,1,0)==800);
}

/* Every container the mixer accepts, at full scale and silence. */
static void test_encodings(void)
{
    PwAudioCapture c;uint8_t ring[8*4];
    int16_t in[3]={32767,0,-32768};
    PwAudioMixFormat f=pcm(16000,1,8);
    assert(pw_audio_capture_init(&c,&f,ring,4,16000)==PW_OK);
    pw_audio_capture_put(&c,in,3);pw_audio_capture_put(&c,NULL,1);
    assert(ring[0]==128 && ring[1]==255 && ring[2]==128 && ring[3]==0);
    f=pcm(16000,1,24);
    assert(pw_audio_capture_init(&c,&f,ring,4,16000)==PW_OK);
    pw_audio_capture_put(&c,in,3);pw_audio_capture_put(&c,NULL,1);
    assert(ring[3]==0x00 && ring[4]==0xff && ring[5]==0x7f);         /* 0x7fff00 */
    assert(ring[9]==0x00 && ring[10]==0x00 && ring[11]==0x80);       /* -0x800000 */
    f=pcm(16000,1,32);
    assert(pw_audio_capture_init(&c,&f,ring,4,16000)==PW_OK);
    pw_audio_capture_put(&c,in,3);pw_audio_capture_put(&c,NULL,1);
    assert(ring[4]==0x00 && ring[6]==0xff && ring[7]==0x7f && ring[12]==0x00 && ring[15]==0x80);
    f=(PwAudioMixFormat){16000,2,32,8,PW_AUDIO_MIX_FLOAT,0};
    assert(pw_audio_capture_init(&c,&f,ring,4,16000)==PW_OK);
    float g[2]={1,0.5f};assert(pw_audio_capture_set_gain(&c,g,2)==PW_OK);
    pw_audio_capture_put(&c,in,2);
    float left,right;
    memcpy(&left,ring+8,4);memcpy(&right,ring+12,4);
    assert(left>0.9999f && left<=1 && right>0.4999f && right<0.5001f);
}

/* A full ring keeps the newest frames and counts the rest. */
static void test_overrun(void)
{
    PwAudioCapture c;uint8_t ring[2*4];
    PwAudioMixFormat f=pcm(16000,1,16);
    int16_t in[7]={1,2,3,4,5,6,7};
    assert(pw_audio_capture_init(&c,&f,ring,4,16000)==PW_OK);
    assert(pw_audio_capture_put(&c,in,7)==7);
    assert(c.held==4 && c.dropped==3 && c.written==7 && c.offset==3);
    /* the oldest kept frame is the fourth (0, then 1..6) */
    assert(s16_at(ring,c.offset,1,0)==3);
    pw_audio_capture_reset(&c);
    assert(c.held==0 && c.dropped==0 && c.offset==0 && c.written==0);
}

int main(void)
{
    test_init();
    test_same_rate_stereo();
    test_upsample();
    test_downsample();
    test_encodings();
    test_overrun();
    return 0;
}
