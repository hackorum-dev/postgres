/*-------------------------------------------------------------------------
 *
 * options.h
 *	  Core support for relation and tablespace options (pg_class.reloptions
 *	  and pg_tablespace.spcoptions)
 *
 * Note: the functions dealing with text-array options values declare
 * them as Datum, not ArrayType *, to avoid needing to include array.h
 * into a lot of low-level code.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/options.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OPTIONS_H
#define OPTIONS_H

#include "storage/lock.h"
#include "nodes/pg_list.h"


/* supported option types */
typedef enum option_type
{
	OPTION_TYPE_BOOL,
	OPTION_TYPE_INT,
	OPTION_TYPE_REAL,
	OPTION_TYPE_ENUM,
	OPTION_TYPE_STRING
}	option_type;


typedef enum option_value_status
{
	OPTION_VALUE_STATUS_EMPTY,	/* Option was just initialized */
	OPTION_VALUE_STATUS_RAW,	/* Option just came from syntax analyzer in
								 * has name, and raw (unparsed) value */
	OPTION_VALUE_STATUS_PARSED, /* Option was parsed and has link to catalog
								 * entry and proper value */
	OPTION_VALUE_STATUS_FOR_RESET		/* This option came from ALTER xxx
										 * RESET */
}	option_value_status;

/* flags for reloptinon definition */
typedef enum option_spec_flags
{
	OPTION_DEFINITION_FLAG_FORBID_ALTER = (1 << 0),		/* Altering this option
														 * is forbidden */
	OPTION_DEFINITION_FLAG_IGNORE = (1 << 1),	/* Skip this option while
												 * parsing. Used for WITH OIDS
												 * special case */
	OPTION_DEFINITION_FLAG_REJECT = (1 << 2)	/* Option will be rejected
												 * when comes from syntax
												 * analyzer, but still have
												 * default value and offset */
} option_spec_flags;

/* flags that tells reloption parser how to parse*/
typedef enum options_parse_mode
{
	OPTIONS_PARSE_MODE_VALIDATE = (1 << 0),
	OPTIONS_PARSE_MODE_FOR_ALTER = (1 << 1),
	OPTIONS_PARSE_MODE_FOR_RESET = (1 << 2)
} options_parse_mode;



/*
 * opt_enum_elt_def -- One member of the array of acceptable values
 * of an enum reloption.
 */
typedef struct opt_enum_elt_def
{
	const char *string_val;
	int			symbol_val;
} opt_enum_elt_def;


/* generic structure to store Option Spec information */
typedef struct option_spec_basic
{
	const char *name;			/* must be first (used as list termination
								 * marker) */
	const char *desc;
	LOCKMODE	lockmode;
	option_spec_flags flags;
	option_type type;
	int			struct_offset;	/* offset of the value in Bytea representation */
}	option_spec_basic;


/* reloptions records for specific variable types */
typedef struct option_spec_bool
{
	option_spec_basic base;
	bool		default_val;
}	option_spec_bool;

typedef struct option_spec_int
{
	option_spec_basic base;
	int			default_val;
	int			min;
	int			max;
}	option_spec_int;

typedef struct option_spec_real
{
	option_spec_basic base;
	double		default_val;
	double		min;
	double		max;
}	option_spec_real;

typedef struct option_spec_enum
{
	option_spec_basic base;
	opt_enum_elt_def *members;/* FIXME rewrite. Null terminated array of allowed values for
								 * the option */
	int			default_val;	/* Number of item of allowed_values array */
	const char  *detailmsg;
}	option_spec_enum;

/* validation routines for strings */
typedef void (*validate_string_option) (const char *value);

/*
 * When storing sting reloptions, we shoud deal with special case when
 * option value is not set. For fixed length options, we just copy default
 * option value into the binary structure. For varlen value, there can be
 * "not set" special case, with no default value offered.
 * In this case we will set offset value to -1, so code that use relptions
 * can deal this case. For better readability it was defined as a constant.
 */
#define OPTION_STRING_VALUE_NOT_SET_OFFSET -1

typedef struct option_spec_string
{
	option_spec_basic base;
	validate_string_option validate_cb;
	char	   *default_val;
}	option_spec_string;

typedef void (*postprocess_bytea_options_function) (void *data, bool validate);

typedef struct options_spec_set
{
	option_spec_basic **definitions;
	int			num;			/* Number of spec_set items in use */
	int			num_allocated;	/* Number of spec_set items allocated */
	bool		forbid_realloc; /* If number of items of the spec_set were
								 * strictly set to certain value do no allow
								 * adding more idems */
	Size		struct_size;	/* Size of a structure for options in binary
								 * representation */
	postprocess_bytea_options_function postprocess_fun; /* This function is
														 * called after options
														 * were converted in
														 * Bytea represenation.
														 * Can be used for extra
														 * validation and so on */
	char	   *namespace;		/* spec_set is used for options from this
								 * namespase */
}	options_spec_set;


/* holds an option value parsed or unparsed */
typedef struct option_value
{
	option_spec_basic *gen;
	char	   *namespace;
	option_value_status status;
	char	   *raw_value;		/* allocated separately */
	char	   *raw_name;
	union
	{
		bool		bool_val;
		int			int_val;
		double		real_val;
		int			enum_val;
		char	   *string_val; /* allocated separately */
	}			values;
}	option_value;




/*
 * Options spec_set related functions
 */
extern options_spec_set *allocateOptionsSpecSet(const char *namespace,
								 int size_of_bytea, int num_items_expected);
extern void optionsSpecSetAddBool(options_spec_set * spec_set, const char *name,
				  const char *desc, LOCKMODE lockmode, option_spec_flags flags,
									int struct_offset, bool default_val);
extern void optionsSpecSetAddInt(options_spec_set * spec_set, const char *name,
					const char *desc, LOCKMODE lockmode, option_spec_flags flags,
					int struct_offset, int default_val, int min_val, int max_val);
extern void optionsSpecSetAddReal(options_spec_set * spec_set, const char *name,
		  const char *desc, LOCKMODE lockmode, option_spec_flags flags,
	  int struct_offset, double default_val, double min_val, double max_val);
extern void optionsSpecSetAddEnum(options_spec_set * spec_set,
						  const char *name, const char *desc, LOCKMODE lockmode, option_spec_flags flags,
			int struct_offset, opt_enum_elt_def* members, int default_val, const char *detailmsg);
extern void optionsSpecSetAddString(options_spec_set * spec_set, const char *name,
		  const char *desc, LOCKMODE lockmode, option_spec_flags flags,
int struct_offset, const char *default_val, validate_string_option validator);


/*
 * This macro allows to get string option value from bytea representation.
 * "optstruct" - is a structure that is stored in bytea options representation
 * "member" - member of this structure that has string option value
 * (actually string values are stored in bytea after the structure, and
 * and "member" will contain an offset to this value. This macro do all
 * the math
 */
#define GET_STRING_OPTION(optstruct, member) \
	((optstruct)->member == OPTION_STRING_VALUE_NOT_SET_OFFSET ? NULL : \
	 (char *)(optstruct) + (optstruct)->member)

/*
 * Functions related to option convertation, parsing, manipulation
 * and validation
 */
extern void optionsDefListValdateNamespaces(List *defList,
								char **allowed_namespaces);
extern List *optionsDefListFilterNamespaces(List *defList, const char *namespace);
extern List *optionsTextArrayToDefList(Datum options);
extern Datum optionsDefListToTextArray(List *defList);
/*
 * Meta functions that uses functions above to get options for relations,
 * tablespaces, views and so on
 */

extern bytea *optionsTextArrayToBytea(options_spec_set * spec_set, Datum data,
																bool validate);
extern Datum transformOptions(options_spec_set * spec_set, Datum oldOptions,
				 List *defList, options_parse_mode parse_mode);

#endif   /* OPTIONS_H */
