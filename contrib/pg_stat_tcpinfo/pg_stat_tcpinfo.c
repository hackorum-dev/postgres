/*-------------------------------------------------------------------------
 *
 * pg_stat_tcpinfo.c
 *		A netstat/ss-like Linux-only function and view for PostgreSQL.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_tcpinfo/pg_stat_tcpinfo.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * It works in three main parts:
 * 1. Scans /proc/net/tcp* to get a list of all active TCP sockets and their
 *    'inode' numbers.
 * 2. Scans the /proc filesystem (all /proc/[PID]/fd/ directories)
 *    to build a map of which PID owns which socket inode (by reading symlink).
 * 3. Queries the netlink INET_DIAG interface to get detailed
 *    TCP info (like RTT, skmem, timers, congestion algorithm used) for all
 *    connections.
 * 4. Joins these three pieces of information and returns them as a set of rows.
 *
 * This function must be run by a user with sufficient permissions
 * (e.g., as part of the 'postgres' superuser) and the PostgreSQL
 * server process itself must have permissions to read the
 * /proc/[PID]/fd directories of processes owned by other users.
 * Without such permissions, the 'pid' column will be NULL for most connections.
 */

#include "c.h"
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <ctype.h>
#include <dirent.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stddef.h>

PG_MODULE_MAGIC_EXT(
					.name = "pg_stat_tcpinfo",
					.version = PG_VERSION
);

#ifdef __linux__

/* Linux kernel TCP states, see linux/include/net/tcp_states.h */
enum
{
	TCP_ESTABLISHED = 1,
	TCP_SYN_SENT,
	TCP_SYN_RECV,
	TCP_FIN_WAIT1,
	TCP_FIN_WAIT2,
	TCP_TIME_WAIT,
	TCP_CLOSE,
	TCP_CLOSE_WAIT,
	TCP_LAST_ACK,
	TCP_LISTEN,
	TCP_CLOSING
};

/* Map of TCP states to strings */
static const char *tcp_states_map[] = {
	[0] = "UNKNOWN",
	[TCP_ESTABLISHED] = "ESTABLISHED",
	[TCP_SYN_SENT] = "SYN-SENT",
	[TCP_SYN_RECV] = "SYN-RECV",
	[TCP_FIN_WAIT1] = "FIN-WAIT-1",
	[TCP_FIN_WAIT2] = "FIN-WAIT-2",
	[TCP_TIME_WAIT] = "TIME-WAIT",
	[TCP_CLOSE] = "CLOSE",
	[TCP_CLOSE_WAIT] = "CLOSE-WAIT",
	[TCP_LAST_ACK] = "LAST-ACK",
	[TCP_LISTEN] = "LISTEN",
	[TCP_CLOSING] = "CLOSING"
};

/* See enum in netinet/tcp.h, TCP_CLOSING seems to be the last one */
#define TCP_MAX_STATE TCP_CLOSING + 1

/* see sock_diag(7) nearby idiag_timer */
static const char *tcptimer_names_map[] = {
	"off",
	"on",
	"keepalive",
	"timewait",
	"persist",					/* zero probe window */
	"unknown"
};

/*
 * Used by struct inet_diag_req_v2 -> idiag_states (there are 11 states
 * so we need 12 bitmask). It could also be named as TCP_ALL_FLAGS
 */
#define TCPF_ALL 0xFFF

/* Netlink socket recieve buffer size */
#define NL_SOCKET_BUFFER_SIZE 8192

/* Stores stuff from /proc/net/tcp */
typedef struct TcpConnection
{
	char		local_addr_str[64]; /* IP:port */
	char		remote_addr_str[64];	/* IP:port */
	char		local_ip_str[INET6_ADDRSTRLEN]; /* IP */
	char		remote_ip_str[INET6_ADDRSTRLEN];	/* IP */
	int			local_port;
	int			remote_port;
	int			state;
	int			uid;
	__u8		family;
	unsigned long long inode;
	struct TcpConnection *next;
}			TcpConnection;

typedef struct InodePid
{
	unsigned long long inode;
	int			pid;
}			InodePid;

typedef struct NlDiagInfo
{
	char		key[512];		/* Key:"L_IP:L_PORT-R_IP:R_PORT" */
	struct tcp_info tcpi;
	__u32		skmem[SK_MEMINFO_VARS];

	/*
	 * 12 below because they are not long names, see available
	 * /lib/modules/$(uname -r)/kernel/net/ipv4/tcp_<cong>.ko kernel modules
	 */
	char		cong[12];
	int			has_tcpi;
	int			has_skmem;
	int			has_cong;
	char		tcp_timer_str[64];
}			NlDiagInfo;


/* Inserts an inode+PID pair into the hash map. */
static void
insert_pid(HTAB *pid_map, unsigned long long inode, int pid)
{
	InodePid   *entry;
	bool		found;

	entry = (InodePid *) hash_search(pid_map, &inode, HASH_ENTER, &found);
	entry->pid = pid;
}

/* Locate a PID in the hash map given an inode. Returns PID or -1 */
static int
find_pid(HTAB *pid_map, unsigned long long inode)
{
	InodePid   *entry;

	entry = (InodePid *) hash_search(pid_map, &inode, HASH_FIND, NULL);
	if (entry)
		return entry->pid;

	return -1;
}

/* Formats a unique key string for a connection. */
static void
format_connection_key(char *hash_key, size_t hash_key_sz,
					  const char *local_addr, int local_port,
					  const char *remote_addr, int remote_port)
{
	snprintf(hash_key, hash_key_sz, "%s:%d-%s:%d",
			 local_addr, local_port, remote_addr, remote_port);
}

/*
 * Finds and creates/updates a node in the nldiag_map and stores data in it.
 */
static void
store_netlink_info(HTAB *nldiag_map,
				   const char *local_addr, const int local_port,
				   const char *remote_addr, const int remote_port,
				   const int type, void *data, const char *tcp_timer_str)
{
	char		hash_key[512];
	NlDiagInfo *entry;
	bool		found;

	format_connection_key(hash_key, sizeof(hash_key), local_addr, local_port, remote_addr, remote_port);
	entry = (NlDiagInfo *) hash_search(nldiag_map, hash_key, HASH_ENTER, &found);
	elog(DEBUG5, "saving some netlink chatter about %s into %p", hash_key, entry);

	if (!found)
	{
		/*
		 * New entry. hash_search() copied the key, but the rest of the struct
		 * is uninitialized. Zero the payload just in case.
		 */
		memset((char *) entry + offsetof(NlDiagInfo, tcpi), 0,
			   sizeof(NlDiagInfo) - offsetof(NlDiagInfo, tcpi));
	}

	/* Update the entry (whether new or old). */
	memcpy(entry->tcp_timer_str, tcp_timer_str, sizeof(entry->tcp_timer_str));

	switch (type)
	{
		case INET_DIAG_INFO:
			memcpy(&entry->tcpi, data, sizeof(struct tcp_info));
			entry->has_tcpi = 1;
			break;
		case INET_DIAG_SKMEMINFO:
			memcpy(entry->skmem, data, sizeof(__u32) * SK_MEMINFO_VARS);
			entry->has_skmem = 1;
			break;
		case INET_DIAG_CONG:
			memcpy(entry->cong, data, sizeof(entry->cong));
			entry->has_cong = 1;
			break;
		default:
			elog(WARNING, "unsupported inet diag type reply");
	}
}

/*
 * Finds netlink info in the hash map.
 * Returns pointer to NlDiagInfo, or NULL if not found.
 */
static NlDiagInfo *
find_netlink_info(HTAB *nldiag_map,
				  const char *local_addr, const int local_port,
				  const char *remote_addr, const int remote_port)
{
	char		hash_key[512];
	NlDiagInfo *entry;

	format_connection_key(hash_key, sizeof(hash_key), local_addr, local_port, remote_addr, remote_port);
	entry = (NlDiagInfo *) hash_search(nldiag_map, hash_key, HASH_FIND, NULL);
	elog(DEBUG5, "nldiag_map returning about %s --> %p", hash_key, entry);
	return entry;
}

/*
 * Please see man sock_diag(7) on Linux for details about this API.
 */
static int
send_diag_msg(int sockfd, __u8 family)
{
	struct msghdr msg;
	struct nlmsghdr nlh;
	struct inet_diag_req_v2 conn_req;
	struct sockaddr_nl sa;
	struct iovec iov[4];
	int			retval = 0;

	elog(DEBUG1, "quering netlink socket for TCP low-level stats");

	memset(&msg, 0, sizeof(msg));
	memset(&sa, 0, sizeof(sa));
	memset(&nlh, 0, sizeof(nlh));
	memset(&conn_req, 0, sizeof(conn_req));

	sa.nl_family = AF_NETLINK;
	conn_req.sdiag_family = family;
	conn_req.sdiag_protocol = IPPROTO_TCP;

	/*
	 * Do not filter out any TCP states (include all).
	 *
	 * Maybe we should filter-out everything else here than TCP_ESTABLISHED?
	 * But somehow stuck connections (e.g. in TCP_SYN_SENT) seems to be useful
	 * info on it's own. Anyway, filtering out could work that way:
	 *
	 * conn_req.idiag_states = TCPF_ALL & ~((1 << TCP_SYN_RECV) | (1 <<
	 * TCP_TIME_WAIT) | (1 << TCP_CLOSE));
	 */
	conn_req.idiag_states = TCPF_ALL;

	/* Request extended TCP information: see linux/inet_diag include */
	conn_req.idiag_ext |= (1 << (INET_DIAG_INFO - 1));
	conn_req.idiag_ext |= (1 << (INET_DIAG_SKMEMINFO - 1));
	conn_req.idiag_ext |= (1 << (INET_DIAG_CONG - 1));
	/* XXX: we could query for INET_DIAG_VEGASINFO too */
#ifdef INET_DIAG_BBRINFO
	conn_req.idiag_ext |= (1 << (INET_DIAG_BBRINFO - 1));
#endif

	nlh.nlmsg_len = NLMSG_LENGTH(sizeof(conn_req));
	nlh.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
	nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
	iov[0].iov_base = (void *) &nlh;
	iov[0].iov_len = sizeof(nlh);
	iov[1].iov_base = (void *) &conn_req;
	iov[1].iov_len = sizeof(conn_req);

	msg.msg_name = (void *) &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;

	retval = sendmsg(sockfd, &msg, 0);

	return retval;
}


static char *
ms_to_min_sec(unsigned long ms)
{
	static char buffer[64];
	long		total_seconds = ms / 1000;
	long		minutes = total_seconds / 60;
	long		seconds = total_seconds % 60;

	memset(buffer, 0, sizeof(buffer));
	snprintf(buffer, sizeof(buffer), "%ldmin%ldsec", minutes, seconds);
	return buffer;
}

static void
parse_diag_msg(HTAB *nldiag_map, struct inet_diag_msg *diag_msg, int rtalen)
{
	struct rtattr *attr;
	char		local_addr_buf[INET6_ADDRSTRLEN];
	char		remote_addr_buf[INET6_ADDRSTRLEN];
	char		tcp_timer_str[64];
	int			local_port,
				remote_port;

	memset(local_addr_buf, 0, sizeof(local_addr_buf));
	memset(remote_addr_buf, 0, sizeof(remote_addr_buf));

	local_port = ntohs(diag_msg->id.idiag_sport);
	remote_port = ntohs(diag_msg->id.idiag_dport);

	if (diag_msg->idiag_family == AF_INET)
	{
		inet_ntop(AF_INET, (struct in_addr *) &(diag_msg->id.idiag_src),
				  local_addr_buf, INET_ADDRSTRLEN);
		inet_ntop(AF_INET, (struct in_addr *) &(diag_msg->id.idiag_dst),
				  remote_addr_buf, INET_ADDRSTRLEN);
	}
	else if (diag_msg->idiag_family == AF_INET6)
	{
		inet_ntop(AF_INET6, (struct in_addr6 *) &(diag_msg->id.idiag_src),
				  local_addr_buf, INET6_ADDRSTRLEN);
		inet_ntop(AF_INET6, (struct in_addr6 *) &(diag_msg->id.idiag_dst),
				  remote_addr_buf, INET6_ADDRSTRLEN);
	}
	else
	{
		/* Unknown family, just log it */
		ereport(WARNING, (errmsg("unknown address family in netlink response: %d", diag_msg->idiag_family)));
		return;
	}

	if (local_addr_buf[0] == 0 || remote_addr_buf[0] == 0)
	{
		ereport(WARNING, (errmsg("could not get connection information from netlink message")));
		return;
	}

	/* Format the TCP timer information that is going to be saved later on. */
	snprintf(tcp_timer_str, sizeof(tcp_timer_str), "%s,%s,%d",
			 tcptimer_names_map[diag_msg->idiag_timer],
			 ms_to_min_sec(diag_msg->idiag_expires),
			 diag_msg->idiag_retrans);

	/*
	 * XXX: perhaps also save diag_msg->idiag_[rw]queue, but we already have
	 * it from skmem
	 */

	/* Parse the attributes, loop as we'll have multiple of them */
	if (rtalen > 0)
	{
		attr = (struct rtattr *) (diag_msg + 1);

		while (RTA_OK(attr, rtalen))
		{
			int			type = attr->rta_type;

			switch (type)
			{
				case INET_DIAG_INFO:
					{
						struct tcp_info *tcpi = (struct tcp_info *) RTA_DATA(attr);

						store_netlink_info(nldiag_map, local_addr_buf, local_port, remote_addr_buf, remote_port,
										   type, tcpi, tcp_timer_str);
						break;
					}
				case INET_DIAG_SKMEMINFO:
					{
						__u32	   *skmem = RTA_DATA(attr);

						store_netlink_info(nldiag_map, local_addr_buf, local_port, remote_addr_buf, remote_port,
										   type, skmem, tcp_timer_str);
						break;
					}
				case INET_DIAG_CONG:
					{
						char	   *cong = (char *) RTA_DATA(attr);

						store_netlink_info(nldiag_map, local_addr_buf, local_port, remote_addr_buf, remote_port,
										   type, cong, tcp_timer_str);
						break;
					}

			}

			attr = RTA_NEXT(attr, rtalen);
		}
	}
}


/*
 * Scans all /proc/[PID]/fd/ entries to map socket inodes to PIDs.
 * Fills the provided pid_map hash table.
 */
static void
scan_proc_fds(HTAB *pid_map)
{
	DIR		   *proc_dir,
			   *fd_dir;
	struct dirent *pid_entry,
			   *fd_entry;
	char		fd_path[MAXPGPATH];
	char		link_path[MAXPGPATH];
	char		link_target[MAXPGPATH];
	ssize_t		link_len;
	unsigned long long inode;

	elog(DEBUG1, "scanning /proc for PIDs");

	proc_dir = opendir("/proc");
	if (!proc_dir)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open /proc: %m")));
		return;
	}

	/* Iterate over each entry in /proc */
	while ((pid_entry = readdir(proc_dir)) != NULL)
	{
		/* Check if the directory name is a number (a PID) */
		if (pid_entry->d_type == DT_DIR && isdigit(pid_entry->d_name[0]))
		{
			int			pid = atoi(pid_entry->d_name);

			snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);

			/* Open the /proc/[PID]/fd directory */
			fd_dir = opendir(fd_path);
			if (!fd_dir)
			{
				ereport(DEBUG4,
						(errcode_for_file_access(),
						 errmsg("could not open directory \"%s\": %m", fd_path)));
				continue;
			}

			/* Iterate over each file descriptor in /proc/[PID]/fd */
			while ((fd_entry = readdir(fd_dir)) != NULL)
			{
				snprintf(link_path, sizeof(link_path), "%s/%s", fd_path, fd_entry->d_name);

				/* Read the symbolic link target */
				link_len = readlink(link_path, link_target, sizeof(link_target) - 1);
				if (link_len == -1)
				{
					/* Failed to read link */
					ereport(DEBUG4,
							(errcode_for_file_access(),
							 errmsg("could not read link \"%s\": %m", link_path)));
					continue;
				}
				link_target[link_len] = '\0';

				/* Check if it's a socket */
				if (strncmp(link_target, "socket:[", strlen("socket:[")) == 0)
				{
					if (sscanf(link_target, "socket:[%llu]", &inode) == 1)
					{
						/* Add this inode->PID mapping to our hash table */
						insert_pid(pid_map, inode, pid);
					}
				}
			}
			closedir(fd_dir);
		}
	}
	closedir(proc_dir);
}


/*
 * Reads /proc/net/tcp* and builds a linked list of connections.
 * Returns pointer to the head of the TcpConnection linked list.
 */
static TcpConnection * read_tcp_connections(__u8 family)
{
	FILE	   *fp;
	char		line[1024],
			   *tcp_file_name;
	TcpConnection *head = NULL;
	int			local_port,
				remote_port,
				state,
				uid,
				slot;
	unsigned long local_ip_hex,
				remote_ip_hex;
	struct in6_addr local_ip6_hex,
				remote_ip6_hex;
	unsigned long long inode;
	TcpConnection *conn;

	tcp_file_name = family == AF_INET ? "/proc/net/tcp" : "/proc/net/tcp6";
	elog(DEBUG1, "scanning %s for TCP connections and inodes", tcp_file_name);

	fp = fopen(tcp_file_name, "r");
	if (fp == NULL)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open %s: %m", tcp_file_name)));
		return NULL;
	}

	/* Skip the header line */
	if (fgets(line, sizeof(line), fp) == NULL)
	{
		fclose(fp);
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				 errmsg("could not read header from %s", tcp_file_name)));
		return NULL;
	}

	/* Read each line */
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		int			num_matched,
					proper_matches;

		if (family == AF_INET)
		{
			num_matched = sscanf(line, "%4d: %08lX:%04X %08lX:%04X %02X %*s %*s %*s %d %*s %llu",
								 &slot,
								 &local_ip_hex,
								 &local_port,
								 &remote_ip_hex,
								 &remote_port,
								 &state,
								 &uid,
								 &inode);
			proper_matches = 8;
		}
		else
		{
			/*
			 * Madness? This ... is ... sparta!
			 *
			 * /proc files for IPv6 tend to use 32-char hex representation of
			 * IPv6 address
			 */
			num_matched = sscanf(line, "%4d: %08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X %02X %*s %*s %*s %d %*s %llu",
								 &slot,
								 &local_ip6_hex.s6_addr32[0],
								 &local_ip6_hex.s6_addr32[1],
								 &local_ip6_hex.s6_addr32[2],
								 &local_ip6_hex.s6_addr32[3],
								 &local_port,
								 &remote_ip6_hex.s6_addr32[0],
								 &remote_ip6_hex.s6_addr32[1],
								 &remote_ip6_hex.s6_addr32[2],
								 &remote_ip6_hex.s6_addr32[3],
								 &remote_port,
								 &state,
								 &uid,
								 &inode);
			proper_matches = 14;
		}

		if (num_matched < proper_matches)
		{
			/* Failed to parse, so chomp last new line character and show it */
			line[strlen(line) - 1] = 0;
			ereport(WARNING, (errmsg("failed to parse line from %s (got just %d matches): %s", tcp_file_name, num_matched, line)));
			continue;
		}

		/* Create a new connection node */
		conn = palloc(sizeof(TcpConnection));

		conn->local_port = local_port;
		conn->remote_port = remote_port;

		/* It's already in network byte order (big-endian) */
		if (family == AF_INET)
		{
			if (inet_ntop(AF_INET, &local_ip_hex, conn->local_ip_str, INET_ADDRSTRLEN) == NULL)
			{
				ereport(WARNING, (errmsg("inet_ntop() failed for local IP: %m")));
				strncpy(conn->local_ip_str, "INVALID_IP", INET_ADDRSTRLEN);
			}

			/* Format remote IP */
			if (inet_ntop(AF_INET, &remote_ip_hex, conn->remote_ip_str, INET_ADDRSTRLEN) == NULL)
			{
				ereport(WARNING, (errmsg("inet_ntop() failed for remote IP: %m")));
				strncpy(conn->remote_ip_str, "INVALID_IP", INET_ADDRSTRLEN);
			}
		}
		else
		{
			/* AF_INET6 */
			if (inet_ntop(AF_INET6, &local_ip6_hex, conn->local_ip_str, INET6_ADDRSTRLEN) == NULL)
			{
				ereport(WARNING, (errmsg("inet_ntop() failed for local IP: %m")));
				strncpy(conn->local_ip_str, "INVALID_IP", INET_ADDRSTRLEN);
			}

			/* Format remote IP */
			if (inet_ntop(AF_INET6, &remote_ip6_hex, conn->remote_ip_str, INET6_ADDRSTRLEN) == NULL)
			{
				ereport(WARNING, (errmsg("inet_ntop() failed for remote IP: %m")));
				strncpy(conn->remote_ip_str, "INVALID_IP", INET_ADDRSTRLEN);
			}

		}

		/* Format combined strings */
		snprintf(conn->local_addr_str, sizeof(conn->local_addr_str), "%s:%d", conn->local_ip_str, conn->local_port);
		snprintf(conn->remote_addr_str, sizeof(conn->remote_addr_str), "%s:%d", conn->remote_ip_str, conn->remote_port);

		conn->state = state;
		conn->uid = uid;
		conn->inode = inode;
		conn->family = family;

		/* Add to the front of the linked list */
		conn->next = head;
		head = conn;
	}

	fclose(fp);
	return head;
}


/* Receive and parse all netlink data, populating nldiag_map */
static int
recv_diag_msgs(int nl_sock, HTAB *nldiag_map)
{
	uint8_t		recv_buf[NL_SOCKET_BUFFER_SIZE];
	int			numbytes = 0,
				done = 0,
				rtalen = 0;
	struct inet_diag_msg *diag_msg;
	struct nlmsghdr *nlh;

	while (1)
	{
		numbytes = recv(nl_sock, recv_buf, sizeof(recv_buf), 0);
		if (numbytes <= 0)
		{
			if (numbytes == 0)
				ereport(WARNING, (errmsg("netlink socket closed prematurely")));
			else
				ereport(WARNING, (errmsg("netlink recv error: %m")));
			break;
			/* Exit loop on error or close */
		}

		nlh = (struct nlmsghdr *) recv_buf;
		done = 0;

		while (NLMSG_OK(nlh, numbytes))
		{
			if (nlh->nlmsg_type == NLMSG_DONE)
			{
				done = 1;
				break;
			}

			if (nlh->nlmsg_type == NLMSG_ERROR)
			{
				struct nlmsgerr *err = (struct nlmsgerr *) NLMSG_DATA(nlh);

				close(nl_sock);
				ereport(ERROR, (errmsg("error in netlink message: %s", strerror(-err->error))));
			}

			diag_msg = (struct inet_diag_msg *) NLMSG_DATA(nlh);
			rtalen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*diag_msg));

			/* This populates nldiag_map */
			parse_diag_msg(nldiag_map, diag_msg, rtalen);

			nlh = NLMSG_NEXT(nlh, numbytes);
		}

		if (done)
			break;
	}
	return 0;
}


PG_FUNCTION_INFO_V1(pg_stat_get_tcpinfo);
Datum
pg_stat_get_tcpinfo(PG_FUNCTION_ARGS)
{
	Datum		values[10];
	bool		nulls[10];
	HTAB	   *pid_map;
	MemoryContext oldcontext;
	MemoryContext per_query_ctx;
	NlDiagInfo *diag_info;
	HTAB	   *nldiag_map;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	StringInfoData json_buf;
	TcpConnection *tcp_connections;
	TcpConnection *current;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	__u32	   *skmem,
				recvq = 0,
				sendq = 0;
	bool		has_data;
	bool		has_q_data;
	const char *state_str;
	int			nl_sock = 0,
				pid;
	HASHCTL		ctl;

	/* Check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	/* Allocate and initialize the hash maps in the CurrentMemoryContext. */
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(unsigned long long);
	ctl.entrysize = sizeof(InodePid);
	pid_map = hash_create("Inode to PID Map", 1024, &ctl, HASH_ELEM);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = 512;
	ctl.entrysize = sizeof(NlDiagInfo);
	nldiag_map = hash_create("Netlink Diag Map", 1024, &ctl, HASH_ELEM | HASH_STRINGS);

	/* Load the inode->PID hash map while scanning /proc/PIDs */
	scan_proc_fds(pid_map);

	/* Read all TCP connections from /proc/net/tcp (IPv4 file) */
	tcp_connections = read_tcp_connections(AF_INET);
	if (!tcp_connections)
		PG_RETURN_VOID();

	/* Find the tail of linked list of TCP connections ... */
	current = tcp_connections;
	while (current->next != NULL)
	{
		current = current->next;
	}
	/* ... and append list of IPv6-based TCP connections */
	current->next = read_tcp_connections(AF_INET6);

	/* Open netlink and query about sockets */
	if ((nl_sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_INET_DIAG)) == -1)
	{
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR),
						errmsg("could not create netlink socket: %m")));
	}

	/* IPv4 netlink message */
	if (send_diag_msg(nl_sock, AF_INET) < 0)
	{
		close(nl_sock);
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR),
						errmsg("could not send AF_INET netlink message: %m")));
	}
	if (recv_diag_msgs(nl_sock, nldiag_map))
	{
		close(nl_sock);
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR),
						errmsg("could not process netlink message: %m")));
	}

	/* IPv6 netlink message */
	if (send_diag_msg(nl_sock, AF_INET6) < 0)
	{
		close(nl_sock);
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR),
						errmsg("could not send AF_INET6 netlink message: %m")));
	}

	if (recv_diag_msgs(nl_sock, nldiag_map))
	{
		close(nl_sock);
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR),
						errmsg("could not process netlink message: %m")));
	}

	close(nl_sock);

	/* Start populating the tuplestore */
	initStringInfo(&json_buf);

	/* For each TCP connection from linked list */
	current = tcp_connections;
	while (current != NULL)
	{
		int			i = 0;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* pid */
		pid = find_pid(pid_map, current->inode);
		if (pid == -1)
			nulls[i++] = true;
		else
			values[i++] = Int32GetDatum(pid);

		/* uid */
		values[i++] = Int32GetDatum(current->uid);

		/* local ip */
		if (strcmp(current->local_ip_str, "INVALID_IP") == 0)
			nulls[i++] = true;
		else
			values[i++] = DirectFunctionCall1(inet_in, CStringGetDatum(current->local_ip_str));

		/* local port */
		values[i++] = Int32GetDatum(current->local_port);

		/* remote ip */
		if (strcmp(current->remote_ip_str, "INVALID_IP") == 0)
			nulls[i++] = true;
		else
			values[i++] = DirectFunctionCall1(inet_in, CStringGetDatum(current->remote_ip_str));

		/* remote port */
		values[i++] = Int32GetDatum(current->remote_port);

		/* state */
		state_str = "UNKNOWN";
		if (current->state > 0 && current->state < TCP_MAX_STATE)
			state_str = tcp_states_map[current->state];
		values[i++] = CStringGetTextDatum(state_str);

		/* build big JSON with detailed information */
		resetStringInfo(&json_buf);
		appendStringInfoChar(&json_buf, '{');
		has_data = false;
		has_q_data = false;

		diag_info = find_netlink_info(nldiag_map,
									  current->local_ip_str, current->local_port,
									  current->remote_ip_str, current->remote_port
			);

		if (diag_info)
		{
			/* add TCP timer information */
			appendStringInfo(&json_buf, "\"timer\": \"(%s)\", ", diag_info->tcp_timer_str);

			/* add low-level TCP stats for troubleshooting */
			if (diag_info->has_tcpi)
			{
				struct tcp_info *tcpi = &diag_info->tcpi;


#define appendTcpinfoMember(var, type) \
				appendStringInfo(&json_buf, "\"%s\": " type ", ", #var, tcpi->tcpi_##var)

#define appendCalculation(var, type, calc) \
				appendStringInfo(&json_buf, "\"%s\": " type ", ", #var, calc)

#define appendLastTcpinfoMember(var, type) \
				appendStringInfo(&json_buf, "\"%s\": " type, #var, tcpi->tcpi_##var)

				/*
				 * XXX: with INET_DIAG_VEGASINFO the "rtt" could also be also
				 * taken from struct tcpvegas_info(?)
				 */
				appendCalculation(rtt, "%.3f", (double) tcpi->tcpi_rtt / 1000.0);
				appendCalculation(rttvar, "%.3f", (double) tcpi->tcpi_rttvar / 1000.0);
				appendCalculation(rcv_rtt, "%.3f", (double) tcpi->tcpi_rcv_rtt / 1000.0);
				appendCalculation(ato, "%.3f", (double) tcpi->tcpi_ato / 1000.0);
				appendCalculation(min_rtt, "%.3f", (double) tcpi->tcpi_min_rtt / 1000.0);

				appendTcpinfoMember(snd_cwnd, "%u");
				appendTcpinfoMember(snd_cwnd, "%u");
				appendTcpinfoMember(sndbuf_limited, "%llu");
				appendTcpinfoMember(rwnd_limited, "%llu");
				appendTcpinfoMember(delivery_rate, "%llu");
				appendTcpinfoMember(state, "%u");
				appendTcpinfoMember(ca_state, "%u");
				appendTcpinfoMember(retransmits, "%u");
				appendTcpinfoMember(probes, "%u");
				appendTcpinfoMember(options, "%u");
				appendTcpinfoMember(snd_wscale, "%u");
				appendTcpinfoMember(rcv_wscale, "%u");
				appendTcpinfoMember(delivery_rate_app_limited, "%u");
				appendTcpinfoMember(total_rto, "%u");
				appendTcpinfoMember(total_rto_recoveries, "%u");
				appendTcpinfoMember(rto, "%u");
				appendTcpinfoMember(snd_mss, "%u");
				appendTcpinfoMember(rcv_mss, "%u");
				appendTcpinfoMember(unacked, "%u");
				appendTcpinfoMember(sacked, "%u");
				appendTcpinfoMember(lost, "%u");
				appendTcpinfoMember(retrans, "%u");
				appendTcpinfoMember(fackets, "%u");
				appendTcpinfoMember(last_data_sent, "%u");
				appendTcpinfoMember(last_ack_sent, "%u");
				appendTcpinfoMember(last_data_recv, "%u");
				appendTcpinfoMember(last_ack_recv, "%u");
				appendTcpinfoMember(pmtu, "%u");
				appendTcpinfoMember(rcv_ssthresh, "%u");
				appendTcpinfoMember(snd_ssthresh, "%u");
				appendTcpinfoMember(snd_cwnd, "%u");
				appendTcpinfoMember(advmss, "%u");
				appendTcpinfoMember(reordering, "%u");
				appendTcpinfoMember(rcv_space, "%u");
				appendTcpinfoMember(total_retrans, "%u");
				appendTcpinfoMember(segs_out, "%u");
				appendTcpinfoMember(segs_in, "%u");
				appendTcpinfoMember(notsent_bytes, "%u");
				appendTcpinfoMember(data_segs_out, "%u");
				appendTcpinfoMember(data_segs_in, "%u");
				appendTcpinfoMember(delivered, "%u");
				appendTcpinfoMember(delivered_ce, "%u");
				appendTcpinfoMember(dsack_dups, "%u");
				appendTcpinfoMember(reord_seen, "%u");
				appendTcpinfoMember(rcv_ooopack, "%u");
				appendTcpinfoMember(snd_wnd, "%u");
				appendTcpinfoMember(rcv_wnd, "%u");
				appendTcpinfoMember(rehash, "%u");
				appendTcpinfoMember(total_rto_time, "%u");
				appendTcpinfoMember(pacing_rate, "%llu");
				appendTcpinfoMember(max_pacing_rate, "%llu");
				appendTcpinfoMember(bytes_acked, "%llu");
				appendTcpinfoMember(bytes_received, "%llu");
				appendTcpinfoMember(delivery_rate, "%llu");
				appendTcpinfoMember(busy_time, "%llu");
				appendTcpinfoMember(rwnd_limited, "%llu");
				appendTcpinfoMember(sndbuf_limited, "%llu");
				appendTcpinfoMember(bytes_sent, "%llu");
				appendLastTcpinfoMember(bytes_retrans, "%llu");

				has_data = true;
			}

			/* add detailed TCP buffer sizes as seen by the kerne */
			if (diag_info->has_skmem)
			{
				if (has_data)
					appendStringInfoString(&json_buf, ", ");
				skmem = diag_info->skmem;
				appendStringInfo(&json_buf,
								 "\"skmem\": {\"rmem_alloc\": %u, \"rcvbuf\": %u, \"wmem_alloc\": %u, \"sndbuf\": %u, \"fwd_alloc\": %u, \"wmem_queued\": %u, \"optmem\": %u}",
								 skmem[SK_MEMINFO_RMEM_ALLOC],
								 skmem[SK_MEMINFO_RCVBUF],
								 skmem[SK_MEMINFO_WMEM_ALLOC],
								 skmem[SK_MEMINFO_SNDBUF],
								 skmem[SK_MEMINFO_FWD_ALLOC],
								 skmem[SK_MEMINFO_WMEM_QUEUED],
								 skmem[SK_MEMINFO_OPTMEM]);

				recvq = skmem[SK_MEMINFO_RMEM_ALLOC];
				sendq = skmem[SK_MEMINFO_WMEM_ALLOC];

				has_data = true;
				has_q_data = true;
			}

			/* also add TCP congestion used for the connection */
			if (diag_info->has_cong)
			{
				if (has_data)
					appendStringInfoString(&json_buf, ", ");

				/* See info about TCP_CONGESTION in tcp(7). */
				appendStringInfo(&json_buf, "\"congestion\": \"%s\"", diag_info->cong);

				has_data = true;
			}
		}
		appendStringInfoChar(&json_buf, '}');

		/* Fill the queues: sendq and recvq */
		if (!has_q_data)
		{
			nulls[i++] = true;
			nulls[i++] = true;
		}
		else
		{
			values[i++] = Int32GetDatum(recvq);
			values[i++] = Int32GetDatum(sendq);
		}

		/* Fill in the main tcpinfo JSON coolumn */
		if (!has_data)
			nulls[i++] = true;
		else
			values[i++] = DirectFunctionCall1(jsonb_in, CStringGetDatum(json_buf.data));

		/* Store the tuple */
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		current = current->next;
	}

	PG_RETURN_VOID();
}

#else

/* On anything else than Linux. */
PG_FUNCTION_INFO_V1(pg_stat_get_tcpinfo);
Datum
pg_stat_get_tcpinfo(PG_FUNCTION_ARGS)
{
	elog(ERROR, "pg_stat_tcpinfo is not supported on this platform");
}
#endif
