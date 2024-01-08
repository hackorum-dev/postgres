select max(x) from unnest(ARRAY[$1, $2]) x;
