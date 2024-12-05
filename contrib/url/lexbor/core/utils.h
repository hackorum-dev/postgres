/*
 * Copyright (C) 2018 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#ifndef LEXBOR_UTILS_H
#define LEXBOR_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/core/base.h"


#define lexbor_utils_whitespace(onechar, action, logic)                        \
    (onechar action ' '  logic                                                 \
     onechar action '\t' logic                                                 \
     onechar action '\n' logic                                                 \
     onechar action '\f' logic                                                 \
     onechar action '\r')


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_UTILS_H */
