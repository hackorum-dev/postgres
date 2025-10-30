#include <immintrin.h>

pg_attribute_target("avx512bw")
static inline bool
is_valid_ascii_avx512(const unsigned char *s, int len)
{
	const unsigned char *const s_end = s + len;
	__m512i chunk;

	__mmask64 res = 0;

	Assert(len % sizeof(chunk) == 0);

	while (s < s_end)
	{
		__m512i ascii_mask;
		__mmask64 resHighBit, resZero;

		ascii_mask = _mm512_set1_epi8((unsigned char)0x80);

		chunk = _mm512_loadu_epi8(s);

		resHighBit = _mm512_cmpeq_epi8_mask(_mm512_and_si512(chunk, ascii_mask), ascii_mask);
		resZero = _mm512_cmpeq_epi8_mask(chunk, _mm512_setzero_si512());
		res |= resHighBit | resZero;

		s += sizeof(chunk);
	}

	return res == 0;
}

pg_attribute_target("avx2")
static inline bool
is_valid_ascii_avx2(const unsigned char *s, int len)
{
	const unsigned char *const s_end = s + len;
	int res_scalar;
	__m256i chunk;
	__m256i res_vector = _mm256_setzero_si256();

	Assert(len % sizeof(chunk) == 0);

	while (s < s_end)
	{
		__m256i ascii_mask, resHighBit, resZero, resPart;

		chunk = _mm256_loadu_si256((const __m256i *) s);

		ascii_mask = _mm256_set1_epi8((unsigned char)0x80);

		resHighBit = _mm256_cmpeq_epi8(_mm256_and_si256(chunk, ascii_mask), ascii_mask);
		resZero = _mm256_cmpeq_epi8(chunk, _mm256_setzero_si256());
		resPart = _mm256_or_si256(resHighBit, resZero);

		res_vector = _mm256_or_si256(res_vector, resPart);

		s += sizeof(chunk);
	}

	res_scalar = _mm256_movemask_epi8(res_vector);
	return res_scalar == 0;
}

pg_attribute_target("avx512bw")
static inline void is_valid_ascii_small_avx512(const unsigned char **s, int *len, int orig_len) {
	__mmask64 mask = (1ull << *len) - 1;

	/* Needs to be any value that isn't zero and doesn't have the high bit set. So 1 */
	__m512i not_zero_not_high = _mm512_set1_epi8(1);

	__m512i x = _mm512_mask_loadu_epi8(not_zero_not_high, mask, *s);

	__m512i ascii_mask = _mm512_set1_epi8((unsigned char)0x80);

	__mmask64 resHighBit = _mm512_cmpeq_epi8_mask(_mm512_and_si512(x, ascii_mask), ascii_mask);
	__mmask64 resZero = _mm512_cmpeq_epi8_mask(x, _mm512_setzero_si512());
	__mmask64 res = resHighBit | resZero;

	if (res == 0){
		*s += *len;
		*len -= *len;
	}
}

pg_attribute_target("avx2")
static inline void is_valid_ascii_small_avx2(const unsigned char **s, int *len, int orig_len) {
	/* If >= 32, we need to run the main avx2 validation function first */
	while (*len >= 32) {
		if (is_valid_ascii_avx2(*s, 32)) {
			*s += 32;
			*len -= 32;
		} else {
			return;
		}
	}

	if (orig_len < 32){ /* Slow route, required if we can't load 32 bytes */
		int res_scalar;
		int chunks = *len / 4;
		int processed = chunks * 4;

		static const int mask_lut[9*8] = {
			 0, 0, 0, 0, 0, 0, 0, 0,
			-1, 0, 0, 0, 0, 0, 0, 0,
			-1,-1, 0, 0, 0, 0, 0, 0,
			-1,-1,-1, 0, 0, 0, 0, 0,
			-1,-1,-1,-1, 0, 0, 0, 0,
			-1,-1,-1,-1,-1, 0, 0, 0,
			-1,-1,-1,-1,-1,-1, 0, 0,
			-1,-1,-1,-1,-1,-1,-1, 0,
			-1,-1,-1,-1,-1,-1,-1,-1,
		};

		__m256i mask, not_zero_not_high, raw_chunk, chunk, ascii_mask, resHighBit, resZero, res_vector;

		Assert(chunks >= 0 && chunks <= 8);

		mask = _mm256_loadu_si256(((const __m256i *) mask_lut) + chunks);

		/* Needs to be any value that isn't zero and doesn't have the high bit set. So 1 */
		not_zero_not_high = _mm256_set1_epi8(1);

		raw_chunk = _mm256_maskload_epi32((const int *) *s, mask);
		chunk = _mm256_castps_si256(
			_mm256_blendv_ps(
				_mm256_castsi256_ps(not_zero_not_high),
				_mm256_castsi256_ps(raw_chunk),
				_mm256_castsi256_ps(mask)));

		ascii_mask = _mm256_set1_epi8((unsigned char)0x80);

		resHighBit = _mm256_cmpeq_epi8(_mm256_and_si256(chunk, ascii_mask), ascii_mask);
		resZero = _mm256_cmpeq_epi8(chunk, _mm256_setzero_si256());
		res_vector = _mm256_or_si256(resHighBit, resZero);

		res_scalar = _mm256_movemask_epi8(res_vector);

		if (res_scalar == 0){
			*s += processed;
			*len -= processed;
		}
	} else { /* Fast route */
		uint32 mask = ~((1ull << *len) - 1);

		__m256i chunk = _mm256_loadu_si256((const __m256i *) *s - (*len - 32));

		__m256i ascii_mask = _mm256_set1_epi8((unsigned char)0x80);

		__m256i resHighBit = _mm256_cmpeq_epi8(_mm256_and_si256(chunk, ascii_mask), ascii_mask);
		__m256i resZero = _mm256_cmpeq_epi8(chunk, _mm256_setzero_si256());
		__m256i res_vector = _mm256_or_si256(resHighBit, resZero);

		int res_scalar = _mm256_movemask_epi8(res_vector);
		res_scalar &= mask;

		if (res_scalar == 0){
			*s += *len;
			*len -= *len;
		}
	}
}

static inline void is_valid_ascii_small_default(const unsigned char **s, int *len, int orig_len) {
	/* Just a placeholder to make the dispatch logic simpler */
	return;
}

/* This will need some checks/more complicated logic, but should be usable (all systems that support attribute target seem to support cpu_supports...) */
#define pg_cpu_supports(...) __builtin_cpu_supports(__VA_ARGS__)
#define pg_cpu_init() __builtin_cpu_init()

static inline bool is_valid_ascii_dispatch_x86(const unsigned char *s, int len);
static inline void is_valid_ascii_small_dispatch_x86(const unsigned char **s, int *len, int orig_len);

static bool (*is_valid_ascii_x86)(const unsigned char *s, int len) = is_valid_ascii_dispatch_x86;
static void (*is_valid_ascii_small_x86)(const unsigned char **s, int *len, int orig_len) = is_valid_ascii_small_dispatch_x86;

static inline bool is_valid_ascii_dispatch_x86(const unsigned char *s, int len) {
	pg_cpu_init();
	if (pg_cpu_supports("avx512bw")) {
		is_valid_ascii_x86 = is_valid_ascii_avx512;
	} else if (pg_cpu_supports("avx2")) {
		is_valid_ascii_x86 = is_valid_ascii_avx2;
	} else {
		is_valid_ascii_x86 = is_valid_ascii;
	}
	return is_valid_ascii_x86(s, len);
}

static inline void is_valid_ascii_small_dispatch_x86(const unsigned char **s, int *len, int orig_len) {
	pg_cpu_init();
	if (pg_cpu_supports("avx512bw")) {
		is_valid_ascii_small_x86 = is_valid_ascii_small_avx512;
	} else if (pg_cpu_supports("avx2")) {
		is_valid_ascii_small_x86 = is_valid_ascii_small_avx2;
	} else {
		is_valid_ascii_small_x86 = is_valid_ascii_small_default;
	}
	is_valid_ascii_small_x86(s, len, orig_len);
}
