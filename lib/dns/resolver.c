/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <config.h>

#include <isc/assertions.h>
#include <isc/boolean.h>
#include <isc/error.h>
#include <isc/result.h>
#include <isc/timer.h>
#include <isc/mutex.h>
#include <isc/event.h>
#include <isc/task.h>
#include <isc/stdtime.h>
#include <isc/util.h>

#include <dns/types.h>
#include <dns/adb.h>
#include <dns/result.h>
#include <dns/name.h>
#include <dns/db.h>
#include <dns/events.h>
#include <dns/message.h>
#include <dns/ncache.h>
#include <dns/dispatch.h>
#include <dns/resolver.h>
#include <dns/rdata.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/tsig.h>
#include <dns/view.h>
#include <dns/log.h>
#include <dst/dst.h>

#define DNS_RESOLVER_TRACE
#ifdef DNS_RESOLVER_TRACE
#define RTRACE(m)	isc_log_write(dns_lctx, \
				      DNS_LOGCATEGORY_RESOLVER, \
				      DNS_LOGMODULE_RESOLVER, \
				      ISC_LOG_DEBUG(3), \
				      "res %p: %s", res, (m))
#define RRTRACE(r, m)	isc_log_write(dns_lctx, \
				      DNS_LOGCATEGORY_RESOLVER, \
				      DNS_LOGMODULE_RESOLVER, \
				      ISC_LOG_DEBUG(3), \
				      "res %p: %s", (r), (m))
#define FCTXTRACE(m)	isc_log_write(dns_lctx, \
				      DNS_LOGCATEGORY_RESOLVER, \
				      DNS_LOGMODULE_RESOLVER, \
				      ISC_LOG_DEBUG(3), \
				      "fctx %p: %s", fctx, (m))
#define FTRACE(m)	isc_log_write(dns_lctx, \
				      DNS_LOGCATEGORY_RESOLVER, \
				      DNS_LOGMODULE_RESOLVER, \
				      ISC_LOG_DEBUG(3), \
				      "fetch %p (fctx %p): %s", \
				      fetch, fetch->private, (m))
#define QTRACE(m)	isc_log_write(dns_lctx, \
				      DNS_LOGCATEGORY_RESOLVER, \
				      DNS_LOGMODULE_RESOLVER, \
				      ISC_LOG_DEBUG(3), \
				      "resquery %p (fctx %p): %s", \
				      query, query->fctx, (m))
#else
#define RTRACE(m)
#define RRTRACE(r, m)
#define FCTXTRACE(m)
#define FTRACE(m)
#define QTRACE(m)
#endif

/*
 * Maximum EDNS0 input packet size.
 */
#define SEND_BUFFER_SIZE		2048		/* XXXRTH  Constant. */

typedef struct fetchctx fetchctx_t;

typedef struct query {
	/* Locked by task event serialization. */
	unsigned int			magic;
	fetchctx_t *			fctx;
	dns_dispatch_t *		dispatch;
	dns_adbaddrinfo_t *		addrinfo;
	isc_time_t			start;
	dns_messageid_t			id;
	dns_dispentry_t *		dispentry;
	ISC_LINK(struct query)		link;
	isc_buffer_t			buffer;
	dns_rdata_any_tsig_t		*tsig;
	dns_tsigkey_t			*tsigkey;
	unsigned int			options;
	unsigned int			attributes;
	unsigned char			data[512];
} resquery_t;

#define QUERY_MAGIC			0x51212121U	/* Q!!! */
#define VALID_QUERY(query)		((query) != NULL && \
					 (query)->magic == QUERY_MAGIC)

#define RESQUERY_ATTR_CONNECTING	0x01
#define RESQUERY_ATTR_CANCELED		0x02

#define RESQUERY_CONNECTING(q)		(((q)->attributes & \
					  RESQUERY_ATTR_CONNECTING) != 0)
#define RESQUERY_CANCELED(q)		(((q)->attributes & \
					  RESQUERY_ATTR_CANCELED) != 0)

typedef enum {
	fetchstate_init = 0,		/* Start event has not run yet. */
	fetchstate_active,
	fetchstate_done			/* FETCHDONE events posted. */
} fetchstate;

struct fetchctx {
	/* Not locked. */
	unsigned int			magic;
	dns_resolver_t *		res;
	dns_name_t			name;
	dns_rdatatype_t			type;
	unsigned int			options;
	isc_task_t *			task;			/* XXX??? */
	unsigned int			bucketnum;
	/* Locked by appropriate bucket lock. */
	fetchstate			state;
	isc_boolean_t			want_shutdown;
	unsigned int			references;
	isc_event_t			control_event;		/* locked? */
	ISC_LINK(struct fetchctx)	link;
	ISC_LIST(dns_fetchevent_t)	events;
	/* Locked by task event serialization. */
	dns_name_t			domain;
	dns_rdataset_t			nameservers;
	unsigned int			attributes;
	isc_timer_t *			timer;
	isc_time_t			expires;
	isc_interval_t			interval;
	dns_message_t *			qmessage;
	dns_message_t *			rmessage;
	ISC_LIST(resquery_t)		queries;
	dns_adbfindlist_t		finds;
	dns_adbfind_t *			find;
	dns_adbaddrinfolist_t		forwaddrs;
	isc_sockaddrlist_t		forwarders;
	/*
	 * # of events we're waiting for.
	 */
	unsigned int			pending;
	unsigned int			validating;
	unsigned int			restarts;
};

#define FCTX_MAGIC			0x46212121U	/* F!!! */
#define VALID_FCTX(fctx)		((fctx) != NULL && \
					 (fctx)->magic == FCTX_MAGIC)

#define FCTX_ATTR_HAVEANSWER		0x01
#define FCTX_ATTR_GLUING		0x02
#define FCTX_ATTR_ADDRWAIT		0x04
#define FCTX_ATTR_SHUTTINGDOWN		0x08
#define FCTX_ATTR_WANTCACHE		0x10
#define FCTX_ATTR_WANTNCACHE		0x20

#define HAVE_ANSWER(f)		(((f)->attributes & FCTX_ATTR_HAVEANSWER) != \
				 0)
#define GLUING(f)		(((f)->attributes & FCTX_ATTR_GLUING) != \
				 0)
#define ADDRWAIT(f)		(((f)->attributes & FCTX_ATTR_ADDRWAIT) != \
				 0)
#define SHUTTINGDOWN(f)		(((f)->attributes & FCTX_ATTR_SHUTTINGDOWN) \
 				 != 0)
#define WANTCACHE(f)		(((f)->attributes & FCTX_ATTR_WANTCACHE) != 0)
#define WANTNCACHE(f)		(((f)->attributes & FCTX_ATTR_WANTNCACHE) != 0)

struct dns_fetch {
	unsigned int			magic;
	void *				private;
};

#define DNS_FETCH_MAGIC			0x46746368U	/* Ftch */
#define DNS_FETCH_VALID(fetch)		((fetch) != NULL && \
					 (fetch)->magic == DNS_FETCH_MAGIC)

typedef struct fctxbucket {
	isc_task_t *			task;
	isc_mutex_t			lock;
	ISC_LIST(fetchctx_t)		fctxs;
	isc_boolean_t			exiting;
} fctxbucket_t;

struct dns_resolver {
	/* Unlocked. */
	unsigned int			magic;
	isc_mem_t *			mctx;
	isc_mutex_t			lock;
	dns_rdataclass_t		rdclass;
	isc_socketmgr_t *		socketmgr;
	isc_timermgr_t *		timermgr;
	dns_view_t *			view;
	isc_boolean_t			frozen;
	isc_sockaddrlist_t		forwarders;
	dns_fwdpolicy_t			fwdpolicy;
	isc_socket_t *			udpsocket4;
	isc_socket_t *			udpsocket6;
	dns_dispatch_t *		dispatch4;
	dns_dispatch_t *		dispatch6;
	unsigned int			nbuckets;
	fctxbucket_t *			buckets;
	/* Locked by lock. */
	unsigned int			references;
	isc_boolean_t			exiting;
	isc_eventlist_t			whenshutdown;
	unsigned int			activebuckets;
};

#define RES_MAGIC			0x52657321U	/* Res! */
#define VALID_RESOLVER(res)		((res) != NULL && \
					 (res)->magic == RES_MAGIC)

/*
 * Private addrinfo flags.  These must not conflict with DNS_FETCHOPT_NOEDNS0,
 * which we also use as an addrinfo flag.
 */
#define FCTX_ADDRINFO_MARK		0x0001
#define FCTX_ADDRINFO_FORWARDER		0x1000
#define UNMARKED(a)			(((a)->flags & FCTX_ADDRINFO_MARK) \
					 == 0)
#define ISFORWARDER(a)			(((a)->flags & \
					 FCTX_ADDRINFO_FORWARDER) != 0)

static void destroy(dns_resolver_t *res);
static void empty_bucket(dns_resolver_t *res);
static isc_result_t resquery_send(resquery_t *query);
static void resquery_response(isc_task_t *task, isc_event_t *event);
static void resquery_connected(isc_task_t *task, isc_event_t *event);
static void fctx_try(fetchctx_t *fctx);
static isc_boolean_t fctx_destroy(fetchctx_t *fctx);

static inline isc_result_t
fctx_starttimer(fetchctx_t *fctx) {
	return (isc_timer_reset(fctx->timer, isc_timertype_once,
				&fctx->expires, &fctx->interval,
				ISC_FALSE));
}

static inline isc_result_t
fctx_stopidletimer(fetchctx_t *fctx) {
	return (isc_timer_reset(fctx->timer, isc_timertype_once,
				&fctx->expires, NULL,
				ISC_FALSE));
}

static inline void
fctx_stoptimer(fetchctx_t *fctx) {
	isc_result_t result;

	/*
	 * We don't return a result if resetting the timer to inactive fails
	 * since there's nothing to be done about it.  Resetting to inactive
	 * should never fail anyway, since the code as currently written
	 * cannot fail in that case.
	 */
	result = isc_timer_reset(fctx->timer, isc_timertype_inactive,
				  NULL, NULL, ISC_TRUE);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_timer_reset(): %s",
				 isc_result_totext(result));
	}
}

static inline void
resquery_destroy(resquery_t **queryp) {
	resquery_t *query;
	
	REQUIRE(queryp != NULL);
	query = *queryp;
	REQUIRE(!ISC_LINK_LINKED(query, link));

	query->magic = 0;
	isc_mem_put(query->fctx->res->mctx, query, sizeof *query);
	*queryp = NULL;
}

static void
fctx_cancelquery(resquery_t **queryp, dns_dispatchevent_t **deventp,
		 isc_time_t *finish, isc_boolean_t no_response)
{
	fetchctx_t *fctx;
	resquery_t *query;
	unsigned int rtt;
	unsigned int factor;
	isc_socket_t *socket;

	query = *queryp;
	fctx = query->fctx;

	FCTXTRACE("cancelquery");

	REQUIRE(!RESQUERY_CANCELED(query));

	query->attributes |= RESQUERY_ATTR_CANCELED;

	/*
	 * Should we update the RTT?
	 */
	if (finish != NULL || no_response) {
		if (finish != NULL) {
			/*
			 * We have both the start and finish times for this
			 * packet, so we can compute a real RTT.
			 */
			rtt = (unsigned int)isc_time_microdiff(finish,
							       &query->start);
			factor = DNS_ADB_RTTADJDEFAULT;
		} else {
			/*
			 * We don't have an RTT for this query.  Maybe the
			 * packet was lost, or maybe this server is very
			 * slow.  We don't know.  Increase the RTT.
			 */
			INSIST(no_response);
			rtt = query->addrinfo->srtt +
				(100000 * fctx->restarts);
			if (rtt > 10000000)
				rtt = 10000000;
			/*
			 * Replace the current RTT with our value.
			 */
			factor = DNS_ADB_RTTADJREPLACE;
		}
		dns_adb_adjustsrtt(fctx->res->view->adb, query->addrinfo, rtt,
				   factor);
	}

	if (query->dispentry != NULL)
		dns_dispatch_removeresponse(query->dispatch, &query->dispentry,
					    deventp);
	ISC_LIST_UNLINK(fctx->queries, query, link);
	if (query->tsig != NULL)
		dns_rdata_freestruct(query->tsig);
	if (RESQUERY_CONNECTING(query)) {
		/*
		 * Cancel the connect.
		 */
		socket = dns_dispatch_getsocket(query->dispatch);
		isc_socket_cancel(socket, NULL, ISC_SOCKCANCEL_CONNECT);
	}
	dns_dispatch_detach(&query->dispatch);
	if (!RESQUERY_CONNECTING(query)) {
		/*
		 * It's safe to destroy the query now.
		 */
		resquery_destroy(&query);
	}
}

static void
fctx_cancelqueries(fetchctx_t *fctx, isc_boolean_t no_response) {
	resquery_t *query, *next_query;

	FCTXTRACE("cancelqueries");

	for (query = ISC_LIST_HEAD(fctx->queries);
	     query != NULL;
	     query = next_query) {
		next_query = ISC_LIST_NEXT(query, link);
		fctx_cancelquery(&query, NULL, NULL, no_response);
	}
}

static void
fctx_cleanupfinds(fetchctx_t *fctx) {
	dns_adbfind_t *find, *next_find;

	REQUIRE(ISC_LIST_EMPTY(fctx->queries));

	for (find = ISC_LIST_HEAD(fctx->finds);
	     find != NULL;
	     find = next_find) {
		next_find = ISC_LIST_NEXT(find, publink);
		ISC_LIST_UNLINK(fctx->finds, find, publink);
		dns_adb_destroyfind(&find);
	}
	fctx->find = NULL;
}

static void
fctx_cleanupforwaddrs(fetchctx_t *fctx) {
	dns_adbaddrinfo_t *addr, *next_addr;

	REQUIRE(ISC_LIST_EMPTY(fctx->queries));

	for (addr = ISC_LIST_HEAD(fctx->forwaddrs);
	     addr != NULL;
	     addr = next_addr) {
		next_addr = ISC_LIST_NEXT(addr, publink);
		ISC_LIST_UNLINK(fctx->forwaddrs, addr, publink);
		dns_adb_freeaddrinfo(fctx->res->view->adb, &addr);
	}
}

static inline void
fctx_stopeverything(fetchctx_t *fctx) {
	FCTXTRACE("stopeverything");
	fctx_cancelqueries(fctx, ISC_FALSE);
	fctx_cleanupfinds(fctx);
	fctx_cleanupforwaddrs(fctx);
	fctx_stoptimer(fctx);
}

static inline void
fctx_sendevents(fetchctx_t *fctx, isc_result_t result) {
	dns_fetchevent_t *event, *next_event;
	isc_task_t *task;

	/*
	 * Caller must be holding the appropriate bucket lock.
	 */
	REQUIRE(fctx->state == fetchstate_done);

	FCTXTRACE("sendevents");

	for (event = ISC_LIST_HEAD(fctx->events);
	     event != NULL;
	     event = next_event) {
		next_event = ISC_LIST_NEXT(event, link);
		task = event->sender;
		event->sender = fctx;
		if (!HAVE_ANSWER(fctx))
			event->result = result;
		isc_task_sendanddetach(&task, (isc_event_t **)&event);
	}
	ISC_LIST_INIT(fctx->events);
}

static void
fctx_done(fetchctx_t *fctx, isc_result_t result) {
	dns_resolver_t *res;

	FCTXTRACE("done");

	res = fctx->res;

	fctx_stopeverything(fctx);

	LOCK(&res->buckets[fctx->bucketnum].lock);

	fctx->state = fetchstate_done;
	fctx_sendevents(fctx, result);

	UNLOCK(&res->buckets[fctx->bucketnum].lock);
}

static void
resquery_senddone(isc_task_t *task, isc_event_t *event) {
	isc_socketevent_t *sevent = (isc_socketevent_t *)event;
	resquery_t *query = event->arg;

	REQUIRE(event->type == ISC_SOCKEVENT_SENDDONE);

	QTRACE("senddone");

	/*
	 * XXXRTH
	 *
	 * Currently we don't wait for the senddone event before retrying
	 * a query.  This means that if we get really behind, we may end
	 * up doing extra work!
	 */

	(void)task;

	if (sevent->result != ISC_R_SUCCESS)
		fctx_cancelquery(&query, NULL, NULL, ISC_FALSE);
				 
	isc_event_free(&event);
}

static inline isc_result_t
fctx_addopt(dns_message_t *message) {
	dns_rdataset_t *rdataset;
	dns_rdatalist_t *rdatalist;
	dns_rdata_t *rdata;
	isc_result_t result;

	rdatalist = NULL;
	result = dns_message_gettemprdatalist(message, &rdatalist);
	if (result != ISC_R_SUCCESS)
		return (result);
	rdata = NULL;
	result = dns_message_gettemprdata(message, &rdata);
	if (result != ISC_R_SUCCESS)
		return (result);
	rdataset = NULL;
	result = dns_message_gettemprdataset(message, &rdataset);
	if (result != ISC_R_SUCCESS)
		return (result);
	dns_rdataset_init(rdataset);

	rdatalist->type = dns_rdatatype_opt;
	rdatalist->covers = 0;

	/*
	 * Set Maximum UDP buffer size.
	 */
	rdatalist->rdclass = SEND_BUFFER_SIZE;

	/*
	 * Set EXTENDED-RCODE, VERSION, and Z to 0.
	 */
	rdatalist->ttl = 0;

	/*
	 * No ENDS options.
	 */
	rdata->data = NULL;
	rdata->length = 0;

	ISC_LIST_INIT(rdatalist->rdata);
	ISC_LIST_APPEND(rdatalist->rdata, rdata, link);
	dns_rdatalist_tordataset(rdatalist, rdataset);

	return (dns_message_setopt(message, rdataset));
}

static inline void
fctx_setretryinterval(fetchctx_t *fctx, unsigned int rtt) {
	unsigned int seconds;

	/*
	 * We retry every 2 seconds the first two times through the address
	 * list, and then we do exponential back-off.
	 */
	if (fctx->restarts < 3)
		seconds = 2;
	else
		seconds = (2 << (fctx->restarts - 1));

	/*
	 * Double the round-trip time and convert to seconds.
	 */
	rtt /= 500000;
	
	/*
	 * Always wait for at least the doubled round-trip time.
	 */
	if (seconds < rtt)
		seconds = rtt;

	/*
	 * But don't ever wait for more than 30 seconds.
	 */
	if (seconds > 30)
		seconds = 30;

	isc_interval_set(&fctx->interval, seconds, 0);
}

static isc_result_t
fctx_query(fetchctx_t *fctx, dns_adbaddrinfo_t *addrinfo,
	   unsigned int options)
{
	dns_resolver_t *res;
	isc_task_t *task;
	isc_result_t result;
	resquery_t *query;
	isc_socket_t *socket;

	FCTXTRACE("query");

	res = fctx->res;
	task = res->buckets[fctx->bucketnum].task;

	fctx_setretryinterval(fctx, addrinfo->srtt);
	result = fctx_starttimer(fctx);
	if (result != ISC_R_SUCCESS)
		return (result);

	dns_message_reset(fctx->rmessage, DNS_MESSAGE_INTENTPARSE);

	query = isc_mem_get(res->mctx, sizeof *query);
	if (query == NULL) {
		result = ISC_R_NOMEMORY; 
		goto stop_timer;
	}
	query->options = options;
	query->attributes = 0;
	/*
	 * Note that the caller MUST guarantee that 'addrinfo' will remain
	 * valid until this query is canceled.
	 */
	query->addrinfo = addrinfo;
	result = isc_time_now(&query->start);
	if (result != ISC_R_SUCCESS)
		goto cleanup_query;
	
	/*
	 * If this is a TCP query, then we need to make a socket and
	 * a dispatch for it here.  Otherwise we use the resolver's
	 * shared dispatch.
	 */
	query->dispatch = NULL;
	if ((query->options & DNS_FETCHOPT_TCP) != 0) {
		socket = NULL;
		result = isc_socket_create(res->socketmgr,
					   isc_sockaddr_pf(addrinfo->sockaddr),
					   isc_sockettype_tcp,
					   &socket);
		if (result != ISC_R_SUCCESS)
			goto cleanup_query;
		result = dns_dispatch_create(res->mctx, socket, task,
					     4096, 2, 1, 1, 3, NULL,
					     &query->dispatch);
		/*
		 * Regardless of whether dns_dispatch_create() succeeded or
		 * not, we don't need our reference to the socket anymore.
		 */
		isc_socket_detach(&socket);
		if (result != ISC_R_SUCCESS)
			goto cleanup_dispatch;
	} else {
		switch (isc_sockaddr_pf(addrinfo->sockaddr)) {
		case PF_INET:
			dns_dispatch_attach(res->dispatch4, &query->dispatch);
			break;
		case PF_INET6:
			dns_dispatch_attach(res->dispatch6, &query->dispatch);
			break;
		default:
			result = DNS_R_NOTIMPLEMENTED;
			goto cleanup_dispatch;
		}
		/*
		 * We should always have a valid dispatcher here.  If we
		 * don't support a protocol family, then its dispatcher
		 * will be NULL, but we shouldn't be finding addresses for
		 * protocol types we don't support, so the dispatcher
		 * we found should never be NULL.
		 */
		INSIST(query->dispatch != NULL);
	}

	query->dispentry = NULL;
	query->fctx = fctx;
	query->tsig = NULL;
	query->tsigkey = NULL;
	query->magic = QUERY_MAGIC;

	if ((query->options & DNS_FETCHOPT_TCP) != 0) {
		/*
		 * Connect to the remote server.
		 *
		 * XXXRTH  Should we attach to the socket?
		 */
		socket = dns_dispatch_getsocket(query->dispatch);
		result = isc_socket_connect(socket, addrinfo->sockaddr,
					    task, resquery_connected, query);
		if (result != ISC_R_SUCCESS)
			goto cleanup_dispatch;
		query->attributes |= RESQUERY_ATTR_CONNECTING;
		QTRACE("connecting via TCP");
	} else {
		result = resquery_send(query);
		if (result != ISC_R_SUCCESS)
			goto cleanup_dispatch;
	}

	ISC_LIST_APPEND(fctx->queries, query, link);

	return (ISC_R_SUCCESS);

 cleanup_dispatch:
	dns_dispatch_detach(&query->dispatch);

 cleanup_query:
	query->magic = 0;
	isc_mem_put(res->mctx, query, sizeof *query);

 stop_timer:
	fctx_stoptimer(fctx);

	return (result);
}

static isc_result_t
resquery_send(resquery_t *query) {
	fetchctx_t *fctx;
	isc_result_t result;
	dns_rdataset_t *qrdataset, *trdataset;
	dns_name_t *qname;
	isc_region_t r;
	dns_resolver_t *res;
	isc_task_t *task;
	isc_socket_t *socket;
	isc_buffer_t tcpbuffer;
	isc_sockaddr_t *address;
	isc_buffer_t *buffer;

	fctx = query->fctx;
	QTRACE("send");

	res = fctx->res;
	task = res->buckets[fctx->bucketnum].task;
	address = NULL;

	if ((query->options & DNS_FETCHOPT_TCP) != 0) {
		/*
		 * Reserve space for the TCP message length.
		 */
		isc_buffer_init(&tcpbuffer, query->data,
				sizeof query->data, ISC_BUFFERTYPE_BINARY);
		isc_buffer_init(&query->buffer, query->data + 2,
				sizeof query->data - 2,
				ISC_BUFFERTYPE_BINARY);
		buffer = &tcpbuffer;
	} else {
		isc_buffer_init(&query->buffer, query->data,
				sizeof query->data, ISC_BUFFERTYPE_BINARY);
		buffer = &query->buffer;
	}

	qname = NULL;
	result = dns_message_gettempname(fctx->qmessage, &qname);
	if (result != ISC_R_SUCCESS)
		goto cleanup_temps;
	qrdataset = NULL;
	result = dns_message_gettemprdataset(fctx->qmessage, &qrdataset);
	if (result != ISC_R_SUCCESS)
		goto cleanup_temps;

	/*
	 * Get a query id from the dispatch.
	 */
	result = dns_dispatch_addresponse(query->dispatch,
					  query->addrinfo->sockaddr,
					  task,
					  resquery_response,
					  query,
					  &query->id,
					  &query->dispentry);
	if (result != ISC_R_SUCCESS)
		goto cleanup_temps;

	fctx->qmessage->opcode = dns_opcode_query;

	/*
	 * Set up question.
	 */
	dns_name_init(qname, NULL);
	dns_name_clone(&fctx->name, qname);
	dns_rdataset_init(qrdataset);
	dns_rdataset_makequestion(qrdataset, res->rdclass, fctx->type);
	ISC_LIST_APPEND(qname->list, qrdataset, link);
	dns_message_addname(fctx->qmessage, qname, DNS_SECTION_QUESTION);

	/*
	 * Set RD if the client has requested that we do a recursive query,
	 * or if we're sending to a forwarder.
	 */
	if ((query->options & DNS_FETCHOPT_RECURSIVE) != 0 ||
	    ISFORWARDER(query->addrinfo))
		fctx->qmessage->flags |= DNS_MESSAGEFLAG_RD;

	/*
	 * We don't have to set opcode because it defaults to query.
	 */
	fctx->qmessage->id = query->id;

	/*
	 * Convert the question to wire format.
	 */
	result = dns_message_renderbegin(fctx->qmessage, &query->buffer);
	if (result != ISC_R_SUCCESS)
		goto cleanup_message;

	result = dns_message_rendersection(fctx->qmessage,
					   DNS_SECTION_QUESTION, 0);
	if (result != ISC_R_SUCCESS)
		goto cleanup_message;

	/*
	 * Use EDNS0, unless the caller doesn't want it, or we know that
	 * the remote server doesn't like it.
	 */
	if ((query->options & DNS_FETCHOPT_NOEDNS0) == 0) {
		if ((query->addrinfo->flags & DNS_FETCHOPT_NOEDNS0) == 0) {
			trdataset = NULL;
			result = fctx_addopt(fctx->qmessage);
			if (result != ISC_R_SUCCESS) {
				/*
				 * We couldn't add the OPT, but we'll press on.
				 * We're not using EDNS0, so set the NOEDNS0
				 * bit.
				 */
				query->options |= DNS_FETCHOPT_NOEDNS0;
			}
		} else {
			/*
			 * We know this server doesn't like EDNS0, so we
			 * won't use it.  Set the NOEDNS0 bit since we're
			 * not using EDNS0.
			 */
			query->options |= DNS_FETCHOPT_NOEDNS0;
		}
	}

	/*
	 * XXXRTH  Add TSIG record tailored to the current recipient?
	 */

	result = dns_message_rendersection(fctx->qmessage,
					   DNS_SECTION_ADDITIONAL, 0);
	if (result != ISC_R_SUCCESS)
		goto cleanup_message;

	result = dns_message_renderend(fctx->qmessage);
	if (result != ISC_R_SUCCESS)
		goto cleanup_message;

	if (fctx->qmessage->tsigkey != NULL) {
		query->tsigkey = fctx->qmessage->tsigkey;
		query->tsig = fctx->qmessage->tsig;
		fctx->qmessage->tsig = NULL;
	}

	/*
	 * If using TCP, write the length of the message at the beginning
	 * of the buffer.
	 */
	if ((query->options & DNS_FETCHOPT_TCP) != 0) {
		isc_buffer_used(&query->buffer, &r);
		isc_buffer_putuint16(&tcpbuffer, (isc_uint16_t)r.length);
		isc_buffer_add(&tcpbuffer, r.length);
	}

	/*
	 * We're now done with the query message.
	 */
	dns_message_reset(fctx->qmessage, DNS_MESSAGE_INTENTRENDER);

	socket = dns_dispatch_getsocket(query->dispatch);
	/*
	 * Send the query!
	 */
	if ((query->options & DNS_FETCHOPT_TCP) == 0)
		address = query->addrinfo->sockaddr;
	isc_buffer_used(buffer, &r);
	result = isc_socket_sendto(socket, &r, task, resquery_senddone,
				   query, address, NULL);
	if (result != ISC_R_SUCCESS)
		goto cleanup_message;
	QTRACE("sent");

	return (ISC_R_SUCCESS);

 cleanup_message:
	dns_message_reset(fctx->qmessage, DNS_MESSAGE_INTENTRENDER);

	/*
	 * Stop the dispatcher from listening.
	 */
	dns_dispatch_removeresponse(query->dispatch,
				    &query->dispentry,
				    NULL);

 cleanup_temps:
	if (qname != NULL)
		dns_message_puttempname(fctx->qmessage, &qname);
	if (qrdataset != NULL)
		dns_message_puttemprdataset(fctx->qmessage, &qrdataset);

	return (result);
}

static void
resquery_connected(isc_task_t *task, isc_event_t *event) {
	isc_socketevent_t *sevent = (isc_socketevent_t *)event;
	resquery_t *query = event->arg;
	isc_result_t result;

	REQUIRE(event->type == ISC_SOCKEVENT_CONNECT);
	REQUIRE(VALID_QUERY(query));

	QTRACE("connected");

	(void)task;

	/*
	 * XXXRTH
	 *
	 * Currently we don't wait for the connect event before retrying
	 * a query.  This means that if we get really behind, we may end
	 * up doing extra work!
	 */

	query->attributes &= ~RESQUERY_ATTR_CONNECTING;

	if (RESQUERY_CANCELED(query)) {
		/*
		 * This query was canceled while the connect() was in
		 * progress.
		 */
		resquery_destroy(&query);
	} else {
		if (sevent->result == ISC_R_SUCCESS) {
			/*
			 * We are connected.  Send the query.
			 */
			result = resquery_send(query);
			if (result != ISC_R_SUCCESS)
				fctx_cancelquery(&query, NULL, NULL,
						 ISC_FALSE);
		} else
			fctx_cancelquery(&query, NULL, NULL, ISC_FALSE);
	}
				 
	isc_event_free(&event);
}

static void
fctx_finddone(isc_task_t *task, isc_event_t *event) {
	fetchctx_t *fctx;
	dns_adbfind_t *find;
	dns_resolver_t *res;
	isc_boolean_t want_try = ISC_FALSE;
	isc_boolean_t want_done = ISC_FALSE;
	isc_boolean_t bucket_empty = ISC_FALSE;
	unsigned int bucketnum;

	find = event->sender;
	fctx = event->arg;
	REQUIRE(VALID_FCTX(fctx));
	res = fctx->res;

	(void)task;

	FCTXTRACE("finddone");

	INSIST(fctx->pending > 0);
	fctx->pending--;

	if (ADDRWAIT(fctx)) {
		/*
		 * The fetch is waiting for a name to be found.
		 */
		fctx->attributes &= ~FCTX_ATTR_ADDRWAIT;
		if (event->type == DNS_EVENT_ADBMOREADDRESSES)
			want_try = ISC_TRUE;
		else if (fctx->pending == 0) {
			/*
			 * We've got nothing else to wait for and don't
			 * know the answer.  There's nothing to do but
			 * fail the fctx.
			 */
			want_done = ISC_TRUE;
		}
	} else if (SHUTTINGDOWN(fctx) && fctx->pending == 0 &&
		   fctx->validating == 0) {
		bucketnum = fctx->bucketnum;
		LOCK(&res->buckets[bucketnum].lock);
		/*
		 * Note that we had to wait until we had the lock before
		 * looking at fctx->references.
		 */
		if (fctx->references == 0)
			bucket_empty = fctx_destroy(fctx);
		UNLOCK(&res->buckets[bucketnum].lock);
	}

	isc_event_free(&event);
	dns_adb_destroyfind(&find);

	if (want_try)
		fctx_try(fctx);
	else if (want_done)
		fctx_done(fctx, ISC_R_FAILURE);
	else if (bucket_empty)
		empty_bucket(res);
}

static void
sort_adbfind(dns_adbfind_t *find) {
	dns_adbaddrinfo_t *best, *curr;
	dns_adbaddrinfolist_t sorted;

	/*
	 * Lame N^2 bubble sort.
	 */

	ISC_LIST_INIT(sorted);
	while (!ISC_LIST_EMPTY(find->list)) {
		best = ISC_LIST_HEAD(find->list);
		curr = ISC_LIST_NEXT(best, publink);
		while (curr != NULL) {
			if (curr->srtt < best->srtt)
				best = curr;
			curr = ISC_LIST_NEXT(curr, publink);
		}
		ISC_LIST_UNLINK(find->list, best, publink);
		ISC_LIST_APPEND(sorted, best, publink);
	} 
	find->list = sorted;
}

static void
sort_finds(fetchctx_t *fctx) {
	dns_adbfind_t *best, *curr;
	dns_adbfindlist_t sorted;
	dns_adbaddrinfo_t *addrinfo, *bestaddrinfo;

	/*
	 * Lame N^2 bubble sort.
	 */

	ISC_LIST_INIT(sorted);
	while (!ISC_LIST_EMPTY(fctx->finds)) {
		best = ISC_LIST_HEAD(fctx->finds);
		bestaddrinfo = ISC_LIST_HEAD(best->list);
		INSIST(bestaddrinfo != NULL);
		curr = ISC_LIST_NEXT(best, publink);
		while (curr != NULL) {
			addrinfo = ISC_LIST_HEAD(curr->list);
			INSIST(addrinfo != NULL);
			if (addrinfo->srtt < bestaddrinfo->srtt) {
				best = curr;
				bestaddrinfo = addrinfo;
			}
			curr = ISC_LIST_NEXT(curr, publink);
		}
		ISC_LIST_UNLINK(fctx->finds, best, publink);
		ISC_LIST_APPEND(sorted, best, publink);
	}
	fctx->finds = sorted;
}

static isc_result_t
fctx_getaddresses(fetchctx_t *fctx) {
	dns_rdata_t rdata;
	isc_region_t r;
	dns_name_t name;
	isc_result_t result;
	dns_resolver_t *res;
	isc_stdtime_t now;
	dns_adbfind_t *find;
	unsigned int stdoptions, options;
	isc_sockaddr_t *sa;
	dns_adbaddrinfo_t *ai;

	FCTXTRACE("getaddresses");

	/*
	 * Don't pound on remote servers.  (Failsafe!)
	 */
	fctx->restarts++;
	if (fctx->restarts > 10) {
		FCTXTRACE("too many restarts");
		return (DNS_R_SERVFAIL);
	}

	res = fctx->res;

	/*
	 * Forwarders.
	 */

	INSIST(ISC_LIST_EMPTY(fctx->forwaddrs));

	/*
	 * If this fctx has forwarders, use them; otherwise the use
	 * resolver's forwarders (if any).
	 */
	sa = ISC_LIST_HEAD(fctx->forwarders);
	if (sa == NULL)
		sa = ISC_LIST_HEAD(res->forwarders);

	while (sa != NULL) {
		ai = NULL;
		result = dns_adb_findaddrinfo(fctx->res->view->adb,
					      sa, &ai);
		if (result == ISC_R_SUCCESS) {
			ai->flags |= FCTX_ADDRINFO_FORWARDER;
			ISC_LIST_APPEND(fctx->forwaddrs, ai, publink);
		}
		sa = ISC_LIST_NEXT(sa, link);
	}

	/*
	 * If the forwarding policy is "only", we don't need the addresses
	 * of the nameservers.
	 */
	if (res->fwdpolicy == dns_fwdpolicy_only)
		goto out;

	/*
	 * Normal nameservers.
	 */

	stdoptions = DNS_ADBFIND_WANTEVENT | DNS_ADBFIND_EMPTYEVENT |
		DNS_ADBFIND_AVOIDFETCHES;
	if (res->dispatch4 != NULL)
		stdoptions |= DNS_ADBFIND_INET;
	if (res->dispatch6 != NULL)
		stdoptions |= DNS_ADBFIND_INET6;
	isc_stdtime_get(&now);

	INSIST(ISC_LIST_EMPTY(fctx->finds));

	result = dns_rdataset_first(&fctx->nameservers);
	while (result == ISC_R_SUCCESS) {
		dns_rdataset_current(&fctx->nameservers, &rdata);
		/*
		 * Extract the name from the NS record.
		 */
		dns_rdata_toregion(&rdata, &r);
		dns_name_init(&name, NULL);
		dns_name_fromregion(&name, &r);
		options = stdoptions;
		/*
		 * If this name is a subdomain of the query domain, tell
		 * the ADB to start looking at "." if it doesn't know the
		 * address.  This keeps us from getting stuck if the
		 * nameserver is beneath the zone cut and we don't know its
		 * address (e.g. because the A record has expired).
		 * By restarting from ".", we ensure that any missing glue
		 * will be reestablished.
		 *
		 * A further optimization would be to get the ADB to start
		 * looking at the most enclosing zone cut above fctx->domain.
		 * We don't expect this situation to happen very frequently,
		 * so we've chosen the simple solution.
		 */
		if (dns_name_issubdomain(&name, &fctx->domain))
			options |= DNS_ADBFIND_STARTATROOT;
		/*
		 * See what we know about this address.
		 */
		find = NULL;
		result = dns_adb_createfind(res->view->adb,
					    res->buckets[fctx->bucketnum].task,
					    fctx_finddone, fctx, &name,
					    &fctx->domain, options, now,
					    &find);
		if (result != ISC_R_SUCCESS)
			return (result);
		if (!ISC_LIST_EMPTY(find->list)) {
			/*
			 * We have at least some of the addresses for the
			 * name.
			 */
			INSIST((find->options & DNS_ADBFIND_WANTEVENT) == 0);
			sort_adbfind(find);
			ISC_LIST_APPEND(fctx->finds, find, publink);
		} else {
			/*
			 * We don't know any of the addresses for this
			 * name.
			 */
			if ((find->options & DNS_ADBFIND_WANTEVENT) != 0) {
				/*
				 * We're looking for them and will get an
				 * event about it later.
				 */
				fctx->pending++;
			} else {
				/*
				 * And ADB isn't going to send us any events
				 * either.  This query loses.
				 */
				dns_adb_destroyfind(&find);
			}
		}
		result = dns_rdataset_next(&fctx->nameservers);
	}
	if (result != DNS_R_NOMORE)
		return (result);

 out:
	if (ISC_LIST_EMPTY(fctx->finds) && ISC_LIST_EMPTY(fctx->forwaddrs)) {
		/*
		 * We've got no addresses.
		 */
		if (fctx->pending > 0) {
			/*
			 * We're fetching the addresses, but don't have any
			 * yet.   Tell the caller to wait for an answer.
			 */
			result = DNS_R_WAIT;
		} else {
			/*
			 * We've lost completely.  We don't know any
			 * addresses, and the ADB has told us it can't get
			 * them.
			 */
			result = ISC_R_FAILURE;
		}
	} else {
		/*
		 * We've found some addresses.  We might still be looking
		 * for more addresses.
		 */
		/*
		 * XXXRTH  We could sort the forwaddrs here if the caller
		 *         wants to use the forwaddrs in "best order" as
		 *         opposed to "fixed order".
		 */
		sort_finds(fctx);
		result = ISC_R_SUCCESS;
	}

	return (result);
}

static inline dns_adbaddrinfo_t *
fctx_nextaddress(fetchctx_t *fctx) {
	dns_adbfind_t *find;
	dns_adbaddrinfo_t *addrinfo;

	/*
	 * Return the next untried address, if any.
	 */

	/*
	 * Find the first unmarked forwarder (if any).
	 */
	for (addrinfo = ISC_LIST_HEAD(fctx->forwaddrs);
	     addrinfo != NULL;
	     addrinfo = ISC_LIST_NEXT(addrinfo, publink)) {
		if (UNMARKED(addrinfo)) {
			addrinfo->flags |= FCTX_ADDRINFO_MARK;
			fctx->find = NULL;
			return (addrinfo);
		}
	}

	/*
	 * No forwarders.  Move to the next find.
	 */
	find = fctx->find;
	if (find == NULL)
		find = ISC_LIST_HEAD(fctx->finds);
	else {
		find = ISC_LIST_NEXT(find, publink);
		if (find == NULL)
			find = ISC_LIST_HEAD(fctx->finds);
	}

	/*
	 * Find the first unmarked addrinfo.
	 */
	addrinfo = NULL;
	while (find != fctx->find) {
		for (addrinfo = ISC_LIST_HEAD(find->list);
		     addrinfo != NULL;
		     addrinfo = ISC_LIST_NEXT(addrinfo, publink)) {
			if (UNMARKED(addrinfo)) {
				addrinfo->flags |= FCTX_ADDRINFO_MARK;
				break;
			}
		}
		if (addrinfo != NULL)
			break;
		find = ISC_LIST_NEXT(find, publink);
		if (find != fctx->find && find == NULL)
			find = ISC_LIST_HEAD(fctx->finds);
	}

	fctx->find = find;

	return (addrinfo);
}

static void
fctx_try(fetchctx_t *fctx) {
	isc_result_t result;
	dns_adbaddrinfo_t *addrinfo;

	FCTXTRACE("try");

	REQUIRE(!ADDRWAIT(fctx));

	/*
	 * XXXRTH  We don't try to handle forwarding yet.
	 */

	addrinfo = fctx_nextaddress(fctx);
	if (addrinfo == NULL) {
		/*
		 * We have no more addresses.  Start over.
		 */
		fctx_cancelqueries(fctx, ISC_TRUE);
		fctx_cleanupfinds(fctx);
		fctx_cleanupforwaddrs(fctx);
		result = fctx_getaddresses(fctx);
		if (result == DNS_R_WAIT) {
			/*
			 * Sleep waiting for addresses.
			 */
			FCTXTRACE("addrwait");
			fctx->attributes |= FCTX_ATTR_ADDRWAIT; 
			return;
		} else if (result != ISC_R_SUCCESS) {
			/*
			 * Something bad happened.
			 */
			fctx_done(fctx, result);
			return;
		}

		addrinfo = fctx_nextaddress(fctx);
		/*
		 * fctx_getaddresses() returned success, so at least one
		 * of the find lists should be nonempty.
		 */
		INSIST(addrinfo != NULL);
	}

	/*
	 * XXXRTH  This is the place where a try strategy routine would
	 *         be called to send one or more queries.  Instead, we
	 *	   just send a single query.
	 */

	result = fctx_query(fctx, addrinfo, fctx->options);
	if (result != ISC_R_SUCCESS)
		fctx_done(fctx, result);
}

static isc_boolean_t
fctx_destroy(fetchctx_t *fctx) {
	dns_resolver_t *res;
	unsigned int bucketnum;

	/*
	 * Caller must be holding the bucket lock.
	 */

	REQUIRE(VALID_FCTX(fctx));
	REQUIRE(fctx->state == fetchstate_done ||
		fctx->state == fetchstate_init);
	REQUIRE(ISC_LIST_EMPTY(fctx->events));
	REQUIRE(ISC_LIST_EMPTY(fctx->queries));
	REQUIRE(ISC_LIST_EMPTY(fctx->finds));
	REQUIRE(fctx->pending == 0);
	REQUIRE(fctx->validating == 0);
	REQUIRE(fctx->references == 0);

	FCTXTRACE("destroy");

	res = fctx->res;
	bucketnum = fctx->bucketnum;

	ISC_LIST_UNLINK(res->buckets[bucketnum].fctxs, fctx, link);

	isc_timer_detach(&fctx->timer);
	dns_message_destroy(&fctx->rmessage);
	dns_message_destroy(&fctx->qmessage);
	if (dns_name_countlabels(&fctx->domain) > 0)
		dns_name_free(&fctx->domain, res->mctx);
	if (dns_rdataset_isassociated(&fctx->nameservers))
		dns_rdataset_disassociate(&fctx->nameservers);
	dns_name_free(&fctx->name, fctx->res->mctx);
	isc_mem_put(res->mctx, fctx, sizeof *fctx);

	if (res->buckets[bucketnum].exiting &&
	    ISC_LIST_EMPTY(res->buckets[bucketnum].fctxs))
		return (ISC_TRUE);

	return (ISC_FALSE);
}

/*
 * Fetch event handlers.
 */

static void
fctx_timeout(isc_task_t *task, isc_event_t *event) {
	fetchctx_t *fctx = event->arg;

	REQUIRE(VALID_FCTX(fctx));

	(void)task;	/* Keep compiler quiet. */

	FCTXTRACE("timeout");

	if (event->type == ISC_TIMEREVENT_LIFE) {
		fctx_done(fctx, DNS_R_TIMEDOUT);
	} else {
		/*
		 * We could cancel the running queries here, or we could let
		 * them keep going.  Right now we choose the latter...
		 */
		fctx->attributes &= ~FCTX_ATTR_ADDRWAIT;
		fctx_try(fctx);
	}

	isc_event_free(&event);
}

static void
fctx_shutdown(fetchctx_t *fctx) {
	isc_event_t *cevent;

	/*
	 * Start the shutdown process for fctx, if it isn't already underway.
	 */

	FCTXTRACE("shutdown");

	/*
	 * The caller must be holding the appropriate bucket lock.
	 */

	if (fctx->want_shutdown)
		return;
	
	fctx->want_shutdown = ISC_TRUE;

	/*
	 * Unless we're still initializing (in which case the
	 * control event is still outstanding), we need to post
	 * the control event to tell the fetch we want it to
	 * exit.
	 */
	if (fctx->state != fetchstate_init) {
		cevent = &fctx->control_event;
		isc_task_send(fctx->res->buckets[fctx->bucketnum].task,
			      &cevent);
	}
}

static void
fctx_doshutdown(isc_task_t *task, isc_event_t *event) {
	fetchctx_t *fctx = event->arg;
	isc_boolean_t bucket_empty = ISC_FALSE;
	dns_resolver_t *res;
	unsigned int bucketnum;

	REQUIRE(VALID_FCTX(fctx));

	res = fctx->res;
	bucketnum = fctx->bucketnum;
	(void)task;	/* Keep compiler quiet. */
	
	FCTXTRACE("doshutdown");

	fctx->attributes |= FCTX_ATTR_SHUTTINGDOWN;

	LOCK(&res->buckets[bucketnum].lock);
	
	INSIST(fctx->state == fetchstate_active ||
	       fctx->state == fetchstate_done);
	INSIST(fctx->want_shutdown);

	if (fctx->state != fetchstate_done) {
		fctx_stopeverything(fctx);
		fctx->state = fetchstate_done;
		fctx_sendevents(fctx, ISC_R_CANCELED);
	}

	if (fctx->references == 0 && fctx->pending == 0 &&
	    fctx->validating == 0)
		bucket_empty = fctx_destroy(fctx);

	UNLOCK(&res->buckets[bucketnum].lock);

	if (bucket_empty)
		empty_bucket(res);
}

static void
fctx_start(isc_task_t *task, isc_event_t *event) {
	fetchctx_t *fctx = event->arg;
	isc_boolean_t done = ISC_FALSE, bucket_empty = ISC_FALSE;
	dns_resolver_t *res;
	unsigned int bucketnum;

	REQUIRE(VALID_FCTX(fctx));

	res = fctx->res;
	bucketnum = fctx->bucketnum;
	(void)task;	/* Keep compiler quiet. */

	FCTXTRACE("start");

	LOCK(&res->buckets[bucketnum].lock);

	INSIST(fctx->state == fetchstate_init);
	if (fctx->want_shutdown) {
		/*
		 * We haven't started this fctx yet, and we've been requested
		 * to shut it down.
		 *
		 * The events list should be empty, so we INSIST on it.
		 */
		INSIST(ISC_LIST_EMPTY(fctx->events));
		bucket_empty = fctx_destroy(fctx);
		done = ISC_TRUE;
	} else {
		/*
		 * Normal fctx startup.
		 */
		fctx->state = fetchstate_active;
		/*
		 * Reset the control event for later use in shutting down
		 * the fctx.
		 */
		ISC_EVENT_INIT(event, sizeof *event, 0, NULL,
			       DNS_EVENT_FETCHCONTROL, fctx_doshutdown, fctx,
			       (void *)fctx_doshutdown, NULL, NULL);
	}

	UNLOCK(&res->buckets[bucketnum].lock);

	if (!done) {
		/*
		 * All is well.  Start working on the fetch.
		 */
		fctx_try(fctx);
	} else if (bucket_empty)
		empty_bucket(res);
}

/*
 * Fetch Creation, Joining, and Cancelation.
 */

static inline isc_result_t
fctx_join(fetchctx_t *fctx, isc_task_t *task, isc_taskaction_t action,
	  void *arg, dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset,
	  dns_fetch_t *fetch)
{
	isc_task_t *clone;
	dns_fetchevent_t *event;

	FCTXTRACE("join");

	/*
	 * We store the task we're going to send this event to in the
	 * sender field.  We'll make the fetch the sender when we actually
	 * send the event.
	 */
	clone = NULL;
	isc_task_attach(task, &clone);
	event = (dns_fetchevent_t *)
		isc_event_allocate(fctx->res->mctx, clone,
				   DNS_EVENT_FETCHDONE,
				   action, arg, sizeof *event);
	if (event == NULL) {
		isc_task_detach(&clone);
		return (ISC_R_NOMEMORY);
	}
	event->result = DNS_R_SERVFAIL;
	event->qtype = fctx->type;
	event->db = NULL;
	event->node = NULL;
	event->rdataset = rdataset;
	event->sigrdataset = sigrdataset;
	event->fetch = fetch;
	dns_fixedname_init(&event->foundname);
	ISC_LIST_APPEND(fctx->events, event, link);

	fctx->references++;

	fetch->magic = DNS_FETCH_MAGIC;
	fetch->private = fctx;
	
	return (ISC_R_SUCCESS);
}

static isc_result_t
fctx_create(dns_resolver_t *res, dns_name_t *name, dns_rdatatype_t type,
	    dns_name_t *domain, dns_rdataset_t *nameservers,
	    unsigned int options, unsigned int bucketnum, fetchctx_t **fctxp)
{
	fetchctx_t *fctx;
	isc_result_t result = ISC_R_SUCCESS;
	isc_result_t iresult;
	isc_interval_t interval;
	dns_fixedname_t qdomain;

	/*
	 * Caller must be holding the lock for bucket number 'bucketnum'.
	 */
	REQUIRE(fctxp != NULL && *fctxp == NULL);

	fctx = isc_mem_get(res->mctx, sizeof *fctx);
	if (fctx == NULL)
		return (ISC_R_NOMEMORY);
	FCTXTRACE("create");
	dns_name_init(&fctx->name, NULL);
	result = dns_name_dup(name, res->mctx, &fctx->name);
	if (result != ISC_R_SUCCESS)
		goto cleanup_fetch;
	dns_name_init(&fctx->domain, NULL);
	dns_rdataset_init(&fctx->nameservers);
	if (domain == NULL) {
		if (res->fwdpolicy != dns_fwdpolicy_only) {
			/*
			 * The caller didn't supply a query domain and
			 * nameservers, and we're not in forward-only mode,
			 * so find the best nameservers to use.
			 */
			dns_fixedname_init(&qdomain);
			result = dns_view_findzonecut(res->view, name,
					      dns_fixedname_name(&qdomain), 0,
						      0, ISC_TRUE,
						      &fctx->nameservers,
						      NULL);
			if (result != ISC_R_SUCCESS)
				goto cleanup_name;
			result = dns_name_dup(dns_fixedname_name(&qdomain),
					      res->mctx, &fctx->domain);
			if (result != ISC_R_SUCCESS) {
				dns_rdataset_disassociate(&fctx->nameservers);
				goto cleanup_name;
			}
		} else {
			/*
			 * We're in forward-only mode.  Set the query domain
			 * to ".".
			 */
			result = dns_name_dup(dns_rootname, res->mctx,
					      &fctx->domain);
			if (result != ISC_R_SUCCESS)
				goto cleanup_name;
		}
	} else {
		result = dns_name_dup(domain, res->mctx, &fctx->domain);
		if (result != ISC_R_SUCCESS)
			goto cleanup_name;
		dns_rdataset_clone(nameservers, &fctx->nameservers);
	}
	fctx->type = type;
	fctx->options = options;
	/*
	 * Note!  We do not attach to the task.  We are relying on the
	 * resolver to ensure that this task doesn't go away while we are
	 * using it.
	 */
	fctx->res = res;
	fctx->references = 0;
	fctx->bucketnum = bucketnum;
	fctx->state = fetchstate_init;
	fctx->want_shutdown = ISC_FALSE;
	ISC_LIST_INIT(fctx->queries);
	ISC_LIST_INIT(fctx->finds);
	ISC_LIST_INIT(fctx->forwaddrs);
	ISC_LIST_INIT(fctx->forwarders);
	fctx->find = NULL;
	fctx->pending = 0;
	fctx->validating = 0;
	fctx->restarts = 0;
	fctx->attributes = 0;

	fctx->qmessage = NULL;
	result = dns_message_create(res->mctx, DNS_MESSAGE_INTENTRENDER,
				    &fctx->qmessage);
				    
	if (result != ISC_R_SUCCESS)
		goto cleanup_domain;

	fctx->rmessage = NULL;
	result = dns_message_create(res->mctx, DNS_MESSAGE_INTENTPARSE,
				    &fctx->rmessage);
				    
	if (result != ISC_R_SUCCESS)
		goto cleanup_qmessage;

	/*
	 * Compute an expiration time for the entire fetch.
	 */
	isc_interval_set(&interval, 90, 0);		/* XXXRTH constant */
	iresult = isc_time_nowplusinterval(&fctx->expires, &interval);
	if (iresult != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_time_nowplusinterval: %s",
				 isc_result_totext(iresult));
		result = DNS_R_UNEXPECTED;
		goto cleanup_rmessage;
	}

	/*
	 * Default retry interval initialization.  We set the interval now
	 * mostly so it won't be uninitialized.  It will be set to the
	 * correct value before a query is issued.
	 */
	isc_interval_set(&fctx->interval, 2, 0);

	/*
	 * Create an inactive timer.  It will be made active when the fetch
	 * is actually started.
	 */
	fctx->timer = NULL;
	iresult = isc_timer_create(res->timermgr, isc_timertype_inactive,
				   NULL, NULL,
				   res->buckets[bucketnum].task, fctx_timeout,
				   fctx, &fctx->timer);
	if (iresult != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_timer_create: %s",
				 isc_result_totext(iresult));
		result = DNS_R_UNEXPECTED;
		goto cleanup_rmessage;
	}

	ISC_LIST_INIT(fctx->events);
	ISC_LINK_INIT(fctx, link);
	fctx->magic = FCTX_MAGIC;

	ISC_LIST_APPEND(res->buckets[bucketnum].fctxs, fctx, link);

	*fctxp = fctx;

	return (ISC_R_SUCCESS);

 cleanup_rmessage:
	dns_message_destroy(&fctx->rmessage);

 cleanup_qmessage:
	dns_message_destroy(&fctx->qmessage);

 cleanup_domain:
	if (dns_name_countlabels(&fctx->domain) > 0)
		dns_name_free(&fctx->domain, res->mctx);
	if (dns_rdataset_isassociated(&fctx->nameservers))
		dns_rdataset_disassociate(&fctx->nameservers);

 cleanup_name:
	dns_name_free(&fctx->name, res->mctx);

 cleanup_fetch:
	isc_mem_put(res->mctx, fctx, sizeof *fctx);

	return (result);
}

/*
 * Handle Responses
 */

static inline isc_result_t
same_question(fetchctx_t *fctx) {
	isc_result_t result;
	dns_message_t *message = fctx->rmessage;
	dns_name_t *name;
	dns_rdataset_t *rdataset;

	/*
	 * Caller must be holding the fctx lock.
	 */

	/*
	 * XXXRTH  Currently we support only one question.
	 */
	if (message->counts[DNS_SECTION_QUESTION] != 1)
		return (DNS_R_FORMERR);

	result = dns_message_firstname(message, DNS_SECTION_QUESTION);
	if (result != ISC_R_SUCCESS)
		return (result);
	name = NULL;
	dns_message_currentname(message, DNS_SECTION_QUESTION, &name);
	rdataset = ISC_LIST_HEAD(name->list);
	INSIST(rdataset != NULL);
	INSIST(ISC_LIST_NEXT(rdataset, link) == NULL);
	if (fctx->type != rdataset->type ||
	    fctx->res->rdclass != rdataset->rdclass ||
	    !dns_name_equal(&fctx->name, name))
		return (DNS_R_FORMERR);
	
	return (ISC_R_SUCCESS);
}

static void
clone_results(fetchctx_t *fctx) {
	dns_fetchevent_t *event, *hevent;
	isc_result_t result;
	dns_name_t *name, *hname;

	/*
	 * Set up any other events to have the same data as the first
	 * event.
	 *
	 * Caller must be holding the appropriate lock.
	 */

	hevent = ISC_LIST_HEAD(fctx->events);
	if (hevent == NULL)
		return;
	hname = dns_fixedname_name(&hevent->foundname);
	for (event = ISC_LIST_NEXT(hevent, link);
	     event != NULL;
	     event = ISC_LIST_NEXT(event, link)) {
		name = dns_fixedname_name(&event->foundname);
		result = dns_name_concatenate(hname, NULL, name, NULL);
		if (result != ISC_R_SUCCESS)
			event->result = result;
		else
			event->result = hevent->result;
		dns_db_attach(hevent->db, &event->db);
		dns_db_attachnode(hevent->db, hevent->node, &event->node);
		if (hevent->rdataset != NULL &&
		    dns_rdataset_isassociated(hevent->rdataset))
			dns_rdataset_clone(hevent->rdataset, event->rdataset);
		if (hevent->sigrdataset != NULL &&
		    dns_rdataset_isassociated(hevent->sigrdataset))
			dns_rdataset_clone(hevent->sigrdataset,
					   event->sigrdataset);
	}
}

#define CACHE(r)	(((r)->attributes & DNS_RDATASETATTR_CACHE) != 0)
#define ANSWER(r)	(((r)->attributes & DNS_RDATASETATTR_ANSWER) != 0)
#define ANSWERSIG(r)	(((r)->attributes & DNS_RDATASETATTR_ANSWERSIG) != 0)
#define EXTERNAL(r)	(((r)->attributes & DNS_RDATASETATTR_EXTERNAL) != 0)
#define CHAINING(r)	(((r)->attributes & DNS_RDATASETATTR_CHAINING) != 0)

#ifdef notyet
static void
validation_done(isc_task_t *task, isc_event_t *event) {
	fetchctx_t *fctx;

	REQUIRE(event->type == XXX);
	fctx = event->arg;
	REQUIRE(VALID_FCTX(fctx));
	REQUIRE(fctx->validating > 0);

	fctx->validating--;

	/*
	 * If shutting down, ignore the results.  Check to see if we're
	 * done waiting for validator completions and ADB pending events; if
	 * so, destroy the fctx.
	 *
	 * Else, we're not shutting down.  If this is "the answer"
	 * call fctx_done().
	 */

	isc_event_free(&event);
}
#endif

static inline isc_result_t
cache_name(fetchctx_t *fctx, dns_name_t *name, isc_stdtime_t now) {
	dns_rdataset_t *rdataset, *sigrdataset;
	dns_rdataset_t *addedrdataset, *ardataset, *asigrdataset;
	dns_dbnode_t *node, **anodep;
	dns_db_t **adbp;
	dns_fixedname_t foundname;
	dns_name_t *fname, *aname;
	dns_resolver_t *res;
	void *data;
	isc_boolean_t need_validation, have_answer, is_answer;
	isc_result_t result, eresult;
	dns_fetchevent_t *event;

	/*
	 * The appropriate bucket lock must be held.
	 */

	res = fctx->res;
	need_validation = ISC_FALSE;
	have_answer = ISC_FALSE;
	is_answer = ISC_FALSE;
	eresult = ISC_R_SUCCESS;

	/*
	 * Is DNSSEC validation required for this name?
	 */
	dns_fixedname_init(&foundname);
	fname = dns_fixedname_name(&foundname);
	data = NULL;
	result = dns_rbt_findname(res->view->secroots, name, fname, &data);
	if (result == ISC_R_SUCCESS || result == DNS_R_PARTIALMATCH) {
		/*
		 * This name is at or below one of the view's security roots,
		 * so DNSSEC validation is required.
		 */
		need_validation = ISC_TRUE;
	} else if (result != ISC_R_NOTFOUND) {
		/*
		 * Something bad happened.
		 */
		return (result);
	}

	adbp = NULL;
	aname = NULL;
	anodep = NULL;
	ardataset = NULL;
	asigrdataset = NULL;
	event = NULL;
	if ((name->attributes & DNS_NAMEATTR_ANSWER) != 0) {
		have_answer = ISC_TRUE;
		event = ISC_LIST_HEAD(fctx->events);
		if (event != NULL) {
			adbp = &event->db;
			aname = dns_fixedname_name(&event->foundname);
			result = dns_name_concatenate(name, NULL, aname, NULL);
			if (result != ISC_R_SUCCESS)
				return (result);
			anodep = &event->node;
			if (fctx->type != dns_rdatatype_any &&
			    fctx->type != dns_rdatatype_sig) {
				ardataset = event->rdataset;
				asigrdataset = event->sigrdataset;
			}
		}
	}

	/*
	 * Find or create the cache node.
	 */
	node = NULL;
	result = dns_db_findnode(res->view->cachedb,
				 name, ISC_TRUE,
				 &node);
	if (result != ISC_R_SUCCESS)
		return (result);

	/*
	 * Cache or validate each cacheable rdataset.
	 */
	for (rdataset = ISC_LIST_HEAD(name->list);
	     rdataset != NULL;
	     rdataset = ISC_LIST_NEXT(rdataset, link)) {
		if (!CACHE(rdataset))
			continue;
		if ((rdataset->attributes & DNS_RDATASETATTR_ANSWER) != 0)
			is_answer = ISC_TRUE;
		else
			is_answer = ISC_FALSE;
		/*
		 * If this rrset is in a secure domain, do DNSSEC validation
		 * for it, unless it is glue.
		 */
		if (need_validation && rdataset->trust != dns_trust_glue) {
			/*
			 * SIGs are validated as part of validating the
			 * type they cover.
			 */
			if (rdataset->type == dns_rdatatype_sig)
				continue;
			/*
			 * Find the SIG for this rdataset, if we have it.
			 */
			for (sigrdataset = ISC_LIST_HEAD(name->list);
			     sigrdataset != NULL;
			     sigrdataset = ISC_LIST_NEXT(sigrdataset, link)) {
				if (sigrdataset->type == dns_rdatatype_sig &&
				    sigrdataset->covers == rdataset->type)
					break;
			}
#ifdef notyet
			validation = NULL;
			result = dns_validation_create(rdataset, sigrdataset,
						      is_answer, fctx->task,
						      validation_done, fctx,
						      &validation);
			if (result == ISC_R_SUCCESS) {
				ISC_LIST_APPEND(validation);
				fctx->validating++;
			}
#else
			result = DNS_R_NOTIMPLEMENTED;
#endif
		} else if (!EXTERNAL(rdataset)) {
			/*
			 * It's OK to cache this rdataset now.
			 */
			if (ANSWER(rdataset))
				addedrdataset = ardataset;
			else
				addedrdataset = NULL;
			if (CHAINING(rdataset)) {
				if (rdataset->type == dns_rdatatype_cname)
					eresult = DNS_R_CNAME;
				else {
					INSIST(rdataset->type ==
					       dns_rdatatype_dname);
					eresult = DNS_R_DNAME;
				}
			}
			result = dns_db_addrdataset(res->view->cachedb,
						    node, NULL, now,
						    rdataset,
						    ISC_FALSE,
						    addedrdataset);
			if (result == DNS_R_UNCHANGED) {
				if (ANSWER(rdataset) &&
				    ardataset != NULL &&
				    ardataset->type == 0) {
					/*
					 * The answer in the cache is better
					 * than the answer we found, and is
					 * a negative cache entry, so we
					 * must set eresult appropriately.
					 */
					 if (ardataset->covers ==
					     dns_rdatatype_any)
						 eresult =
							 DNS_R_NCACHENXDOMAIN;
					 else
						 eresult =
							 DNS_R_NCACHENXRRSET;
				}
				result = ISC_R_SUCCESS;
			} else if (result != ISC_R_SUCCESS)
				break;
		}
	}

	if (result == ISC_R_SUCCESS && have_answer) {
		fctx->attributes |= FCTX_ATTR_HAVEANSWER;
		if (event != NULL) {
			event->result = eresult;
			dns_db_attach(res->view->cachedb, adbp);
			*anodep = node;
			clone_results(fctx);
		}
	} else
		dns_db_detachnode(res->view->cachedb, &node);

	return (result);
}

static inline isc_result_t
cache_message(fetchctx_t *fctx, isc_stdtime_t now) {
	isc_result_t result;
	dns_section_t section;
	dns_name_t *name;

	FCTXTRACE("cache_message");

	fctx->attributes &= ~FCTX_ATTR_WANTCACHE;

	LOCK(&fctx->res->buckets[fctx->bucketnum].lock);

	for (section = DNS_SECTION_ANSWER;
	     section <= DNS_SECTION_ADDITIONAL;
	     section++) {
		result = dns_message_firstname(fctx->rmessage, section);
		while (result == ISC_R_SUCCESS) {
			name = NULL;
			dns_message_currentname(fctx->rmessage, section,
						&name);
			if ((name->attributes & DNS_NAMEATTR_CACHE) != 0) {
				result = cache_name(fctx, name, now);
				if (result != ISC_R_SUCCESS)
					break;
			}
			result = dns_message_nextname(fctx->rmessage, section);
		}
		if (result != ISC_R_NOMORE)
			break;
	}
	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;

	UNLOCK(&fctx->res->buckets[fctx->bucketnum].lock);

	return (result);
}

static inline isc_result_t
ncache_message(fetchctx_t *fctx, dns_rdatatype_t covers, isc_stdtime_t now) {
	isc_result_t result, eresult;
	dns_name_t *name;
	dns_resolver_t *res;
	dns_db_t **adbp;
	dns_dbnode_t *node, **anodep;
	dns_rdataset_t *ardataset;
	isc_boolean_t need_validation;
	dns_fixedname_t foundname;
	dns_name_t *fname, *aname;
	dns_fetchevent_t *event;
	void *data;

	FCTXTRACE("ncache_message");

	fctx->attributes &= ~FCTX_ATTR_WANTNCACHE;

	res = fctx->res;
	need_validation = ISC_FALSE;
	eresult = ISC_R_SUCCESS;
	name = &fctx->name;

	/*
	 * Is DNSSEC validation required for this name?
	 */
	dns_fixedname_init(&foundname);
	fname = dns_fixedname_name(&foundname);
	data = NULL;
	result = dns_rbt_findname(res->view->secroots, name, fname, &data);
	if (result == ISC_R_SUCCESS || result == DNS_R_PARTIALMATCH) {
		/*
		 * This name is at or below one of the view's security roots,
		 * so DNSSEC validation is required.
		 */
		need_validation = ISC_TRUE;
	} else if (result != ISC_R_NOTFOUND) {
		/*
		 * Something bad happened.
		 */
		return (result);
	}

	LOCK(&res->buckets[fctx->bucketnum].lock);

	adbp = NULL;
	aname = NULL;
	anodep = NULL;
	ardataset = NULL;
	if (!HAVE_ANSWER(fctx)) {
		event = ISC_LIST_HEAD(fctx->events);
		if (event != NULL) {
			adbp = &event->db;
			aname = dns_fixedname_name(&event->foundname);
			result = dns_name_concatenate(name, NULL, aname, NULL);
			if (result != ISC_R_SUCCESS)
				goto unlock;
			anodep = &event->node;
			ardataset = event->rdataset;
		}
	} else
		event = NULL;

	node = NULL;
	result = dns_db_findnode(res->view->cachedb, name, ISC_TRUE,
				 &node);
	if (result != ISC_R_SUCCESS)
		goto unlock;
	result = dns_ncache_add(fctx->rmessage, res->view->cachedb, node,
				covers, now, ardataset);
	if (result == DNS_R_UNCHANGED) {
		/*
		 * The data in the cache is better than the negative cache
		 * entry we're trying to add.
		 */
		if (ardataset != NULL && ardataset->type == 0) {
			/*
			 * The cache data is also a negative cache
			 * entry.
			 */
			if (ardataset->covers == dns_rdatatype_any)
				eresult = DNS_R_NCACHENXDOMAIN;
			else
				eresult = DNS_R_NCACHENXRRSET;
			result = ISC_R_SUCCESS;
		} else {
			/*
			 * Either we don't care about the nature of the
			 * cache rdataset (because no fetch is interested
			 * in the outcome), or the cache rdataset is not
			 * a negative cache entry.  Whichever case it is,
			 * we can return success.  In the latter case,
			 * 'eresult' is already set correctly.
			 *
			 * XXXRTH  Is there a CNAME/DNAME problem here?
			 */
			result = ISC_R_SUCCESS;
		}
	} else if (result == ISC_R_SUCCESS) {
		if (covers == dns_rdatatype_any)
			eresult = DNS_R_NCACHENXDOMAIN;
		else
			eresult = DNS_R_NCACHENXRRSET;
	} else
		goto unlock;

	if (!HAVE_ANSWER(fctx)) {
		fctx->attributes |= FCTX_ATTR_HAVEANSWER;
		if (event != NULL) {
			event->result = eresult;
			dns_db_attach(res->view->cachedb, adbp);
			*anodep = node;
			node = NULL;
			clone_results(fctx);
		}
	}

 unlock:
	UNLOCK(&res->buckets[fctx->bucketnum].lock);

	if (node != NULL)
		dns_db_detachnode(res->view->cachedb, &node);

	return (result);
}

static inline void
mark_related(dns_name_t *name, dns_rdataset_t *rdataset,
	     isc_boolean_t external, isc_boolean_t gluing)
{
	name->attributes |= DNS_NAMEATTR_CACHE;
	if (gluing)
		rdataset->trust = dns_trust_glue;
	else
		rdataset->trust = dns_trust_additional;
	rdataset->attributes |= DNS_RDATASETATTR_CACHE;
	if (external)
		rdataset->attributes |= DNS_RDATASETATTR_EXTERNAL;
#if 0
	/*
	 * XXXRTH  TEMPORARY FOR TESTING!!!
	 */
	rdataset->ttl = 5;
#endif
}

static isc_result_t
check_related(void *arg, dns_name_t *addname, dns_rdatatype_t type) {
	fetchctx_t *fctx = arg;
	isc_result_t result;
	dns_name_t *name;
	dns_rdataset_t *rdataset;
	isc_boolean_t external;
	dns_rdatatype_t rtype;
	isc_boolean_t gluing;

	REQUIRE(VALID_FCTX(fctx));

	if (GLUING(fctx))
		gluing = ISC_TRUE;
	else
		gluing = ISC_FALSE;
	name = NULL;
	rdataset = NULL;
	result = dns_message_findname(fctx->rmessage, DNS_SECTION_ADDITIONAL,
				      addname, dns_rdatatype_any, 0, &name,
				      NULL);
	if (result == ISC_R_SUCCESS) {
		external = ISC_TF(!dns_name_issubdomain(name, &fctx->domain));
		if (type == dns_rdatatype_a) {
			for (rdataset = ISC_LIST_HEAD(name->list);
			     rdataset != NULL;
			     rdataset = ISC_LIST_NEXT(rdataset, link)) {
				if (rdataset->type == dns_rdatatype_sig)
					rtype = rdataset->covers;
				else
					rtype = rdataset->type;
				if (rtype == dns_rdatatype_a ||
				    rtype == dns_rdatatype_aaaa ||
				    rtype == dns_rdatatype_a6)
					mark_related(name, rdataset, external,
						     gluing);
				/*
				 * XXXRTH  Need to do a controlled recursion
				 *	   on the A6 prefix names to mark
				 *	   any additional data related to them.
				 *
				 *	   Ick.
				 */
			}
		} else {
			result = dns_message_findtype(name, type, 0,
						      &rdataset);
			if (result == ISC_R_SUCCESS) {
				mark_related(name, rdataset, external, gluing);
				/*
				 * Do we have its SIG too?
				 */
				result = dns_message_findtype(name,
						      dns_rdatatype_sig,
						      type, &rdataset);
				if (result == ISC_R_SUCCESS)
					mark_related(name, rdataset, external,
						     gluing);
			}
		}
		/*
		 * XXXRTH  Some other stuff still needs to be marked.
		 *         See query.c.
		 */
	}

	return (ISC_R_SUCCESS);
}

static inline isc_result_t
cname_target(dns_rdataset_t *rdataset, dns_name_t *tname) {
	isc_result_t result;
	dns_rdata_t rdata;
	isc_region_t r;

	result = dns_rdataset_first(rdataset);
	if (result != ISC_R_SUCCESS)
		return (result);
	dns_rdataset_current(rdataset, &rdata);
	dns_rdata_toregion(&rdata, &r);
	dns_name_init(tname, NULL);
	dns_name_fromregion(tname, &r);

	return (ISC_R_SUCCESS);
}

static inline isc_result_t
dname_target(dns_rdataset_t *rdataset, dns_name_t *qname, dns_name_t *oname,
	     dns_fixedname_t *fixeddname)
{
	isc_result_t result;
	dns_rdata_t rdata;
	isc_region_t r;
	dns_name_t *dname, tname;
	unsigned int nlabels, nbits;
	int order;
	dns_namereln_t namereln;

	/*
	 * Get the target name of the DNAME.
	 */
	dns_fixedname_init(fixeddname);
	dname = dns_fixedname_name(fixeddname);

	result = dns_rdataset_first(rdataset);
	if (result != ISC_R_SUCCESS)
		return (result);
	dns_rdataset_current(rdataset, &rdata);
	dns_rdata_toregion(&rdata, &r);
	dns_name_init(&tname, NULL);
	dns_name_fromregion(&tname, &r);

	/*
	 * Get the prefix of qname.
	 */
	namereln = dns_name_fullcompare(qname, oname, &order, &nlabels,
					&nbits);
	if (namereln != dns_namereln_subdomain)
		return (DNS_R_FORMERR);
	result = dns_name_split(qname, nlabels, nbits, dname, NULL);
	if (result != ISC_R_SUCCESS)
		return (result);

	return (dns_name_concatenate(dname, &tname, dname, NULL));
}

static isc_result_t
noanswer_response(fetchctx_t *fctx, dns_name_t *oqname) {
	isc_result_t result;
	dns_message_t *message;
	dns_name_t *name, *qname, *ns_name, *soa_name;
	dns_rdataset_t *rdataset, *ns_rdataset;
	isc_boolean_t done, aa, negative_response;
	dns_rdatatype_t type;

	FCTXTRACE("noanswer_response");

	message = fctx->rmessage;

	/*
	 * Setup qname.
	 */
	if (oqname == NULL) {
		/*
		 * We have a normal, non-chained negative response or
		 * referral.
		 */
		if ((message->flags & DNS_MESSAGEFLAG_AA) != 0)
			aa = ISC_TRUE;
		else
			aa = ISC_FALSE;
		qname = &fctx->name;
	} else {
		/*
		 * We're being invoked by answer_response() after it has
		 * followed a CNAME/DNAME chain.
		 */
		qname = oqname;
		aa = ISC_FALSE;
		/*
		 * If the current qname is not a subdomain of the query
		 * domain, there's no point in looking at the authority
		 * section without doing DNSSEC validation.
		 *
		 * Until we do that validation, we'll just return success
		 * in this case.
		 */
		if (!dns_name_issubdomain(qname, &fctx->domain))
			return (ISC_R_SUCCESS);
	}
	
	/*
	 * We have to figure out if this is a negative response, or a
	 * referral.
	 */

	/*
	 * Sometimes we can tell if its a negative response by looking at
	 * the message header.
	 */
	negative_response = ISC_FALSE;
	if (message->rcode == dns_rcode_nxdomain ||
	    (message->counts[DNS_SECTION_ANSWER] == 0 &&
	     message->counts[DNS_SECTION_AUTHORITY] == 0))
		negative_response = ISC_TRUE;

	/*
	 * Process the authority section.
	 */
	done = ISC_FALSE;
	ns_name = NULL;
	ns_rdataset = NULL;
	soa_name = NULL;
	result = dns_message_firstname(message, DNS_SECTION_AUTHORITY);
	while (!done && result == ISC_R_SUCCESS) {
		name = NULL;
		dns_message_currentname(message, DNS_SECTION_AUTHORITY, &name);
		if (dns_name_issubdomain(name, &fctx->domain)) {
			for (rdataset = ISC_LIST_HEAD(name->list);
			     rdataset != NULL;
			     rdataset = ISC_LIST_NEXT(rdataset, link)) {
				type = rdataset->type;
				if (type == dns_rdatatype_sig)
					type = rdataset->covers;
				if (rdataset->type == dns_rdatatype_ns) {
					/*
					 * NS or SIG NS.
					 *
					 * Only one set of NS RRs is allowed.
					 */
					if (ns_name != NULL && name != ns_name)
						return (DNS_R_FORMERR);
					ns_name = name;
					name->attributes |=
						DNS_NAMEATTR_CACHE;
					rdataset->attributes |=
						DNS_RDATASETATTR_CACHE;
					rdataset->trust = dns_trust_glue;
					ns_rdataset = rdataset;
				} else if (rdataset->type ==
					   dns_rdatatype_soa ||
					   rdataset->type ==
					   dns_rdatatype_nxt) {
					/*
					 * SOA, SIG SOA, NXT, or SIG NXT.
					 *
					 * Only one SOA is allowed.
					 */
					if (soa_name != NULL &&
					    name != soa_name)
						return (DNS_R_FORMERR);
					soa_name = name;
					negative_response = ISC_TRUE;
					name->attributes |=
						DNS_NAMEATTR_NCACHE;
					rdataset->attributes |=
						DNS_RDATASETATTR_NCACHE;
					if (aa)
						rdataset->trust =
						    dns_trust_authauthority;
					else
						rdataset->trust =
							dns_trust_additional;
					/*
					 * No additional data needs to be
					 * marked.
					 */
				}
			}
		}
		result = dns_message_nextname(message, DNS_SECTION_AUTHORITY);
		if (result != ISC_R_NOMORE)
			return (result);
	}

	/*
	 * Did we find anything?
	 */
	if (!negative_response && ns_name == NULL) {
		/*
		 * Nope.
		 */
		if (oqname != NULL) {
			/*
			 * We've already got a partial CNAME/DNAME chain,
			 * and haven't found else anything useful here, but
			 * no error has occurred since we have an answer.
			 */
			return (ISC_R_SUCCESS);
		} else {
			/*
			 * The responder is insane.
			 */
			return (DNS_R_FORMERR);
		}
	}

	/*
	 * If we found both NS and SOA, they should be the same name.
	 */
	if (ns_name != NULL && soa_name != NULL && ns_name != soa_name)
		return (DNS_R_FORMERR);

	/*
	 * Do we have a referral?  (We only want to follow a referral if
	 * we're not following a chain.)
	 */
	if (!negative_response && ns_name != NULL && oqname == NULL) {
		/*
		 * Mark any additional data related to this rdataset.
		 * It's important that we do this before we change the
		 * query domain.
		 */
		INSIST(ns_rdataset != NULL);
		fctx->attributes |= FCTX_ATTR_GLUING;
		(void)dns_rdataset_additionaldata(ns_rdataset, check_related,
						  fctx);
		fctx->attributes &= ~FCTX_ATTR_GLUING;
		/*
		 * Set the current query domain to the referral name.
		 *
		 * XXXRTH  We should check if we're in forward-only mode, and
		 *         if so we should bail out.
		 */
		INSIST(dns_name_countlabels(&fctx->domain) > 0);
		dns_name_free(&fctx->domain, fctx->res->mctx);
		if (dns_rdataset_isassociated(&fctx->nameservers))
			dns_rdataset_disassociate(&fctx->nameservers);
		dns_name_init(&fctx->domain, NULL);
		result = dns_name_dup(ns_name, fctx->res->mctx, &fctx->domain);
		if (result != ISC_R_SUCCESS)
			return (result);
		fctx->attributes |= FCTX_ATTR_WANTCACHE;
		return (DNS_R_DELEGATION);
	}

	/*
	 * Since we're not doing a referral, we don't want to cache any
	 * NS RRs we may have found.
	 */
	if (ns_name != NULL)
		ns_name->attributes &= ~DNS_NAMEATTR_CACHE;

	if (negative_response)
		fctx->attributes |= FCTX_ATTR_WANTNCACHE;
		
	return (ISC_R_SUCCESS);
}

static isc_result_t
answer_response(fetchctx_t *fctx) {
	isc_result_t result;
	dns_message_t *message;
	dns_name_t *name, *qname, tname;
	dns_rdataset_t *rdataset;
	isc_boolean_t done, external, chaining, aa, found, want_chaining;
	isc_boolean_t have_sig, have_answer;
	unsigned int aflag;
	dns_rdatatype_t type;
	dns_fixedname_t dname;

	FCTXTRACE("answer_response");

	message = fctx->rmessage;

	/*
	 * Examine the answer section, marking those rdatasets which are
	 * part of the answer and should be cached.
	 */

	done = ISC_FALSE;
	chaining = ISC_FALSE;
	have_answer = ISC_FALSE;
	have_sig = ISC_FALSE;
	want_chaining = ISC_FALSE;
	if ((message->flags & DNS_MESSAGEFLAG_AA) != 0)
		aa = ISC_TRUE;
	else
		aa = ISC_FALSE;
	qname = &fctx->name;
	type = fctx->type;
	result = dns_message_firstname(message, DNS_SECTION_ANSWER);
	while (!done && result == ISC_R_SUCCESS) {
		name = NULL;
		dns_message_currentname(message, DNS_SECTION_ANSWER, &name);
		external = ISC_TF(!dns_name_issubdomain(name, &fctx->domain));
		if (dns_name_equal(name, qname)) {
			for (rdataset = ISC_LIST_HEAD(name->list);
			     rdataset != NULL;
			     rdataset = ISC_LIST_NEXT(rdataset, link)) {
				found = ISC_FALSE;
				want_chaining = ISC_FALSE;
				aflag = 0;
				if (rdataset->type == type ||
				    type == dns_rdatatype_any) {
					/*
					 * We've found an ordinary answer.
					 */
					found = ISC_TRUE;
					done = ISC_TRUE;
					aflag = DNS_RDATASETATTR_ANSWER;
				} else if (rdataset->type == dns_rdatatype_sig
					   && rdataset->covers == type) {
					/*
					 * We've found a signature that
					 * covers the type we're looking for.
					 */
					found = ISC_TRUE;
					aflag = DNS_RDATASETATTR_ANSWERSIG;
				} else if (rdataset->type ==
					   dns_rdatatype_cname) {
					/*
					 * We're looking for something else,
					 * but we found a CNAME.
					 *
					 * Getting a CNAME response for some
					 * query types is an error.
					 */
					if (type == dns_rdatatype_sig ||
					    type == dns_rdatatype_key ||
					    type == dns_rdatatype_nxt)
						return (DNS_R_FORMERR);
					found = ISC_TRUE;
					want_chaining = ISC_TRUE;
					aflag = DNS_RDATASETATTR_ANSWER;
					result = cname_target(rdataset,
							      &tname);
					if (result != ISC_R_SUCCESS)
						return (result);
				} else if (rdataset->type == dns_rdatatype_sig
					   && rdataset->covers ==
					   dns_rdatatype_cname) {
					/*
					 * We're looking for something else,
					 * but we found a SIG CNAME.
					 */
					found = ISC_TRUE;
					aflag = DNS_RDATASETATTR_ANSWERSIG;
				}

				if (found) {
					/*
					 * We've found an answer to our
					 * question.
					 */
					name->attributes |=
						DNS_NAMEATTR_CACHE;
					rdataset->attributes |=
						DNS_RDATASETATTR_CACHE;
					rdataset->trust = dns_trust_answer;
					if (!chaining) {
						/*
						 * This data is "the" answer
						 * to our question only if
						 * we're not chaining (i.e.
						 * if we haven't followed
						 * a CNAME or DNAME).
						 */
						INSIST(!external);
						if (aflag ==
						    DNS_RDATASETATTR_ANSWER)
							have_answer = ISC_TRUE;
						else
							have_sig = ISC_TRUE;
						name->attributes |=
							DNS_NAMEATTR_ANSWER;
						rdataset->attributes |= aflag;
						if (aa)
							rdataset->trust =
							  dns_trust_authanswer;
					} else if (external) {
						/*
						 * This data is outside of
						 * our query domain, and
						 * may only be cached if it
						 * comes from a secure zone
						 * and validates.
						 */
						rdataset->attributes |=
						    DNS_RDATASETATTR_EXTERNAL;
					}

					/*
					 * Mark any additional data related
					 * to this rdataset.
					 */
					(void)dns_rdataset_additionaldata(
							rdataset,
							check_related,
							fctx);

					/*
					 * CNAME chaining.
					 */
					if (want_chaining) {
						chaining = ISC_TRUE;
						rdataset->attributes |=
						    DNS_RDATASETATTR_CHAINING;
						qname = &tname;
					}
				}
				/*
				 * We could add an "else" clause here and
				 * log that we're ignoring this rdataset.
				 */
			}
		} else {
			/*
			 * Look for a DNAME (or its SIG).  Anything else is
			 * ignored.
			 */
			for (rdataset = ISC_LIST_HEAD(name->list);
			     rdataset != NULL;
			     rdataset = ISC_LIST_NEXT(rdataset, link)) {
				found = ISC_FALSE;
				want_chaining = ISC_FALSE;
				aflag = 0;
				if (rdataset->type == dns_rdatatype_dname) {
					/*
					 * We're looking for something else,
					 * but we found a DNAME.
					 *
					 * If we're not chaining, then the
					 * DNAME should not be external.
					 */
					if (!chaining && external)
						return (DNS_R_FORMERR);
					found = ISC_TRUE;
					want_chaining = ISC_TRUE;
					aflag = DNS_RDATASETATTR_ANSWER;
					result = dname_target(rdataset,
							      qname, name,
							      &dname);
					if (result == ISC_R_NOSPACE) {
						/*
						 * We can't construct the
						 * DNAME target.  Do not
						 * try to continue.
						 */
						want_chaining = ISC_FALSE;
					} else if (result != ISC_R_SUCCESS)
						return (result);
				} else if (rdataset->type == dns_rdatatype_sig
					   && rdataset->covers ==
					   dns_rdatatype_dname) {
					/*
					 * We've found a signature that
					 * covers the DNAME.
					 */
					found = ISC_TRUE;
					aflag = DNS_RDATASETATTR_ANSWERSIG;
				}

				if (found) {
					/*
					 * We've found an answer to our
					 * question.
					 */
					name->attributes |=
						DNS_NAMEATTR_CACHE;
					rdataset->attributes |=
						DNS_RDATASETATTR_CACHE;
					rdataset->trust = dns_trust_answer;
					if (!chaining) {
						/*
						 * This data is "the" answer
						 * to our question only if
						 * we're not chaining.
						 */
						INSIST(!external);
						name->attributes |=
							DNS_NAMEATTR_ANSWER;
						rdataset->attributes |= aflag;
						if (aa)
							rdataset->trust =
							  dns_trust_authanswer;
					} else if (external) {
						rdataset->attributes |=
						    DNS_RDATASETATTR_EXTERNAL;
					}

					/*
					 * DNAME chaining.
					 */
					if (want_chaining) {
						chaining = ISC_TRUE;
						rdataset->attributes |=
						    DNS_RDATASETATTR_CHAINING;
						qname = dns_fixedname_name(
								   &dname);
					}
				}
			}
		}
		result = dns_message_nextname(message, DNS_SECTION_ANSWER);
	}
	if (result != ISC_R_NOMORE)
		return (result);

	/*
	 * We should have found an answer.
	 */
	if (!have_answer)
		return (DNS_R_FORMERR);

	/*
	 * This response is now potentially cacheable.
	 */
	fctx->attributes |= FCTX_ATTR_WANTCACHE;

	/*
	 * Did chaining end before we got the final answer?
	 */
	if (want_chaining) {
		/*
		 * Yes.  This may be a negative reply, so hand off
		 * authority section processing to the noanswer code.
		 * If it isn't a noanswer response, no harm will be
		 * done.
		 */
		return (noanswer_response(fctx, qname));
	}

	/*
	 * We didn't end with an incomplete chain, so the rcode should be
	 * "no error".
	 */
	if (message->rcode != dns_rcode_noerror)
		return (DNS_R_FORMERR);

	/*
	 * Examine the authority section (if there is one).
	 *
	 * We expect there to be only one owner name for all the rdatasets
	 * in this section, and we expect that it is not external.
	 */
	done = ISC_FALSE;
	result = dns_message_firstname(message, DNS_SECTION_AUTHORITY);
	while (!done && result == ISC_R_SUCCESS) {
		name = NULL;
		dns_message_currentname(message, DNS_SECTION_AUTHORITY, &name);
		external = ISC_TF(!dns_name_issubdomain(name, &fctx->domain));
		if (!external) {
			/*
			 * We expect to find NS or SIG NS rdatasets, and
			 * nothing else.
			 */
			for (rdataset = ISC_LIST_HEAD(name->list);
			     rdataset != NULL;
			     rdataset = ISC_LIST_NEXT(rdataset, link)) {
				if (rdataset->type == dns_rdatatype_ns ||
				    (rdataset->type == dns_rdatatype_sig &&
				     rdataset->covers == dns_rdatatype_ns)) {
					name->attributes |=
						DNS_NAMEATTR_CACHE;
					rdataset->attributes |=
						DNS_RDATASETATTR_CACHE;
					if (aa && !chaining)
						rdataset->trust =
						    dns_trust_authauthority;
					else
						rdataset->trust =
						    dns_trust_additional;

					/*
					 * Mark any additional data related
					 * to this rdataset.
					 */
					(void)dns_rdataset_additionaldata(
							rdataset,
							check_related,
							fctx);
				}
			}
			/*
			 * Since we've found a non-external name in the
			 * authority section, we should stop looking, even
			 * if we didn't find any NS or SIG NS.
			 */
			done = ISC_TRUE;
		}
		result = dns_message_nextname(message, DNS_SECTION_AUTHORITY);
	}
	if (result != ISC_R_NOMORE)
		return (result);

	return (ISC_R_SUCCESS);
}

static void
resquery_response(isc_task_t *task, isc_event_t *event) {
	isc_result_t result;
	resquery_t *query = event->arg;
	dns_dispatchevent_t *devent = (dns_dispatchevent_t *)event;
	isc_boolean_t keep_trying, broken_server, get_nameservers, resend;
	isc_boolean_t truncated;
	dns_message_t *message;
	fetchctx_t *fctx;
	dns_rdatatype_t covers;
	dns_name_t *fname;
	dns_fixedname_t foundname;
	isc_stdtime_t now;
	isc_time_t tnow, *finish;
	dns_adbaddrinfo_t *addrinfo;
	unsigned int options;

	REQUIRE(VALID_QUERY(query));
	fctx = query->fctx;
	options = query->options;
	REQUIRE(VALID_FCTX(fctx));
	REQUIRE(event->type == DNS_EVENT_DISPATCH);

	(void)task;
	QTRACE("response");

	(void)isc_timer_touch(fctx->timer);

	keep_trying = ISC_FALSE;
	broken_server = ISC_FALSE;
	get_nameservers = ISC_FALSE;
	resend = ISC_FALSE;
	truncated = ISC_FALSE;
	covers = 0;
	finish = NULL;

	/*
	 * XXXRTH  We should really get the current time just once.  We
	 *         need a routine to convert from an isc_time_t to an
	 *	   isc_stdtime_t.
	 */
	result = isc_time_now(&tnow);
	if (result != ISC_R_SUCCESS)
		goto done;
	finish = &tnow;
	isc_stdtime_get(&now);

	message = fctx->rmessage;
	message->querytsig = query->tsig;
	message->tsigkey = query->tsigkey;
	result = dns_message_parse(message, &devent->buffer, ISC_FALSE);
	if (result != ISC_R_SUCCESS) {
		switch (result) {
		case DNS_R_UNEXPECTEDEND:
			if (!message->question_ok ||
			    (message->flags & DNS_MESSAGEFLAG_TC) == 0 ||
			    (options & DNS_FETCHOPT_TCP) != 0) {
				/*
				 * Either the message ended prematurely,
				 * and/or wasn't marked as being truncated,
				 * and/or this is a response to a query we
				 * sent over TCP.  In all of these cases,
				 * something is wrong with the remote
				 * server and we don't want to retry using
				 * TCP.
				 */
				if ((query->options & DNS_FETCHOPT_NOEDNS0)
				    == 0) {
					/*
					 * The problem might be that they
					 * don't understand EDNS0.  Turn it
					 * off and try again.
					 */
					options |= DNS_FETCHOPT_NOEDNS0;
					resend = ISC_TRUE;
					/*
					 * Remember that they don't like EDNS0.
					 */
					dns_adb_changeflags(
							fctx->res->view->adb,
							query->addrinfo,
							DNS_FETCHOPT_NOEDNS0,
							DNS_FETCHOPT_NOEDNS0);
				} else {
					broken_server = ISC_TRUE;
					keep_trying = ISC_TRUE;
				}
				goto done;
			}
			/*
			 * We defer retrying via TCP for a bit so we can
			 * check out this message further.
			 */
			truncated = ISC_TRUE;
			break;
		case DNS_R_FORMERR:
			if ((query->options & DNS_FETCHOPT_NOEDNS0) == 0) { 
				/*
				 * The problem might be that they
				 * don't understand EDNS0.  Turn it
				 * off and try again.
				 */
				options |= DNS_FETCHOPT_NOEDNS0;
				resend = ISC_TRUE;
				/*
				 * Remember that they don't like EDNS0.
				 */
				dns_adb_changeflags(fctx->res->view->adb,
						    query->addrinfo,
						    DNS_FETCHOPT_NOEDNS0,
						    DNS_FETCHOPT_NOEDNS0);
			} else {
				broken_server = ISC_TRUE;
				keep_trying = ISC_TRUE;
			}
			goto done;
		case DNS_R_MOREDATA:
			result = DNS_R_NOTIMPLEMENTED;
			goto done;
		default:
			/*
			 * Something bad has happened.
			 */
			goto done;
		}
	}

	/*
	 * The dispatcher should ensure we only get responses with QR set.
	 */
	INSIST((message->flags & DNS_MESSAGEFLAG_QR) != 0);
	/*
	 * INSIST() that the message comes from the place we sent it to,
	 * since the dispatch code should ensure this.
	 *
	 * INSIST() that the message id is correct (this should also be	
	 * ensured by the dispatch code).
	 */


	/*
	 * Deal with truncated responses by retrying using TCP.
	 */
	if ((message->flags & DNS_MESSAGEFLAG_TC) != 0)
		truncated = ISC_TRUE;
		
	if (truncated) {
		if ((options & DNS_FETCHOPT_TCP) != 0) {
			broken_server = ISC_TRUE;
			keep_trying = ISC_TRUE;
		} else {
			options |= DNS_FETCHOPT_TCP;
			resend = ISC_TRUE;
		}
		goto done;
	}

	/*
	 * Is it a query response?
	 */
	if (message->opcode != dns_opcode_query) {
		/* XXXRTH Log */
		broken_server = ISC_TRUE;
		keep_trying = ISC_TRUE;
		goto done;
	}

	/*
	 * Is the remote server broken, or does it dislike us?
	 */
	if (message->rcode != dns_rcode_noerror &&
	    message->rcode != dns_rcode_nxdomain) {
		if ((query->options & DNS_FETCHOPT_NOEDNS0) == 0 &&
		    message->rcode == dns_rcode_formerr) {
			/*
			 * It's very likely they don't like EDNS0.
			 */
			options |= DNS_FETCHOPT_NOEDNS0;
			resend = ISC_TRUE;
			/*
			 * Remember that they don't like EDNS0.
			 */
			dns_adb_changeflags(fctx->res->view->adb,
					    query->addrinfo,
					    DNS_FETCHOPT_NOEDNS0,
					    DNS_FETCHOPT_NOEDNS0);
		} else {
			/*
			 * XXXRTH log.
			 */
			broken_server = ISC_TRUE;
			keep_trying = ISC_TRUE;
			/*
			 * XXXRTH Need to deal with YXDOMAIN code.
			 */
		}
		goto done;
	}

	/*
	 * Is the question the same as the one we asked?
	 */
	result = same_question(fctx);
	if (result != ISC_R_SUCCESS) {
		/* XXXRTH Log */
		if (result == DNS_R_FORMERR)
			keep_trying = ISC_TRUE;
		goto done;
	}

	/*
	 * Did we get any answers?
	 */
	if (message->counts[DNS_SECTION_ANSWER] > 0 &&
	    (message->rcode == dns_rcode_noerror ||
	     message->rcode == dns_rcode_nxdomain)) {
		/*
		 * We've got answers.
		 */
		result = answer_response(fctx);
		if (result != ISC_R_SUCCESS) {
			if (result == DNS_R_FORMERR)
				keep_trying = ISC_TRUE;
			goto done;
		}
	} else if (message->counts[DNS_SECTION_AUTHORITY] > 0 ||
		   message->rcode == dns_rcode_noerror ||
		   message->rcode == dns_rcode_nxdomain) {
		/*
		 * NXDOMAIN, NXRDATASET, or referral.
		 */
		result = noanswer_response(fctx, NULL);
		if (result == DNS_R_DELEGATION) {
			/*
			 * We don't have the answer, but we know a better
			 * place to look.
			 */
			get_nameservers = ISC_TRUE;
			keep_trying = ISC_TRUE;
			result = ISC_R_SUCCESS;
		} else if (result != ISC_R_SUCCESS) {
			/*
			 * Something has gone wrong.
			 */
			if (result == DNS_R_FORMERR)
				keep_trying = ISC_TRUE;
			goto done;
		}
	} else {
		/*
		 * The server is insane.
		 */
		/* XXXRTH Log */
		broken_server = ISC_TRUE;
		keep_trying = ISC_TRUE;
		goto done;
	}

	/*
	 * XXXRTH  Explain this.
	 */
	query->tsig = NULL;

	/*
	 * Cache the cacheable parts of the message.  This may also cause
	 * work to be queued to the DNSSEC validator.
	 */
	if (WANTCACHE(fctx)) {
		result = cache_message(fctx, now);
		if (result != ISC_R_SUCCESS)
			goto done;
	}

	/*
	 * Ncache the negatively cacheable parts of the message.  This may
	 * also cause work to be queued to the DNSSEC validator.
	 */
	if (WANTNCACHE(fctx)) {
		if (message->rcode == dns_rcode_nxdomain)
			covers = dns_rdatatype_any;
		else
			covers = fctx->type;
		/*
		 * Cache any negative cache entries in the message.
		 */
		result = ncache_message(fctx, covers, now);
	}

 done:
	/*
	 * Remember the query's addrinfo, in case we need to mark the
	 * server as broken.
	 */
	addrinfo = query->addrinfo;

	/*
	 * Cancel the query.
	 *
	 * XXXRTH  Don't cancel the query if waiting for validation?
	 */
	fctx_cancelquery(&query, &devent, finish, ISC_FALSE);

	if (keep_trying) {
		if (result == DNS_R_FORMERR)
			broken_server = ISC_TRUE;
		if (broken_server) {
			/*
			 * XXXRTH  Replace "600" with a configurable
			 *	   value.
			 *
			 * Would we want to mark "." or "com." lame, even
			 * if they were???
			 *
			 * Do badness instead?
			 *
			 * Suppress/change if we're forwarding.
			 */
			result = dns_adb_marklame(fctx->res->view->adb,
						  addrinfo,
						  &fctx->domain,
						  now + 600);
			result = ISC_R_SUCCESS;
			if (result != ISC_R_SUCCESS) {
				fctx_done(fctx, result);
				return;
			}
		}

		if (get_nameservers) {
			dns_fixedname_init(&foundname);
			fname = dns_fixedname_name(&foundname);
			if (result != ISC_R_SUCCESS) {
				fctx_done(fctx, DNS_R_SERVFAIL);
				return;
			}
			result = dns_view_findzonecut(fctx->res->view,
						      &fctx->domain,
						      fname,
						      now, 0, ISC_TRUE,
						      &fctx->nameservers,
						      NULL);
			if (result != ISC_R_SUCCESS) {
				FCTXTRACE("couldn't find a zonecut");
				fctx_done(fctx, DNS_R_SERVFAIL);
				return;
			}
			if (!dns_name_issubdomain(fname, &fctx->domain)) {
				/*
				 * The best nameservers are now above our
				 * previous QDOMAIN.
				 *
				 * XXXRTH  What should we do here?
				 */
				FCTXTRACE("nameservers now above QDOMAIN");
				fctx_done(fctx, DNS_R_SERVFAIL);
				return;
			}
			dns_name_free(&fctx->domain, fctx->res->mctx);
			dns_name_init(&fctx->domain, NULL);
			result = dns_name_dup(fname, fctx->res->mctx,
					      &fctx->domain);
			if (result != ISC_R_SUCCESS) {
				fctx_done(fctx, DNS_R_SERVFAIL);
				return;
			}
			fctx_cancelqueries(fctx, ISC_TRUE);
			fctx_cleanupfinds(fctx);
			fctx_cleanupforwaddrs(fctx);
		}					  
		/*
		 * Try again.
		 */
		fctx_try(fctx);
	} else if (resend) {
		/*
		 * Resend (probably with changed options).
		 */
		FCTXTRACE("resend");
		result = fctx_query(fctx, addrinfo, options);
		if (result != ISC_R_SUCCESS)
			fctx_done(fctx, result);
	} else if (result == ISC_R_SUCCESS && !HAVE_ANSWER(fctx)) {
		/*
		 * All has gone well so far, but we are waiting for the
		 * DNSSEC validator to validate the answer.
		 */
		fctx_cancelqueries(fctx, ISC_TRUE);
		result = fctx_stopidletimer(fctx);
		if (result != ISC_R_SUCCESS)
			fctx_done(fctx, result);
	} else {
		/*
		 * We're done.
		 */
		fctx_done(fctx, result);
	}
}


/***
 *** Resolver Methods
 ***/

static void
free_forwarders(dns_resolver_t *res) {
	isc_sockaddr_t *sa, *next_sa;

	for (sa = ISC_LIST_HEAD(res->forwarders);
	     sa != NULL;
	     sa = next_sa) {
		next_sa = ISC_LIST_NEXT(sa, link);
		ISC_LIST_UNLINK(res->forwarders, sa, link);
		isc_mem_put(res->mctx, sa, sizeof *sa);
	}
}

static void
destroy(dns_resolver_t *res) {
	unsigned int i;

	REQUIRE(res->references == 0);

	RTRACE("destroy");

	isc_mutex_destroy(&res->lock);
	for (i = 0; i < res->nbuckets; i++) {
		INSIST(ISC_LIST_EMPTY(res->buckets[i].fctxs));
		isc_task_shutdown(res->buckets[i].task);
		isc_task_detach(&res->buckets[i].task);
		isc_mutex_destroy(&res->buckets[i].lock);
	}
	isc_mem_put(res->mctx, res->buckets,
		    res->nbuckets * sizeof (fctxbucket_t));
	if (res->dispatch4 != NULL)
		dns_dispatch_detach(&res->dispatch4);
	if (res->udpsocket4 != NULL)
		isc_socket_detach(&res->udpsocket4);
	if (res->dispatch6 != NULL)
		dns_dispatch_detach(&res->dispatch6);
	if (res->udpsocket6 != NULL)
		isc_socket_detach(&res->udpsocket6);
	free_forwarders(res);
	res->magic = 0;
	isc_mem_put(res->mctx, res, sizeof *res);
}

static void
send_shutdown_events(dns_resolver_t *res) {
	isc_event_t *event, *next_event;
	isc_task_t *etask;

	/*
	 * Caller must be holding the resolver lock.
	 */

	for (event = ISC_LIST_HEAD(res->whenshutdown);
	     event != NULL;
	     event = next_event) {
		next_event = ISC_LIST_NEXT(event, link);
		ISC_LIST_UNLINK(res->whenshutdown, event, link);
		etask = event->sender;
		event->sender = res;
		isc_task_sendanddetach(&etask, &event);
	}
}

static void
empty_bucket(dns_resolver_t *res) {
	RTRACE("empty_bucket");

	LOCK(&res->lock);

	INSIST(res->activebuckets > 0);
	res->activebuckets--;
	if (res->activebuckets == 0)
		send_shutdown_events(res);

	UNLOCK(&res->lock);
}

isc_result_t
dns_resolver_create(dns_view_t *view,
		    isc_taskmgr_t *taskmgr, unsigned int ntasks,
		    isc_socketmgr_t *socketmgr,
		    isc_timermgr_t *timermgr,
		    dns_dispatch_t *dispatch, dns_resolver_t **resp)
{
	dns_resolver_t *res;
	isc_result_t result = ISC_R_SUCCESS;
	unsigned int i, buckets_created = 0;
	in_port_t port = 5353;

	/*
	 * Create a resolver.
	 */

	REQUIRE(DNS_VIEW_VALID(view));
	REQUIRE(ntasks > 0);
	REQUIRE(resp != NULL && *resp == NULL);

	res = isc_mem_get(view->mctx, sizeof *res);
	if (res == NULL)
		return (ISC_R_NOMEMORY);
	RTRACE("create");
	res->mctx = view->mctx;
	res->rdclass = view->rdclass;
	res->socketmgr = socketmgr;
	res->timermgr = timermgr;
	res->view = view;

	res->nbuckets = ntasks;
	res->activebuckets = ntasks;
	res->buckets = isc_mem_get(view->mctx,
				   ntasks * sizeof (fctxbucket_t));
	if (res->buckets == NULL) {
		result = ISC_R_NOMEMORY;
		goto cleanup_res;
	}
	for (i = 0; i < ntasks; i++) {
		result = isc_mutex_init(&res->buckets[i].lock);
		if (result != ISC_R_SUCCESS)
			goto cleanup_buckets;
		res->buckets[i].task = NULL;
		result = isc_task_create(taskmgr, view->mctx, 0,
					  &res->buckets[i].task);
		if (result != ISC_R_SUCCESS) {
			isc_mutex_destroy(&res->buckets[i].lock);
			goto cleanup_buckets;
		}
		ISC_LIST_INIT(res->buckets[i].fctxs);
		res->buckets[i].exiting = ISC_FALSE;
		buckets_created++;
	}

	/*
	 * IPv4 Dispatcher.
	 */
	res->dispatch4 = NULL;
	res->udpsocket4 = NULL;
	if (dispatch != NULL) {
		dns_dispatch_attach(dispatch, &res->dispatch4);
	} else if (isc_net_probeipv4() == ISC_R_SUCCESS) {
		struct in_addr ina;
		isc_sockaddr_t sa;

		/*
		 * Create an IPv4 UDP socket and a dispatcher for it.
		 */
		result = isc_socket_create(socketmgr, AF_INET,
					   isc_sockettype_udp,
					   &res->udpsocket4);
		if (result != ISC_R_SUCCESS)
			goto cleanup_buckets;
		result = ISC_R_UNEXPECTED;
		while (result != ISC_R_SUCCESS && port < 5400) {
			ina.s_addr = htonl(INADDR_ANY);
			isc_sockaddr_fromin(&sa, &ina, port);
			result = isc_socket_bind(res->udpsocket4, &sa);
			if (result != ISC_R_SUCCESS)
				port++;
		}
		if (result != ISC_R_SUCCESS) {
			RTRACE("Could not open UDP port");
			goto cleanup_buckets;
		}
		result = dns_dispatch_create(res->mctx, res->udpsocket4,
					     res->buckets[0].task, 4096,
					     1000, 32768, 16411, 16433, NULL,
					     &res->dispatch4);
		if (result != ISC_R_SUCCESS)
			goto cleanup_udpsocket4;
	}

	/*
	 * IPv6 Dispatcher.
	 */
	res->dispatch6 = NULL;
	res->udpsocket6 = NULL;
	if (isc_net_probeipv6() == ISC_R_SUCCESS) {
		/*
		 * Create an IPv6 UDP socket and a dispatcher for it.
		 */
		result = isc_socket_create(socketmgr, AF_INET6,
					   isc_sockettype_udp,
					   &res->udpsocket6);
		if (result != ISC_R_SUCCESS)
			goto cleanup_dispatch4;
		result = dns_dispatch_create(res->mctx, res->udpsocket6,
					     res->buckets[0].task, 4096, 
					     1000, 32768, 16411, 16433, NULL,
					     &res->dispatch6);
		if (result != ISC_R_SUCCESS)
			goto cleanup_udpsocket6;
	}

	/*
	 * Forwarding.
	 */
	ISC_LIST_INIT(res->forwarders);
	res->fwdpolicy = dns_fwdpolicy_none;

	res->references = 1;
	res->exiting = ISC_FALSE;
	res->frozen = ISC_FALSE;
	ISC_LIST_INIT(res->whenshutdown);

	result = isc_mutex_init(&res->lock);
	if (result != ISC_R_SUCCESS)
		goto cleanup_dispatch6;

	res->magic = RES_MAGIC;
	
	*resp = res;

	return (ISC_R_SUCCESS);

 cleanup_dispatch6:
	if (res->dispatch6 != NULL)
		dns_dispatch_detach(&res->dispatch6);

 cleanup_udpsocket6:
	if (res->udpsocket6 != NULL)
		isc_socket_detach(&res->udpsocket6);

 cleanup_dispatch4:
	if (res->dispatch4 != NULL)
		dns_dispatch_detach(&res->dispatch4);

 cleanup_udpsocket4:
	if (res->udpsocket4 != NULL)
		isc_socket_detach(&res->udpsocket4);

 cleanup_buckets:
	for (i = 0; i < buckets_created; i++) {
		(void)isc_mutex_destroy(&res->buckets[i].lock);
		isc_task_shutdown(res->buckets[i].task);
		isc_task_detach(&res->buckets[i].task);
	}
	isc_mem_put(view->mctx, res->buckets,
		    res->nbuckets * sizeof (fctxbucket_t));

 cleanup_res:
	isc_mem_put(view->mctx, res, sizeof *res);

	return (result);
}

isc_result_t
dns_resolver_setforwarders(dns_resolver_t *res,
			   isc_sockaddrlist_t *forwarders)
{
	isc_sockaddr_t *sa, *nsa;

	/*
	 * Set the default forwarders to be used by the resolver.
	 */

	REQUIRE(VALID_RESOLVER(res));
	REQUIRE(!res->frozen);
	REQUIRE(!ISC_LIST_EMPTY(*forwarders));

	if (!ISC_LIST_EMPTY(res->forwarders))
		free_forwarders(res);

	for (sa = ISC_LIST_HEAD(*forwarders);
	     sa != NULL;
	     sa = ISC_LIST_NEXT(sa, link)) {
		nsa = isc_mem_get(res->mctx, sizeof *nsa);
		if (nsa == NULL) {
			free_forwarders(res);
			return (ISC_R_NOMEMORY);
		}
		/* XXXRTH  Create and use isc_sockaddr_copy(). */
		*nsa = *sa;
		ISC_LINK_INIT(nsa, link);
		ISC_LIST_APPEND(res->forwarders, nsa, link);
	}
	
	return (ISC_R_SUCCESS);
}

isc_result_t
dns_resolver_setfwdpolicy(dns_resolver_t *res, dns_fwdpolicy_t fwdpolicy) {

	/*
	 * Set the default forwarding policy to be used by the resolver.
	 */

	REQUIRE(VALID_RESOLVER(res));
	REQUIRE(!res->frozen);

	res->fwdpolicy = fwdpolicy;

	return (ISC_R_SUCCESS);
}

void
dns_resolver_freeze(dns_resolver_t *res) {

	/*
	 * Freeze resolver.
	 */

	REQUIRE(VALID_RESOLVER(res));
	REQUIRE(!res->frozen);

	res->frozen = ISC_TRUE;
}

void
dns_resolver_attach(dns_resolver_t *source, dns_resolver_t **targetp) {
	REQUIRE(VALID_RESOLVER(source));
	REQUIRE(targetp != NULL && *targetp == NULL);

	RRTRACE(source, "attach");
	LOCK(&source->lock);
	REQUIRE(!source->exiting);

	INSIST(source->references > 0);
	source->references++;
	INSIST(source->references != 0);
	UNLOCK(&source->lock);

	*targetp = source;
}

void
dns_resolver_whenshutdown(dns_resolver_t *res, isc_task_t *task,
			  isc_event_t **eventp)
{
	isc_task_t *clone;
	isc_event_t *event;

	REQUIRE(VALID_RESOLVER(res));
	REQUIRE(eventp != NULL);

	event = *eventp;
	*eventp = NULL;

	LOCK(&res->lock);
	
	if (res->exiting && res->activebuckets == 0) {
		/*
		 * We're already shutdown.  Send the event.
		 */
		event->sender = res;
		isc_task_send(task, &event);
	} else {
		clone = NULL;
		isc_task_attach(task, &clone);
		event->sender = clone;
		ISC_LIST_APPEND(res->whenshutdown, event, link);
	}
	
	UNLOCK(&res->lock);
}

void
dns_resolver_shutdown(dns_resolver_t *res) {
	unsigned int i;
	fetchctx_t *fctx;

	REQUIRE(VALID_RESOLVER(res));

	RTRACE("shutdown");
	
	LOCK(&res->lock);

	if (!res->exiting) {
		RTRACE("exiting");
		res->exiting = ISC_TRUE;

		for (i = 0; i < res->nbuckets; i++) {
			LOCK(&res->buckets[i].lock);
			for (fctx = ISC_LIST_HEAD(res->buckets[i].fctxs);
			     fctx != NULL;
			     fctx = ISC_LIST_NEXT(fctx, link))
				fctx_shutdown(fctx);
			if (res->udpsocket4 != NULL)
				isc_socket_cancel(res->udpsocket4,
						  res->buckets[i].task,
						  ISC_SOCKCANCEL_ALL);
			if (res->udpsocket6 != NULL)
				isc_socket_cancel(res->udpsocket6,
						  res->buckets[i].task,
						  ISC_SOCKCANCEL_ALL);
			res->buckets[i].exiting = ISC_TRUE;
			if (ISC_LIST_EMPTY(res->buckets[i].fctxs)) {
				INSIST(res->activebuckets > 0);
				res->activebuckets--;
			}
			UNLOCK(&res->buckets[i].lock);
		}
		if (res->activebuckets == 0)
			send_shutdown_events(res);
	}

	UNLOCK(&res->lock);
}

/*
 * XXXRTH  Do we need attach/detach semantics for the resolver and the
 *         adb?  They can't be used separately, and the references to
 *	   them in the view MUST exist until they're both shutdown.
 *	   Using create/destroy is probably better.  Allow attach/detach
 *	   to be done at the view level.
 */

void
dns_resolver_detach(dns_resolver_t **resp) {
	dns_resolver_t *res;
	isc_boolean_t need_destroy = ISC_FALSE;

	REQUIRE(resp != NULL);
	res = *resp;
	REQUIRE(VALID_RESOLVER(res));

	RTRACE("detach");

	LOCK(&res->lock);

	INSIST(res->references > 0);
	res->references--;
	if (res->references == 0) {
		INSIST(res->exiting && res->activebuckets == 0);
		need_destroy = ISC_TRUE;
	}

	UNLOCK(&res->lock);

	if (need_destroy)
		destroy(res);

	*resp = NULL;
}

static inline isc_boolean_t
fctx_match(fetchctx_t *fctx, dns_name_t *name, dns_rdatatype_t type,
	   unsigned int options)
{
	if (fctx->type != type || fctx->options != options)
		return (ISC_FALSE);
	return (dns_name_equal(&fctx->name, name));
}

static inline void
log_fetch(dns_name_t *name, dns_rdatatype_t type) {
	isc_buffer_t b;
	char text[1024];
	isc_region_t r;

	/*
	 * XXXRTH  Allow this to be turned on and off...
	 */

	isc_buffer_init(&b, (unsigned char *)text, sizeof text,
			ISC_BUFFERTYPE_TEXT);
	if (dns_name_totext(name, ISC_FALSE, &b) !=
	    ISC_R_SUCCESS)
		return;
	isc_buffer_available(&b, &r);
	if (r.length < 1)
		return;
	*r.base = ' ';
	isc_buffer_add(&b, 1);
	if (dns_rdatatype_totext(type, &b) != ISC_R_SUCCESS)
		return;
	isc_buffer_used(&b, &r);
	/* XXXRTH  Give them their own category? */
	isc_log_write(dns_lctx, DNS_LOGCATEGORY_RESOLVER,
		      DNS_LOGMODULE_RESOLVER, ISC_LOG_DEBUG(1),
		      "createfetch: %.*s", (int)r.length, (char *)r.base);
}

/*
 * XXXRTH  This routine takes an unconscionable number of arguments!
 *
 * Maybe caller should allocate an event and pass that in?  Something must
 * be done!
 */

isc_result_t
dns_resolver_createfetch(dns_resolver_t *res, dns_name_t *name,
			 dns_rdatatype_t type,
			 dns_name_t *domain, dns_rdataset_t *nameservers,
			 dns_forwarders_t *forwarders,
			 unsigned int options, isc_task_t *task,
			 isc_taskaction_t action, void *arg,
			 dns_rdataset_t *rdataset,
			 dns_rdataset_t *sigrdataset, 
			 dns_fetch_t **fetchp)
{
	dns_fetch_t *fetch;
	fetchctx_t *fctx = NULL;
	isc_result_t result;
	unsigned int bucketnum;
	isc_boolean_t new_fctx = ISC_FALSE;
	isc_event_t *event;

	(void)forwarders;

	REQUIRE(VALID_RESOLVER(res));
	REQUIRE(res->frozen);
	/* XXXRTH  Check for meta type */
	if (domain != NULL) {
		REQUIRE(DNS_RDATASET_VALID(nameservers));
		REQUIRE(nameservers->type == dns_rdatatype_ns);
	} else
		REQUIRE(nameservers == NULL);
	REQUIRE(forwarders == NULL);
	REQUIRE(!dns_rdataset_isassociated(rdataset));
	REQUIRE(sigrdataset == NULL ||
		!dns_rdataset_isassociated(sigrdataset));
	REQUIRE(fetchp != NULL && *fetchp == NULL);

	log_fetch(name, type);

	/*
	 * XXXRTH  use a mempool?
	 */
	fetch = isc_mem_get(res->mctx, sizeof *fetch);
	if (fetch == NULL)
		return (ISC_R_NOMEMORY);

	bucketnum = dns_name_hash(name, ISC_FALSE) % res->nbuckets;

	LOCK(&res->buckets[bucketnum].lock);

	if (res->buckets[bucketnum].exiting) {
		result = ISC_R_SHUTTINGDOWN;
		goto unlock;
	}

	if ((options & DNS_FETCHOPT_UNSHARED) == 0) {
		for (fctx = ISC_LIST_HEAD(res->buckets[bucketnum].fctxs);
		     fctx != NULL;
		     fctx = ISC_LIST_NEXT(fctx, link)) {
			if (fctx_match(fctx, name, type, options))
				break;
		}
	}

	if (fctx == NULL || fctx->state == fetchstate_done) {
		fctx = NULL;
		result = fctx_create(res, name, type, domain, nameservers,
				     options, bucketnum, &fctx);
		if (result != ISC_R_SUCCESS)
			goto unlock;
		new_fctx = ISC_TRUE;
	}
	result = fctx_join(fctx, task, action, arg,
			   rdataset, sigrdataset, fetch);
	if (new_fctx) {
		if (result == ISC_R_SUCCESS) {
			/*
			 * Launch this fctx.
			 */
			event = &fctx->control_event;
			ISC_EVENT_INIT(event, sizeof *event, 0, NULL,
				       DNS_EVENT_FETCHCONTROL,
				       fctx_start, fctx, (void *)fctx_create,
				       NULL, NULL);
			isc_task_send(res->buckets[bucketnum].task, &event);
		} else {
			/*
			 * We don't care about the result of fctx_destroy()
			 * since we know we're not exiting.
			 */
			(void)fctx_destroy(fctx);
		}
	}

 unlock:
	UNLOCK(&res->buckets[bucketnum].lock);

	if (result == ISC_R_SUCCESS) {
		FTRACE("created");
		*fetchp = fetch;
	} else
		isc_mem_put(res->mctx, fetch, sizeof *fetch);

	return (result);
}

void
dns_resolver_cancelfetch(dns_resolver_t *res, dns_fetch_t *fetch) {
	fetchctx_t *fctx;
	dns_fetchevent_t *event, *next_event;
	isc_task_t *etask;

	REQUIRE(VALID_RESOLVER(res));
	REQUIRE(res->frozen);
	REQUIRE(DNS_FETCH_VALID(fetch));
	fctx = fetch->private;
	REQUIRE(VALID_FCTX(fctx));

	FTRACE("cancelfetch");

	LOCK(&res->buckets[fctx->bucketnum].lock);

	event = NULL;
	if (fctx->state != fetchstate_done) {
		for (event = ISC_LIST_HEAD(fctx->events);
		     event != NULL;
		     event = next_event) {
			next_event = ISC_LIST_NEXT(event, link);
			if (event->fetch == fetch) {
				ISC_LIST_UNLINK(fctx->events, event,
						link);
				break;
			}
		}
	}
	if (event != NULL) {
		etask = event->sender;
		event->result = ISC_R_CANCELED;
		isc_task_sendanddetach(&etask, (isc_event_t **)&event);
	}

	UNLOCK(&res->buckets[fctx->bucketnum].lock);
}

void
dns_resolver_destroyfetch(dns_resolver_t *res, dns_fetch_t **fetchp) {
	dns_fetch_t *fetch;
	dns_fetchevent_t *event, *next_event;
	fetchctx_t *fctx;
	unsigned int bucketnum;
	isc_boolean_t bucket_empty = ISC_FALSE;

	REQUIRE(VALID_RESOLVER(res));
	REQUIRE(res->frozen);
	REQUIRE(fetchp != NULL);
	fetch = *fetchp;
	REQUIRE(DNS_FETCH_VALID(fetch));
	fctx = fetch->private;
	REQUIRE(VALID_FCTX(fctx));

	FTRACE("destroyfetch");

	bucketnum = fctx->bucketnum;
	LOCK(&res->buckets[bucketnum].lock);

	/*
	 * Sanity check: the caller should have gotten its event before
	 * trying to destroy the fetch.
	 */
	event = NULL;
	if (fctx->state != fetchstate_done) {
		for (event = ISC_LIST_HEAD(fctx->events);
		     event != NULL;
		     event = next_event) {
			next_event = ISC_LIST_NEXT(event, link);
			RUNTIME_CHECK(event->fetch != fetch);
		}
	}

	INSIST(fctx->references > 0);
	fctx->references--;
	if (fctx->references == 0) {
		/*
		 * No one cares about the result of this fetch anymore.
		 */
		if (fctx->pending == 0 && fctx->validating == 0 &&
		    SHUTTINGDOWN(fctx)) {
			/*
			 * This fctx is already shutdown; we were just
			 * waiting for the last reference to go away.
			 */
			bucket_empty = fctx_destroy(fctx);
		} else {
			/*
			 * Initiate shutdown.
			 */
			fctx_shutdown(fctx);
		}
	}

	UNLOCK(&res->buckets[bucketnum].lock);

	isc_mem_put(res->mctx, fetch, sizeof *fetch);
	*fetchp = NULL;

	if (bucket_empty)
		empty_bucket(res);
}
