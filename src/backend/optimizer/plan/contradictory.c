/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/contradictory.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "utils/array.h"
#include "access/nbtree.h"
#include "lib/qunique.h"
#include "common/int.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/paths.h"

/*  
 *  Self-contradictory definations
 *
 */
#define DEFAULT_ELEMS_NUM	4
#define STRATEGY_NOT_EQUAL (BTMaxStrategyNumber + 1)	/* operator '<>' */

typedef enum ClauseCxtType
{
	CCT_DEL,
	CCT_CONSTT,		/* Evaluated expr is const and true. */
	CCT_CONSTF,		/* Evaluated expr is const and false. */
	CCT_REPLACE
}ClauseCxtType;

#define NT_MARK 			(1<<7)
#define NT_IS_NULL 			(1<<IS_NULL)
#define NT_IS_NOT_NULL 		(1<<IS_NOT_NULL)

/* Boolean test definations. */
#define BT_MARK 			(1<<7)
#define BT_IS_TRUE 			(1<<IS_TRUE)
#define BT_IS_NOT_TRUE 		(1<<IS_NOT_TRUE)
#define BT_IS_FALSE 		(1<<IS_FALSE)
#define BT_IS_NOT_FALSE 	(1<<IS_NOT_FALSE)
#define BT_IS_UNKNOWN 		(1<<IS_UNKNOWN)
#define BT_IS_NOT_UNKNOWN 	(1<<IS_NOT_UNKNOWN)

typedef struct ConstVal
{
	bool		push_down;
	uint16		strategy;
	uint32 		idx;
	Expr		*expr;
	Datum		*value;
	FmgrInfo	op_func;
}ConstVal;

typedef struct SAEqualDatElem
{
	uint32 		idx;
	int			elem_nums;
	Expr		*expr;
	Datum		*elem_values;
}SAEqualDatElem;

typedef struct SortArrayContext
{
	FmgrInfo    sortproc;
	Oid			collation;
	Oid			elmtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	bool		reverse;
} SortArrayContext;

typedef struct SortNeConstCxt
{
	FmgrInfo    cmp_proc;
	Oid			collation;
	bool		reverse;
} SortNeConstCxt;

typedef struct SAEqualDat
{
	RegProcedure		opfuncid;
	SortArrayContext	sortcxt;
	List		*upper_elems;
	List		*elemnents;		/* Type of SAEqualDatElem. */
}SAEqualDat;

typedef struct NeConstDat
{
	uint32 		idx;
	Datum		*val;
}NeConstDat;

typedef struct NeConstVal
{
	SortNeConstCxt	sncc;
	bool		can_sort;
	bool		sorted;
	FmgrInfo	op_func;
	int 		size;
	int 		capacity;
	NeConstDat	*ncds;
	struct NeConstVal	*upper_ncv;
}NeConstVal;

typedef struct VarattInfo
{
	uint8 		stratcount;
	uint8       null_test;
	uint8       bool_test;
	uint16		atidx;
	Oid			collid;
	Oid			type;
	NeConstVal	ne_const;
	SAEqualDat	saop;
	ConstVal	st_table[BTMaxStrategyNumber];
	struct VarattInfo *next;
}VarattInfo;

typedef struct RowCompElem
{
	Oid		opid;
	Oid		stricter_opid;
	Expr	*contact;
}RowCompElem;

typedef struct ExtractedAttris
{
	bool		addOp;
	AttrNumber	min_attr;
	VarattInfo **array_vi;
	VarattInfo	*att_lst;
	VarattInfo	*last;
	struct ExtractedAttris *upper_ea;
}ExtractedAttris;

typedef struct ClauseContext
{
	ClauseCxtType	cct;
	uint32	idx;
	Expr	*origin;
	void 	*cxt;
}ClauseContext;

typedef struct ClauseCxtWapper
{
	List 		*clausecxt;		/* Type of ClauseContext. */
}ClauseCxtWapper;

typedef struct AttrOpFamily
{
	Oid 	negator;
	Oid 	opfamily;
}AttrOpFamily;

typedef struct AttrOpFamilySet
{
	AttrOpFamily	elems[DEFAULT_ELEMS_NUM];
	struct AttrOpFamilySet	*next;
}AttrOpFamilySet;


typedef struct AttrsDetailsCache
{
	AttrNumber	min_attr;
	AttrNumber	max_attr;
	int			len_set;	/* The number of current AttrOpFamilySet elements used. */
	AttrOpFamily	**atop_array;
	AttrOpFamilySet	*atop_set;
}AttrsDetailsCache;

typedef struct OpcInfoElement
{
	Oid		opcid;
	Oid		opcfamily;
	Oid		opcintype;
}OpcInfoElement;

typedef struct SortedOpclassInfo
{
	int		n_members;
	OpcInfoElement	**opcies;
}SortedOpclassInfo;

typedef struct SelfExamContext
{
	SortedOpclassInfo	sopcinfo;
	AttrsDetailsCache	attrsdc;
	ClauseContext		*ccfree;
	VarattInfo			*vifree;
}SelfExamContext;

#define EXAMINE_ABORT(x) \
do{ \
		ret = x; \
		goto finished; \
}while(0)

extern void check_mergejoinable(RestrictInfo *restrictinfo);
extern bool contain_volatile_functions(Node *clause);
void examine_self_contradictory_rels_phase1(PlannerInfo *root);
void examine_self_contradictory_rels_phase2(PlannerInfo *root);

static inline ClauseContext* get_clause_cxt_internal(SelfExamContext *sec)
{
	ClauseContext	*ccxt;
	if (sec->ccfree)
	{
		ccxt = sec->ccfree;
		sec->ccfree = (ClauseContext*) ccxt->cxt;
		ccxt->cxt = NULL;
		ccxt->origin = NULL;
	}
	else
		ccxt = palloc0(sizeof(ClauseContext));
	
	return ccxt;
}

static inline void free_clause_cxt_internal(SelfExamContext *sec, ClauseContext *cc)
{
	if (sec->ccfree)
	{
		cc->cxt = sec->ccfree;
		sec->ccfree = cc;
	}
	else
	{
		cc->cxt = NULL;
		sec->ccfree = cc;
	}
}

static inline VarattInfo* get_attinfo_cxt_internal(SelfExamContext *sec)
{
	VarattInfo	*varatt;
	if (sec->vifree)
	{
		varatt = sec->vifree;
		sec->vifree = varatt->next;
		varatt->next = NULL;
	}
	else
		varatt = palloc0(sizeof(VarattInfo));
	
	return varatt;
}

static inline void free_attinfo_cxt_internal(SelfExamContext *sec, VarattInfo *atinfo)
{
	if (sec->vifree)
	{
		atinfo->next = sec->vifree;
		sec->vifree = atinfo;
	}
	else
	{
		atinfo->next = NULL;
		sec->vifree = atinfo;
	}
}


static inline AttrOpFamily* get_atop_element_internal(AttrsDetailsCache	*attrsdc)
{
	AttrOpFamilySet	*atops;
	if (attrsdc->len_set == DEFAULT_ELEMS_NUM)
	{
		atops = palloc0(sizeof(AttrOpFamilySet));
		atops->next = attrsdc->atop_set;
		attrsdc->atop_set = atops;
		attrsdc->len_set = 0;
	}
	else
		attrsdc->atop_set->elems[attrsdc->len_set].negator = InvalidOid;
	return attrsdc->atop_set->elems + attrsdc->len_set++;
}

static int compare_scalararray_elements(const void *a, const void *b, void *arg)
{
	Datum		da = *((const Datum *) a);
	Datum		db = *((const Datum *) b);
	SortArrayContext *cxt = (SortArrayContext *) arg;
	int32		compare;

	compare = DatumGetInt32(FunctionCall2Coll(&cxt->sortproc,
											  cxt->collation,
											  da, db));
	if (cxt->reverse)
		INVERT_COMPARE_RESULT(compare);
	return compare;
}

static int sort_scalararray_elements(SortArrayContext *cxt,
													Oid intype,
													Oid opfamily,
													Oid collid,
													bool reverse,
													Datum *elems,
													int nelems)
{
	RegProcedure cmp_proc;

	if (nelems <= 1)
		return nelems;			/* No work to do */
	/*
	 * Look up the appropriate comparison function in the opfamily.
	 *
	 * Note: it's possible that this would fail, if the opfamily is
	 * incomplete, but it seems quite unlikely that an opfamily would omit
	 * non-cross-type support functions for any datatype that it supports at
	 * all.
	 */
	cmp_proc = get_opfamily_proc(opfamily,
								 intype,
								 intype,
								 BTORDER_PROC);
	if (!RegProcedureIsValid(cmp_proc))
		return 0;		/* No support function, give up, do nothing. */

	/* Sort the array elements */
	fmgr_info(cmp_proc, &cxt->sortproc);
	cxt->collation = collid;
	cxt->reverse = reverse;
	qsort_arg((void *) elems, nelems, sizeof(Datum),
			  compare_scalararray_elements, (void *) cxt);

	/* Now scan the sorted elements and remove duplicates */
	return qunique_arg(elems, nelems, sizeof(Datum),
					   compare_scalararray_elements, cxt);
}

static void extract_opc_info(int nums, Oid opcid, Oid opcfamily, Oid opcintype, void *infocxt)
{
	int 	i;
	OpcInfoElement	*opcele;
	SortedOpclassInfo	*sopcinfo = (SortedOpclassInfo*) infocxt;
	if (sopcinfo->n_members == 0)
	{
		sopcinfo->opcies = palloc0(nums * (sizeof(OpcInfoElement*) + sizeof(OpcInfoElement)));
		opcele = (OpcInfoElement*) (sopcinfo->opcies + nums);
		for (i = 0; i < nums; i++)
			sopcinfo->opcies[i] = opcele + i;
	}

	sopcinfo->opcies[sopcinfo->n_members]->opcfamily = opcfamily;
	sopcinfo->opcies[sopcinfo->n_members]->opcid = opcid;
	sopcinfo->opcies[sopcinfo->n_members]->opcintype = opcintype;
	sopcinfo->n_members++;
	Assert(sopcinfo->n_members <= nums);
}

static int opc_element_cmp(const void *p1, const void *p2)
{
	Oid 		v1 = (*((const OpcInfoElement **) p1))->opcintype;
	Oid 		v2 = (*((const OpcInfoElement **) p2))->opcintype;

	return pg_cmp_u32(v1, v2);
}

static Oid binary_search_opc_info(SortedOpclassInfo *sopi, Oid inputtype)
{
	Assert(sopi != NULL);
	if (sopi->n_members > 0)
	{	
		int		i, l, r, res;
		l = 0;
		r = sopi->n_members - 1;
		while (l <= r)
		{
			i = (l + r) / 2;
			res = pg_cmp_u32(sopi->opcies[i]->opcintype, inputtype);
			if (res == 0)
			{
				return sopi->opcies[i]->opcfamily;
			}
			else if (res < 0)
				l = i + 1;
			else
				r = i - 1;
		}
	}
	return InvalidOid;
}

static int ne_const_arg_cmp(const void *a, const void *b, void *arg)
{
	Datum		da = *(((const NeConstDat *) a)->val);
	Datum		db = *(((const NeConstDat *) b)->val);
	SortNeConstCxt *cncc = (SortNeConstCxt *) arg;
	int32		compare;

	compare = DatumGetInt32(FunctionCall2Coll(&cncc->cmp_proc,
											  cncc->collation,
											  da, db));
	return compare;
}

static inline void init_ne_const(NeConstVal *nv)
{
	if (nv->capacity == 0)
    {	
		nv->ncds = palloc0(DEFAULT_ELEMS_NUM * sizeof(NeConstDat));
    	nv->size = 0;
    	nv->capacity = DEFAULT_ELEMS_NUM;
	}
}

static inline void insert_ne_const(NeConstVal	*nv, uint32 idx, Datum *dat)
{
    if (nv->size == nv->capacity) {
        nv->capacity *= 2;
    	nv->ncds = repalloc(nv->ncds, nv->capacity * sizeof(NeConstDat));
    }
	nv->ncds[nv->size].idx = idx;
    nv->ncds[nv->size].val = dat;
    nv->size++;
	nv->sorted = false;
}

/* binary_search_ne_const
 *     Note: caller need to check number of elements in NeConstVal.
 *
 */
static bool binary_search_ne_const(NeConstVal *nv, Datum *dat)
{
	int		i, l, r, res;
	NeConstDat	ncd;
	ncd.val = dat;
	l = 0;
	r = nv->size - 1;
	while (l <= r)
	{
		i = (l + r) / 2;
		res = ne_const_arg_cmp(&nv->ncds[i], &ncd, (void *) &nv->sncc);
		if (res == 0)
		{
			return true;
		}
		else if (res < 0)
			l = i + 1;
		else
			r = i - 1;
	}
	return false;
}


static bool search_ne_const(NeConstVal	*nv, Oid collid, Datum *val)
{
	int		i;
	bool	result;

	if (nv->upper_ncv)
	{
		if (search_ne_const(nv->upper_ncv, collid, val))
			return true;
	}

	if (nv->size == 0)
		return false;

	/* If there is no support function or size is small less than 4, use
	 * linear searching.
	 */
	if (!nv->can_sort || nv->size <= DEFAULT_ELEMS_NUM)
	{
		for (i = 0; i < nv->size;i++)
		{
			result = DatumGetBool(FunctionCall2Coll(&nv->op_func,
											collid,
											 *val,
											*nv->ncds[i].val));
			if (!result)
				return true;
		}
		return false;
	}

	if (nv->can_sort && !nv->sorted)
	{
		qsort_arg((void *) nv->ncds, nv->size, sizeof(NeConstDat),
						ne_const_arg_cmp, (void *) &nv->sncc);
		nv->sorted = true;
	}

	if (binary_search_ne_const(nv, val))
		return true;
	return false;
}

static VarattInfo* get_varatt_info_internal(SelfExamContext *sec, ExtractedAttris *ea, Var *var)
{
	VarattInfo 		*attinfo, *attupper;
	ConstVal		*constv;
	ExtractedAttris *up_ea;
	NeConstVal		**up_ncv;
	int		idx, i;
	idx = var->varattno - ea->min_attr;
	if (ea->array_vi[idx] == NULL)
	{
		attinfo = get_attinfo_cxt_internal(sec);
		if (ea->att_lst)
		{
			attinfo->next = ea->att_lst;
			ea->att_lst = attinfo;
		}
		else
			ea->att_lst = ea->last = attinfo;
		ea->array_vi[idx] = attinfo;
		
		attinfo->stratcount = 0;
		attinfo->null_test = 0;
		attinfo->bool_test = 0;
		attinfo->atidx = idx;
		attinfo->collid = var->varcollid;
		attinfo->type = var->vartype;
		attinfo->ne_const.size = 0;
		attinfo->ne_const.upper_ncv = NULL;
		attinfo->saop.elemnents = NIL;
		attinfo->saop.upper_elems = NIL;
		for(i = 0; i < BTMaxStrategyNumber; i++)
			attinfo->st_table[i].strategy = InvalidStrategy;

		/* Copy push-down attribute particulars. */
		up_ea = ea->upper_ea;
		up_ncv = &attinfo->ne_const.upper_ncv;
		while(up_ea)
		{
			if (up_ea->array_vi[idx])
			{
				attupper = up_ea->array_vi[idx];

				if (attinfo->null_test == 0 && attupper->null_test != 0)
					attinfo->null_test = attupper->null_test;
				if (attinfo->bool_test == 0 && attupper->bool_test != 0)
					attinfo->bool_test = attupper->bool_test;
				if (!*up_ncv && attupper->ne_const.size != 0)
				{
					*up_ncv = &attupper->ne_const;
					up_ncv = &(*up_ncv)->upper_ncv;
				}
				if (attinfo->stratcount == 0 && attupper->stratcount != 0)
				{
					attinfo->stratcount = attupper->stratcount;
					for(i = 0; i < BTMaxStrategyNumber; i++)
					{
						constv = &attupper->st_table[i];
						if (constv->strategy != InvalidStrategy)
						{
							memcpy(&attinfo->st_table[i], constv, sizeof(ConstVal));
							attinfo->st_table[i].push_down = true;
						}
					}
				}
				if (attinfo->saop.upper_elems == NIL && attupper->saop.elemnents != NIL)
					attinfo->saop.upper_elems = attupper->saop.elemnents;
			}
			up_ea = up_ea->upper_ea;
		}
	}
	else
		attinfo = ea->array_vi[idx];
	return attinfo;
}

static bool deconstruct_nulltest_attr(Node *ntnode,
												NullTestType ntt,
												SelfExamContext *sec,
												ExtractedAttris *ea)
{
	if (IsA(ntnode, Var))
	{
		VarattInfo		*att_elem;
		att_elem = get_varatt_info_internal(sec, ea, (Var *) ntnode);

		if (ntt == IS_NULL)
			att_elem->null_test = att_elem->null_test | NT_MARK | NT_IS_NULL;
		else
			att_elem->null_test = att_elem->null_test | NT_MARK | NT_IS_NOT_NULL;
		if (att_elem->null_test == (NT_MARK | NT_IS_NULL | NT_IS_NOT_NULL))
		{
			return false;
		}
	}
	else if (IsA(ntnode, ArrayExpr))
	{
		ListCell   *l;
		foreach(l, ((ArrayExpr *) ntnode)->elements)
		{
			if(!deconstruct_nulltest_attr((Node *) lfirst(l), ntt, sec, ea))
				return false;
		}
	}

	return true;
}

static bool add_binary_op_clause_info(VarattInfo *att_elem,
										Datum *vals,
										int nums,
										uint32 idx,
										uint16 strat,
										RegProcedure opfuncid,
										Expr *expr,
										SelfExamContext *sec,
										ClauseCxtWapper *ccw)
{
	ConstVal		*constval;
	ClauseContext	*ccxt;
	AttrOpFamily	*atop;
	Oid		cmp_proc;
	bool 	result;

	if (strat == STRATEGY_NOT_EQUAL)
	{
		if (att_elem->st_table[BTEqualStrategyNumber-1].strategy != InvalidStrategy)
		{
			constval = &(att_elem->st_table[BTEqualStrategyNumber-1]);
			result = DatumGetBool(FunctionCall2Coll(&constval->op_func,
											att_elem->collid,
											 *vals,
											*(constval->value)));
			if (result)
				return false;
		}

		if (att_elem->ne_const.size == 0)
		{
			init_ne_const(&att_elem->ne_const);
			att_elem->ne_const.can_sort = false;
			att_elem->ne_const.sorted = false;
			fmgr_info(opfuncid, &(att_elem->ne_const.op_func));
			atop = sec->attrsdc.atop_array[att_elem->atidx];
			cmp_proc = get_opfamily_proc(atop->opfamily,
								 att_elem->type,
								 att_elem->type,
								 BTORDER_PROC);
			if (!RegProcedureIsValid(cmp_proc))
			{
				/* No support function, use linear searching. */
				att_elem->ne_const.can_sort = false;
			}
			else
			{
				att_elem->ne_const.sncc.reverse = false;
				fmgr_info(cmp_proc, &att_elem->ne_const.sncc.cmp_proc);
				att_elem->ne_const.sncc.collation = att_elem->collid;
				att_elem->ne_const.can_sort = true;
			}
		}
		insert_ne_const(&att_elem->ne_const, idx, vals);
	}
	else if (strat <= BTMaxStrategyNumber)
	{
		Assert(strat >= BTLessStrategyNumber);
		constval = &(att_elem->st_table[strat-1]);
		if (constval->strategy == InvalidStrategy)
		{
			att_elem->stratcount++;
			constval->strategy = strat;
			fmgr_info(opfuncid, &(constval->op_func));
			constval->value = vals;
			constval->expr = expr;
			constval->push_down = false;
			constval->idx = idx;
		}
		else
		{
			result = DatumGetBool(FunctionCall2Coll(&constval->op_func,
										att_elem->collid,
										*vals,
										*(constval->value)));
			/* Repeat operator = but the consts ain't equal ( eg. x=2 and x=3 ). */
			if (strat == BTEqualStrategyNumber)
			{
				if (!result)
					return false;
				if (constval->push_down)
				{
					constval->value = vals;
					constval->expr = expr;
					constval->push_down = false;
					constval->idx = idx;
				}
				else if (IsA(expr, OpExpr))
				{
					ccxt = get_clause_cxt_internal(sec);
					ccxt->cct = CCT_DEL;
					ccxt->idx = idx;
					ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
				}
			}
			else if (result)
			{
				/* Current Expr is more restrict, so keep it. */
				if (!constval->push_down && IsA(constval->expr, OpExpr))
				{
					ccxt = get_clause_cxt_internal(sec);
					ccxt->cct = CCT_DEL;
					ccxt->idx = constval->idx;
					ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
				}
				constval->value = vals;
				constval->expr = expr;
				constval->push_down = false;
				constval->idx = idx;
			}
			else
			{
				/* Current Expr is less restrict, so abandon it. */
				if (constval->push_down)
				{
					ccxt = get_clause_cxt_internal(sec);
					ccxt->cct = CCT_REPLACE;
					ccxt->idx = idx;
					ccxt->cxt = copyObjectImpl(constval->expr);
					ccw->clausecxt = lappend(ccw->clausecxt, ccxt);

					constval->push_down = false;
					constval->expr = ccxt->cxt;
					constval->idx = idx;
				}
				else if (IsA(expr, OpExpr))
				{
					ccxt = get_clause_cxt_internal(sec);
					ccxt->cct = CCT_DEL;
					ccxt->idx = idx;
					ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
				}
			}
		}
	}
	return true;
}

static bool self_contradictory_element(SelfExamContext *sec,
													ClauseCxtWapper *ccw,
													VarattInfo *att_elem)
{
	ConstVal		*leftcv, *rightcv;
	ClauseContext	*ccxt;
	int			i;
	uint8		btest;
	bool		result;
	bool		macthed;

	if (att_elem->bool_test & BT_MARK)
	{
		btest = att_elem->bool_test & ~BT_MARK;
		if (((btest & BT_IS_TRUE) && (btest & (BT_IS_NOT_TRUE | BT_IS_FALSE | BT_IS_UNKNOWN)))
			|| ((btest & BT_IS_FALSE) && (btest &(BT_IS_TRUE | BT_IS_NOT_FALSE | BT_IS_UNKNOWN)))
			|| ((btest & BT_IS_UNKNOWN) && (btest &(BT_IS_NOT_UNKNOWN | BT_IS_TRUE | BT_IS_FALSE))))
		{
			return true;
		}
	}
	
	if (att_elem->null_test == (NT_MARK | NT_IS_NULL)
			&& (att_elem->stratcount > 0
				|| att_elem->ne_const.size != 0
				|| att_elem->ne_const.upper_ncv))
		return true;

	if ((att_elem->bool_test & BT_MARK) && (att_elem->null_test & NT_MARK))
	{
		if (att_elem->null_test == (NT_MARK | NT_IS_NULL)
				&& (btest & (BT_IS_TRUE | BT_IS_FALSE | BT_IS_NOT_UNKNOWN)))
			return true;
		else if (att_elem->null_test == (NT_MARK | NT_IS_NOT_NULL)
					&& (btest & BT_IS_UNKNOWN))
			return true;
	}
	
	/* If operator '=' and '<>' are both appeared. */
	if (att_elem->st_table[BTEqualStrategyNumber-1].strategy != 0
			&& (att_elem->ne_const.size != 0 || att_elem->ne_const.upper_ncv))
	{
		leftcv = &att_elem->st_table[BTEqualStrategyNumber-1];

		if (search_ne_const(&att_elem->ne_const, att_elem->collid, leftcv->value))
			return true;
		/* If they are not contradictory, eliminate the not equalities. */
		for (i = 0; i < att_elem->ne_const.size; i++)
		{
			ccxt = get_clause_cxt_internal(sec);
			ccxt->cct = CCT_DEL;
			ccxt->idx = att_elem->ne_const.ncds[i].idx;
			ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
		}
		att_elem->ne_const.size = 0;
	}

	/* Try to keep only one of <, <= */
	if (att_elem->st_table[BTLessStrategyNumber - 1].strategy != 0
			&& att_elem->st_table[BTLessEqualStrategyNumber - 1].strategy != 0)
	{
		leftcv = &att_elem->st_table[BTLessStrategyNumber - 1];
		rightcv = &att_elem->st_table[BTLessEqualStrategyNumber - 1];
		result = DatumGetBool(FunctionCall2Coll(&(rightcv->op_func),
												att_elem->collid,
												*(leftcv->value),
												*(rightcv->value)));
		if (result)
		{
			rightcv->strategy = InvalidStrategy;
			if (leftcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_REPLACE;
				ccxt->idx = rightcv->idx;
				ccxt->cxt = copyObjectImpl(leftcv->expr);
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);

				leftcv->push_down = false;
				leftcv->expr = ccxt->cxt;
				leftcv->idx = rightcv->idx;
			}
			else if (!rightcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_DEL;
				ccxt->idx = rightcv->idx;
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
			}
		}
		else
		{
			leftcv->strategy = InvalidStrategy;
			if (rightcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_REPLACE;
				ccxt->idx = leftcv->idx;
				ccxt->cxt = copyObjectImpl(rightcv->expr);
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);

				rightcv->push_down = false;
				rightcv->expr = ccxt->cxt;
				rightcv->idx = leftcv->idx;
			}
			else if (!leftcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_DEL;
				ccxt->idx = leftcv->idx;
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
			}
		}
		att_elem->stratcount--;
	}

	/* try to keep only one of >, >= */
	if (att_elem->st_table[BTGreaterStrategyNumber - 1].strategy != 0
			&& att_elem->st_table[BTGreaterEqualStrategyNumber - 1].strategy != 0)
	{
		leftcv = &att_elem->st_table[BTGreaterStrategyNumber - 1];
		rightcv = &att_elem->st_table[BTGreaterEqualStrategyNumber - 1];
		result = DatumGetBool(FunctionCall2Coll(&(rightcv->op_func),
												att_elem->collid,
												*(leftcv->value),
												*(rightcv->value)));
		if (result)
		{
			rightcv->strategy = InvalidStrategy;
			if (leftcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_REPLACE;
				ccxt->idx = rightcv->idx;
				ccxt->cxt = copyObjectImpl(leftcv->expr);
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);

				leftcv->push_down = false;
				leftcv->expr = ccxt->cxt;
				leftcv->idx = rightcv->idx;
			}
			else if (!rightcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_DEL;
				ccxt->idx = rightcv->idx;
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
			}
		}
		else
		{
			leftcv->strategy = InvalidStrategy;
			if (rightcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_REPLACE;
				ccxt->idx = leftcv->idx;
				ccxt->cxt = copyObjectImpl(rightcv->expr);
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);

				rightcv->push_down = false;
				rightcv->expr = ccxt->cxt;
				rightcv->idx = leftcv->idx;
			}
			else if (!leftcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_DEL;
				ccxt->idx = leftcv->idx;
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
			}
		}
		att_elem->stratcount--;
	}

	if (att_elem->type == BOOLOID && att_elem->stratcount > 0)
	{
		/*
		 * Beyond the scope of the defined values of boolean type,
		 * obviously it does not make sense.
		 */
		if (att_elem->st_table[BTGreaterStrategyNumber - 1].strategy != 0)
		{
			leftcv = &att_elem->st_table[BTGreaterStrategyNumber - 1];
			result = DatumGetBool(FunctionCall2Coll(&leftcv->op_func,
									att_elem->collid,
									BoolGetDatum(true),
									*leftcv->value));
			if (!result)
				return true;
		}
		if (att_elem->st_table[BTLessStrategyNumber - 1].strategy != 0)
		{
			leftcv = &att_elem->st_table[BTLessStrategyNumber - 1];
			result = DatumGetBool(FunctionCall2Coll(&leftcv->op_func,
									att_elem->collid,
									BoolGetDatum(false),
									*leftcv->value));
			if (!result)
				return true;
		}

		if (att_elem->bool_test & BT_MARK)
		{
			btest = att_elem->bool_test & ~BT_MARK;
			if (btest & BT_IS_UNKNOWN)
				return true;
			else
			{
				bool isvalue;
				macthed = true;
				/* If IS_NOT_FALSE is set, that mean the equivalent expression is
				 *  (x IS NULL OR x IS TRUE). The NULL test will always fail with 
				 * the strategy operator, so we only need to test whether the 
				 * sub-expression (x IS TRUE) is contradictory to the strategy 
				 * operator. If IS_NOT_TRUE is set, it's similar to IS_NOT_FALSE.
				 */
				if (btest & (BT_IS_TRUE| BT_IS_NOT_FALSE))
					isvalue = true;
				else if (btest & (BT_IS_FALSE| BT_IS_NOT_TRUE))
					isvalue = false;
				else
					macthed = false;
				if (macthed)
				{
					for (i = BTMaxStrategyNumber; --i >= 0;)
					{
						leftcv = &att_elem->st_table[i];
						if (leftcv->strategy == InvalidStrategy)
							continue;
						result = DatumGetBool(FunctionCall2Coll(&leftcv->op_func,
												att_elem->collid,
												BoolGetDatum(isvalue),
												*leftcv->value));
						if (!result)
							return true;
					}
				}
			}
		}
	}
	
	if (att_elem->stratcount <=1)
		return false;
	
	if (att_elem->st_table[BTEqualStrategyNumber-1].strategy != 0)
	{
		leftcv = &att_elem->st_table[BTEqualStrategyNumber - 1];
		for (i = BTMaxStrategyNumber; --i >= 0;)
		{
			rightcv = &att_elem->st_table[i];
			if (rightcv->strategy == InvalidStrategy || i == (BTEqualStrategyNumber - 1))
					continue;
			result = DatumGetBool(FunctionCall2Coll(&(rightcv->op_func),
													att_elem->collid,
													*(leftcv->value),
													*(rightcv->value)));
			if (!result)
				return true;
			/* The element had be dominated by operator =, so abandon it. */
			if (leftcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_REPLACE;
				ccxt->idx = rightcv->idx;
				ccxt->cxt = copyObjectImpl(leftcv->expr);
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);

				leftcv->push_down = false;
				leftcv->expr = ccxt->cxt;
				leftcv->idx = rightcv->idx;
			}
			else if (!rightcv->push_down)
			{
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_DEL;
				ccxt->idx = rightcv->idx;
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
			}
			rightcv->strategy = InvalidStrategy;
		}
		/* The operator = has dominated others, so no need to check other case. */
		return false;
	}

	/* Detect the contradictory case that like x <4 and x >10. */
	macthed = true;
	if (att_elem->st_table[BTGreaterStrategyNumber - 1].strategy != 0)
	{
		rightcv = &att_elem->st_table[BTGreaterStrategyNumber - 1];
		if (att_elem->st_table[BTLessStrategyNumber - 1].strategy != 0)
			leftcv = &att_elem->st_table[BTLessStrategyNumber - 1];
		else if (att_elem->st_table[BTLessEqualStrategyNumber - 1].strategy != 0)
			leftcv = &att_elem->st_table[BTLessEqualStrategyNumber - 1];
		else
			macthed = false;
	}
	else if (att_elem->st_table[BTLessStrategyNumber - 1].strategy != 0)
	{
		rightcv = &att_elem->st_table[BTLessStrategyNumber - 1];
		if (att_elem->st_table[BTGreaterStrategyNumber - 1].strategy != 0)
			leftcv = &att_elem->st_table[BTGreaterStrategyNumber - 1];
		else if (att_elem->st_table[BTGreaterEqualStrategyNumber - 1].strategy != 0)
			leftcv = &att_elem->st_table[BTGreaterEqualStrategyNumber - 1];
		else
			macthed = false;
	}
	else if (att_elem->st_table[BTLessEqualStrategyNumber - 1].strategy != 0
		&& att_elem->st_table[BTGreaterEqualStrategyNumber - 1].strategy != 0)
	{
		rightcv = &att_elem->st_table[BTGreaterEqualStrategyNumber - 1];
		leftcv = &att_elem->st_table[BTLessEqualStrategyNumber - 1];
	}
	else
		macthed = false;
	if (macthed)
	{
		result = DatumGetBool(FunctionCall2Coll(&(rightcv->op_func),
													att_elem->collid,
													*(leftcv->value),
													*(rightcv->value)));
		if (!result)
			return true;
	}

	return false;
}

static bool check_eq_sa_op_elemnet(Datum *value, VarattInfo *att_elem)
{
	ConstVal	*constval;
	int			i;
	bool		result;
	uint8		btest;
	bool		isvalue, macthed;

	if (att_elem->null_test == (NT_MARK | NT_IS_NULL))
		return true;

	if (att_elem->bool_test & BT_MARK)
	{
		btest = att_elem->bool_test & ~BT_MARK;

		if (btest & BT_IS_UNKNOWN)
			return true;

		macthed = true;
		if (btest & (BT_IS_TRUE| BT_IS_NOT_FALSE))
			isvalue = true;
		else if (btest & (BT_IS_FALSE| BT_IS_NOT_TRUE))
			isvalue = false;
		else
			macthed = false;

		if (macthed && DatumGetBool(*value) != isvalue)
			return true;
	}

	if (search_ne_const(&att_elem->ne_const, att_elem->collid, value))
		return true;
	if (att_elem->stratcount > 0)
	{
		for (i = BTMaxStrategyNumber; --i >= 0;)
		{
			constval = &att_elem->st_table[i];
			if (constval->strategy == InvalidStrategy)
				continue;
			result = DatumGetBool(FunctionCall2Coll(&(constval->op_func),
									att_elem->collid,
									*(value),
									*(constval->value)));
			if (!result)
				return true;
		}
	}
	return false;
}

/*
 * self_contradictory_saop
 *    The Scalar array expr of any-equality expression are postponed here
 *    to examine them (eg. x in (1,3) and x in (3,5) and x = 1 ).
 *    Extract intersection elements and report to the caller if there is
 *    no intersection element or check intersection elements one by one
 *    and eliminate it if it's self-contradictory. All of them are
 *    self-contradictory report it.
 *    Rebuilt the saop finally.
 */
static bool self_contradictory_saop(SelfExamContext *sec,
												ClauseCxtWapper *ccw,
												VarattInfo *att_elem)
{
	/* Scalar array expr of single attribute. */
	ScalarArrayOpExpr	*saopexpr;
	SAEqualDatElem	*saout;
	ClauseContext	*ccxt;
	ListCell	*lc;
	Expr		*newclause;
	Const		*con;
	int			i, j, initial_nums;

	if (att_elem->saop.elemnents == NIL)
		return false;

	if (att_elem->saop.upper_elems != NIL)
		att_elem->saop.elemnents = list_concat(att_elem->saop.elemnents, att_elem->saop.upper_elems);

	saout = linitial(att_elem->saop.elemnents);
	initial_nums = saout->elem_nums;
	if (list_length(att_elem->saop.elemnents) > 1)
	{
		/* Initialize saout with the first one, merge the rest of elemnents
		 * one by one and extract the intersection element at the same time.
		 * Update the saout with merged result.
		 */
		for_each_from(lc, att_elem->saop.elemnents, 1)
		{
			SAEqualDatElem	*sade;
			int				nelems_dups;
			
			sade =lfirst(lc);
			nelems_dups = 0;
			for (i = 0, j = 0; i < saout->elem_nums && j < sade->elem_nums;)
			{
				Datum		*oelem = saout->elem_values + i,
				   			*nelem = sade->elem_values + j;
				int			res = compare_scalararray_elements(oelem, nelem, &att_elem->saop.sortcxt);
				if (res == 0)
				{
					saout->elem_values[nelems_dups++] = *oelem;
					i++;
					j++;
				}
				else if (res < 0)
					i++;
				else					/* res > 0 */
					j++;
			}
			if (nelems_dups == 0)
			{
				att_elem->saop.elemnents = NIL;
				return true; /* No intersection element, so report it. */
			}
			else
				saout->elem_nums = nelems_dups;
		}
	}

	/* Examine the rest of intersection elements one by one. */
	for(i = 0; i < saout->elem_nums;)
	{
		if (check_eq_sa_op_elemnet(&saout->elem_values[i], att_elem))
		{
			if (i < saout->elem_nums - 1)
				memmove(saout->elem_values + i, saout->elem_values + i + 1,
							sizeof(Datum) * (saout->elem_nums - i - 1));
			saout->elem_nums--;
		}
		else
			i++;
	}
	if (0 == saout->elem_nums)
	{
		att_elem->saop.elemnents = NIL;
		return true;
	}

	/*
	 * Convert the intersection elements to a new clause and delete orignal
 	 * elements.
	 */
	saopexpr = (ScalarArrayOpExpr*)saout->expr;
	if (saout->elem_nums == 1)
	{
		con = makeConst(att_elem->saop.sortcxt.elmtype,
						-1,			/* typmod -1 is OK for all cases */
						InvalidOid,
						att_elem->saop.sortcxt.elmlen,
						saout->elem_values[0],
						false,
						att_elem->saop.sortcxt.elmbyval);
		con->location = -1;
		newclause = make_opclause(saopexpr->opno,
									BOOLOID,
									false,
									linitial(saopexpr->args),
									(Expr*)con,
									InvalidOid,
									saopexpr->inputcollid);
		ccxt = get_clause_cxt_internal(sec);
		ccxt->cct = CCT_REPLACE;
		ccxt->idx = saout->idx;
		ccxt->cxt = newclause;
		ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
		add_binary_op_clause_info(att_elem,
									&saout->elem_values[0],
									1,
									saout->idx,
									BTEqualStrategyNumber,
									att_elem->saop.opfuncid,
									newclause,
									sec,
									ccw);
	}
	else if (saout->elem_nums != initial_nums)
	{
		/* It's need to rebuilt the first saop if there are multi-saop,
		 * and delete others saop later.
		 */
		ArrayType *arrval = construct_array(saout->elem_values,
									saout->elem_nums,
									att_elem->saop.sortcxt.elmtype,
									att_elem->saop.sortcxt.elmlen,
									att_elem->saop.sortcxt.elmbyval,
									att_elem->saop.sortcxt.elmalign);
		con = lsecond(saopexpr->args);
		con->constvalue = PointerGetDatum(arrval);

		ccxt = get_clause_cxt_internal(sec);
		ccxt->cct = CCT_REPLACE;
		ccxt->idx = saout->idx;
		ccxt->cxt = saopexpr;
		ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
		
	}
	
	/* The push-down elements have finished their jobs, so truncate them. */
	if (att_elem->saop.upper_elems != NIL)
	{	att_elem->saop.elemnents = list_truncate(att_elem->saop.elemnents, \
				list_length(att_elem->saop.elemnents) - list_length(att_elem->saop.upper_elems));
	}
	
	/* Since the whole elements except first one have be merged into first one,
	 * it's time to delete them from the orignal clause list.
	 */
	for_each_from(lc, att_elem->saop.elemnents, 1)
	{
		saout =lfirst(lc);
		ccxt = get_clause_cxt_internal(sec);
		ccxt->cct = CCT_DEL;
		ccxt->idx = saout->idx;
		ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
	}
	/* It's need to truncate the list, it maybe push down to
	 * compare with lower level ScalarArrayOpExpr, since the
	 * whole elements except first one have already merged.
	 */
	att_elem->saop.elemnents = list_truncate(att_elem->saop.elemnents, 1);
	return false;
}

static bool self_contradictory_check(SelfExamContext *sec, ExtractedAttris *ea, ClauseCxtWapper *ccw)
{
	VarattInfo	*attinfo;
	if (!ea->addOp)
		return false;
	attinfo = ea->att_lst;
	while(attinfo)
	{
		if (self_contradictory_element(sec, ccw, attinfo))
			return true;
		
		if (self_contradictory_saop(sec, ccw, attinfo))
			return true;
		attinfo = attinfo->next;
	}
	return false;
}

/*
 * extract_binary_op_clause_info
 *    Extract binary operation clause information
 *
 *    In practice,  B-tree index is used widely; Binary operation
 *    expr that Var op Const is often written; Also without
 *    data type coercion and collation. So this routine support
 *    above features.
 *    There are several consideration 1) self-contradictory
 *    rel seldom appeared. 2) Invoker will get upset if spend
 *    too much effort on it and find that rel is not
 *    self-contradictory (eg. strict function sqrt, expr like
 *    sqrt(x) > 2 and sqrt(x) < 4 ). 3) Finally the expression
 *    evaluation mechanism will do the right things. So this is
 *    a compromise.
 */
static bool extract_binary_op_clause_info(void *expr,
											Node *lop,
											Node *rop,
											uint32 idx,
											Oid opno,
											Oid collid,
											SelfExamContext *sec,
											ExtractedAttris *ea,
											ClauseCxtWapper *ccw)
{
	VarattInfo		*attinfo;
	Node	   		*leftop, *rightop;
	AttrOpFamily	*atop;
	Expr			*newclause;
	Oid				left_type, right_type;
	Oid 			varcollid;
	Oid 			opid, opfamily;
	RegProcedure 	opfuncid;
	StrategyNumber	strat;
	uint16 			attidx;

	if (!(IsA(lop, Const) || IsA(rop, Const)))
		return true;

	if (IsA(lop, Const))
	{
		opid = get_commutator(opno);
		if (!OidIsValid(opid))
			return true;
		leftop = rop;
		rightop = lop;
	}
	else
	{
		opid = opno;
		leftop = lop;
		rightop = rop;
	}
	opfuncid = get_opcode(opid);
	Assert(OidIsValid(opfuncid));

	if (((Const *) rightop)->constisnull)
		return true;

	left_type = InvalidOid;
	right_type = ((Const *) rightop)->consttype;

	if (IsA(expr, ScalarArrayOpExpr))
	{
		Oid 	typInput, typelem;
		getTypeInputInfo(right_type, &typInput, &typelem);
		right_type = typelem;
	}

	if (leftop && IsA(leftop, RelabelType))
	{
		left_type = ((RelabelType *) leftop)->resulttype;
		varcollid = ((RelabelType *) leftop)->resultcollid;
		leftop = (Node*)((RelabelType *) leftop)->arg;
	}

	if (IsA(leftop, Var))
	{
		if (((Var *) leftop)->varattno > sec->attrsdc.max_attr)
			elog(ERROR, "bogus relation attribute");
		attidx = ((Var *) leftop)->varattno - sec->attrsdc.min_attr;
	}
	else	/* Don't spend much effort for other types, so skip it */
		return true;

	if (!OidIsValid(left_type))
	{
		left_type = ((Var *) leftop)->vartype;
		varcollid = ((Var *) leftop)->varcollid;
	}
	
	if (left_type != right_type)
		return true;
	if (collid != varcollid)
		return true;

	/* Search attribute opfamily cache, be happy to use it if find. */
	if (sec->attrsdc.atop_array[attidx])
	{
		atop = sec->attrsdc.atop_array[attidx];
		if (!OidIsValid(atop->opfamily))
			return true;
		opfamily = atop->opfamily;
	}
	else
	{
		atop = get_atop_element_internal(&sec->attrsdc);
		sec->attrsdc.atop_array[attidx] = atop;

		opfamily = binary_search_opc_info(&sec->sopcinfo, left_type);
		if (!OidIsValid(opfamily))
		{
			atop->opfamily = InvalidOid;
			return true;
		}
		atop->opfamily = opfamily;
	}

	if (!OidIsValid(atop->negator) && atop->negator == opid)
		strat = STRATEGY_NOT_EQUAL;
	else
	{
		strat = get_op_opfamily_strategy(opid, opfamily);
		if (strat == 0)
		{
			/* Perhaps it is a <> operator. See if it has a negator that is in an opfamily. */
			Oid		op_negator = get_negator(opid);
			if (!OidIsValid(op_negator))
				return true;
			if (BTEqualStrategyNumber == get_op_opfamily_strategy(op_negator, opfamily))
			{
				atop->negator = opid;
				strat = STRATEGY_NOT_EQUAL;
			}
			else
				return true;
		}
	}
	
	/* If it's scalar array expr, deconstruct the const values. */
	if (unlikely(IsA(expr, ScalarArrayOpExpr)))
	{
		ArrayType  *arrayval;
		int			num_elems;
		Datum	   *elem_values, *one_elem;
		bool	   *elem_nulls;
		int			num_nonnulls;
		int			j;
		SortArrayContext sacontext;
		ClauseContext	*ccxt;
		Const		*con;
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *)expr;
		arrayval = DatumGetArrayTypeP(((Const *) rightop)->constvalue);
		/* We could cache this data, but not clear it's worth it */
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &sacontext.elmlen, &sacontext.elmbyval, &sacontext.elmalign);
		sacontext.elmtype = ARR_ELEMTYPE(arrayval);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  sacontext.elmlen, sacontext.elmbyval, sacontext.elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * Compress out any null elements.	We can ignore them since we assume
		 * all  B-tree operators are strict.
		 */
		num_nonnulls = 0;
		for (j = 0; j < num_elems; j++)
		{
			if (!elem_nulls[j])
				elem_values[num_nonnulls++] = elem_values[j];
		}
		if (num_nonnulls == 0)
			return true;

		/* If NULL is existed and with ALL flag, report it. */
		if (!saop->useOr && num_nonnulls != num_elems)
			return false;

		/*    Sort the non-null elements and eliminate any duplicates. 
         *    If there is no support function will return 0 and then
         *    give up.
		 */
		if ( 0 == (num_elems = sort_scalararray_elements(&sacontext, right_type, opfamily, collid,
									false, elem_values, num_nonnulls)))
			return true;

		attinfo = get_varatt_info_internal(sec, ea, (Var *) leftop);
		if (unlikely(strat == STRATEGY_NOT_EQUAL))
		{
			/* operator <> */
			if (!saop->useOr)
			{
				for (j = 0; j < num_elems; j++)
				{
					if (!add_binary_op_clause_info(attinfo,
													&elem_values[j],
													1,
													idx,
													strat,
													opfuncid,
													expr,
													sec,
													ccw))
						return false;
					ea->addOp = true;
				}
			}
			else
			{
				/* Treat it as and-expr implicitly if there is only one element. */
				if (num_elems != 1)
					return true; /* Don't flatten, forget it. */
				con = makeConst(left_type,
								-1, 		/* typmod -1 is OK for all cases */
								InvalidOid,
								sacontext.elmlen,
								elem_values[0],
								false,
								sacontext.elmbyval);
				con->location = -1;
				newclause = make_opclause(opid,
											BOOLOID,
											false,
											(Expr*)leftop,
											(Expr*)con,
											InvalidOid,
											collid);
				if (!add_binary_op_clause_info(attinfo,
												&elem_values[0],
												1,
												idx,
												strat,
												opfuncid,
												newclause,
												sec,
												ccw))
					return false;
				ea->addOp = true;
				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_REPLACE;
				ccxt->idx = idx;
				ccxt->cxt = newclause;
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
			}
		}
		else
		{
			switch (strat)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					if (!saop->useOr)
						one_elem = &elem_values[0];
					else
						one_elem = &elem_values[num_elems-1];
					break;
				case BTEqualStrategyNumber:
					/* If the number of elements is not one and postpone them,
					 * examine them later, because maybe there are multiple
					 * any-op ScalarArrayOpExpr (eg. x in(1,3) and x in(1,5)),
					 * see self_contradictory_saop comment.
					 */
					if (num_elems != 1)
					{
						if (!saop->useOr)
							return false;	/* It's contradictory obviously. */
						else
						{
							SAEqualDatElem	*saopele;
							if (attinfo->saop.elemnents == NIL)
							{
								attinfo->saop.opfuncid = opfuncid;
								memcpy(&(attinfo->saop.sortcxt), &sacontext, sizeof(SortArrayContext));
							}
							saopele = palloc0(sizeof(SAEqualDatElem));
							saopele->idx = idx;
							saopele->expr = expr;
							saopele->elem_nums = num_elems;
							saopele->elem_values = elem_values;
							attinfo->saop.elemnents = lappend(attinfo->saop.elemnents, saopele);
						}
					}
					else
					{
						Assert(num_elems == 1);
						one_elem = &elem_values[0];
					}
					break;
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
					if (!saop->useOr)
						one_elem = &elem_values[num_elems-1];
					else
						one_elem = &elem_values[0];
					break;
				default:
					elog(ERROR, "unrecognized StrategyNumber: %d",
						 strat);
					break;
			}
			if (!(strat == BTEqualStrategyNumber && num_elems != 1))
			{
				con = makeConst(left_type,
								-1,			/* typmod -1 is OK for all cases */
								InvalidOid,
								sacontext.elmlen,
								*one_elem,
								false,
								sacontext.elmbyval);
				con->location = -1;
				newclause = make_opclause(opid,
											BOOLOID,
											false,
											(Expr*)leftop,
											(Expr*)con,
											InvalidOid,
											collid);
				if (!add_binary_op_clause_info(attinfo,
													one_elem,
													1,
													idx,
													strat,
													opfuncid,
													newclause,
													sec,
													ccw))
					return false;

				ccxt = get_clause_cxt_internal(sec);
				ccxt->cct = CCT_REPLACE;
				ccxt->idx = idx;
				ccxt->cxt = newclause;
				ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
			}
		}
	}
	else
	{
		attinfo = get_varatt_info_internal(sec, ea, (Var *) leftop);
		if (!add_binary_op_clause_info(attinfo,
										&((Const *) rightop)->constvalue,
										1,
										idx,
										strat,
										opfuncid,
										expr,
										sec,
										ccw))
			return false;
	}
	ea->addOp = true;

	return true;
}

/*
 * row_comparsion_to_nonconstructor
 *	  Convert a row comparison to nonconstructor
 *	
 *	  We have checked the expression in analysis phase, so 
 *    there is no need to check whether it can satisfy the 
 *    required of B-tree or not. 
 */
static int row_comparsion_to_nonconstructor(Expr *rowexpr, Expr **transformed)
{
	Oid 		left_type, right_type;
	Oid 		varcollid;	
	ListCell   *l_left_expr,
			   *l_right_expr,
			   *l_opno,
			   *l_opfamily,
			   *l_inputcollid;
	ListCell   *lc;
	List	   *rcelst, *arglst;
	BoolExpr   *orclause, *andclause;
	RowCompElem		*rce;
	Expr	   *newclause;
	RowCompareExpr *rcexpr;
	FmgrInfo	op_func;
	bool		result, lasteva;
	
	*transformed = NULL;
	rcexpr = (RowCompareExpr *) rowexpr;
	Assert(list_length(rcexpr->opnos) != 1);
	Assert(rcexpr->rctype != ROWCOMPARE_EQ && rcexpr->rctype != ROWCOMPARE_NE);

	rcelst = NIL;
	orclause = (BoolExpr*)make_orclause(NIL);
	forfive(l_left_expr, rcexpr->largs,
			l_right_expr, rcexpr->rargs,
			l_opno, rcexpr->opnos,
			l_opfamily, rcexpr->opfamilies,
			l_inputcollid, rcexpr->inputcollids)
	{
		RegProcedure 	opfuncid;
		int			strat;
		Oid			eqid;
		Expr	   *tmp_expr;
		Expr	   *left_expr = (Expr *) lfirst(l_left_expr);
		Expr	   *right_expr = (Expr *) lfirst(l_right_expr);
		Oid			opno = lfirst_oid(l_opno);
		Oid			opfamily = lfirst_oid(l_opfamily);
		Oid			inputcollid = lfirst_oid(l_inputcollid);

		if ((IsA(left_expr, Const)
			&& ((Const*)left_expr)->constisnull)
			|| (IsA(right_expr, Const)
			&& ((Const*)right_expr)->constisnull))
		{
			if (list_cell_number(rcexpr->opnos, l_opno) == 0)
				return CCT_CONSTF;
			lasteva = false;
			break; /* The rest of pairs are dominated by the NULL, ignore them. */
		}
		if (equal(left_expr, right_expr))
		{
			if (!contain_volatile_functions((Node*)left_expr))
			{
				if (!IsA(left_expr, Const))
				{
					NullTest *nulltest = makeNode(NullTest);
					nulltest->arg = left_expr;
					nulltest->nulltesttype = IS_NOT_NULL;
					nulltest->argisrow = false;
					nulltest->location = -1;
					rce = palloc0(sizeof(RowCompElem));
					rce->contact = (Expr*)nulltest;
					rcelst = lappend(rcelst, rce);
				}
				/* IF the last operator is > or <, the last sub-expr must be failed. */
				if (l_opno == list_last_cell(rcexpr->opnos))
				{
					strat = get_op_opfamily_strategy(opno, opfamily);
					if (strat == BTLessStrategyNumber || strat == BTGreaterStrategyNumber)
						lasteva = false;
					else
						lasteva = true;
					if (lasteva)
					{
						arglst = NIL;
						foreach(lc, rcelst)
						{
							arglst = lappend(arglst, ((RowCompElem*)lfirst(lc))->contact);
						}
						goto lab_built;
					}
					else
						break;
				}
				continue;
			}
			else
			{
				/* Forget this rcexpr, cann't do anything more. */
				*transformed = NULL;
				return 0;
			}
		}

		/*   If Const Op Const and are not equal. It's need to evaluate
		 *   the expt is positive or not.
		 */
		if (IsA(left_expr, Const) && IsA(right_expr, Const))
		{
			opfuncid = get_opcode(opno);
			fmgr_info(opfuncid, &op_func);
			result = DatumGetBool(FunctionCall2Coll(&op_func,
										inputcollid,
										 ((Const *) left_expr)->constvalue,
										((Const *) right_expr)->constvalue));
			if (result)
			{
				if (list_cell_number(rcexpr->opnos, l_opno) == 0)
					return CCT_CONSTT;
				else
				{
					arglst = NIL;
					foreach(lc, rcelst)
					{
						arglst = lappend(arglst, ((RowCompElem*)lfirst(lc))->contact);
					}
					if (list_length(arglst) == 1)
					{
						orclause->args = lappend(orclause->args, linitial(arglst));
					}
					else if (list_length(arglst) > 1)
					{
						andclause = (BoolExpr*) make_andclause(arglst);
						orclause->args = lappend(orclause->args, andclause);
					}
					lasteva = true;
					break;
				}
			}
			else
			{
				if (list_cell_number(rcexpr->opnos, l_opno) == 0)
					return CCT_CONSTF;
				else
				{
					lasteva = false;
					break;
				}
			}
		}

		/* Only accept Var op Const, others cause the rcexpr will be ignored. */
		if (!(IsA(left_expr, Const) || IsA(right_expr, Const)))
		{
			*transformed = NULL;
			return 0;
		}

		if (IsA(left_expr, Const))
		{
			opno = get_commutator(opno);
			tmp_expr = left_expr;
			left_expr = right_expr;
			right_expr = tmp_expr;
		}

		tmp_expr = left_expr;
		if (IsA(left_expr, RelabelType))
		{
			left_type = ((RelabelType *) left_expr)->resulttype;
			varcollid = ((RelabelType *) left_expr)->resultcollid;
			left_expr = ((RelabelType *) left_expr)->arg;
		}
		
		if (IsA(left_expr, Var))
		{
			left_type = ((Var *) left_expr)->vartype;
			varcollid = ((Var *) left_expr)->varcollid;
		}
		else
		{
			*transformed = NULL;
			return 0;
		}
		right_type = ((Const *) right_expr)->consttype;
		if ( left_type != right_type
			|| inputcollid != varcollid)	/* Don't spend much effort for other types, so skip it */
	
		{
			*transformed = NULL;
			return 0;
		}
		/* Built sub expr. */
		rce = palloc0(sizeof(RowCompElem));
		rce->opid = opno;
		eqid = get_opfamily_member(opfamily, left_type, left_type, BTEqualStrategyNumber);
		newclause = make_opclause(eqid,
									BOOLOID,
									false,
									tmp_expr,
									right_expr,
									InvalidOid,
									inputcollid);
		rce->contact = newclause;
		strat = get_op_opfamily_strategy(opno, opfamily);
		if (strat == BTLessEqualStrategyNumber)
			rce->stricter_opid = get_opfamily_member(opfamily, left_type, left_type, BTLessStrategyNumber);
		else if (strat == BTGreaterEqualStrategyNumber)
			rce->stricter_opid = get_opfamily_member(opfamily, left_type, left_type, BTGreaterStrategyNumber);
		else
			rce->stricter_opid = opno;

		if (l_opno == list_last_cell(rcexpr->opnos))
			opno = rce->opid;
		else
			opno = rce->stricter_opid;
		newclause = make_opclause(opno,
									BOOLOID,
									false,
									tmp_expr,
									right_expr,
									InvalidOid,
									inputcollid);

		arglst = NIL;
		foreach(lc, rcelst)
		{
			arglst = lappend(arglst, ((RowCompElem*)lfirst(lc))->contact);
		}
		arglst = lappend(arglst, newclause);
		rcelst = lappend(rcelst, rce);

lab_built:
		if (list_length(arglst) == 1)
		{
			orclause->args = lappend(orclause->args, linitial(arglst));
		}
		else if (list_length(arglst) > 1)
		{
			andclause = (BoolExpr*)make_andclause(NIL);
			andclause->args = arglst;
			orclause->args = lappend(orclause->args, andclause);
		}
	}

	/*   If or-clause args is empty, that mean all of pairs of elements 
     *   are const equal, so check the result of last sub-expr.
	 */
	if (orclause->args == NIL)
	{
		if (lasteva)
			return CCT_CONSTT;
		else
			return CCT_CONSTF;
	}

	if (list_length(orclause->args) == 1)
	{
		*transformed = linitial(orclause->args);
	}
	else
		*transformed = (Expr*)orclause;
#ifdef DEBUG_ROW_DECODE
	elog(NOTICE, "ROW COMPARSION TO NONCONSTRUCTOR: %s", nodeToString(*transformed));
#endif
	return 0;
}


/*
 * self_contradictory_recurse
 *	  Extract particulares of implict-AND clauses list
 *	  passed in and check whether it
 *	  is self contradictory or not. During the time of invoking routine
 *    also do the following jobs pruning and replacing and deleting clause.
 *
 *	  Returned value:
 *	  0: it's Ok.
 *	  1: is self-contradictory
 *	  Function expect a couple of caluse that mean these clauses are
 *	  implict-AND caluses. Once a pair of clauses or a clause are
 *	  self-contradictory report to the caller immediately.
 *	  A or-clause is a bit tricky, it's need to check every arg, if
 *	  sub-arg is self-contradictory abandon it. After recursion
 *	  if all of them are self contradictory that mean the or-clause
 *	  is self contradictory and report to the caller, else rebuilt the
 *	  or-clause (eg. x > 2 and x < 1 or y >3 in the case x > 2 and x < 1
 *	  will be eliminated, the output expression is y > 3 ).
 *	  We can push other expressions these have the same semantic level 
 *    with OR-expression down into the OR-expression 
 *    (eg. (x > 2 and x < 1 or y > 3) and y < 2, y < 2 will be
 *	  push down to sub-arg y > 3, this clause is self-contradictory).
 */
static int self_contradictory_recurse(List *node,
										ExtractedAttris *ea,
										ClauseCxtWapper *ccw,
										SelfExamContext *sec)
{
	int 			ret = 0;
	int 			i;
	Expr			*inter_node;
	VarattInfo 		*att_elem;
	ListCell		*lc;
	ClauseContext	*clausecxt;
	NullTest		*nt_node;
	Var				*varnode;
	ExtractedAttris	ea_info;
	ClauseCxtWapper	orccw;

	ea_info.addOp = false;
	ea_info.min_attr = sec->attrsdc.min_attr;
	ea_info.att_lst = NULL;
	ea_info.upper_ea = NULL;
	ea_info.array_vi = palloc0((sec->attrsdc.max_attr - sec->attrsdc.min_attr + 1) * sizeof(VarattInfo*));
	orccw.clausecxt = NIL;
	if (ea)
		ea_info.upper_ea = ea;
	
	if (IsA(node, List))
	{
		i = -1;
		foreach(lc, (List*) node)
		{
			inter_node = lfirst(lc);
			i++;
			if (IsA(inter_node, RestrictInfo))
			{
				RestrictInfo *rinfo = (RestrictInfo *) inter_node;
				if (rinfo->pseudoconstant)
					continue;
				inter_node = rinfo->clause;
				if (rinfo->fromec)
				{
					clausecxt = get_clause_cxt_internal(sec);
					clausecxt->cct = CCT_DEL;
					clausecxt->idx = i;
					ccw->clausecxt = lappend(ccw->clausecxt, clausecxt);					
				}
			}
			if (IsA(inter_node, Var))
			{
				varnode = (Var*) inter_node;
				if (varnode->vartype == BOOLOID)
				{
					att_elem = get_varatt_info_internal(sec, &ea_info, varnode);
					att_elem->bool_test |= BT_MARK | BT_IS_TRUE;
					ea_info.addOp = true;
				}
			}
			else if (IsA(inter_node, BoolExpr))
			{
				/*
				 *  It's need to postpone the or-expr, then the same
				 *  level simple expr can be push down.
				 */
				BoolExpr	*boolnode;
				boolnode = (BoolExpr*) inter_node;
				if (boolnode->boolop == NOT_EXPR
						&& list_length(boolnode->args) == 1
						&& IsA(linitial(boolnode->args), Var))
				{
					varnode = (Var*) linitial(boolnode->args);
					if (varnode->vartype == BOOLOID)
					{
						att_elem = get_varatt_info_internal(sec, &ea_info, varnode);
						att_elem->bool_test |= BT_MARK | BT_IS_FALSE;
						ea_info.addOp = true;
					}
				}
				else
				{
					clausecxt = get_clause_cxt_internal(sec);
					clausecxt->idx = i;
					clausecxt->cxt = inter_node;
					clausecxt->origin = inter_node;
					orccw.clausecxt = lappend(orccw.clausecxt, clausecxt);
				}
			}
			else if (IsA(inter_node, NullTest))
			{
				nt_node = (NullTest *)inter_node;
				if (!nt_node->argisrow && nt_node->arg)
				{
					if(!deconstruct_nulltest_attr((Node *)nt_node->arg,
													nt_node->nulltesttype, sec, &ea_info))
						EXAMINE_ABORT(1);
					ea_info.addOp = true;
				}
			}
			else if (IsA(inter_node, BooleanTest))
			{
				Node	    *basenode;
				
				BooleanTest *btest = (BooleanTest *) inter_node;
				if (IsA(btest->arg, RelabelType))
					basenode = (Node *) ((RelabelType *) btest->arg)->arg;
				else
					basenode = (Node *)btest->arg;
				if (basenode && IsA(basenode, Var))
				{
					att_elem = get_varatt_info_internal(sec, &ea_info, (Var *) btest->arg);
					switch (btest->booltesttype)
					{
						case IS_TRUE:
							att_elem->bool_test |= BT_MARK | BT_IS_TRUE;
							break;
						case IS_NOT_TRUE:
							att_elem->bool_test |= BT_MARK | BT_IS_NOT_TRUE;
							break;
						case IS_FALSE:
							att_elem->bool_test |= BT_MARK | BT_IS_FALSE;
							break;
						case IS_NOT_FALSE:
							att_elem->bool_test |= BT_MARK | BT_IS_NOT_FALSE;
							break;
						case IS_UNKNOWN:
							att_elem->bool_test |= BT_MARK | BT_IS_UNKNOWN;
							break;
						case IS_NOT_UNKNOWN:
							att_elem->bool_test |= BT_MARK | BT_IS_NOT_UNKNOWN;
							break;
						default:
							elog(ERROR, "unrecognized booltesttype: %d",
								 (int) btest->booltesttype);
							break;
					}
					ea_info.addOp = true;
				}
			}
			else if (IsA(inter_node, OpExpr))
			{
				OpExpr 		*opexpr;
				opexpr = (OpExpr *) inter_node;
				if (list_length(opexpr->args) != 2)
					continue;
				if (!extract_binary_op_clause_info(opexpr,
												linitial(opexpr->args),
												lsecond(opexpr->args),
												i,
												opexpr->opno,
												opexpr->inputcollid,
												sec,
												&ea_info,
												ccw))
					EXAMINE_ABORT(1);
			}
			else if (IsA(inter_node, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) inter_node;
				if (list_length(saop->args) != 2)
					continue;
				if (!extract_binary_op_clause_info(saop,
												linitial(saop->args),
												lsecond(saop->args),
												i,
												saop->opno,
												saop->inputcollid,
												sec,
												&ea_info,
												ccw))
					EXAMINE_ABORT(1);
			}
			else if (IsA(inter_node, RowCompareExpr))
			{
				Expr	*trans;
				int		result;
				result = row_comparsion_to_nonconstructor(inter_node, &trans);
				if (result == 0 && trans != NULL)
				{
					if (is_orclause(trans))
					{
						clausecxt = get_clause_cxt_internal(sec);
						clausecxt->idx = i;
						clausecxt->cxt = trans;
						clausecxt->origin = inter_node;
						orccw.clausecxt = lappend(orccw.clausecxt, clausecxt);
					}
					else if (is_andclause(trans))
					{
						/* Must be NullTest expr eg. row(a,b) >= row(a,b) */
						ListCell	*lnt;
						foreach(lnt, ((BoolExpr*)trans)->args)
						{
							Assert(IsA(lfirst(lnt), NullTest));
							nt_node = lfirst(lnt);
							if(!deconstruct_nulltest_attr((Node *)nt_node->arg,
															nt_node->nulltesttype,
															sec,
															&ea_info))
								EXAMINE_ABORT(1);
						}
						ea_info.addOp = true;
					}
					else if (IsA(trans, OpExpr))
					{
						if (!extract_binary_op_clause_info(trans,
											linitial(((OpExpr*)trans)->args),
											lsecond(((OpExpr*)trans)->args),
											i,
											((OpExpr*)trans)->opno,
											((OpExpr*)trans)->inputcollid,
											sec,
											&ea_info,
											ccw))
							EXAMINE_ABORT(1);
						clausecxt = get_clause_cxt_internal(sec);
						clausecxt->cct = CCT_REPLACE;
						clausecxt->idx = i;
						clausecxt->cxt = trans;
						ccw->clausecxt = lappend(ccw->clausecxt, clausecxt);
					}
					else if (IsA(trans, NullTest))
					{
						nt_node = (NullTest *)trans;
						if(!deconstruct_nulltest_attr((Node *)nt_node->arg,
														nt_node->nulltesttype,
														sec,
														&ea_info))
							EXAMINE_ABORT(1);
						clausecxt = get_clause_cxt_internal(sec);
						clausecxt->cct = CCT_REPLACE;
						clausecxt->idx = i;
						clausecxt->cxt = trans;
						ccw->clausecxt = lappend(ccw->clausecxt, clausecxt);
					}
				}
				else if (result == CCT_CONSTT)
				{
					/* (eg. row(1,2,3) >= row(1,2,3)) */
					clausecxt = get_clause_cxt_internal(sec);
					clausecxt->cct = CCT_CONSTT;
					clausecxt->idx = i;
					ccw->clausecxt = lappend(ccw->clausecxt, clausecxt);
				}
				else if (result == CCT_CONSTF)
					EXAMINE_ABORT(1);
			}
		}
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(node));
	}

	/* All base particulars had been collected, it's time to examine. */
	if(self_contradictory_check(sec, &ea_info, ccw))
		EXAMINE_ABORT(1);
	
	/*   Examine the or-clause that was postponed here because every
     *   OpExpr at same semantic level can be push down ( eg. x > 10 and y > 12 and
     *   ( x < 9 or y < 11) ), this case can be transform to (x > 10 and y > 12 and
     *   x <9) or (x > 10 and y > 12 and y < 11).
     */
	foreach(lc, orccw.clausecxt)
	{
		ListCell			*arg, *lc1;
		List				*arglst, *newraglst;
		ClauseCxtWapper		boolccw;
		ClauseContext		*ccxt;
		BoolExpr			*boolnode;
		uint8				*delidx;
		uint8				orchanged;
		clausecxt = lfirst(lc);
		boolnode = (BoolExpr *) clausecxt->cxt;

		switch (boolnode->boolop)
		{
			case AND_EXPR:
				delidx = palloc0((list_length(boolnode->args)+1) * sizeof(uint8));
				delidx++;
				boolccw.clausecxt = NIL;
				ret = self_contradictory_recurse(boolnode->args, ea, &boolccw, sec);
				if (ret > 0)
					EXAMINE_ABORT(1);

				foreach(lc1, boolccw.clausecxt)
				{
					ccxt = lfirst(lc1);
					delidx[-1] = 0x01;
					switch (ccxt->cct)
					{
						case CCT_DEL:
						case CCT_CONSTT:
							delidx[ccxt->idx] = 0x01;
							break;
						case CCT_REPLACE:
							arg = list_nth_cell(boolnode->args, ccxt->idx);
							/* It's need to be flatten if a and-expr, so flag it. */
							if (is_andclause(ccxt->cxt))
								delidx[ccxt->idx] = 0x02;
							lfirst(arg) = ccxt->cxt;
							break;
						default:
							break;
					}
					free_clause_cxt_internal(sec, ccxt);
				}
				/* OK, fix the args. */
				if (delidx[-1])
				{
					i = 0;
					arglst = NIL;
					newraglst = NIL;
					foreach(lc1, boolnode->args)
					{
						if (delidx[i] == 0x00)
							arglst = lappend(arglst, lfirst(lc1));
						else if (delidx[i] == 0x02)
							newraglst = list_concat(newraglst, ((BoolExpr*)lfirst(lc1))->args);
						i++;
					}
					boolnode->args = list_concat(arglst,newraglst);
					ccxt = get_clause_cxt_internal(sec);
					ccxt->idx = clausecxt->idx;
					/* If the reset of args is empty, that mean the and-expr
					 * is const true, because only weaker restrictive expr will be
					 * delete and the args must be not empty.
					 */
					if (boolnode->args == NIL)
						ccxt->cct = CCT_CONSTT;
					else
					{
						ccxt->cct = CCT_REPLACE;
						if (list_length(boolnode->args) == 1)
							ccxt->cxt = linitial(boolnode->args);
						else
							ccxt->cxt = boolnode;
					}
					ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
				}
				break;
			case OR_EXPR:
				newraglst = NIL;
				orchanged = 0x0;
				foreach(arg, boolnode->args)
				{
					boolccw.clausecxt = NIL;
					arglst = list_make1(lfirst(arg));
					if (self_contradictory_recurse(arglst, &ea_info, &boolccw, sec))
					{
						orchanged = 0x01;
						ret++;
					}
					else
					{
						Assert(list_length(boolccw.clausecxt) <= 1);
						if (list_length(boolccw.clausecxt) == 1)
						{
							orchanged = 0x01;
							ccxt = linitial(boolccw.clausecxt);
							Assert(ccxt->idx == 0);
							if (ccxt->cct == CCT_REPLACE)
								lfirst(arg) = ccxt->cxt;
							else if (ccxt->cct == CCT_CONSTT)
							{
								/*
								 *    Once found a const true sub-arg, skip checking any others,
								 *    the or-expr must be const true.
								 */
								orchanged = 0x02;
								free_clause_cxt_internal(sec, ccxt);
								list_free(arglst);
								break;
							}
							free_clause_cxt_internal(sec, ccxt);
						}
						if (is_orclause(lfirst(arg)))
							newraglst = list_concat(newraglst, ((const BoolExpr *) lfirst(arg))->args);
						else
							newraglst = lappend(newraglst, lfirst(arg));
					}
					list_free(arglst);
				}
				if (ret == list_length(boolnode->args))
					EXAMINE_ABORT(1);
#ifndef DEBUG_ROW_DECODE
				else if (orchanged == 0x01)
				{
					/*
					 * RowCompareExpr has been coverted to nonconstructor (OR-expression),
					 * if the number of the or-expr's args is more than
					 * one, it do not has benifit to subsequent planning, so
					 * do not do anything and keep itself.
					 */

					if ((IsA(clausecxt->origin, RowCompareExpr)
										&& list_length(newraglst) != 1))
						;
					else
#else
				else if (orchanged == 0x01 || (IsA(clausecxt->origin, RowCompareExpr) && orchanged != 0x02))
				{
#endif
					{
						ccxt = get_clause_cxt_internal(sec);
						ccxt->cct = CCT_REPLACE;
						ccxt->idx = clausecxt->idx;
						if (list_length(newraglst) == 1)
							ccxt->cxt = linitial(newraglst);
						else
						{
							boolnode->args = newraglst;
							ccxt->cxt = boolnode;
						}
						ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
					}
				}
				else if (orchanged == 0x02)
				{
					ccxt = get_clause_cxt_internal(sec);
					ccxt->cct = CCT_CONSTT;
					ccxt->idx = clausecxt->idx;
					ccw->clausecxt = lappend(ccw->clausecxt, ccxt);
				}
				/* If or-expr is not changed, do nothing more. */
				ret = 0;
				break;
			case NOT_EXPR:
				break;
			default:
				elog(ERROR, "unrecognized boolop: %d", (int) boolnode->boolop);
				break;
		}
		free_clause_cxt_internal(sec, clausecxt);
	}

finished:
	if (ea_info.att_lst != NULL && ea_info.att_lst == ea_info.last)
	{
		free_attinfo_cxt_internal(sec, ea_info.att_lst);
	}
	else if (ea_info.att_lst != NULL)
	{
		ea_info.last->next = sec->vifree;
		sec->vifree = ea_info.att_lst;
	}
	pfree(ea_info.array_vi);
	return ret;
}

/*
 * examine_self_contradictory_rels
 *
 *    Examine the base rel's baserestrictinfo to confirm that it's a
 *    self-contradictory rel or not.
 *    Mark the rel as a dummy rel if it's a self-contradictory rel,
 *    this is a huge win for subsequent planning.
 *    Note: deduct will be set true if caller need to process EC.
 */
static void examine_self_contradictory_rels(PlannerInfo *root, RelOptInfo *rel, bool deduct)
{
	int		n;
	RestrictInfo	*restrictinfo;
	ClauseCxtWapper	ccwapper;
	SelfExamContext sec;

	MemSet(&sec, 0, sizeof(SelfExamContext));
	/*
	 * Initialize sopcinfo with opclass info. Extract all bt-am opclass
	 * particulars and sort by opcintype.
	 */
	if (sec.sopcinfo.n_members == 0)
	{
		if (!default_opclassinfo_for_am(BTREE_AM_OID, extract_opc_info, &sec.sopcinfo))
			elog(ERROR, "non default opclass records for AM %d", (int) BTREE_AM_OID);
		qsort(sec.sopcinfo.opcies, sec.sopcinfo.n_members, sizeof(OpcInfoElement*), opc_element_cmp);
	}

	/* For every base rel reuse the AttrsDetailsCache memory. */
	if (!sec.attrsdc.atop_array)
	{
		sec.attrsdc.atop_array = palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(AttrOpFamily*));
		sec.attrsdc.atop_set = palloc0(sizeof(AttrOpFamilySet));
	}
	else
	{
		AttrOpFamilySet *atfs, *atfs_tmp;
		atfs = sec.attrsdc.atop_set->next;
		n = rel->max_attr - rel->min_attr + 1;
		/* If the number of current rel attri greater than previous rel and reset it. */
		if (n > sec.attrsdc.max_attr - sec.attrsdc.min_attr + 1)
		{
			pfree(sec.attrsdc.atop_array);
			sec.attrsdc.atop_array = palloc0(n * sizeof(AttrOpFamily*));
		}
		else
			MemSet(sec.attrsdc.atop_array, 0, n * sizeof(AttrOpFamily*));
		while(atfs)
		{
			atfs_tmp = atfs->next;
			pfree(atfs);
			atfs = atfs_tmp;
		}
		sec.attrsdc.len_set = 0;
	}
	sec.attrsdc.min_attr = rel->min_attr;
	sec.attrsdc.max_attr = rel->max_attr;
	
	ccwapper.clausecxt = NIL;
	if (!self_contradictory_recurse(rel->baserestrictinfo, NULL, &ccwapper, &sec))
	{
		/* Check any pruned-clause and rebuilt it. */
		List		*arglst;
		ListCell	*br, *lc;
		ListCell	*arg;
		BoolExpr 	*expression;
		RestrictInfo 	*or_rinfo;
		ClauseContext	*ccxt;
		bool		*delidx = NULL;
		if (list_length(ccwapper.clausecxt) > 0)
		{
			delidx = palloc0((list_length(rel->baserestrictinfo) + 1) * sizeof(bool));
			delidx++;
		}
		foreach(lc, ccwapper.clausecxt)
		{
			ccxt = lfirst(lc);
			br = list_nth_cell(rel->baserestrictinfo, ccxt->idx);
			restrictinfo = lfirst(br);
			switch (ccxt->cct)
			{
				case CCT_DEL:
					/* Be carefull if it's keeped from EC, 
					 * count baseri_nums because it will be delete below
					 * and can't count it in phase1.
					 */
					if (deduct && restrictinfo->fromec)
						rel->baseri_nums++;
					delidx[-1] = delidx[ccxt->idx] = true;
					break;
				case CCT_CONSTT:
					delidx[-1] = delidx[ccxt->idx] = true;
					break;
				case CCT_REPLACE:
					Assert(!IsA(ccxt->cxt, List));
					arglst = NIL;
					if (is_andclause(ccxt->cxt))
					{
						expression = ccxt->cxt;
						arglst = list_concat(arglst, expression->args);
					}
					else
						arglst = lappend(arglst, ccxt->cxt);
					delidx[-1] = delidx[ccxt->idx] = true;
					foreach(arg, arglst)
					{
						or_rinfo = make_restrictinfo(root,
   														(Expr*) lfirst(arg),
   														restrictinfo->is_pushed_down,
   														restrictinfo->has_clone,
   														restrictinfo->is_clone,
   														restrictinfo->pseudoconstant,
   														restrictinfo->security_level,
   														NULL,
   														restrictinfo->incompatible_relids,
   														restrictinfo->outer_relids);
						if (deduct)
						{
							check_mergejoinable(or_rinfo);
							if (or_rinfo->mergeopfamilies)
							{
								if (restrictinfo->allow_equivalence)
								{
									if (process_equivalence(root, &or_rinfo, restrictinfo->jdomain))
									{	
										rel->baseri_nums++;
										continue;
									}
									if (or_rinfo->mergeopfamilies)
										initialize_mergeclause_eclasses(root, or_rinfo);
								}
								else
									initialize_mergeclause_eclasses(root, or_rinfo);
							}
						}
						rel->baserestrictinfo = lappend(rel->baserestrictinfo, or_rinfo);
					}
					break;
				default:
					break;
			}
			free_clause_cxt_internal(&sec, ccxt);
		}
		if (delidx && delidx[-1])
		{
			List	*rlist = NIL;
			int 	i = 0;
			foreach(lc, rel->baserestrictinfo)
			{
				if (!delidx[i++])
					rlist = lappend(rlist, lfirst(lc));
			}
			rel->baserestrictinfo = rlist;
		}
	}
	else
		mark_dummy_rel(rel);
}

/*
 * examine_self_contradictory_rels_phase1
 *
 *    Some clause maybe be changed and the reult may match the
 *    EC mechanism, so it's need to add it into the EC.
 */
void examine_self_contradictory_rels_phase1(PlannerInfo *root)
{
	int		rti;
	RestrictInfo	*rinfo;

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		/* There may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;
		/* Ignore any "otherrels". */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;
		if (rel->baserestrictinfo == NIL)
			continue;
		if (list_length(rel->baserestrictinfo) == 1)
		{
			rinfo = linitial(rel->baserestrictinfo);
			if (rinfo->fromec)
			{
				rel->baseri_nums = 1;
				rel->baserestrictinfo = NIL;
				continue;
			}
		}
		examine_self_contradictory_rels(root, rel, true);
		if (!is_dummy_rel(rel))
			rel->baseri_nums += list_length(rel->baserestrictinfo);
	}	
}

/*
 * examine_self_contradictory_rels_phase1
 *
 *    Only care about rel's baserestrictinfo that had added a EC clause.
 */
void examine_self_contradictory_rels_phase2(PlannerInfo *root)
{
	int		rti;
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		/* There may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;
		/* Ignore any "otherrels". */
		if (rel->reloptkind != RELOPT_BASEREL)
			continue;
		if (rel->baserestrictinfo == NIL)
			continue;
		if (list_length(rel->baserestrictinfo) == 1)
			continue;
		if (is_dummy_rel(rel))
			continue;
		if (rel->baseri_nums != list_length(rel->baserestrictinfo))
			examine_self_contradictory_rels(root, rel, false);
	}
}
