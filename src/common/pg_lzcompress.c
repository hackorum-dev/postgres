/* ----------
 * pg_lzcompress.c -
 *
 *		This is an implementation of LZ compression for PostgreSQL.
 *		It uses a simple history table and generates 2-3 byte tags
 *		capable of backward copy information for 3-273 bytes with
 *		a max offset of 4095.
 *
 *		Entry routines:
 *
 *			int32
 *			pglz_compress(const char *source, int32 slen, char *dest,
 *						  const PGLZ_Strategy *strategy);
 *
 *				source is the input data to be compressed.
 *
 *				slen is the length of the input data.
 *
 *				dest is the output area for the compressed result.
 *					It must be at least as big as PGLZ_MAX_OUTPUT(slen).
 *
 *				strategy is a pointer to some information controlling
 *					the compression algorithm. If NULL, the compiled
 *					in default strategy is used.
 *
 *				The return value is the number of bytes written in the
 *				buffer dest, or -1 if compression fails; in the latter
 *				case the contents of dest are undefined.
 *
 *			int32
 *			pglz_decompress(const char *source, int32 slen, char *dest,
 *							int32 rawsize, bool check_complete)
 *
 *				source is the compressed input.
 *
 *				slen is the length of the compressed input.
 *
 *				dest is the area where the uncompressed data will be
 *					written to. It is the callers responsibility to
 *					provide enough space.
 *
 *					The data is written to buff exactly as it was handed
 *					to pglz_compress(). No terminating zero byte is added.
 *
 *				rawsize is the length of the uncompressed data.
 *
 *				check_complete is a flag to let us know if -1 should be
 *					returned in cases where we don't reach the end of the
 *					source or dest buffers, or not.  This should be false
 *					if the caller is asking for only a partial result and
 *					true otherwise.
 *
 *				The return value is the number of bytes written in the
 *				buffer dest, or -1 if decompression fails.
 *
 *		The decompression algorithm and internal data format:
 *
 *			It is made with the compressed data itself.
 *
 *			The data representation is easiest explained by describing
 *			the process of decompression.
 *
 *			If compressed_size == rawsize, then the data
 *			is stored uncompressed as plain bytes. Thus, the decompressor
 *			simply copies rawsize bytes to the destination.
 *
 *			Otherwise the first byte tells what to do the next 8 times.
 *			We call this the control byte.
 *
 *			An unset bit in the control byte means, that one uncompressed
 *			byte follows, which is copied from input to output.
 *
 *			A set bit in the control byte means, that a tag of 2-3 bytes
 *			follows. A tag contains information to copy some bytes, that
 *			are already in the output buffer, to the current location in
 *			the output. Let's call the three tag bytes T1, T2 and T3. The
 *			position of the data to copy is coded as an offset from the
 *			actual output position.
 *
 *			The offset is in the upper nibble of T1 and in T2.
 *			The length is in the lower nibble of T1.
 *
 *			So the 16 bits of a 2 byte tag are coded as
 *
 *				7---T1--0  7---T2--0
 *				OOOO LLLL  OOOO OOOO
 *
 *			This limits the offset to 1-4095 (12 bits) and the length
 *			to 3-18 (4 bits) because 3 is always added to it. To emit
 *			a tag of 2 bytes with a length of 2 only saves one control
 *			bit. But we lose one byte in the possible length of a tag.
 *
 *			In the actual implementation, the 2 byte tag's length is
 *			limited to 3-17, because the value 0xF in the length nibble
 *			has special meaning. It means, that the next following
 *			byte (T3) has to be added to the length value of 18. That
 *			makes total limits of 1-4095 for offset and 3-273 for length.
 *
 *			Now that we have successfully decoded a tag. We simply copy
 *			the output that occurred <offset> bytes back to the current
 *			output location in the specified <length>. Thus, a
 *			sequence of 200 spaces (think about bpchar fields) could be
 *			coded in 4 bytes. One literal space and a three byte tag to
 *			copy 199 bytes with a -1 offset. Whow - that's a compression
 *			rate of 98%! Well, the implementation needs to save the
 *			original data size too, so we need another 4 bytes for it
 *			and end up with a total compression rate of 96%, what's still
 *			worth a Whow.
 *
 *			Version 2 streams start with a marker that is an invalid legacy
 *			offset-zero match.  The marker is followed by a 256-bit literal
 *			alphabet bitmap and packed canonical Huffman code lengths.  The
 *			remaining bit stream uses one bit to distinguish literals from
 *			matches.  Literals use their Huffman code.  Matches use an 8- or
 *			16-bit offset and a compact variable-length match length, allowing
 *			a 64KB history window and matches up to 65535 bytes.  New decoders
 *			continue to accept the legacy representation described above.
 *
 *			Version 3 uses the same LZ77 parser and limits, but stores literals
 *			as byte-aligned runs.  A one-byte sequence header contains four-bit
 *			literal and match lengths, extended with ULEB128 when necessary.
 *			Each match has a fixed two-byte offset.  This representation gives
 *			up some literal entropy coding in exchange for a much simpler decode
 *			loop.  If it cannot meet the requested compression rate, the encoder
 *			uses version 2 instead.
 *
 *		The compression algorithm
 *
 *			The following uses numbers used in the default strategy.
 *
 *			The compressor works best for attributes of a size between
 *			1K and 1M. For smaller items there's not that much chance of
 *			redundancy in the character sequence (except for large areas
 *			of identical bytes like trailing spaces) and for bigger ones
 *			our 4K maximum look-back distance is too small.
 *
 *			The compressor creates a table for lists of positions.
 *			For each input position (except the last 3), a hash key is
 *			built from the 4 next input bytes and the position remembered
 *			in the appropriate list. Thus, the table points to linked
 *			lists of likely to be at least in the first 4 characters
 *			matching strings. This is done on the fly while the input
 *			is compressed into the output area.  Table entries are only
 *			kept for the last 4096 input positions, since we cannot use
 *			back-pointers larger than that anyway.  The size of the hash
 *			table is chosen based on the size of the input - a larger table
 *			has a larger startup cost, as it needs to be initialized to
 *			zero, but reduces the number of hash collisions on long inputs.
 *
 *			For each byte in the input, its hash key (built from this
 *			byte and the next 3) is used to find the appropriate list
 *			in the table. The lists remember the positions of all bytes
 *			that had the same hash key in the past in increasing backward
 *			offset order. Now for all entries in the used lists, the
 *			match length is computed by comparing the characters from the
 *			entries position with the characters from the actual input
 *			position.
 *
 *			The compressor starts with a so called "good_match" of 128.
 *			It is a "prefer speed against compression ratio" optimizer.
 *			So if the first entry looked at already has 128 or more
 *			matching characters, the lookup stops and that position is
 *			used for the next tag in the output.
 *
 *			For each subsequent entry in the history list, the "good_match"
 *			is lowered by 10%. So the compressor will be more happy with
 *			short matches the further it has to go back in the history.
 *			Another "speed against ratio" preference characteristic of
 *			the algorithm.
 *
 *			Thus there are 3 stop conditions for the lookup of matches:
 *
 *				- a match >= good_match is found
 *				- there are no more history entries to look at
 *				- the next history entry is already too far back
 *				  to be coded into a tag.
 *
 *			Finally the match algorithm checks that at least a match
 *			of 3 or more bytes has been found, because that is the smallest
 *			amount of copy information to code into a tag. If so, a tag
 *			is omitted and all the input bytes covered by that are just
 *			scanned for the history add's, otherwise a literal character
 *			is omitted and only his history entry added.
 *
 *		Acknowledgments:
 *
 *			Many thanks to Adisak Pochanayon, who's article about SLZ
 *			inspired me to write the PostgreSQL compression this way.
 *
 *			Jan Wieck
 *
 * Copyright (c) 1999-2026, PostgreSQL Global Development Group
 *
 * src/common/pg_lzcompress.c
 * ----------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <limits.h>

#ifdef USE_AVX2_WITH_RUNTIME_CHECK
#include <immintrin.h>
#endif

#include "common/pg_lzcompress.h"
#include "port/pg_bswap.h"
#include "port/pg_bitutils.h"
#include "port/pg_cpu.h"
#include "port/simd.h"
#ifndef FRONTEND
#include "utils/memutils.h"
#endif


/* ----------
 * Local definitions
 * ----------
 */
#define PGLZ_MAX_HISTORY_LISTS	8192	/* must be power of 2 */
#define PGLZ_HISTORY_SIZE		4096
#define PGLZ_MAX_MATCH			273
#define PGLZ_FAST_MATCH_PROBES	4
#define PGLZ_FULL_MATCH_PROBES	8
#define PGLZ_PROBE_MIN_INPUT		(PGLZ_HISTORY_SIZE * 8)
#define PGLZ_PROBE_SAMPLE_SIZE	64

/*
 * Versions 2 and 3 use a fast single-candidate LZ77 parser with a 64KB window.
 * Their magic starts with a legacy control byte and an offset-zero match,
 * which can never begin a valid legacy stream, so the decompressor can
 * distinguish the formats without ambiguity.
 */
#define PGLZ_V2_MIN_INPUT		(PGLZ_HISTORY_SIZE * 8)
#define PGLZ_V2_HASH_BITS		14
#define PGLZ_V2_HASH_SIZE		(1 << PGLZ_V2_HASH_BITS)
#define PGLZ_V2_WINDOW_SIZE		65535
#define PGLZ_V2_MIN_MATCH		6
#define PGLZ_V2_MAX_MATCH		65535
#define PGLZ_V2_HUFF_BITS		13
#define PGLZ_V2_DECODE_BITS		11
#define PGLZ_V2_DECODE_EXTRA		(PGLZ_V2_HUFF_BITS - PGLZ_V2_DECODE_BITS)
#define PGLZ_V2_SUBTABLE_SIZE	(1 << PGLZ_V2_DECODE_EXTRA)
#define PGLZ_V2_MAX_SUBTABLES	256
#define PGLZ_V2_SUBTABLE_FLAG	0x80000000U
#define PGLZ_V2_LITERAL_PAIR_FLAG 0x40000000U
#define PGLZ_V2_PAIR_SYMBOL_SHIFT 16
#define PGLZ_V2_PAIR_LENGTH_SHIFT 24
#define PGLZ_V2_BITMAP_SIZE		32
#define PGLZ_V2_MAX_HEADER		(7 + PGLZ_V2_BITMAP_SIZE + 128)

/*
 * Version 3 groups literals into byte-aligned runs.  The sequence header is:
 *
 *	7..4	literal length, with 15 followed by a ULEB128 extension
 *	3..0	match length minus PGLZ_V2_MIN_MATCH, extended at 15
 *
 * Every sequence except the last literal-only one is followed by a two-byte
 * match offset.  Fixed-width offsets and byte-aligned literals keep the hot
 * decode path branch-light, while the two four-bit lengths make extensions
 * uncommon.
 */
#define PGLZ_V3_LITERAL_SHIFT	4
#define PGLZ_V3_LITERAL_MASK	0x0f
#define PGLZ_V3_MATCH_MASK		0x0f
#define PGLZ_V3_MAX_HEADER		(7 + 6)
#define PGLZ_V3_NO_MATCH		(-2)

static const unsigned char pglz_v2_magic[] = {
	0x01, 0x00, 0x00, 'P', 'G', 'L', '2'
};

static const unsigned char pglz_v3_magic[] = {
	0x01, 0x00, 0x00, 'P', 'G', 'L', '3'
};

#ifndef FRONTEND
#define PGLZ_ALLOC(size)		palloc(size)
#define PGLZ_FREE(ptr)			pfree(ptr)
#else
#define PGLZ_ALLOC(size)		malloc(size)
#define PGLZ_FREE(ptr)			free(ptr)
#endif

typedef struct PGLZ_BitWriter
{
	unsigned char *ptr;
	unsigned char *end;
	uint64		bits;
	int			nbits;
	bool		failed;
} PGLZ_BitWriter;

typedef struct PGLZ_BitReader
{
	const unsigned char *ptr;
	const unsigned char *end;
	uint64		bits;
	int			nbits;
} PGLZ_BitReader;

typedef struct PGLZ_HuffNode
{
	uint64		frequency;
	int16		parent;
	uint16		min_symbol;
} PGLZ_HuffNode;

typedef struct PGLZ_TokenWriter
{
	unsigned char *ptr;
	unsigned char *end;
	unsigned char *control;
	unsigned char mask;
	int			items_left;
} PGLZ_TokenWriter;

typedef struct PGLZ_TokenReader
{
	const unsigned char *ptr;
	const unsigned char *end;
	unsigned char control;
	int			items_left;
} PGLZ_TokenReader;

static int32 pglz_decompress_legacy(const char *source, int32 slen,
									char *dest, int32 rawsize,
									bool check_complete);


/* ----------
 * PGLZ_HistEntry -
 *
 *		Linked list for the backward history lookup
 *
 * All the entries sharing a hash key are linked from newest to oldest.  Links
 * are indexes into hist_entries rather than pointers.  Entries live in a
 * fixed-size ring, and overwritten links are recognized during lookup from
 * their hash key and ring distance.
 * ----------
 */
typedef struct PGLZ_HistEntry
{
	uint16		next;			/* links for my hash key's list */
	uint16		hindex;			/* my current hash key */
} PGLZ_HistEntry;


/* ----------
 * The provided standard strategies
 * ----------
 */
static const PGLZ_Strategy strategy_default_data = {
	32,							/* Data chunks less than 32 bytes are not
								 * compressed */
	INT_MAX,					/* No upper limit on what we'll try to
								 * compress */
	25,							/* Require 25% compression rate, or not worth
								 * it */
	1024,						/* Give up if no compression in the first 1KB */
	128,						/* Stop history lookup if a match of 128 bytes
								 * is found */
	10							/* Lower good match size by 10% at every loop
								 * iteration */
};
const PGLZ_Strategy *const PGLZ_strategy_default = &strategy_default_data;


static const PGLZ_Strategy strategy_always_data = {
	0,							/* Chunks of any size are compressed */
	INT_MAX,
	0,							/* It's enough to save one single byte */
	INT_MAX,					/* Never give up early */
	128,						/* Stop history lookup if a match of 128 bytes
								 * is found */
	6							/* Look harder for a good match */
};
const PGLZ_Strategy *const PGLZ_strategy_always = &strategy_always_data;


/* ----------
 * Statically allocated work arrays for history
 * ----------
 */
static uint16 hist_start[PGLZ_MAX_HISTORY_LISTS];
static PGLZ_HistEntry hist_entries[PGLZ_HISTORY_SIZE + 1];

/*
 * Element 0 in hist_entries is unused, and means 'invalid'.
 */
#define INVALID_ENTRY			0

/* ----------
 * pglz_hist_idx -
 *
 *		Computes the history table slot for the lookup by the next 4
 *		characters in the input.
 *
 * NB: because we use the next 4 characters, we are not guaranteed to
 * find 3-character matches; they very possibly will be in the wrong
 * hash list.  This seems an acceptable tradeoff for spreading out the
 * hash keys more.
 *
 * Multiplication by the golden-ratio constant spreads similar sequences
 * across the table.  The high 13 bits select among the maximum 8192 lists;
 * the mask adapts that result for the smaller tables used with short inputs.
 * ----------
 */
static inline uint32
pglz_hist_sequence(const char *s)
{
	return ((uint32) (uint8) s[0]) |
		((uint32) (uint8) s[1] << 8) |
		((uint32) (uint8) s[2] << 16) |
		((uint32) (uint8) s[3] << 24);
}

static inline int
pglz_hist_idx(uint32 sequence, int mask)
{
	uint32		hash = sequence * 0x9E3779B1U;

	return (hash >> 19) & mask;
}

static inline int
pglz_hist_advance(uint32 *sequence, const char *s, const char *end, int mask)
{
	if (end - s < 4)
	{
		*sequence = (uint8) s[0];
		return *sequence & mask;
	}

	/* Drop the previous first byte and append the new fourth byte. */
	*sequence = (*sequence >> 8) | ((uint32) (uint8) s[3] << 24);
	return pglz_hist_idx(*sequence, mask);
}

static inline bool
pglz_prefix_has_nul(const char *source)
{
	const Size	zero_ones = ~(Size) 0 / UCHAR_MAX;
	const Size	zero_highs = zero_ones << (BITS_PER_BYTE - 1);

	/* Detect zero bytes a machine word at a time without a library call. */
	for (int i = 0; i < PGLZ_PROBE_SAMPLE_SIZE; i += sizeof(Size))
	{
		Size		word;

		memcpy(&word, source + i, sizeof(word));
		if (((word - zero_ones) & ~word & zero_highs) != 0)
			return true;
	}

	return false;
}

static inline void
pglz_bitwriter_init(PGLZ_BitWriter *writer, unsigned char *dest,
					unsigned char *end)
{
	writer->ptr = dest;
	writer->end = end;
	writer->bits = 0;
	writer->nbits = 0;
	writer->failed = false;
}

static inline void
pglz_bitwriter_put(PGLZ_BitWriter *writer, uint32 value, int nbits)
{
	uint64		mask;

	Assert(nbits > 0 && nbits <= 24);
	mask = (((uint64) 1) << nbits) - 1;
	writer->bits = (writer->bits << nbits) | (value & mask);
	writer->nbits += nbits;

	while (writer->nbits >= 8)
	{
		int			shift = writer->nbits - 8;

		if (writer->ptr >= writer->end)
		{
			writer->failed = true;
			return;
		}
		*writer->ptr++ = (unsigned char) (writer->bits >> shift);
		writer->nbits -= 8;
		if (writer->nbits == 0)
			writer->bits = 0;
		else
			writer->bits &= (((uint64) 1) << writer->nbits) - 1;
	}
}

static inline bool
pglz_bitwriter_finish(PGLZ_BitWriter *writer)
{
	if (writer->failed)
		return false;
	if (writer->nbits > 0)
	{
		if (writer->ptr >= writer->end)
			return false;
		*writer->ptr++ = (unsigned char) (writer->bits << (8 - writer->nbits));
		writer->bits = 0;
		writer->nbits = 0;
	}
	return true;
}

static inline void
pglz_bitreader_init(PGLZ_BitReader *reader, const unsigned char *source,
					const unsigned char *end)
{
	reader->ptr = source;
	reader->end = end;
	reader->bits = 0;
	reader->nbits = 0;
}

/* Return an index padded with zero bits without consuming it. */
static inline uint32
pglz_bitreader_peek_padded(PGLZ_BitReader *reader, int nbits)
{
	uint64		mask;

	Assert(nbits > 0 && nbits <= 24);
	if (reader->nbits < nbits && reader->end - reader->ptr >= 4)
	{
		uint32		word;

		memcpy(&word, reader->ptr, sizeof(word));
		reader->bits = (reader->bits << 32) | pg_ntoh32(word);
		reader->ptr += sizeof(word);
		reader->nbits += 32;
	}
	while (reader->nbits < nbits && reader->ptr < reader->end)
	{
		reader->bits = (reader->bits << 8) | *reader->ptr++;
		reader->nbits += 8;
	}
	mask = (((uint64) 1) << nbits) - 1;
	if (reader->nbits >= nbits)
		return (reader->bits >> (reader->nbits - nbits)) & mask;
	if (reader->nbits == 0)
		return 0;
	return (reader->bits & ((((uint64) 1) << reader->nbits) - 1)) <<
		(nbits - reader->nbits);
}

static inline bool
pglz_bitreader_drop(PGLZ_BitReader *reader, int nbits)
{
	if (reader->nbits < nbits)
		return false;
	reader->nbits -= nbits;
	return true;
}

static inline bool
pglz_huff_node_less(const PGLZ_HuffNode *nodes, int left, int right)
{
	if (nodes[left].frequency != nodes[right].frequency)
		return nodes[left].frequency < nodes[right].frequency;
	return nodes[left].min_symbol < nodes[right].min_symbol;
}

static void
pglz_huff_heap_push(int *heap, int *heap_size, const PGLZ_HuffNode *nodes,
					int node)
{
	int			child = ++(*heap_size);

	while (child > 1)
	{
		int			parent = child / 2;

		if (!pglz_huff_node_less(nodes, node, heap[parent]))
			break;
		heap[child] = heap[parent];
		child = parent;
	}
	heap[child] = node;
}

static int
pglz_huff_heap_pop(int *heap, int *heap_size, const PGLZ_HuffNode *nodes)
{
	int			result = heap[1];
	int			last = heap[(*heap_size)--];
	int			parent = 1;

	while (parent * 2 <= *heap_size)
	{
		int			child = parent * 2;

		if (child < *heap_size &&
			pglz_huff_node_less(nodes, heap[child + 1], heap[child]))
			child++;
		if (!pglz_huff_node_less(nodes, heap[child], last))
			break;
		heap[parent] = heap[child];
		parent = child;
	}
	if (*heap_size > 0)
		heap[parent] = last;
	return result;
}

/* Validate lengths and assign canonical Huffman codes. */
static bool
pglz_huffman_codes(const uint8 *lengths, uint16 *codes, int *nsymbolsp)
{
	int			bit_count[PGLZ_V2_HUFF_BITS + 1] = {0};
	int			next_code[PGLZ_V2_HUFF_BITS + 1] = {0};
	int			code = 0;
	int			nsymbols = 0;

	for (int sym = 0; sym < 256; sym++)
	{
		if (lengths[sym] > PGLZ_V2_HUFF_BITS)
			return false;
		if (lengths[sym] > 0)
		{
			bit_count[lengths[sym]]++;
			nsymbols++;
		}
	}
	if (nsymbols == 0)
		return false;

	for (int bits = 1; bits <= PGLZ_V2_HUFF_BITS; bits++)
	{
		code = (code + bit_count[bits - 1]) << 1;
		if (code + bit_count[bits] > (1 << bits))
			return false;
		next_code[bits] = code;
	}
	if (nsymbols > 1 &&
		code + bit_count[PGLZ_V2_HUFF_BITS] !=
		(1 << PGLZ_V2_HUFF_BITS))
		return false;

	for (int sym = 0; sym < 256; sym++)
	{
		int			length = lengths[sym];

		if (length > 0)
			codes[sym] = next_code[length]++;
	}
	*nsymbolsp = nsymbols;
	return true;
}

static bool
pglz_build_huffman(const uint32 *frequencies, uint8 *lengths, uint16 *codes,
				   int *nsymbols)
{
	PGLZ_HuffNode nodes[511];
	int			heap[512];
	int			bit_count[256] = {0};
	int			sorted_symbols[256];
	int16		leaf[256];
	int			heap_size = 0;
	int			nnode = 0;
	int			symbol_count = 0;
	int			leaf_count;
	int			overflow = 0;

	memset(lengths, 0, 256 * sizeof(uint8));
	for (int sym = 0; sym < 256; sym++)
	{
		leaf[sym] = -1;
		if (frequencies[sym] == 0)
			continue;
		nodes[nnode].frequency = frequencies[sym];
		nodes[nnode].parent = -1;
		nodes[nnode].min_symbol = sym;
		leaf[sym] = nnode;
		sorted_symbols[symbol_count++] = sym;
		pglz_huff_heap_push(heap, &heap_size, nodes, nnode++);
	}

	if (heap_size == 0)
		return false;
	if (heap_size == 1)
	{
		lengths[nodes[heap[1]].min_symbol] = 1;
		return pglz_huffman_codes(lengths, codes, nsymbols);
	}

	while (heap_size > 1)
	{
		int			left = pglz_huff_heap_pop(heap, &heap_size, nodes);
		int			right = pglz_huff_heap_pop(heap, &heap_size, nodes);

		nodes[left].parent = nnode;
		nodes[right].parent = nnode;
		nodes[nnode].frequency = nodes[left].frequency + nodes[right].frequency;
		nodes[nnode].parent = -1;
		nodes[nnode].min_symbol = Min(nodes[left].min_symbol,
									  nodes[right].min_symbol);
		pglz_huff_heap_push(heap, &heap_size, nodes, nnode++);
	}

	for (int i = 1; i < symbol_count; i++)
	{
		int			sym = sorted_symbols[i];
		int			j = i;

		while (j > 0)
		{
			int			previous = sorted_symbols[j - 1];

			if (frequencies[previous] < frequencies[sym] ||
				(frequencies[previous] == frequencies[sym] &&
				 previous < sym))
				break;
			sorted_symbols[j] = previous;
			j--;
		}
		sorted_symbols[j] = sym;
	}

	for (int i = 0; i < symbol_count; i++)
	{
		int			depth = 0;
		int			node = leaf[sorted_symbols[i]];

		while (nodes[node].parent >= 0)
		{
			depth++;
			node = nodes[node].parent;
		}
		if (depth > PGLZ_V2_HUFF_BITS)
		{
			depth = PGLZ_V2_HUFF_BITS;
			overflow++;
		}
		bit_count[depth]++;
	}

	/* Restore a complete tree after clamping overlong codes. */
	while (overflow > 0)
	{
		int			bits = PGLZ_V2_HUFF_BITS - 1;

		while (bits > 0 && bit_count[bits] == 0)
			bits--;
		if (bits == 0 || bit_count[PGLZ_V2_HUFF_BITS] == 0)
			return false;
		bit_count[bits]--;
		bit_count[bits + 1] += 2;
		bit_count[PGLZ_V2_HUFF_BITS]--;
		overflow -= 2;
	}

	leaf_count = symbol_count;
	symbol_count = 0;
	for (int bits = PGLZ_V2_HUFF_BITS; bits > 0; bits--)
	{
		for (int i = 0; i < bit_count[bits]; i++)
		{
			if (symbol_count >= leaf_count)
				return false;
			lengths[sorted_symbols[symbol_count++]] = bits;
		}
	}
	if (symbol_count != leaf_count)
		return false;

	return pglz_huffman_codes(lengths, codes, nsymbols);
}


/* ----------
 * pglz_hist_add -
 *
 *		Adds a new entry to the history table.
 *
 * ----------
 */
static inline void
pglz_hist_add(uint16 *hstart, PGLZ_HistEntry *hentries, int *hist_next,
			  int hindex)
{
	uint16	   *head = &hstart[hindex];
	PGLZ_HistEntry *entry = &hentries[*hist_next];

	entry->next = *head;
	entry->hindex = hindex;
	*head = *hist_next;

	if (++(*hist_next) >= PGLZ_HISTORY_SIZE + 1)
		*hist_next = 1;
}


/* ----------
 * pglz_out_ctrl -
 *
 *		Outputs the last and allocates a new control byte if needed.
 * ----------
 */
#define pglz_out_ctrl(__ctrlp,__ctrlb,__ctrl,__buf) \
do { \
	if ((__ctrl & 0xff) == 0)												\
	{																		\
		*(__ctrlp) = __ctrlb;												\
		__ctrlp = (__buf)++;												\
		__ctrlb = 0;														\
		__ctrl = 1;															\
	}																		\
} while (0)


/* ----------
 * pglz_out_literal -
 *
 *		Outputs a literal byte to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
#define pglz_out_literal(_ctrlp,_ctrlb,_ctrl,_buf,_byte) \
do { \
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf);								\
	*(_buf)++ = (unsigned char)(_byte);										\
	_ctrl <<= 1;															\
} while (0)


/* ----------
 * pglz_out_tag -
 *
 *		Outputs a backward reference tag of 2-4 bytes (depending on
 *		offset and length) to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
#define pglz_out_tag(_ctrlp,_ctrlb,_ctrl,_buf,_len,_off) \
do { \
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf);								\
	_ctrlb |= _ctrl;														\
	_ctrl <<= 1;															\
	if (_len > 17)															\
	{																		\
		(_buf)[0] = (unsigned char)((((_off) & 0xf00) >> 4) | 0x0f);		\
		(_buf)[1] = (unsigned char)(((_off) & 0xff));						\
		(_buf)[2] = (unsigned char)((_len) - 18);							\
		(_buf) += 3;														\
	} else {																\
		(_buf)[0] = (unsigned char)((((_off) & 0xf00) >> 4) | ((_len) - 3)); \
		(_buf)[1] = (unsigned char)((_off) & 0xff);							\
		(_buf) += 2;														\
	}																		\
} while (0)


/* Return the byte position of the first difference in two native words. */
static inline int32
pglz_word_mismatch(Size diff)
{
#ifdef WORDS_BIGENDIAN
	return sizeof(Size) - 1 -
		(pg_leftmost_one_pos_size_t(diff) / BITS_PER_BYTE);
#else
#if SIZEOF_SIZE_T == 4
	return pg_rightmost_one_pos32((uint32) diff) / BITS_PER_BYTE;
#else
	return pg_rightmost_one_pos64((uint64) diff) / BITS_PER_BYTE;
#endif
#endif
}

/*
 * Determine the exact length of a match, stopping at matchend.  Compare a
 * vector at a time where PostgreSQL has a SIMD implementation, then compare
 * native words and finally individual bytes.  The word loads use memcpy() so
 * they do not require aligned input pointers.
 */
static inline int32
pglz_match_len_fallback(const char *ip, const char *hp, const char *matchend)
{
	const char *start = ip;

#ifndef USE_NO_SIMD
	while (matchend - ip >= (int) sizeof(Vector8))
	{
		Vector8		iv;
		Vector8		hv;
		uint32		mask;
		uint32		allmatch;

		vector8_load(&iv, (const uint8 *) ip);
		vector8_load(&hv, (const uint8 *) hp);
		mask = vector8_highbit_mask(vector8_eq(iv, hv));
		allmatch = UINT32_MAX >> (32 - sizeof(Vector8));

		if (mask != allmatch)
			return (ip - start) +
				pg_rightmost_one_pos32((~mask) & allmatch);

		ip += sizeof(Vector8);
		hp += sizeof(Vector8);
	}
#endif

	while (matchend - ip >= (int) sizeof(Size))
	{
		Size		ival;
		Size		hval;
		Size		diff;

		memcpy(&ival, ip, sizeof(Size));
		memcpy(&hval, hp, sizeof(Size));
		diff = ival ^ hval;

		if (diff != 0)
			return (ip - start) + pglz_word_mismatch(diff);

		ip += sizeof(Size);
		hp += sizeof(Size);
	}

	while (ip < matchend && *ip == *hp)
	{
		ip++;
		hp++;
	}

	return ip - start;
}

#ifdef USE_AVX2_WITH_RUNTIME_CHECK
/* AVX2-specialized match extension, selected after a runtime CPU check. */
pg_attribute_target("avx2")
static int32
pglz_match_len_avx2(const char *ip, const char *hp, const char *matchend)
{
	const char *start = ip;

	while (matchend - ip >= (int) sizeof(__m256i))
	{
		__m256i		iv;
		__m256i		hv;
		uint32		mask;

		iv = _mm256_loadu_si256((const __m256i *) ip);
		hv = _mm256_loadu_si256((const __m256i *) hp);
		mask = (uint32) _mm256_movemask_epi8(_mm256_cmpeq_epi8(iv, hv));

		if (mask != UINT32_MAX)
			return (ip - start) + pg_rightmost_one_pos32(~mask);

		ip += sizeof(__m256i);
		hp += sizeof(__m256i);
	}

	return (ip - start) + pglz_match_len_fallback(ip, hp, matchend);
}
#endif							/* USE_AVX2_WITH_RUNTIME_CHECK */

static inline int32
pglz_match_len(const char *ip, const char *hp, const char *end, int32 maxlen,
			   bool use_avx2)
{
	const char *start = ip;
	const char *matchend;

	if (end - ip > maxlen)
		matchend = ip + maxlen;
	else
		matchend = end;

	/* Most hash collisions differ within the first native word. */
	if (matchend - ip >= (int) sizeof(Size))
	{
		Size		ival;
		Size		hval;
		Size		diff;

		memcpy(&ival, ip, sizeof(Size));
		memcpy(&hval, hp, sizeof(Size));
		diff = ival ^ hval;

		if (diff != 0)
			return pglz_word_mismatch(diff);

		ip += sizeof(Size);
		hp += sizeof(Size);
	}

#ifdef USE_AVX2_WITH_RUNTIME_CHECK
	if (use_avx2 && matchend - ip >= (int) sizeof(__m256i))
		return (ip - start) +
			pglz_match_len_avx2(ip, hp, matchend);
#else
	(void) use_avx2;
#endif

	return (ip - start) +
		pglz_match_len_fallback(ip, hp, matchend);
}

static inline bool
pglz_token_begin(PGLZ_TokenWriter *writer)
{
	if (writer->items_left == 0)
	{
		if (writer->ptr >= writer->end)
			return false;
		writer->control = writer->ptr++;
		*writer->control = 0;
		writer->mask = 1;
		writer->items_left = 8;
	}
	return true;
}

static inline void
pglz_token_advance(PGLZ_TokenWriter *writer)
{
	writer->mask <<= 1;
	writer->items_left--;
}

static inline bool
pglz_token_literal(PGLZ_TokenWriter *writer, unsigned char literal)
{
	if (!pglz_token_begin(writer) || writer->ptr >= writer->end)
		return false;
	*writer->ptr++ = literal;
	pglz_token_advance(writer);
	return true;
}

static inline bool
pglz_token_match(PGLZ_TokenWriter *writer, int32 length, int32 offset)
{
	if (!pglz_token_begin(writer) || writer->end - writer->ptr < 4)
		return false;
	*writer->control |= writer->mask;
	writer->ptr[0] = offset & 0xff;
	writer->ptr[1] = (offset >> 8) & 0xff;
	writer->ptr[2] = length & 0xff;
	writer->ptr[3] = (length >> 8) & 0xff;
	writer->ptr += 4;
	pglz_token_advance(writer);
	return true;
}

static inline void
pglz_token_reader_init(PGLZ_TokenReader *reader,
					   const unsigned char *tokens,
					   const unsigned char *token_end)
{
	reader->ptr = tokens;
	reader->end = token_end;
	reader->control = 0;
	reader->items_left = 0;
}

static inline bool
pglz_token_read(PGLZ_TokenReader *reader, bool *is_match,
				unsigned char *literal, int32 *length, int32 *offset)
{
	if (reader->items_left == 0)
	{
		if (reader->ptr >= reader->end)
			return false;
		reader->control = *reader->ptr++;
		reader->items_left = 8;
	}

	*is_match = (reader->control & 1) != 0;
	if (*is_match)
	{
		if (reader->end - reader->ptr < 4)
			return false;
		*offset = reader->ptr[0] | (reader->ptr[1] << 8);
		*length = reader->ptr[2] | (reader->ptr[3] << 8);
		reader->ptr += 4;
	}
	else
	{
		if (reader->ptr >= reader->end)
			return false;
		*literal = *reader->ptr++;
	}
	reader->control >>= 1;
	reader->items_left--;
	return true;
}

static inline int
pglz_v2_hash(const char *source)
{
	uint32		sequence = pglz_hist_sequence(source);

	return (sequence * 0x9E3779B1U) >> (32 - PGLZ_V2_HASH_BITS);
}

static inline void
pglz_v2_hash_position(uint32 *hash_table, const char *source, int32 slen,
					  int32 position)
{
	if (position >= 0 && position <= slen - 4)
		hash_table[pglz_v2_hash(source + position)] = position + 1;
}

static pg_always_inline int32
pglz_modern_find_match(uint32 *hash_table, const char *source,
					   const char *source_end, int32 position,
					   int32 *match_offset, bool use_avx2)
{
	int			hindex = pglz_v2_hash(source + position);
	uint32		entry = hash_table[hindex];

	hash_table[hindex] = position + 1;
	if (entry != 0)
	{
		int32		candidate = entry - 1;

		*match_offset = position - candidate;
		if (*match_offset > 0 && *match_offset <= PGLZ_V2_WINDOW_SIZE)
			return pglz_match_len(source + position, source + candidate,
								  source_end, PGLZ_V2_MAX_MATCH, use_avx2);
	}

	*match_offset = 0;
	return 0;
}

static inline void
pglz_modern_hash_match(uint32 *hash_table, const char *source, int32 slen,
					   int32 match_start, int32 match_end)
{
	/* Keep a few positions around both match boundaries searchable. */
	pglz_v2_hash_position(hash_table, source, slen, match_start + 1);
	pglz_v2_hash_position(hash_table, source, slen, match_start + 2);
	pglz_v2_hash_position(hash_table, source, slen, match_end - 2);
	pglz_v2_hash_position(hash_table, source, slen, match_end - 1);
}

/* Build a compact token stream for the version 2 fallback encoder. */
static int32
pglz_v2_tokenize(const char *source, int32 slen, unsigned char *tokens,
				 unsigned char *token_end, uint32 *hash_table,
				 int32 first_success_by, bool use_avx2)
{
	PGLZ_TokenWriter writer;
	const char *source_end = source + slen;
	int32		position = 0;
	bool		found_match = false;

	writer.ptr = tokens;
	writer.end = token_end;
	writer.control = NULL;
	writer.mask = 0;
	writer.items_left = 0;

	while (position <= slen - 4)
	{
		int32		match_length;
		int32		match_offset = 0;

		match_length = pglz_modern_find_match(hash_table, source, source_end,
											  position, &match_offset, use_avx2);

		if (match_length >= PGLZ_V2_MIN_MATCH)
		{
			int32		match_start = position;

			if (!pglz_token_match(&writer, match_length, match_offset))
				return -1;
			position += match_length;
			found_match = true;

			pglz_modern_hash_match(hash_table, source, slen, match_start,
								   position);
		}
		else
		{
			unsigned char literal = (unsigned char) source[position++];

			if (!pglz_token_literal(&writer, literal))
				return -1;
		}

		if (!found_match && position >= first_success_by)
			return -1;
	}

	while (position < slen)
	{
		unsigned char literal = (unsigned char) source[position++];

		if (!pglz_token_literal(&writer, literal))
			return -1;
	}

	return writer.ptr - tokens;
}

static inline bool
pglz_v3_put_varint(unsigned char **ptr, unsigned char *end, uint32 value)
{
	do
	{
		unsigned char byte = value & 0x7f;

		value >>= 7;
		if (value != 0)
			byte |= 0x80;
		if (*ptr >= end)
			return false;
		*(*ptr)++ = byte;
	} while (value != 0);
	return true;
}

static bool
pglz_v3_emit_sequence(unsigned char **dest, unsigned char *dest_end,
					  const char *literals, int32 literal_length,
					  int32 match_length, int32 match_offset)
{
	unsigned char *bp = *dest;
	int32		match_code = match_length - PGLZ_V2_MIN_MATCH;
	unsigned char header;

	if (literal_length < 0 || match_length < 0 ||
		(match_length != 0 &&
		 (match_code < 0 || match_length > PGLZ_V2_MAX_MATCH ||
		  match_offset <= 0 || match_offset > PGLZ_V2_WINDOW_SIZE)))
		return false;

	header = Min(literal_length, PGLZ_V3_LITERAL_MASK) <<
		PGLZ_V3_LITERAL_SHIFT;
	if (match_length != 0)
		header |= Min(match_code, PGLZ_V3_MATCH_MASK);
	if (bp >= dest_end)
		return false;
	*bp++ = header;
	if (literal_length >= PGLZ_V3_LITERAL_MASK &&
		!pglz_v3_put_varint(&bp, dest_end,
							literal_length - PGLZ_V3_LITERAL_MASK))
		return false;
	if (dest_end - bp < literal_length)
		return false;
	memcpy(bp, literals, literal_length);
	bp += literal_length;

	if (match_length != 0)
	{
		if (dest_end - bp < 2)
			return false;
		bp[0] = (match_offset - 1) & 0xff;
		bp[1] = (match_offset - 1) >> 8;
		bp += 2;
		if (match_code >= PGLZ_V3_MATCH_MASK &&
			!pglz_v3_put_varint(&bp, dest_end,
								match_code - PGLZ_V3_MATCH_MASK))
			return false;
	}

	*dest = bp;
	return true;
}

/* Parse and encode version 3 directly, without an intermediate token stream. */
static int32
pglz_v3_compress(const char *source, int32 slen, char *dest,
				 uint32 *hash_table, int32 result_max,
				 int32 first_success_by, bool use_avx2)
{
	const char *source_end = source + slen;
	const char *literal_start = source;
	unsigned char *bp = (unsigned char *) dest;
	unsigned char *dest_end = bp + result_max;
	int32		position = 0;
	bool		found_match = false;

	if (dest_end - bp < (int) sizeof(pglz_v3_magic) + 1)
		return -1;
	memcpy(bp, pglz_v3_magic, sizeof(pglz_v3_magic));
	bp += sizeof(pglz_v3_magic);

	while (position <= slen - 4)
	{
		int32		match_length;
		int32		match_offset = 0;

		match_length = pglz_modern_find_match(hash_table, source, source_end,
											  position, &match_offset, use_avx2);

		if (match_length >= PGLZ_V2_MIN_MATCH)
		{
			int32		match_start = position;
			int32		literal_length = source + position - literal_start;

			if (!pglz_v3_emit_sequence(&bp, dest_end, literal_start,
									   literal_length, match_length,
									   match_offset))
				return -1;
			position += match_length;
			literal_start = source + position;
			found_match = true;

			pglz_modern_hash_match(hash_table, source, slen, match_start,
								   position);
		}
		else
			position++;

		if (!found_match && position >= first_success_by)
			return PGLZ_V3_NO_MATCH;
	}

	if (literal_start < source_end &&
		!pglz_v3_emit_sequence(&bp, dest_end, literal_start,
							   source_end - literal_start, 0, 0))
		return -1;
	if (bp - (unsigned char *) dest >= result_max)
		return -1;
	return bp - (unsigned char *) dest;
}

static bool
pglz_token_frequencies(const unsigned char *tokens,
					   const unsigned char *token_end,
					   uint32 *frequencies)
{
	PGLZ_TokenReader reader;
	unsigned char literal = 0;
	int32		length = 0;
	int32		offset = 0;
	bool		is_match;

	pglz_token_reader_init(&reader, tokens, token_end);
	while (pglz_token_read(&reader, &is_match, &literal, &length, &offset))
	{
		if (!is_match)
			frequencies[literal]++;
	}
	return reader.ptr == reader.end;
}

static inline void
pglz_v2_put_match(PGLZ_BitWriter *writer, int32 length, int32 offset)
{
	int32		extra;

	pglz_bitwriter_put(writer, 1, 1);
	if (offset <= 256)
	{
		pglz_bitwriter_put(writer, 0, 1);
		pglz_bitwriter_put(writer, offset - 1, 8);
	}
	else
	{
		pglz_bitwriter_put(writer, 1, 1);
		pglz_bitwriter_put(writer, offset - 1, 16);
	}

	if (length - PGLZ_V2_MIN_MATCH < 15)
	{
		pglz_bitwriter_put(writer, length - PGLZ_V2_MIN_MATCH, 4);
		return;
	}

	pglz_bitwriter_put(writer, 15, 4);
	extra = length - PGLZ_V2_MIN_MATCH - 15;
	do
	{
		uint32		byte = extra & 0x7f;

		extra >>= 7;
		if (extra != 0)
			byte |= 0x80;
		pglz_bitwriter_put(writer, byte, 8);
	} while (extra != 0);
}

static int32
pglz_v2_encode(const unsigned char *tokens, int32 token_size, int32 rawsize,
			   char *dest, int32 result_max, const uint8 *lengths,
			   const uint16 *codes, int nsymbols)
{
	unsigned char *bp = (unsigned char *) dest;
	unsigned char *dest_end = bp + result_max;
	unsigned char *bitmap;
	unsigned char *length_data;
	const unsigned char *tp = tokens;
	const unsigned char *token_end = tokens + token_size;
	PGLZ_BitWriter writer;
	int			length_index = 0;
	int32		raw_position = 0;
	unsigned char control = 0;
	int			items_left = 0;

	if (dest_end - bp < PGLZ_V2_MAX_HEADER)
		return -1;
	memcpy(bp, pglz_v2_magic, sizeof(pglz_v2_magic));
	bp += sizeof(pglz_v2_magic);
	bitmap = bp;
	memset(bitmap, 0, PGLZ_V2_BITMAP_SIZE);
	bp += PGLZ_V2_BITMAP_SIZE;
	length_data = bp;
	memset(length_data, 0, (nsymbols + 1) / 2);

	for (int sym = 0; sym < 256; sym++)
	{
		if (lengths[sym] == 0)
			continue;
		bitmap[sym >> 3] |= 1U << (sym & 7);
		if ((length_index & 1) == 0)
			length_data[length_index >> 1] = lengths[sym];
		else
			length_data[length_index >> 1] |= lengths[sym] << 4;
		length_index++;
	}
	bp += (nsymbols + 1) / 2;
	pglz_bitwriter_init(&writer, bp, dest_end);

	while (raw_position < rawsize)
	{
		if (items_left == 0)
		{
			if (tp >= token_end)
				return -1;
			control = *tp++;
			items_left = 8;
		}

		if (control & 1)
		{
			int32		offset;
			int32		length;

			if (token_end - tp < 4)
				return -1;
			offset = tp[0] | (tp[1] << 8);
			length = tp[2] | (tp[3] << 8);
			tp += 4;
			if (offset <= 0 ||
				offset > Min(raw_position, PGLZ_V2_WINDOW_SIZE) ||
				length < PGLZ_V2_MIN_MATCH ||
				length > PGLZ_V2_MAX_MATCH ||
				raw_position + length > rawsize)
				return -1;
			pglz_v2_put_match(&writer, length, offset);
			raw_position += length;
		}
		else
		{
			unsigned char literal;

			if (tp >= token_end)
				return -1;
			literal = *tp++;
			pglz_bitwriter_put(&writer, 0, 1);
			pglz_bitwriter_put(&writer, codes[literal], lengths[literal]);
			raw_position++;
		}
		if (writer.failed)
			return -1;
		control >>= 1;
		items_left--;
	}

	if (tp != token_end || !pglz_bitwriter_finish(&writer))
		return -1;
	return writer.ptr - (unsigned char *) dest;
}

static int32
pglz_compress_modern(const char *source, int32 slen, char *dest,
					 const PGLZ_Strategy *strategy)
{
	uint32		frequencies[256] = {0};
	uint8		lengths[256];
	uint16		codes[256];
	unsigned char *tokens;
	uint32	   *hash_table;
	int64		token_max64;
	int32		token_size;
	int32		result_size;
	int32		result_max;
	int32		need_rate;
	int			nsymbols;
	bool		use_avx2 = false;

	if (strategy == NULL)
		strategy = PGLZ_strategy_default;
	if (strategy->match_size_good <= 0 ||
		slen < Max(strategy->min_input_size, PGLZ_V2_MIN_INPUT) ||
		slen > strategy->max_input_size)
		return -1;

	need_rate = strategy->min_comp_rate;
	if (need_rate < 0)
		need_rate = 0;
	else if (need_rate > 99)
		need_rate = 99;
	if (slen > (INT_MAX / 100))
		result_max = (slen / 100) * (100 - need_rate);
	else
		result_max = (slen * (100 - need_rate)) / 100;
	if (result_max <= PGLZ_V3_MAX_HEADER)
		return -1;

	hash_table = (uint32 *) PGLZ_ALLOC(PGLZ_V2_HASH_SIZE * sizeof(uint32));
	if (hash_table == NULL)
		return -1;
	memset(hash_table, 0, PGLZ_V2_HASH_SIZE * sizeof(uint32));

#ifdef USE_AVX2_WITH_RUNTIME_CHECK
	use_avx2 = x86_feature_available(PG_AVX2);
#endif
	result_size = pglz_v3_compress(source, slen, dest, hash_table, result_max,
								   strategy->first_success_by, use_avx2);
	if (result_size >= 0)
	{
		PGLZ_FREE(hash_table);
		return result_size;
	}
	if (result_size == PGLZ_V3_NO_MATCH ||
		result_max <= PGLZ_V2_MAX_HEADER)
	{
		PGLZ_FREE(hash_table);
		return -1;
	}

	/*
	 * Keep the common version 3 path free of an intermediate token buffer.
	 * Allocate and rebuild the parse only when its byte-aligned stream could
	 * not meet the requested compression rate and version 2 might do so. One
	 * control bit plus one byte per literal is the token worst case.
	 */
	token_max64 = ((int64) slen * 9 + 7) / 8 + 1;
	if (token_max64 > (int64) MaxAllocSize)
	{
		PGLZ_FREE(hash_table);
		return -1;
	}
	tokens = (unsigned char *) PGLZ_ALLOC((Size) token_max64);
	if (tokens == NULL)
	{
		PGLZ_FREE(hash_table);
		return -1;
	}
	memset(hash_table, 0, PGLZ_V2_HASH_SIZE * sizeof(uint32));
	token_size = pglz_v2_tokenize(source, slen, tokens,
								  tokens + token_max64, hash_table,
								  strategy->first_success_by,
								  use_avx2);

	if (token_size < 0 ||
		!pglz_token_frequencies(tokens, tokens + token_size, frequencies) ||
		!pglz_build_huffman(frequencies, lengths, codes, &nsymbols))
	{
		PGLZ_FREE(tokens);
		PGLZ_FREE(hash_table);
		return -1;
	}
	result_size = pglz_v2_encode(tokens, token_size, slen, dest, result_max,
								 lengths, codes, nsymbols);
	PGLZ_FREE(tokens);
	PGLZ_FREE(hash_table);
	if (result_size < 0 || result_size >= result_max)
		return -1;
	return result_size;
}


/* ----------
 * pglz_find_match -
 *
 *		Lookup the history table if the actual input stream matches
 *		another sequence of characters, starting somewhere earlier
 *		in the input buffer.
 * ----------
 */
static pg_always_inline int
pglz_find_match_internal(uint16 *hstart, const char *input, const char *end,
						 int hist_next, int hindex, int probe_limit,
						 int *lenp, int *offp, int good_match, int good_drop,
						 bool use_avx2)
{
	uint16		hentno;
	int32		len = 0;
	int32		off = 0;
	int32		previousoff = 0;
	int			probes = 0;

	/*
	 * Traverse the linked history list until a good enough match is found.
	 */
	hentno = hstart[hindex];
	while (hentno != INVALID_ENTRY)
	{
		PGLZ_HistEntry *hent = &hist_entries[hentno];
		const char *ip = input;
		const char *hp;
		int32		thisoff;
		int32		thislen;

		/*
		 * Since one ring entry is added per input byte, the modular distance
		 * between hist_next and hentno is also the backward offset.  A link
		 * to an overwritten ring entry either has a different hash key or
		 * fails to increase the offset as we walk from newer to older
		 * entries.
		 */
		thisoff = hist_next - hentno;
		if (thisoff <= 0)
			thisoff += PGLZ_HISTORY_SIZE;
		if (hent->hindex != hindex || thisoff <= previousoff)
			break;
		if (thisoff >= 0x0fff)
			break;
		hp = input - thisoff;

		/*
		 * A candidate can only improve on the best match if its next byte
		 * also matches.  Check that byte before comparing the whole prefix;
		 * this rejects most hash collisions and many shorter matches cheaply.
		 */
		if (len < PGLZ_MAX_MATCH && len < end - input &&
			hp[len] != input[len])
			goto next_candidate;

		/*
		 * Determine length of match. A better match must be larger than the
		 * best so far. And if we already have a match of 16 or more bytes,
		 * it's worth the call overhead to use memcmp() to check if this match
		 * is equal for the same size. After that, extend the match using the
		 * best implementation available on this CPU.
		 */
		thislen = 0;
		if (len >= 16)
		{
			if (probe_limit == PGLZ_FAST_MATCH_PROBES)
			{
				Size		input_tail;
				Size		history_tail;

				/*
				 * Reject a differing tail word before comparing the whole
				 * prefix.
				 */
				memcpy(&input_tail, ip + len - sizeof(Size), sizeof(Size));
				memcpy(&history_tail, hp + len - sizeof(Size), sizeof(Size));
				if (input_tail != history_tail)
					goto next_candidate;
			}
			if (memcmp(ip, hp, len) == 0)
			{
				thislen = len;
				ip += len;
				hp += len;
				thislen += pglz_match_len(ip, hp, end,
										  PGLZ_MAX_MATCH - thislen,
										  use_avx2);
			}
		}
		else
			thislen = pglz_match_len(ip, hp, end, PGLZ_MAX_MATCH,
									 use_avx2);

		/*
		 * Remember this match as the best (if it is)
		 */
		if (thislen > len)
		{
			len = thislen;
			off = thisoff;
		}

next_candidate:

		/*
		 * Advance to the next history entry
		 */
		previousoff = thisoff;
		hentno = hent->next;

		/*
		 * Long history chains can dominate compression time.  Newer entries
		 * usually provide the useful matches.  If the good-match heuristic
		 * did not stop first, stop when this input's candidate budget is
		 * exhausted.
		 */
		if (++probes >= probe_limit)
			break;

		/*
		 * Be happy with lesser good matches the more entries we visited. But
		 * no point in doing calculation if we're at end of list.
		 */
		if (hentno != INVALID_ENTRY)
		{
			if (len >= good_match)
				break;
			good_match -= (good_match * good_drop) / 100;
		}
	}

	/*
	 * Return match information only if it results at least in one byte
	 * reduction.
	 */
	if (len > 2)
	{
		*lenp = len;
		*offp = off;
		return 1;
	}

	return 0;
}


/* ----------
 * pglz_compress_internal -
 *
 *		Compresses source into dest using strategy. Returns the number of
 *		bytes written in buffer dest, or -1 if compression fails.
 *
 * Always inline so the specialized callers have a constant probe limit.
 * ----------
 */
static pg_always_inline int32
pglz_compress_internal(const char *source, int32 slen, char *dest,
					   const PGLZ_Strategy *strategy, int probe_limit)
{
	unsigned char *bp = (unsigned char *) dest;
	unsigned char *bstart = bp;
	int			hist_next = 1;
	const char *dp = source;
	const char *dend = source + slen;
	unsigned char ctrl_dummy = 0;
	unsigned char *ctrlp = &ctrl_dummy;
	unsigned char ctrlb = 0;
	unsigned char ctrl = 0;
	bool		found_match = false;
	bool		use_avx2 = false;
	int32		match_len;
	int32		match_off;
	int32		good_match;
	int32		good_drop;
	int32		result_size;
	int32		result_max;
	int32		need_rate;
	uint32		hist_sequence = 0;
	int			hashsz;
	int			hindex = 0;
	int			mask;

	/*
	 * Our fallback strategy is the default.
	 */
	if (strategy == NULL)
		strategy = PGLZ_strategy_default;

	/*
	 * If the strategy forbids compression (at all or if source chunk size out
	 * of range), fail.
	 */
	if (strategy->match_size_good <= 0 ||
		slen < strategy->min_input_size ||
		slen > strategy->max_input_size)
		return -1;

	/*
	 * Limit the match parameters to the supported range.
	 */
	good_match = strategy->match_size_good;
	if (good_match > PGLZ_MAX_MATCH)
		good_match = PGLZ_MAX_MATCH;
	else if (good_match < 17)
		good_match = 17;

	good_drop = strategy->match_size_drop;
	if (good_drop < 0)
		good_drop = 0;
	else if (good_drop > 100)
		good_drop = 100;

	need_rate = strategy->min_comp_rate;
	if (need_rate < 0)
		need_rate = 0;
	else if (need_rate > 99)
		need_rate = 99;

#ifdef USE_AVX2_WITH_RUNTIME_CHECK
	/* Do runtime feature detection once per input, not once per match. */
	use_avx2 = x86_feature_available(PG_AVX2);
#endif

	/*
	 * Compute the maximum result size allowed by the strategy, namely the
	 * input size minus the minimum wanted compression rate.  This had better
	 * be <= slen, else we might overrun the provided output buffer.
	 */
	if (slen > (INT_MAX / 100))
	{
		/* Approximate to avoid overflow */
		result_max = (slen / 100) * (100 - need_rate);
	}
	else
		result_max = (slen * (100 - need_rate)) / 100;

	/*
	 * Experiments suggest that these hash sizes work pretty well. A large
	 * hash table minimizes collision, but has a higher startup cost. For a
	 * small input, the startup cost dominates. The table size must be a power
	 * of two.
	 */
	if (slen < 128)
		hashsz = 512;
	else if (slen < 256)
		hashsz = 1024;
	else if (slen < 512)
		hashsz = 2048;
	else if (slen < 1024)
		hashsz = 4096;
	else
		hashsz = 8192;
	mask = hashsz - 1;

	/*
	 * Initialize the history lists to empty.  We do not need to zero the
	 * hist_entries[] array; its entries are initialized as they are used.
	 */
	memset(hist_start, 0, hashsz * sizeof(uint16));

	/* Initialize the sequence used by the rolling history hash. */
	if (dp < dend)
	{
		if (dend - dp >= 4)
		{
			hist_sequence = pglz_hist_sequence(dp);
			hindex = pglz_hist_idx(hist_sequence, mask);
		}
		else
		{
			hist_sequence = (uint8) dp[0];
			hindex = hist_sequence & mask;
		}
	}

	/*
	 * Compress the source directly into the output buffer.
	 */
	while (dp < dend)
	{
		bool		match_found;

		/*
		 * If we already exceeded the maximum result size, fail.
		 *
		 * We check once per loop; since the loop body could emit as many as 4
		 * bytes (a control byte and 3-byte tag), PGLZ_MAX_OUTPUT() had better
		 * allow 4 slop bytes.
		 */
		if (bp - bstart >= result_max)
			return -1;

		/*
		 * If we've emitted more than first_success_by bytes without finding
		 * anything compressible at all, fail.  This lets us fall out
		 * reasonably quickly when looking at incompressible input (such as
		 * pre-compressed data).
		 */
		if (!found_match && bp - bstart >= strategy->first_success_by)
			return -1;

		/*
		 * Try to find a match in the history
		 */
		match_found = pglz_find_match_internal(hist_start, dp, dend, hist_next,
											   hindex, probe_limit, &match_len,
											   &match_off, good_match, good_drop,
											   use_avx2);

		if (match_found)
		{
			/*
			 * Create the tag and add history entries for all matched
			 * characters.
			 */
			pglz_out_tag(ctrlp, ctrlb, ctrl, bp, match_len, match_off);
			while (match_len--)
			{
				pglz_hist_add(hist_start, hist_entries, &hist_next, hindex);
				dp++;
				if (dp < dend)
					hindex = pglz_hist_advance(&hist_sequence, dp, dend, mask);
			}
			found_match = true;
		}
		else
		{
			/*
			 * No match found. Copy one literal byte.
			 */
			pglz_out_literal(ctrlp, ctrlb, ctrl, bp, *dp);
			pglz_hist_add(hist_start, hist_entries, &hist_next, hindex);
			dp++;
			if (dp < dend)
				hindex = pglz_hist_advance(&hist_sequence, dp, dend, mask);
		}
	}

	/*
	 * Write out the last control byte and check that we haven't overrun the
	 * output size allowed by the strategy.
	 */
	*ctrlp = ctrlb;
	result_size = bp - bstart;
	if (result_size >= result_max)
		return -1;

	/* success */
	return result_size;
}


static pg_noinline int32
pglz_compress_full(const char *source, int32 slen, char *dest,
				   const PGLZ_Strategy *strategy)
{
	return pglz_compress_internal(source, slen, dest, strategy,
								  PGLZ_FULL_MATCH_PROBES);
}

int32
pglz_compress(const char *source, int32 slen, char *dest,
			  const PGLZ_Strategy *strategy)
{
	int32		result;

	/*
	 * Long inputs can benefit from the larger-window parser.  Prefer the
	 * byte-aligned version 3 stream, then use version 2 when Huffman-coded
	 * literals are needed to satisfy the requested compression rate.
	 */
	if (slen >= PGLZ_V2_MIN_INPUT)
	{
		result = pglz_compress_modern(source, slen, dest, strategy);
		if (result >= 0)
			return result;
		if (!pglz_prefix_has_nul(source))
			return pglz_compress_internal(source, slen, dest, strategy,
										  PGLZ_FAST_MATCH_PROBES);
	}
	return pglz_compress_full(source, slen, dest, strategy);
}

static bool
pglz_huffman_decode_tables(const uint8 *lengths, uint32 *root_table,
						   uint16 *subtables)
{
	uint16		codes[256];
	int			nsymbols;
	int			nsubtables = 0;

	if (!pglz_huffman_codes(lengths, codes, &nsymbols))
		return false;
	memset(root_table, 0,
		   (1 << PGLZ_V2_DECODE_BITS) * sizeof(uint32));

	for (int sym = 0; sym < 256; sym++)
	{
		int			length = lengths[sym];
		int			first;
		int			count;
		uint16		entry;

		if (length == 0)
			continue;
		entry = (length << 8) | sym;
		if (length <= PGLZ_V2_DECODE_BITS)
		{
			first = codes[sym] << (PGLZ_V2_DECODE_BITS - length);
			count = 1 << (PGLZ_V2_DECODE_BITS - length);
			for (int i = 0; i < count; i++)
			{
				if (root_table[first + i] != 0)
					return false;
				root_table[first + i] = entry;
			}
		}
		else
		{
			int			prefix = codes[sym] >>
				(length - PGLZ_V2_DECODE_BITS);
			int			subtable;
			int			suffix_bits = length - PGLZ_V2_DECODE_BITS;

			if (root_table[prefix] == 0)
			{
				if (nsubtables >= PGLZ_V2_MAX_SUBTABLES)
					return false;
				subtable = nsubtables++;
				memset(subtables + subtable * PGLZ_V2_SUBTABLE_SIZE, 0,
					   PGLZ_V2_SUBTABLE_SIZE * sizeof(uint16));
				root_table[prefix] = PGLZ_V2_SUBTABLE_FLAG | subtable;
			}
			else if (root_table[prefix] & PGLZ_V2_SUBTABLE_FLAG)
				subtable = root_table[prefix] &
					~PGLZ_V2_SUBTABLE_FLAG;
			else
				return false;

			first = (codes[sym] & ((1 << suffix_bits) - 1)) <<
				(PGLZ_V2_DECODE_EXTRA - suffix_bits);
			count = 1 << (PGLZ_V2_DECODE_EXTRA - suffix_bits);
			for (int i = 0; i < count; i++)
			{
				uint16	   *slot = subtables +
					subtable * PGLZ_V2_SUBTABLE_SIZE + first + i;

				if (*slot != 0)
					return false;
				*slot = entry;
			}
		}
	}

	/*
	 * Use otherwise redundant root-table suffix bits to decode a second
	 * literal.  The packed length includes the first code, the next literal's
	 * zero token bit, and the second code.  Long codes still use the ordinary
	 * subtable path.
	 */
	for (int index = 0; index < (1 << PGLZ_V2_DECODE_BITS); index++)
	{
		uint32		entry = root_table[index];
		uint32		next_entry;
		int			first_length;
		int			available;
		int			next_index;
		int			second_length;

		if (entry == 0 || (entry & PGLZ_V2_SUBTABLE_FLAG) != 0)
			continue;
		first_length = (entry >> 8) & 0xff;
		available = PGLZ_V2_DECODE_BITS - first_length - 1;
		if (available <= 0 || ((index >> available) & 1) != 0)
			continue;

		next_index = (index & ((1 << available) - 1)) <<
			(PGLZ_V2_DECODE_BITS - available);
		next_entry = root_table[next_index];
		if (next_entry == 0 ||
			(next_entry & PGLZ_V2_SUBTABLE_FLAG) != 0)
			continue;
		second_length = (next_entry >> 8) & 0xff;
		if (second_length > available)
			continue;

		root_table[index] |= PGLZ_V2_LITERAL_PAIR_FLAG |
			((next_entry & 0xff) << PGLZ_V2_PAIR_SYMBOL_SHIFT) |
			((first_length + 1 + second_length) <<
			 PGLZ_V2_PAIR_LENGTH_SHIFT);
	}
	return true;
}

static inline void
pglz_copy_match(unsigned char **dest, int32 length, int32 offset,
				unsigned char *destend)
{
	unsigned char *dp = *dest;

	/*
	 * Away from the output boundary, copy a fixed amount for common short
	 * matches.  Bytes beyond the match are harmless because a later token
	 * overwrites them.  Staging the copies in offset-sized-or-smaller chunks
	 * also produces the right repetition when source and destination overlap.
	 */
	if (length <= 18 && offset >= 8 && destend - dp >= 18)
	{
		uint64		word;
		uint16		tail;

		memcpy(&word, dp - offset, sizeof(word));
		memcpy(dp, &word, sizeof(word));
		memcpy(&word, dp - offset + 8, sizeof(word));
		memcpy(dp + 8, &word, sizeof(word));
		memcpy(&tail, dp - offset + 16, sizeof(tail));
		memcpy(dp + 16, &tail, sizeof(tail));
		*dest = dp + length;
		return;
	}
	if (length <= 32 && offset >= 16 && destend - dp >= 32)
	{
		uint64		word0;
		uint64		word1;

		memcpy(&word0, dp - offset, sizeof(word0));
		memcpy(&word1, dp - offset + 8, sizeof(word1));
		memcpy(dp, &word0, sizeof(word0));
		memcpy(dp + 8, &word1, sizeof(word1));
		memcpy(&word0, dp - offset + 16, sizeof(word0));
		memcpy(&word1, dp - offset + 24, sizeof(word1));
		memcpy(dp + 16, &word0, sizeof(word0));
		memcpy(dp + 24, &word1, sizeof(word1));
		*dest = dp + length;
		return;
	}
	if (length <= 64 && offset >= 32 && destend - dp >= 64)
	{
		memcpy(dp, dp - offset, 32);
		memcpy(dp + 32, dp - offset + 32, 32);
		*dest = dp + length;
		return;
	}

	/* Avoid a library call for other small, non-overlapping matches. */
	if (offset >= length && length <= 64)
	{
		while (length >= 8)
		{
			uint64		word;

			memcpy(&word, dp - offset, sizeof(word));
			memcpy(dp, &word, sizeof(word));
			dp += sizeof(word);
			length -= sizeof(word);
		}
		if (length >= 4)
		{
			uint32		word;

			memcpy(&word, dp - offset, sizeof(word));
			memcpy(dp, &word, sizeof(word));
			dp += sizeof(word);
			length -= sizeof(word);
		}
		if (length >= 2)
		{
			uint16		word;

			memcpy(&word, dp - offset, sizeof(word));
			memcpy(dp, &word, sizeof(word));
			dp += sizeof(word);
			length -= sizeof(word);
		}
		if (length != 0)
		{
			*dp = *(dp - offset);
			dp++;
		}
		*dest = dp;
		return;
	}

	while (offset < length)
	{
		memcpy(dp, dp - offset, offset);
		length -= offset;
		dp += offset;
		offset += offset;
	}
	memcpy(dp, dp - offset, length);
	*dest = dp + length;
}

static inline bool
pglz_v3_get_varint(const unsigned char **ptr, const unsigned char *end,
				   uint32 *value)
{
	uint32		result = 0;

	for (int shift = 0; shift <= 28; shift += 7)
	{
		unsigned char byte;

		if (*ptr >= end)
			return false;
		byte = *(*ptr)++;
		if (shift == 28 && (byte & 0xf0) != 0)
			return false;
		result |= (uint32) (byte & 0x7f) << shift;
		if ((byte & 0x80) == 0)
		{
			*value = result;
			return true;
		}
	}
	return false;
}

static inline void
pglz_copy_literals(unsigned char *dest, const unsigned char *source,
				   int32 length, unsigned char *destend,
				   const unsigned char *srcend)
{
	if (length > 0 && length <= 16 && destend - dest >= 16 &&
		srcend - source >= 16)
	{
		uint64		word0;
		uint64		word1;

		memcpy(&word0, source, sizeof(word0));
		memcpy(&word1, source + 8, sizeof(word1));
		memcpy(dest, &word0, sizeof(word0));
		memcpy(dest + 8, &word1, sizeof(word1));
	}
	else
		memcpy(dest, source, length);
}

static int32
pglz_decompress_v3(const char *source, int32 slen, char *dest,
				   int32 rawsize, bool check_complete)
{
	const unsigned char *sp;
	const unsigned char *srcend = (const unsigned char *) source + slen;
	unsigned char *dp = (unsigned char *) dest;
	unsigned char *destend = dp + rawsize;

	if (slen < (int32) (sizeof(pglz_v3_magic) + 1))
		return -1;
	sp = (const unsigned char *) source + sizeof(pglz_v3_magic);

	while (dp < destend)
	{
		unsigned char header;
		uint32		extra;
		int32		literal_length;
		int32		copy_length;
		int32		match_length;
		int32		match_offset;

		/* Decode the common sequence with boundary checks hoisted. */
		if (destend - dp >= 64 && srcend - sp >= 32)
		{
			header = *sp;
			literal_length = header >> PGLZ_V3_LITERAL_SHIFT;
			match_length = header & PGLZ_V3_MATCH_MASK;
			if (literal_length != PGLZ_V3_LITERAL_MASK)
			{
				sp++;
				pglz_copy_literals(dp, sp, literal_length, destend, srcend);
				dp += literal_length;
				sp += literal_length;
				match_offset = (sp[0] | (sp[1] << 8)) + 1;
				sp += 2;
				if (match_offset > PGLZ_V2_WINDOW_SIZE ||
					match_offset > dp - (unsigned char *) dest)
					return -1;
				match_length += PGLZ_V2_MIN_MATCH;
				if ((header & PGLZ_V3_MATCH_MASK) == PGLZ_V3_MATCH_MASK)
				{
					if (!pglz_v3_get_varint(&sp, srcend, &extra) ||
						extra > PGLZ_V2_MAX_MATCH - PGLZ_V2_MIN_MATCH -
						PGLZ_V3_MATCH_MASK)
						return -1;
					match_length += extra;
					if (check_complete && match_length > destend - dp)
						return -1;
					match_length = Min(match_length, destend - dp);
				}
				pglz_copy_match(&dp, match_length, match_offset, destend);
				continue;
			}
		}

		if (sp >= srcend)
			return -1;
		header = *sp++;
		literal_length = (header >> PGLZ_V3_LITERAL_SHIFT) &
			PGLZ_V3_LITERAL_MASK;
		if (literal_length == PGLZ_V3_LITERAL_MASK)
		{
			if (!pglz_v3_get_varint(&sp, srcend, &extra) ||
				extra > INT_MAX - PGLZ_V3_LITERAL_MASK)
				return -1;
			literal_length += extra;
		}
		if (check_complete && literal_length > destend - dp)
			return -1;

		copy_length = Min(literal_length, destend - dp);
		if (copy_length > srcend - sp)
			return -1;
		pglz_copy_literals(dp, sp, copy_length, destend, srcend);
		dp += copy_length;
		sp += copy_length;
		if (copy_length != literal_length)
			return (char *) dp - dest;

		if (sp == srcend)
		{
			if ((header & PGLZ_V3_MATCH_MASK) != 0 || dp != destend)
				return -1;
			return (char *) dp - dest;
		}
		if (dp == destend)
			return check_complete ? -1 : (char *) dp - dest;

		if (srcend - sp < 2)
			return -1;
		match_offset = (sp[0] | (sp[1] << 8)) + 1;
		sp += 2;

		match_length = PGLZ_V2_MIN_MATCH +
			(header & PGLZ_V3_MATCH_MASK);
		if ((header & PGLZ_V3_MATCH_MASK) == PGLZ_V3_MATCH_MASK)
		{
			if (!pglz_v3_get_varint(&sp, srcend, &extra) ||
				extra > PGLZ_V2_MAX_MATCH - PGLZ_V2_MIN_MATCH -
				PGLZ_V3_MATCH_MASK)
				return -1;
			match_length += extra;
		}
		if (match_offset > PGLZ_V2_WINDOW_SIZE ||
			match_offset > dp - (unsigned char *) dest ||
			(check_complete && match_length > destend - dp))
			return -1;
		match_length = Min(match_length, destend - dp);
		pglz_copy_match(&dp, match_length, match_offset, destend);
	}

	if (check_complete && sp != srcend)
		return -1;
	return (char *) dp - dest;
}

static int32
pglz_decompress_v2(const char *source, int32 slen, char *dest,
				   int32 rawsize, bool check_complete)
{
	const unsigned char *sp;
	const unsigned char *srcend = (const unsigned char *) source + slen;
	const unsigned char *bitmap;
	unsigned char *dp = (unsigned char *) dest;
	unsigned char *destend = dp + rawsize;
	uint8		lengths[256] = {0};
	uint32		root_table[1 << PGLZ_V2_DECODE_BITS];
	uint16		subtables[PGLZ_V2_MAX_SUBTABLES * PGLZ_V2_SUBTABLE_SIZE];
	PGLZ_BitReader reader;
	int			nsymbols = 0;
	int			length_index = 0;

	if (slen < (int32) (sizeof(pglz_v2_magic) + PGLZ_V2_BITMAP_SIZE + 1))
		return -1;
	sp = (const unsigned char *) source + sizeof(pglz_v2_magic);
	bitmap = sp;
	sp += PGLZ_V2_BITMAP_SIZE;

	for (int sym = 0; sym < 256; sym++)
	{
		if (bitmap[sym >> 3] & (1U << (sym & 7)))
			nsymbols++;
	}
	if (nsymbols == 0 || srcend - sp < (nsymbols + 1) / 2)
		return -1;

	for (int sym = 0; sym < 256; sym++)
	{
		uint8		length_byte;

		if ((bitmap[sym >> 3] & (1U << (sym & 7))) == 0)
			continue;
		length_byte = sp[length_index >> 1];
		if (length_index & 1)
			lengths[sym] = length_byte >> 4;
		else
			lengths[sym] = length_byte & 0x0f;
		length_index++;
	}
	if ((nsymbols & 1) != 0 && (sp[nsymbols >> 1] & 0xf0) != 0)
		return -1;
	sp += (nsymbols + 1) / 2;
	if (!pglz_huffman_decode_tables(lengths, root_table, subtables))
		return -1;

	pglz_bitreader_init(&reader, sp, srcend);
	while (dp < destend)
	{
		uint32		token_bits;

		token_bits = pglz_bitreader_peek_padded(&reader,
												PGLZ_V2_HUFF_BITS + 1);
		if ((token_bits >> PGLZ_V2_HUFF_BITS) == 0)
		{
			uint32		bits;
			uint32		root_index;
			uint32		entry;
			int			code_length;

			bits = token_bits & ((1 << PGLZ_V2_HUFF_BITS) - 1);
			root_index = bits >> PGLZ_V2_DECODE_EXTRA;
			entry = root_table[root_index];
			if (entry & PGLZ_V2_SUBTABLE_FLAG)
			{
				int			subtable = entry & ~PGLZ_V2_SUBTABLE_FLAG;

				entry = subtables[subtable * PGLZ_V2_SUBTABLE_SIZE +
								  (bits & (PGLZ_V2_SUBTABLE_SIZE - 1))];
			}
			else if ((entry & PGLZ_V2_LITERAL_PAIR_FLAG) != 0 &&
					 dp + 1 < destend)
			{
				int			pair_length =
					(entry >> PGLZ_V2_PAIR_LENGTH_SHIFT) & 0x0f;

				if (!pglz_bitreader_drop(&reader, pair_length + 1))
					return -1;
				*dp++ = entry & 0xff;
				*dp++ = (entry >> PGLZ_V2_PAIR_SYMBOL_SHIFT) & 0xff;
				continue;
			}
			code_length = (entry >> 8) & 0xff;
			if (code_length == 0 ||
				!pglz_bitreader_drop(&reader, code_length + 1))
				return -1;
			*dp++ = entry & 0xff;
		}
		else
		{
			uint32		value;
			uint32		length_code;
			int32		offset;
			int32		length;

			if ((token_bits >> (PGLZ_V2_HUFF_BITS - 1)) & 1)
			{
				uint32		match_bits;

				match_bits = pglz_bitreader_peek_padded(&reader, 22);
				value = (match_bits >> 4) & 0xffff;
				length_code = match_bits & 0x0f;
				if (!pglz_bitreader_drop(&reader, 22))
					return -1;
			}
			else
			{
				value = (token_bits >> 4) & 0xff;
				length_code = token_bits & 0x0f;
				if (!pglz_bitreader_drop(&reader,
										 PGLZ_V2_HUFF_BITS + 1))
					return -1;
			}
			offset = value + 1;
			length = PGLZ_V2_MIN_MATCH + length_code;
			if (length_code == 15)
			{
				uint32		first_byte;
				uint32		second_byte;
				uint32		third_byte;
				uint32		extra;

				first_byte = pglz_bitreader_peek_padded(&reader, 8);
				if (!pglz_bitreader_drop(&reader, 8))
					return -1;
				extra = first_byte & 0x7f;
				if (first_byte & 0x80)
				{
					second_byte = pglz_bitreader_peek_padded(&reader, 8);
					if (!pglz_bitreader_drop(&reader, 8))
						return -1;
					extra |= (second_byte & 0x7f) << 7;
					if (second_byte & 0x80)
					{
						third_byte = pglz_bitreader_peek_padded(&reader, 8);
						if (!pglz_bitreader_drop(&reader, 8))
							return -1;
						extra |= (third_byte & 0x7f) << 14;
						if (third_byte & 0x80)
							return -1;
					}
				}
				if (extra > PGLZ_V2_MAX_MATCH -
					PGLZ_V2_MIN_MATCH - 15)
					return -1;
				length += extra;
			}

			if (offset > PGLZ_V2_WINDOW_SIZE ||
				offset > dp - (unsigned char *) dest ||
				(check_complete && length > destend - dp))
				return -1;
			length = Min(length, destend - dp);
			pglz_copy_match(&dp, length, offset, destend);
		}
	}

	if (check_complete &&
		(reader.ptr != reader.end || reader.nbits >= 8 ||
		 (reader.nbits > 0 &&
		  (reader.bits & ((((uint64) 1) << reader.nbits) - 1)) != 0)))
		return -1;
	return (char *) dp - dest;
}

int32
pglz_decompress(const char *source, int32 slen, char *dest,
				int32 rawsize, bool check_complete)
{
	if (slen >= (int32) sizeof(pglz_v3_magic) &&
		memcmp(source, pglz_v3_magic, sizeof(pglz_v3_magic)) == 0)
		return pglz_decompress_v3(source, slen, dest, rawsize,
								  check_complete);
	if (slen >= (int32) sizeof(pglz_v2_magic) &&
		memcmp(source, pglz_v2_magic, sizeof(pglz_v2_magic)) == 0)
		return pglz_decompress_v2(source, slen, dest, rawsize,
								  check_complete);
	return pglz_decompress_legacy(source, slen, dest, rawsize,
								  check_complete);
}


/* ----------
 * pglz_decompress -
 *
 *		Decompresses source into dest. Returns the number of bytes
 *		decompressed into the destination buffer, or -1 if the
 *		compressed data is corrupted.
 *
 *		If check_complete is true, the data is considered corrupted
 *		if we don't exactly fill the destination buffer.  Callers that
 *		are extracting a slice typically can't apply this check.
 * ----------
 */
static int32
pglz_decompress_legacy(const char *source, int32 slen, char *dest,
					   int32 rawsize, bool check_complete)
{
	const unsigned char *sp;
	const unsigned char *srcend;
	unsigned char *dp;
	unsigned char *destend;

	sp = (const unsigned char *) source;
	srcend = ((const unsigned char *) source) + slen;
	dp = (unsigned char *) dest;
	destend = dp + rawsize;

	while (sp < srcend && dp < destend)
	{
		/*
		 * Read one control byte and process the next 8 items (or as many as
		 * remain in the compressed input).
		 */
		unsigned char ctrl = *sp++;
		int			ctrlc;

		for (ctrlc = 0; ctrlc < 8 && sp < srcend && dp < destend; ctrlc++)
		{
			if (ctrl & 1)
			{
				/*
				 * Set control bit means we must read a match tag. The match
				 * is coded with two bytes. First byte uses lower nibble to
				 * code length - 3. Higher nibble contains upper 4 bits of the
				 * offset. The next following byte contains the lower 8 bits
				 * of the offset. If the length is coded as 18, another
				 * extension tag byte tells how much longer the match really
				 * was (0-255).
				 */
				int32		len;
				int32		off;

				/*
				 * A match tag is at least 2 bytes; if the length nibble is
				 * 0x0f the tag is 3 bytes (extended length).  Verify we have
				 * enough source data before reading them.
				 */
				if (unlikely(sp + 2 > srcend))
					return -1;

				len = (sp[0] & 0x0f) + 3;
				off = ((sp[0] & 0xf0) << 4) | sp[1];
				sp += 2;
				if (len == 18)
				{
					if (unlikely(sp >= srcend))
						return -1;
					len += *sp++;
				}

				/*
				 * Check for corrupt data: if we obtained off = 0, or if off
				 * is more than the distance back to the buffer start, we have
				 * problems.  (We must check for off = 0, else we risk an
				 * infinite loop below in the face of corrupt data. Likewise,
				 * the upper limit on off prevents accessing outside the
				 * buffer boundaries.)
				 */
				if (unlikely(off == 0 ||
							 off > (dp - (unsigned char *) dest)))
					return -1;

				/*
				 * Don't emit more data than requested.
				 */
				len = Min(len, destend - dp);

				/*
				 * Now we copy the bytes specified by the tag from OUTPUT to
				 * OUTPUT (copy len bytes from dp - off to dp).  The copied
				 * areas could overlap, so to avoid undefined behavior in
				 * memcpy(), be careful to copy only non-overlapping regions.
				 *
				 * Note that we cannot use memmove() instead, since while its
				 * behavior is well-defined, it's also not what we want.
				 */
				while (off < len)
				{
					/*
					 * We can safely copy "off" bytes since that clearly
					 * results in non-overlapping source and destination.
					 */
					memcpy(dp, dp - off, off);
					len -= off;
					dp += off;

					/*----------
					 * This bit is less obvious: we can double "off" after
					 * each such step.  Consider this raw input:
					 *		112341234123412341234
					 * This will be encoded as 5 literal bytes "11234" and
					 * then a match tag with length 16 and offset 4.  After
					 * memcpy'ing the first 4 bytes, we will have emitted
					 *		112341234
					 * so we can double "off" to 8, then after the next step
					 * we have emitted
					 *		11234123412341234
					 * Then we can double "off" again, after which it is more
					 * than the remaining "len" so we fall out of this loop
					 * and finish with a non-overlapping copy of the
					 * remainder.  In general, a match tag with off < len
					 * implies that the decoded data has a repeat length of
					 * "off".  We can handle 1, 2, 4, etc repetitions of the
					 * repeated string per memcpy until we get to a situation
					 * where the final copy step is non-overlapping.
					 *
					 * (Another way to understand this is that we are keeping
					 * the copy source point dp - off the same throughout.)
					 *----------
					 */
					off += off;
				}
				memcpy(dp, dp - off, len);
				dp += len;
			}
			else
			{
				/*
				 * An unset control bit means LITERAL BYTE. So we just copy
				 * one from INPUT to OUTPUT.
				 */
				*dp++ = *sp++;
			}

			/*
			 * Advance the control bit
			 */
			ctrl >>= 1;
		}
	}

	/*
	 * If requested, check we decompressed the right amount.
	 */
	if (check_complete && (dp != destend || sp != srcend))
		return -1;

	/*
	 * That's it.
	 */
	return (char *) dp - dest;
}


/* ----------
 * pglz_maximum_compressed_size -
 *
 *		Calculate the maximum compressed size for a given amount of raw data.
 *		Return the maximum size, or total compressed size if maximum size is
 *		larger than total compressed size.
 *
 * We can't use PGLZ_MAX_OUTPUT for this purpose, because that's used to size
 * the compression buffer (and abort the compression). It does not really say
 * what's the maximum compressed size for an input of a given length, and it
 * may happen that while the whole value is compressible (and thus fits into
 * PGLZ_MAX_OUTPUT nicely), the prefix is not compressible at all.
 * ----------
 */
int32
pglz_maximum_compressed_size(int32 rawsize, int32 total_compressed_size)
{
	int64		legacy_size;
	int64		v2_size;
	int64		v3_size;
	int64		compressed_size;

	/*
	 * pglz uses one control bit per byte, so if the entire desired prefix is
	 * represented as literal bytes, we'll need (rawsize * 9) bits.  We care
	 * about bytes though, so be sure to round up not down.
	 *
	 * Use int64 here to prevent overflow during calculation.
	 */
	legacy_size = ((int64) rawsize * 9 + 7) / 8;

	/*
	 * The above fails to account for a corner case: we could have compressed
	 * data that starts with N-1 or N-2 literal bytes and then has a match tag
	 * of 2 or 3 bytes.  It's therefore possible that we need to fetch 1 or 2
	 * more bytes in order to have the whole match tag.  (Match tags earlier
	 * in the compressed data don't cause a problem, since they should
	 * represent more decompressed bytes than they occupy themselves.)
	 */
	legacy_size += 2;

	/*
	 * Version 2 needs its Huffman header, and a literal can occupy at most
	 * one token bit plus PGLZ_V2_HUFF_BITS code bits.  Six bytes of slop
	 * cover the largest offset and length encoding for a match that crosses
	 * the requested output boundary.
	 */
	v2_size = PGLZ_V2_MAX_HEADER +
		(((int64) rawsize * (PGLZ_V2_HUFF_BITS + 1) + 7) / 8) + 6;

	/*
	 * A version 3 literal occupies one byte, with enough header slop for a
	 * length extension and a match that crosses the requested boundary.
	 */
	v3_size = (int64) rawsize + PGLZ_V3_MAX_HEADER;
	compressed_size = Max(Max(legacy_size, v2_size), v3_size);

	/*
	 * Maximum compressed size can't be larger than total compressed size.
	 * (This also ensures that our result fits in int32.)
	 */
	compressed_size = Min(compressed_size, total_compressed_size);

	return (int32) compressed_size;
}
