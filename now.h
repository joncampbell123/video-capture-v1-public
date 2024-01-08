/* Precision time public definition.
 * (C) 2008-2015 Jonathan Campbell
 */

#ifndef __VIDEOCAP_VIDEOCAP_TIME_NOW_H
#define __VIDEOCAP_VIDEOCAP_TIME_NOW_H

#include <stdint.h>
#include <time.h>	/* time_t */

/* the microsecond-precision version of time_t */
typedef uint64_t	us_time_t;
typedef uint64_t	ns_time_t;

/* conversion */
static inline us_time_t time_t_to_us_time_t(time_t t) {
	return (us_time_t)t * 1000000ULL;
}

static inline time_t us_time_t_to_time_t(us_time_t t) {
	return (time_t)(t / 1000000ULL);
}

#ifdef __cplusplus
extern "C" {
#endif

/* what time is it NOW */
extern __thread double		NOW;
extern __thread us_time_t	NOW_us;
extern __thread double		monoNOW;
extern __thread us_time_t	monoNOW_us;

double f_time();
us_time_t us_time();
void update_now_time();
void f_usleep(double n);
void f_sleep_until(double n);
us_time_t gettimeofday_us(void);
us_time_t monotonic_clock_us(void);

#ifdef __cplusplus
}
#endif

#endif /* __VIDEOCAP_VIDEOCAP_TIME_NOW_H */
