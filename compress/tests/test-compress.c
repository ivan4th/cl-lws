/* cscompress unit tests: round trips through both encodings, the
 * wire formats, and the failure returns. */

#include <stdlib.h>

#include "cscompress.h"
#include "test-util.h"

#define ENCODINGS { CSCOMPRESS_GZIP, CSCOMPRESS_ZSTD }

static unsigned char *
json_like(size_t len)
{
	static const char row[] =
		"[\"acc-state\",\"Accelerator state\",\"\",\"string\",\"ready\",1,"
		"null,null,null,\"state main\",10],";
	unsigned char *buf = malloc(len);
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)row[i % (sizeof(row) - 1)];
	return buf;
}

static unsigned char *
random_bytes(size_t len)
{
	unsigned char *buf = malloc(len);
	unsigned int x = 12345;
	size_t i;

	for (i = 0; i < len; i++) {
		x = x * 1103515245u + 12345u;
		buf[i] = (unsigned char)(x >> 16);
	}
	return buf;
}

/* compress at LEVEL, check the size against the bound, decompress and
 * compare; returns the compressed size */
static size_t
round_trip(int encoding, int level, const unsigned char *src, size_t len)
{
	size_t cap = cscompress_bound(encoding, len), n, m;
	unsigned char *dst = malloc(cap), *back = malloc(len + 1);

	TCHECK(cap > 0);
	n = cscompress(encoding, level, src, len, dst, cap);
	TCHECK(n > 0);
	TCHECK(n <= cap);
	m = cscompress_decompress(encoding, dst, n, back, len + 1);
	TCHECK_EQ(m, len);
	TCHECK(!memcmp(src, back, len));
	free(dst);
	free(back);
	return n;
}

static void
test_round_trips(void)
{
	int encodings[] = ENCODINGS;
	size_t sizes[] = { 0, 1, 100, 70000, 700000 };
	size_t e, s;

	for (e = 0; e < 2; e++)
		for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
			unsigned char *json = json_like(sizes[s]);
			unsigned char *noise = random_bytes(sizes[s]);
			size_t n;

			n = round_trip(encodings[e], 1, json, sizes[s]);
			/* the repetitive JSON shrinks a lot */
			if (sizes[s] >= 70000)
				TCHECK(n < sizes[s] / 10);
			/* incompressible input still fits the bound */
			round_trip(encodings[e], 1, noise, sizes[s]);
			round_trip(encodings[e], 3, json, sizes[s]);
			free(json);
			free(noise);
		}
}

static void
test_wire_formats(void)
{
	unsigned char *src = json_like(1000);
	unsigned char dst[2000];
	size_t n;

	/* RFC 1952: ID1 ID2 CM=8 (deflate) */
	n = cscompress(CSCOMPRESS_GZIP, 1, src, 1000, dst, sizeof(dst));
	TCHECK(n > 18);
	TCHECK_EQ(dst[0], 0x1f);
	TCHECK_EQ(dst[1], 0x8b);
	TCHECK_EQ(dst[2], 8);
	/* RFC 8878: magic 0xFD2FB528, little-endian */
	n = cscompress(CSCOMPRESS_ZSTD, 1, src, 1000, dst, sizeof(dst));
	TCHECK(n > 4);
	TCHECK_EQ(dst[0], 0x28);
	TCHECK_EQ(dst[1], 0xb5);
	TCHECK_EQ(dst[2], 0x2f);
	TCHECK_EQ(dst[3], 0xfd);
	free(src);
}

static void
test_failures(void)
{
	int encodings[] = ENCODINGS;
	unsigned char *src = random_bytes(10000);
	unsigned char dst[20000], back[20000];
	size_t e, n;

	TCHECK_EQ(cscompress_bound(0, 100), 0);
	TCHECK_EQ(cscompress_bound(3, 100), 0);
	TCHECK_EQ(cscompress(3, 1, src, 100, dst, sizeof(dst)), 0);
	TCHECK_EQ(cscompress_decompress(3, src, 100, dst, sizeof(dst)),
		  (size_t)-1);
	/* levels outside the library's range */
	TCHECK_EQ(cscompress(CSCOMPRESS_GZIP, 0, src, 100, dst, sizeof(dst)), 0);
	TCHECK_EQ(cscompress(CSCOMPRESS_GZIP, 10, src, 100, dst, sizeof(dst)), 0);
	TCHECK_EQ(cscompress(CSCOMPRESS_ZSTD, 0, src, 100, dst, sizeof(dst)), 0);
	TCHECK_EQ(cscompress(CSCOMPRESS_ZSTD, 1000, src, 100, dst, sizeof(dst)), 0);
	TCHECK_EQ(cscompress(CSCOMPRESS_ZSTD, CSCOMPRESS_ZSTD_MAX_LEVEL + 1, src, 100,
			     dst, sizeof(dst)), 0);
	TCHECK(cscompress(CSCOMPRESS_ZSTD, CSCOMPRESS_ZSTD_MAX_LEVEL, src, 100,
			  dst, sizeof(dst)) > 0);

	for (e = 0; e < 2; e++) {
		/* incompressible input into far too little room */
		TCHECK_EQ(cscompress(encodings[e], 1, src, 10000, dst, 100), 0);
		n = cscompress(encodings[e], 1, src, 10000, dst, sizeof(dst));
		TCHECK(n > 0);
		/* output room for all but the last byte */
		TCHECK_EQ(cscompress_decompress(encodings[e], dst, n, back, 9999),
			  (size_t)-1);
		/* truncated input */
		TCHECK_EQ(cscompress_decompress(encodings[e], dst, n - 1, back,
						sizeof(back)), (size_t)-1);
		/* trailing garbage */
		dst[n] = 0x55;
		TCHECK_EQ(cscompress_decompress(encodings[e], dst, n + 1, back,
						sizeof(back)), (size_t)-1);
		/* not compressed data at all */
		TCHECK_EQ(cscompress_decompress(encodings[e], src, 1000, back,
						sizeof(back)), (size_t)-1);
	}
	free(src);
}

TEST_MAIN(test_round_trips, test_wire_formats, test_failures)
