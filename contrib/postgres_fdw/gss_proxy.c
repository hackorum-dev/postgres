#include <gssapi/gssapi.h>

#include "postgres.h"
#include "miscadmin.h"
#include "libpq/libpq-be.h"

bool has_gss_proxy_cred()
{
	return MyProcPort->gss != NULL && MyProcPort->gss->proxy != NULL;
}

/* ugly hack to pass the gss proxy credential from backend Port to frontend libpq. */
char* get_gss_proxy_cred()
{
	char* addr = NULL;
	if (MyProcPort->gss)
	{
		if (MyProcPort->gss->proxy)
		{
			addr = palloc(sizeof(void*) + 3);
			sprintf(addr, "%p", MyProcPort->gss->proxy);
		}
	}
	return addr;
}
