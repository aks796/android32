/* dcr_dircache.h -- "does this file exist?" from directory listings
 * (dcr_dircache.c). MIT. */
#ifndef DCR_DIRCACHE_H
#define DCR_DIRCACHE_H

/* 1: `real` (a translated path) certainly does not exist, answer ENOENT;
 * 0: ask the file system. Always 0 when RT_DIRCACHE is 0. */
int dcr_dircache_missing(const char *real);
/* Something was created, deleted or renamed: forget every listing. */
void dcr_dircache_forget(void);
/* One line in debug.log: listings made, misses answered. */
void dcr_dircache_report(void);

#endif
