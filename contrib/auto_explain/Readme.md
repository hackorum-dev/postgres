Auto_explain module can be use to log plans of long queries.
Using "auto_explain.log_min_duration" parameter it is possible to specify logging threshold.
Performing explain with analyze provides us information not only about query plan and execution time,
but also actual and estimated number of rows for each plan node.
This information can be used for query optimizer tuning: adaptive query optimization (AQO).

Auto_explain provides two AQO modes:
1. Generation multicolumn statistics for clauses with large estimation error.
2. Adjust selectivity for such clauses.

Estimation errors in Postgres are mostly caused by correlation between columns which is not taken in account by
Postgres optimizer, because by default only single-column statistics is collected.
Multicolumn statistics can be only explicitly created by DBA.
Auto_explain makes it possible to automatically create multicolumn statistics for
columns causes with huge estimation errors.
Generation of multicolumn statistics can be ttoggled by setting "auto_explain.add_statistics_threshold" option
which specifies minimal actual/estimated #rows ratio for multicolumn statistics generation.
Once multicolumn statistics is added, you should perform ANALYZE command and then this updated statistics
will be used by query optimizer in all backends.

Unfortunately right now multicolumn statistics
is used only for restriction clauses, not for joins and for joins estimation errors
are most critical. This is why AQO makes it possible to directly adjust estimation of number of rows for particular nodes.

Option "auto_explain.aqo_threshold" specifies minimal actual/estimated #rows ratio
for node selectivity adjustment. When values of this option is greater than zero, AQO stores
correction coefficient for nodes with equal or greater estimation error.
Correction coefficient is stored in small array in element with index floor(log10(estimation)).
When optimizer assigns estimated number of rows, it searches in AQO hash if correction coefficient
is available for this node and if so, adjusts (multiplies) estimation by this coefficient.

As far as we get actual values only for one particular plan, AQO is able to store correction coefficient only for
clauses used in nodes of this plan. After such adjustment this plan most like becomes non-optimal which cause optimizer
to choose other plan. But there may be no information about actually selectivity of nodes of new winner plan.
And actually it can be even less efficient than original plan. So it can take several iterations before
best plan will be actually selected. The more joins query has, the more alternative plans exist (factorial of
number of joins). This why many iteration may be needed and some of them can cause choosing bad execution plan.

Once optimal plan is constructed, you can disable further learning by setting -1 value to "auto_explain.aqo_threshold".
In this case optimizer will still use stored AQO data for estimations adjustment, but doesn't update this
information. Assigning zero value to "auto_explain.aqo_threshold" completely disable AQO optimization.

AQO is collecting information and performs estimation adjustment only within current backend.
Other backends can perform independent query tuning. Once session is closed, all collected AQO information is lost.
But it is possible to save collected AQO information in the file, specified by "auto_explain.aqo_file" option.
AQO information will be saved on backend exit. This file is always overwritten, so if several backends
tries to save AQO data, only data of last one will be kept. So the intended way of AQO usage is that
you perform learning in one backend (specifying positive "auto_explain.aqo_threshold") and then store this result
in some file. All other backends should use "auto_explain.aqo_threshold"=-1 and specify "auto_explain.aqo_file",
which cause them to load this AQO information from this file.

By default AQO takes in account actual constant values in clauses when perform clauses matching.
It increase estimation quality because of data skews: frequency of different values can significantly vary.
But it cause fast growth of AQO hash, because each new constant value produces new entry in the hash.
AQO provides two ways to address this problems.

First of all you can ignore constant values. It can be achieved by setting "on" value to
"auto_explain.aqo_disregard_constants" option. In this case all literals will be treated as parameters with unknown
value.

Also you can limit amount of collected AQO data by setting non zero value to "auto_explain.aqo_limit".
In this case LRU replacement algorithm will be used to restrict size of AQO hash.
