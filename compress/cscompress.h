/* cscompress - one-shot gzip / zstd compression of in-memory buffers,
 * for HTTP response bodies (the LWS-COMPRESS system).
 *
 * The whole body is known up front, so it is compressed in one call
 * and sent with its real Content-Length: no streaming state, no
 * chunked transfer coding.  Pure functions of their arguments, safe to
 * call from any thread. */

#ifndef CSCOMPRESS_H_INCLUDED
#define CSCOMPRESS_H_INCLUDED

#include <stddef.h>

enum cscompress_encoding {
	CSCOMPRESS_GZIP = 1,	/* a gzip member, Content-Encoding: gzip */
	CSCOMPRESS_ZSTD = 2,	/* a zstd frame, Content-Encoding: zstd */
};

/* An upper bound of the compressed size of LEN input bytes, or 0 for
 * an unknown encoding or an input too large for it. */
size_t cscompress_bound(int encoding, size_t len);

/* The highest zstd level cscompress() takes: the levels above it use
 * windows over 8 MB, which RFC 9659 rules out for Content-Encoding:
 * zstd and browsers refuse to decode. */
#define CSCOMPRESS_ZSTD_MAX_LEVEL 19

/* Compress SRC[0..LEN) into DST, which has room for CAP bytes, at
 * LEVEL: 1..9 for gzip, 1..CSCOMPRESS_ZSTD_MAX_LEVEL for zstd.
 * Returns the compressed size, or 0 on failure: an unknown encoding or
 * level, CAP too small (a CAP of cscompress_bound() is always enough),
 * or out of memory.  No successful result is 0 bytes long. */
size_t cscompress(int encoding, int level, const void *src, size_t len,
		  void *dst, size_t cap);

/* Decompress SRC[0..LEN), one complete gzip member or complete zstd
 * frames, into DST, which has room for CAP bytes.  Returns the
 * decompressed size, or (size_t)-1 on failure: corrupt or truncated
 * input, trailing bytes, or CAP too small.  For tests and
 * diagnostics. */
size_t cscompress_decompress(int encoding, const void *src, size_t len,
			     void *dst, size_t cap);

#endif
