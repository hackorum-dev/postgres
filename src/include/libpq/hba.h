/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  Interface to hba.c
 *
 *
 * src/include/libpq/hba.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include "libpq/pqcomm.h"		/* needed for NetBSD */
#include "nodes/pg_list.h"
#include "regex/regex.h"


/*
 * The following enum represents the authentication methods that
 * are supported by PostgreSQL.
 *
 * Note: keep this in sync with the UserAuthName array in hba.c.
 */
typedef enum UserAuth
{
	uaReject,
	uaImplicitReject,			/* Not a user-visible option */
	uaTrust,
	uaIdent,
	uaPassword,
	uaMD5,
	uaSCRAM,
	uaGSS,
	uaSSPI,
	uaPAM,
	uaBSD,
	uaLDAP,
	uaCert,
	uaPeer,
	uaOAuth,
#define USER_AUTH_LAST uaOAuth	/* Must be last value of this enum */
} UserAuth;

/*
 * Data structures representing pg_hba.conf entries
 */

typedef enum IPCompareMethod
{
	ipCmpMask,
	ipCmpSameHost,
	ipCmpSameNet,
	ipCmpAll,
} IPCompareMethod;

typedef enum ConnType
{
	ctLocal,
	ctHost,
	ctHostSSL,
	ctHostNoSSL,
	ctHostGSS,
	ctHostNoGSS,
} ConnType;

typedef enum ClientCertMode
{
	clientCertOff,
	clientCertCA,
	clientCertFull,
} ClientCertMode;

typedef enum ClientCertName
{
	clientCertCN,
	clientCertDN,
} ClientCertName;

/*
 * A single string token lexed from an authentication configuration file
 * (pg_ident.conf or pg_hba.conf), together with whether the token has
 * been quoted.  If "string" begins with a slash, it may optionally
 * contain a regular expression (currently used for pg_ident.conf when
 * building IdentLines and for pg_hba.conf when building HbaLines).
 */
typedef struct AuthToken
{
	char	   *string;
	bool		quoted;
	regex_t    *regex;
} AuthToken;

typedef struct HbaLine
{
	char	   *sourcefile;
	int			linenumber;
	char	   *rawline;
	ConnType	conntype;
	List	   *databases;
	List	   *roles;
	struct sockaddr_storage addr;
	int			addrlen;		/* zero if we don't have a valid addr */
	struct sockaddr_storage mask;
	int			masklen;		/* zero if we don't have a valid mask */
	IPCompareMethod ip_cmp_method;
	char	   *hostname;
	UserAuth	auth_method;
	char	   *usermap;
	char	   *pamservice;
	bool		pam_use_hostname;
	bool		ldaptls;
	char	   *ldapscheme;
	char	   *ldapserver;
	int			ldapport;
	char	   *ldapbinddn;
	char	   *ldapbindpasswd;
	char	   *ldapsearchattribute;
	char	   *ldapsearchfilter;
	char	   *ldapbasedn;
	int			ldapscope;
	char	   *ldapprefix;
	char	   *ldapsuffix;
	ClientCertMode clientcert;
	ClientCertName clientcertname;
	char	   *krb_realm;
	bool		include_realm;
	bool		compat_realm;
	bool		upn_username;
	char	   *oauth_issuer;
	char	   *oauth_scope;
	char	   *oauth_validator;
	bool		oauth_skip_usermap;
	List	   *oauth_opt_keys;
	List	   *oauth_opt_vals;
	char	   *node_path;		/* JSON node this rule was compiled from, or
								 * NULL for a legacy line */
} HbaLine;

typedef struct IdentLine
{
	int			linenumber;

	char	   *usermap;
	AuthToken  *system_user;
	AuthToken  *pg_user;
} IdentLine;

typedef struct HostsLine
{
	int			linenumber;

	char	   *sourcefile;
	char	   *rawline;

	/* Required fields */
	List	   *hostnames;
	char	   *ssl_key;
	char	   *ssl_cert;

	/* Optional fields */
	char	   *ssl_ca;
	char	   *ssl_passphrase_cmd;
	bool		ssl_passphrase_reload;

	/* Internal bookkeeping */
	void	   *ssl_ctx;		/* associated SSL_CTX* for the above settings */
} HostsLine;

/*
 * TokenizedAuthLine represents one line lexed from an authentication
 * configuration file.  Each item in the "fields" list is a sub-list of
 * AuthTokens.  We don't emit a TokenizedAuthLine for empty or all-comment
 * lines, so "fields" is never NIL (nor are any of its sub-lists).
 *
 * Exception: if an error occurs during tokenization, we might have
 * fields == NIL, in which case err_msg != NULL.
 */
typedef struct TokenizedAuthLine
{
	List	   *fields;			/* List of lists of AuthTokens */
	char	   *file_name;		/* File name of origin */
	int			line_num;		/* Line number */
	char	   *raw_line;		/* Raw line text */
	char	   *err_msg;		/* Error message if any */
	char	   *node_path;		/* JSON node this rule was compiled from, or
								 * NULL for a legacy line */
} TokenizedAuthLine;

/* avoid including libpq/libpq-be.h here */
typedef struct Port Port;

extern bool load_hba(void);
extern bool load_ident(void);
extern const char *hba_authname(UserAuth auth_method);
extern void hba_getauthmethod(Port *port);
extern int	check_usermap(const char *usermap_name,
						  const char *pg_user, const char *system_user,
						  bool case_insensitive);
extern HbaLine *parse_hba_line(TokenizedAuthLine *tok_line, int elevel);
extern IdentLine *parse_ident_line(TokenizedAuthLine *tok_line, int elevel);
extern FILE *open_auth_file(const char *filename, int elevel, int depth,
							char **err_msg);
extern void free_auth_file(FILE *file, int depth);
extern void tokenize_auth_file(const char *filename, FILE *file,
							   List **tok_lines, int elevel, int depth);
extern void hba_tokenize_context_create(void);
extern void hba_tokenize_context_delete(void);
extern AuthToken *make_auth_token(const char *token, bool quoted);
extern List *hba_expand_auth_file(List *tokens, const char *outer_filename,
								  const char *inc_filename, int elevel,
								  char **err_msg);

/* in hba_json.c */
struct JsonConfNode;
extern bool tokenize_hba_json(List *documents, List **tok_lines, int elevel,
							  char **file_err);
extern bool tokenize_ident_json(List *documents, List **tok_lines, int elevel,
								char **file_err);
extern void free_json_section_tokens(void);
extern bool import_auth_lines(struct JsonConfNode *imports, List **tok_lines,
							  int elevel, char **file_err);
extern bool section_shape_error(struct JsonConfNode *node, int elevel,
								char **file_err, const char *fmt, ...)
			pg_attribute_printf(4, 5);

#endif							/* HBA_H */
