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
 * caller.  Failure is reported with PLy_elog(ERROR), so the result is
 * never NULL.
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

	if (rv == NULL)
		PLy_elog(ERROR, "could not create bytes representation of Python object");

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
	char	   *s = PyBytes_AsString(o);
	char	   *rv;

	if (s == NULL)
	{
		Py_DECREF(o);
		PLy_elog(ERROR, "could not extract bytes from encoded string");
	}

	rv = pstrdup(s);
	Py_DECREF(o);
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
 * Common part of PLyDict_SetItemString() and PLyDict_SetItemNone().
 */
static void
PLyDict_SetItem_v(PyObject *dict, const char *key, PyObject *value,
				  const char *fmt, va_list ap)
{
	if (value != NULL)
	{
		int			rc = PyDict_SetItemString(dict, key, value);

		/* PyDict_SetItemString does not steal the reference */
		Py_DECREF(value);

		if (rc == 0)
			return;
	}

	PLy_elog_impl_v(ERROR, fmt, ap);
	pg_unreachable();
}

/*
 * Insert value into dict under key, consuming the caller's reference.
 *
 * value is a new reference owned by the caller, or NULL with a Python
 * exception set. Py_None is not special here: the caller must own a
 * reference to it, or use PLyDict_SetItemNone(). A failed conversion and a
 * failed insert are both reported under fmt.
 */
void
PLyDict_SetItemString(PyObject *dict, const char *key, PyObject *value,
					  const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);
	PLyDict_SetItem_v(dict, key, value, fmt, ap);
	va_end(ap);
}

/*
 * Insert the borrowed Py_None singleton into dict under key.
 */
void
PLyDict_SetItemNone(PyObject *dict, const char *key, const char *fmt, ...)
{
	va_list		ap;

	Py_INCREF(Py_None);

	va_start(ap, fmt);
	PLyDict_SetItem_v(dict, key, Py_None, fmt, ap);
	va_end(ap);
}

/*
 * Store value at index i of list, consuming the caller's reference.
 *
 * value is a new reference owned by the caller, or NULL with a Python
 * exception set. Py_None is not special here: the caller must own a
 * reference to it. listobject.h says PyList_SetItem decrements the new
 * reference even when the item is not inserted, so a failed store must not
 * release it again. A failed conversion and a failed store are both
 * reported under fmt.
 */
void
PLyList_SetItem(PyObject *list, Py_ssize_t i, PyObject *value,
				const char *fmt, ...)
{
	va_list		ap;

	if (value != NULL && PyList_SetItem(list, i, value) == 0)
		return;

	va_start(ap, fmt);
	PLy_elog_impl_v(ERROR, fmt, ap);
	va_end(ap);
	pg_unreachable();
}
