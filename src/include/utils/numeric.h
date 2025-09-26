/*-------------------------------------------------------------------------
 *
 * numeric.h
 *	  Definitions for the exact numeric data type of Postgres
 *
 * Original coding 1998, Jan Wieck.  Heavily revised 2003, Tom Lane.
 *
 * Copyright (c) 1998-2025, PostgreSQL Global Development Group
 *
 * src/include/utils/numeric.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_NUMERIC_H_
#define _PG_NUMERIC_H_

#include "common/pg_prng.h"
#include "fmgr.h"

/* forward declaration to avoid node.h include */
typedef struct Node Node;

/*
 * Limits on the precision and scale specifiable in a NUMERIC typmod.  The
 * precision is strictly positive, but the scale may be positive or negative.
 * A negative scale implies rounding before the decimal point.
 *
 * Note that the minimum display scale defined below is zero --- we always
 * display all digits before the decimal point, even when the scale is
 * negative.
 *
 * Note that the implementation limits on the precision and display scale of a
 * numeric value are much larger --- beware of what you use these for!
 */
#define NUMERIC_MAX_PRECISION		1000

#define NUMERIC_MIN_SCALE			(-1000)
#define NUMERIC_MAX_SCALE			1000

/*
 * Internal limits on the scales chosen for calculation results
 */
#define NUMERIC_MAX_DISPLAY_SCALE	NUMERIC_MAX_PRECISION
#define NUMERIC_MIN_DISPLAY_SCALE	0

#define NUMERIC_MAX_RESULT_SCALE	(NUMERIC_MAX_PRECISION * 2)

/* ----------
 * Local data types
 *
 * Numeric values are represented in a base-NBASE floating point format.
 * Each "digit" ranges from 0 to NBASE-1.  The type NumericDigit is signed
 * and wide enough to store a digit.  We assume that NBASE*NBASE can fit in
 * an int.  Although the purely calculational routines could handle any even
 * NBASE that's less than sqrt(INT_MAX), in practice we are only interested
 * in NBASE a power of ten, so that I/O conversions and decimal rounding
 * are easy.  Also, it's actually more efficient if NBASE is rather less than
 * sqrt(INT_MAX), so that there is "headroom" for mul_var and div_var to
 * postpone processing carries.
 *
 * Values of NBASE other than 10000 are considered of historical interest only
 * and are no longer supported in any sense; no mechanism exists for the client
 * to discover the base, so every client supporting binary mode expects the
 * base-10000 format.  If you plan to change this, also note the numeric
 * abbreviation code, which assumes NBASE=10000.
 * ----------
 */

#if 0
#define NBASE		10
#define HALF_NBASE	5
#define DEC_DIGITS	1			/* decimal digits per NBASE digit */
#define MUL_GUARD_DIGITS	4	/* these are measured in NBASE digits */
#define DIV_GUARD_DIGITS	8

typedef signed char NumericDigit;
#endif

#if 0
#define NBASE		100
#define HALF_NBASE	50
#define DEC_DIGITS	2			/* decimal digits per NBASE digit */
#define MUL_GUARD_DIGITS	3	/* these are measured in NBASE digits */
#define DIV_GUARD_DIGITS	6

typedef signed char NumericDigit;
#endif

#if 1
#define NBASE		10000
#define HALF_NBASE	5000
#define DEC_DIGITS	4			/* decimal digits per NBASE digit */
#define MUL_GUARD_DIGITS	2	/* these are measured in NBASE digits */
#define DIV_GUARD_DIGITS	4

typedef int16 NumericDigit;
#endif

#define NBASE_SQR	(NBASE * NBASE)

/*
 * For inherently inexact calculations such as division and square root,
 * we try to get at least this many significant digits; the idea is to
 * deliver a result no worse than float8 would.
 */
#define NUMERIC_MIN_SIG_DIGITS		16

/*
 * sign field of NumericVar
 */

#define NUMERIC_POS      0x0000
#define NUMERIC_NEG      0x4000
#define NUMERIC_NAN      0xC000
#define NUMERIC_PINF     0xD000
#define NUMERIC_NINF     0xF000

/*
 * Maximum weight of a stored Numeric value (based on the use of int16 for the
 * weight in NumericLong).  Note that intermediate values held in NumericVar
 * and NumericSumAccum variables may have much larger weights.
 */
#define NUMERIC_WEIGHT_MAX			PG_INT16_MAX

/* ----------
 * NumericVar is the format we use for arithmetic.  The digit-array part
 * is the same as the NumericData storage format, but the header is more
 * complex.
 *
 * The value represented by a NumericVar is determined by the sign, weight,
 * ndigits, and digits[] array.  If it is a "special" value (NaN or Inf)
 * then only the sign field matters; ndigits should be zero, and the weight
 * and dscale fields are ignored.
 *
 * Note: the first digit of a NumericVar's value is assumed to be multiplied
 * by NBASE ** weight.  Another way to say it is that there are weight+1
 * digits before the decimal point.  It is possible to have weight < 0.
 *
 * buf points at the physical start of the palloc'd digit buffer for the
 * NumericVar.  digits points at the first digit in actual use (the one
 * with the specified weight).  We normally leave an unused digit or two
 * (preset to zeroes) between buf and digits, so that there is room to store
 * a carry out of the top digit without reallocating space.  We just need to
 * decrement digits (and increment weight) to make room for the carry digit.
 * (There is no such extra space in a numeric value stored in the database,
 * only in a NumericVar in memory.)
 *
 * If buf is NULL then the digit buffer isn't actually palloc'd and should
 * not be freed --- see the constants below for an example.
 *
 * dscale, or display scale, is the nominal precision expressed as number
 * of digits after the decimal point (it must always be >= 0 at present).
 * dscale may be more than the number of physically stored fractional digits,
 * implying that we have suppressed storage of significant trailing zeroes.
 * It should never be less than the number of stored digits, since that would
 * imply hiding digits that are present.  NOTE that dscale is always expressed
 * in *decimal* digits, and so it may correspond to a fractional number of
 * base-NBASE digits --- divide by DEC_DIGITS to convert to NBASE digits.
 *
 * rscale, or result scale, is the target precision for a computation.
 * Like dscale it is expressed as number of *decimal* digits after the decimal
 * point, and is always >= 0 at present.
 * Note that rscale is not stored in variables --- it's figured on-the-fly
 * from the dscales of the inputs.
 *
 * While we consistently use "weight" to refer to the base-NBASE weight of
 * a numeric value, it is convenient in some scale-related calculations to
 * make use of the base-10 weight (ie, the approximate log10 of the value).
 * To avoid confusion, such a decimal-units weight is called a "dweight".
 *
 * NB: All the variable-level functions are written in a style that makes it
 * possible to give one and the same variable as argument and destination.
 * This is feasible because the digit buffer is separate from the variable.
 * ----------
 */
typedef struct NumericVar
{
	int			ndigits;		/* # of digits in digits[] - can be 0! */
	int			weight;			/* weight of first digit */
	int			sign;			/* NUMERIC_POS, _NEG, _NAN, _PINF, or _NINF */
	int			dscale;			/* display scale */
	NumericDigit *buf;			/* start of palloc'd space for digits[] */
	NumericDigit *digits;		/* base-NBASE digits */
} NumericVar;

/* The actual contents of Numeric are private to numeric.c */
struct NumericData;
typedef struct NumericData *Numeric;

/*
 * fmgr interface macros
 */

static inline Numeric
DatumGetNumeric(Datum X)
{
	return (Numeric) PG_DETOAST_DATUM(X);
}

static inline Numeric
DatumGetNumericCopy(Datum X)
{
	return (Numeric) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
NumericGetDatum(Numeric X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_NUMERIC(n)	  DatumGetNumeric(PG_GETARG_DATUM(n))
#define PG_GETARG_NUMERIC_COPY(n) DatumGetNumericCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_NUMERIC(x)	  return NumericGetDatum(x)

#define init_var(v)		memset(v, 0, sizeof(NumericVar))

/*
 * Utility functions in numeric.c
 */
extern void alloc_var(NumericVar *var, int ndigits);
extern void free_var(NumericVar *var);
extern void zero_var(NumericVar *var);

extern void set_var_from_num(Numeric num, NumericVar *dest);
extern void init_var_from_num(Numeric num, NumericVar *dest);
extern void set_var_from_var(const NumericVar *value, NumericVar *dest);

extern Numeric numeric_make_result(const NumericVar *var);
extern Numeric numeric_make_result_safe(const NumericVar *var, Node *escontext);

extern bool numeric_is_nan(Numeric num);
extern bool numeric_is_inf(Numeric num);
extern int32 numeric_maximum_size(int32 typmod);
extern char *numeric_out_sci(Numeric num, int scale);
extern char *numeric_normalize(Numeric num);

extern Numeric int64_to_numeric(int64 val);
extern Numeric int64_div_fast_to_numeric(int64 val1, int log10val2);

extern Numeric numeric_add_safe(Numeric num1, Numeric num2, Node *escontext);
extern Numeric numeric_sub_safe(Numeric num1, Numeric num2, Node *escontext);
extern Numeric numeric_mul_safe(Numeric num1, Numeric num2, Node *escontext);
extern Numeric numeric_div_safe(Numeric num1, Numeric num2, Node *escontext);
extern Numeric numeric_mod_safe(Numeric num1, Numeric num2, Node *escontext);
extern int32 numeric_int4_safe(Numeric num, Node *escontext);
extern int64 numeric_int8_safe(Numeric num, Node *escontext);

extern Numeric random_numeric(pg_prng_state *state,
							  Numeric rmin, Numeric rmax);

#endif							/* _PG_NUMERIC_H_ */
