/* cscompress - one-shot gzip / zstd compression, see cscompress.h. */

#include <limits.h>
#include <string.h>

#include <zlib.h>
#include <zstd.h>

#include "cscompress.h"

/* windowBits 15 plus 16: a gzip wrapper instead of the zlib one */
#define GZIP_WINDOW_BITS (15 + 16)
/* the gzip wrapper is 18 bytes, the zlib one compressBound() counts 6 */
#define GZIP_EXTRA_OVERHEAD 12

static size_t
gzip_compress(int level, const void *src, size_t len, void *dst, size_t cap)
{
	z_stream zs;
	size_t out;
	int rc;

	if (level < 1 || level > 9 || len > UINT_MAX)
		return 0;
	if (cap > UINT_MAX)
		cap = UINT_MAX;

	memset(&zs, 0, sizeof(zs));
	if (deflateInit2(&zs, level, Z_DEFLATED, GZIP_WINDOW_BITS, 8,
			 Z_DEFAULT_STRATEGY) != Z_OK)
		return 0;
	zs.next_in = (Bytef *)src;
	zs.avail_in = (uInt)len;
	zs.next_out = (Bytef *)dst;
	zs.avail_out = (uInt)cap;
	/* with all of the input and the output space at once, Z_FINISH
	 * either completes the member or ran out of output space */
	rc = deflate(&zs, Z_FINISH);
	out = zs.total_out;
	deflateEnd(&zs);

	return rc == Z_STREAM_END ? out : 0;
}

static size_t
gzip_decompress(const void *src, size_t len, void *dst, size_t cap)
{
	z_stream zs;
	size_t out;
	int rc;

	if (len > UINT_MAX)
		return (size_t)-1;
	if (cap > UINT_MAX)
		cap = UINT_MAX;

	memset(&zs, 0, sizeof(zs));
	if (inflateInit2(&zs, GZIP_WINDOW_BITS) != Z_OK)
		return (size_t)-1;
	zs.next_in = (Bytef *)src;
	zs.avail_in = (uInt)len;
	zs.next_out = (Bytef *)dst;
	zs.avail_out = (uInt)cap;
	rc = inflate(&zs, Z_FINISH);
	out = zs.total_out;
	inflateEnd(&zs);

	/* exactly one member, nothing after it */
	return rc == Z_STREAM_END && !zs.avail_in ? out : (size_t)-1;
}

size_t
cscompress_bound(int encoding, size_t len)
{
	size_t bound;

	switch (encoding) {
	case CSCOMPRESS_GZIP:
		if (len > UINT_MAX)
			return 0;
		return (size_t)compressBound((uLong)len) + GZIP_EXTRA_OVERHEAD;
	case CSCOMPRESS_ZSTD:
		bound = ZSTD_compressBound(len);
		return ZSTD_isError(bound) ? 0 : bound;
	default:
		return 0;
	}
}

size_t
cscompress(int encoding, int level, const void *src, size_t len,
	   void *dst, size_t cap)
{
	size_t n;

	switch (encoding) {
	case CSCOMPRESS_GZIP:
		return gzip_compress(level, src, len, dst, cap);
	case CSCOMPRESS_ZSTD:
		/* no negative levels: ZSTD_minCLevel() is zstd 1.4+, and
		 * buster (the armhf build) has 1.3.8 */
		if (level < 1 || level > CSCOMPRESS_ZSTD_MAX_LEVEL)
			return 0;
		n = ZSTD_compress(dst, cap, src, len, level);
		return ZSTD_isError(n) ? 0 : n;
	default:
		return 0;
	}
}

size_t
cscompress_decompress(int encoding, const void *src, size_t len,
		      void *dst, size_t cap)
{
	size_t n;

	switch (encoding) {
	case CSCOMPRESS_GZIP:
		return gzip_decompress(src, len, dst, cap);
	case CSCOMPRESS_ZSTD:
		/* ZSTD_decompress() rejects trailing bytes that do not form
		 * another frame, and decodes every frame present */
		n = ZSTD_decompress(dst, cap, src, len);
		return ZSTD_isError(n) ? (size_t)-1 : n;
	default:
		return (size_t)-1;
	}
}
