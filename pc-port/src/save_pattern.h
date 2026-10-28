// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <ctype.h>
// PS4 save searches use SQL-style '%' and '_', not filesystem '*' and '?'.
static int save_pattern_matches(const char *name,const char *pattern){
    const char *star=NULL,*retry=NULL;
    while(*name){
        if(*pattern=='%'){star=pattern++;retry=name;}
        else if(*pattern=='_'||(*pattern&&tolower((unsigned char)*pattern)==tolower((unsigned char)*name))){++name;++pattern;}
        else if(star){pattern=star+1;name=++retry;}
        else return 0;
    }
    while(*pattern=='%')++pattern;return !*pattern;
}
