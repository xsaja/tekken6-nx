/* tekken6-nx: Tekken 6 DATA3xx.BIN (XCTD) decoder for tools/dump_tekken_shaders.py; built by tools/build_xctd.bat
   against the SDK's libmspack. Usage: xctd in.bin out.dec */
/* XCTD (0x0FF512ED) decoder: header 16 B, u32 BE table of decoded sizes, one per 0x20000-byte raw block;
   each block = XMem LZX native frames ([u16 BE csize] or [FF u16 usize u16 csize]). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "lzx.h"
typedef struct { struct mspack_system *sys; unsigned char *buf; size_t len, pos; } memf;
static struct mspack_file *mopen(struct mspack_system *s, const char *f, int m){return NULL;}
static void mclose(struct mspack_file *f){}
static int mread(struct mspack_file *f, void *b, int n){memf*m=(memf*)f; size_t r=m->len-m->pos; if((size_t)n<r)r=n; memcpy(b,m->buf+m->pos,r); m->pos+=r; return (int)r;}
static int mwrite(struct mspack_file *f, void *b, int n){memf*m=(memf*)f; size_t r=m->len-m->pos; if((size_t)n<r)r=n; memcpy(m->buf+m->pos,b,r); m->pos+=r; return (int)r;}
static void *malloc_(struct mspack_system *s, size_t n){return malloc(n);}
static void free_(void*p){free(p);}
static void copy_(void*a,void*b,size_t n){memcpy(b,a,n);}
static void msg_(struct mspack_file*f,const char*fmt,...){}
static int seek_(struct mspack_file*f,off_t o,int m){return -1;}
static off_t tell_(struct mspack_file*f){return 0;}
static struct mspack_system msys={mopen,mclose,mread,mwrite,seek_,tell_,msg_,malloc_,free_,copy_,NULL};
static unsigned be32(const unsigned char*p){return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];}
int main(int argc,char**argv){
  if(argc<3){fprintf(stderr,"xctd in.bin out.dec [blockbits=17] [windowbits=17]\n");return 2;}
  FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long long bs=_ftelli64(f); fseek(f,0,SEEK_SET);
  unsigned char*raw=malloc(bs); fread(raw,1,bs,f); fclose(f);
  if(be32(raw)!=0x0FF512ED){fprintf(stderr,"not XCTD\n");return 1;}
  int bb=argc>3?atoi(argv[3]):17, wb=argc>4?atoi(argv[4]):17; long long B=1LL<<bb;
  long long n=(bs+B-1)/B; long long hdr=16+4*n;
  FILE*o=fopen(argv[2],"wb"); unsigned char*cat=malloc(B+16); unsigned char*out=malloc(1<<25); int bad=0; long long total=0;
  for(long long i=0;i<n;i++){
    unsigned dsz=be32(raw+16+4*i); long long s=i*B; if(s<hdr)s=hdr; long long e=(i+1)*B; if(e>bs)e=bs;
    size_t cl=0; long long p=s; unsigned got=0;
    while(p+2<=e && got<dsz){ unsigned us=0x8000,cs; if(raw[p]==0xFF){us=(raw[p+1]<<8)|raw[p+2]; cs=(raw[p+3]<<8)|raw[p+4]; p+=5;} else {cs=(raw[p]<<8)|raw[p+1]; p+=2;}
      if(cs==0||p+cs>e)break; memcpy(cat+cl,raw+p,cs); cl+=cs; p+=cs; got+=us; }
    memf in={&msys,cat,cl,0}, ou={&msys,out,dsz,0};
    struct lzxd_stream*l=lzxd_init(&msys,(struct mspack_file*)&in,(struct mspack_file*)&ou,wb,0,0x8000,dsz,0);
    int err=l?lzxd_decompress(l,dsz):-1; if(l)lzxd_free(l);
    if(err||got<dsz){ if(bad<10)fprintf(stderr,"block %lld err %d got %u dsz %u\n",i,err,got,dsz); bad++; memset(out,0,dsz);}
    fwrite(out,1,dsz,o); total+=dsz;
  }
  fclose(o); printf("%s: blocks %lld bad %d decoded %lld\n",argv[1],n,bad,total); return bad?1:0;
}
