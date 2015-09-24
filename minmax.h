
#ifndef __ISP_UTILS_V4_MISC_MINMAX_H
#define __ISP_UTILS_V4_MISC_MINMAX_H

#if !defined(min) && !defined(__cplusplus)
# define min(a,b)	((a) < (b) ? (a) : (b))
#endif

#if !defined(max) && !defined(__cplusplus)
# define max(a,b)	((a) > (b) ? (a) : (b))
#endif

static inline int _int_minmax(const int v,const int vmin,const int vmax) {
	if (v < vmin) return vmin;
	else if (v > vmax) return vmax;
	return v;
}

#endif /* __ISP_UTILS_V4_MISC_MINMAX_H */

