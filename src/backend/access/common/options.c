/*-------------------------------------------------------------------------
 *
 * options.c
 *	  An unifom, context-free API for processing name=value options. Used
 *	  to process relation optons (reloptions), attribute options, opclass
 *	  options, etc.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/options.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/options.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "mb/pg_wchar.h"


/*
 * OPTIONS SPECIFICATION and OPTION SPECIFICATION SET
 *
 * Each option is defined via Option Specification object (Option Spec).
 * Option Spec should have all information that is needed for processing
 * (parsing, validating, converting) of a single option. Implemented via set of
 * option_spec_* structures.
 *
 * A set of Option Specs (Options Spec Set), defines all options available for
 * certain object (certain relation kind for example). It is a list of
 * Options Specs, plus validation functions that can be used to validate whole
 * option set, if needed. Implemenred via options_spec_set structure and set of
 * optionsSpecSetAdd* functions that are used for adding Option Specs items to
 * a Set.
 *
 * NOTE: we choose therm "sepcification" instead of "definition" because therm
 * "definition" is used for objects that came from lexer. So to avoud confusion
 * here we have Option Specifications, and all "definitions" are from lexer.
 */

/*
 * OPTION VALUES REPRESENTATIONS
 *
 * Option values usually came from lexer in form of defList obect, stored in
 * pg_catalog as text array, and used when they are stored in memory as
 * C-structure. These are different option values representations. Here goes
 * brief description of all representations used in the code.
 *
 * Values
 *
 * Values are an internal representation that is used while converting
 * Values between other representation. Value is called "parsed",
 * when Value's value is converted to a proper type and validated, or is called
 * "unparsed", when Value's value is stored as raw string that was obtained
 * from the source without any cheks. In convertation funcion names first case
 * is refered as Values, second case is refered as RawValues. Values is
 * implemented as List of option_value C-structures.
 *
 * defList
 *
 * Options in form of definition List that comes from lexer. (For reloptions it
 * is a part of SQL query that goes after WITH, SET or RESET keywords). Can be
 * converted to and from Values using optionsDefListToRawValues and
 * optionsTextArrayToRawValues functions.
 *
 * TEXT[]
 *
 * Options in form suitable for storig in TEXT[] field in DB. (E.g. reloptions
 * are stores in pg_catalog.pg_class table in reloptions field). Can be converted
 * to and from Values using optionsValuesToTextArray and optionsTextArrayToRawValues
 * functions.
 *
 * Bytea
 *
 * Option data stored in C-structure with varlena header in the beginning of the
 * structure. This representation is used to pass option values to the core
 * postgres. It is fast to read, it can be cached and so on. Bytea rpresentation
 * can be obtained from Vales using optionsValuesToBytea function, and can't be
 * converted back.
 */

static option_spec_basic *allocateOptionSpec(int type, const char *name,
						 const char *desc, LOCKMODE lockmode,
						 option_spec_flags flags, int struct_offset);

static void parse_one_option(option_value * option, const char *text_str,
				 int text_len, bool validate);
static void *optionsAllocateBytea(options_spec_set * spec_set, List *options);


static List *
optionsDefListToRawValues(List *defList, options_parse_mode
						  parse_mode);
static Datum optionsValuesToTextArray(List *options_values);
static List *optionsMergeOptionValues(List *old_options, List *new_options);
static bytea *optionsValuesToBytea(List *options, options_spec_set * spec_set);
List *optionsTextArrayToRawValues(Datum array_datum);
List *optionsParseRawValues(List *raw_values, options_spec_set * spec_set,
					  options_parse_mode mode);


/*
 * Options spec_set functions
 */

/*
 * Options catalog describes options available for certain object. Catalog has
 * all necessary information for parsing transforming and validating options
 * for an object. All parsing/validation/transformation functions should not
 * know any details of option implementation for certain object, all this
 * information should be stored in catalog instead and interpreted by
 * pars/valid/transf functions blindly.
 *
 * The heart of the option catalog is an array of option definitions.  Options
 * definition specifies name of option, type, range of acceptable values, and
 * default value.
 *
 * Options values can be one of the following types: bool, int, real, enum,
 * string. For more info see "option_type" and "optionsCatalogAddItemYyyy"
 * functions.
 *
 * Option definition flags allows to define parser behavior for special (or not
 * so special) cases. See option_spec_flags for more info.
 *
 * Options and Lock levels:
 *
 * The default choice for any new option should be AccessExclusiveLock.
 * In some cases the lock level can be reduced from there, but the lock
 * level chosen should always conflict with itself to ensure that multiple
 * changes aren't lost when we attempt concurrent changes.
 * The choice of lock level depends completely upon how that parameter
 * is used within the server, not upon how and when you'd like to change it.
 * Safety first. Existing choices are documented here, and elsewhere in
 * backend code where the parameters are used.
 *
 * In general, anything that affects the results obtained from a SELECT must be
 * protected by AccessExclusiveLock.
 *
 * Autovacuum related parameters can be set at ShareUpdateExclusiveLock
 * since they are only used by the AV procs and don't change anything
 * currently executing.
 *
 * Fillfactor can be set because it applies only to subsequent changes made to
 * data blocks, as documented in heapio.c
 *
 * n_distinct options can be set at ShareUpdateExclusiveLock because they
 * are only used during ANALYZE, which uses a ShareUpdateExclusiveLock,
 * so the ANALYZE will not be affected by in-flight changes. Changing those
 * values has no affect until the next ANALYZE, so no need for stronger lock.
 *
 * Planner-related parameters can be set with ShareUpdateExclusiveLock because
 * they only affect planning and not the correctness of the execution. Plans
 * cannot be changed in mid-flight, so changes here could not easily result in
 * new improved plans in any case. So we allow existing queries to continue
 * and existing plans to survive, a small price to pay for allowing better
 * plans to be introduced concurrently without interfering with users.
 *
 * Setting parallel_workers is safe, since it acts the same as
 * max_parallel_workers_per_gather which is a USERSET parameter that doesn't
 * affect existing plans or queries.
*/

/*
 * allocateOptionsSpecSet
 *		Creates new Option Spec Set object: Allocates memory and initializes
 *		structure members.
 *
 * Spec Set items can be add via allocateOptionSpec and optionSpecSetAddItem functions
 * or by calling directly any of optionsSpecSetAdd* function (preferable way)
 *
 * namespace - Spec Set can be bind to certain namespace (E.g.
 * namespace.option=value). Options from other namespaces will be ignored while
 * processing. If set to NULL, no namespace will be used at all.
 *
 * size_of_bytea - size of target structure of Bytea options represenation
 *
 * num_items_expected - if you know expected number of Spec Set items set it here.
 * Set to -1 in other cases. num_items_expected will be used for preallocating memory
 * and will trigger error, if you try to add more items than you expected.
 */

options_spec_set *
allocateOptionsSpecSet(const char *namespace, int size_of_bytea, int num_items_expected)
{
	MemoryContext oldcxt;
	options_spec_set *spec_set;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	spec_set = palloc(sizeof(options_spec_set));
	if (namespace)
	{
		spec_set->namespace = palloc(strlen(namespace) + 1);
		strcpy(spec_set->namespace, namespace);
	}
	else
		spec_set->namespace = NULL;
	if (num_items_expected > 0)
	{
		spec_set->num_allocated = num_items_expected;
		spec_set->forbid_realloc = true;
		spec_set->definitions = palloc(
				 spec_set->num_allocated * sizeof(option_spec_basic *));
	}
	else
	{
		spec_set->num_allocated = 0;
		spec_set->forbid_realloc = false;
		spec_set->definitions = NULL;
	}
	spec_set->num = 0;
	spec_set->struct_size = size_of_bytea;
	spec_set->postprocess_fun = NULL;
	MemoryContextSwitchTo(oldcxt);
	return spec_set;
}

/*
 * allocateOptionSpec
 *		Allocates a new Option Specifiation object of desired type and
 *		initialize the type-independent fields
 */
static option_spec_basic *
allocateOptionSpec(int type, const char *name, const char *desc, LOCKMODE lockmode,
						 option_spec_flags flags, int struct_offset)
{
	MemoryContext oldcxt;
	size_t		size;
	option_spec_basic *newoption;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	switch (type)
	{
		case OPTION_TYPE_BOOL:
			size = sizeof(option_spec_bool);
			break;
		case OPTION_TYPE_INT:
			size = sizeof(option_spec_int);
			break;
		case OPTION_TYPE_REAL:
			size = sizeof(option_spec_real);
			break;
		case OPTION_TYPE_ENUM:
			size = sizeof(option_spec_enum);
			break;
		case OPTION_TYPE_STRING:
			size = sizeof(option_spec_string);
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", type);
			return NULL;		/* keep compiler quiet */
	}

	newoption = palloc(size);

	newoption->name = pstrdup(name);
	if (desc)
		newoption->desc = pstrdup(desc);
	else
		newoption->desc = NULL;
	newoption->type = type;
	newoption->lockmode = lockmode;
	newoption->flags = flags;
	newoption->struct_offset = struct_offset;

	MemoryContextSwitchTo(oldcxt);

	return newoption;
}

/*
 * optionSpecSetAddItem
 *		Adds pre-created Option Specification objec to the Spec Set
 */
static void
optionSpecSetAddItem(option_spec_basic * newoption,
					 options_spec_set * spec_set)
{
	if (spec_set->num >= spec_set->num_allocated)
	{
		MemoryContext oldcxt;

		Assert(!spec_set->forbid_realloc);
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		if (spec_set->num_allocated == 0)
		{
			spec_set->num_allocated = 8;
			spec_set->definitions = palloc(
				 spec_set->num_allocated * sizeof(option_spec_basic *));
		}
		else
		{
			spec_set->num_allocated *= 2;
			spec_set->definitions = repalloc(spec_set->definitions,
				 spec_set->num_allocated * sizeof(option_spec_basic *));
		}
		MemoryContextSwitchTo(oldcxt);
	}
	spec_set->definitions[spec_set->num] = newoption;
	spec_set->num++;
}


/*
 * optionsSpecSetAddBool
 *		Adds boolean Option Specification entry to the Spec Set
 */
void
optionsSpecSetAddBool(options_spec_set * spec_set, const char *name, const char *desc,
						  LOCKMODE lockmode, option_spec_flags flags,
						  int struct_offset, bool default_val)
{
	option_spec_bool *spec_set_item;

	spec_set_item = (option_spec_bool *)
		allocateOptionSpec(OPTION_TYPE_BOOL, name, desc, lockmode,
								 flags, struct_offset);

	spec_set_item->default_val = default_val;

	optionSpecSetAddItem((option_spec_basic *) spec_set_item, spec_set);
}

/*
 * optionsSpecSetAddInt
 *		Adds integer Option Specification entry to the Spec Set
 */
void
optionsSpecSetAddInt(options_spec_set * spec_set, const char *name,
		  const char *desc, LOCKMODE lockmode, option_spec_flags flags,
				int struct_offset, int default_val, int min_val, int max_val)
{
	option_spec_int *spec_set_item;

	spec_set_item = (option_spec_int *)
		allocateOptionSpec(OPTION_TYPE_INT, name, desc, lockmode,
								 flags, struct_offset);

	spec_set_item->default_val = default_val;
	spec_set_item->min = min_val;
	spec_set_item->max = max_val;

	optionSpecSetAddItem((option_spec_basic *) spec_set_item, spec_set);
}

/*
 * optionsSpecSetAddReal
 *		Adds float Option Specification entry to the Spec Set
 */
void
optionsSpecSetAddReal(options_spec_set * spec_set, const char *name, const char *desc,
		 LOCKMODE lockmode, option_spec_flags flags, int struct_offset,
						  double default_val, double min_val, double max_val)
{
	option_spec_real *spec_set_item;

	spec_set_item = (option_spec_real *)
		allocateOptionSpec(OPTION_TYPE_REAL, name, desc, lockmode,
								 flags, struct_offset);

	spec_set_item->default_val = default_val;
	spec_set_item->min = min_val;
	spec_set_item->max = max_val;

	optionSpecSetAddItem((option_spec_basic *) spec_set_item, spec_set);
}

/*
 * optionsSpecSetAddEnum
 *		Adds enum Option Specification entry to the Spec Set
 *
 * The members array must have a terminating NULL entry.
 *
 * The detailmsg is shown when unsupported values are passed, and has this
 * form:   "Valid values are \"foo\", \"bar\", and \"bar\"."
 *
 * The members array and detailmsg are not copied -- caller must ensure that
 * they are valid throughout the life of the process.
 */

void
optionsSpecSetAddEnum(options_spec_set * spec_set, const char *name, const char *desc,
		LOCKMODE lockmode, option_spec_flags flags, int struct_offset,
		opt_enum_elt_def * members, int default_val, const char *detailmsg)
{
	option_spec_enum *spec_set_item;

	spec_set_item = (option_spec_enum *)
		allocateOptionSpec(OPTION_TYPE_ENUM, name, desc, lockmode,
								 flags, struct_offset);

	spec_set_item->default_val = default_val;
	spec_set_item->members = members;
	spec_set_item->detailmsg = detailmsg;

	optionSpecSetAddItem((option_spec_basic *) spec_set_item, spec_set);
}

/*
 * optionsSpecSetAddString
 *		Adds string Option Specification entry to the Spec Set
 *
 * "validator" is an optional function pointer that can be used to test the
 * validity of the values. It must elog(ERROR) when the argument string is
 * not acceptable for the variable. Note that the default value must pass
 * the validation.
 */
void
optionsSpecSetAddString(options_spec_set * spec_set, const char *name, const char *desc,
		 LOCKMODE lockmode, option_spec_flags flags, int struct_offset,
				   const char *default_val, validate_string_option validator)
{
	option_spec_string *spec_set_item;

	/* make sure the validator/default combination is sane */
	if (validator)
		(validator) (default_val);

	spec_set_item = (option_spec_string *)
		allocateOptionSpec(OPTION_TYPE_STRING, name, desc, lockmode,
								 flags, struct_offset);
	spec_set_item->validate_cb = validator;

	if (default_val)
		spec_set_item->default_val = MemoryContextStrdup(TopMemoryContext,
														default_val);
	else
		spec_set_item->default_val = NULL;
	optionSpecSetAddItem((option_spec_basic *) spec_set_item, spec_set);
}


/*
 * Options transform functions
 */

/* FIXME this comment should be updated
 * Option values exists in five representations: DefList, TextArray, Values and
 * Bytea:
 *
 * DefList: Is a List of DefElem structures, that comes from syntax analyzer.
 * It can be transformed to Values representation for further parsing and
 * validating
 *
 * Values: A List of option_value structures. Is divided into two subclasses:
 * RawValues, when values are already transformed from DefList or TextArray,
 * but not parsed yet. (In this case you should use raw_name and raw_value
 * structure members to see option content). ParsedValues (or just simple
 * Values) is crated after finding a definition for this option in a spec_set
 * and after parsing of the raw value. For ParsedValues content is stored in
 * values structure member, and name can be taken from option definition in gen
 * structure member.  Actually Value list can have both Raw and Parsed values,
 * as we do not validate options that came from database, and db option that
 * does not exist in spec_set is just ignored, and kept as RawValues
 *
 * TextArray: The representation in which  options for existing object comes
 * and goes from/to database; for example from pg_class.reloptions. It is a
 * plain TEXT[] db object with name=value text inside. This representation can
 * be transformed into Values for further processing, using options spec_set.
 *
 * Bytea: Is a binary representation of options. Each object that has code that
 * uses options, should create a C-structure for this options, with varlen
 * 4-byte header in front of the data; all items of options spec_set should have
 * an offset of a corresponding binary data in this structure, so transform
 * function can put this data in the correct place. One can transform options
 * data from values representation into Bytea, using spec_set data, and then use
 * it as a usual Datum object, when needed. This Datum should be cached
 * somewhere (for example in rel->rd_options for relations) when object that
 * has option is loaded from db.
 */


/* optionsDefListToRawValues
 *		Converts option values that came from syntax analyzer (DefList) into
 *		Values List.
 *
 * No parsing is done here except for checking that RESET syntax is correct
 * (syntax analyzer do not see difference between SET and RESET cases, we
 * should treat it here manually
 */
static List *
optionsDefListToRawValues(List *defList, options_parse_mode parse_mode)
{
	ListCell   *cell;
	List	   *result = NIL;

	foreach(cell, defList)
	{
		option_value *option_dst;
		DefElem    *def = (DefElem *) lfirst(cell);
		char	   *value;

		option_dst = palloc(sizeof(option_value));

		if (def->defnamespace)
		{
			option_dst->namespace = palloc(strlen(def->defnamespace) + 1);
			strcpy(option_dst->namespace, def->defnamespace);
		}
		else
		{
			option_dst->namespace = NULL;
		}
		option_dst->raw_name = palloc(strlen(def->defname) + 1);
		strcpy(option_dst->raw_name, def->defname);

		if (parse_mode & OPTIONS_PARSE_MODE_FOR_RESET)
		{
			/*
			 * If this option came from RESET statement we should throw error
			 * it it brings us name=value data, as syntax analyzer do not
			 * prevent it
			 */
			if (def->arg != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("RESET must not include values for parameters")));

			option_dst->status = OPTION_VALUE_STATUS_FOR_RESET;
		}
		else
		{
			/*
			 * For SET statement we should treat (name) expression as if it is
			 * actually (name=true) so do it here manually. In other cases
			 * just use value as we should use it
			 */
			option_dst->status = OPTION_VALUE_STATUS_RAW;
			if (def->arg != NULL)
				value = defGetString(def);
			else
				value = "true";
			option_dst->raw_value = palloc(strlen(value) + 1);
			strcpy(option_dst->raw_value, value);
		}

		result = lappend(result, option_dst);
	}
	return result;
}

/*
 * optionsValuesToTextArray
 *		Converts List of option_values into TextArray
 *
 *	Convertation is made to put options into database (e.g. in
 *	pg_class.reloptions for all relation options)
 */

Datum
optionsValuesToTextArray(List *options_values)
{
	ArrayBuildState *astate = NULL;
	ListCell   *cell;
	Datum		result;

	foreach(cell, options_values)
	{
		option_value *option = (option_value *) lfirst(cell);
		const char *name;
		char	   *value;
		text	   *t;
		int			len;

		/*
		 * Raw value were not cleared while parsing, so instead of converting
		 * it back, just use it to store value as text
		 */
		value = option->raw_value;

		Assert(option->status != OPTION_VALUE_STATUS_EMPTY);

		/*
		 * Name will be taken from option definition, if option were parsed or
		 * from raw_name if option were not parsed for some reason
		 */
		if (option->status == OPTION_VALUE_STATUS_PARSED)
			name = option->gen->name;
		else
			name = option->raw_name;

		/*
		 * Now build "name=value" string and append it to the array
		 */
		len = VARHDRSZ + strlen(name) + strlen(value) + 1;
		t = (text *) palloc(len + 1);
		SET_VARSIZE(t, len);
		sprintf(VARDATA(t), "%s=%s", name, value);
		astate = accumArrayResult(astate, PointerGetDatum(t), false,
								  TEXTOID, CurrentMemoryContext);
	}
	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;

	return result;
}

/*
 * optionsTextArrayToRawValues
 *		Converts options from TextArray format into RawValues list.
 *
 *	This function is used to convert options data that comes from database to
 *	List of option_values, for further parsing, and, in the case of ALTER
 *	command, for merging with new option values.
 */
List *
optionsTextArrayToRawValues(Datum array_datum)
{
	List	   *result = NIL;

	if (PointerIsValid(DatumGetPointer(array_datum)))
	{
		ArrayType  *array = DatumGetArrayTypeP(array_datum);
		Datum	   *options;
		int			noptions;
		int			i;

		deconstruct_array(array, TEXTOID, -1, false, 'i',
						  &options, NULL, &noptions);

		for (i = 0; i < noptions; i++)
		{
			option_value *option_dst;
			char	   *text_str = VARDATA(options[i]);
			int			text_len = VARSIZE(options[i]) - VARHDRSZ;
			int			i;
			int			name_len = -1;
			char	   *name;
			int			raw_value_len;
			char	   *raw_value;

			/*
			 * Find position of '=' sign and treat id as a separator between
			 * name and value in "name=value" item
			 */
			for (i = 0; i < text_len; i = i + pg_mblen(text_str))
			{
				if (text_str[i] == '=')
				{
					name_len = i;
					break;
				}
			}
			Assert(name_len >= 1);		/* Just in case */

			raw_value_len = text_len - name_len - 1;

			/*
			 * Copy name from src
			 */
			name = palloc(name_len + 1);
			memcpy(name, text_str, name_len);
			name[name_len] = '\0';

			/*
			 * Copy value from src
			 */
			raw_value = palloc(raw_value_len + 1);
			memcpy(raw_value, text_str + name_len + 1, raw_value_len);
			raw_value[raw_value_len] = '\0';

			/*
			 * Create new option_value item
			 */
			option_dst = palloc(sizeof(option_value));
			option_dst->status = OPTION_VALUE_STATUS_RAW;
			option_dst->raw_name = name;
			option_dst->raw_value = raw_value;
			option_dst->namespace = NULL;

			result = lappend(result, option_dst);
		}
	}
	return result;
}

/*
 * optionsMergeOptionValues
 *		Merges two lists of option_values into one list
 *
 * This function is used to merge two Values list into one. It is used for all
 * kinds of ALTER commands when existing options are merged|replaced with new
 * options list. This function also process RESET variant of ALTER command. It
 * merges two lists as usual, and then removes all items with RESET flag on.
 *
 * Both incoming lists will be destroyed while merging
 */
static List *
optionsMergeOptionValues(List *old_options, List *new_options)
{
	List	   *result = NIL;
	ListCell   *old_cell;
	ListCell   *new_cell;

	/*
	 * First add to result all old options that are not mentioned in new list
	 */
	foreach(old_cell, old_options)
	{
		bool		found;
		const char *old_name;
		option_value *old_option;

		old_option = (option_value *) lfirst(old_cell);
		if (old_option->status == OPTION_VALUE_STATUS_PARSED)
			old_name = old_option->gen->name;
		else
			old_name = old_option->raw_name;

		/*
		 * Looking for a new option with same name
		 */
		found = false;
		foreach(new_cell, new_options)
		{
			option_value *new_option;
			const char *new_name;

			new_option = (option_value *) lfirst(new_cell);
			if (new_option->status == OPTION_VALUE_STATUS_PARSED)
				new_name = new_option->gen->name;
			else
				new_name = new_option->raw_name;

			if (strcmp(new_name, old_name) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			result = lappend(result, old_option);
	}
	/*
	 * Now add all to result all new options that are not designated for reset
	 */
	foreach(new_cell, new_options)
	{
		option_value *new_option;
		new_option = (option_value *) lfirst(new_cell);

		if(new_option->status != OPTION_VALUE_STATUS_FOR_RESET)
			result = lappend(result, new_option);
	}
	return result;
}

/*
 * optionsDefListValdateNamespaces
 *		Function checks that all options represented as DefList has no
 *		namespaces or have namespaces only from allowed list
 *
 * Function accept options as DefList and NULL terminated list of allowed
 * namespaces. It throws an error if not proper namespace was found.
 *
 * This function actually used only for tables with it's toast. namespace
 */
void
optionsDefListValdateNamespaces(List *defList, char **allowed_namespaces)
{
	ListCell   *cell;

	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		/*
		 * Checking namespace only for options that have namespaces. Options
		 * with no namespaces are always accepted
		 */
		if (def->defnamespace)
		{
			bool		found = false;
			int			i = 0;

			while (allowed_namespaces[i])
			{
				if (strcmp(def->defnamespace,
								  allowed_namespaces[i]) == 0)
				{
					found = true;
					break;
				}
				i++;
			}
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized parameter namespace \"%s\"",
								def->defnamespace)));
		}
	}
}

/*
 * optionsDefListFilterNamespaces
 *		Iterates over DefList, choose items with specified namespace and adds
 *		them to a result List
 *
 * This function does not destroy source DefList but does not create copies
 * of List nodes.
 * It is actually used only for tables, in order to split toast and heap
 * reloptions, so each one can be stored in on it's own pg_class record
 */
List *
optionsDefListFilterNamespaces(List *defList, const char *namespace)
{
	ListCell   *cell;
	List	   *result = NIL;

	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if ((!namespace && !def->defnamespace) ||
			(namespace && def->defnamespace &&
			 strcmp(namespace, def->defnamespace) == 0))
		{
			result = lappend(result, def);
		}
	}
	return result;
}

/*
 * optionsTextArrayToDefList
 *		Convert the text-array format of reloptions into a List of DefElem.
 */
List *
optionsTextArrayToDefList(Datum options)
{
	List	   *result = NIL;
	ArrayType  *array;
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	/* Nothing to do if no options */
	if (!PointerIsValid(DatumGetPointer(options)))
		return result;

	array = DatumGetArrayTypeP(options);

	deconstruct_array(array, TEXTOID, -1, false, 'i',
					  &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *s;
		char	   *p;
		Node	   *val = NULL;

		s = TextDatumGetCString(optiondatums[i]);
		p = strchr(s, '=');
		if (p)
		{
			*p++ = '\0';
			val = (Node *) makeString(pstrdup(p));
		}
		result = lappend(result, makeDefElem(pstrdup(s), val, -1));
	}

	return result;
}

/* FIXME write comment here */

Datum
optionsDefListToTextArray(List *defList)
{
	ListCell   *cell;
	Datum		result;
	ArrayBuildState *astate = NULL;

	foreach(cell, defList)
	{
		DefElem	   *def = (DefElem *) lfirst(cell);
		const char *name = def->defname;
		const char *value;
		text	   *t;
		int			len;

		if (def->arg != NULL)
			value = defGetString(def);
		else
			value = "true";

		if (def->defnamespace)
		{
			Assert(false); /* Should not get here */
			/* This function is used for backward compatibility in the place were namespases are not allowed */
			return (Datum) 0;
		}
		len = VARHDRSZ + strlen(name) + strlen(value) + 1;
		t = (text *) palloc(len + 1);
		SET_VARSIZE(t, len);
		sprintf(VARDATA(t), "%s=%s", name, value);
		astate = accumArrayResult(astate, PointerGetDatum(t), false,
								  TEXTOID, CurrentMemoryContext);

	}
	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;
	return result;
}


/*
 * optionsParseRawValues
 *		Parses and vlaidates (if proper flag is set) option_values. As a result
 *		caller will get the list of parsed (or partly parsed) option_values
 *
 * This function is used in cases when caller gets raw values from db or
 * syntax and want to parse them.
 * This function uses option_spec_set to get information about how each option
 * should be parsed.
 * If validate mode is off, function found an option that do not have proper
 * option_spec_set entry, this option kept unparsed (if some garbage came from
 * the DB, we should put it back there)
 *
 * This function destroys incoming list.
 */
List *
optionsParseRawValues(List *raw_values, options_spec_set * spec_set,
					  options_parse_mode mode)
{
	ListCell   *cell;
	List	   *result = NIL;
	bool	   *is_set;
	int			i;
	bool		validate = mode & OPTIONS_PARSE_MODE_VALIDATE;
	bool		for_alter = mode & OPTIONS_PARSE_MODE_FOR_ALTER;


	is_set = palloc0(sizeof(bool) * spec_set->num);
	foreach(cell, raw_values)
	{
		option_value *option = (option_value *) lfirst(cell);
		bool		found = false;
		bool		skip = false;


		if (option->status == OPTION_VALUE_STATUS_PARSED)
		{
			/*
			 * This can happen while ALTER, when new values were already
			 * parsed, but old values merged from DB are still raw
			 */
			result = lappend(result, option);
			continue;
		}
		if (validate && option->namespace && (!spec_set->namespace ||
				  strcmp(spec_set->namespace, option->namespace) != 0))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized parameter namespace \"%s\"",
							option->namespace)));
		}

		for (i = 0; i < spec_set->num; i++)
		{
			option_spec_basic *definition = spec_set->definitions[i];

			if (strcmp(option->raw_name,
							  definition->name) == 0)
			{
				/*
				 * Skip option with "ignore" flag, as it is processed
				 * somewhere else. (WITH OIDS special case)
				 */
				if (definition->flags & OPTION_DEFINITION_FLAG_IGNORE)
				{
					found = true;
					skip = true;
					break;
				}

				/*
				 * Reject option as if it was not in spec_set. Needed for cases
				 * when option should have default value, but should not be
				 * changed
				 */
				if (definition->flags & OPTION_DEFINITION_FLAG_REJECT)
				{
					found = false;
					break;
				}

				if (validate && is_set[i])
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						  errmsg("parameter \"%s\" specified more than once",
								 option->raw_name)));
				}
				if ((for_alter) &&
					(definition->flags & OPTION_DEFINITION_FLAG_FORBID_ALTER))
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						   errmsg("changing parameter \"%s\" is not allowed",
								  definition->name)));
				}
				if (option->status == OPTION_VALUE_STATUS_FOR_RESET)
				{
					/*
					 * For RESET options do not need further processing so
					 * mark it found and stop searching
					 */
					found = true;
					break;
				}
				pfree(option->raw_name);
				option->raw_name = NULL;
				option->gen = definition;
				parse_one_option(option, NULL, -1, validate);
				is_set[i] = true;
				found = true;
				break;
			}
		}
		if (!found)
		{
			if (validate)
			{
				if (option->namespace)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized parameter \"%s.%s\"",
									option->namespace, option->raw_name)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized parameter \"%s\"",
									option->raw_name)));
			} else
			{
				/* RESET is always in non-validating mode, unkown names should
				 * be ignored. This is traditional behaviour of postgres/
				 * FIXME may be it should be changed someday
				 */
				if (option->status == OPTION_VALUE_STATUS_FOR_RESET)
				{
					skip = true;
				}
			}
			/*
			 * In other cases, if we are parsing not in validate mode, then
			 * we should keep unknown node, because non-validate mode is for
			 * data that is already in the DB and should not be changed after
			 * altering another entries
			 */
		}
		if (!skip)
			result = lappend(result, option);
	}
	return result;
}

/*
 * parse_one_option
 *
 *		Subroutine for optionsParseRawValues, to parse and validate a
 *		single option's value
 */
static void
parse_one_option(option_value * option, const char *text_str, int text_len,
				 bool validate)
{
	char	   *value;
	bool		parsed;

	value = option->raw_value;

	switch (option->gen->type)
	{
		case OPTION_TYPE_BOOL:
			{
				parsed = parse_bool(value, &option->values.bool_val);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid value for boolean option \"%s\": %s",
							   option->gen->name, value)));
			}
			break;
		case OPTION_TYPE_INT:
			{
				option_spec_int *optint =
				(option_spec_int *) option->gen;

				parsed = parse_int(value, &option->values.int_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid value for integer option \"%s\": %s",
							   option->gen->name, value)));
				if (validate && (option->values.int_val < optint->min ||
								 option->values.int_val > optint->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						   errmsg("value %s out of bounds for option \"%s\"",
								  value, option->gen->name),
					 errdetail("Valid values are between \"%d\" and \"%d\".",
							   optint->min, optint->max)));
			}
			break;
		case OPTION_TYPE_REAL:
			{
				option_spec_real *optreal =
				(option_spec_real *) option->gen;

				parsed = parse_real(value, &option->values.real_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for floating point option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.real_val < optreal->min ||
								 option->values.real_val > optreal->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						   errmsg("value %s out of bounds for option \"%s\"",
								  value, option->gen->name),
					 errdetail("Valid values are between \"%f\" and \"%f\".",
							   optreal->min, optreal->max)));
			}
			break;
		case OPTION_TYPE_ENUM:
			{
				option_spec_enum *optenum =
										(option_spec_enum *) option->gen;
				opt_enum_elt_def *elt;
				parsed = false;
				for (elt = optenum->members; elt->string_val; elt++)
				{
					if (strcmp(value, elt->string_val) == 0)
					{
						option->values.enum_val = elt->symbol_val;
						parsed = true;
						break;
					}
				}
				if (!parsed)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for enum option \"%s\": %s",
									option->gen->name, value),
							 optenum->detailmsg ?
							 errdetail_internal("%s", _(optenum->detailmsg)) : 0));
				}
			}
			break;
		case OPTION_TYPE_STRING:
			{
				option_spec_string *optstring =
				(option_spec_string *) option->gen;

				option->values.string_val = value;
				if (validate && optstring->validate_cb)
					(optstring->validate_cb) (value);
				parsed = true;
			}
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", option->gen->type);
			parsed = true;		/* quiet compiler */
			break;
	}

	if (parsed)
		option->status = OPTION_VALUE_STATUS_PARSED;

}

/*
 * optionsAllocateBytea
 *		Allocates memory for bytea options representation
 *
 * Function allocates memory for byrea structure of an option, plus adds space
 * for values of string options. We should keep all data including string
 * values in the same memory chunk, because Cache code copies bytea option
 * data from one MemoryConext to another without knowing about it's internal
 * structure, so it would not be able to copy string values if they are outside
 * of bytea memory chunk.
 */
static void *
optionsAllocateBytea(options_spec_set * spec_set, List *options)
{
	Size		size;
	int			i;
	ListCell   *cell;
	int			length;
	void	   *res;

	size = spec_set->struct_size;

	/* Calculate size needed to store all string values for this option */
	for (i = 0; i < spec_set->num; i++)
	{
		option_spec_basic *definition = spec_set->definitions[i];
		bool		found = false;
		option_value *option;

		/* Not interested in non-string options, skipping */
		if (definition->type != OPTION_TYPE_STRING)
			continue;

		/*
		 * Trying to find option_value that references definition spec_set
		 * entry
		 */
		foreach(cell, options)
		{
			option = (option_value *) lfirst(cell);
			if (option->status == OPTION_VALUE_STATUS_PARSED &&
				strcmp(option->gen->name, definition->name) == 0)
			{
				found = true;
				break;
			}
		}
		if (found)
			/* If found, it'value will be stored */
			length = strlen(option->values.string_val) + 1;
		else
			/* If not found, then there would be default value there */
		if (((option_spec_string *) definition)->default_val)
			length = strlen(
				 ((option_spec_string *) definition)->default_val) + 1;
		else
			length = 0;
		/* Add total length of all string values to basic size */
		size += length;
	}

	res = palloc0(size);
	SET_VARSIZE(res, size);
	return res;
}

/*
 * optionsValuesToBytea
 *		Converts options from List of option_values to binary bytea structure
 *
 * Convertation goes according to options_spec_set: each spec_set item
 * has offset value, and option value in binary mode is written to the
 * structure with that offset.
 *
 * More special case is string values. Memory for bytea structure is allocated
 * by optionsAllocateBytea which adds some more space for string values to
 * the size of original structure. All string values are copied there and
 * inside the bytea structure an offset to that value is kept.
 *
 */
static bytea *
optionsValuesToBytea(List *options, options_spec_set * spec_set)
{
	char	   *data;
	char	   *string_values_buffer;
	int			i;

	data = optionsAllocateBytea(spec_set, options);

	/* place for string data starts right after original structure */
	string_values_buffer = data + spec_set->struct_size;

	for (i = 0; i < spec_set->num; i++)
	{
		option_value *found = NULL;
		ListCell   *cell;
		char	   *item_pos;
		option_spec_basic *definition = spec_set->definitions[i];

		if (definition->flags & OPTION_DEFINITION_FLAG_IGNORE)
			continue;

		/* Calculate the position of the item inside the structure */
		item_pos = data + definition->struct_offset;

		/* Looking for the corresponding option from options list */
		foreach(cell, options)
		{
			option_value *option = (option_value *) lfirst(cell);

			if (option->status == OPTION_VALUE_STATUS_RAW)
				continue;		/* raw can come from db. Just ignore them then */
			Assert(option->status != OPTION_VALUE_STATUS_EMPTY);

			if (strcmp(definition->name, option->gen->name) == 0)
			{
				found = option;
				break;
			}
		}
		/* writing to the proper position either option value or default val */
		switch (definition->type)
		{
			case OPTION_TYPE_BOOL:
				*(bool *) item_pos = found ?
					found->values.bool_val :
					((option_spec_bool *) definition)->default_val;
				break;
			case OPTION_TYPE_INT:
				*(int *) item_pos = found ?
					found->values.int_val :
					((option_spec_int *) definition)->default_val;
				break;
			case OPTION_TYPE_REAL:
				*(double *) item_pos = found ?
					found->values.real_val :
					((option_spec_real *) definition)->default_val;
				break;
			case OPTION_TYPE_ENUM:
				*(int *) item_pos = found ?
					found->values.enum_val :
					((option_spec_enum *) definition)->default_val;
				break;

			case OPTION_TYPE_STRING:
				{
					/*
					 * For string options: writing string value at the string
					 * buffer after the structure, and storing and offset to
					 * that value
					 */
					char	   *value = NULL;

					if (found)
						value = found->values.string_val;
					else
						value = ((option_spec_string *) definition)
							->default_val;
					*(int *) item_pos = value ?
						string_values_buffer - data :
						OPTION_STRING_VALUE_NOT_SET_OFFSET;
					if (value)
					{
						strcpy(string_values_buffer, value);
						string_values_buffer += strlen(value) + 1;
					}
				}
				break;
			default:
				elog(ERROR, "unsupported reloption type %d",
					 definition->type);
				break;
		}
	}
	return (void *) data;
}


/*
 * transformOptions
 *		This function is used by src/backend/commands/Xxxx in order to process
 *		new option values, merge them with existing values (in the case of
 *		ALTER command) and prepare to put them [back] into DB
 */

Datum
transformOptions(options_spec_set * spec_set, Datum oldOptions,
				 List *defList, options_parse_mode parse_mode)
{
	Datum		result;
	List	   *new_values;
	List	   *old_values;
	List	   *merged_values;

	/*
	 * Parse and validate New values
	 */
	new_values = optionsDefListToRawValues(defList, parse_mode);
	if (! (parse_mode & OPTIONS_PARSE_MODE_FOR_RESET))
	{
		/* FIXME: postgres usual behaviour vas not to vaidate names that
		 * came from RESET command. Once this behavious should be changed,
		 * I guess. But for now we keep it as it was.
		 */
		parse_mode|= OPTIONS_PARSE_MODE_VALIDATE;
	}
	new_values = optionsParseRawValues(new_values, spec_set, parse_mode);

	/*
	 * Old values exists in case of ALTER commands. Transform them to raw
	 * values and merge them with new_values, and parse it.
	 */
	if (PointerIsValid(DatumGetPointer(oldOptions)))
	{
		old_values = optionsTextArrayToRawValues(oldOptions);
		merged_values = optionsMergeOptionValues(old_values, new_values);

		/*
		 * Parse options only after merging in order not to parse options that
		 * would be removed by merging later
		 */
		merged_values = optionsParseRawValues(merged_values, spec_set, 0);
	}
	else
	{
		merged_values = new_values;
	}

	/*
	 * If we have postprocess_fun function defined in spec_set, then there
	 * might be some custom options checks there, with error throwing. So we
	 * should do it here to throw these errors while CREATing or ALTERing
	 * options
	 */
	if (spec_set->postprocess_fun)
	{
		bytea	   *data = optionsValuesToBytea(merged_values, spec_set);

		spec_set->postprocess_fun(data, true);
		pfree(data);
	}

	/*
	 * Convert options to TextArray format so caller can store them into
	 * database
	 */
	result = optionsValuesToTextArray(merged_values);
	return result;
}


/*
 * optionsTextArrayToBytea
 *		A meta-function that transforms options stored as TextArray into binary
 *		(bytea) representation.
 *
 *	This function runs other transform functions that leads to the desired
 *	result in no-validation mode. This function is used by cache mechanism,
 *	in order to load and cache options when object itself is loaded and cached
 */
bytea *
optionsTextArrayToBytea(options_spec_set * spec_set, Datum data, bool validate)
{
	List	   *values;
	bytea	   *options;

	values = optionsTextArrayToRawValues(data);
	values = optionsParseRawValues(values, spec_set,
								validate ? OPTIONS_PARSE_MODE_VALIDATE : 0);
	options = optionsValuesToBytea(values, spec_set);

	if (spec_set->postprocess_fun)
	{
		spec_set->postprocess_fun(options, false);
	}
	return options;
}
