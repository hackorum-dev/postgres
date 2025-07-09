/*-------------------------------------------------------------------------
 *
 * jsonbsubs.c
 *	  Subscripting support functions for jsonb.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonbsubs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execExpr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"


/*
 * SubscriptingRefState.workspace for generic jsonb subscripting execution.
 *
 * Stores state for both jsonb simple subscripting and dot notation access.
 * Dot notation additionally uses `jsonpath` for JsonPath evaluation.
 */
typedef struct JsonbSubWorkspace
{
	bool		expectArray;	/* jsonb root is expected to be an array */
	Oid		   *indexOid;		/* OID of coerced subscript expression, could
								 * be only integer or text */
	Datum	   *index;			/* Subscript values in Datum format */
	JsonPath   *jsonpath;		/* JsonPath for dot notation execution via
								 * JsonPathQuery() */
} JsonbSubWorkspace;

/*
 * During transformation, determine whether to build a JsonPath
 * for JsonPathQuery() execution.
 *
 * JsonPath is needed if the indirection list includes:
 * - String-based access (dot notation)
 * - Slice-based subscripting (when isSlice is true)
 *
 * Otherwise, simple jsonb subscripting is enough.
 */
static bool
jsonb_check_jsonpath_needed(List *indirection)
{
	ListCell   *lc;

	foreach(lc, indirection)
	{
		Node	   *accessor = lfirst(lc);

		if (IsA(accessor, String))
			return true;
		else
			Assert(IsA(accessor, A_Indices));
	}

	return false;
}

/*
 * Helper functions for constructing JsonPath expressions.
 *
 * The make_jsonpath_item_* functions create various types of JsonPathParseItem
 * nodes, which are used to build JsonPath expressions for jsonb simplified
 * accessor.
 */

static JsonPathParseItem *
make_jsonpath_item(JsonPathItemType type)
{
	JsonPathParseItem *v = palloc(sizeof(*v));

	v->type = type;
	v->next = NULL;

	return v;
}

/*
 * Convert a constant integer expression into a JsonPathParseItem.
 *
 * The input expression must be a non-null constant of type INT4. Returns NULL otherwise.
 * This function constructs a jpiNumeric item for use in JsonPath and appends a matching
 * Const(INT4) node to the given expression list for use in EXPLAIN, views, etc.
 *
 * Parameters:
 * - pstate: parse state context
 * - expr: input expression node
 * - exprs: list of expression nodes (updated in place)
 */
static JsonPathParseItem *
make_jsonpath_item_expr(ParseState *pstate, Node *expr, List **exprs)
{
	Const	   *cnst;

	expr = transformExpr(pstate, expr, pstate->p_expr_kind);

	if (IsA(expr, Const))
	{
		cnst = (Const *) expr;
		if (cnst->consttype == INT4OID && !(cnst->constisnull))
		{
			JsonPathParseItem *jpi = make_jsonpath_item(jpiNumeric);

			jpi->value.numeric =
				DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(cnst->constvalue)));

			*exprs = lappend(*exprs, makeConst(INT4OID, -1, InvalidOid, 4,
											   Int32GetDatum(cnst->constvalue), false, true));

			return jpi;
		}
	}

	return NULL;
}

/*
 * Constructs a JsonPath expression from a list of indirections.
 * This function is used when jsonb subscripting involves dot notation,
 * requiring JsonPath-based evaluation.
 *
 * The function modifies the indirection list in place, removing processed
 * elements as it converts them into JsonPath components, as follows:
 * - String keys (dot notation) -> jpiKey items.
 * - Array indices -> jpiIndexArray items.
 *
 * In addition to building the JsonPath expression, this function populates
 * the following fields of the given SubscriptingRef:
 * - refprivate: the generated JsonPath
 * - refskipsubscripteval: set to true when refprivate is used
 * - refupperindexpr: upper index expressions (object keys or array indexes)
 * - reflowerindexpr: lower index expressions, remains NIL as slices are not supported.
 *
 * Parameters:
 * - pstate: Parse state context.
 * - indirection: List of subscripting expressions (modified in-place).
 * - sbsref: SubscriptingRef node to update
 */
static int
jsonb_subscript_make_jsonpath(ParseState *pstate, List *indirection, SubscriptingRef *sbsref)
{
	JsonPathParseResult jpres;
	JsonPathParseItem *path = make_jsonpath_item(jpiRoot);
	ListCell   *lc;
	Datum		jsp;
	int			pathlen = 0;

	sbsref->refupperindexpr = NIL;
	sbsref->reflowerindexpr = NIL;
	sbsref->refprivate = NULL;

	jpres.expr = path;
	jpres.lax = true;

	foreach(lc, indirection)
	{
		Node	   *accessor = lfirst(lc);
		JsonPathParseItem *jpi;

		if (IsA(accessor, String))
		{
			char	   *field = strVal(accessor);
			FieldAccessorExpr *accessor_expr;

			jpi = make_jsonpath_item(jpiKey);
			jpi->value.string.val = field;
			jpi->value.string.len = strlen(field);

			accessor_expr = makeNode(FieldAccessorExpr);
			accessor_expr->type = T_FieldAccessorExpr;
			accessor_expr->fieldname = field;

			sbsref->refupperindexpr = lappend(sbsref->refupperindexpr, accessor_expr);
		}
		else if (IsA(accessor, A_Indices))
		{
			A_Indices  *ai = castNode(A_Indices, accessor);

			if (!ai->is_slice)
			{
				JsonPathParseItem *jpi_from = NULL;

				Assert(ai->uidx && !ai->lidx);
				jpi_from = make_jsonpath_item_expr(pstate, ai->uidx, &sbsref->refupperindexpr);
				if (jpi_from == NULL)
				{
					/*
					 * Break out of the loop if the subscript is not a
					 * non-null integer constant, so that we can fall back to
					 * jsonb subscripting logic.
					 *
					 * This is needed to handle cases with mixed usage of SQL
					 * standard json simplified accessor syntax and PostgreSQL
					 * jsonb subscripting syntax, e.g:
					 *
					 * select (jb).a['b'].c from jsonb_table;
					 *
					 * where dot-notation (.a and .c) is the SQL standard json
					 * simplified accessor syntax, and the ['b'] subscript is
					 * the PostgreSQL jsonb subscripting syntax, because 'b'
					 * is not a non-null constant integer and cannot be used
					 * for json array access.
					 *
					 * In this case, we cannot create a JsonPath item, so we
					 * break out of the loop and let
					 * jsonb_subscript_transform() handle this indirection as
					 * a PostgreSQL jsonb subscript.
					 */
					break;
				}

				jpi = make_jsonpath_item(jpiIndexArray);
				jpi->value.array.nelems = 1;
				jpi->value.array.elems = palloc(sizeof(jpi->value.array.elems[0]));

				jpi->value.array.elems[0].from = jpi_from;
				jpi->value.array.elems[0].to = NULL;
			}
			else
			{
				Node	   *expr = ai->uidx ? ai->uidx : ai->lidx;

				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("jsonb subscript does not support slices"),
						 parser_errposition(pstate, exprLocation(expr))));
			}
		}
		else
		{
			/*
			 * Unexpected node type in indirection list. This should not
			 * happen with current grammar, but we handle it defensively by
			 * breaking out of the loop rather than crashing. In case of
			 * future grammar changes that might introduce new node types,
			 * this allows us to create a jsonpath from as many indirection
			 * elements as we can and let transformIndirection() fallback to
			 * alternative logic to handle the remaining indirection elements.
			 */
			Assert(false);		/* not reachable */
			break;
		}

		/* append path item */
		path->next = jpi;
		path = jpi;
		pathlen++;
	}

	if (pathlen != 0)
	{
		jsp = jsonPathFromParseResult(&jpres, 0, NULL);
		sbsref->refprivate = (Node *) makeConst(JSONPATHOID, -1, InvalidOid, -1, jsp, false, false);
		sbsref->refskipsubscripteval = true;
	}

	return pathlen;
}

/*
 * Finish parse analysis of a SubscriptingRef expression for a jsonb.
 *
 * This is a partial transform function that transforms the subscript
 * expressions, coerces them to text, and determines the result type
 * of the SubscriptingRef node.
 *
 * Returns the number of indirection elements processed.
 */
static int
jsonb_subscript_transform(SubscriptingRef *sbsref,
						  List *indirection,
						  ParseState *pstate,
						  bool isAssignment)
{
	List	   *upperIndexpr = NIL;
	ListCell   *idx;

	/* Determine the result type of the subscripting operation; always jsonb */
	sbsref->refrestype = JSONBOID;
	sbsref->reftypmod = -1;

	/*
	 * For dot notation (first element is a String), check if a function with
	 * that name exists. If so, let the parser try function call instead. This
	 * preserves backward compatibility with existing single-argument
	 * functions like jsonb_typeof(), jsonb_array_length(), etc.
	 */
	if (IsA(linitial(indirection), String))
	{
		char	   *fieldname = strVal(linitial(indirection));
		Oid			argtypes[1] = {sbsref->refcontainertype};
		Oid			funcoid;

		funcoid = LookupFuncName(list_make1(makeString(pstrdup(fieldname))),
								 1, argtypes, true);
		if (OidIsValid(funcoid))
			return 0;
	}

	if (jsonb_check_jsonpath_needed(indirection))
	{
		int			pathlen;

		pathlen = jsonb_subscript_make_jsonpath(pstate, indirection, sbsref);
		if (sbsref->refprivate)
			return pathlen;
	}

	/*
	 * We reach here only in two cases: (a) the JSON simplified accessor is
	 * not needed at all (for example, a plain array subscript like [1] or
	 * object key access like ['a']), or (b) jsonb_subscript_make_jsonpath()
	 * was called but could not complete the JsonPath construction (for
	 * example, when mixing dot notation with non-integer subscripts like
	 * (jb)['a'].b where 'a' is not a constant integer).
	 *
	 * In both cases we fall back to pre-standard jsonb subscripting, coercing
	 * each subscript to array index or object key as needed.
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai;
		Node	   *subExpr;

		if (!IsA(lfirst(idx), A_Indices))
			break;

		ai = lfirst_node(A_Indices, idx);

		if (ai->is_slice)
		{
			Node	   *expr = ai->uidx ? ai->uidx : ai->lidx;

			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(expr))));
		}

		if (ai->uidx)
		{
			Oid			subExprType;
			Oid			targetType = InvalidOid;

			subExpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			subExprType = exprType(subExpr);

			if (subExprType != UNKNOWNOID)
			{
				const Oid	targets[] = {INT4OID, TEXTOID};

				/*
				 * Jsonb can handle multiple subscript types, but cases when a
				 * subscript could be coerced to multiple target types must be
				 * avoided, similar to overloaded functions. It could be
				 * possibly extended with jsonpath in the future.
				 */
				for (int i = 0; i < lengthof(targets); i++)
				{
					if (can_coerce_type(1, &subExprType, &targets[i], COERCION_IMPLICIT))
					{
						/*
						 * One type has already succeeded, it means there are
						 * two coercion targets possible, failure.
						 */
						if (OidIsValid(targetType))
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
									 errhint("jsonb subscript must be coercible to only one type, integer or text."),
									 parser_errposition(pstate, exprLocation(subExpr))));

						targetType = targets[i];
					}
				}

				/*
				 * No suitable types were found, failure.
				 */
				if (!OidIsValid(targetType))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
							 errhint("jsonb subscript must be coercible to either integer or text."),
							 parser_errposition(pstate, exprLocation(subExpr))));
			}
			else
				targetType = TEXTOID;

			/*
			 * We know from can_coerce_type that coercion will succeed, so
			 * coerce_type could be used. Note the implicit coercion context,
			 * which is required to handle subscripts of different types,
			 * similar to overloaded functions.
			 */
			subExpr = coerce_type(pstate,
								  subExpr, subExprType,
								  targetType, -1,
								  COERCION_IMPLICIT,
								  COERCE_IMPLICIT_CAST,
								  -1);
		}
		else
		{
			/*
			 * Slice with omitted upper bound. Should not happen as we already
			 * errored out on slice earlier, but handle this just in case.
			 */
			Assert(ai->is_slice);
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(ai->uidx))));
		}

		upperIndexpr = lappend(upperIndexpr, subExpr);
	}

	/* store the transformed lists into the SubscriptingRef node */
	sbsref->refupperindexpr = upperIndexpr;
	sbsref->reflowerindexpr = NIL;

	return list_length(upperIndexpr);
}

/*
 * During execution, process the subscripts in a SubscriptingRef expression.
 *
 * The subscript expressions are already evaluated in Datum form in the
 * SubscriptingRefState's arrays.  Check and convert them as necessary.
 *
 * If any subscript is NULL, we throw error in assignment cases, or in fetch
 * cases set result to NULL and return false (instructing caller to skip the
 * rest of the SubscriptingRef sequence).
 */
static bool
jsonb_subscript_check_subscripts(ExprState *state,
								 ExprEvalStep *op,
								 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;

	/*
	 * In case if the first subscript is an integer, the source jsonb is
	 * expected to be an array. This information is not used directly, all
	 * such cases are handled within corresponding jsonb assign functions. But
	 * if the source jsonb is NULL the expected type will be used to construct
	 * an empty source.
	 */
	if (sbsrefstate->numupper > 0 && sbsrefstate->upperprovided[0] &&
		!sbsrefstate->upperindexnull[0] && workspace->indexOid[0] == INT4OID)
		workspace->expectArray = true;

	/* Process upper subscripts */
	for (int i = 0; i < sbsrefstate->numupper; i++)
	{
		if (sbsrefstate->upperprovided[i])
		{
			/* If any index expr yields NULL, result is NULL or error */
			if (sbsrefstate->upperindexnull[i])
			{
				if (sbsrefstate->isassignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("jsonb subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}

			/*
			 * For jsonb fetch and assign functions we need to provide path in
			 * text format. Convert if it's not already text.
			 */
			if (!workspace->jsonpath && workspace->indexOid[i] == INT4OID)
			{
				Datum		datum = sbsrefstate->upperindex[i];
				char	   *cs = DatumGetCString(DirectFunctionCall1(int4out, datum));

				workspace->index[i] = CStringGetTextDatum(cs);
			}
			else
				workspace->index[i] = sbsrefstate->upperindex[i];
		}
	}

	return true;
}

/*
 * Evaluate SubscriptingRef fetch for a jsonb element.
 *
 * Source container is in step's result variable (it's known not NULL, since
 * we set fetch_strict to true).
 */
static void
jsonb_subscript_fetch(ExprState *state,
					  ExprEvalStep *op,
					  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;

	/* Should not get here if source jsonb (or any subscript) is null */
	Assert(!(*op->resnull));

	if (workspace->jsonpath)
	{
		bool		empty = false;
		bool		error = false;

		*op->resvalue = JsonPathQuery(*op->resvalue, workspace->jsonpath,
									  JSW_CONDITIONAL,
									  &empty, &error, NULL,
									  NULL);

		*op->resnull = empty || error;
	}
	else
	{
		Jsonb	   *jsonbSource = DatumGetJsonbP(*op->resvalue);

		*op->resvalue = jsonb_get_element(jsonbSource,
										  workspace->index,
										  sbsrefstate->numupper,
										  op->resnull,
										  false);
	}
}

/*
 * Evaluate SubscriptingRef assignment for a jsonb element assignment.
 *
 * Input container (possibly null) is in result area, replacement value is in
 * SubscriptingRefState's replacevalue/replacenull.
 */
static void
jsonb_subscript_assign(ExprState *state,
					   ExprEvalStep *op,
					   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;
	Jsonb	   *jsonbSource;
	JsonbValue	replacevalue;

	if (sbsrefstate->replacenull)
		replacevalue.type = jbvNull;
	else
		JsonbToJsonbValue(DatumGetJsonbP(sbsrefstate->replacevalue),
						  &replacevalue);

	/*
	 * In case if the input container is null, set up an empty jsonb and
	 * proceed with the assignment.
	 */
	if (*op->resnull)
	{
		JsonbValue	newSource;

		/*
		 * To avoid any surprising results, set up an empty jsonb array in
		 * case of an array is expected (i.e. the first subscript is integer),
		 * otherwise jsonb object.
		 */
		if (workspace->expectArray)
		{
			newSource.type = jbvArray;
			newSource.val.array.nElems = 0;
			newSource.val.array.rawScalar = false;
		}
		else
		{
			newSource.type = jbvObject;
			newSource.val.object.nPairs = 0;
		}

		jsonbSource = JsonbValueToJsonb(&newSource);
		*op->resnull = false;
	}
	else
		jsonbSource = DatumGetJsonbP(*op->resvalue);

	*op->resvalue = jsonb_set_element(jsonbSource,
									  workspace->index,
									  sbsrefstate->numupper,
									  &replacevalue);
	/* The result is never NULL, so no need to change *op->resnull */
}

/*
 * Compute old jsonb element value for a SubscriptingRef assignment
 * expression.  Will only be called if the new-value subexpression
 * contains SubscriptingRef or FieldStore.  This is the same as the
 * regular fetch case, except that we have to handle a null jsonb,
 * and the value should be stored into the SubscriptingRefState's
 * prevvalue/prevnull fields.
 */
static void
jsonb_subscript_fetch_old(ExprState *state,
						  ExprEvalStep *op,
						  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;

	if (*op->resnull)
	{
		/* whole jsonb is null, so any element is too */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
	{
		Jsonb	   *jsonbSource = DatumGetJsonbP(*op->resvalue);

		sbsrefstate->prevvalue = jsonb_get_element(jsonbSource,
												   sbsrefstate->upperindex,
												   sbsrefstate->numupper,
												   &sbsrefstate->prevnull,
												   false);
	}
}

/*
 * Set up execution state for a jsonb subscript operation. Opposite to the
 * arrays subscription, there is no limit for number of subscripts as jsonb
 * type itself doesn't have nesting limits.
 */
static void
jsonb_exec_setup(const SubscriptingRef *sbsref,
				 SubscriptingRefState *sbsrefstate,
				 SubscriptExecSteps *methods)
{
	JsonbSubWorkspace *workspace;
	ListCell   *lc;
	int			nupper = list_length(sbsref->refupperindexpr);
	char	   *ptr;

	/* Allocate type-specific workspace with space for per-subscript data */
	workspace = palloc0(MAXALIGN(sizeof(JsonbSubWorkspace)) +
						nupper * (sizeof(Datum) + sizeof(Oid)));
	workspace->expectArray = false;
	ptr = ((char *) workspace) + MAXALIGN(sizeof(JsonbSubWorkspace));

	if (sbsref->refprivate)
		workspace->jsonpath = DatumGetJsonPathP(castNode(Const, sbsref->refprivate)->constvalue);

	/*
	 * This coding assumes sizeof(Datum) >= sizeof(Oid), else we might
	 * misalign the indexOid pointer
	 */
	workspace->index = (Datum *) ptr;
	ptr += nupper * sizeof(Datum);
	workspace->indexOid = (Oid *) ptr;

	sbsrefstate->workspace = workspace;

	/* Collect subscript data types necessary at execution time */
	foreach(lc, sbsref->refupperindexpr)
	{
		Node	   *expr = lfirst(lc);
		int			i = foreach_current_index(lc);

		workspace->indexOid[i] = exprType(expr);
	}

	/*
	 * Pass back pointers to appropriate step execution functions.
	 */
	methods->sbs_check_subscripts = jsonb_subscript_check_subscripts;
	methods->sbs_fetch = jsonb_subscript_fetch;
	methods->sbs_assign = jsonb_subscript_assign;
	methods->sbs_fetch_old = jsonb_subscript_fetch_old;
}

/*
 * jsonb_subscript_handler
 *		Subscripting handler for jsonb.
 *
 */
Datum
jsonb_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = NULL,		/* jsonb uses partial transform instead */
		.transform_partial = jsonb_subscript_transform,
		.exec_setup = jsonb_exec_setup,
		.fetch_strict = true,	/* fetch returns NULL for NULL inputs */
		.fetch_leakproof = true,	/* fetch returns NULL for bad subscript */
		.store_leakproof = false	/* ... but assignment throws error */
	};

	PG_RETURN_POINTER(&sbsroutines);
}
