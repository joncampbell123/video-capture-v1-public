/* UTF-8 and UTF-16 Unicode encoder/decoder functions.
 * Impact Studio Pro utilities - Text processors
 * (C) 2008-2011 Impact Studio Pro ALL RIGHTS RESERVED.
 * Written by Jonathan Campbell
 *
 * For each encode/decode function you pass the address
 * of the char pointer itself, not the value of the char
 * pointer. This is how the function uses the pointer,
 * and updates it before returning it to you. The code
 * is written to always move forward in memory, and to
 * never step past the 'fence' pointer value (returning
 * errors rather than cause memory access violations).
 *
 * This code is not for wussies who can't handle pointers.
 * If pointers scare you, then by all means run crying
 * back to the comfortable managed world of Java/C# you
 * big baby :)
 */

#include "unicode.h"

/* [doc] utf8_encode
 *
 * Encode unicode character 'code' as UTF-8 ASCII string
 *
 * Parameters:
 *
 *    p = pointer to a char* pointer where the output will be written.
 *        on return the pointer will have been updated.
 *
 *    fence = first byte past the end of the buffer. the function will not
 *            write the output and update the pointer if doing so would bring
 *            it past this point, in order to prevent buffer overrun and
 *            possible memory corruption issues
 *
 *    c = unicode character to encode
 *
 * Warning:
 *
 *    Remember that one encoded UTF-8 character can occupy anywhere between
 *    1 to 4 bytes. Do not assume one byte = one char. Use the fence pointer
 *    to prevent buffer overruns and to know when the buffer should be
 *    emptied and refilled.
 * 
 */
int utf8_encode(char **ptr,char *fence,uint32_t code) {
	int uchar_size=1;
	char *p = *ptr;

	if (!p) return UTF8ERR_NO_ROOM;
	if (code >= (uint32_t)0x80000000UL) return UTF8ERR_INVALID;
	if (p >= fence) return UTF8ERR_NO_ROOM;

	if (code >= 0x4000000) uchar_size = 6;
	else if (code >= 0x200000) uchar_size = 5;
	else if (code >= 0x10000) uchar_size = 4;
	else if (code >= 0x800) uchar_size = 3;
	else if (code >= 0x80) uchar_size = 2;

	if ((p+uchar_size) > fence) return UTF8ERR_NO_ROOM;

	switch (uchar_size) {
		case 1:	*p++ = (char)code;
			break;
		case 2:	*p++ = (char)(0xC0 | (code >> 6));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
		case 3:	*p++ = (char)(0xE0 | (code >> 12));
			*p++ = (char)(0x80 | ((code >> 6) & 0x3F));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
		case 4:	*p++ = (char)(0xF0 | (code >> 18));
			*p++ = (char)(0x80 | ((code >> 12) & 0x3F));
			*p++ = (char)(0x80 | ((code >> 6) & 0x3F));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
		case 5:	*p++ = (char)(0xF8 | (code >> 24));
			*p++ = (char)(0x80 | ((code >> 18) & 0x3F));
			*p++ = (char)(0x80 | ((code >> 12) & 0x3F));
			*p++ = (char)(0x80 | ((code >> 6) & 0x3F));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
		case 6:	*p++ = (char)(0xFC | (code >> 30));
			*p++ = (char)(0x80 | ((code >> 24) & 0x3F));
			*p++ = (char)(0x80 | ((code >> 18) & 0x3F));
			*p++ = (char)(0x80 | ((code >> 12) & 0x3F));
			*p++ = (char)(0x80 | ((code >> 6) & 0x3F));
			*p++ = (char)(0x80 | (code & 0x3F));
			break;
	};

	*ptr = p;
	return 0;
}

/* [doc] utf8_decode
 *
 * Decode one UTF-8 unicode char from an ASCII string
 *
 * Parameters:
 *
 *    p = pointer to a char* pointer where the input will be read from.
 *        on return the pointer will have been updated.
 *
 *    fence = first byte past the end of the buffer. the function will not
 *            read from the pointer and update the pointer if doing so would
 *            bring it past this point, in order to prevent buffer overrun
 *            and memory access violation issues.
 *
 * Warning:
 *
 *    One UTF-8 character may be between 1 to 4 bytes long. This function will
 *    not decode a character if doing so will reach past the end of the buffer
 * 
 */
int utf8_decode(const char **ptr,const char *fence) {
	const char *p = *ptr;
	int uchar_size=1;
	int ret = 0,c;

	if (!p) return UTF8ERR_NO_ROOM;
	if (p >= fence) return UTF8ERR_NO_ROOM;

	ret = (unsigned char)(*p);
	if (ret >= 0xFE) { p++; return UTF8ERR_INVALID; }
	else if (ret >= 0xFC) uchar_size=6;
	else if (ret >= 0xF8) uchar_size=5;
	else if (ret >= 0xF0) uchar_size=4;
	else if (ret >= 0xE0) uchar_size=3;
	else if (ret >= 0xC0) uchar_size=2;
	else if (ret >= 0x80) { p++; return UTF8ERR_INVALID; }

	if ((p+uchar_size) > fence)
		return UTF8ERR_NO_ROOM;

	switch (uchar_size) {
		case 1:	p++;
			break;
		case 2:	ret = (ret&0x1F)<<6; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
		case 3:	ret = (ret&0xF)<<12; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<6;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
		case 4:	ret = (ret&0x7)<<18; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<12;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<6;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
		case 5:	ret = (ret&0x3)<<24; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<18;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<12;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<6;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
		case 6:	ret = (ret&0x1)<<30; p++;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<24;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<18;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<12;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= (c&0x3F)<<6;
			c = (unsigned char)(*p++); if ((c&0xC0) != 0x80) return UTF8ERR_INVALID;
			ret |= c&0x3F;
			break;
	};

	*ptr = p;
	return ret;
}

/* [doc] utf16le_encode
 *
 * Encode unicode character 'code' as UTF-16 (little endian)
 *
 * Parameters:
 *
 *    p = pointer to a char* pointer where the output will be written.
 *        on return the pointer will have been updated.
 *
 *    fence = first byte past the end of the buffer. the function will not
 *            write the output and update the pointer if doing so would bring
 *            it past this point, in order to prevent buffer overrun and
 *            possible memory corruption issues
 *
 *    c = unicode character to encode
 *
 * Warning:
 *
 *    UTF-16 output is generally 2 bytes long, but it can be 4 bytes long if
 *    a surrogate pair is needed to encode the unicode char.
 * 
 */
int utf16le_encode(char **ptr,char *fence,uint32_t code) {
	char *p = *ptr;

	if (!p) return UTF8ERR_NO_ROOM;
	if (code > 0x10FFFF) return UTF8ERR_INVALID;
	if (code > 0xFFFF) { /* UTF-16 surrogate pair */
		uint32_t lo = (code - 0x10000) & 0x3FF;
		uint32_t hi = ((code - 0x10000) >> 10) & 0x3FF;
		if ((p+2+2) > fence) return UTF8ERR_NO_ROOM;
		*p++ = (char)( (hi+0xD800)       & 0xFF);
		*p++ = (char)(((hi+0xD800) >> 8) & 0xFF);
		*p++ = (char)( (lo+0xDC00)       & 0xFF);
		*p++ = (char)(((lo+0xDC00) >> 8) & 0xFF);
	}
	else if ((code&0xF800) == 0xD800) { /* do not allow accidental surrogate pairs (0xD800-0xDFFF) */
		return UTF8ERR_INVALID;
	}
	else {
		if ((p+2) > fence) return UTF8ERR_NO_ROOM;
		*p++ = (char)( code       & 0xFF);
		*p++ = (char)((code >> 8) & 0xFF);
	}

	*ptr = p;
	return 0;
}

/* [doc] utf16le_decode
 *
 * Decode one UTF-16 (little endian) unicode char from a UTF-16 string
 *
 * Parameters:
 *
 *    p = pointer to a char* pointer where the input will be read from.
 *        on return the pointer will have been updated.
 *
 *    fence = first byte past the end of the buffer. the function will not
 *            read from the pointer and update the pointer if doing so would
 *            bring it past this point, in order to prevent buffer overrun
 *            and memory access violation issues.
 *
 * Warning:
 *
 *    UTF-16 characters are generally 2 bytes long, but they can be 4
 *    bytes long if stored in a surrogate pair. This function will not
 *    decode the char if doing so would reach beyond the end of the buffer
 * 
 */

int utf16le_decode(const char **ptr,const char *fence) {
	const char *p = *ptr;
	int ret,b=2;

	if (!p) return UTF8ERR_NO_ROOM;
	if ((p+1) >= fence) return UTF8ERR_NO_ROOM;

	ret = (unsigned char)p[0];
	ret |= ((unsigned int)((unsigned char)p[1])) << 8;
	if (ret >= 0xD800 && ret <= 0xDBFF)
		b=4;
	else if (ret >= 0xDC00 && ret <= 0xDFFF)
		{ p++; return UTF8ERR_INVALID; }

	if ((p+b) > fence)
		return UTF8ERR_NO_ROOM;

	p += 2;
	if (ret >= 0xD800 && ret <= 0xDBFF) {
		/* decode surrogate pair */
		int hi = ret & 0x3FF;
		int lo = (unsigned char)p[0];
		lo |= ((unsigned int)((unsigned char)p[1])) << 8;
		p += 2;
		if (lo < 0xDC00 || lo > 0xDFFF) return UTF8ERR_INVALID;
		lo &= 0x3FF;
		ret = ((hi << 10) | lo) + 0x10000;
	}

	*ptr = p;
	return ret;
}
