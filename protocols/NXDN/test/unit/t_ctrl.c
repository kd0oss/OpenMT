/* Created by Rick KD0OSS */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "nxdn_func.h"
#include "nxdn_gen.h"
#define MAXF 400
static uint8_t net[MAXF][40]; static uint8_t nt[MAXF]; static int nn;
static uint8_t rf[MAXF][48]; static int nr; static int rfeot;
static NxdnCall ev[64]; static int ne;
static void onNet(void*c,const uint8_t*d,uint8_t t){ memcpy(net[nn],d,40); nt[nn++]=t; }
static void onRf(void*c,const uint8_t*f,bool eot){ memcpy(rf[nr++],f,48); if(eot) rfeot++; }
static void onEv(void*c,const NxdnCall*k){ ev[ne++]=*k; }
static int fails;
#define CK(c,m) do{ if(!(c)){ printf("FAIL %s\n",m); fails++; } }while(0)
int main(){
  static uint8_t frames[MAXF][48];
  unsigned nf = nxdnGenVoiceCall(7,1234,9,true,50,frames,MAXF,5);
  CK(nf==52,"gen count");
  NxdnCtrl a; nxdnCtrlInit(&a); a.ran=7; a.sendNet=onNet; a.event=onEv; a.localRepeat=true; a.sendRf=onRf;
  for(unsigned i=0;i<nf;i++) nxdnCtrlRf(&a,frames[i],false);
  printf("net msgs %d (hdr+50+trailer expect 52), events %d\n",nn,ne);
  CK(nn==52,"net count"); CK(nt[0]==NXDN_NET_VOICE_HEADER && nt[51]==NXDN_NET_VOICE_TRAILER,"net types");
  for(int i=0;i<ne;i++) printf(" ev %d src %u dst %u grp %d sec %.2f ber %.2f\n",ev[i].event,ev[i].src,ev[i].dst,ev[i].group,ev[i].seconds,ev[i].ber);
  CK(ne==2 && ev[0].event==NXDN_EV_RF_START && ev[0].src==1234 && ev[0].dst==9 && ev[0].group,"events");
  CK(ev[1].event==NXDN_EV_RF_END && ev[1].ber==0.0f,"end ber");
  // localRepeat output must equal original frames
  CK(nr==52,"repeat count");
  int same=0; for(int i=0;i<nr&&i<52;i++) if(!memcmp(rf[i],frames[i],48)) same++; printf("repeat identical %d/%d\n",same,nr);
  // net -> rf
  nr=0; rfeot=0; ne=0; int nn0=nn;
  NxdnCtrl b; nxdnCtrlInit(&b); b.ran=7; b.sendRf=onRf; b.event=onEv;
  for(int i=0;i<nn0;i++) nxdnCtrlNet(&b,net[i]);
  printf("net->rf frames %d eot %d events %d\n",nr,rfeot,ne);
  same=0; for(int i=0;i<nr&&i<52;i++) if(!memcmp(rf[i],frames[i],48)) same++; printf("net->rf identical to original %d/%d\n",same,nr);
  CK(nr==52 && same==52,"net->rf identical"); CK(rfeot==1,"eot");
  // late entry: skip header and first 5 voice frames
  nn=0; ne=0; nr=0;
  NxdnCtrl c; nxdnCtrlInit(&c); c.ran=7; c.sendNet=onNet; c.event=onEv;
  for(unsigned i=6;i<nf;i++) nxdnCtrlRf(&c,frames[i],false);
  printf("late entry: net msgs %d events %d (first ev %d)\n",nn,ne,ne?ev[0].event:-1);
  CK(ne>=1 && ev[0].event==NXDN_EV_RF_LATE && ev[0].src==1234,"late entry"); CK(nt[0]==NXDN_NET_VOICE_HEADER,"late hdr");
  // wrong RAN
  nn=0; ne=0; NxdnCtrl d; nxdnCtrlInit(&d); d.ran=3; d.sendNet=onNet; d.event=onEv;
  for(unsigned i=0;i<nf;i++) nxdnCtrlRf(&d,frames[i],false);
  CK(nn==0,"RAN filter");
  // selfOnly
  nn=0; ne=0; NxdnCtrl e; nxdnCtrlInit(&e); e.ran=7; e.selfOnly=true; e.id=99; e.sendNet=onNet; e.event=onEv;
  for(unsigned i=0;i<nf;i++) nxdnCtrlRf(&e,frames[i],false);
  CK(nn==0 && ne==1 && ev[0].event==NXDN_EV_RF_REJECTED,"selfOnly");
  // bit errors in voice frames: flip 6 bits in each voice frame
  nn=0; ne=0; NxdnCtrl g; nxdnCtrlInit(&g); g.ran=7; g.sendNet=onNet; g.event=onEv;
  srand(3);
  for(unsigned i=0;i<nf;i++){ uint8_t t[48]; memcpy(t,frames[i],48); if(i>0&&i<nf-1) for(int k=0;k<6;k++){int b=40+rand()%344; t[b>>3]^=0x80>>(b&7);} nxdnCtrlRf(&g,t,false);} 
  printf("noisy: net %d, last ev ber %.2f\n",nn,ne?ev[ne-1].ber:-1.0f);
  CK(nn>=50,"noisy delivery");
  // data call
  nn=0; ne=0; nr=0; unsigned nd = nxdnGenDataCall(7,55,66,false,6,frames,MAXF,9);
  NxdnCtrl h; nxdnCtrlInit(&h); h.ran=7; h.sendNet=onNet; h.event=onEv;
  for(unsigned i=0;i<nd;i++) nxdnCtrlRf(&h,frames[i],false);
  printf("data: frames %u net %d types %d..%d events %d\n",nd,nn,nt[0],nt[nn-1],ne);
  CK(nn==8 && nt[0]==NXDN_NET_DATA_HEADER && nt[7]==NXDN_NET_DATA_TRAILER,"data net");
  nr=0; ne=0; NxdnCtrl k; nxdnCtrlInit(&k); k.ran=7; k.sendRf=onRf; k.event=onEv; for(int i=0;i<nn;i++) nxdnCtrlNet(&k,net[i]);
  same=0; for(int i=0;i<nr&&i<(int)nd;i++) if(!memcmp(rf[i],frames[i],48)) same++; printf("data net->rf identical %d/%d\n",same,nr);
  CK(nr==8&&same==8,"data roundtrip");
  // watchdog: net voice without trailer
  nr=0; rfeot=0; ne=0; nn=0; nf = nxdnGenVoiceCall(7,1,2,true,10,frames,MAXF,5);
  NxdnCtrl w; nxdnCtrlInit(&w); w.ran=7; w.sendNet=onNet; w.sendRf=onRf; w.event=onEv;
  { NxdnCtrl s; nxdnCtrlInit(&s); s.ran=7; s.sendNet=onNet; for(unsigned i=0;i<nf-1;i++) nxdnCtrlRf(&s,frames[i],false);} 
  for(int i=0;i<nn;i++) nxdnCtrlNet(&w,net[i]);
  for(int t=0;t<20;t++) nxdnCtrlClock(&w,100);
  CK(rfeot==1 && !nxdnCtrlBusy(&w),"net watchdog eot");
  printf(fails?"FAIL\n":"PASS\n"); return fails!=0;
}
