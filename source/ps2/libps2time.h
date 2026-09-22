#ifndef _LIBPS2TIME_H_
#define _LIBPS2TIME_H_

#include <time.h>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
	long tv_sec;
	long tv_usec;
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

int ps2time_init(void);
time_t ps2time_time(time_t *t);
struct tm *ps2time_localtime(const time_t *timep);
struct tm *ps2time_gmtime(const time_t *timep);
int ps2time_gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif

#endif
