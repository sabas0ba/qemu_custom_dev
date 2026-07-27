// SPDX-License-Identifier: GPL-2.0-only
/*
 * ppm2png - convert a binary PPM (P6) frame to a PNG.
 *
 * renderd writes PPM because it is trivial to produce and trivial to
 * verify byte by byte (tests/ppm_check.c). Browsers and GitHub do not
 * display PPM, so anything meant to be *looked at* — the example frames
 * in docs/, the frames CI uploads as artifacts — goes through here.
 *
 * Written out longhand rather than linking zlib, because the whole build
 * is "a C11 toolchain and GNU make" and one image converter is not worth
 * giving that up. That means a PNG writer plus enough of DEFLATE to fill
 * the IDAT chunk:
 *
 *   - per-row filtering (None/Sub/Up, picked by the usual minimum sum of
 *     absolute differences heuristic), which is what makes the flat-colour
 *     frames this project draws compress at all;
 *   - LZ77 with a 32 KiB window and a hash chain over 3-byte prefixes;
 *   - one static-Huffman block (RFC 1951 §3.2.6), so no code lengths have
 *     to be transmitted.
 *
 * Output is deterministic: the same PPM always produces the same bytes,
 * which is what lets scripts/make-doc-images.sh --check assert that the
 * committed images still match what the renderer draws.
 *
 * Usage: ppm2png IN.ppm OUT.png
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ output */

struct buf {
	unsigned char *p;
	size_t len, cap;
};

static void buf_put(struct buf *b, const void *data, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 4096;

		while (cap < b->len + n)
			cap *= 2;
		b->p = realloc(b->p, cap);
		if (!b->p) {
			fprintf(stderr, "ppm2png: out of memory\n");
			exit(1);
		}
		b->cap = cap;
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
}

static void buf_byte(struct buf *b, unsigned char c)
{
	buf_put(b, &c, 1);
}

/* -------------------------------------------------------- checksums */

static unsigned long crc_table[256];

static void crc_init(void)
{
	for (unsigned long n = 0; n < 256; n++) {
		unsigned long c = n;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static unsigned long crc32_of(const unsigned char *d, size_t n)
{
	unsigned long c = 0xffffffffUL;

	for (size_t i = 0; i < n; i++)
		c = crc_table[(c ^ d[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffUL;
}

static unsigned long adler32_of(const unsigned char *d, size_t n)
{
	unsigned long a = 1, b = 0;

	for (size_t i = 0; i < n; i++) {
		a = (a + d[i]) % 65521;
		b = (b + a) % 65521;
	}
	return (b << 16) | a;
}

/* ------------------------------------------------------------ deflate */

/*
 * DEFLATE packs bits into bytes starting at the least significant bit,
 * but Huffman codes go in most significant bit first. Keeping the two
 * operations separate is the only way to stay sane here.
 */
static struct buf *bit_out;
static unsigned long bit_buf;
static int bit_cnt;

static void putbits(unsigned long val, int n)
{
	bit_buf |= (val & ((1UL << n) - 1)) << bit_cnt;
	bit_cnt += n;
	while (bit_cnt >= 8) {
		buf_byte(bit_out, (unsigned char)(bit_buf & 0xff));
		bit_buf >>= 8;
		bit_cnt -= 8;
	}
}

static void puthuff(unsigned long code, int n)
{
	for (int i = n - 1; i >= 0; i--)
		putbits((code >> i) & 1, 1);
}

static void bits_flush(void)
{
	if (bit_cnt > 0)
		putbits(0, 8 - bit_cnt);
}

/* Fixed literal/length code, RFC 1951 §3.2.6. */
static void put_symbol(int sym)
{
	if (sym < 144)
		puthuff(0x30UL + sym, 8);
	else if (sym < 256)
		puthuff(0x190UL + (sym - 144), 9);
	else if (sym < 280)
		puthuff((unsigned long)(sym - 256), 7);
	else
		puthuff(0xc0UL + (sym - 280), 8);
}

static const int len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
	59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
	4, 5, 5, 5, 5, 0
};
static const int dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
	513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
	24577
};
static const int dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
	10, 11, 11, 12, 12, 13, 13
};

static void put_match(int len, int dist)
{
	int lc = 28, dc = 29;

	while (lc > 0 && len < len_base[lc])
		lc--;
	put_symbol(257 + lc);
	putbits((unsigned long)(len - len_base[lc]), len_extra[lc]);

	while (dc > 0 && dist < dist_base[dc])
		dc--;
	puthuff((unsigned long)dc, 5);
	putbits((unsigned long)(dist - dist_base[dc]), dist_extra[dc]);
}

#define WINDOW    32768
#define MIN_MATCH 3
#define MAX_MATCH 258
#define HASH_BITS 15
#define HASH_SIZE (1 << HASH_BITS)
/* How far down a hash chain to look. Longer costs time for a few bytes. */
#define MAX_CHAIN 64

static int hash3(const unsigned char *d)
{
	return (int)(((unsigned)d[0] << 10 ^ (unsigned)d[1] << 5 ^ d[2]) &
		     (HASH_SIZE - 1));
}

/* Compress src into one static-Huffman block appended to out. */
static void deflate_static(struct buf *out, const unsigned char *src,
			   size_t n)
{
	int *head = malloc(sizeof(int) * HASH_SIZE);
	int *prev = malloc(sizeof(int) * (n ? n : 1));
	size_t pos = 0;

	if (!head || !prev) {
		fprintf(stderr, "ppm2png: out of memory\n");
		exit(1);
	}
	for (int i = 0; i < HASH_SIZE; i++)
		head[i] = -1;

	bit_out = out;
	bit_buf = 0;
	bit_cnt = 0;
	putbits(1, 1);	/* BFINAL: this is the only block */
	putbits(1, 2);	/* BTYPE = 01, fixed Huffman codes */

	while (pos < n) {
		int best_len = 0, best_dist = 0;

		if (pos + MIN_MATCH <= n) {
			int h = hash3(src + pos);
			int cand = head[h];
			int chain = MAX_CHAIN;

			while (cand >= 0 && chain-- > 0) {
				size_t dist = pos - (size_t)cand;
				size_t len = 0;
				size_t max = n - pos;

				if (dist > WINDOW)
					break;
				if (max > MAX_MATCH)
					max = MAX_MATCH;
				while (len < max &&
				       src[cand + len] == src[pos + len])
					len++;
				if ((int)len > best_len) {
					best_len = (int)len;
					best_dist = (int)dist;
					if (best_len == MAX_MATCH)
						break;
				}
				cand = prev[cand];
			}
			/* Insert this position before moving on. */
			prev[pos] = head[h];
			head[h] = (int)pos;
		}

		if (best_len >= MIN_MATCH) {
			put_match(best_len, best_dist);
			/*
			 * Index the bytes the match covered too, or later
			 * positions lose the chains running through them.
			 */
			for (int i = 1; i < best_len; i++) {
				size_t p = pos + (size_t)i;

				if (p + MIN_MATCH <= n) {
					int h = hash3(src + p);

					prev[p] = head[h];
					head[h] = (int)p;
				}
			}
			pos += (size_t)best_len;
		} else {
			put_symbol(src[pos]);
			pos++;
		}
	}

	put_symbol(256);	/* end of block */
	bits_flush();
	free(head);
	free(prev);
}

/* ---------------------------------------------------------------- PNG */

static void put_be32(struct buf *b, unsigned long v)
{
	buf_byte(b, (unsigned char)(v >> 24));
	buf_byte(b, (unsigned char)(v >> 16));
	buf_byte(b, (unsigned char)(v >> 8));
	buf_byte(b, (unsigned char)v);
}

static void put_chunk(FILE *f, const char *type, const unsigned char *data,
		      size_t n)
{
	struct buf c = { 0 };
	unsigned char be[4];
	unsigned long crc;

	be[0] = (unsigned char)(n >> 24);
	be[1] = (unsigned char)(n >> 16);
	be[2] = (unsigned char)(n >> 8);
	be[3] = (unsigned char)n;
	fwrite(be, 1, 4, f);

	/* The CRC covers the type and the data, but not the length. */
	buf_put(&c, type, 4);
	if (n)
		buf_put(&c, data, n);
	fwrite(c.p, 1, c.len, f);

	crc = crc32_of(c.p, c.len);
	be[0] = (unsigned char)(crc >> 24);
	be[1] = (unsigned char)(crc >> 16);
	be[2] = (unsigned char)(crc >> 8);
	be[3] = (unsigned char)crc;
	fwrite(be, 1, 4, f);
	free(c.p);
}

/* Sum of absolute differences, treating filtered bytes as signed. */
static unsigned long score(const unsigned char *row, size_t n)
{
	unsigned long s = 0;

	for (size_t i = 0; i < n; i++)
		s += (unsigned long)(row[i] < 128 ? row[i] : 256 - row[i]);
	return s;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: ppm2png IN.ppm OUT.png\n");
		return 2;
	}

	FILE *in = fopen(argv[1], "rb");
	char magic[3] = { 0 };
	int w, h, maxval;

	if (!in) {
		perror(argv[1]);
		return 1;
	}
	if (fscanf(in, "%2s %d %d %d", magic, &w, &h, &maxval) != 4 ||
	    strcmp(magic, "P6") != 0 || maxval != 255 || w <= 0 || h <= 0 ||
	    fgetc(in) == EOF) {
		fprintf(stderr, "ppm2png: %s: unsupported or malformed PPM\n",
			argv[1]);
		fclose(in);
		return 1;
	}

	size_t stride = (size_t)w * 3;
	size_t npix = stride * (size_t)h;
	unsigned char *img = malloc(npix);

	if (!img) {
		fprintf(stderr, "ppm2png: out of memory\n");
		fclose(in);
		return 1;
	}
	if (fread(img, 1, npix, in) != npix) {
		fprintf(stderr, "ppm2png: %s: short read\n", argv[1]);
		fclose(in);
		free(img);
		return 1;
	}
	fclose(in);

	/* Filter each row, choosing between None, Sub and Up. */
	struct buf raw = { 0 };
	unsigned char *cand = malloc(stride);

	if (!cand) {
		fprintf(stderr, "ppm2png: out of memory\n");
		free(img);
		return 1;
	}
	for (int y = 0; y < h; y++) {
		const unsigned char *cur = img + (size_t)y * stride;
		const unsigned char *up = y ? cur - stride : NULL;
		unsigned long best = 0;
		int best_type = 0;

		for (int type = 0; type < 3; type++) {
			unsigned long s;

			for (size_t i = 0; i < stride; i++) {
				unsigned char pred = 0;

				if (type == 1 && i >= 3)
					pred = cur[i - 3];
				else if (type == 2 && up)
					pred = up[i];
				cand[i] = (unsigned char)(cur[i] - pred);
			}
			s = score(cand, stride);
			if (type == 0 || s < best) {
				best = s;
				best_type = type;
			}
		}

		buf_byte(&raw, (unsigned char)best_type);
		for (size_t i = 0; i < stride; i++) {
			unsigned char pred = 0;

			if (best_type == 1 && i >= 3)
				pred = cur[i - 3];
			else if (best_type == 2 && up)
				pred = up[i];
			buf_byte(&raw, (unsigned char)(cur[i] - pred));
		}
	}
	free(cand);

	/* zlib stream: 2-byte header, one deflate block, adler32 trailer. */
	struct buf idat = { 0 };

	buf_byte(&idat, 0x78);
	buf_byte(&idat, 0x9c);
	deflate_static(&idat, raw.p, raw.len);
	put_be32(&idat, adler32_of(raw.p, raw.len));

	crc_init();

	FILE *out = fopen(argv[2], "wb");

	if (!out) {
		perror(argv[2]);
		free(img);
		free(raw.p);
		free(idat.p);
		return 1;
	}

	static const unsigned char sig[8] = {
		0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
	};
	fwrite(sig, 1, sizeof(sig), out);

	unsigned char ihdr[13];

	ihdr[0] = (unsigned char)((unsigned)w >> 24);
	ihdr[1] = (unsigned char)((unsigned)w >> 16);
	ihdr[2] = (unsigned char)((unsigned)w >> 8);
	ihdr[3] = (unsigned char)w;
	ihdr[4] = (unsigned char)((unsigned)h >> 24);
	ihdr[5] = (unsigned char)((unsigned)h >> 16);
	ihdr[6] = (unsigned char)((unsigned)h >> 8);
	ihdr[7] = (unsigned char)h;
	ihdr[8] = 8;	/* bit depth */
	ihdr[9] = 2;	/* colour type 2 = truecolour RGB */
	ihdr[10] = 0;	/* compression: deflate */
	ihdr[11] = 0;	/* filter method 0 */
	ihdr[12] = 0;	/* no interlacing */
	put_chunk(out, "IHDR", ihdr, sizeof(ihdr));
	put_chunk(out, "IDAT", idat.p, idat.len);
	put_chunk(out, "IEND", NULL, 0);

	if (fclose(out) != 0) {
		perror(argv[2]);
		free(img);
		free(raw.p);
		free(idat.p);
		return 1;
	}
	free(img);
	free(raw.p);
	free(idat.p);
	return 0;
}
