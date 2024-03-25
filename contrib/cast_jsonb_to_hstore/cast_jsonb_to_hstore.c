#include "postgres.h"
#include "hstore/hstore.h"
#include "utils/jsonb.h"

PG_MODULE_MAGIC;

typedef int (*hstoreUniquePairs_t) (Pairs *a, int32 l, int32 *buflen);
static hstoreUniquePairs_t hstoreUniquePairs_p;
typedef HStore *(*hstorePairs_t) (Pairs *pairs, int32 pcount, int32 buflen);
static hstorePairs_t hstorePairs_p;
typedef size_t (*hstoreCheckKeyLen_t) (size_t len);
static hstoreCheckKeyLen_t hstoreCheckKeyLen_p;
typedef size_t (*hstoreCheckValLen_t) (size_t len);
static hstoreCheckValLen_t hstoreCheckValLen_p;

void
_PG_init(void)
{
	AssertVariableIsOfType(&hstoreUniquePairs, hstoreUniquePairs_t);
	hstoreUniquePairs_p = (hstoreUniquePairs_t)
		load_external_function("$libdir/hstore", "hstoreUniquePairs",
							   true, NULL);
	AssertVariableIsOfType(&hstorePairs, hstorePairs_t);
	hstorePairs_p = (hstorePairs_t)
		load_external_function("$libdir/hstore", "hstorePairs",
							   true, NULL);
	AssertVariableIsOfType(&hstoreCheckKeyLen, hstoreCheckKeyLen_t);
	hstoreCheckKeyLen_p = (hstoreCheckKeyLen_t)
		load_external_function("$libdir/hstore", "hstoreCheckKeyLen",
							   true, NULL);
	AssertVariableIsOfType(&hstoreCheckValLen, hstoreCheckValLen_t);
	hstoreCheckValLen_p = (hstoreCheckValLen_t)
		load_external_function("$libdir/hstore", "hstoreCheckValLen",
							   true, NULL);
}

#define hstoreUniquePairs hstoreUniquePairs_p
#define hstorePairs hstorePairs_p
#define hstoreCheckKeyLen hstoreCheckKeyLen_p
#define hstoreCheckValLen hstoreCheckValLen_p

PG_FUNCTION_INFO_V1(jsonb_to_hstore);

Datum 
jsonb_to_hstore(PG_FUNCTION_ARGS)
{
	int32 buflen;
	int32 i;
	int32 pcount;
	HStore *out;
	Pairs *pairs;

	Jsonb *in = PG_GETARG_JSONB_P(0);
	JsonbContainer *jsonb = &in->root;

	JsonbValue v;
	JsonbIterator *it;
	JsonbIteratorToken r;

	it = JsonbIteratorInit(jsonb);
	r = JsonbIteratorNext(&it, &v, true);

	i = 0;
	pcount = v.val.object.nPairs;
	pairs = palloc(pcount * sizeof(Pairs));

	if (r == WJB_BEGIN_OBJECT)
	{
		while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
		{
			if (r == WJB_KEY)
			{
				//key- v, value- val
				JsonbValue	val;
				if (JsonbIteratorNext(&it, &val, true) == WJB_VALUE)
				{
					pairs[i].key = pstrdup(v.val.string.val);
					pairs[i].keylen = hstoreCheckKeyLen(v.val.string.len);
					pairs[i].needfree = true;

					switch (val.type)
					{
					case jbvNumeric:
						pairs[i].val = pstrdup((numeric_normalize(val.val.numeric)));
						pairs[i].vallen = hstoreCheckValLen(strlen(pairs[i].val));
						pairs[i].isnull = false;
						break;
					case jbvString:
						pairs[i].val = strdup((val.val.string.val));
						pairs[i].vallen = hstoreCheckValLen(val.val.string.len);
						pairs[i].isnull = false;
						break;
					case jbvNull:
						pairs[i].isnull = true;
						break;
					case jbvBool:
						if (val.val.boolean)
						{
							pairs[i].val = "true";
							pairs[i].vallen = hstoreCheckValLen(strlen("true"));
						}
						else
						{
							pairs[i].val = "false";
							pairs[i].vallen = hstoreCheckValLen(strlen("false"));
						}
						pairs[i].isnull = false;
					default:
						break;
					}
				}
			}
			++i;
		}
	}
	pcount = hstoreUniquePairs(pairs, pcount, &buflen);
	out = hstorePairs(pairs, pcount, buflen);
	PG_RETURN_POINTER(out);
}
