/*-------------------------------------------------------------------------
 *
 * catalog_20.h
 *		Row layouts of catalogs as PostgreSQL 20 wrote them
 *
 * WIP: temporary hack until catalog transformation is implemented
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/upgrade/catalog_20.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_20_H
#define CATALOG_20_H

/* The major version whose row layouts these are */
#define CATALOG_20_MAJOR_VERSION	20

/* pg_database.datconnlimit of a database that is being dropped */
#define DATCONNLIMIT_INVALID_DB_20	(-2)

typedef struct FormData_pg_database_20
{
	Oid			oid;
	NameData	datname;
	Oid			datdba;
	int32		encoding;
	char		datlocprovider;
	bool		datistemplate;
	bool		datallowconn;
	bool		dathasloginevt;
	int32		datconnlimit;
	TransactionId datfrozenxid;
	TransactionId datminmxid;
	Oid			dattablespace;
	/* variable-length fields follow */
} FormData_pg_database_20;

typedef FormData_pg_database_20 *Form_pg_database_20;

#endif							/* CATALOG_20_H */
