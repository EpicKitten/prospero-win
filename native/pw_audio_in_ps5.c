/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_in_ps5.h"
#include <string.h>

/* sceAudioInOpen's types and its one parameter this adapter uses: 16-bit
 * mono. A voice-chat port is the one every voice-chat game opens. */
enum { PW_AUDIO_IN_PS5_TYPE_VOICE_CHAT=0, PW_AUDIO_IN_PS5_TYPE_GENERAL=1,
       PW_AUDIO_IN_PS5_PARAM_S16_MONO=0 };

static const struct { uint32_t type,rate; } ports[]={
    {PW_AUDIO_IN_PS5_TYPE_VOICE_CHAT,16000},
    {PW_AUDIO_IN_PS5_TYPE_GENERAL,48000},
};

#ifndef PW_AUDIO_IN_PS5_HOST_TEST
extern int sceAudioInOpen(int32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern int sceAudioInInput(int32_t,void *);
extern int sceAudioInGetSilentState(int32_t);
extern int sceAudioInClose(int32_t);
static int platform_open(int32_t u,uint32_t t,uint32_t i,uint32_t g,uint32_t r,uint32_t p)
{return sceAudioInOpen(u,t,i,g,r,p);}
static int platform_input(int32_t h,void *s){return sceAudioInInput(h,s);}
static int platform_silent_state(int32_t h){return sceAudioInGetSilentState(h);}
static int platform_close(int32_t h){return sceAudioInClose(h);}
#endif

int pw_audio_in_ps5_platform_ops(PwAudioInPs5Ops *ops)
{
    if(!ops)return PW_ERR_PRECONDITION;
#ifdef PW_AUDIO_IN_PS5_HOST_TEST
    memset(ops,0,sizeof(*ops));return PW_ERR_UNSUPPORTED;
#else
    *ops=(PwAudioInPs5Ops){platform_open,platform_input,platform_silent_state,platform_close};
    return PW_OK;
#endif
}

int pw_audio_in_ps5_open(PwAudioInPs5 *in,const PwAudioInPs5Ops *ops,int32_t user_id)
{
    if(!in)return PW_ERR_PRECONDITION;
    memset(in,0,sizeof(*in));in->handle=-1;in->user_id=user_id;
    if(!ops || !ops->open || !ops->input || !ops->close || user_id<0)return PW_ERR_PRECONDITION;
    in->ops=*ops;
    for(size_t i=0;i<sizeof(ports)/sizeof(ports[0]);i++) {
        in->open_rc=ops->open(user_id,ports[i].type,0,PW_AUDIO_IN_PS5_GRAIN,ports[i].rate,
                              PW_AUDIO_IN_PS5_PARAM_S16_MONO);
        if(in->open_rc>=0) {
            in->handle=in->open_rc;in->type=ports[i].type;in->rate=ports[i].rate;
            (void)pw_audio_in_ps5_poll(in);
            return PW_OK;
        }
    }
    return PW_ERR_STATE;
}

uint32_t pw_audio_in_ps5_classify(int silent)
{
    if(silent<0)return PW_AUDIO_IN_PS5_UNKNOWN;
    if(!silent)return PW_AUDIO_IN_PS5_LIVE;
    return silent&PW_AUDIO_IN_PS5_SILENT_DEVICE_NONE?PW_AUDIO_IN_PS5_NO_DEVICE:PW_AUDIO_IN_PS5_MUTED;
}

uint32_t pw_audio_in_ps5_poll(PwAudioInPs5 *in)
{
    uint32_t state=PW_AUDIO_IN_PS5_UNKNOWN;
    if(!in)return state;
    if(in->handle>=0 && in->ops.silent_state) {
        int silent=in->ops.silent_state(in->handle);
        __atomic_store_n(&in->last_silent,silent,__ATOMIC_RELAXED);
        state=pw_audio_in_ps5_classify(silent);
    }
    __atomic_store_n(&in->state,state,__ATOMIC_RELAXED);
    return state;
}

int pw_audio_in_ps5_read(PwAudioInPs5 *in,int16_t grain[PW_AUDIO_IN_PS5_GRAIN],uint32_t *state)
{
    if(state)*state=PW_AUDIO_IN_PS5_UNKNOWN;
    if(!in || !grain)return PW_ERR_PRECONDITION;
    if(in->handle<0){memset(grain,0,PW_AUDIO_IN_PS5_GRAIN*sizeof(*grain));return PW_ERR_PRECONDITION;}
    in->last_input_rc=in->ops.input(in->handle,grain);
    if(in->last_input_rc<0) {
        in->failed_grains++;
        memset(grain,0,PW_AUDIO_IN_PS5_GRAIN*sizeof(*grain));
        return PW_ERR_STATE;
    }
    in->grains++;
    uint32_t now=pw_audio_in_ps5_poll(in);
    if(now==PW_AUDIO_IN_PS5_MUTED || now==PW_AUDIO_IN_PS5_NO_DEVICE) {
        /* what a silenced port returns is not ours to pass on */
        memset(grain,0,PW_AUDIO_IN_PS5_GRAIN*sizeof(*grain));
        in->silent_grains++;
    }
    if(state)*state=now;
    return PW_OK;
}

int pw_audio_in_ps5_close(PwAudioInPs5 *in)
{
    if(!in)return PW_ERR_PRECONDITION;
    if(in->handle<0)return PW_OK;
    int rc=in->ops.close(in->handle);
    in->handle=-1;
    __atomic_store_n(&in->state,PW_AUDIO_IN_PS5_UNKNOWN,__ATOMIC_RELAXED);
    return rc<0?PW_ERR_STATE:PW_OK;
}
