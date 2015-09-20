/* Precision time API.
 * (C) 2008-2015 Jonathan Campbell
 *
 * Typically this is used to obtain the system clock time at greater
 * precision than one second.
 */

#include "now.h"
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

__thread double		NOW = 0.0;
__thread us_time_t	NOW_us = 0ULL;

us_time_t us_time() {
	struct timeval tv;
	gettimeofday(&tv,NULL);
	return (((us_time_t)tv.tv_sec) * 1000000ULL) + (us_time_t)tv.tv_usec;
}

void update_now_time() {
	NOW_us = us_time();
	NOW = (double)NOW_us / 1000000;
}

void f_sleep_until(double n) {
	double t = n - NOW;
	if (t > 0.0001) f_usleep(t);
}

double f_time() {
	return (double)NOW_us / 1000000;
}

void f_usleep(double n) {
	if (n >= 0.000001) {
		if (n > 30) n = 30;
		(void)usleep((unsigned long)(n * 1000000));
	}
}

