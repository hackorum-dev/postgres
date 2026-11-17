/*--------------------------
 * common utility functions
 *--------------------------
 */

#ifndef PLPY_UTIL_H
#define PLPY_UTIL_H

#include "plpython.h"

extern PGDLLEXPORT PyObject *PLyUnicode_Bytes(PyObject *unicode);
extern PGDLLEXPORT char *PLyUnicode_AsString(PyObject *unicode);

extern PGDLLEXPORT PyObject *PLyUnicode_FromString(const char *s);
extern PGDLLEXPORT PyObject *PLyUnicode_FromStringAndSize(const char *s, Py_ssize_t size);

extern void PLyDict_SetItemString(PyObject *dict, const char *key,
								  PyObject *value,
								  const char *fmt, ...) pg_attribute_printf(4, 5);
extern void PLyDict_SetItemNone(PyObject *dict, const char *key,
								const char *fmt, ...) pg_attribute_printf(3, 4);
extern void PLyList_SetItem(PyObject *list, Py_ssize_t i, PyObject *value,
							const char *fmt, ...) pg_attribute_printf(4, 5);

#endif							/* PLPY_UTIL_H */
