// SPDX-License-Identifier: GPL-2.0-or-later
// Native WinHTTP resource lifecycle; selected runtime profile remains offline.
#include "runtime.h"
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct {int active,kind,parent;void *arena;HINTERNET handle;wchar_t agent[256];int chunked;} Object;
static Object objects[512];static SRWLOCK lock=SRWLOCK_INIT;static int next=1;
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80431000)|n);}
static int allocate(int kind,int parent,HINTERNET h,void *arena){AcquireSRWLockExclusive(&lock);if(next>=512){ReleaseSRWLockExclusive(&lock);return -1;}int id=next++;objects[id]=(Object){.active=1,.kind=kind,.parent=parent,.arena=arena,.handle=h};ReleaseSRWLockExclusive(&lock);return id;}
static Object *object(int id,int kind){return id>0&&id<512&&objects[id].active&&(!kind||objects[id].kind==kind)?&objects[id]:NULL;}
static int wide(const char *src,wchar_t *dst,int n){return src&&MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,src,-1,dst,n)>0;}
static int32_t GUEST_ABI ssl_init(uint64_t size){if(!size||size>0x40000000)return (int32_t)UINT32_C(0x80435022);void *p=malloc((size_t)size);if(!p)return (int32_t)UINT32_C(0x8043500c);int id=allocate(1,0,NULL,p);if(id<0){free(p);return (int32_t)UINT32_C(0x8043500c);}return id;}
static int32_t GUEST_ABI release(int id){Object *o=object(id,0);if(!o)return err(22);if(o->handle)WinHttpCloseHandle(o->handle);free(o->arena);o->active=0;return 0;}
static int32_t GUEST_ABI http_init(int net,int ssl,uint64_t size){(void)net;if(!object(ssl,1)||!size||size>0x40000000)return err(22);void *p=malloc((size_t)size);if(!p)return err(12);HINTERNET h=WinHttpOpen(L"PS4 PC runtime",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!h){free(p);return err(99);}int id=allocate(2,ssl,h,p);if(id<0){WinHttpCloseHandle(h);free(p);return err(12);}return id;}
static int32_t GUEST_ABI create_template(int ctx,const char *agent,int version,int proxy){(void)proxy;if(!object(ctx,2)||!agent||version<1||version>2)return err(22);int id=allocate(3,ctx,NULL,NULL);if(id<0)return err(12);if(!wide(agent,objects[id].agent,256)){objects[id].active=0;return err(22);}return id;}
static int32_t GUEST_ABI create_connection(int tid,const char *url,int keep){(void)keep;Object *t=object(tid,3);if(!t)return err(22);Object *ctx=object(t->parent,2);if(!ctx)return err(22);wchar_t text[4096],hostname[1024];if(!wide(url,text,4096))return err(22);URL_COMPONENTS parts={0};parts.dwStructSize=sizeof(parts);parts.lpszHostName=hostname;parts.dwHostNameLength=1024;if(!WinHttpCrackUrl(text,0,0,&parts))return err(22);HINTERNET h=WinHttpConnect(ctx->handle,hostname,parts.nPort,0);if(!h)return err(99);int id=allocate(4,tid,h,NULL);if(id<0){WinHttpCloseHandle(h);return err(12);}return id;}
static int32_t GUEST_ABI create_request(int cid,int method,const char *url,uint64_t length){(void)length;Object *c=object(cid,4);if(!c||method<0||method>7)return err(22);static const wchar_t *methods[]={L"GET",L"POST",L"HEAD",L"OPTIONS",L"PUT",L"DELETE",L"TRACE",L"CONNECT"};wchar_t text[4096],path[4096];if(!wide(url,text,4096))return err(22);URL_COMPONENTS parts={0};parts.dwStructSize=sizeof(parts);parts.lpszUrlPath=path;parts.dwUrlPathLength=4096;if(!WinHttpCrackUrl(text,0,0,&parts))return err(22);HINTERNET h=WinHttpOpenRequest(c->handle,methods[method],path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);if(!h)return err(99);int id=allocate(5,cid,h,NULL);if(id<0){WinHttpCloseHandle(h);return err(12);}return id;}
static int32_t GUEST_ABI send_request(int id,const void *data,uint64_t size){(void)data;(void)size;return object(id,5)?err(99):err(22);}
static int32_t GUEST_ABI status_code(int id,int *code){return object(id,5)&&code?err(99):err(22);}
static int32_t GUEST_ABI read_data(int id,void *data,uint64_t size){(void)size;return object(id,5)&&data?err(99):err(22);}
static int32_t GUEST_ABI response_headers(int id,char **headers,uint64_t *size){return object(id,5)&&headers&&size?err(99):err(22);}
static int32_t GUEST_ABI timeout_setting(int id,uint32_t us){Object *o=object(id,0);if(!o)return err(22);while(o&&!o->handle)o=object(o->parent,0);if(!o)return err(22);int ms=(int)(us/1000);return WinHttpSetTimeouts(o->handle,ms,ms,ms,ms)?0:err(22);}
static int32_t GUEST_ABI chunked(int id,int enabled){Object *o=object(id,0);if(!o)return err(22);o->chunked=!!enabled;return 0;}
static int32_t GUEST_ABI add_header(int id,const char *name,const char *value,uint32_t mode){Object *o=object(id,5);if(!o||!name||!value)return err(22);char text[4096];snprintf(text,sizeof(text),"%s: %s",name,value);wchar_t line[4096];if(!wide(text,line,4096))return err(22);return WinHttpAddRequestHeaders(o->handle,line,(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD|(mode?0:WINHTTP_ADDREQ_FLAG_REPLACE))?0:err(22);}
static int32_t GUEST_ABI load_cert(int id,int count,const void *ca,const void *cert,const void *key){if(!object(id,2)||count||ca||cert||key)return err(22);return 0;}
uintptr_t http_resolve(const char *identity){const char *api=runtime_api_name(identity);
    if((strstr(identity,"#libSceSsl#1#libSceSsl#257")||strstr(identity,"#libSceSsl#1#libSceSsl#513"))){if(!strcmp(api,"sceSslInit"))return (uintptr_t)ssl_init;if(!strcmp(api,"sceSslTerm"))return (uintptr_t)release;}
    if(!strstr(identity,"#libSceHttp#1#libSceHttp#257"))return 0;
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceHttpInit",http_init);API("sceHttpTerm",release);API("sceHttpCreateTemplate",create_template);API("sceHttpCreateConnectionWithURL",create_connection);API("sceHttpCreateRequestWithURL",create_request);
    API("sceHttpDeleteTemplate",release);API("sceHttpDeleteConnection",release);API("sceHttpDeleteRequest",release);API("sceHttpSendRequest",send_request);API("sceHttpReadData",read_data);API("sceHttpGetStatusCode",status_code);API("sceHttpGetAllResponseHeaders",response_headers);
    API("sceHttpSetConnectTimeOut",timeout_setting);API("sceHttpSetRecvTimeOut",timeout_setting);API("sceHttpSetSendTimeOut",timeout_setting);API("sceHttpSetChunkedTransferEnabled",chunked);API("sceHttpAddRequestHeader",add_header);API("sceHttpsLoadCert",load_cert);
#undef API
    return 0;
}
