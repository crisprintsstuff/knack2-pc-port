// SPDX-License-Identifier: GPL-2.0-or-later
// Save data is isolated under the new runtime's own user directory.
#include "runtime.h"
#include "game.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "save_pattern.h"
typedef struct { char name[32]; } DirName;
typedef struct { char name[16]; } Point;
typedef struct { char title[128],subtitle[128],detail[1024];uint32_t user_param,pad;int64_t mtime;char reserved[32]; } Param;
typedef struct { int32_t user,pad;const DirName *dir;uint64_t blocks;uint32_t mode;char reserved[32];int32_t pad2; } Mount;
typedef struct { Point point;uint64_t required;uint32_t unused,status;char reserved[28];int32_t pad; } MountResult;
typedef struct { int32_t user,pad;const void *title;const DirName *dir;uint32_t key,order;char reserved[32]; } Search;
typedef struct { uint64_t blocks,free_blocks;char reserved[32]; } SearchInfo;
typedef struct { uint32_t hits,pad;DirName *names;uint32_t capacity,count;Param *params;SearchInfo *infos;char reserved[12];int32_t pad2; } Result;
_Static_assert(sizeof(Param)==1328&&sizeof(Mount)==64&&sizeof(MountResult)==64&&sizeof(Result)==56,"SaveData ABI");
static SRWLOCK lock=SRWLOCK_INIT;
static int initialized;
static struct { int active,readonly;char path[1200],meta[1200]; } slots[16];
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x809f0000)|n);}
static int name_valid(const DirName *dir){if(!dir||!memchr(dir->name,0,32)||!dir->name[0])return 0;
    return !strchr(dir->name,'/')&&!strchr(dir->name,'\\')&&!strchr(dir->name,':')&&strcmp(dir->name,".")&&strcmp(dir->name,"..");}
static int root(int user,char *path,size_t size){
    if(user<0)return 0;char base[1200];snprintf(base,sizeof(base),"%s",runtime_save_root());CreateDirectoryA(base,NULL);
    snprintf(path,size,"%s\\%d",base,user);CreateDirectoryA(path,NULL);
    snprintf(path,size,"%s\\%d\\" GAME_TITLE_ID,base,user);return CreateDirectoryA(path,NULL)||GetLastError()==ERROR_ALREADY_EXISTS;
}
static int32_t GUEST_ABI initialize(const void *p){(void)p;char path[1200];if(!root(1,path,sizeof(path)))return err(11);initialized=1;return 0;}
static int32_t GUEST_ABI terminate(void){if(!initialized)return err(1);initialized=0;return 0;}
static int32_t GUEST_ABI mount_save(const Mount *m,MountResult *out){
    if(!initialized)return err(1);if(!m||!out||!name_valid(m->dir))return err(0);if(m->user<0)return err(17);
    char base[1200],path[1200],meta[1200];if(!root(m->user,base,sizeof(base)))return err(11);
    snprintf(path,sizeof(path),"%s\\%s",base,m->dir->name);snprintf(meta,sizeof(meta),"%s\\%s.sce_sys",base,m->dir->name);
    DWORD attributes=GetFileAttributesA(path);int exists=attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY);
    if(exists&&(m->mode&4))return err(7);if(!exists&&!(m->mode&36))return err(8);
    AcquireSRWLockExclusive(&lock);int slot=-1;
    for(int i=0;i<16;++i){if(slots[i].active&&!strcmp(slots[i].path,path)){ReleaseSRWLockExclusive(&lock);return err(13);}if(!slots[i].active&&slot<0)slot=i;}
    if(slot<0){ReleaseSRWLockExclusive(&lock);return err(12);}
    if(!exists&&(!CreateDirectoryA(path,NULL)||!CreateDirectoryA(meta,NULL))){ReleaseSRWLockExclusive(&lock);return err(11);}
    slots[slot].active=1;slots[slot].readonly=!!(m->mode&1);strcpy(slots[slot].path,path);strcpy(slots[slot].meta,meta);
    memset(out,0,sizeof(*out));snprintf(out->point.name,16,"/savedata%d",slot);out->status=!exists;
    runtime_file_mount((unsigned)slot,out->point.name,path,slots[slot].readonly);ReleaseSRWLockExclusive(&lock);
    printf("Mounted save '%s' at %s (%s)\n",m->dir->name,out->point.name,exists?"existing":"created");return 0;
}
static int slot_of(const Point *p){if(!p||!memchr(p->name,0,16))return -1;for(int i=0;i<16;++i){char n[16];snprintf(n,16,"/savedata%d",i);if(slots[i].active&&!strcmp(n,p->name))return i;}return -1;}
static int32_t GUEST_ABI umount_save(const Point *p){AcquireSRWLockExclusive(&lock);int slot=slot_of(p);if(slot>=0){runtime_file_unmount((unsigned)slot);slots[slot].active=0;}ReleaseSRWLockExclusive(&lock);return slot>=0?0:err(4);}
static void read_param(const char *meta,Param *p){memset(p,0,sizeof(*p));char name[1400];snprintf(name,sizeof(name),"%s\\param.bin",meta);FILE *f=fopen(name,"rb");if(f){fread(p,1,sizeof(*p),f);fclose(f);}
    WIN32_FILE_ATTRIBUTE_DATA a;if(GetFileAttributesExA(name,GetFileExInfoStandard,&a)){uint64_t ticks=((uint64_t)a.ftLastWriteTime.dwHighDateTime<<32)|a.ftLastWriteTime.dwLowDateTime;p->mtime=(int64_t)((ticks-UINT64_C(116444736000000000))/10000000);}}
typedef struct {DirName dir;Param param;SearchInfo info;} SearchRow;
static int compare_rows(void *context,const void *left,const void *right){
    const Search *query=context;const SearchRow *a=left,*b=right;int result=0;
    if(query->key==1)result=(a->param.user_param>b->param.user_param)-(a->param.user_param<b->param.user_param);
    else if(query->key==3)result=(a->param.mtime>b->param.mtime)-(a->param.mtime<b->param.mtime);
    else if(query->key==2)result=(a->info.blocks>b->info.blocks)-(a->info.blocks<b->info.blocks);
    else if(query->key==5)result=(a->info.free_blocks>b->info.free_blocks)-(a->info.free_blocks<b->info.free_blocks);
    if(!result)result=strcmp(a->dir.name,b->dir.name);return query->order?-result:result;
}
static int32_t GUEST_ABI set_param(const Point *point,uint32_t type,const void *buf,uint64_t size){
    if(!buf)return err(0);AcquireSRWLockExclusive(&lock);int slot=slot_of(point);if(slot<0){ReleaseSRWLockExclusive(&lock);return err(4);}if(slots[slot].readonly){ReleaseSRWLockExclusive(&lock);return err(0);}
    Param p;read_param(slots[slot].meta,&p);int valid=1;
    switch(type){case 0:if(size<sizeof(p))valid=0;else memcpy(&p,buf,sizeof(p));break;
    case 1:snprintf(p.title,128,"%.*s",(int)(size>127?127:size),(const char *)buf);break;
    case 2:snprintf(p.subtitle,128,"%.*s",(int)(size>127?127:size),(const char *)buf);break;
    case 3:snprintf(p.detail,1024,"%.*s",(int)(size>1023?1023:size),(const char *)buf);break;
    case 4:if(size<4)valid=0;else memcpy(&p.user_param,buf,4);break;
    case 5:if(size<8)valid=0;else memcpy(&p.mtime,buf,8);break;default:valid=0;}
    char name[1400];snprintf(name,sizeof(name),"%s\\param.bin",slots[slot].meta);FILE *f=valid?fopen(name,"wb"):NULL;
    int success=f&&fwrite(&p,1,sizeof(p),f)==sizeof(p);if(f)fclose(f);ReleaseSRWLockExclusive(&lock);return success?0:err(valid?11:0);
}
static int32_t GUEST_ABI search(const Search *query,Result *out){
    if(!initialized)return err(1);if(!query||!out||query->key>5||query->order>1||(out->capacity&&!out->names))return err(0);char base[1200];if(!root(query->user,base,sizeof(base)))return err(11);
    if(query->dir&&(!memchr(query->dir->name,0,32)||strchr(query->dir->name,'/')||strchr(query->dir->name,'\\')||strchr(query->dir->name,':')))return err(0);
    const char *pattern=query->dir&&query->dir->name[0]?query->dir->name:"%";
    char wildcard[1400];snprintf(wildcard,sizeof(wildcard),"%s\\*",base);
    fprintf(stderr,"SAVE_SEARCH user=%d pattern='%s' key=%u order=%u capacity=%u\n",query->user,pattern,query->key,query->order,out->capacity);
    WIN32_FIND_DATAA info;HANDLE h=FindFirstFileA(wildcard,&info);out->hits=out->count=0;if(h==INVALID_HANDLE_VALUE)return 0;
    SearchRow *rows=NULL;size_t capacity=0;
    do{if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!strcmp(info.cFileName,".")||!strcmp(info.cFileName,"..")||strstr(info.cFileName,".sce_sys"))continue;
        if(strlen(info.cFileName)>=32)continue;
        if(!save_pattern_matches(info.cFileName,pattern))continue;
        char meta[1400],metadata[1500];snprintf(meta,sizeof(meta),"%s\\%s.sce_sys",base,info.cFileName);snprintf(metadata,sizeof(metadata),"%s\\param.bin",meta);
        if(GetFileAttributesA(metadata)==INVALID_FILE_ATTRIBUTES)continue;
        if(out->hits==capacity){size_t next=capacity?capacity*2:16;SearchRow *grown=realloc(rows,next*sizeof(*rows));if(!grown){free(rows);FindClose(h);return err(11);}rows=grown;capacity=next;}
        SearchRow *row=&rows[out->hits++];memset(row,0,sizeof(*row));strcpy(row->dir.name,info.cFileName);read_param(meta,&row->param);
        char data_path[1500];snprintf(data_path,sizeof(data_path),"%s\\%s\\dis.dat",base,info.cFileName);WIN32_FILE_ATTRIBUTE_DATA data;
        if(GetFileAttributesExA(data_path,GetFileExInfoStandard,&data))row->info.blocks=((((uint64_t)data.nFileSizeHigh<<32)|data.nFileSizeLow)+32767)/32768;
    }while(FindNextFileA(h,&info));FindClose(h);
    if(out->hits)qsort_s(rows,out->hits,sizeof(*rows),compare_rows,(void *)query);
    out->count=out->hits<out->capacity?out->hits:out->capacity;
    for(uint32_t i=0;i<out->count;++i){out->names[i]=rows[i].dir;if(out->params)out->params[i]=rows[i].param;if(out->infos)out->infos[i]=rows[i].info;
        fprintf(stderr,"SAVE_SEARCH found='%s' index=%u mtime=%lld\n",rows[i].dir.name,i,(long long)rows[i].param.mtime);}
    free(rows);return 0;
}
typedef struct {const void *buffer;uint64_t buffer_size,data_size;uint8_t reserved[32];} Icon;
static int32_t GUEST_ABI save_icon(const Point *point,const Icon *icon){
    if(!initialized)return err(1);if(!icon||!icon->buffer||!icon->data_size||icon->data_size>icon->buffer_size)return err(0);
    AcquireSRWLockExclusive(&lock);int slot=slot_of(point);if(slot<0){ReleaseSRWLockExclusive(&lock);return err(4);}
    if(slots[slot].readonly){ReleaseSRWLockExclusive(&lock);return err(0);}
    CreateDirectoryA(slots[slot].meta,NULL);
    char path[1400],temporary[1400];snprintf(path,sizeof(path),"%s\\icon0.png",slots[slot].meta);snprintf(temporary,sizeof(temporary),"%s\\icon0.pending",slots[slot].meta);
    FILE *f=fopen(temporary,"wb");int ok=f&&fwrite(icon->buffer,1,(size_t)icon->data_size,f)==icon->data_size;if(f){fflush(f);fclose(f);}
    if(ok)ok=MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    ReleaseSRWLockExclusive(&lock);return ok?0:err(11);
}
// ---- save data memory -----------------------------------------------------------------------
// One file per user and slot, <user save root>\sce_sdmemory[slot]\memory.dat (as in shadPS4), with
// param.bin and icon0.png in the matching .sce_sys folder. Held in host memory; every Set and Sync
// writes the file back through a temporary.
typedef struct {void *buf;uint64_t size;int64_t offset;uint8_t reserved[40];} MemoryData;
typedef struct {int32_t user;uint8_t pad[4];MemoryData *data;Param *param;Icon *icon;uint32_t slot;uint8_t reserved[28];} MemoryGet;
typedef struct {int32_t user;uint8_t pad[4];const MemoryData *data;const Param *param;const Icon *icon;uint32_t count,slot;uint8_t reserved[32];} MemorySet;
typedef struct {uint32_t option;int32_t user;uint64_t size,icon_size;const Param *param;const Icon *icon;uint32_t slot;uint8_t reserved[20];} MemorySetup;
typedef struct {uint64_t existed;uint8_t reserved[16];} MemorySetupResult;
typedef struct {int32_t user;uint32_t slot,option;uint8_t reserved[28];} MemorySync;
_Static_assert(sizeof(MemoryData)==64&&sizeof(MemoryGet)==64&&sizeof(MemorySet)==72&&sizeof(MemorySetup)==64,"SaveDataMemory ABI");
enum {MEMORY_SLOTS=4};
static struct {int active,user;uint64_t size;unsigned char *data;char path[1300],meta[1300];} memories[MEMORY_SLOTS];
static int32_t memory_not_ready(void){return err(0x12);} // approximate: memory not set up
static int memory_paths(int user,uint32_t slot,char *path,char *meta,size_t size){
    char base[1200];if(!root(user,base,sizeof(base)))return 0;
    char name[32];if(slot)snprintf(name,sizeof(name),"sce_sdmemory%u",slot);else snprintf(name,sizeof(name),"sce_sdmemory");
    snprintf(path,size,"%s\\%s",base,name);snprintf(meta,size,"%s\\%s.sce_sys",base,name);return 1;
}
static int write_whole(const char *path,const void *data,uint64_t size){
    char temporary[1400];snprintf(temporary,sizeof(temporary),"%s.pending",path);
    FILE *f=fopen(temporary,"wb");int ok=f&&(!size||fwrite(data,1,(size_t)size,f)==size);if(f){fflush(f);fclose(f);}
    return ok&&MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
static int memory_flush(int slot){char file[1400];snprintf(file,sizeof(file),"%s\\memory.dat",memories[slot].path);return write_whole(file,memories[slot].data,memories[slot].size);}
static int memory_slot(int user,uint32_t slot){return slot<MEMORY_SLOTS&&memories[slot].active&&memories[slot].user==user?(int)slot:-1;}
static int32_t GUEST_ABI memory_setup(const MemorySetup *setup,MemorySetupResult *result){
    if(!initialized)return err(1);if(!setup||setup->user<0||!setup->size||setup->slot>=MEMORY_SLOTS)return err(0);
    char path[1300],meta[1300];if(!memory_paths(setup->user,setup->slot,path,meta,sizeof(path)))return err(11);
    AcquireSRWLockExclusive(&lock);uint32_t s=setup->slot;
    if(memories[s].active){ReleaseSRWLockExclusive(&lock);return err(13);} // already set up: busy
    CreateDirectoryA(path,NULL);CreateDirectoryA(meta,NULL);
    unsigned char *data=calloc(1,(size_t)setup->size);if(!data){ReleaseSRWLockExclusive(&lock);return err(11);}
    char file[1400];snprintf(file,sizeof(file),"%s\\memory.dat",path);
    uint64_t existed=0;FILE *f=fopen(file,"rb");
    if(f){_fseeki64(f,0,SEEK_END);existed=(uint64_t)_ftelli64(f);_fseeki64(f,0,SEEK_SET);
        fread(data,1,(size_t)(existed<setup->size?existed:setup->size),f);fclose(f);}
    memories[s].active=1;memories[s].user=setup->user;memories[s].size=setup->size;memories[s].data=data;
    strcpy(memories[s].path,path);strcpy(memories[s].meta,meta);
    int ok=1;
    if(!existed){
        ok=memory_flush((int)s);
        char name[1400];snprintf(name,sizeof(name),"%s\\param.bin",meta);
        Param param;if(setup->param)param=*setup->param;else memset(&param,0,sizeof(param));
        if(ok)ok=write_whole(name,&param,sizeof(param));
        if(ok&&setup->icon&&setup->icon->buffer&&setup->icon->data_size){snprintf(name,sizeof(name),"%s\\icon0.png",meta);ok=write_whole(name,setup->icon->buffer,setup->icon->data_size);}
    }
    ReleaseSRWLockExclusive(&lock);
    if(result){memset(result,0,sizeof(*result));result->existed=existed;}
    printf("Save data memory slot %u: %llu bytes (%s)\n",s,(unsigned long long)setup->size,existed?"existing":"created");
    return ok?0:err(11);
}
static int32_t GUEST_ABI memory_get(MemoryGet *get){
    if(!initialized)return err(1);if(!get)return err(0);
    AcquireSRWLockShared(&lock);int s=memory_slot(get->user,get->slot);
    if(s<0){ReleaseSRWLockShared(&lock);return memory_not_ready();}
    int32_t r=0;
    if(get->data){MemoryData *d=get->data;
        if(!d->buf||d->offset<0||(uint64_t)d->offset>memories[s].size||d->size>memories[s].size-(uint64_t)d->offset)r=err(0);
        else memcpy(d->buf,memories[s].data+d->offset,(size_t)d->size);}
    if(!r&&get->param)read_param(memories[s].meta,get->param);
    if(!r&&get->icon&&get->icon->buffer){char name[1400];snprintf(name,sizeof(name),"%s\\icon0.png",memories[s].meta);
        FILE *f=fopen(name,"rb");Icon *icon=(Icon *)get->icon;icon->data_size=f?fread((void *)icon->buffer,1,(size_t)icon->buffer_size,f):0;if(f)fclose(f);}
    ReleaseSRWLockShared(&lock);return r;
}
static int32_t GUEST_ABI memory_set(const MemorySet *set){
    if(!initialized)return err(1);if(!set||(set->count&&!set->data))return err(0);
    AcquireSRWLockExclusive(&lock);int s=memory_slot(set->user,set->slot);
    if(s<0){ReleaseSRWLockExclusive(&lock);return memory_not_ready();}
    for(uint32_t i=0;i<set->count;++i){const MemoryData *d=&set->data[i];
        if(!d->buf||d->offset<0||(uint64_t)d->offset>memories[s].size||d->size>memories[s].size-(uint64_t)d->offset){ReleaseSRWLockExclusive(&lock);return err(0);}}
    for(uint32_t i=0;i<set->count;++i)memcpy(memories[s].data+set->data[i].offset,set->data[i].buf,(size_t)set->data[i].size);
    int ok=memory_flush(s);char name[1400];
    if(ok&&set->param){snprintf(name,sizeof(name),"%s\\param.bin",memories[s].meta);ok=write_whole(name,set->param,sizeof(*set->param));}
    if(ok&&set->icon&&set->icon->buffer&&set->icon->data_size){snprintf(name,sizeof(name),"%s\\icon0.png",memories[s].meta);ok=write_whole(name,set->icon->buffer,set->icon->data_size);}
    ReleaseSRWLockExclusive(&lock);return ok?0:err(11);
}
static int32_t GUEST_ABI memory_sync(const MemorySync *sync){
    if(!initialized)return err(1);if(!sync)return err(0);
    AcquireSRWLockExclusive(&lock);int s=memory_slot(sync->user,sync->slot);
    int ok=s>=0&&memory_flush(s);ReleaseSRWLockExclusive(&lock);return s<0?memory_not_ready():ok?0:err(11);
}
// ---- delete, transfer ------------------------------------------------------------------------
typedef struct {int32_t user,pad;const char *title;const DirName *dir;uint32_t unused;uint8_t reserved[32];int32_t pad2;} Delete;
static int remove_tree(const char *path){
    char pattern[1400];snprintf(pattern,sizeof(pattern),"%s\\*",path);WIN32_FIND_DATAA f;HANDLE h=FindFirstFileA(pattern,&f);
    if(h!=INVALID_HANDLE_VALUE){do{if(!strcmp(f.cFileName,".")||!strcmp(f.cFileName,".."))continue;char child[1400];snprintf(child,sizeof(child),"%s\\%s",path,f.cFileName);
        if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)remove_tree(child);else DeleteFileA(child);}while(FindNextFileA(h,&f));FindClose(h);}
    return RemoveDirectoryA(path)||GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND;
}
static int32_t GUEST_ABI delete_save(const Delete *d){
    if(!initialized)return err(1);if(!d||d->user<0||!name_valid(d->dir))return err(0);
    if(d->title&&d->title[0]&&strncmp(d->title,GAME_TITLE_ID,9))return err(8); // other titles' saves are not here
    char base[1200],path[1300],meta[1300];if(!root(d->user,base,sizeof(base)))return err(11);
    snprintf(path,sizeof(path),"%s\\%s",base,d->dir->name);snprintf(meta,sizeof(meta),"%s\\%s.sce_sys",base,d->dir->name);
    AcquireSRWLockExclusive(&lock);
    for(int i=0;i<16;++i)if(slots[i].active&&!strcmp(slots[i].path,path)){ReleaseSRWLockExclusive(&lock);return err(13);}
    DWORD a=GetFileAttributesA(path);
    if(a==INVALID_FILE_ATTRIBUTES||!(a&FILE_ATTRIBUTE_DIRECTORY)){ReleaseSRWLockExclusive(&lock);return err(8);}
    int ok=remove_tree(path)&&remove_tree(meta);ReleaseSRWLockExclusive(&lock);
    printf("Deleted save '%s'\n",d->dir->name);return ok?0:err(11);
}
// Saves of another title (PS4 to PS5 transfers): none exist on this PC.
static int32_t GUEST_ABI transferring_mount(const void *mount,void *result){(void)result;if(!initialized)return err(1);if(!mount)return err(0);return err(8);}
// api_names.h only names symbols this title imports. CUSA08014 imports six of these thirteen
// (Initialize3, Terminate, Mount2, Umount, SetParam, Delete). DirNameSearch, SaveIcon, the four
// save-memory calls and TransferringMount are implemented here, and save_search_test asks for
// DirNameSearch by NID, but a name lookup returns "" for anything absent from the import table,
// so those handlers were unreachable from the game and from the test. The NID is the first field
// of the identity and does not depend on that table.
static int nid_is(const char *identity,const char *nid){
    size_t n=strlen(nid);return !strncmp(identity,nid,n)&&identity[n]=='#';
}
uintptr_t save_resolve(const char *identity){
    if(!strstr(identity,"#libSceSaveData#1#libSceSaveData#257"))return 0;
#define API(nid,f) if(nid_is(identity,nid))return (uintptr_t)f
    API("TywrFKCoLGY",initialize);API("yKDy8S5yLA0",terminate);API("0z45PIH+SNI",mount_save);
    API("BMR4F-Uek3E",umount_save);API("85zul--eGXs",set_param);API("dyIhnXq-0SM",search);
    API("c88Yy54Mx0w",save_icon);
    API("oQySEUfgXRA",memory_setup);API("QwOO7vegnV8",memory_get);
    API("cduy9v4YmT4",memory_set);API("wiT9jeC7xPw",memory_sync);
    API("S1GkePI17zQ",delete_save);API("WAzWTZm1H+I",transferring_mount);
#undef API
    return 0;
}
