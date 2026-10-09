/*-------------------------------------------------------------------------
 *
 * config_json.h
 *	  JSON configuration file support
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/config_json.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONFIG_JSON_H
#define CONFIG_JSON_H

#include "nodes/pg_list.h"
#include "utils/guc.h"

typedef enum JsonConfNodeType
{
	JCN_OBJECT,
	JCN_ARRAY,
	JCN_STRING,
	JCN_NUMBER,
	JCN_BOOL,
	JCN_NULL,
} JsonConfNodeType;

/*
 * One node of a parsed JSON configuration document.  Objects keep their
 * keys in document order (keys and values are parallel lists), arrays keep
 * element order.  Every node remembers where it came from, so that errors
 * found long after parsing can still point at a file and line.
 */
typedef struct JsonConfNode
{
	JsonConfNodeType type;
	const char *file;			/* absolute path of the source file */
	int			line;			/* 1-based line of the value */
	char	   *strval;			/* JCN_STRING: text; JCN_NUMBER: raw lexeme */
	bool		boolval;		/* JCN_BOOL */
	List	   *keys;			/* JCN_OBJECT: List of char * */
	List	   *values;			/* JCN_OBJECT, JCN_ARRAY: List of JsonConfNode * */
	struct JsonConfNode *parent;
	const char *key;			/* key in parent object, or NULL */
	int			index;			/* index in parent array, or -1 */
} JsonConfNode;

extern bool config_is_json(const char *filename);

extern const char *config_json_auth_source(const char *filename);
extern bool config_json_load_section(const char *section,
									 const char *filename, int elevel,
									 List **documents_p, char **err_msg);

extern JsonConfNode *config_json_get(JsonConfNode *obj, const char *key);
extern char *config_json_path(JsonConfNode *node);
extern int	config_json_errcontext(JsonConfNode *node);
extern char *config_json_error_text(JsonConfNode *node, const char *msg);
extern const char *config_json_type_name(JsonConfNodeType type);
extern char *config_json_scalar_text(JsonConfNode *node);

#endif							/* CONFIG_JSON_H */
