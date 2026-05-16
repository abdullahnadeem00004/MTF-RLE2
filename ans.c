/*
 * ans.c
 * -----
 * Asymmetric Numeral Systems (tANS / FSE) entropy coder.
 *
 * Stage 5 in the pipeline, after RLE-2.
 *
 * === Algorithm (tANS) ===
 *
 * State x lives in [ANS_L, 2*ANS_L).
 *
 * ENCODING (process symbols right-to-left, i = n-1 .. 0):
 *   Lift xs = x + ANS_L  (bring [0,ANS_L) → [ANS_L,2*ANS_L))
 *   1. Renorm: find nb >= 0 s.t. (xs >> nb) ∈ [freq[s], 2*freq[s])
 *   2. Push (xs & ((1<<nb)-1)) — nb LSBs — into the bit accumulator (LSB first)
 *   3. rank = (xs >> nb) - freq[s]
 *   4. x = encode_table[cumul[s] + rank]  (a decode-table slot index in [0,ANS_L))
 *
 * DECODING (process symbols left-to-right, i = 0 .. n-1):
 *   Decoder maintains a bit pointer 'ptr' starting at total_bits and moving DOWN.
 *   For each symbol:
 *     sym       = decode_table[x].sym
 *     nb        = decode_table[x].nb
 *     bits      = read nb bits at positions [ptr-nb .. ptr-1] (LSB at ptr-nb)
 *     ptr      -= nb
 *     x         = decode_table[x].next_base + bits - ANS_L
 *
 * === Bit ordering rationale ===
 *   The encoder processes symbols right-to-left and pushes bits LSB-first.
 *   Symbol i=0 (leftmost, decoded first) is processed LAST by the encoder,
 *   so its bits end up at the HIGHEST bit positions in the stream.
 *   The decoder reads groups top-down (ptr starts at total_bits and decrements),
 *   so symbol i=0's bits are read first. ✓
 *
 * === Output format (all little-endian integers) ===
 *   [4]    magic  0x414E5300  ("ANS\0")
 *   [4]    original byte count
 *   [1]    ANS_LOG
 *   [512]  freq table (256 × uint16_t, little-endian)
 *   [4]    final encoder state (= initial decoder x, in [0,ANS_L))
 *   [4]    total_bits (the bit-pointer starting value for the decoder)
 *   [ceil(total_bits/8)]  raw bit buffer (LSB of byte 0 = bit position 0)
 */

#include "bzip2.h"
#include <stdint.h>

#define ANS_LOG   11
#define ANS_L     (1u << ANS_LOG)
#define ANS_SYMS  256
#define ANS_MAGIC 0x414E5300u

/* ── little-endian I/O ───────────────────────────────────────── */
static void w32(unsigned char *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static uint32_t r32(const unsigned char *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

/* ── normalise counts → freq[] summing to ANS_L ─────────────── */
static void normalise(const size_t *cnt, uint16_t *freq) {
    size_t tot=0;
    for(int i=0;i<ANS_SYMS;i++) tot+=cnt[i];
    if(!tot) return;
    size_t sum=0;
    for(int i=0;i<ANS_SYMS;i++){
        if(!cnt[i]){freq[i]=0;continue;}
        uint32_t f=(uint32_t)(((uint64_t)cnt[i]*ANS_L)/tot);
        if(!f) f=1;
        freq[i]=(uint16_t)f; sum+=f;
    }
    while(sum<ANS_L){
        int b=-1; for(int i=0;i<ANS_SYMS;i++) if(freq[i]&&(b<0||freq[i]>freq[b]))b=i;
        if(b<0)break; freq[b]++; sum++;
    }
    while(sum>ANS_L){
        int b=-1; for(int i=0;i<ANS_SYMS;i++) if(freq[i]>1&&(b<0||freq[i]>freq[b]))b=i;
        if(b<0)break; freq[b]--; sum--;
    }
}

/* ── decode-table entry ──────────────────────────────────────── */
typedef struct {
    uint8_t  sym;
    uint8_t  nb;         /* bits to consume from stream                    */
    uint16_t next_base;  /* next_state = next_base + bits_read             */
                         /* next_base ∈ [ANS_L, 2*ANS_L)                   */
} DEntry;

/* ── build spread + decode table ────────────────────────────── */
static void build_dtab(const uint16_t *freq, DEntry *dtab) {
    uint8_t spread[ANS_L];
    {
        uint32_t step=(ANS_L>>1)+(ANS_L>>3)+3;
        if(!(step&1)) step|=1;
        uint32_t pos=0;
        for(int s=0;s<ANS_SYMS;s++)
            for(uint32_t k=0;k<(uint32_t)freq[s];k++){
                spread[pos]=(uint8_t)s;
                pos=(pos+step)&(ANS_L-1);
            }
    }
    uint32_t nxt[ANS_SYMS];
    for(int s=0;s<ANS_SYMS;s++) nxt[s]=(uint32_t)freq[s];
    for(uint32_t i=0;i<ANS_L;i++){
        uint8_t  s=spread[i];
        uint32_t x=nxt[s]++;         /* x ∈ [freq[s], 2*freq[s]) */
        int nb=0; uint32_t t=x;
        while(t<ANS_L){nb++;t<<=1;}  /* t = x<<nb ∈ [ANS_L,2*ANS_L) */
        dtab[i].sym      =s;
        dtab[i].nb       =(uint8_t)nb;
        dtab[i].next_base=(uint16_t)t;
    }
}

/* ── build encode table ──────────────────────────────────────── */
static void build_etab(const uint16_t *freq, const DEntry *dtab,
                       uint16_t *etab, uint32_t *cumul) {
    cumul[0]=0;
    for(int s=0;s<ANS_SYMS;s++) cumul[s+1]=cumul[s]+freq[s];
    for(uint32_t i=0;i<ANS_L;i++){
        uint8_t  s =dtab[i].sym;
        uint8_t  nb=dtab[i].nb;
        uint32_t xp=(uint32_t)dtab[i].next_base>>nb;   /* ∈ [freq[s],2*freq[s]) */
        uint32_t rank=xp-freq[s];
        etab[cumul[s]+rank]=(uint16_t)i;
    }
}

/* ── growable byte buffer ────────────────────────────────────── */
typedef struct{unsigned char *buf;size_t len,cap;}BB;
static void bb_init(BB *b,size_t c){b->buf=(unsigned char*)malloc(c);b->len=0;b->cap=c;}
static void bb_put(BB *b,unsigned char c){
    if(b->len>=b->cap){b->cap=b->cap*2+64;b->buf=(unsigned char*)realloc(b->buf,b->cap);}
    b->buf[b->len++]=c;
}

/* ── public encode ───────────────────────────────────────────── */
int ans_encode(const unsigned char *in, size_t len,
               unsigned char **out, size_t *out_len)
{
    if(!in||!out||!out_len) return -1;
    *out=NULL; *out_len=0;

    /* 1. Frequencies */
    size_t cnt[ANS_SYMS]={0};
    for(size_t i=0;i<len;i++) cnt[in[i]]++;
    uint16_t freq[ANS_SYMS]={0};
    normalise(cnt,freq);
    /* need ≥2 non-zero freqs so nb≥1 is possible */
    {
        int ns=0,only=-1;
        for(int i=0;i<ANS_SYMS;i++) if(freq[i]){ns++;only=i;}
        if(ns==1){int o=(only==0)?1:0;freq[only]=(uint16_t)(ANS_L-1);freq[o]=1;}
    }

    /* 2. Tables */
    DEntry  *dtab=(DEntry*)  malloc(ANS_L*sizeof(DEntry));
    uint16_t *etab=(uint16_t*)malloc(ANS_L*sizeof(uint16_t));
    uint32_t cumul[ANS_SYMS+1];
    if(!dtab||!etab){free(dtab);free(etab);return -1;}
    build_dtab(freq,dtab);
    build_etab(freq,dtab,etab,cumul);

    /* 3. Encode right-to-left, accumulating bits LSB-first */
    BB bb; bb_init(&bb,len+64);
    uint64_t acc=0; int acnt=0;
    uint32_t x=0;

    for(size_t i=len;i-->0;){
        uint8_t  s=in[i];
        uint32_t f=(uint32_t)freq[s];
        if(!f){free(dtab);free(etab);free(bb.buf);return -1;}
        uint32_t xs=x+ANS_L;
        int nb=0; uint32_t t=xs;
        while(t>=2*f){nb++;t>>=1;}
        acc|=(uint64_t)(xs&((1u<<nb)-1))<<acnt; acnt+=nb;
        while(acnt>=8){bb_put(&bb,(uint8_t)(acc&0xFF));acc>>=8;acnt-=8;}
        x=etab[cumul[s]+(t-f)];
    }
    int last_acnt=acnt;
    if(acnt>0) bb_put(&bb,(uint8_t)(acc&0xFF));

    free(dtab); free(etab);

    /* total_bits: the decoder's starting ptr value */
    uint32_t total_bits=(uint32_t)((bb.len-(last_acnt>0?1:0))*8+(size_t)(last_acnt>0?last_acnt:8));
    if(last_acnt==0 && bb.len>0) total_bits=(uint32_t)bb.len*8;

    /* 4. Assemble output */
    size_t hdr  = 4+4+1+ANS_SYMS*2+4+4;
    size_t plen = bb.len;
    unsigned char *res=(unsigned char*)malloc(hdr+plen);
    if(!res){free(bb.buf);return -1;}

    unsigned char *p=res;
    w32(p,ANS_MAGIC);           p+=4;
    w32(p,(uint32_t)len);       p+=4;
    *p++=(uint8_t)ANS_LOG;
    for(int s=0;s<ANS_SYMS;s++){p[0]=(uint8_t)freq[s];p[1]=(uint8_t)(freq[s]>>8);p+=2;}
    w32(p,x);                   p+=4;
    w32(p,total_bits);          p+=4;
    memcpy(p,bb.buf,plen);

    free(bb.buf);
    *out=res; *out_len=hdr+plen;
    return 0;
}

/* ── public decode ───────────────────────────────────────────── */
int ans_decode(const unsigned char *in, size_t in_len,
               unsigned char **out, size_t *out_len)
{
    if(!in||!out||!out_len) return -1;
    *out=NULL; *out_len=0;

    size_t hdr=4+4+1+ANS_SYMS*2+4+4;
    if(in_len<hdr) return -1;

    const unsigned char *p=in;
    if(r32(p)!=ANS_MAGIC) return -1; p+=4;
    uint32_t olen=r32(p); p+=4;
    if((uint8_t)*p!=ANS_LOG) return -1; p++;
    uint16_t freq[ANS_SYMS];
    for(int s=0;s<ANS_SYMS;s++){freq[s]=(uint16_t)p[0]|((uint16_t)p[1]<<8);p+=2;}
    uint32_t x        =r32(p); p+=4;
    uint32_t total_bits=r32(p); p+=4;
    if((size_t)(p-in)>in_len) return -1;
    const unsigned char *bits_buf=p;

    DEntry *dtab=(DEntry*)malloc(ANS_L*sizeof(DEntry));
    if(!dtab) return -1;
    build_dtab(freq,dtab);

    unsigned char *res=(unsigned char*)malloc(olen+1);
    if(!res){free(dtab);return -1;}

    /* bit pointer: starts at total_bits, decrements as bits are consumed */
    uint32_t ptr=total_bits;

    for(uint32_t i=0;i<olen;i++){
        if(x>=ANS_L){free(dtab);free(res);return -1;}
        DEntry *e=&dtab[x];
        res[i]=e->sym;
        int nb=e->nb;
        /* read nb bits at absolute positions [ptr-nb .. ptr-1], LSB at ptr-nb */
        uint32_t bits=0;
        for(int k=0;k<nb;k++){
            uint32_t pos=ptr-(uint32_t)nb+(uint32_t)k;
            bits|=(uint32_t)((bits_buf[pos/8]>>(pos%8))&1)<<k;
        }
        ptr-=(uint32_t)nb;
        x=(uint32_t)e->next_base+bits-ANS_L;
    }

    free(dtab);
    *out=res; *out_len=olen;
    return 0;
}

/* ── self-tests ──────────────────────────────────────────────── */
void run_ans_tests(void)
{
    printf("=== ANS Self-Tests ===\n");

    typedef struct{const char *l;unsigned char *d;size_t n;int heap;}C;
    C cases[8]; int nc=0;

    static unsigned char same[64];   memset(same,0xAA,64);
    static unsigned char ascii[]="Hello, World! ANS entropy coding test.";
    static unsigned char zeros[200]; memset(zeros,0,200);
    static unsigned char all256[256]; for(int i=0;i<256;i++) all256[i]=(uint8_t)i;
    static unsigned char one[]={42};
    static unsigned char two[]={0,255};

    cases[nc++]=(C){"All same (64x 0xAA)",      same,   64,            0};
    cases[nc++]=(C){"ASCII string",              ascii,  sizeof(ascii)-1,0};
    cases[nc++]=(C){"200 zeros",                 zeros,  200,           0};
    cases[nc++]=(C){"All 256 values",            all256, 256,           0};
    cases[nc++]=(C){"Single byte",               one,    1,             0};
    cases[nc++]=(C){"Two bytes {0,255}",         two,    2,             0};
    {
        size_t n=500; unsigned char *d=(unsigned char*)malloc(n);
        for(size_t i=0;i<n;i++) d[i]=(i%4<2)?0:(i%4==2)?1:(uint8_t)(i%20);
        cases[nc++]=(C){"Skewed MTF-like (500B)", d, n, 1};
    }
    {
        size_t n=4096; unsigned char *d=(unsigned char*)malloc(n);
        uint32_t st=0xDEADBEEF;
        for(size_t i=0;i<n;i++){st=st*1664525u+1013904223u;d[i]=(uint8_t)(st>>24);}
        cases[nc++]=(C){"4096 pseudo-random", d, n, 1};
    }

    int tot=0,pass=0;
    for(int t=0;t<nc;t++){
        unsigned char *enc=NULL,*dec=NULL; size_t el=0,dl=0; int ok=0;
        if(ans_encode(cases[t].d,cases[t].n,&enc,&el)==0)
            if(ans_decode(enc,el,&dec,&dl)==0&&dl==cases[t].n&&
               memcmp(dec,cases[t].d,cases[t].n)==0) ok=1;
        double r=cases[t].n?((double)el/(double)cases[t].n*100.0):0.0;
        printf("  [%s] %-30s in=%4zu enc=%4zu (%.0f%%) dec=%4zu\n",
               ok?"PASS":"FAIL",cases[t].l,cases[t].n,el,r,dl);
        tot++; if(ok)pass++;
        free(enc); free(dec);
        if(cases[t].heap) free(cases[t].d);
    }
    printf("  Result: %d / %d passed\n",pass,tot);
    printf("======================\n\n");
}
