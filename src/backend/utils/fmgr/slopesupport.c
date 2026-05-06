#include "postgres.h"

#include "c.h"
#include "nodes/supportnodes.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
/**
 * Extended numeric sign, the usual -1, 0, 1,
 * for real numbers, and additional values for non-real
 * numeric values and null.
*/

typedef enum SLOPE_SIGN
{
	SLOPE_SIGN_NINF = -2,
	SLOPE_SIGN_NEG = -1,
	SLOPE_SIGN_ZERO = 0,
	SLOPE_SIGN_POS = 1,
	SLOPE_SIGN_PINF = 2,
	SLOPE_SIGN_NAN = 3,
	SLOPE_SIGN_NULL = 4,
} SLOPE_SIGN;


/*
 * monotonic_slope_support
 *		Generic helper for prosupport functions that declare monotonic slopes.
 *
 * If the request is a SupportRequestMonotonic, fills in nslopes and slopes
 * and returns the request pointer.  Otherwise returns NULL.
 */
static Datum
monotonic_slope_support(Node *rawreq, int nslopes,
						const MonotonicFunction *slopes)
{
	if (IsA(rawreq, SupportRequestMonotonic))
	{
		SupportRequestMonotonic *req = (SupportRequestMonotonic *) rawreq;

		req->nslopes = nslopes;
		req->slopes = slopes;
		return PointerGetDatum(req);
	}

	return PointerGetDatum(NULL);
}


static bool
get_2slope_args(Node *rawreq, Node **arg0, Node **arg1)
{
	SupportRequestMonotonic *req = (SupportRequestMonotonic *) rawreq;
	List	   *args;

	if (!IsA(rawreq, SupportRequestMonotonic))
		return false;
	if (IsA(req->expr, FuncExpr))
		args = ((FuncExpr *) req->expr)->args;
	else if (IsA(req->expr, OpExpr))
		args = ((OpExpr *) req->expr)->args;
	else
		return false;
	if (list_length(args) < 2)
		return false;
	*arg0 = (Node *) linitial(args);
	*arg1 = (Node *) lsecond(args);
	return true;
}

/*
 * get_const_sign
 *  Helper to determine the sign of a numeric constant.
 *  It will classify a number according
 */
static inline SLOPE_SIGN
get_const_sign(Const *constval)
{
	if (constval->constisnull)
		return SLOPE_SIGN_NULL;

	switch (constval->consttype)
	{
		case INT2OID:
			{
				int16		val = DatumGetInt16(constval->constvalue);

				return (val > 0) ? 1 : (val < 0) ? -1 : 0;
			}
		case INT4OID:
			{
				int32		val = DatumGetInt32(constval->constvalue);

				return (val > 0) ? 1 : (val < 0) ? -1 : 0;
			}
		case INT8OID:
			{
				int64		val = DatumGetInt64(constval->constvalue);

				return (val > 0) ? 1 : (val < 0) ? -1 : 0;
			}
		case FLOAT4OID:
			{
				float4		val = DatumGetFloat4(constval->constvalue);

				if (isnan(val))
					return SLOPE_SIGN_NAN;
				if (isinf(val))
					return val > 0 ? SLOPE_SIGN_PINF : SLOPE_SIGN_NINF;
				else if (val == 0)
					return SLOPE_SIGN_ZERO;
				else
					return val > 0 ? SLOPE_SIGN_POS : SLOPE_SIGN_NEG;
			}
		case FLOAT8OID:
			{
				float8		val = DatumGetFloat8(constval->constvalue);

				if (isnan(val))
					return SLOPE_SIGN_NAN;
				if (isinf(val))
					return val > 0 ? SLOPE_SIGN_PINF : SLOPE_SIGN_NINF;
				else if (val == 0)
					return SLOPE_SIGN_ZERO;
				else
					return val > 0 ? SLOPE_SIGN_POS : SLOPE_SIGN_NEG;
			}
		case NUMERICOID:
			{
				Numeric		num = DatumGetNumeric(constval->constvalue);
				Datum		result;
				Numeric		sign_num;
				int			val_sign;

				if (numeric_is_nan(num))
					return SLOPE_SIGN_NAN;

				result = DirectFunctionCall1(numeric_sign, NumericGetDatum(num));
				sign_num = DatumGetNumeric(result);
				val_sign = numeric_int4_safe(sign_num, NULL);
				if (numeric_is_inf(num))
					return val_sign == 1 ? SLOPE_SIGN_PINF : SLOPE_SIGN_NINF;
				else
					return (enum SLOPE_SIGN) val_sign;
			}
		default:
			return 0;
	}
}

static const MonotonicFunction asc_slope[2] = {MONOTONICFUNC_INCREASING, MONOTONICFUNC_INCREASING};
static const MonotonicFunction desc_slope[2] = {MONOTONICFUNC_DECREASING, MONOTONICFUNC_DECREASING};
static const MonotonicFunction flat_slope[2] = {MONOTONICFUNC_BOTH, MONOTONICFUNC_BOTH};
static const MonotonicFunction diff_slope[2] = {MONOTONICFUNC_INCREASING, MONOTONICFUNC_DECREASING};



/*
 * Prosupport: f(x, ...) is monotonically increasing in x.
 */
Datum
arg0_asc_slope_support(PG_FUNCTION_ARGS)
{
	static const MonotonicFunction pattern[1] = {MONOTONICFUNC_INCREASING};

	return monotonic_slope_support((Node *) PG_GETARG_POINTER(0),
								   lengthof(pattern), pattern);
}

 /*
  * Prosupport: f(x, ...) is monotonically decreasing in x.
  */
Datum
arg0_desc_slope_support(PG_FUNCTION_ARGS)
{
	static const MonotonicFunction pattern[1] = {MONOTONICFUNC_DECREASING};

	return monotonic_slope_support((Node *) PG_GETARG_POINTER(0),
								   lengthof(pattern), pattern);
}

 /*
  * Prosupport: f(a, x, ...) is monotonically increasing in x.
  */
Datum
arg1_asc_slope_support(PG_FUNCTION_ARGS)
{
	static const MonotonicFunction pattern[2] = {MONOTONICFUNC_NONE,
	MONOTONICFUNC_INCREASING};

	return monotonic_slope_support((Node *) PG_GETARG_POINTER(0),
								   lengthof(pattern), pattern);
}

/*
 * diff_slope_support
 *		Prosupport: f(x, y) = x - y is increasing in x, decreasing in y.
 */
Datum
diff_slope_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *arg0;
	Node	   *arg1;

	if (!get_2slope_args(rawreq, &arg0, &arg1))
		PG_RETURN_POINTER(NULL);

	if (IsA(arg0, Const))
	{
		switch (get_const_sign((Const *) arg0))
		{
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(rawreq, 2, flat_slope);
			default:
				break;
		}
	}
	else if (IsA(arg1, Const))
	{
		switch (get_const_sign((Const *) arg1))
		{
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(rawreq, 2, flat_slope);
			default:
				break;
		}
	}

	return monotonic_slope_support(rawreq, 2, diff_slope);
}

/*
 * addition_slope_support
 *		Prosupport: f(x, y) = x + y is increasing in both x and y.
 */
Datum
addition_slope_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *arg0;
	Node	   *arg1;

	if (!get_2slope_args(rawreq, &arg0, &arg1))
		PG_RETURN_POINTER(NULL);

	if (IsA(arg0, Const))
	{
		switch (get_const_sign((Const *) arg0))
		{
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(rawreq, 2, flat_slope);
			default:
				break;
		}
	}
	else if (IsA(arg1, Const))
	{
		switch (get_const_sign((Const *) arg1))
		{
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(rawreq, 2, flat_slope);
			default:
				break;
		}
	}

	return monotonic_slope_support(rawreq, 2, asc_slope);
}

/*
 * multiply_slope_support
 *		Prosupport: x * c is increasing if c > 0, decreasing if c < 0.
 *		Similarly for c * x.
 *
 * For multiplication, the monotonicity depends on the sign of the constant:
 * - x * positive_const: increasing in x
 * - x * negative_const: decreasing in x
 * - x * 0: not monotonic (constant result)
 */
Datum
multiply_slope_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Const	   *constval;
	Node	   *arg0;
	Node	   *arg1;

	if (!get_2slope_args(rawreq, &arg0, &arg1))
		PG_RETURN_POINTER(NULL);

	if (IsA(arg0, Const))
		constval = (Const *) arg0;
	else if (IsA(arg1, Const))
		constval = (Const *) arg1;
	else
		PG_RETURN_POINTER(NULL);

	switch (get_const_sign(constval))
	{
		case SLOPE_SIGN_POS:
			return monotonic_slope_support(rawreq, 2, asc_slope);
		case SLOPE_SIGN_NEG:
			return monotonic_slope_support(rawreq, 2, desc_slope);
		case SLOPE_SIGN_NAN:
			return monotonic_slope_support(rawreq, 2, flat_slope);
		case SLOPE_SIGN_ZERO:
			return monotonic_slope_support(rawreq, 2, flat_slope);
		default:
			PG_RETURN_POINTER(NULL);
	}

}

/*
 * divide_slope_support
 *		Prosupport: x / c is increasing if c > 0, decreasing if c < 0.
 *
 * Division by a constant has the same monotonicity as multiplication:
 * - x / positive_const: increasing in x
 * - x / negative_const: decreasing in x
 * - x / 0: undefined (not monotonic)
 */
Datum
divide_slope_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Const	   *constval;
	Node	   *arg0;
	Node	   *arg1;

	if (!get_2slope_args(rawreq, &arg0, &arg1))
		PG_RETURN_POINTER(NULL);

	if (!IsA(arg1, Const))
		PG_RETURN_POINTER(NULL);

	constval = (Const *) arg1;
	switch (get_const_sign(constval))
	{
		case SLOPE_SIGN_POS:
			return monotonic_slope_support(rawreq, 2, asc_slope);
		case SLOPE_SIGN_NEG:
			return monotonic_slope_support(rawreq, 2, desc_slope);
		case SLOPE_SIGN_NAN:
			return monotonic_slope_support(rawreq, 2, flat_slope);
		case SLOPE_SIGN_PINF:
		case SLOPE_SIGN_NINF:
			return monotonic_slope_support(rawreq, 2, flat_slope);
		default:
			PG_RETURN_POINTER(NULL);
	}
	PG_RETURN_POINTER(NULL);
}
