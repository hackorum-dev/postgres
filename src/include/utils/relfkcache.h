#ifndef RELFKCACHE_H
#define RELFKCACHE_H

/*
 * ForeignKeyCacheInfo
 *		Information the relcache can cache about foreign key constraints
 *
 * This is basically just an image of relevant columns from pg_constraint.
 * We make it a subclass of Node so that copyObject() can be used on a list
 * of these, but we also ensure it is a "flat" object without substructure,
 * so that list_free_deep() is sufficient to free such a list.
 * The per-FK-column arrays can be fixed-size because we allow at most
 * INDEX_MAX_KEYS columns in a foreign key constraint.
 *
 * Currently, we mostly cache fields of interest to the planner, but the set
 * of fields has already grown the constraint OID for other uses.
 */
typedef struct ForeignKeyCacheInfo
{
	pg_node_attr(no_equal, no_read, no_query_jumble)

	NodeTag		type;
	/* oid of the constraint itself */
	Oid			conoid;
	/* relation constrained by the foreign key */
	Oid			conrelid;
	/* relation referenced by the foreign key */
	Oid			confrelid;
	/* number of columns in the foreign key */
	int			nkeys;

	/* Is enforced ? */
	bool		conenforced;

	/*
	 * these arrays each have nkeys valid entries:
	 */
	/* cols in referencing table */
	AttrNumber	conkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* cols in referenced table */
	AttrNumber	confkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* PK = FK operator OIDs */
	Oid			conpfeqop[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
} ForeignKeyCacheInfo;



#endif		/* RELFKCACHE_H */
