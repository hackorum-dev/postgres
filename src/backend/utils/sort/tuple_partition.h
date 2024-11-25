/*
 * Based on psuedocode from https://orlp.net/blog/branchless-lomuto-partitioning/
 *
 * There is deliberately no include guard here.
 */

size_t i = 0;
size_t j = 0;
SortTuple pivot = *pivot_pos;

Assert(n>0);


/* create gap at front */
*pivot_pos = v[0];

while (j < n - 1)
{
	v[j] = v[i];
	j += 1;
	v[i] = v[j];
#ifdef PARTITION_LEFT
	i += !CMP_2WAY(pivot, v[i]);
#else
	i += CMP_2WAY(v[i], pivot);
#endif
}

v[j] = v[i];
v[i] = pivot;
#ifdef PARTITION_LEFT
	i += !CMP_2WAY(pivot, v[i]);
#else
	i += CMP_2WAY(v[i], pivot);
#endif

/* i is the number of elements in the left partition */
return i;
