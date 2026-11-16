/*
 * utility functions
 *
 * src/pl/plpython/plpy_util.c
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "plpy_elog.h"
#include "plpy_util.h"

/*
 * Convert a Python unicode object to a Python string/bytes object in
 * PostgreSQL server encoding.  Reference ownership is passed to the
 * caller.
 */
PyObject *
PLyUnicode_Bytes(PyObject *unicode)
{
	PyObject   *bytes,
			   *rv;
	char	   *utf8string,
			   *encoded;

	/* First encode the Python unicode object with UTF-8. */
	bytes = PyUnicode_AsUTF8String(unicode);
	if (bytes == NULL)
		PLy_elog(ERROR, "could not convert Python Unicode object to bytes");

	utf8string = PyBytes_AsString(bytes);
	if (utf8string == NULL)
	{
		Py_DECREF(bytes);
		PLy_elog(ERROR, "could not extract bytes from encoded string");
	}

	/*
	 * Then convert to server encoding if necessary.
	 *
	 * PyUnicode_AsEncodedString could be used to encode the object directly
	 * in the server encoding, but Python doesn't support all the encodings
	 * that PostgreSQL does (EUC_TW). UTF-8 is used as an intermediary in
	 * PLyUnicode_FromString as well.
	 */
	if (GetDatabaseEncoding() != PG_UTF8)
	{
		PG_TRY();
		{
			encoded = pg_any_to_server(utf8string,
									   strlen(utf8string),
									   PG_UTF8);
		}
		PG_CATCH();
		{
			Py_DECREF(bytes);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
		encoded = utf8string;

	/* finally, build a bytes object in the server encoding */
	rv = PyBytes_FromStringAndSize(encoded, strlen(encoded));

	/* if pg_any_to_server allocated memory, free it now */
	if (utf8string != encoded)
		pfree(encoded);

	Py_DECREF(bytes);
	return rv;
}

/*
 * Convert a Python unicode object to a C string in PostgreSQL server
 * encoding.  No Python object reference is passed out of this
 * function.  The result is palloc'ed.
 */
char *
PLyUnicode_AsString(PyObject *unicode)
{
	PyObject   *o = PLyUnicode_Bytes(unicode);
	char	   *rv = pstrdup(PyBytes_AsString(o));

	Py_XDECREF(o);
	return rv;
}

/*
 * Convert a C string in the PostgreSQL server encoding to a Python
 * unicode object.  Reference ownership is passed to the caller.
 */
PyObject *
PLyUnicode_FromStringAndSize(const char *s, Py_ssize_t size)
{
	char	   *utf8string;
	PyObject   *o;

	utf8string = pg_server_to_any(s, size, PG_UTF8);

	if (utf8string == s)
	{
		o = PyUnicode_FromStringAndSize(s, size);
	}
	else
	{
		o = PyUnicode_FromString(utf8string);
		pfree(utf8string);
	}

	return o;
}

PyObject *
PLyUnicode_FromString(const char *s)
{
	return PLyUnicode_FromStringAndSize(s, strlen(s));
}

/*
 * Return obj. NULL means a Python exception is set. Report it with
 * PLy_elog(ERROR) and do not return.
 */
PyObject *
PLy_check(PyObject *obj)
{
	if (obj == NULL)
		PLy_elog(ERROR, NULL);
	return obj;
}

/*
 * Insert value into dict under key.
 *
 * value is a new reference owned by the caller, or NULL with a Python
 * exception set. A new reference is stolen. Py_None is borrowed, and this
 * function takes its own reference. On failure the Python exception is
 * reported with PLy_elog(ERROR).
 */
void
PLy_dict_setitem_string(PyObject *dict, const char *key, PyObject *value)
{
	if (value == NULL)
		PLy_elog(ERROR, NULL);

	if (value == Py_None)
		Py_INCREF(Py_None);

	if (PyDict_SetItemString(dict, key, value) < 0)
	{
		Py_DECREF(value);
		PLy_elog(ERROR, NULL);
	}
	Py_DECREF(value);
}

/*
 * Store value at index i.
 *
 * value is a new reference owned by the caller, or NULL with a Python
 * exception set. A new reference is stolen. Py_None is borrowed, and this
 * function takes its own reference. listobject.h says PyList_SetItem
 * decrements the new reference even when the item is not inserted, so it
 * is not released again here. On failure the Python exception is reported
 * with PLy_elog(ERROR).
 */
void
PLy_list_setitem(PyObject *list, Py_ssize_t i, PyObject *value)
{
	if (value == NULL)
		PLy_elog(ERROR, NULL);

	if (value == Py_None)
		Py_INCREF(Py_None);

	if (PyList_SetItem(list, i, value) < 0)
		PLy_elog(ERROR, NULL);
}
