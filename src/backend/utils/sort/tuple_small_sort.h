/*
 * There is deliberately no include guard here.
 */

SortTuple	 *pl,
			 *pm;

for (pm = begin + 1; pm < begin + n; pm++)
{
	pl = pm;

	/*
	 * Compare first so we can avoid 2 moves for an element already
	 * positioned correctly.
	 */
	if (CMP_3WAY(pl - 1, pl) > 0)
	{
		SortTuple tmp = *pl;

		do
		{
			*pl = *(pl - 1);
			pl--;
		}
		while (pl > begin && CMP_3WAY(pl - 1, &tmp) > 0);

		*pl = tmp;
	}

}
