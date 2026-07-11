#ifndef __CACHE_FLUSH_H__
#define __CACHE_FLUSH_H__

void cbo_cache_flush(unsigned long base);
void cbo_cache_inval(unsigned long base);
void cbo_cache_clean(unsigned long base);
void cbo_cache_zero(unsigned long base);
void prefetch_i(unsigned long base);
void prefetch_w(unsigned long base);
void prefetch_r(unsigned long base);

void dcache_clean_range(unsigned long base, unsigned long size);    //把脏 cache line 写回内存，但 cache line 仍可保留。
void dcache_flush_range(unsigned long base, unsigned long size);   //通常表示 clean + invalidate，既写回又失效。
void dcache_inval_range(unsigned long base, unsigned long size);    //让 cache line 失效，CPU 下次读必须从内存重新取。

#endif
