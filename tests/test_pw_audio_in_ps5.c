/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_audio_in_ps5.h"
#include <assert.h>
#include <string.h>

static unsigned opens,inputs,closes;
static int refuse_voice,refuse_general,fail_input,silent=0;
static uint32_t opened_type,opened_rate;

static int op_open(int32_t u,uint32_t t,uint32_t i,uint32_t g,uint32_t r,uint32_t p)
{
    assert(u==3 && i==0 && g==256 && p==0);
    opens++;
    if(t==0){assert(r==16000);if(refuse_voice)return -0x7fffff00;}
    else {assert(t==1 && r==48000);if(refuse_general)return -0x7fffff01;}
    opened_type=t;opened_rate=r;
    return 0x30000000|(int)t<<16;
}
static int op_input(int32_t h,void *s)
{
    assert(h>=0x30000000);inputs++;
    if(fail_input)return -5;
    int16_t *grain=s;
    for(int i=0;i<PW_AUDIO_IN_PS5_GRAIN;i++)grain[i]=(int16_t)(i+1);
    return PW_AUDIO_IN_PS5_GRAIN;
}
static int op_silent(int32_t h){assert(h>=0x30000000);return silent;}
static int op_close(int32_t h){assert(h>=0x30000000);closes++;return 0;}

static void reset(void){opens=inputs=closes=0;refuse_voice=refuse_general=fail_input=silent=0;}

static void test_classify(void)
{
    assert(pw_audio_in_ps5_classify(-1)==PW_AUDIO_IN_PS5_UNKNOWN);
    assert(pw_audio_in_ps5_classify(0)==PW_AUDIO_IN_PS5_LIVE);
    assert(pw_audio_in_ps5_classify(1)==PW_AUDIO_IN_PS5_NO_DEVICE);
    assert(pw_audio_in_ps5_classify(1|8)==PW_AUDIO_IN_PS5_NO_DEVICE);
    /* the system's mute, a higher-priority user (party chat), any other reason */
    assert(pw_audio_in_ps5_classify(2)==PW_AUDIO_IN_PS5_MUTED);
    assert(pw_audio_in_ps5_classify(4)==PW_AUDIO_IN_PS5_MUTED);
    assert(pw_audio_in_ps5_classify(8)==PW_AUDIO_IN_PS5_MUTED);
}

static void test_open(void)
{
    PwAudioInPs5Ops ops={op_open,op_input,op_silent,op_close};
    PwAudioInPs5 in;
    reset();
    assert(pw_audio_in_ps5_open(NULL,&ops,3)==PW_ERR_PRECONDITION);
    assert(pw_audio_in_ps5_open(&in,NULL,3)==PW_ERR_PRECONDITION && in.handle==-1);
    assert(pw_audio_in_ps5_open(&in,&ops,-1)==PW_ERR_PRECONDITION && opens==0);
    PwAudioInPs5Ops partial=ops;partial.input=NULL;
    assert(pw_audio_in_ps5_open(&in,&partial,3)==PW_ERR_PRECONDITION && opens==0);
    /* the voice-chat port first */
    assert(pw_audio_in_ps5_open(&in,&ops,3)==PW_OK && opens==1);
    assert(in.type==0 && in.rate==16000 && in.handle==0x30000000 && in.user_id==3);
    assert(in.state==PW_AUDIO_IN_PS5_LIVE);
    assert(pw_audio_in_ps5_close(&in)==PW_OK && closes==1 && in.handle==-1);
    assert(in.state==PW_AUDIO_IN_PS5_UNKNOWN);
    assert(pw_audio_in_ps5_close(&in)==PW_OK && closes==1);
    /* then a general one at 48 kHz */
    reset();refuse_voice=1;
    assert(pw_audio_in_ps5_open(&in,&ops,3)==PW_OK && opens==2);
    assert(in.type==1 && in.rate==48000);
    /* neither: the last refusal is kept */
    reset();refuse_voice=refuse_general=1;
    assert(pw_audio_in_ps5_open(&in,&ops,3)==PW_ERR_STATE && opens==2);
    assert(in.handle==-1 && in.open_rc==-0x7fffff01);
    /* a muted microphone opens muted */
    reset();silent=4;
    assert(pw_audio_in_ps5_open(&in,&ops,3)==PW_OK && in.state==PW_AUDIO_IN_PS5_MUTED);
}

static void test_read(void)
{
    PwAudioInPs5Ops ops={op_open,op_input,op_silent,op_close};
    PwAudioInPs5 in;
    int16_t grain[PW_AUDIO_IN_PS5_GRAIN];
    uint32_t state=99;
    reset();
    memset(&in,0,sizeof(in));in.handle=-1;
    assert(pw_audio_in_ps5_read(&in,grain,&state)==PW_ERR_PRECONDITION && state==PW_AUDIO_IN_PS5_UNKNOWN);
    assert(pw_audio_in_ps5_open(&in,&ops,3)==PW_OK);
    assert(pw_audio_in_ps5_read(&in,NULL,&state)==PW_ERR_PRECONDITION);
    /* live: the grain as captured */
    assert(pw_audio_in_ps5_read(&in,grain,&state)==PW_OK && state==PW_AUDIO_IN_PS5_LIVE);
    assert(grain[0]==1 && grain[255]==256 && in.grains==1 && in.silent_grains==0);
    assert(pw_audio_in_ps5_read(&in,grain,NULL)==PW_OK && in.grains==2);
    /* the mute button: zeros, whatever the port returned */
    silent=4;
    assert(pw_audio_in_ps5_read(&in,grain,&state)==PW_OK && state==PW_AUDIO_IN_PS5_MUTED);
    assert(grain[0]==0 && grain[255]==0 && in.silent_grains==1 && in.last_silent==4);
    assert(pw_audio_in_ps5_poll(&in)==PW_AUDIO_IN_PS5_MUTED);
    /* the controller went away */
    silent=1;
    assert(pw_audio_in_ps5_read(&in,grain,&state)==PW_OK && state==PW_AUDIO_IN_PS5_NO_DEVICE);
    assert(grain[10]==0 && in.silent_grains==2);
    /* unmuted again */
    silent=0;
    assert(pw_audio_in_ps5_poll(&in)==PW_AUDIO_IN_PS5_LIVE && in.state==PW_AUDIO_IN_PS5_LIVE);
    /* a failed state call does not silence the grain */
    silent=-7;
    assert(pw_audio_in_ps5_read(&in,grain,&state)==PW_OK && state==PW_AUDIO_IN_PS5_UNKNOWN);
    assert(grain[0]==1);
    /* a failed capture is zeros and counted */
    silent=0;fail_input=1;
    assert(pw_audio_in_ps5_read(&in,grain,&state)==PW_ERR_STATE && grain[0]==0);
    assert(in.failed_grains==1 && in.last_input_rc==-5);
    /* without a silent-state op the state is unknown and grains pass */
    fail_input=0;
    PwAudioInPs5Ops plain=ops;plain.silent_state=NULL;
    assert(pw_audio_in_ps5_open(&in,&plain,3)==PW_OK && in.state==PW_AUDIO_IN_PS5_UNKNOWN);
    assert(pw_audio_in_ps5_read(&in,grain,&state)==PW_OK && state==PW_AUDIO_IN_PS5_UNKNOWN && grain[3]==4);
}

int main(void)
{
    test_classify();
    test_open();
    test_read();
    PwAudioInPs5Ops ops;
    assert(pw_audio_in_ps5_platform_ops(NULL)==PW_ERR_PRECONDITION);
    /* The host build has no console library behind the ops. */
    assert(pw_audio_in_ps5_platform_ops(&ops)==PW_ERR_UNSUPPORTED && !ops.open && !ops.input);
    assert(pw_audio_in_ps5_poll(NULL)==PW_AUDIO_IN_PS5_UNKNOWN);
    return 0;
}
