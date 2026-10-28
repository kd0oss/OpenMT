#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "bridge_common.h"
bool readHostConfigN(const char*a,const char*b,const char*c,char*v,size_t n){strncpy(v,"sample",n);return true;}
bool setHostConfig(const char*a,const char*b,const char*c,const char*d,const char*e){return true;}
bool addHostConfigDefault(const char*a,const char*b,const char*c,const char*d,const char*e){return true;}
static int fails;
#define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);fails++;} }while(0)
int main(void){
    uint8_t f[BRIDGE_CTL_MAX],t=0; const uint8_t *pl; size_t pn; bool more;
    CHECK(bridgeCtlBuild(BRIDGE_CTL_RELOAD,NULL,0,f)==3 && f[0]==0xFE && f[1]==3 && f[2]==1);
    CHECK(bridgeCtlParse(f,3,&t,&pl,&pn,&more)==3 && t==1 && pn==0 && !more);
    CHECK(bridgeCtlParse(f,2,&t,&pl,&pn,&more)==0 && more);
    CHECK(bridgeCtlBuild(BRIDGE_CTL_MODE_ON,(const uint8_t*)"DMR",3,f)==6 && f[1]==6);
    CHECK(bridgeCtlParse(f,6,&t,&pl,&pn,&more)==6 && t==2 && pn==3 && memcmp(pl,"DMR",3)==0);
    CHECK(bridgeCtlParse(f,5,&t,&pl,&pn,&more)==0 && more);
    CHECK(bridgeCtlParse(f,8,&t,&pl,&pn,&more)==6);
    uint8_t big[64]; memset(big,'x',64); CHECK(bridgeCtlBuild(2,big,40,f)==0);
    uint8_t e0[3]={0xE0,3,0}; CHECK(bridgeCtlParse(e0,3,&t,&pl,&pn,&more)==0 && !more);
    uint8_t o[3]={0x61,3,0xD0}; CHECK(bridgeCtlParse(o,3,&t,&pl,&pn,&more)==0 && !more);
    uint8_t odd[3]={0xFE,1,1}; CHECK(bridgeCtlParse(odd,3,&t,&pl,&pn,&more)==0 && !more);
    uint8_t odd2[3]={0xFE,200,1}; CHECK(bridgeCtlParse(odd2,3,&t,&pl,&pn,&more)==0 && !more);
    CHECK(bridgeStreamFromName("bit")==STREAM_BIT && bridgeStreamFromName("Frame")==STREAM_FRAME && bridgeStreamFromName("SAMPLE")==STREAM_SAMPLE && bridgeStreamFromName("x")==STREAM_NONE && bridgeStreamFromName(NULL)==STREAM_NONE);
    CHECK(strcmp(bridgeStreamName(STREAM_BIT),"BIT")==0 && bridgeStreamName(STREAM_NONE)[0]==0);
    CHECK(bridgeReadStreamType("m")==STREAM_SAMPLE);
    char slave[256]; int m=bridgeOpenPty("/tmp/bctest_link",slave,sizeof slave);
    CHECK(m>=0);
    int s=open("/tmp/bctest_link",O_RDWR|O_NOCTTY); CHECK(s>=0);
    uint8_t out[5]={0x61,5,0x10,0x00,0xFF}; CHECK(write(s,out,5)==5);
    uint8_t in[8]; usleep(20000); ssize_t n=read(m,in,8); CHECK(n==5 && memcmp(in,out,5)==0);
    bridgeCtlBuild(1,NULL,0,f); CHECK(write(m,f,3)==3); usleep(20000); n=read(s,in,8); CHECK(n==3 && in[0]==0xFE);
    printf("FAILS: %d\n",fails); return fails;
}
