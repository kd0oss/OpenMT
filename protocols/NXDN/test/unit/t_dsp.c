/* Created by Rick KD0OSS */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "nxdn_func.h"
#include "nxdn_dsp.h"
#include "nxdn_gen.h"
#define MAXF 200
static uint8_t got[MAXF*4][48]; static int ng, nlost;
static void cb(void*c,const uint8_t*f,bool lost){ if(lost){nlost++;return;} memcpy(got[ng++],f,48); }
static double gauss(){ double u=(rand()+1.0)/(RAND_MAX+2.0), v=(rand()+1.0)/(RAND_MAX+2.0); return sqrt(-2*log(u))*cos(6.2831853*v); }
static int run(const char*name,double noiseRms,double dc,double gain,double ppm,int lead,int tail,uint8_t shapeTx,int expectAll, int* outFramesMatched){
  static uint8_t frames[MAXF][48]; unsigned nf=nxdnGenVoiceCall(5,4321,77,true,60,frames,MAXF,11);
  static NxdnTx tx; static NxdnRx rx; nxdnTxInit(&tx,NXDN_LEVEL_OUTER,shapeTx); nxdnRxInit(&rx,cb,NULL,NXDN_SHAPE_RRC);
  int cap=(nf+4)*1920+4000+lead+tail; int16_t* s=calloc(cap+10,2); int pos=lead;
  pos+=nxdnTxPreamble(&tx,40,s+pos);
  for(unsigned i=0;i<nf;i++) pos+=nxdnTxFrame(&tx,frames[i],s+pos);
  pos+=nxdnTxFlush(&tx,s+pos);
  int total=pos+tail;
  // impairments: resample for clock error, add noise/dc/gain
  int N=total; float* y=malloc(sizeof(float)*(N+10)); double step=1.0+ppm*1e-6;
  for(int i=0;i<N;i++){ double t=i*step; int i0=(int)t; double fr=t-i0; double v=(i0+1<total)? s[i0]*(1-fr)+s[i0+1]*fr:0; y[i]=(float)(v*gain+dc+noiseRms*gauss()); }
  int16_t* o=malloc(2*N); for(int i=0;i<N;i++){ float v=y[i]; if(v>32767)v=32767; if(v<-32768)v=-32768; o[i]=(int16_t)lrintf(v);} 
  ng=0; nlost=0;
  // feed in odd chunks
  for(int i=0;i<N;){ int c=96; if(i+c>N)c=N-i; nxdnRxSamples(&rx,o+i,c); i+=c; }
  int match=0; for(int i=0;i<ng;i++){ for(unsigned j=0;j<nf;j++) if(!memcmp(got[i],frames[j],48)){match++;break;} }
  int bitErr=0; { // compare aligned by first matching
    for(int i=0;i<ng;i++){ int best=999; for(unsigned j=0;j<nf;j++){ int e=0; for(int b=0;b<48;b++) e+=__builtin_popcount(got[i][b]^frames[j][b]); if(e<best)best=e;} bitErr+=best; } }
  printf("%-34s frames in %u out %d exact %d lost %d bitErrs %d locks %u cand %u lastR %.2f amp %.0f\n",name,nf,ng,match,nlost,bitErr,rx.statLocks,rx.statCandidates,rx.statLastR,rx.statAmp);
  free(s);free(y);free(o);
  *outFramesMatched=match; return match>=expectAll;
}
int main(){ srand(7); int fails=0,m;
  if(!run("clean",0,0,1,0,3000,3000,NXDN_SHAPE_RRC,50,&m)) fails++;
  if(!run("offset lead 1234",0,0,1,0,1234,3000,NXDN_SHAPE_RRC,50,&m)) fails++;
  if(!run("dc 800 gain 0.4",0,800,0.4,0,2222,3000,NXDN_SHAPE_RRC,50,&m)) fails++;
  if(!run("clock +150ppm",0,0,1,150,2222,3000,NXDN_SHAPE_RRC,50,&m)) fails++;
  if(!run("clock -300ppm",0,0,1,-300,2222,3000,NXDN_SHAPE_RRC,50,&m)) fails++;
  if(!run("noise rms 120 (signal ~735)",120,0,1,0,2222,3000,NXDN_SHAPE_RRC,50,&m)) fails++;
  if(!run("noise rms 200",200,0,1,50,2222,3000,NXDN_SHAPE_RRC,40,&m)) fails++;
  if(!run("tx RC shape rx RRC",0,0,1,0,2222,3000,NXDN_SHAPE_RC,40,&m)) fails++;
  // noise only: no locks
  { static NxdnRx rx; nxdnRxInit(&rx,cb,NULL,NXDN_SHAPE_RRC); ng=0; int16_t b[96]; for(int k=0;k<24000*60/96;k++){ for(int i=0;i<96;i++) b[i]=(int16_t)lrint(400*gauss()); nxdnRxSamples(&rx,b,96);} printf("noise only 60s: cand %u locks %u frames %d\n",rx.statCandidates,rx.statLocks,ng); if(rx.statLocks) fails++; }
  printf(fails?"FAIL\n":"PASS\n"); return fails;
}
