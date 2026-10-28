// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"SAVE DIALOG FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef int32_t(GUEST_ABI *Call)(void);
typedef int32_t(GUEST_ABI *ParamCall)(void*);
int main(void){
    Call common=(Call)dialog_resolve("uoUpLGNkygk#libSceCommonDialog#1#libSceCommonDialog#257");
    Call init=(Call)dialog_resolve("s9e3+YpRnzw#libSceSaveDataDialog#1#libSceSaveDataDialog#257");
    Call status=(Call)dialog_resolve("KK3Bdg1RWK0#libSceSaveDataDialog#1#libSceSaveDataDialog#257");
    Call terminate=(Call)dialog_resolve("YuH2FA7azqQ#libSceSaveDataDialog#1#libSceSaveDataDialog#257");
    ParamCall open=(ParamCall)dialog_resolve("4tPhsP6FpDI#libSceSaveDataDialog#1#libSceSaveDataDialog#257");
    ParamCall result=(ParamCall)dialog_resolve("yEiJ-qqr6Cg#libSceSaveDataDialog#1#libSceSaveDataDialog#257");
    CHECK(common&&init&&status&&terminate&&open&&result);CHECK(status()==0);
    CHECK((uint32_t)init()==0x80b80001);CHECK(common()==0);CHECK(init()==0&&status()==1);
    CHECK((uint32_t)init()==0x80b80004);CHECK((uint32_t)result(NULL)==0x80b80005);
    CHECK((uint32_t)open(NULL)==0x80b8000d);unsigned char param[152]={0};CHECK((uint32_t)open(param)==0x80b8000a);
    uint64_t base=48;uint32_t size=152,mode=1,type=1;memcpy(param,&base,8);memcpy(param+48,&size,4);memcpy(param+52,&mode,4);memcpy(param+56,&type,4);
    CHECK((uint32_t)open(param)==0x80b8000d);CHECK(status()==1);CHECK(terminate()==0&&status()==0);
    CHECK((uint32_t)terminate()==0x80b80003);CHECK(init()==0);CHECK(terminate()==0);
    puts("PASS: save dialog imports, lifecycle, ABI validation and invalid parameter rejection.");return 0;
}
