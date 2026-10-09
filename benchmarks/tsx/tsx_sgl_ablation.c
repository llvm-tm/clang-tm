/*
 * tsx_sgl_ablation.c — isolates the TSXSGL SGL-entry store-buffer race
 * (docs/CORRECTNESS_FIXES.md §19). N threads do N 1-access RMW transactions
 * each on one shared counter: TSX first (5 xbegin tries), mutex+SGL-owner
 * fallback.  Modes:
 *   M=0  original interlock (plain stores to sgl_owner)       -> LOST (8/8 @4t)
 *   M=1  mfence after sgl_owner=1 (the fix)                   -> PASS  (8/8)
 *   M=2  mfence only after sgl_owner=0                        -> LOST (8/8)
 * Run: gcc -O2 -mrtm -pthread -o tsx_sgl_ablation tsx_sgl_ablation.c
 *      ./tsx_sgl_ablation 0 4 200000     # M threads iters-per-thread
 * Requires real TSX (rtm); results from intel14v2 Broadwell-EP 2026-10-05.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
static volatile uint64_t ctr __attribute__((aligned(64))) = 0;
static volatile uint64_t sgl_owner __attribute__((aligned(64))) = 0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int M,NTHREADS; static int N = 200000;
static void *w(void *a){ long id=(long)a; static const int cpus[]={1,13,2,14,3,15}; cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpus[id%6],&s); pthread_setaffinity_np(pthread_self(),sizeof(s),&s);
 for(int i=0;i<N;i++){ int in_tx=0;
  for(int t=0;t<5;t++){ unsigned st=_xbegin();
   if(st==_XBEGIN_STARTED){
    if(sgl_owner!=0){_xabort(0xFF);continue;}
    ctr=ctr+1;
    if(sgl_owner!=0)_xabort(0x01);
    _xend(); in_tx=1; break; } }
  if(!in_tx){ pthread_mutex_lock(&mtx);
    sgl_owner=1; if(M==1){_mm_mfence();}
    ctr=ctr+1;
    sgl_owner=0; if(M==2){_mm_mfence();}
    pthread_mutex_unlock(&mtx);} }
 return NULL;}
int main(int c,char**v){ M=atoi(v[1]); NTHREADS=c>2?atoi(v[2]):4; if(c>3)N=atoi(v[3]);
 pthread_t t[8]; for(long i=0;i<NTHREADS;i++)pthread_create(&t[i],NULL,w,(void*)i); for(int i=0;i<NTHREADS;i++)pthread_join(t[i],NULL); printf("M=%d final=%llu expected=%lld -> %s\n",M,(unsigned long long)ctr,(long long)N*(long long)NTHREADS,ctr==(uint64_t)N*(uint64_t)NTHREADS?"PASS":"LOST"); return 0;}
