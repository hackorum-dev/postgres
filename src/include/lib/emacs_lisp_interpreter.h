/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define evalquote[fn;x] = apply[fn;x;NIL]

#define apply[fn;x;a] = \
     [atom[fn] -> [eq[fn;CAR] -> caar[x]; \
                  eq[fn;CDR] -> cdar[x]; \
                  eq[fn;CONS] -> cons[car[x];cadr[x]]; \
                  eq[fn;ATOM] -> atom[car[x]]; \
                  eq[fn;EQ] -> eq[car[x];cadr[x]]; \
                  T -> apply[eval[fn;a];x;a]]; \
     eq[car[fn];LAMBDA] -> eval[caddr[fn];pairlis[cadr[fn];x;a]]; \
     eq[car[fn];LABEL] -> apply[caddr[fn];x;cons[cons[cadr[fn]; \
                               caddr[fn]];a]]]

#define eval[e;a] = [atom[e] -> cdr[assoc[e;a]]; \
     atom[car[e]] -> \
      [eq[car[e],QUOTE] -> cadr[e]; \
      eq[car[e];COND] -> evcon[cdr[e];a]; \
      T -> apply[car[e];evlis[cdr[e];a];a]]; \
      T -> apply[car[e];evlis[cdr[e];a];a]]

#define evcon[c;a] = [eval[caar[c];a] -> eval[cadar[c];a]; \
      T -> evcon[cdr[c];a]]

#define evlis[m;a] = [null[m] -> NIL; \
      T -> cons[eval[car[m];a];evlis[cdr[m];a]]]
