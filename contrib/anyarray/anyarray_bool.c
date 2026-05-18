/*-------------------------------------------------------------------------
 *
 * anyarray_bool.c
 *		Boolean query type for anyarray.
 *
 * Provides the anyquery type and the @@ operator.  A query value is a
 * boolean expression whose leaves are text tokens; the element type is
 * supplied at @@-time by the array operand, and the leaves are parsed
 * through the element type's text input function.
 *
 * Grammar:
 *
 *	  expr        ::= or_expr
 *	  or_expr     ::= and_expr ( '|' and_expr )*
 *	  and_expr    ::= unary_expr ( '&' unary_expr )*
 *	  unary_expr  ::= '!' unary_expr | atom
 *	  atom        ::= VALUE | '(' expr ')'
 *	  VALUE       ::= bare_token | quoted_string
 *	  bare_token  ::= any run of characters that are not whitespace,
 *	                  '&', '|', '!', '(', ')', or '"'
 *	  quoted_string := '"' chars-with-backslash-escapes '"'
 *
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/anyarray/anyarray_bool.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "anyarray.h"

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

PG_FUNCTION_INFO_V1(anyquery_in);
PG_FUNCTION_INFO_V1(anyquery_out);
PG_FUNCTION_INFO_V1(anyarray_boolop);
PG_FUNCTION_INFO_V1(anyquery_boolop_rev);
PG_FUNCTION_INFO_V1(anyquery_querytree);


/* ------------------------------------------------------------------------
 *  Parser
 * ------------------------------------------------------------------------
 */

/*
 * Working state used while building the postfix item list.
 *
 * Items are emitted in postfix (reverse polish) order directly into the
 * "items" array.  Value strings are concatenated into "strs"; each VAL item
 * stores the byte offset of its string within strs->data.
 */
typedef struct ParserState
{
	const char *cur;
	const char *end;
	AnyQueryItem *items;
	int			nitems;
	int			capitems;
	StringInfoData strs;
	struct Node *escontext;
} ParserState;

#define PARSE_FAIL(state, errc, ...) \
	do { \
		errsave((state)->escontext, \
				(errcode(errc), \
				 __VA_ARGS__)); \
		return false; \
	} while (0)

static bool parse_expr(ParserState *state);

static void
skip_ws(ParserState *state)
{
	while (state->cur < state->end &&
		   (*state->cur == ' ' || *state->cur == '\t' ||
			*state->cur == '\r' || *state->cur == '\n' ||
			*state->cur == '\v' || *state->cur == '\f'))
		state->cur++;
}

static bool
is_value_char(char c)
{
	if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
		c == '\v' || c == '\f')
		return false;
	if (c == '&' || c == '|' || c == '!' || c == '(' || c == ')' || c == '"')
		return false;
	return true;
}

static bool
emit_item(ParserState *state, int16 type, int32 payload)
{
	if (state->nitems == state->capitems)
	{
		if ((size_t) state->capitems * 2 > ANYQUERY_MAXITEMS)
			PARSE_FAIL(state, ERRCODE_PROGRAM_LIMIT_EXCEEDED,
					   errmsg("anyquery expression is too complex"));
		state->capitems *= 2;
		state->items = (AnyQueryItem *) repalloc(state->items,
												 sizeof(AnyQueryItem) *
												 state->capitems);
	}
	state->items[state->nitems].type = type;
	state->items[state->nitems].left = 0;
	state->items[state->nitems].payload = payload;
	state->nitems++;
	return true;
}

/*
 * Consume a value token, append its string (with terminating NUL) to
 * state->strs, and emit a VAL item whose payload is the byte offset of the
 * string within strs->data.
 */
static bool
parse_value(ParserState *state)
{
	int32		off;

	skip_ws(state);

	if (state->cur >= state->end)
		PARSE_FAIL(state, ERRCODE_SYNTAX_ERROR,
				   errmsg("unexpected end of input in anyquery"));

	off = state->strs.len;

	if (*state->cur == '"')
	{
		state->cur++;			/* opening quote */
		while (state->cur < state->end && *state->cur != '"')
		{
			if (*state->cur == '\\' && state->cur + 1 < state->end)
				state->cur++;
			appendStringInfoChar(&state->strs, *state->cur);
			state->cur++;
		}
		if (state->cur >= state->end)
			PARSE_FAIL(state, ERRCODE_SYNTAX_ERROR,
					   errmsg("unterminated quoted value in anyquery"));
		state->cur++;			/* closing quote */
	}
	else if (is_value_char(*state->cur))
	{
		while (state->cur < state->end && is_value_char(*state->cur))
		{
			appendStringInfoChar(&state->strs, *state->cur);
			state->cur++;
		}
	}
	else
		PARSE_FAIL(state, ERRCODE_SYNTAX_ERROR,
				   errmsg("expected value at character \"%c\"", *state->cur));

	appendStringInfoChar(&state->strs, '\0');
	return emit_item(state, ANYQ_VAL, off);
}

static bool
parse_atom(ParserState *state)
{
	skip_ws(state);
	if (state->cur < state->end && *state->cur == '(')
	{
		state->cur++;
		if (!parse_expr(state))
			return false;
		skip_ws(state);
		if (state->cur >= state->end || *state->cur != ')')
			PARSE_FAIL(state, ERRCODE_SYNTAX_ERROR,
					   errmsg("missing closing parenthesis in anyquery"));
		state->cur++;
		return true;
	}
	return parse_value(state);
}

static bool
parse_not(ParserState *state)
{
	skip_ws(state);
	if (state->cur < state->end && *state->cur == '!')
	{
		state->cur++;
		if (!parse_not(state))
			return false;
		return emit_item(state, ANYQ_OPR, ANYQ_NOT);
	}
	return parse_atom(state);
}

static bool
parse_and(ParserState *state)
{
	if (!parse_not(state))
		return false;
	for (;;)
	{
		skip_ws(state);
		if (state->cur >= state->end || *state->cur != '&')
			return true;
		state->cur++;
		if (!parse_not(state))
			return false;
		if (!emit_item(state, ANYQ_OPR, ANYQ_AND))
			return false;
	}
}

static bool
parse_expr(ParserState *state)
{
	check_stack_depth();

	if (!parse_and(state))
		return false;
	for (;;)
	{
		skip_ws(state);
		if (state->cur >= state->end || *state->cur != '|')
			return true;
		state->cur++;
		if (!parse_and(state))
			return false;
		if (!emit_item(state, ANYQ_OPR, ANYQ_OR))
			return false;
	}
}

/*
 * Compute the "left" field for every OPR item by walking the postfix array
 * top-down.  Returns false on overflow of the int16 left field.
 */
static bool
compute_lefts(ParserState *state, int *pos)
{
	int			mypos;

	check_stack_depth();

	mypos = (*pos)--;
	Assert(mypos >= 0);

	if (state->items[mypos].type == ANYQ_VAL)
	{
		state->items[mypos].left = 0;
		return true;
	}
	else if (state->items[mypos].payload == ANYQ_NOT)
	{
		state->items[mypos].left = -1;
		return compute_lefts(state, pos);
	}
	else
	{
		int			delta;

		/* binary operator: walk right operand */
		if (!compute_lefts(state, pos))
			return false;
		delta = *pos - mypos;
		if (delta < PG_INT16_MIN)
		{
			errsave(state->escontext,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("anyquery expression is too complex")));
			return false;
		}
		state->items[mypos].left = (int16) delta;
		return compute_lefts(state, pos);
	}
}


/* ------------------------------------------------------------------------
 *  Input / output / debug
 * ------------------------------------------------------------------------
 */

Datum
anyquery_in(PG_FUNCTION_ARGS)
{
	char	   *buf = PG_GETARG_CSTRING(0);
	ParserState state;
	AnyQuery   *out;
	Size		items_bytes;
	Size		total;
	int			pos;

	state.cur = buf;
	state.end = buf + strlen(buf);
	state.capitems = 16;
	state.items = (AnyQueryItem *) palloc(sizeof(AnyQueryItem) *
										  state.capitems);
	state.nitems = 0;
	initStringInfo(&state.strs);
	state.escontext = fcinfo->context;

	if (!parse_expr(&state))
		PG_RETURN_NULL();

	skip_ws(&state);
	if (state.cur < state.end)
		ereturn(state.escontext, (Datum) 0,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unexpected trailing input in anyquery")));

	if (state.nitems == 0)
		ereturn(state.escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty anyquery")));

	if ((size_t) state.nitems > ANYQUERY_MAXITEMS)
		ereturn(state.escontext, (Datum) 0,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("anyquery has too many items (%d, maximum %zu)",
						state.nitems, ANYQUERY_MAXITEMS)));

	pos = state.nitems - 1;
	if (!compute_lefts(&state, &pos))
		PG_RETURN_NULL();
	Assert(pos == -1);

	items_bytes = MAXALIGN(state.nitems * sizeof(AnyQueryItem));
	total = ANYQUERY_HDRSIZE + items_bytes + state.strs.len;

	out = (AnyQuery *) palloc0(total);
	SET_VARSIZE(out, total);
	out->size = state.nitems;
	out->str_off = ANYQUERY_HDRSIZE + items_bytes;
	out->str_len = state.strs.len;
	memcpy(out->items, state.items, state.nitems * sizeof(AnyQueryItem));
	memcpy((char *) out + out->str_off, state.strs.data, state.strs.len);

	pfree(state.items);
	pfree(state.strs.data);

	PG_RETURN_POINTER(out);
}

/*
 * Quote a value string if it contains anything the parser would consider
 * special.  Otherwise emit it verbatim.
 */
static void
append_value_token(StringInfo out, const char *s)
{
	const char *p;
	bool		need_quote = (*s == '\0');

	for (p = s; *p; p++)
	{
		if (!is_value_char(*p))
		{
			need_quote = true;
			break;
		}
	}

	if (!need_quote)
	{
		appendStringInfoString(out, s);
		return;
	}

	appendStringInfoChar(out, '"');
	for (p = s; *p; p++)
	{
		if (*p == '"' || *p == '\\')
			appendStringInfoChar(out, '\\');
		appendStringInfoChar(out, *p);
	}
	appendStringInfoChar(out, '"');
}

/*
 * Infix walker for the output function.  Returns the postfix index that
 * "cur" decremented to so the caller can chain.
 */
static int
infix_walk(StringInfo out, AnyQuery *q, int cur, bool top)
{
	AnyQueryItem *it;

	check_stack_depth();
	Assert(cur >= 0);
	it = &q->items[cur];

	if (it->type == ANYQ_VAL)
	{
		append_value_token(out, ANYQUERY_STRING(q, it));
		return cur - 1;
	}
	else if (it->payload == ANYQ_NOT)
	{
		AnyQueryItem *child = &q->items[cur - 1];
		bool		paren = (child->type == ANYQ_OPR);

		appendStringInfoChar(out, '!');
		if (paren)
			appendStringInfoString(out, "( ");
		cur = infix_walk(out, q, cur - 1, false);
		if (paren)
			appendStringInfoString(out, " )");
		return cur;
	}
	else
	{
		int			op = it->payload;
		StringInfoData right;
		int			next;

		if (op == ANYQ_OR && !top)
			appendStringInfoString(out, "( ");

		/* right operand first into a side buffer */
		initStringInfo(&right);
		next = infix_walk(&right, q, cur - 1, false);
		/* left operand into the main buffer */
		cur = infix_walk(out, q, next, false);

		appendStringInfo(out, " %c %s", op, right.data);
		pfree(right.data);

		if (op == ANYQ_OR && !top)
			appendStringInfoString(out, " )");
		return cur;
	}
}

Datum
anyquery_out(PG_FUNCTION_ARGS)
{
	AnyQuery   *q = PG_GETARG_ANYQUERY_P(0);
	StringInfoData out;

	if (q->size <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty anyquery")));

	initStringInfo(&out);
	(void) infix_walk(&out, q, q->size - 1, true);

	PG_RETURN_CSTRING(out.data);
}

/*
 * Debugging helper: produce a postfix string with operator codes spelled out.
 * Mostly useful while writing tests.
 */
Datum
anyquery_querytree(PG_FUNCTION_ARGS)
{
	AnyQuery   *q = PG_GETARG_ANYQUERY_P(0);
	StringInfoData out;
	int			i;

	initStringInfo(&out);
	for (i = 0; i < q->size; i++)
	{
		AnyQueryItem *it = &q->items[i];

		if (i > 0)
			appendStringInfoChar(&out, ' ');
		if (it->type == ANYQ_VAL)
			append_value_token(&out, ANYQUERY_STRING(q, it));
		else
			appendStringInfoChar(&out, (char) it->payload);
	}
	PG_RETURN_TEXT_P(cstring_to_text_with_len(out.data, out.len));
}


/* ------------------------------------------------------------------------
 *  Matching: anyarray @@ anyquery
 * ------------------------------------------------------------------------
 */

typedef struct MatchContext
{
	AnyArrayTypeInfo *meta;
	Datum	   *sorted;			/* sorted array elements (owned) */
	int			nelems;
	/* cached parsed values for each VAL item in the query */
	Datum	   *parsed;
	bool	   *parsed_valid;
} MatchContext;

/*
 * Look up val in the sorted element array via binary search.
 */
static bool
contains_value(MatchContext *ctx, Datum val)
{
	int			lo = 0;
	int			hi = ctx->nelems;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;
		int			c = DatumGetInt32(FunctionCall2Coll(&ctx->meta->cmp_proc,
														ctx->meta->typcollation,
														ctx->sorted[mid],
														val));

		if (c == 0)
			return true;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

/*
 * Evaluate one node of the postfix tree.  Recursive on operators.
 */
static bool
eval_item(AnyQuery *q, int idx, MatchContext *ctx, Oid input_func,
		  int input_typioparam, int32 input_typmod)
{
	AnyQueryItem *it;

	check_stack_depth();
	Assert(idx >= 0);
	it = &q->items[idx];

	if (it->type == ANYQ_VAL)
	{
		Datum		v;

		if (!ctx->parsed_valid[idx])
		{
			const char *s = ANYQUERY_STRING(q, it);

			v = OidInputFunctionCall(input_func, (char *) s,
									 input_typioparam, input_typmod);
			ctx->parsed[idx] = v;
			ctx->parsed_valid[idx] = true;
		}
		return contains_value(ctx, ctx->parsed[idx]);
	}
	else if (it->payload == ANYQ_NOT)
	{
		return !eval_item(q, idx - 1, ctx, input_func,
						  input_typioparam, input_typmod);
	}
	else if (it->payload == ANYQ_AND)
	{
		if (!eval_item(q, idx + it->left, ctx, input_func,
					   input_typioparam, input_typmod))
			return false;
		return eval_item(q, idx - 1, ctx, input_func,
						 input_typioparam, input_typmod);
	}
	else						/* ANYQ_OR */
	{
		if (eval_item(q, idx + it->left, ctx, input_func,
					  input_typioparam, input_typmod))
			return true;
		return eval_item(q, idx - 1, ctx, input_func,
						 input_typioparam, input_typmod);
	}
}

static Datum
do_boolop(FunctionCallInfo fcinfo, ArrayType *arr, AnyQuery *q)
{
	AnyArrayTypeInfo *meta;
	Datum	   *values;
	bool	   *nulls;
	int			nelems;
	Oid			input_func;
	Oid			input_typioparam;
	bool		result;
	MatchContext ctx;

	ANYARRAY_CHECK_ARRAY(arr);

	if (q->size <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty anyquery")));

	meta = anyarray_get_meta(fcinfo, ARR_ELEMTYPE(arr), false);

	if (ARR_NDIM(arr) == 0)
	{
		nelems = 0;
		values = NULL;
	}
	else
	{
		deconstruct_array(arr, meta->element_type, meta->typlen,
						  meta->typbyval, meta->typalign,
						  &values, &nulls, &nelems);
		pfree(nulls);
		if (nelems > 1)
			qsort_arg(values, nelems, sizeof(Datum),
					  anyarray_cmp_datum, meta);
	}

	getTypeInputInfo(meta->element_type, &input_func, &input_typioparam);

	ctx.meta = meta;
	ctx.sorted = values;
	ctx.nelems = nelems;
	ctx.parsed = (Datum *) palloc0(sizeof(Datum) * q->size);
	ctx.parsed_valid = (bool *) palloc0(sizeof(bool) * q->size);

	result = eval_item(q, q->size - 1, &ctx, input_func,
					   input_typioparam, -1);

	pfree(ctx.parsed);
	pfree(ctx.parsed_valid);
	if (values)
		pfree(values);

	PG_RETURN_BOOL(result);
}

Datum
anyarray_boolop(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	AnyQuery   *q = PG_GETARG_ANYQUERY_P(1);
	Datum		result = do_boolop(fcinfo, arr, q);

	PG_FREE_IF_COPY(arr, 0);
	PG_FREE_IF_COPY(q, 1);
	return result;
}

/*
 * Commutator: anyquery ~~ anyarray simply swaps the arguments.
 */
Datum
anyquery_boolop_rev(PG_FUNCTION_ARGS)
{
	AnyQuery   *q = PG_GETARG_ANYQUERY_P(0);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	Datum		result = do_boolop(fcinfo, arr, q);

	PG_FREE_IF_COPY(q, 0);
	PG_FREE_IF_COPY(arr, 1);
	return result;
}
