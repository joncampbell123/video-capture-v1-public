#ifndef ____UNICODE_H
#define ____UNICODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	UTF8ERR_INVALID=-1,
	UTF8ERR_NO_ROOM=-2
};

#ifndef UNICODE_BOM
#define UNICODE_BOM 0xFEFF
#endif

int utf8_encode(char **ptr,char *fence,uint32_t code);
int utf8_decode(const char **ptr,const char *fence);
int utf16le_encode(char **ptr,char *fence,uint32_t code);
int utf16le_decode(const char **ptr,const char *fence);

typedef char utf8_t;
typedef uint16_t utf16_t;

#ifdef __cplusplus
}
#endif

#endif

