// SPDX-License-Identifier: GPL-2.0-or-later
// PS4 read-only app mounts and workspace-only writable runtime data.
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int64_t sec, nsec; } Timestamp;
typedef struct {
    uint32_t dev,ino; uint16_t mode,nlink; uint32_t uid,gid,rdev;
    Timestamp atime,mtime,ctime; int64_t size,blocks;
    uint32_t blksize,flags,gen; int32_t spare; Timestamp birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"PS4 stat layout");
typedef struct { HANDLE host; SRWLOCK lock; int active; uint64_t entries_read; char path[1024]; } File;
static SRWLOCK log_lock = SRWLOCK_INIT;
static FILE *io_log;
static void trace_io(const char *op,int fd,const char *path,int64_t offset,uint64_t requested,uint64_t completed,DWORD native,int err) {
    AcquireSRWLockExclusive(&log_lock);
    if(!io_log){const char *p=getenv("ISS_IO_LOG");if(p)io_log=fopen(p,"a");}
    if(io_log){fprintf(io_log,"%llu\t%lu\t%s\t%d\t%lld\t%llu\t%llu\t%lu\t%d\t%s\n",
        (unsigned long long)GetTickCount64(),GetCurrentThreadId(),op,fd,(long long)offset,
        (unsigned long long)requested,(unsigned long long)completed,native,err,path?path:"");fflush(io_log);}
    ReleaseSRWLockExclusive(&log_lock);
}
static File files[4096];
static SRWLOCK registry = SRWLOCK_INIT;
static INIT_ONCE initialize_once = INIT_ONCE_STATIC_INIT;
static char app_root[1024],user_root[1024],save_root[1200];
static struct { char guest[32],host[1200]; int active,readonly; } mounts[16];
static int32_t error(int n) { return (int32_t)(UINT32_C(0x80020000)|(unsigned)n); }
static int windows_error(void) {
    switch(GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return 2;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: return 13;
    case ERROR_INVALID_HANDLE: return 9;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return 17;
    case ERROR_DISK_FULL: return 28;
    default: return 5;
    }
}
static BOOL CALLBACK initialize(PINIT_ONCE o,void *p,void **c) {
    (void)o;(void)p;(void)c;
    const char *game=getenv("ISS_GAME_DIR");
    snprintf(app_root,sizeof(app_root),"%s",game?game:".");
    GetModuleFileNameA(NULL,user_root,sizeof(user_root));
    char *slash=strrchr(user_root,'\\'); if(!slash) return FALSE;
    strcpy(slash,"\\user"); CreateDirectoryA(user_root,NULL);
    const char *save_override=getenv("ISS_SAVE_ROOT");
    if(save_override&&*save_override)snprintf(save_root,sizeof(save_root),"%s",save_override);
    else snprintf(save_root,sizeof(save_root),"%s\\savedata",user_root);
    const char *names[]={"temp0","download0","data","savedata0"};
    for(unsigned i=0;i<4;++i) { char path[1100];snprintf(path,sizeof(path),"%s\\%s",user_root,names[i]);CreateDirectoryA(path,NULL); }
    return TRUE;
}
static int translate(const char *path,char *out,size_t size,int write) {
    if(!path||!*path) return 2;
    if(!InitOnceExecuteOnce(&initialize_once,initialize,NULL,NULL)) return 5;
    char normalized[1024];
    int n=snprintf(normalized,sizeof(normalized),"%s%s",path[0]=='/'?"":"/app0/",path);
    if(n<0||(size_t)n>=sizeof(normalized)) return 63;
    if(strchr(normalized,'\\')||strchr(normalized,':')) return 13;
    for(char *p=normalized;*p;++p)
        if((p==normalized||p[-1]=='/')&&p[0]=='.'&&p[1]=='.'&&(p[2]=='/'||!p[2])) return 13;
    const char *suffix=NULL,*base=NULL; char writable[1100];
    for(unsigned i=0;i<16;++i)if(mounts[i].active){size_t length=strlen(mounts[i].guest);
        if(!strncmp(normalized,mounts[i].guest,length)&&(normalized[length]=='/'||!normalized[length])){
            if(write&&mounts[i].readonly)return 30;suffix=normalized+length;base=mounts[i].host;break;}}
    const char *mounts[]={"/app0","/hostapp","/temp0","/download0","/data","/savedata0"};
    for(unsigned i=0;!base&&i<6;++i) {
        size_t len=strlen(mounts[i]);
        if(!strncmp(normalized,mounts[i],len)&&(normalized[len]=='/'||!normalized[len])) {
            suffix=normalized+len;
            if(i<2) { if(write) return 30; base=app_root; }
            else {snprintf(writable,sizeof(writable),"%s\\%s",user_root,mounts[i]+1);base=writable;}
            break;
        }
    }
    if(!base) return 2;
    n=snprintf(out,size,"%s%s",base,suffix);
    if(n<0||(size_t)n>=size) return 63;
    for(char *p=out;*p;++p) if(*p=='/') *p='\\';
    return 0;
}
int runtime_file_translate(const char *guest,char *host,size_t size,int write){return translate(guest,host,size,write);}
const char *runtime_user_root(void){InitOnceExecuteOnce(&initialize_once,initialize,NULL,NULL);return user_root;}
const char *runtime_save_root(void){InitOnceExecuteOnce(&initialize_once,initialize,NULL,NULL);return save_root;}
int runtime_file_mount(unsigned slot,const char *guest,const char *host,int readonly){
    if(slot>=16||!guest||strlen(guest)>=32||!host||strlen(host)>=1200)return -1;
    AcquireSRWLockExclusive(&registry);strcpy(mounts[slot].guest,guest);strcpy(mounts[slot].host,host);
    mounts[slot].readonly=readonly;mounts[slot].active=1;ReleaseSRWLockExclusive(&registry);return 0;
}
void runtime_file_unmount(unsigned slot){if(slot>=16)return;AcquireSRWLockExclusive(&registry);mounts[slot].active=0;ReleaseSRWLockExclusive(&registry);}
static int32_t GUEST_ABI open_file(const char *path,int32_t flags,uint16_t mode) {
    (void)mode; char translated[2048]; int writing=(flags&3)|| (flags&0x600);
    int e=translate(path,translated,sizeof(translated),writing); if(e) return error(e);
    DWORD access=(flags&3)==0?GENERIC_READ:(flags&3)==1?GENERIC_WRITE:GENERIC_READ|GENERIC_WRITE;
    DWORD disposition=(flags&0x200)?((flags&0x800)?CREATE_NEW:(flags&0x400)?CREATE_ALWAYS:OPEN_ALWAYS):
                       (flags&0x400)?TRUNCATE_EXISTING:OPEN_EXISTING;
    HANDLE host=CreateFileA(translated,access,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,disposition,
                            (flags&0x20000)?FILE_FLAG_BACKUP_SEMANTICS:FILE_ATTRIBUTE_NORMAL,NULL);
    if(host==INVALID_HANDLE_VALUE) {DWORD native=GetLastError();e=windows_error();trace_io("open",-1,path,0,0,0,native,e);printf("File open failed: %s errno=%d\n",path,e);return error(e);}
    AcquireSRWLockExclusive(&registry);
    int fd;
    for(fd=3;fd<4096&&files[fd].active;++fd){}
    if(fd==4096){ReleaseSRWLockExclusive(&registry);CloseHandle(host);return error(24);}
    files[fd].host=host;files[fd].active=1;files[fd].entries_read=0;InitializeSRWLock(&files[fd].lock);
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",path);
    trace_io("open",fd,path,0,0,0,0,0);
    if(flags&8){LARGE_INTEGER zero={0};SetFilePointerEx(host,zero,NULL,FILE_END);}
    ReleaseSRWLockExclusive(&registry);printf("Opened %s fd=%d\n",path,fd);
    {size_t n=strlen(path);if(n>5&&!strcmp(path+n-5,".xpps"))gpu_file_opened(path);}
    return fd;
}
static File *acquire_file(int fd) {
    AcquireSRWLockShared(&registry);
    if(fd<3||fd>=4096||!files[fd].active){ReleaseSRWLockShared(&registry);return NULL;}
    AcquireSRWLockExclusive(&files[fd].lock);return &files[fd];
}
static void release_file(File *f){ReleaseSRWLockExclusive(&f->lock);ReleaseSRWLockShared(&registry);}
static int32_t GUEST_ABI close_file(int fd) {
    AcquireSRWLockExclusive(&registry);
    if(fd<3||fd>=4096||!files[fd].active){ReleaseSRWLockExclusive(&registry);return error(9);}
    trace_io("close",fd,files[fd].path,0,0,0,0,0);
    CloseHandle(files[fd].host);files[fd].active=0;ReleaseSRWLockExclusive(&registry);return 0;
}
static int64_t transfer(int fd,void *buffer,uint64_t size,int write,int64_t offset,int positioned) {
    if(write && (fd==1||fd==2)) {
        uint64_t total=0;HANDLE h=GetStdHandle(fd==1?STD_OUTPUT_HANDLE:STD_ERROR_HANDLE);
        while(total<size){DWORD n=(DWORD)((size-total)>0x40000000?0x40000000:size-total),done=0;
            if(!WriteFile(h,(unsigned char *)buffer+total,n,&done,NULL))return total?(int64_t)total:error(windows_error());
            total+=done;if(!done)break;}return (int64_t)total;
    }
    File *f=acquire_file(fd);if(!f)return error(9);
    LARGE_INTEGER previous={0},move={0};
    if(positioned){SetFilePointerEx(f->host,move,&previous,FILE_CURRENT);move.QuadPart=offset;
        if(offset<0||!SetFilePointerEx(f->host,move,NULL,FILE_BEGIN)){release_file(f);return error(22);}}
    LARGE_INTEGER start={0},zero={0};SetFilePointerEx(f->host,zero,&start,FILE_CURRENT);
    uint64_t total=0;int e=0;DWORD native_error=0;
    // Windows kernel I/O cannot invoke the user-mode GPU tracking handler.
    // Stage through host memory, then copy in user mode, where faults are handled.
    // Pre-touching is insufficient: the GPU can protect a page again before I/O.
    size_t capacity=(size_t)(size>1048576?1048576:size);
    unsigned char *staging=capacity?malloc(capacity):NULL;
    if(capacity&&!staging)e=12;
    while(!e&&total<size){DWORD amount=(DWORD)((size-total)>capacity?capacity:size-total),done=0;
        if(write)memcpy(staging,(unsigned char *)buffer+total,amount);
        BOOL ok=write?WriteFile(f->host,staging,amount,&done,NULL):
                      ReadFile(f->host,staging,amount,&done,NULL);
        if(!ok){native_error=GetLastError();e=windows_error();fprintf(stderr,"File transfer failed: fd=%d write=%d bytes=%lu offset=%lld destination=%p winerror=%lu\n",fd,write,amount,(long long)offset,(unsigned char *)buffer+total,native_error);break;}
        // The GPU tracking of the destination invalidated at once before the copy (else one
        // fault per 4 KB page into tracked textures; a page protected again meanwhile still
        // faults and is handled as before).
        if(!write&&done){gpu_invalidate((uint64_t)(uintptr_t)((unsigned char *)buffer+total),done);
            memcpy((unsigned char *)buffer+total,staging,done);}
        total+=done;if(done<amount)break;
    }
    free(staging);
    if(positioned)SetFilePointerEx(f->host,previous,NULL,FILE_BEGIN);
    trace_io(write?"write":"read",fd,f->path,start.QuadPart,size,total,native_error,e);
    release_file(f);return total?(int64_t)total:e?error(e):0;
}
static int64_t GUEST_ABI read_file(int fd,void *p,uint64_t n){return transfer(fd,p,n,0,0,0);}
static int64_t GUEST_ABI write_file(int fd,const void *p,uint64_t n){return transfer(fd,(void *)p,n,1,0,0);}
static int64_t GUEST_ABI pread_file(int fd,void *p,uint64_t n,int64_t off){return transfer(fd,p,n,0,off,1);}
static int64_t GUEST_ABI pwrite_file(int fd,const void *p,uint64_t n,int64_t off){return transfer(fd,(void *)p,n,1,off,1);}
static int32_t GUEST_ABI fsync_file(int fd){File *f=acquire_file(fd);if(!f)return error(9);BOOL ok=FlushFileBuffers(f->host);int e=ok?0:windows_error();release_file(f);return e?error(e):0;}
static int32_t GUEST_ABI ftruncate_file(int fd,int64_t size){
    if(size<0)return error(22);File *f=acquire_file(fd);if(!f)return error(9);LARGE_INTEGER zero={0},old={0},end;end.QuadPart=size;
    BOOL ok=SetFilePointerEx(f->host,zero,&old,FILE_CURRENT)&&SetFilePointerEx(f->host,end,NULL,FILE_BEGIN)&&SetEndOfFile(f->host);
    int e=ok?0:windows_error();SetFilePointerEx(f->host,old,NULL,FILE_BEGIN);release_file(f);return e?error(e):0;
}
static int32_t GUEST_ABI truncate_file(const char *path,int64_t size){int fd=open_file(path,1,0);if(fd<0)return fd;int r=ftruncate_file(fd,size);close_file(fd);return r;}
static int32_t GUEST_ABI mkdir_file(const char *path,uint16_t mode){(void)mode;char host[2048];int e=translate(path,host,sizeof(host),1);if(e)return error(e);return CreateDirectoryA(host,NULL)?0:error(windows_error());}
static int32_t GUEST_ABI unlink_file(const char *path){char host[2048];int e=translate(path,host,sizeof(host),1);if(e)return error(e);return DeleteFileA(host)?0:error(windows_error());}
static int32_t GUEST_ABI rename_file(const char *from,const char *to){char a[2048],b[2048];int e=translate(from,a,sizeof(a),1);if(e)return error(e);e=translate(to,b,sizeof(b),1);if(e)return error(e);return MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?0:error(windows_error());}
// Directory entries in FreeBSD's struct dirent: fileno, reclen, type, namlen, name. Each call
// enumerates the directory again and skips the entries already returned on this descriptor.
static int64_t directory_entries(int fd,char *buffer,uint64_t size,int64_t *base){
    if(!buffer)return error(14);
    File *f=acquire_file(fd);if(!f)return error(9);
    char host[2048],pattern[2100];int e=translate(f->path,host,sizeof(host),0);
    if(e){release_file(f);return error(e);}
    snprintf(pattern,sizeof(pattern),"%s\\*",host);
    WIN32_FIND_DATAA found;HANDLE h=FindFirstFileA(pattern,&found);
    if(h==INVALID_HANDLE_VALUE){e=windows_error();release_file(f);return error(e==2?20:e);}
    uint64_t index=0,written=0;
    do{
        if(index++<f->entries_read)continue;
        size_t n=strlen(found.cFileName);
        if(n>255){++f->entries_read;continue;}
        uint16_t length=(uint16_t)((8+n+1+3)&~(size_t)3);
        if(written+length>size)break;
        char *out=buffer+written;uint32_t number=(uint32_t)index;uint8_t type=(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?4:8,namlen=(uint8_t)n;
        memcpy(out,&number,4);memcpy(out+4,&length,2);out[6]=(char)type;out[7]=(char)namlen;
        memset(out+8,0,length-8);memcpy(out+8,found.cFileName,n);
        written+=length;++f->entries_read;
    }while(FindNextFileA(h,&found));
    FindClose(h);
    uint64_t consumed=f->entries_read;
    release_file(f);
    if(base)*base=(int64_t)consumed;
    if(!written&&index>consumed)return error(22); // buffer smaller than one entry
    return (int64_t)written;
}
static int64_t GUEST_ABI get_dents(int fd,char *buffer,uint64_t size){return directory_entries(fd,buffer,size,NULL);}
static int64_t GUEST_ABI get_direntries(int fd,char *buffer,uint64_t size,int64_t *base){return directory_entries(fd,buffer,size,base);}
static int32_t GUEST_ABI rmdir_file(const char *path){char host[2048];int e=translate(path,host,sizeof(host),1);if(e)return error(e);return RemoveDirectoryA(host)?0:error(windows_error());}
// Windows has no POSIX modes or settable times worth mirroring; the path only has to exist.
static int32_t GUEST_ABI exists_file(const char *path){char host[2048];int e=translate(path,host,sizeof(host),0);if(e)return error(e);return GetFileAttributesA(host)!=INVALID_FILE_ATTRIBUTES?0:error(windows_error());}
static int32_t GUEST_ABI chmod_file(const char *path,uint32_t mode){(void)mode;return exists_file(path);}
static int32_t GUEST_ABI utimes_file(const char *path,const void *times){(void)times;return exists_file(path);}
static int posix_result(int64_t r){if(r<0){*runtime_errno()=(int32_t)r&65535;return -1;}return 0;}
static int GUEST_ABI posix_fsync(int fd){return posix_result(fsync_file(fd));}
static int GUEST_ABI posix_rmdir(const char *path){return posix_result(rmdir_file(path));}
static int64_t GUEST_ABI posix_write(int fd,const void *p,uint64_t n){int64_t r=write_file(fd,p,n);if(r<0){*runtime_errno()=(int32_t)r&65535;return -1;}return r;}
static int64_t GUEST_ABI posix_read(int fd,void *p,uint64_t n){int64_t r=read_file(fd,p,n);if(r<0){*runtime_errno()=(int32_t)r&65535;return -1;}return r;}
static int64_t GUEST_ABI lseek_file(int fd,int64_t off,int origin){
    if(origin<0||origin>2)return error(22);File *f=acquire_file(fd);if(!f)return error(9);
    LARGE_INTEGER move,out;move.QuadPart=off;BOOL ok=SetFilePointerEx(f->host,move,&out,(DWORD)origin);
    int e=ok?0:windows_error();release_file(f);return ok?out.QuadPart:error(e);
}
static Timestamp timestamp(FILETIME ft){uint64_t n=(((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime)-UINT64_C(116444736000000000);return (Timestamp){(int64_t)(n/10000000),(int64_t)((n%10000000)*100)};}
static int fill_stat(HANDLE h,GuestStat *s){
    if(!s)return 14;BY_HANDLE_FILE_INFORMATION i;if(!GetFileInformationByHandle(h,&i))return windows_error();
    memset(s,0,sizeof(*s));s->mode=(uint16_t)((i.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY?0040000:0100000)|0444);
    s->ino=i.nFileIndexLow;s->dev=i.dwVolumeSerialNumber;s->nlink=(uint16_t)i.nNumberOfLinks;
    s->size=(int64_t)(((uint64_t)i.nFileSizeHigh<<32)|i.nFileSizeLow);s->blocks=(s->size+511)/512;s->blksize=4096;
    s->atime=timestamp(i.ftLastAccessTime);s->mtime=timestamp(i.ftLastWriteTime);s->ctime=s->birthtime=timestamp(i.ftCreationTime);return 0;
}
static int32_t GUEST_ABI fstat_file(int fd,GuestStat *s){File *f=acquire_file(fd);if(!f)return error(9);int e=fill_stat(f->host,s);release_file(f);return e?error(e):0;}
static int32_t GUEST_ABI stat_file(const char *p,GuestStat *s){int fd=open_file(p,0,0);if(fd<0)return fd;int r=fstat_file(fd,s);close_file(fd);return r;}
uintptr_t file_resolve(const char *identity){
    if(!strstr(identity,"#libkernel#1#libkernel#257")&&!strstr(identity,"#libScePosix#1#libkernel#257"))return 0;
    const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceKernelGetdents",get_dents);API("sceKernelGetdirentries",get_direntries);API("sceKernelRmdir",rmdir_file);
    API("sceKernelChmod",chmod_file);API("sceKernelUtimes",utimes_file);API("fsync",posix_fsync);API("rmdir",posix_rmdir);
    API("sceKernelOpen",open_file);API("sceKernelClose",close_file);API("sceKernelRead",read_file);
    API("sceKernelWrite",write_file);API("sceKernelPread",pread_file);API("sceKernelLseek",lseek_file);
    API("sceKernelFstat",fstat_file);API("sceKernelStat",stat_file);
    API("sceKernelPwrite",pwrite_file);API("sceKernelFsync",fsync_file);
    API("sceKernelFtruncate",ftruncate_file);API("sceKernelTruncate",truncate_file);API("sceKernelMkdir",mkdir_file);
    API("sceKernelUnlink",unlink_file);API("sceKernelRename",rename_file);
    API("_write",posix_write);API("write",posix_write);API("_read",posix_read);API("read",posix_read);
#undef API
    return 0;
}
