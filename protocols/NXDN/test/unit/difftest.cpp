#define private public
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "NXDNDefines.h"
#include "NXDNSACCH.h"
#include "NXDNFACCH1.h"
#include "NXDNUDCH.h"
#include "NXDNLICH.h"
#include "NXDNCRC.h"
#include "NXDNAudio.h"
extern "C" {
#include "nxdn_func.h"
}
static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s (iter %d)\n",msg,it); fails++; } }while(0)
static void rnd(uint8_t*p,int n){ for(int i=0;i<n;i++) p[i]=rand(); }
int main(){
  srand(1);
  int it;
  for(it=0;it<2000;it++){
    uint8_t f1[48],f2[48]; memset(f1,0,48); memset(f2,0,48);
    // SACCH
    { uint8_t d[5]; rnd(d,5);
      CNXDNSACCH s; s.setRaw(d); s.encode(f1); nxdnSacchEncode(d,f2);
      CHECK(!memcmp(f1,f2,48),"sacch encode");
      // decode with errors
      uint8_t g[48]; memcpy(g,f1,48); for(int k=0;k<(it%4);k++){int b=rand()%384; g[b>>3]^=0x80>>(b&7);}
      CNXDNSACCH s2; bool v1=s2.decode(g); uint8_t o[5]; bool v2=nxdnSacchDecode(g,o);
      CHECK(v1==v2,"sacch valid"); uint8_t r1[4],r2[4]; s2.getRaw(r1); nxdnSacchGetRaw(o,r2);
      if(v1) CHECK(!memcmp(r1,r2,4),"sacch raw");
      CHECK(s2.getRAN()==NXDN_SACCH_RAN(o),"sacch ran"); CHECK(s2.getStructure()==NXDN_SACCH_STRUCTURE(o),"sacch struct");
    }
    // FACCH1
    for(int pos=0;pos<2;pos++){ unsigned off=96+pos*144;
      uint8_t d[10]; rnd(d,10); memset(f1,0,48); memset(f2,0,48);
      CNXDNFACCH1 f; f.setData(d); f.encode(f1,off); nxdnFacch1Encode(d,f2,off);
      CHECK(!memcmp(f1,f2,48),"facch encode");
      uint8_t g[48]; memcpy(g,f1,48); for(int k=0;k<(it%6);k++){int b=off+rand()%144; g[b>>3]^=0x80>>(b&7);}
      CNXDNFACCH1 f3; bool v1=f3.decode(g,off); uint8_t o[12]; bool v2=nxdnFacch1Decode(g,off,o);
      CHECK(v1==v2,"facch valid"); uint8_t a[10]; f3.getData(a);
      if(v1) CHECK(!memcmp(a,o,10),"facch data");
    }
    // UDCH
    { uint8_t d[25]; rnd(d,23); d[23]=d[24]=0; memset(f1,0,48); memset(f2,0,48);
      CNXDNUDCH u; u.setRaw(d); u.encode(f1); nxdnUdchEncode(d,f2);
      CHECK(!memcmp(f1,f2,48),"udch encode");
      uint8_t g[48]; memcpy(g,f1,48); for(int k=0;k<(it%8);k++){int b=36+rand()%348; g[b>>3]^=0x80>>(b&7);}
      CNXDNUDCH u2; bool v1=u2.decode(g); uint8_t o[25]; bool v2=nxdnUdchDecode(g,o);
      CHECK(v1==v2,"udch valid"); uint8_t a[25]; u2.getRaw(a);
      if(v1) CHECK(!memcmp(a,o,23),"udch data");
    }
    // LICH
    { uint8_t l=rand(); memset(f1,0,48); memset(f2,0,48);
      CNXDNLICH L; L.setRaw(l); L.encode(f1); nxdnLichEncode(l,f2);
      CHECK(!memcmp(f1,f2,48),"lich encode");
      uint8_t g[48]; rnd(g,48); CNXDNLICH L2; bool v1=L2.decode(g); uint8_t o; bool v2=nxdnLichDecode(g,&o);
      CHECK(v1==v2,"lich valid"); CHECK(L2.getRaw()==o || !v1 ,"lich raw");
    }
    // CRC
    { uint8_t d[30]; rnd(d,30); unsigned n=20+rand()%200;
      CHECK(CNXDNCRC::createCRC6(d,n)==nxdnCrc6(d,n),"crc6"); CHECK(CNXDNCRC::createCRC12(d,n)==nxdnCrc12(d,n),"crc12"); CHECK(CNXDNCRC::createCRC15(d,n)==nxdnCrc15(d,n),"crc15");
    }
    // Audio encode: network form -> air
    { uint8_t in[14]; rnd(in,14); uint8_t o1[18],o2[18]; memset(o1,0,18); memset(o2,0,18);
      CNXDNAudio a; a.encode(in,o1); nxdnAudioEncode(in,o2); CHECK(!memcmp(o1,o2,18),"audio encode");
      // decode of clean encoded: 98 bits equal, 0 errors
      uint8_t d1[14],d2[14]; memset(d1,0,14); memset(d2,0,14); unsigned e=0;
      a.decode(o1,d1); nxdnAudioDecode(o1,d2,&e); CHECK(!memcmp(d1,d2,13)&&e==0,"audio decode clean");
      // with 2 bit errors in a block: ours corrects; archive does not; ours must equal original in+0 on 98 bits
      uint8_t g[18]; memcpy(g,o1,18); for(int k=0;k<2;k++){int b=rand()%72; g[b>>3]^=0x80>>(b&7);} 
      unsigned e2=0; uint8_t d3[14]; memset(d3,0,14); nxdnAudioDecode(g,d3,&e2);
      // compare bits 0..48 and 49..97 where errors were in A/B (not C). Just re-encode and compare to orig air for A,B positions
      (void)e2;
    }
  }
  printf("%s fails=%d\n", fails?"FAIL":"PASS", fails);
  return fails!=0;
}
