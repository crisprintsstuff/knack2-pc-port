// SPDX-License-Identifier: GPL-2.0-or-later
// Local unlock flags only; never changes the game's source files or PSN state.
#include "runtime.h"
#include "game.h"
#include <stdio.h>
#include <string.h>
#include "content_profile.h"
static SRWLOCK lock=SRWLOCK_INIT;
static int handles[16];
static struct {int active,registered,user;uint32_t flags[4];char file[1400];} contexts[8];
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80551600)|n);}
static int valid(int c,int h){return c>0&&c<=8&&contexts[c-1].active&&h>0&&h<=16&&handles[h-1];}
static int32_t GUEST_ABI create_handle(int *out){if(!out)return err(4);AcquireSRWLockExclusive(&lock);for(int i=0;i<16;++i)if(!handles[i]){handles[i]=1;*out=i+1;ReleaseSRWLockExclusive(&lock);return 0;}ReleaseSRWLockExclusive(&lock);return err(5);}
static int32_t GUEST_ABI create_context(int *out,int user,uint32_t label,uint64_t options){
    if(!out||user<0||label||options)return err(4);AcquireSRWLockExclusive(&lock);
    for(int i=0;i<8;++i)if(!contexts[i].active){memset(&contexts[i],0,sizeof(contexts[i]));contexts[i].active=1;contexts[i].user=user;*out=i+1;ReleaseSRWLockExclusive(&lock);return 0;}
    ReleaseSRWLockExclusive(&lock);return err(5);
}
static int32_t GUEST_ABI register_context(int c,int h,uint64_t options){
    if(options)return err(4);AcquireSRWLockExclusive(&lock);if(!valid(c,h)){ReleaseSRWLockExclusive(&lock);return err(9);}
    char root[1300];snprintf(root,sizeof(root),"%s\\trophies",runtime_user_root());CreateDirectoryA(root,NULL);
    snprintf(contexts[c-1].file,sizeof(contexts[c-1].file),"%s\\" GAME_TITLE_ID "-user%d.bin",root,contexts[c-1].user);
    FILE *f=fopen(contexts[c-1].file,"rb");if(f){fread(contexts[c-1].flags,1,16,f);fclose(f);}contexts[c-1].registered=1;
    ReleaseSRWLockExclusive(&lock);return 0;
}
static int32_t GUEST_ABI unlock_state(int c,int h,uint32_t *flags,uint32_t *count){
    if(!flags||!count)return err(4);AcquireSRWLockShared(&lock);
    if(!valid(c,h)||!contexts[c-1].registered){ReleaseSRWLockShared(&lock);return err(15);}
    memcpy(flags,contexts[c-1].flags,16);*count=LOCAL_TROPHY_COUNT;ReleaseSRWLockShared(&lock);return 0;
}
static int32_t GUEST_ABI unlock(int c,int h,int id,int *platinum){
    if(!platinum)return err(4);int known=0;for(unsigned i=0;i<LOCAL_TROPHY_COUNT;++i)if(trophy_ids[i]==id)known=1;
    if(!known)return err(7);AcquireSRWLockExclusive(&lock);
    if(!valid(c,h)||!contexts[c-1].registered){ReleaseSRWLockExclusive(&lock);return err(15);}
    contexts[c-1].flags[id/32]|=1u<<(id%32);*platinum=-1;
    FILE *f=fopen(contexts[c-1].file,"wb");int ok=f&&fwrite(contexts[c-1].flags,1,16,f)==16;if(f)fclose(f);
    ReleaseSRWLockExclusive(&lock);return ok?0:err(1);
}
// Names, descriptions and grades live in the encrypted trophy configuration, so counts and unlock
// state are real while names are generic and grades unknown (0).
typedef struct {uint64_t size;uint32_t groups,trophies,platinum,gold,silver,bronze;char title[128],description[1024];} GameDetails;
typedef struct {uint64_t size;uint32_t unlocked,platinum,gold,silver,bronze,progress;} GameData;
typedef struct {uint64_t size;int32_t id,grade,group;uint8_t hidden,reserved[3];char name[128],description[1024];} Details;
typedef struct {uint64_t size;int32_t id;uint8_t unlocked,reserved[3];uint64_t timestamp;} Data;
static unsigned unlocked_count(const uint32_t *flags){unsigned n=0;for(unsigned i=0;i<LOCAL_TROPHY_COUNT;++i)n+=(flags[trophy_ids[i]/32]>>(trophy_ids[i]%32))&1;return n;}
static int32_t GUEST_ABI game_info(int c,int h,GameDetails *details,GameData *data){
    if(!details&&!data)return err(4);AcquireSRWLockShared(&lock);
    if(!valid(c,h)||!contexts[c-1].registered){ReleaseSRWLockShared(&lock);return err(15);}
    unsigned unlocked=unlocked_count(contexts[c-1].flags);ReleaseSRWLockShared(&lock);
    if(details){uint64_t size=details->size;memset(details,0,sizeof(*details));details->size=size;details->trophies=LOCAL_TROPHY_COUNT;snprintf(details->title,128,"%s",GAME_TITLE);}
    if(data){uint64_t size=data->size;memset(data,0,sizeof(*data));data->size=size;data->unlocked=unlocked;data->progress=LOCAL_TROPHY_COUNT?unlocked*100/LOCAL_TROPHY_COUNT:0;}
    return 0;
}
static int32_t GUEST_ABI trophy_info(int c,int h,int id,Details *details,Data *data){
    if(!details&&!data)return err(4);int known=0;for(unsigned i=0;i<LOCAL_TROPHY_COUNT;++i)if(trophy_ids[i]==id)known=1;
    if(!known)return err(7);AcquireSRWLockShared(&lock);
    if(!valid(c,h)||!contexts[c-1].registered){ReleaseSRWLockShared(&lock);return err(15);}
    int unlocked=(contexts[c-1].flags[id/32]>>(id%32))&1;ReleaseSRWLockShared(&lock);
    if(details){uint64_t size=details->size;memset(details,0,sizeof(*details));details->size=size;details->id=id;details->group=-1;snprintf(details->name,128,"Trophy %d",id);}
    if(data){uint64_t size=data->size;memset(data,0,sizeof(*data));data->size=size;data->id=id;data->unlocked=(uint8_t)unlocked;}
    return 0;
}
static int32_t GUEST_ABI destroy_handle(int h){if(h<1||h>16)return err(8);AcquireSRWLockExclusive(&lock);handles[h-1]=0;ReleaseSRWLockExclusive(&lock);return 0;}
static int32_t GUEST_ABI destroy_context(int c){if(c<1||c>8)return err(9);AcquireSRWLockExclusive(&lock);contexts[c-1].active=0;ReleaseSRWLockExclusive(&lock);return 0;}
uintptr_t trophy_resolve(const char *identity){
    if(!strstr(identity,"#libSceNpTrophy#1#libSceNpTrophy#257"))return 0;const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceNpTrophyCreateHandle",create_handle);API("sceNpTrophyCreateContext",create_context);
    API("sceNpTrophyRegisterContext",register_context);API("sceNpTrophyGetTrophyUnlockState",unlock_state);
    API("sceNpTrophyUnlockTrophy",unlock);API("sceNpTrophyGetGameInfo",game_info);API("sceNpTrophyGetTrophyInfo",trophy_info);API("sceNpTrophyDestroyHandle",destroy_handle);API("sceNpTrophyDestroyContext",destroy_context);
#undef API
    return 0;
}
