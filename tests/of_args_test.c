#include "of_args.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    char *argv[]={"game","--slow-frame-ms","60","--bench-battles"};
    unsigned ms=99; int index=1;
    assert(ofParseSlowFrameArg(4,argv,&index,&ms) && index==2 && ms==60);
    char *missing[]={"game","--slow-frame-ms","--bench-battles"};
    index=1;ms=99;assert(!ofParseSlowFrameArg(3,missing,&index,&ms) && index==1 && ms==99);
    assert(!ofParseSlowFrameArg(2,missing,&index,&ms) && index==1);
    const char *invalid[]={"","-1","+1","1.5","12x"," 12","0","9999999999999999999999999999999"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        argv[2]=(char*)invalid[i];index=1;ms=99;
        assert(!ofParseSlowFrameArg(4,argv,&index,&ms) && index==1 && ms==99);
    }
    char maximum[32];snprintf(maximum,sizeof(maximum),"%u",UINT_MAX);
    argv[2]=maximum;index=1;assert(ofParseSlowFrameArg(4,argv,&index,&ms) && ms==UINT_MAX);
    puts("slow-frame option validation: passed");return 0;
}
