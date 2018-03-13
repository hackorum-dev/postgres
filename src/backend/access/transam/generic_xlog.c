/*-------------------------------------------------------------------------
 *
 * generic_xlog.c
 *	 Implementation of generic xlog records.
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/generic_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/generic_xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "utils/memutils.h"

/*-------------------------------------------------------------------------
 * Internally, a delta between pages consists of a set of fragments.  Each
 * fragment represents changes made in a given region of a page.  A fragment
 * is made up as follows:
 *
 * - offset of page region (OffsetNumber)
 * - length of page region (OffsetNumber)
 * - data - the data to place into the region ('length' number of bytes)
 *
 * Unchanged regions of a page are not represented in its delta.  As a result,
 * a delta can be more compact than the full page image.  But having an
 * unchanged region between two fragments that is smaller than the fragment
 * header (offset+length) does not pay off in terms of the overall size of
 * the delta.  For this reason, we merge adjacent fragments if the unchanged
 * region between them is <= MATCH_THRESHOLD bytes.
 *
 * We do not bother to merge fragments across the "lower" and "upper" parts
 * of a page; it's very seldom the case that pd_lower and pd_upper are within
 * MATCH_THRESHOLD bytes of each other, and handling that infrequent case
 * would complicate and slow down the delta-computation code unduly.
 * Therefore, the worst-case delta size includes two fragment headers plus
 * a full page's worth of data.
 *-------------------------------------------------------------------------
 */

#define MAX_ALIGN_MISMATCHES	255
/* MAX_ALIGN_MISMATCHES is not supposed to be greater than PG_UINT8_MAX */
#if MAX_ALIGN_MISMATCHES > PG_UINT8_MAX
#error "MAX_ALIGN_MISMATCHES must be not greater than PG_UINT8_MAX"
#endif

#define FRAGMENT_HEADER_SIZE	(2 * sizeof(OffsetNumber))
#define REGION_HEADER_SIZE		(sizeof(char) + sizeof(int))
#define DIFF_DELTA_HEADER_SIZE	(sizeof(char) + 2 * sizeof(OffsetNumber))
#define MISMATCH_HEADER_SIZE	(sizeof(char) + sizeof(uint8) + \
								 sizeof(OffsetNumber))
#define MATCH_THRESHOLD			FRAGMENT_HEADER_SIZE
#define MAX_DELTA_SIZE			(BLCKSZ + \
								 2 * REGION_HEADER_SIZE + \
								 2 * FRAGMENT_HEADER_SIZE + \
								 2 * DIFF_DELTA_HEADER_SIZE + \
								 MAX_ALIGN_MISMATCHES * MISMATCH_HEADER_SIZE \
								 + sizeof(bool))

#define writeToPtr(ptr, x)		memcpy(ptr, &(x), sizeof(x)), ptr += sizeof(x)
#define readFromPtr(ptr, x)		memcpy(&(x), ptr, sizeof(x)), ptr += sizeof(x)

/* Struct of generic xlog data for single page */
typedef struct
{
	Buffer		buffer;			/* registered buffer */
	int			flags;			/* flags for this buffer */

	int			deltaLen;		/* space consumed in delta field */
	char	   *image;			/* copy of page image for modification, do not
								 * do it in-place to have aligned memory chunk */
	char		delta[MAX_DELTA_SIZE];	/* delta between page images */

	PageXLogCompressParams compressParams;	/* compress parameters */
} PageData;

/* State of generic xlog record construction */
struct GenericXLogState
{
	/*
	 * page's images. Should be first in this struct to have MAXALIGN'ed
	 * images addresses, because some code working with pages directly aligns
	 * addresses, not offsets from beginning of page
	 */
	char		images[MAX_GENERIC_XLOG_PAGES * BLCKSZ];
	PageData	pages[MAX_GENERIC_XLOG_PAGES];
	bool		isLogged;
};

/* Describes for the region which type of delta is used in it */
typedef enum
{
	DIFF_DELTA = 0,				/* diff delta with insert, remove and replace
								 * operations */
	BASE_DELTA = 1				/* base delta with update operations only */
} DeltaType;

/* Diff delta operations for transforming current page to target page */
typedef enum
{
	DIFF_INSERT = 0,
	DIFF_REMOVE = 1,
	DIFF_REPLACE = 2
} DiffDeltaOperations;

/* Describes the kind of region of the page */
typedef enum
{
	UPPER_REGION = 0,
	LOWER_REGION = 1
} RegionType;

static void writeFragment(PageData *pageData, OffsetNumber offset,
			  OffsetNumber len, const char *data);
static void computeRegionDelta(PageData *pageData,
				   const char *curpage, const char *targetpage,
				   int targetStart, int targetEnd,
				   int validStart, int validEnd,
				   uint8 maxMismatches);
static void computeDelta(PageData *pageData, Page curpage, Page targetpage);
static void applyPageRedo(Page page, const char *delta, Size deltaSize);

static int alignRegions(const char *curRegion, const char *targetRegion,
			 int curRegionLen, int targetRegionLen, uint8 maxMismatches);
static int restoreCompactAlignment(const char *curRegion,
						const char *targetRegion,
						int curRegionLen,
						int targetRegionLen,
						int numMismatches);

static bool computeRegionDiffDelta(PageData *pageData,
					   const char *curpage, const char *targetpage,
					   int targetStart, int targetEnd,
					   int validStart, int validEnd,
					   uint8 maxMismatches);
static const char *applyPageDiffRedo(Page page, const char *delta, Size deltaSize);

static void computeRegionBaseDelta(PageData *pageData,
					   const char *curpage, const char *targetpage,
					   int targetStart, int targetEnd,
					   int validStart, int validEnd);
static const char *applyPageBaseRedo(Page page, const char *delta, Size deltaSize);

static bool pageDataContainsDiffDelta(PageData *pageData);
static void downgradeDeltaToBaseFormat(PageData *pageData);

/* Arrays for the alignment building and for the resulting alignments */
static int	V[MAX_ALIGN_MISMATCHES + 1][2 * MAX_ALIGN_MISMATCHES + 1];
static int	prevDiag[MAX_ALIGN_MISMATCHES + 1][2 * MAX_ALIGN_MISMATCHES + 1];
static int	alignmentDiag[MAX_ALIGN_MISMATCHES + 1];
static char curRegionAligned[MAX_ALIGN_MISMATCHES];
static bool curRegionAlignedGap[MAX_ALIGN_MISMATCHES];
static char targetRegionAligned[MAX_ALIGN_MISMATCHES];
static bool targetRegionAlignedGap[MAX_ALIGN_MISMATCHES];
static int	curRegionAlignedPos[MAX_ALIGN_MISMATCHES];
static int	targetRegionAlignedPos[MAX_ALIGN_MISMATCHES];

/* Array for diff delta application */
static char localPage[BLCKSZ];


/*
 * Write next fragment into pageData's delta.
 *
 * The fragment has the given offset and length, and data points to the
 * actual data (of length length).
 */
static void
writeFragment(PageData *pageData, OffsetNumber offset, OffsetNumber length,
			  const char *data)
{
	char	   *ptr = pageData->delta + pageData->deltaLen;

	/* Verify we have enough space */
	Assert(pageData->deltaLen + sizeof(offset) +
		   sizeof(length) + length <= sizeof(pageData->delta));

	/* Write fragment data */
	memcpy(ptr, &offset, sizeof(offset));
	ptr += sizeof(offset);
	memcpy(ptr, &length, sizeof(length));
	ptr += sizeof(length);
	memcpy(ptr, data, length);
	ptr += length;

	pageData->deltaLen = ptr - pageData->delta;
}

/*
 * Compute the XLOG fragments needed to transform a region of curpage into the
 * corresponding region of targetpage, and append them to pageData's delta
 * field.  The region to transform runs from targetStart to targetEnd-1.
 * Bytes in curpage outside the range validStart to validEnd-1 should be
 * considered invalid, and always overwritten with target data.
 *
 * If forceBaseDelta is true, fucntion just calls computeRegionBaseDelta.
 * Otherwise this function tries to build diff delta first and, if it fails,
 * uses the base delta. It also provides the header before the delta in which
 * the type and the length of the delta are contained.
 */
static void
computeRegionDelta(PageData *pageData,
				   const char *curpage, const char *targetpage,
				   int targetStart, int targetEnd,
				   int validStart, int validEnd,
				   uint8 maxMismatches)
{
	int			length;
	char		header;
	int			prevDeltaLen;
	bool		diff = false;
	char	   *ptr = pageData->delta + pageData->deltaLen;

	/* Verify we have enough space */
	Assert(pageData->deltaLen + sizeof(header) +
		   sizeof(length) <= MAX_DELTA_SIZE);

	pageData->deltaLen += sizeof(header) + sizeof(length);
	prevDeltaLen = pageData->deltaLen;

	/* Not sure what to do with too big maxMismathes. Now we just clip it. */
	if (maxMismatches > MAX_ALIGN_MISMATCHES)
		maxMismatches = MAX_ALIGN_MISMATCHES;

	/* Try building diff delta only if necessary */
	if (maxMismatches > 0)
	{
		diff = computeRegionDiffDelta(pageData,
									  curpage, targetpage,
									  targetStart, targetEnd,
									  validStart, validEnd,
									  maxMismatches);
	}

	/*
	 * If we succeeded to make diff delta, just set the header. Otherwise,
	 * make base delta.
	 */
	if (diff)
	{
		header = DIFF_DELTA;
	}
	else
	{
		header = BASE_DELTA;
		computeRegionBaseDelta(pageData,
							   curpage, targetpage,
							   targetStart, targetEnd,
							   validStart, validEnd);
	}
	length = pageData->deltaLen - prevDeltaLen;

	writeToPtr(ptr, header);
	writeToPtr(ptr, length);
}

/*
 * Compute the XLOG fragments needed to transform a region of curpage into the
 * corresponding region of targetpage, and append them to pageData's delta
 * field. The region to transform runs from targetStart to targetEnd-1.
 * Bytes in curpage outside the range validStart to validEnd-1 should be
 * considered invalid, and always overwritten with target data.
 *
 * This function is a hot spot, so it's worth being as tense as possible
 * about the data-matching loops.
 */
static void
computeRegionBaseDelta(PageData *pageData,
					   const char *curpage, const char *targetpage,
					   int targetStart, int targetEnd,
					   int validStart, int validEnd)
{
	int			i,
				loopEnd,
				fragmentBegin = -1,
				fragmentEnd = -1;

	/* Deal with any invalid start region by including it in first fragment */
	if (validStart > targetStart)
	{
		fragmentBegin = targetStart;
		targetStart = validStart;
	}

	/* We'll deal with any invalid end region after the main loop */
	loopEnd = Min(targetEnd, validEnd);

	/* Examine all the potentially matchable bytes */
	i = targetStart;
	while (i < loopEnd)
	{
		if (curpage[i] != targetpage[i])
		{
			/* On unmatched byte, start new fragment if not already in one */
			if (fragmentBegin < 0)
				fragmentBegin = i;
			/* Mark unmatched-data endpoint as uncertain */
			fragmentEnd = -1;
			/* Extend the fragment as far as possible in a tight loop */
			i++;
			while (i < loopEnd && curpage[i] != targetpage[i])
				i++;
			if (i >= loopEnd)
				break;
		}

		/* Found a matched byte, so remember end of unmatched fragment */
		fragmentEnd = i;

		/*
		 * Extend the match as far as possible in a tight loop.  (On typical
		 * workloads, this inner loop is the bulk of this function's runtime.)
		 */
		i++;
		while (i < loopEnd && curpage[i] == targetpage[i])
			i++;

		/*
		 * There are several possible cases at this point:
		 *
		 * 1. We have no unwritten fragment (fragmentBegin < 0).  There's
		 * nothing to write; and it doesn't matter what fragmentEnd is.
		 *
		 * 2. We found more than MATCH_THRESHOLD consecutive matching bytes.
		 * Dump out the unwritten fragment, stopping at fragmentEnd.
		 *
		 * 3. The match extends to loopEnd.  We'll do nothing here, exit the
		 * loop, and then dump the unwritten fragment, after merging it with
		 * the invalid end region if any.  If we don't so merge, fragmentEnd
		 * establishes how much the final writeFragment call needs to write.
		 *
		 * 4. We found an unmatched byte before loopEnd.  The loop will repeat
		 * and will enter the unmatched-byte stanza above.  So in this case
		 * also, it doesn't matter what fragmentEnd is.  The matched bytes
		 * will get merged into the continuing unmatched fragment.
		 *
		 * Only in case 3 do we reach the bottom of the loop with a meaningful
		 * fragmentEnd value, which is why it's OK that we unconditionally
		 * assign "fragmentEnd = i" above.
		 */
		if (fragmentBegin >= 0 && i - fragmentEnd > MATCH_THRESHOLD)
		{
			writeFragment(pageData, fragmentBegin,
						  fragmentEnd - fragmentBegin,
						  targetpage + fragmentBegin);
			fragmentBegin = -1;
			fragmentEnd = -1;	/* not really necessary */
		}
	}

	/* Deal with any invalid end region by including it in final fragment */
	if (loopEnd < targetEnd)
	{
		if (fragmentBegin < 0)
			fragmentBegin = loopEnd;
		fragmentEnd = targetEnd;
	}

	/* Write final fragment if any */
	if (fragmentBegin >= 0)
	{
		if (fragmentEnd < 0)
			fragmentEnd = targetEnd;
		writeFragment(pageData, fragmentBegin,
					  fragmentEnd - fragmentBegin,
					  targetpage + fragmentBegin);
	}
}

/*
 * Align curRegion and targetRegion and return the number of mismatches
 * or -1 if the alignment with number of mismatching positions less than
 * or equal to maxMismatches does not exist.
 * The algorithm guarantees to find the alignment with the least possible
 * number of mismathing positions or return that such least number is greater
 * than maxMismatches.
 *
 * For a good introduction to the subject, read about the "Levenshtein
 * distance" in Wikipedia.
 *
 * The basic algorithm is described in:
 * "An O(ND) Difference Algorithm and its Variations", Eugene W. Myers,
 * Algorithmica Vol. 1, 1986, pp. 251-266,
 * <http://dx.doi.org/10.1007/BF01840446>,
 * PDF: <http://mail.xmailserver.net/diff2.pdf>.
 * See especially section 3, which describes the variation used below.
 *
 * This variation requires O(N + D ^ 2) memory and has time complexity O(ND).
 * We choose it because it is faster than described in section 4.2 modification
 * with O(N + D) memory requirement.
 *
 * The only modification we made to the original algorithm is the introduction
 * of REPLACE operations, while in the original algorithm only INSERT and
 * REMOVE are considered. This introduction doesn't affect time and memory
 * complexity of the algorithm.
 */
static int
alignRegions(const char *curRegion, const char *targetRegion,
			 int curRegionLen, int targetRegionLen,
			 uint8 maxMismatches)
{
	/* Number of mismatches */
	int			m;

	/* Difference between curRegion and targetRegion prefix lengths */
	int			k;

	/* Curbuf and targetRegion prefix lengths */
	int			i,
				j;

	/* Number of mismathes in the answer */
	int			numMismatches = -1;

	/*
	 * If lengths differ too much, there is no alignment with a small number
	 * of mismatches.
	 */
	if (!(-maxMismatches < curRegionLen - targetRegionLen &&
		  curRegionLen - targetRegionLen < maxMismatches))
		return -1;

	/*
	 * V is an array to store the values of dynamic programming. The first
	 * dimension corresponds to m, i. e. to the number of performed editing
	 * operations, and the second one is for k + m, where k is the number of a
	 * diagonal. A diagonal numbered k is such points (i, j) where i - j = k.
	 * Here i means the length of curRegion prefix and j means the length of
	 * targetRegion prefix. V[m][m + k] is the length of the longest prefix of
	 * curRegion i which can be aligned with the prefix of length j of
	 * targetRegion using m editing operations. In the loop below we
	 * initialize V[0][0] and then compute V[m][m + k] based on, if defined,
	 * V[m - 1][m + k - 1], V[m - 1][m + k], and V[m - 1][m + k + 1]. V[m][m +
	 * k] is undefined if k < -m or k > m.
	 */
	V[0][0] = 0;

	/* Find the longest path with the given number of mismatches */
	for (m = 0; m <= maxMismatches; ++m)
	{
		/*
		 * Find the largest prefix alignment with the given number of
		 * mismatches and the given diagonal, i. e. difference between
		 * curRegion and targetRegion prefix lengths.
		 */
		for (k = -m; k <= m; ++k)
		{
			/* Dynamic programming recurrent step */
			if (m > 0)
			{
				i = -1;
				if (k != -m && k != m &&
					V[m - 1][m - 1 + k] + 1 > i)
				{
					i = V[m - 1][m - 1 + k] + 1;
					prevDiag[m][m + k] = k;
				}
				if (k != -m && k != -m + 1 &&
					V[m - 1][m - 1 + k - 1] + 1 > i)
				{
					i = V[m - 1][m - 1 + k - 1] + 1;
					prevDiag[m][m + k] = k - 1;
				}
				if (k != m && k != m - 1 &&
					V[m - 1][m - 1 + k + 1] > i)
				{
					i = V[m - 1][m - 1 + k + 1];
					prevDiag[m][m + k] = k + 1;
				}
			}
			else
				i = 0;
			j = i - k;

			/* Boundary checks */
			Assert(i >= 0);
			Assert(j >= 0);

			/* Increase the prefixes while the bytes are equal */
			while (i < curRegionLen && j < targetRegionLen &&
				   curRegion[i] == targetRegion[j])
				i++, j++;

			/*
			 * Save the largest curRegion prefix that was aligned with given
			 * number of mismatches and difference between curRegion and
			 * targetRegion prefix lengths.
			 */
			V[m][m + k] = i;

			/* If we find the alignment, save its length and break */
			if (i == curRegionLen && j == targetRegionLen)
			{
				numMismatches = m;
				break;
			}
		}
		/* Break if we find an alignment */
		if (numMismatches != -1)
			break;
	}
	/* No alignment was found */
	if (numMismatches == -1)
		return -1;

	/*
	 * Restore the path k-s for each iteration of the main loop for the found
	 * alignment.
	 */
	Assert(m >= 0);
	while (m != 0)
	{
		Assert(-m <= k && k <= m);
		alignmentDiag[m] = k;
		k = prevDiag[m][m + k];
		m--;
	}
	Assert(k == 0);
	alignmentDiag[0] = 0;

	return numMismatches;
}

/*
 * Restore the mismathcing parts of the alignment based on V, alignmentDiag,
 * and numMismacthes. Do some alignment assertions also.
 *
 * The compressed alignment is stored in curRegionAligned, targetRegionAligned,
 * curRegionAlignedGap, targetRegionAlignedGap, curRegionAlignedPos, and
 * targetRegionAlignedPos. The first two arrays contain the aligned data,
 * the second two contain the map of align gaps in the first two arrays,
 * and the last two arrays contain the positions of the aligned parts in
 * the original arrays.
 */
int
restoreCompactAlignment(const char *curRegion, const char *targetRegion,
						int curRegionLen, int targetRegionLen,
						int numMismatches)
{
	int			i,
				j,
				k,
				m;

	/* The length of the equal block */
	int			curLen = 0;

	/* Result alignment length */
	int			resLen = 0;

	/* Maximal possible result alignment length */
	int			maxResLen = Min(curRegionLen, targetRegionLen) + numMismatches;

	/* Keep compiler quiet */
	if (curLen != 0)
		curLen = maxResLen;

	/* Check whether the first equal block is computed correctly */
	curLen = V[0][0];
	Assert(curLen >= 0);
	Assert(resLen + curLen <= maxResLen);
	Assert(memcmp(curRegion, targetRegion, curLen) == 0);

	/* Restore the alignment */
	for (m = 1; m <= numMismatches; ++m)
	{
		/* Initialize the variables for the block */
		int			dk = alignmentDiag[m] - alignmentDiag[m - 1];
		int			prevDiag = alignmentDiag[m - 1];

		k = alignmentDiag[m];
		i = V[m - 1][m - 1 + prevDiag];
		j = i - prevDiag;
		/* Check state consistency */
		Assert(0 <= i && i <= curRegionLen);
		Assert(0 <= j && j <= targetRegionLen);

		/* Check the block operation correctness */
		Assert(dk == -1 || i < curRegionLen);
		Assert(dk == 1 || j < targetRegionLen);
		Assert(-1 <= dk && dk <= 1);
		Assert(resLen + 1 <= maxResLen);

		/* Do the alignment operation of the block */
		curRegionAlignedPos[resLen] = i;
		targetRegionAlignedPos[resLen] = j;
		if (dk == 1 || dk == 0)
		{
			curRegionAlignedGap[resLen] = false;
			curRegionAligned[resLen] = curRegion[i++];
		}
		else
			curRegionAlignedGap[resLen] = true;
		if (dk == 0 || dk == -1)
		{
			targetRegionAlignedGap[resLen] = false;
			targetRegionAligned[resLen] = targetRegion[j++];
		}
		else
			targetRegionAlignedGap[resLen] = true;
		resLen++;

		/* Compute the size of the equal part of the block */
		curLen = V[m][m + k] - i;

		/* Check whether the equal part of the block is computed correctly */
		Assert(curLen >= 0);
		Assert(resLen + curLen <= maxResLen);
		Assert(memcmp(&curRegion[i], &targetRegion[j], curLen) == 0);
	}

	return resLen;
}

/*
 * Try to build a short alignment in order to produce a short diff delta.
 * If fails, return false, otherwise return true and write the delta to
 * pageData->delta.
 */
static bool
computeRegionDiffDelta(PageData *pageData,
					   const char *curpage, const char *targetpage,
					   int targetStart, int targetEnd,
					   int validStart, int validEnd,
					   uint8 maxMismatches)
{
	char	   *ptr = pageData->delta + pageData->deltaLen;
	int			i,
				j;
	char		type;
	uint8		len;
	OffsetNumber start;
	OffsetNumber tmp;

	int			numMismatches;
	int			alignmentLength;

	int			curRegionLen = validEnd - validStart;
	int			targetRegionLen = targetEnd - targetStart;

	numMismatches = alignRegions(&curpage[validStart],
								 &targetpage[targetStart],
								 curRegionLen,
								 targetRegionLen,
								 maxMismatches);
	Assert(numMismatches <= maxMismatches);

	/* If no proper alignment was found return false */
	if (numMismatches < 0)
		return false;

	/* Restore the alignment in a compact form */
	alignmentLength = restoreCompactAlignment(&curpage[validStart],
											  &targetpage[targetStart],
											  curRegionLen,
											  targetRegionLen,
											  numMismatches);

	/*
	 * Translate the alignment into the set of instructions for transformation
	 * from curRegion into targetRegion, and write these instructions into
	 * pageData->delta.
	 */

	/* Verify we have enough space */
	Assert(pageData->deltaLen + sizeof(type) + 2 * sizeof(tmp) <= MAX_DELTA_SIZE);

	/* Check whether the region is the upper or the lower part of the page */
	Assert((validStart == 0 && targetStart == 0) ||
		   (validEnd == BLCKSZ && targetEnd == BLCKSZ));

	/* Write start and end indexes of the buffers */
	if (validStart == 0 && targetStart == 0)
	{
		/* We are in the upper part, there is no need to store Starts */
		type = UPPER_REGION;
		writeToPtr(ptr, type);
		tmp = validEnd;
		writeToPtr(ptr, tmp);
		tmp = targetEnd;
		writeToPtr(ptr, tmp);
	}
	else
	{
		/* We are in the lower part, there is no need to store Ends */
		type = LOWER_REGION;
		writeToPtr(ptr, type);
		tmp = validStart;
		writeToPtr(ptr, tmp);
		tmp = targetStart;
		writeToPtr(ptr, tmp);
	}

	/* Transform the alignment into the set of instructions */
	for (i = 0; i < alignmentLength; ++i)
	{
		/* Verify the alignment */
		Assert(!curRegionAlignedGap[i] || !targetRegionAlignedGap[i]);
		Assert(curRegionAligned[i] != targetRegionAligned[i] ||
			   curRegionAlignedGap[i] || targetRegionAlignedGap[i]);

		/* Determine the type of the instruction */
		if (curRegionAlignedGap[i])
			type = DIFF_INSERT;
		else if (targetRegionAlignedGap[i])
			type = DIFF_REMOVE;
		else
			type = DIFF_REPLACE;

		/* Find the end of the block of the same instructions */
		j = i + 1;
		while (j < alignmentLength)
		{
			bool		sameBlock;

			sameBlock = (
						 (curRegionAlignedPos[j] <=
						  curRegionAlignedPos[j - 1] + 1) &&
						 (targetRegionAlignedPos[j] <=
						  targetRegionAlignedPos[j - 1] + 1)
				);

			switch (type)
			{
				case DIFF_INSERT:
					sameBlock &= (curRegionAlignedGap[j]);
					break;
				case DIFF_REMOVE:
					sameBlock &= (targetRegionAlignedGap[j]);
					break;
				case DIFF_REPLACE:
					sameBlock &= (!curRegionAlignedGap[j] &&
								  !targetRegionAlignedGap[j] &&
								  (curRegionAligned[j] !=
								   targetRegionAligned[j]));
					break;
				default:
					elog(ERROR, "unrecognized delta operation type: %d", type);
					break;
			}
			if (sameBlock)
				j++;
			else
				break;
		}
		len = j - i;

		start = curRegionAlignedPos[i];
		/* Verify we have enough space */
		Assert(pageData->deltaLen + sizeof(type) +
			   sizeof(len) + sizeof(start) <= MAX_DELTA_SIZE);
		/* Write the header for instruction */
		writeToPtr(ptr, type);
		writeToPtr(ptr, len);
		writeToPtr(ptr, start);

		/* Write instruction data and go to the end of the block */
		if (type != DIFF_REMOVE)
		{
			/* Verify we have enough space */
			Assert(pageData->deltaLen + len <= MAX_DELTA_SIZE);
			while (i < j)
			{
				char		c = targetRegionAligned[i++];

				writeToPtr(ptr, c);
			}
		}
		else
			i = j;
		i--;
	}

	pageData->deltaLen = ptr - pageData->delta;

	return true;
}

/*
 * Return whether pageData->delta contains diff deltas or not.
 */
static bool
pageDataContainsDiffDelta(PageData *pageData)
{
	char	   *ptr = pageData->delta + sizeof(bool);
	char	   *end = pageData->delta + pageData->deltaLen;
	char		header;
	int			length;

	while (ptr < end)
	{
		readFromPtr(ptr, header);
		readFromPtr(ptr, length);

		if (header == DIFF_DELTA)
			return true;
		ptr += length;
	}
	return false;
}

/*
 * Downgrade pageData->delta to base delta format.
 *
 * Only base diffs are allowed to perform the transformation.
 */
static void
downgradeDeltaToBaseFormat(PageData *pageData)
{
	char	   *ptr = pageData->delta;
	char	   *end = pageData->delta + pageData->deltaLen;
	char	   *cur;
	bool		containsDiffDelta;
	char		header;
	int			length;
	int			newDeltaLength = 0;

	/* Check whether containsDiffDelta is false */
	readFromPtr(ptr, containsDiffDelta);
	Assert(!containsDiffDelta);

	cur = ptr;
	while (ptr < end)
	{
		readFromPtr(ptr, header);
		readFromPtr(ptr, length);

		/* Check whether the region delta is base delta */
		Assert(header == BASE_DELTA);
		newDeltaLength += length;

		memmove(cur, ptr, length);
		cur += length;
		ptr += length;
	}
	pageData->deltaLen = newDeltaLength;
}

/*
 * Compute the XLOG delta record needed to transform curpage into targetpage,
 * and store it in pageData's delta field.
 */
static void
computeDelta(PageData *pageData, Page curpage, Page targetpage)
{
	bool	   *containsDiffDelta = pageData->delta;
	int			targetLower = ((PageHeader) targetpage)->pd_lower,
				targetUpper = ((PageHeader) targetpage)->pd_upper,
				curLower = ((PageHeader) curpage)->pd_lower,
				curUpper = ((PageHeader) curpage)->pd_upper;

	pageData->deltaLen = sizeof(bool);

	/* Compute delta records for lower part of page ... */
	computeRegionDelta(pageData, curpage, targetpage,
					   0, targetLower,
					   0, curLower,
					   pageData->compressParams.lowerMaxMismatches);
	/* ... and for upper part, ignoring what's between */
	computeRegionDelta(pageData, curpage, targetpage,
					   targetUpper, BLCKSZ,
					   curUpper, BLCKSZ,
					   pageData->compressParams.upperMaxMismatches);

	/*
	 * Set first byte to true if at least one of the region deltas is diff
	 * delta. Otherwise set first byte to false and downgrade all regions to
	 * base format without extra headers.
	 */
	*containsDiffDelta = pageDataContainsDiffDelta(pageData);
	if (!(*containsDiffDelta))
		downgradeDeltaToBaseFormat(pageData);

	/*
	 * If xlog debug is enabled, then check produced delta.  Result of delta
	 * application to curpage should be equivalent to targetpage.
	 */
#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
	{
		char		tmp[BLCKSZ];

		memcpy(tmp, curpage, BLCKSZ);
		applyPageRedo(tmp, pageData->delta, pageData->deltaLen);
		if (memcmp(tmp, targetpage, targetLower) != 0 ||
			memcmp(tmp + targetUpper, targetpage + targetUpper,
				   BLCKSZ - targetUpper) != 0)
			elog(ERROR, "result of generic xlog apply does not match");
	}
#endif
}

/*
 * Start new generic xlog record for modifications to specified relation.
 */
GenericXLogState *
GenericXLogStart(Relation relation)
{
	GenericXLogState *state;
	int			i;

	state = (GenericXLogState *) palloc(sizeof(GenericXLogState));
	state->isLogged = RelationNeedsWAL(relation);

	for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
	{
		state->pages[i].image = state->images + BLCKSZ * i;
		state->pages[i].buffer = InvalidBuffer;
	}

	return state;
}

/*
 * Register new buffer for generic xlog record.
 *
 * Returns pointer to the page's image in the GenericXLogState, which
 * is what the caller should modify.
 *
 * If the buffer is already registered, just return its existing entry.
 * (It's not very clear what to do with the flags and parameters in such
 * a case, but for now we stay with the original flags and parameters.)
 */
Page
GenericXLogRegisterBufferEx(GenericXLogState *state,
							Buffer buffer, int flags,
							PageXLogCompressParams compressParams)
{
	int			block_id;

	/* Search array for existing entry or first unused slot */
	for (block_id = 0; block_id < MAX_GENERIC_XLOG_PAGES; block_id++)
	{
		PageData   *page = &state->pages[block_id];

		if (BufferIsInvalid(page->buffer))
		{
			/* Empty slot, so use it (there cannot be a match later) */
			page->buffer = buffer;
			page->flags = flags;
			page->compressParams = compressParams;
			memcpy(page->image, BufferGetPage(buffer), BLCKSZ);
			return (Page) page->image;
		}
		else if (page->buffer == buffer)
		{
			/*
			 * Buffer is already registered.  Just return the image, which is
			 * already prepared.
			 */
			return (Page) page->image;
		}
	}

	elog(ERROR, "maximum number %d of generic xlog buffers is exceeded",
		 MAX_GENERIC_XLOG_PAGES);
	/* keep compiler quiet */
	return NULL;
}

/*
 * An alias for GenericXLogRegisterBufferEx with default parameters
 * which are optimal for Bloom and RUM indexes.
 * Left for backward compatibility.
 */
Page
GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buffer, int flags)
{
	PageXLogCompressParams defaultParameters;

	defaultParameters.upperMaxMismatches = 0;
	defaultParameters.lowerMaxMismatches = 24;
	return GenericXLogRegisterBufferEx(state, buffer, flags,
									   defaultParameters);
}

/*
 * Apply changes represented by GenericXLogState to the actual buffers,
 * and emit a generic xlog record.
 */
XLogRecPtr
GenericXLogFinish(GenericXLogState *state)
{
	XLogRecPtr	lsn;
	int			i;

	if (state->isLogged)
	{
		/* Logged relation: make xlog record in critical section. */
		XLogBeginInsert();

		START_CRIT_SECTION();

		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];
			Page		page;
			PageHeader	pageHeader;

			if (BufferIsInvalid(pageData->buffer))
				continue;

			page = BufferGetPage(pageData->buffer);
			pageHeader = (PageHeader) pageData->image;

			if (pageData->flags & GENERIC_XLOG_FULL_IMAGE)
			{
				/*
				 * A full-page image does not require us to supply any xlog
				 * data.  Just apply the image, being careful to zero the
				 * "hole" between pd_lower and pd_upper in order to avoid
				 * divergence between actual page state and what replay would
				 * produce.
				 */
				memcpy(page, pageData->image, pageHeader->pd_lower);
				memset(page + pageHeader->pd_lower, 0,
					   pageHeader->pd_upper - pageHeader->pd_lower);
				memcpy(page + pageHeader->pd_upper,
					   pageData->image + pageHeader->pd_upper,
					   BLCKSZ - pageHeader->pd_upper);

				XLogRegisterBuffer(i, pageData->buffer,
								   REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
			}
			else
			{
				/*
				 * In normal mode, calculate delta and write it as xlog data
				 * associated with this page.
				 */
				computeDelta(pageData, page, (Page) pageData->image);

				/* Apply the image, with zeroed "hole" as above */
				memcpy(page, pageData->image, pageHeader->pd_lower);
				memset(page + pageHeader->pd_lower, 0,
					   pageHeader->pd_upper - pageHeader->pd_lower);
				memcpy(page + pageHeader->pd_upper,
					   pageData->image + pageHeader->pd_upper,
					   BLCKSZ - pageHeader->pd_upper);

				XLogRegisterBuffer(i, pageData->buffer, REGBUF_STANDARD);
				XLogRegisterBufData(i, pageData->delta, pageData->deltaLen);
			}
		}

		/* Insert xlog record */
		lsn = XLogInsert(RM_GENERIC_ID, 0);

		/* Set LSN and mark buffers dirty */
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];

			if (BufferIsInvalid(pageData->buffer))
				continue;
			PageSetLSN(BufferGetPage(pageData->buffer), lsn);
			MarkBufferDirty(pageData->buffer);
		}
		END_CRIT_SECTION();
	}
	else
	{
		/* Unlogged relation: skip xlog-related stuff */
		START_CRIT_SECTION();
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];

			if (BufferIsInvalid(pageData->buffer))
				continue;
			memcpy(BufferGetPage(pageData->buffer),
				   pageData->image,
				   BLCKSZ);
			/* We don't worry about zeroing the "hole" in this case */
			MarkBufferDirty(pageData->buffer);
		}
		END_CRIT_SECTION();
		/* We don't have a LSN to return, in this case */
		lsn = InvalidXLogRecPtr;
	}

	pfree(state);

	return lsn;
}

/*
 * Abort generic xlog record construction.  No changes are applied to buffers.
 *
 * Note: caller is responsible for releasing locks/pins on buffers, if needed.
 */
void
GenericXLogAbort(GenericXLogState *state)
{
	pfree(state);
}

/*
 * Apply delta to given page image.
 *
 * Read blocks of instructions and apply them based on their type:
 * BASE_DELTA or DIFF_DELTA.
 */
static void
applyPageRedo(Page page, const char *delta, Size deltaSize)
{
	const char *ptr = delta;
	const char *end = delta + deltaSize;
	char		header;
	int			length;
	bool		containsDiffDelta;

	/* If page delta is base delta, apply it. */
	readFromPtr(ptr, containsDiffDelta);
	if (!containsDiffDelta)
	{
		applyPageBaseRedo(page, ptr, end - ptr);
		return;
	}

	/* Otherwise apply each region delta. */
	while (ptr < end)
	{
		readFromPtr(ptr, header);
		readFromPtr(ptr, length);

		switch (header)
		{
			case DIFF_DELTA:
				ptr = applyPageDiffRedo(page, ptr, length);
				break;
			case BASE_DELTA:
				ptr = applyPageBaseRedo(page, ptr, length);
				break;
			default:
				elog(ERROR,
					 "unrecognized delta type: %d",
					 header);
				break;
		}
	}
}

/*
 * Apply base delta to given page image.
 */
static const char *
applyPageBaseRedo(Page page, const char *delta, Size deltaSize)
{
	const char *ptr = delta;
	const char *end = delta + deltaSize;

	while (ptr < end)
	{
		OffsetNumber offset,
					length;

		memcpy(&offset, ptr, sizeof(offset));
		ptr += sizeof(offset);
		memcpy(&length, ptr, sizeof(length));
		ptr += sizeof(length);

		memcpy(page + offset, ptr, length);

		ptr += length;
	}
	return ptr;
}

/*
 * Apply diff delta to given page image.
 */
static const char *
applyPageDiffRedo(Page page, const char *delta, Size deltaSize)
{
	const char *ptr = delta;
	const char *end = delta + deltaSize;
	char		type;
	uint8		len;
	OffsetNumber targetStart,
				targetEnd;
	OffsetNumber validStart,
				validEnd;
	int			i,
				j;
	OffsetNumber start;

	/* Read start and end indexes of the buffers */
	validStart = 0;
	validEnd = BLCKSZ;
	targetStart = 0;
	targetEnd = BLCKSZ;
	readFromPtr(ptr, type);
	switch (type)
	{
		case UPPER_REGION:
			readFromPtr(ptr, validEnd);
			readFromPtr(ptr, targetEnd);
			break;
		case LOWER_REGION:
			readFromPtr(ptr, validStart);
			readFromPtr(ptr, targetStart);
			break;
		default:
			elog(ERROR,
				 "unrecognized region type: %d",
				 type);
			break;
	}

	/* Read and apply the instructions */
	i = 0, j = 0;
	while (ptr < end)
	{
		/* Read the header of the current instruction */
		readFromPtr(ptr, type);
		readFromPtr(ptr, len);
		readFromPtr(ptr, start);

		/* Copy the data before current instruction to buffer */
		memcpy(&localPage[j], page + validStart + i, start - i);
		j += start - i;
		i = start;

		/* Apply the instruction */
		switch (type)
		{
			case DIFF_INSERT:
				memcpy(&localPage[j], ptr, len);
				ptr += len;
				j += len;
				break;
			case DIFF_REMOVE:
				i += len;
				break;
			case DIFF_REPLACE:
				memcpy(&localPage[j], ptr, len);
				i += len;
				j += len;
				ptr += len;
				break;
			default:
				elog(ERROR,
					 "unrecognized delta instruction type: %d",
					 type);
				break;
		}
	}

	/* Copy the data after the last instruction */
	memcpy(&localPage[j], page + validStart + i, validEnd - validStart - i);
	j += validEnd - validStart - i;
	i = validEnd - validStart;

	memcpy(page + targetStart, localPage, j);
	return ptr;
}

/*
 * Redo function for generic xlog record.
 */
void
generic_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffers[MAX_GENERIC_XLOG_PAGES];
	uint8		block_id;

	/* Protect limited size of buffers[] array */
	Assert(record->max_block_id < MAX_GENERIC_XLOG_PAGES);

	/* Iterate over blocks */
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		XLogRedoAction action;

		if (!XLogRecHasBlockRef(record, block_id))
		{
			buffers[block_id] = InvalidBuffer;
			continue;
		}

		action = XLogReadBufferForRedo(record, block_id, &buffers[block_id]);

		/* Apply redo to given block if needed */
		if (action == BLK_NEEDS_REDO)
		{
			Page		page;
			PageHeader	pageHeader;
			char	   *blockDelta;
			Size		blockDeltaSize;

			page = BufferGetPage(buffers[block_id]);
			blockDelta = XLogRecGetBlockData(record, block_id, &blockDeltaSize);
			applyPageRedo(page, blockDelta, blockDeltaSize);

			/*
			 * Since the delta contains no information about what's in the
			 * "hole" between pd_lower and pd_upper, set that to zero to
			 * ensure we produce the same page state that application of the
			 * logged action by GenericXLogFinish did.
			 */
			pageHeader = (PageHeader) page;
			memset(page + pageHeader->pd_lower, 0,
				   pageHeader->pd_upper - pageHeader->pd_lower);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffers[block_id]);
		}
	}

	/* Changes are done: unlock and release all buffers */
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		if (BufferIsValid(buffers[block_id]))
			UnlockReleaseBuffer(buffers[block_id]);
	}
}

/*
 * Mask a generic page before performing consistency checks on it.
 */
void
generic_mask(char *page, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(page);

	mask_unused_space(page);
}
