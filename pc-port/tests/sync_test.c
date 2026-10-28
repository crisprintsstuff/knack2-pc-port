// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL sync line %d: %s\n",__LINE__,#x);ExitProcess(1);}}while(0)
typedef int32_t (GUEST_ABI *Unary)(void **);
typedef int32_t (GUEST_ABI *Init)(void **,void **,const char *);
typedef int32_t (GUEST_ABI *SetType)(void **,int);
typedef int32_t (GUEST_ABI *Wait)(void **,void **);
typedef int32_t (GUEST_ABI *TimedWait)(void **,void **,uint64_t);
typedef void *(GUEST_ABI *Entry)(void *);
typedef int32_t (GUEST_ABI *Create)(void **,void **,Entry,void *,const char *);
typedef int32_t (GUEST_ABI *Join)(void *,void **);
static Unary mutex_lock,mutex_unlock,mutex_try,cond_signal;
static Wait cond_wait;
static void *counter_mutex,*condition;
static int counter,ready;
static unsigned char fixture[8192];
static void *GUEST_ABI increment(void *data){
    (void)data;
    CHECK(mutex_lock(&counter_mutex)==0);ready++;CHECK(cond_signal(&condition)==0);CHECK(mutex_unlock(&counter_mutex)==0);
    for(int i=0;i<2000;++i){CHECK(mutex_lock(&counter_mutex)==0);counter++;CHECK(mutex_unlock(&counter_mutex)==0);}return (void *)0x1234;
}
static DWORD WINAPI ownership(void *arg){(void)arg;CHECK(mutex_try(&counter_mutex)==(int32_t)UINT32_C(0x80020010));CHECK(mutex_unlock(&counter_mutex)==(int32_t)UINT32_C(0x80020001));return 0;}
static uintptr_t fn(const char *id){uintptr_t f=runtime_resolve(id,0);CHECK(f);return f;}
int main(void){
    GuestModule module={0,sizeof(fixture),0,4096,0,48,8,1,0};runtime_configure(fixture,sizeof(fixture),&module,1,1024);
    Unary attr_init=(Unary)fn("F8bUHwAG284#libkernel#1#libkernel#257");
    Unary attr_destroy=(Unary)fn("smWEktiyyG0#libkernel#1#libkernel#257");
    SetType set_type=(SetType)fn("iMp8QpE+XO4#libkernel#1#libkernel#257");
    Init init=(Init)fn("cmo1RIYva9o#libkernel#1#libkernel#257");
    mutex_lock=(Unary)fn("9UK1vLZQft4#libkernel#1#libkernel#257");
    mutex_unlock=(Unary)fn("tn3VlD0hG60#libkernel#1#libkernel#257");mutex_try=(Unary)fn("upoVrzMHFeE#libkernel#1#libkernel#257");
    Unary destroy=(Unary)fn("2Of0f+3mhhE#libkernel#1#libkernel#257");
    void *attr=NULL,*m=NULL;CHECK(attr_init(&attr)==0);CHECK(set_type(&attr,2)==0);CHECK(init(&m,&attr,"recursive")==0);
    CHECK(mutex_lock(&m)==0&&mutex_lock(&m)==0);CHECK(mutex_unlock(&m)==0);CHECK(destroy(&m)!=0);CHECK(mutex_unlock(&m)==0);CHECK(destroy(&m)==0);CHECK(mutex_lock(&m)!=0);
    CHECK(set_type(&attr,1)==0);CHECK(init(&m,&attr,"errorcheck")==0);CHECK(mutex_lock(&m)==0);CHECK(mutex_lock(&m)==(int32_t)UINT32_C(0x8002000b));CHECK(mutex_unlock(&m)==0);CHECK(destroy(&m)==0);CHECK(attr_destroy(&attr)==0);
    CHECK(attr_init(&attr)==0);CHECK(set_type(&attr,4)==0);CHECK(init(&m,&attr,"adaptive")==0);
    CHECK(mutex_lock(&m)==0);CHECK(mutex_lock(&m)==(int32_t)UINT32_C(0x8002000b));CHECK(mutex_unlock(&m)==0);CHECK(destroy(&m)==0);CHECK(attr_destroy(&attr)==0);
    CHECK(init(&counter_mutex,NULL,"counter")==0);CHECK(mutex_lock(&counter_mutex)==0);
    HANDLE check=CreateThread(NULL,0,ownership,NULL,0,NULL);CHECK(WaitForSingleObject(check,5000)==WAIT_OBJECT_0);CloseHandle(check);CHECK(mutex_unlock(&counter_mutex)==0);
    Init cond_init=(Init)fn("2Tb92quprl0#libkernel#1#libkernel#257");CHECK(cond_init(&condition,NULL,"wake")==0);
    cond_signal=(Unary)fn("kDh-NfxgMtE#libkernel#1#libkernel#257");cond_wait=(Wait)fn("WKAXJ4XBPQ4#libkernel#1#libkernel#257");
    TimedWait timed=(TimedWait)fn("BmMjYxmew1w#libkernel#1#libkernel#257");
    CHECK(mutex_lock(&counter_mutex)==0);CHECK(timed(&condition,&counter_mutex,1000)==(int32_t)UINT32_C(0x8002003c));CHECK(mutex_unlock(&counter_mutex)==0);
    Create create=(Create)fn("6UgtwV+0zb4#libkernel#1#libkernel#257");Join join=(Join)fn("onNY9Byn-W8#libkernel#1#libkernel#257");
    void *threads[4];for(int i=0;i<4;++i)CHECK(create(&threads[i],NULL,increment,NULL,"sync-test")==0);
    CHECK(mutex_lock(&counter_mutex)==0);while(ready<4)CHECK(cond_wait(&condition,&counter_mutex)==0);CHECK(mutex_unlock(&counter_mutex)==0);
    for(int i=0;i<4;++i){void *result=NULL;CHECK(join(threads[i],&result)==0&&result==(void *)0x1234);}CHECK(counter==8000);
    CHECK(destroy(&counter_mutex)==0);runtime_tls_detach();puts("PASS: mutex recursion/errors/ownership, condition wake/timeout, four guest threads and joins.");return 0;
}
