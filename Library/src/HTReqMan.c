/*								     HTReqMan.c
**	REQUEST MANAGER
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**	@(#) $Id$
**
** Authors
**	TBL	Tim Berners-Lee timbl@w3.org
**	JFG	Jean-Francois Groff jfg@dxcern.cern.ch
**	DD	Denis DeLaRoca (310) 825-4580  <CSP1DWD@mvs.oac.ucla.edu>
**	HFN	Henrik Frystyk, frystyk@w3.org
** History
**       8 Jun 92 Telnet hopping prohibited as telnet is not secure TBL
**	26 Jun 92 When over DECnet, suppressed FTP, Gopher and News. JFG
**	 6 Oct 92 Moved HTClientHost and HTlogfile into here. TBL
**	17 Dec 92 Tn3270 added, bug fix. DD
**	 4 Feb 93 Access registration, Search escapes bad chars TBL
**		  PARAMETERS TO HTSEARCH AND HTLOADRELATIVE CHANGED
**	28 May 93 WAIS gateway explicit if no WAIS library linked in.
**	   Dec 93 Bug change around, more reentrant, etc
**	09 May 94 logfile renamed to HTlogfile to avoid clash with WAIS
**	 8 Jul 94 Insulate free() from _free structure element.
**	02 Sep 95 Rewritten and spawned from HTAccess.c, HFN
*/

#if !defined(HT_DIRECT_WAIS) && !defined(HT_DEFAULT_WAIS_GATEWAY)
#define HT_DEFAULT_WAIS_GATEWAY "http://www.w3.org:8001/"
#endif

/* Library include files */
#include "sysdep.h"
#include "WWWUtil.h"
#include "HTParse.h"
#include "HTAlert.h"
#include "HTError.h"
#include "HTNetMan.h"
#include "HTEvent.h"
#include "HTProt.h"
#include "HTHeader.h"
#include "HTLib.h"
#include "HTReqMan.h"					 /* Implemented here */

#ifndef HT_MAX_RELOADS
#define HT_MAX_RELOADS	6
#endif

PRIVATE int HTMaxRetry = HT_MAX_RELOADS;

struct _HTStream {
	HTStreamClass * isa;
	/* ... */
};

/* --------------------------------------------------------------------------*/
/*			Create and delete the HTRequest Object		     */
/* --------------------------------------------------------------------------*/

PUBLIC HTRequest * HTRequest_new (void)
{
    HTRequest * me;
    if ((me = (HTRequest *) HT_CALLOC(1, sizeof(HTRequest))) == NULL)
        HT_OUTOFMEM("HTRequest_new()");
    
   /* Force Reload */
    me->reload = HT_CACHE_OK;

    /* Set the default user profile */
    me->userprofile = HTLib_userProfile();

    /* Format of output */
    me->output_format	= WWW_PRESENT;	    /* default it to present to user */
    me->debug_format	= WWW_DEBUG;	 /* default format of error messages */

    /* HTTP headers */
    me->GenMask		= DEFAULT_GENERAL_HEADERS;
    me->RequestMask	= DEFAULT_REQUEST_HEADERS;
    me->ResponseMask	= DEFAULT_RESPONSE_HEADERS;
    me->EntityMask	= DEFAULT_ENTITY_HEADERS;

    /* Default retry after value */
    me->priority = HT_PRIORITY_MAX;

    /* Default max forward value */
    me->max_forwards = -1;

    /* Content negotiation */
    me->ContentNegotiation = YES;		       /* Do this by default */

    if (CORE_TRACE) HTTrace("Request..... Created %p\n", me);

    return me;
}

/*	HTRequest_clear
**	---------------
**	Clears all protocol specific information so that the request object
**	can be used for another request.
**	Returns YES if OK, else NO
*/
PUBLIC BOOL HTRequest_clear (HTRequest * me)
{
    if (me) {
	me->error_stack = NULL;
	me->net = NULL;
	me->realm = NULL;
	me->credentials = NULL;
	me->connected = NO;
	if (me->response) {
	    HTResponse_delete(me->response);
	    me->response = NULL;
	}
	return YES;
    }
    return NO;
}

/*	HTRequest_dup
**	-------------
**	Creates a new HTRequest object as a duplicate of the src request.
**	Returns YES if OK, else NO
*/
PUBLIC HTRequest * HTRequest_dup (HTRequest * src)
{
    HTRequest * me;
    if (!src) return NULL;
    if ((me = (HTRequest  *) HT_MALLOC(sizeof(HTRequest))) == NULL)
        HT_OUTOFMEM("HTRequest_dup");
    memcpy(me, src, sizeof(HTRequest));
    if (CORE_TRACE) HTTrace("Request..... Duplicated %p to %p\n", src, me);
    return me;
}

/*	HTRequest_dupInternal
**	---------------------
**	Creates a new HTRequest object as a duplicate of the src request.
**	The difference to the HTRequest_dup function is that we don't copy the
**	error_stack and other information that the application keeps in its
**	copy of the request object. Otherwise it will be freed multiple times
**	Returns YES if OK, else NO
*/
PUBLIC HTRequest * HTRequest_dupInternal (HTRequest * src)
{
    HTRequest * me;
    if (!src) return 0;
    if ((me = (HTRequest  *) HT_MALLOC(sizeof(HTRequest))) == NULL)
        HT_OUTOFMEM("HTRequest_dup");
    memcpy(me, src, sizeof(HTRequest));
    HTRequest_clear(me);
    return me;
}

PUBLIC void HTRequest_delete (HTRequest * me)
{
    if (me) {
	if (CORE_TRACE) HTTrace("Request..... Delete %p\n", me);
	if (me->net) HTNet_setRequest(me->net, NULL);

	/* Should we delete the output stream? */
	if (!me->connected && me->output_stream) {
	    if (CORE_TRACE)
		HTTrace("Request..... Deleting dangling output stream\n");
	    (*me->output_stream->isa->_free)(me->output_stream);
	    me->output_stream = NULL;
	}

	/* Clean up the error stack */
	if (me->error_stack) HTError_deleteAll(me->error_stack);

	/* Before and After Filters */
	if (me->afters) HTList_delete(me->afters);
	if (me->befores) HTList_delete(me->befores);

	/* Access Authentication */
	HT_FREE(me->realm);
	if (me->credentials) HTAssocList_delete(me->credentials);

	/* Cache control headers */
	if (me->cache_control)
	    HTAssocList_delete(me->cache_control);

	/* Byte ranges */
	if (me->byte_ranges) HTAssocList_delete(me->byte_ranges);

	/* Connection headers */
	if (me->connection) HTAssocList_delete(me->connection);

	/* Proxy information */
	HT_FREE(me->proxy);

	/* PEP Information */
	if (me->protocol) HTAssocList_delete(me->protocol);
	if (me->protocol_request) HTAssocList_delete(me->protocol_request);
	if (me->protocol_info) HTAssocList_delete(me->protocol_info);

	/* Any response object */
	if (me->response) HTResponse_delete(me->response);

	HT_FREE(me);
    }
}

/*
**	Kill this request
*/
PUBLIC BOOL HTRequest_kill(HTRequest * me)
{
    return me ? HTNet_kill(me->net) : NO;
}

/* --------------------------------------------------------------------------*/
/*			Methods on the HTRequest Object			     */
/* --------------------------------------------------------------------------*/

/*
**  Internal request object
*/
PUBLIC BOOL HTRequest_setInternal (HTRequest * me, BOOL mode)
{
    if (me) {
	me->internal = mode;
	return YES;
    }
    return NO;
}

PUBLIC BOOL HTRequest_internal (HTRequest * me)
{
    return (me ? me->internal : NO);
}

/*
**	Date/time stamp when then request was issued
**	This is normally set when generating the request headers.
*/
PUBLIC time_t HTRequest_date (HTRequest * me)
{
    return me ? me->date : -1;
}

PUBLIC BOOL HTRequest_setDate (HTRequest * me, time_t date)
{
    if (me) {
	me->date = date;
	return YES;
    }
    return NO;
}

/*
**	Method
*/
PUBLIC void HTRequest_setMethod (HTRequest * me, HTMethod method)
{
    if (me) me->method = method;
}

PUBLIC HTMethod HTRequest_method (HTRequest * me)
{
    return me ? me->method : METHOD_INVALID;
}

/*
**  Priority to be inherited by all HTNet object hanging off this request
**  The priority can later be chaned by calling the HTNet object directly
*/
PUBLIC BOOL HTRequest_setPriority (HTRequest * me, HTPriority priority)
{
    if (me) {
	me->priority = priority;
	return YES;
    }
    return NO;
}

PUBLIC HTPriority HTRequest_priority (HTRequest * me)
{
    return (me ? me->priority : HT_PRIORITY_INV);
}

/*
**	User Profile
*/
PUBLIC BOOL HTRequest_setUserProfile (HTRequest * me, HTUserProfile * up)
{
    if (me) {
	me->userprofile = up;
	return YES;
    }
    return NO;
}

PUBLIC HTUserProfile * HTRequest_userProfile (HTRequest * me)
{
    return me ? me->userprofile : NULL;
}

/*
**	Net Object
*/
PUBLIC BOOL HTRequest_setNet (HTRequest * me, HTNet * net)
{
    if (me) {
	me->net = net;
	return YES;
    }
    return NO;
}

PUBLIC HTNet * HTRequest_net (HTRequest * me)
{
    return me ? me->net : NULL;
}

/*
**	Response Object. If the object does not exist then create it at the
**	same time it is asked for.
*/
PUBLIC HTResponse * HTRequest_response (HTRequest * me)
{
    if (me) {
	if (!me->response)
	    me->response = HTResponse_new();
	return me->response;
    }
    return NULL;
}

PUBLIC BOOL HTRequest_setResponse (HTRequest * me, HTResponse * response)
{
    if (me) {
	me->response = response;
	return YES;
    }
    return NO;
}

/*	Error Management
**	----------------
**	Returns the error stack if a stream is 
*/
PUBLIC HTList * HTRequest_error (HTRequest * me)
{
    return me ? me->error_stack : NULL;
}

PUBLIC void HTRequest_setError (HTRequest * me, HTList * list)
{
    if (me) me->error_stack = list;
}

PUBLIC BOOL HTRequest_addError (HTRequest * 	me,
				HTSeverity	severity,
				BOOL		ignore,
				int		element,
				void *		par,
				unsigned int	length,
				char *		where)
{
    if (me) {
	if (!me->error_stack) me->error_stack = HTList_new();
	return HTError_add(me->error_stack, severity, ignore, element,
			   par, length, where);
    }
    return NO;
}

PUBLIC BOOL HTRequest_addSystemError (HTRequest * 	me,
				      HTSeverity 	severity,
				      int		errornumber,
				      BOOL		ignore,
				      char *		syscall)
{
    if (me) {
	if (!me->error_stack) me->error_stack = HTList_new();
	return HTError_addSystem(me->error_stack, severity, errornumber,
				 ignore, syscall);
    }
    return NO;
}

/*
**  Set max number of automatic reload. Default is HT_MAX_RELOADS
*/
PUBLIC BOOL HTRequest_setMaxRetry (int newmax)
{
    if (newmax > 0) {
	HTMaxRetry = newmax;
	return YES;
    }
    return NO;
}

PUBLIC int HTRequest_maxRetry (void)
{
    return HTMaxRetry;
}

PUBLIC int HTRequest_retrys (HTRequest * me)
{
    return me ? me->retrys : 0;
}

PUBLIC BOOL HTRequest_addRetry (HTRequest * me)
{
    return (me && me->retrys++);
}

/*
**	Should we try again?
**	--------------------
**	Returns YES if we are to retry the load, NO otherwise. We check
**	this so that we don't go into an infinte loop
*/
PUBLIC BOOL HTRequest_doRetry (HTRequest * me)
{
    return (me && me->retrys < HTMaxRetry-1);
}

/*
**	Socket mode: preemptive or non-preemptive (blocking or non-blocking)
*/
PUBLIC void HTRequest_setPreemptive (HTRequest * me, BOOL mode)
{
    if (me) me->preemptive = mode;
}

PUBLIC BOOL HTRequest_preemptive (HTRequest * me)
{
    return me ? me->preemptive : NO;
}

/*
**	Should we use content negotiation?
*/
PUBLIC void HTRequest_setNegotiation (HTRequest * me, BOOL mode)
{
    if (me) me->ContentNegotiation = mode;
}

PUBLIC BOOL HTRequest_negotiation (HTRequest * me)
{
    return me ? me->ContentNegotiation : NO;
}

/*
**	Set General Headers
*/
PUBLIC void HTRequest_setGnHd (HTRequest * me, HTGnHd gnhd)
{
    if (me) me->GenMask = gnhd;
}

PUBLIC void HTRequest_addGnHd (HTRequest * me, HTGnHd gnhd)
{
    if (me) me->GenMask |= gnhd;
}

PUBLIC HTGnHd HTRequest_gnHd (HTRequest * me)
{
    return me ? me->GenMask : 0;
}

/*
**	Set Request Headers
*/
PUBLIC void HTRequest_setRqHd (HTRequest * me, HTRqHd rqhd)
{
    if (me) me->RequestMask = rqhd;
}

PUBLIC void HTRequest_addRqHd (HTRequest * me, HTRqHd rqhd)
{
    if (me) me->RequestMask |= rqhd;
}

PUBLIC HTRqHd HTRequest_rqHd (HTRequest * me)
{
    return me ? me->RequestMask : 0;
}

/*
**	Set Response Headers
*/
PUBLIC void HTRequest_setRsHd (HTRequest * me, HTRsHd rshd)
{
    if (me) me->ResponseMask = rshd;
}

PUBLIC void HTRequest_addRsHd (HTRequest * me, HTRsHd rshd)
{
    if (me) me->ResponseMask |= rshd;
}

PUBLIC HTRsHd HTRequest_rsHd (HTRequest * me)
{
    return me ? me->ResponseMask : 0;
}

/*
**	Set Entity Headers (for the object)
*/
PUBLIC void HTRequest_setEnHd (HTRequest * me, HTEnHd enhd)
{
    if (me) me->EntityMask = enhd;
}

PUBLIC void HTRequest_addEnHd (HTRequest * me, HTEnHd enhd)
{
    if (me) me->EntityMask |= enhd;
}

PUBLIC HTEnHd HTRequest_enHd (HTRequest * me)
{
    return me ? me->EntityMask : 0;
}

/*
**	Extra Header Generators. list can be NULL
*/
PUBLIC void HTRequest_setGenerator (HTRequest * me, HTList *generator,
				    BOOL override)
{
    if (me) {
	me->generators = generator;
	me->gens_local = override;
    }
}

PUBLIC HTList * HTRequest_generator (HTRequest * me, BOOL *override)
{
    if (me) {
	*override = me->gens_local;
	return me->generators;
    }
    return NULL;
}

/*
**	Extra Header Parsers. list can be NULL
*/
PUBLIC void HTRequest_setMIMEParseSet (HTRequest * me, 
				       HTMIMEParseSet * parseSet, BOOL local)
{
    if (me) {
        me->parseSet = parseSet;
	me->pars_local = local;
    }
}

PUBLIC HTMIMEParseSet * HTRequest_MIMEParseSet (HTRequest * me, BOOL * pLocal)
{
    if (me) {
        if (pLocal) *pLocal = me->pars_local;
	return me->parseSet;
    }
    return NULL;
}

/*
**	Accept Format Types
**	list can be NULL
*/
PUBLIC void HTRequest_setConversion (HTRequest * me, HTList *type,
				     BOOL override)
{
    if (me) {
	me->conversions = type;
	me->conv_local = override;
    }
}

PUBLIC HTList * HTRequest_conversion (HTRequest * me)
{
    return me ? me->conversions : NULL;
}

/*
**	Accept Encoding 
**	list can be NULL
*/
PUBLIC void HTRequest_setEncoding (HTRequest * me, HTList *enc,
				   BOOL override)
{
    if (me) {
	me->encodings = enc;
	me->enc_local = override;
    }
}

PUBLIC HTList * HTRequest_encoding (HTRequest * me)
{
    return me ? me->encodings : NULL;
}

/*
**	Accept Transfer Encoding 
**	list can be NULL
*/
PUBLIC void HTRequest_setTransfer (HTRequest * me,
				   HTList * cte, BOOL override)
{
    if (me) {
	me->ctes = cte;
	me->cte_local = override;
    }
}

PUBLIC HTList * HTRequest_transfer (HTRequest * me)
{
    return me ? me->ctes : NULL;
}

/*
**	Accept Language
**	list can be NULL
*/
PUBLIC void HTRequest_setLanguage (HTRequest * me, HTList *lang,
				   BOOL override)
{
    if (me) {
	me->languages = lang;
	me->lang_local = override;
    }
}

PUBLIC HTList * HTRequest_language (HTRequest * me)
{
    return me ? me->languages : NULL;
}

/*
**	Accept Charset
**	list can be NULL
*/
PUBLIC void HTRequest_setCharset (HTRequest * me, HTList *charset,
				  BOOL override)
{
    if (me) {
	me->charsets = charset;
	me->char_local = override;
    }
}

PUBLIC HTList * HTRequest_charset (HTRequest * me)
{
    return me ? me->charsets : NULL;
}

/*
**	Are we using the full URL in the request or not?
*/
PUBLIC void HTRequest_setFullURI (HTRequest * me, BOOL mode)
{
    if (me) me->full_uri = mode;
}

PUBLIC BOOL HTRequest_fullURI (HTRequest * me)
{
    return me ? me->full_uri : NO;
}

/*
**	Are we using a proxy or not and in that case, which one?
*/
PUBLIC BOOL HTRequest_setProxy (HTRequest * me, const char * proxy)
{
    if (me && proxy) {
	StrAllocCopy(me->proxy, proxy);
	return YES;
    }
    return NO;
}

PUBLIC char * HTRequest_proxy (HTRequest * me)
{
    return me ? me->proxy : NULL;
}

PUBLIC BOOL HTRequest_deleteProxy (HTRequest * me)
{
    if (me) {
	HT_FREE(me->proxy);
	return YES;
    }
    return NO;
}

/*
**	Reload Mode
*/
PUBLIC void HTRequest_setReloadMode (HTRequest * me, HTReload mode)
{
    if (me) me->reload = mode;
}

PUBLIC HTReload HTRequest_reloadMode (HTRequest * me)
{
    return me ? me->reload : HT_CACHE_OK;
}

/*
**	Cache control directives. The cache control can be initiated by both
**	the server and the client which is the reason for keeping two lists
*/
PUBLIC BOOL HTRequest_addCacheControl (HTRequest * me,
				       char * token, char * value)
{
    if (me) {
	if (!me->cache_control) me->cache_control = HTAssocList_new();
	return HTAssocList_replaceObject(me->cache_control, token, value);
    }
    return NO;
}

PUBLIC BOOL HTRequest_deleteCacheControl (HTRequest * me)
{
    if (me && me->cache_control) {
	HTAssocList_delete(me->cache_control);
	me->cache_control = NULL;
	return YES;
    }
    return NO;
}

PUBLIC HTAssocList * HTRequest_cacheControl (HTRequest * me)
{
    return (me ? me->cache_control : NULL);
}

/*
**  Byte ranges
*/
PUBLIC BOOL HTRequest_deleteRange (HTRequest * me)
{
    if (me && me->byte_ranges) {
	HTAssocList_delete(me->byte_ranges);
	me->byte_ranges = NULL;
	return YES;
    }
    return NO;
}

PUBLIC BOOL HTRequest_addRange (HTRequest * me, char * unit, char * range)
{
    if (me) {
	if (!me->byte_ranges) me->byte_ranges = HTAssocList_new();
	return HTAssocList_replaceObject(me->byte_ranges, unit, range);
    }
    return NO;
}

PUBLIC HTAssocList * HTRequest_range (HTRequest * me)
{
    return (me ? me->byte_ranges : NULL);
}

/*
**	Connection directives. The connection directies can be initiated by
**	both the server and the client which is the reason for keeping two
**	lists
*/
PUBLIC BOOL HTRequest_addConnection (HTRequest * me,
				     char * token, char * value)
{
    if (me) {
	if (!me->connection) me->connection = HTAssocList_new();
	return HTAssocList_replaceObject(me->connection, token, value);
    }
    return NO;
}

PUBLIC BOOL HTRequest_deleteConnection (HTRequest * me)
{
    if (me && me->connection) {
	HTAssocList_delete(me->connection);
	me->connection = NULL;
	return YES;
    }
    return NO;
}

PUBLIC HTAssocList * HTRequest_connection (HTRequest * me)
{
    return (me ? me->connection : NULL);
}

/*
**  Access Authentication Credentials
*/
PUBLIC BOOL HTRequest_deleteCredentialsAll (HTRequest * me)
{
    if (me && me->credentials) {
	HTAssocList_delete(me->credentials);
	me->credentials = NULL;
	return YES;
    }
    return NO;
}

PUBLIC BOOL HTRequest_addCredentials (HTRequest * me,
				    char * token, char * value)
{
    if (me) {
	if (!me->credentials) me->credentials = HTAssocList_new();
	return HTAssocList_addObject(me->credentials, token, value);
    }
    return NO;
}

PUBLIC HTAssocList * HTRequest_credentials (HTRequest * me)
{
    return (me ? me->credentials : NULL);
}

/*
**  Access Authentication Realms
*/
PUBLIC BOOL HTRequest_setRealm (HTRequest * me, char * realm)
{
    if (me && realm) {
	StrAllocCopy(me->realm, realm);
	return YES;
    }
    return NO;
}

PUBLIC const char * HTRequest_realm (HTRequest * me)
{
    return (me ? me->realm : NULL);
}

/*
**  PEP Protocol header
*/
PUBLIC BOOL HTRequest_addProtocol (HTRequest * me,
				   char * token, char * value)
{
    if (me) {
	if (!me->protocol) me->protocol = HTAssocList_new();
	return HTAssocList_addObject(me->protocol, token,value);
    }
    return NO;
}

PUBLIC BOOL HTRequest_deleteProtocolAll (HTRequest * me)
{
    if (me && me->protocol) {
	HTAssocList_delete(me->protocol);
	me->protocol = NULL;
	return YES;
    }
    return NO;
}

PUBLIC HTAssocList * HTRequest_protocol (HTRequest * me)
{
    return (me ? me->protocol : NULL);
}

/*
**  PEP Protocol Info header
*/
PUBLIC BOOL HTRequest_addProtocolInfo (HTRequest * me,
				       char * token, char * value)
{
    if (me) {
	if (!me->protocol_info) me->protocol_info = HTAssocList_new();
	return HTAssocList_addObject(me->protocol_info, token,value);
    }
    return NO;
}

PUBLIC BOOL HTRequest_deleteProtocolInfoAll (HTRequest * me)
{
    if (me && me->protocol_info) {
	HTAssocList_delete(me->protocol_info);
	me->protocol_info = NULL;
	return YES;
    }
    return NO;
}

PUBLIC HTAssocList * HTRequest_protocolInfo (HTRequest * me)
{
    return (me ? me->protocol_info : NULL);
}

/*
**  PEP Protocol request header
*/
PUBLIC BOOL HTRequest_addProtocolRequest (HTRequest * me,
					  char * token, char * value)
{
    if (me) {
	if (!me->protocol_request) me->protocol_request = HTAssocList_new();
	return HTAssocList_addObject(me->protocol_request, token,value);
    }
    return NO;
}

PUBLIC BOOL HTRequest_deleteProtocolRequestAll (HTRequest * me)
{
    if (me && me->protocol_request) {
	HTAssocList_delete(me->protocol_request);
	me->protocol_request = NULL;
	return YES;
    }
    return NO;
}

PUBLIC HTAssocList * HTRequest_protocolRequest (HTRequest * me)
{
    return (me ? me->protocol_request : NULL);
}

/*
**	Anchor
*/
PUBLIC void HTRequest_setAnchor (HTRequest * me, HTAnchor *anchor)
{
    if (me) {
	me->anchor = HTAnchor_parent(anchor);
	me->childAnchor = ((HTAnchor *) me->anchor != anchor) ?
	    (HTChildAnchor *) anchor : NULL;
    }
}

PUBLIC HTParentAnchor * HTRequest_anchor (HTRequest * me)
{
    return me ? me->anchor : NULL;
}

PUBLIC HTChildAnchor * HTRequest_childAnchor (HTRequest * me)
{
    return me ? me->childAnchor : NULL;
}

/*
**	Parent anchor for Referer field
*/
PUBLIC void HTRequest_setParent (HTRequest * me, HTParentAnchor *parent)
{
    if (me) me->parentAnchor = parent;
}

PUBLIC HTParentAnchor * HTRequest_parent (HTRequest * me)
{
    return me ? me->parentAnchor : NULL;
}

/*
**	Output stream
*/
PUBLIC void HTRequest_setOutputStream (HTRequest * me, HTStream *output)
{
    if (me) me->output_stream = output;
}

PUBLIC HTStream *HTRequest_outputStream (HTRequest * me)
{
    return me ? me->output_stream : NULL;
}

/*
**	Output format
*/
PUBLIC void HTRequest_setOutputFormat (HTRequest * me, HTFormat format)
{
    if (me) me->output_format = format;
}

PUBLIC HTFormat HTRequest_outputFormat (HTRequest * me)
{
    return me ? me->output_format : NULL;
}

/*
**	Debug stream
*/
PUBLIC void HTRequest_setDebugStream (HTRequest * me, HTStream *debug)
{
    if (me) me->debug_stream = debug;
}

PUBLIC HTStream *HTRequest_debugStream (HTRequest * me)
{
    return me ? me->debug_stream : NULL;
}

/*
**	Debug Format
*/
PUBLIC void HTRequest_setDebugFormat (HTRequest * me, HTFormat format)
{
    if (me) me->debug_format = format;
}

PUBLIC HTFormat HTRequest_debugFormat (HTRequest * me)
{
    return me ? me->debug_format : NULL;
}

/*
**	Input stream
*/
PUBLIC void HTRequest_setInputStream (HTRequest * me, HTStream *input)
{
    if (me) me->input_stream = input;
}

PUBLIC HTStream *HTRequest_inputStream (HTRequest * me)
{
    return me ? me->input_stream : NULL;
}

/*
**	Net before and after callbacks
*/
PUBLIC BOOL HTRequest_addBefore (HTRequest * me, HTNetBefore * filter,
				 const char * tmplate, void * param,
				 HTFilterOrder order, BOOL override)
{
    if (me) {
	me->befores_local = override;
	if (filter) {
	    if (!me->befores) me->befores = HTList_new();
	    return HTNetCall_addBefore(me->befores, filter,
				       tmplate, param, order);
	}
	return YES;			/* It's OK to register a NULL filter */
    }
    return NO;
}

PUBLIC BOOL HTRequest_deleteBefore (HTRequest * me, HTNetBefore * filter)
{
    if (me && me->befores)
	return HTNetCall_deleteBefore(me->befores, filter);
    return NO;
}

PUBLIC BOOL HTRequest_deleteBeforeAll (HTRequest * me)
{
    if (me && me->befores) {
	HTNetCall_deleteBeforeAll(me->befores);
	me->befores = NULL;
	me->befores_local = NO;
	return YES;
    }
    return NO;
}

PUBLIC HTList * HTRequest_before (HTRequest * me, BOOL *override)
{
    if (me) {
	*override = me->befores_local;
	return me->befores;
    }
    return NULL;
}

PUBLIC BOOL HTRequest_addAfter (HTRequest * me, HTNetAfter * filter,
				const char * tmplate, void * param,
				int status, HTFilterOrder order,
				BOOL override)
{
    if (me) {
	me->afters_local = override;
	if (filter) {
	    if (!me->afters) me->afters = HTList_new();
	    return HTNetCall_addAfter(me->afters, filter,
				      tmplate, param, status, order);
	}
	return YES;			/* It's OK to register a NULL filter */
    }
    return NO;
}

PUBLIC BOOL HTRequest_deleteAfter (HTRequest * me, HTNetAfter * filter)
{
    return (me && me->afters) ?
	HTNetCall_deleteAfter(me->afters, filter) : NO;
}

PUBLIC BOOL HTRequest_deleteAfterStatus (HTRequest * me, int status)
{
    return (me && me->afters) ?
	HTNetCall_deleteAfterStatus(me->afters, status) : NO;
}

PUBLIC BOOL HTRequest_deleteAfterAll (HTRequest * me)
{
    if (me && me->afters) {
	HTNetCall_deleteAfterAll(me->afters);
	me->afters = NULL;
	me->afters_local = NO;
	return YES;
    }
    return NO;
}

PUBLIC HTList * HTRequest_after (HTRequest * me, BOOL *override)
{
    if (me) {
	*override = me->afters_local;
	return me->afters;
    }
    return NULL;
}

/*
**	Call back function for context swapping
*/
PUBLIC void HTRequest_setCallback (HTRequest * me, HTRequestCallback *cbf)
{
    if (me) me->callback = cbf;
}

PUBLIC HTRequestCallback *HTRequest_callback (HTRequest * me)
{
    return me ? me->callback : NULL;
}

/*
**	Context pointer to be used in context call back function
*/
PUBLIC void HTRequest_setContext (HTRequest * me, void *context)
{
    if (me) me->context = context;
}

PUBLIC void *HTRequest_context (HTRequest * me)
{
    return me ? me->context : NULL;
}

/*
**	Has output stream been connected to the channel? If not then we
**	must free it explicitly when deleting the request object
*/
PUBLIC void HTRequest_setOutputConnected (HTRequest * me, BOOL mode)
{
    if (me) me->connected = mode;
}

PUBLIC BOOL HTRequest_outputConnected (HTRequest * me)
{
    return me ? me->connected : NO;
}

/*
**	Bytes read in this request
*/
PUBLIC long HTRequest_bodyRead(HTRequest * me)
{
    return me ? HTNet_bytesRead(me->net) - HTNet_headerLength(me->net) : -1;
}

/*
**	Bytes written in this request
*/
PUBLIC long HTRequest_bytesWritten (HTRequest * me)
{
    return me ? HTNet_bytesWritten(me->net) : -1;
}

/*
**	Handle the max forward header value
*/
PUBLIC BOOL HTRequest_setMaxForwards (HTRequest * me, int maxforwards)
{
    if (me && maxforwards >= 0) {
	me->max_forwards = maxforwards;
	HTRequest_addRqHd(me, HT_C_MAX_FORWARDS);	       /* Turn it on */
	return YES;
    }
    return NO;
}

PUBLIC int HTRequest_maxForwards (HTRequest * me)
{
    return me ? me->max_forwards : -1;
}

/*
**  Source request
*/
PUBLIC BOOL HTRequest_setSource (HTRequest * me, HTRequest * source)
{
    if (me) {
	me->source = source;
	return YES;
    }
    return NO;
}

PUBLIC HTRequest * HTRequest_source (HTRequest * me)
{
    return (me ? me->source : NULL);
}

PUBLIC BOOL HTRequest_isPostWeb (HTRequest * me)
{
    return (me ? me->source != NULL: NO);
}

/*
**	POST Call back function for sending data to the destination
*/
PUBLIC void HTRequest_setPostCallback (HTRequest * me, HTPostCallback *cbf)
{
    if (me) me->PostCallback = cbf;
}

PUBLIC HTPostCallback * HTRequest_postCallback (HTRequest * me)
{
    return me ? me->PostCallback : NULL;
}

/*
**	Entity Anchor
*/
PUBLIC BOOL HTRequest_setEntityAnchor (HTRequest * me,
				       HTParentAnchor * anchor)
{
    if (me) {
	me->source_anchor = anchor;
	return YES;
    }
    return NO;
}

PUBLIC HTParentAnchor * HTRequest_entityAnchor (HTRequest * me)
{
    return me ? me->source_anchor ? me->source_anchor :
	me->anchor : NULL;
}

/* ------------------------------------------------------------------------- */
/*				POST WEB METHODS	      	 	     */
/* ------------------------------------------------------------------------- */

/*
**  Add a destination request to this source request structure so that we
**  build the internal request representation of the POST web
**  Returns YES if OK, else NO
*/
PUBLIC BOOL HTRequest_addDestination (HTRequest * src, HTRequest * dest)
{
    if (src && dest) {
	dest->source = src->source = src;
	if (!src->mainDestination) {
	    src->mainDestination = dest;
	    src->destRequests = 1;
	    if (CORE_TRACE)
		HTTrace("POSTWeb..... Adding dest %p to src %p\n",
			 dest, src);
	    return YES;
	} else {
	    if (!src->destinations) src->destinations = HTList_new();
	    if (HTList_addObject(src->destinations, (void *) dest)==YES) {
		src->destRequests++;
		if (CORE_TRACE)
		    HTTrace("POSTWeb..... Adding dest %p to src %p\n",
			     dest, src);
		return YES;
	    }
	}
    }
    return NO;
}

/*
**  Remove a destination request from this source request structure
**  Remember only to delete the internal request objects as the other
**  comes from the application!
**  Returns YES if OK, else NO
*/
PUBLIC BOOL HTRequest_removeDestination (HTRequest * dest)
{
    BOOL found=NO;
    if (dest && dest->source) {
	HTRequest *src = dest->source;
	if (src->mainDestination == dest) {
	    dest->source = NULL;
	    src->mainDestination = NULL;
	    src->destRequests--;
	    found = YES;
	} else if (src->destinations) {
	    if (HTList_removeObject(src->destinations, (void *) dest)) {
		src->destRequests--;
		found = YES;
	    }
	}
	if (found) {
	    if (dest->internal) HTRequest_delete(dest);
	    if (CORE_TRACE)
	    	HTTrace("POSTWeb..... Deleting dest %p from src %p\n",
			 dest, src);
	}
	if (src->destRequests <= 0) {
	    if (CORE_TRACE)
		HTTrace("POSTWeb..... terminated\n");
	    if (src->internal) HTRequest_delete(src);
	}
    }
    return found;
}

/*
**  Check to see whether all destinations are ready. If so then enable the
**  source as ready for reading.
**  Returns YES if all dests are ready, NO otherwise
*/
PUBLIC BOOL HTRequest_destinationsReady (HTRequest * me)
{
    HTRequest * source = me ? me->source : NULL;
    if (source) {
	if (source->destStreams == source->destRequests) {
	    HTNet * net = source->net;
	    if (CORE_TRACE)
		HTTrace("POSTWeb..... All destinations are ready!\n");
	    if (net)			      /* Might already have finished */
		HTEvent_register(HTNet_socket(net), HTEvent_READ, &net->event);
	    return YES;
	}
    }
    return NO;
}

/*
**  Find the source request object and make the link between the 
**  source output stream and the destination input stream. There can be
**  a conversion between the two streams!
**  Returns YES if link is made, NO otherwise
*/
PUBLIC BOOL HTRequest_linkDestination (HTRequest *dest)
{
    if (dest && dest->input_stream && dest->source && dest!=dest->source) {
	HTRequest *source = dest->source;
	HTStream *pipe = HTStreamStack(source->output_format,
				       dest->input_format,
				       dest->input_stream,
				       dest, YES);

	/* Check if we are the only one - else spawn off T streams */
	/* @@@ We don't do this yet @@@ */

	/* Now set up output stream of the source */
	if (source->output_stream)
	    (*source->output_stream->isa->_free)(source->output_stream);
	source->output_stream = pipe ? pipe : dest->input_stream;

	if (CORE_TRACE)
	    HTTrace("POSTWeb..... Linking dest %p to src %p\n",
		     dest, source);
	if (++source->destStreams == source->destRequests) {
	    HTNet *net = source->net;
	    if (CORE_TRACE)
		HTTrace("POSTWeb..... All destinations ready!\n");
	    if (net)			      /* Might already have finished */
		HTEvent_register(HTNet_socket(net), HTEvent_READ, &net->event);
	    return YES;
	}
    }
    return NO;
}

/*
**  Remove a feed stream to a destination request from this source
**  request structure. When all feeds are removed the request tree is
**  ready to take down and the operation can be terminated.
**  Returns YES if removed, else NO
*/
PUBLIC BOOL HTRequest_unlinkDestination (HTRequest *dest)
{
    BOOL found = NO;
    if (dest && dest->source && dest != dest->source) {
	HTRequest *src = dest->source;
	if (src->mainDestination == dest) {
	    src->output_stream = NULL;
	    if (dest->input_stream)
		(*dest->input_stream->isa->_free)(dest->input_stream);
	    found = YES;
	} else if (src->destinations) {

	    /* LOOK THROUGH THE LIST AND FIND THE RIGHT ONE */

	}	
	if (found) {
	    src->destStreams--;
	    if (CORE_TRACE)
		HTTrace("POSTWeb..... Unlinking dest %p from src %p\n",
			 dest, src);
	    return YES;
	}
    }
    return NO;
}

/*
**  Removes all request structures in this PostWeb.
*/
PUBLIC BOOL HTRequest_removePostWeb (HTRequest *me)
{
    if (me && me->source) {
	HTRequest *source = me->source;

	/* Kill main destination */
	if (source->mainDestination)
	    HTRequest_removeDestination(source->mainDestination);

	/* Kill all other destinations */
	if (source->destinations) {
	    HTList *cur = source->destinations;
	    HTRequest *pres;
	    while ((pres = (HTRequest *) HTList_nextObject(cur)) != NULL)
		HTRequest_removeDestination(pres);
	}

	/* Remove source request */
	HTRequest_removeDestination(source);
	return YES;
    }
    return NO;
}

/*
**  Kills all threads in a POST WEB connected to this request but
**  NOT this request itself. We also keep the request structures.
**  Some requests might be preemptive, for example a SMTP request (when
**  that has been implemented). However, this will be handled internally
**  in the load function.
*/
PUBLIC BOOL HTRequest_killPostWeb (HTRequest *me)
{
    if (me && me->source) {
	HTRequest *source = me->source;
	if (CORE_TRACE) HTTrace("POSTWeb..... Killing\n");

	/*
	** Kill source. The stream tree is now freed so we have to build
	** that again. This is done in HTRequest_linkDestination()
	*/
	if (me != source) {
	    HTNet_kill(source->net);
	    source->output_stream = NULL;
	}

	/* Kill all other destinations */
	if (source->destinations) {
	    HTList *cur = source->destinations;
	    HTRequest *pres;
	    while ((pres = (HTRequest *) HTList_nextObject(cur)) != NULL)
		if (me != pres) HTNet_kill(pres->net);
	}

	/* Kill main destination */
	if (source->mainDestination && me != source->mainDestination)
	    HTNet_kill(source->mainDestination->net);
	return YES;
    }
    return NO;
}

PUBLIC int HTRequest_forceFlush (HTRequest * request)
{
    HTHost * host = HTNet_host(request->net);
    if (host == NULL) return HT_ERROR;
    return HTHost_forceFlush(host);
}

/* --------------------------------------------------------------------------*/
/*				Document Loader 			     */
/* --------------------------------------------------------------------------*/

/*	Request a resource
**	------------------
**	This is an internal routine, which has an address AND a matching
**	anchor.  (The public routines are called with one OR the other.)
**	Returns:
**		YES	if request has been registered (success)
**		NO	an error occured
*/
PUBLIC BOOL HTLoad (HTRequest * me, BOOL recursive)
{
    if (!me || !me->anchor) {
        if (CORE_TRACE) HTTrace("Load Start.. Bad argument\n");
        return NO;
    }

    /* Make sure that we don't carry over any old physical address */
    HTAnchor_clearPhysical(me->anchor);

    /* Set the default method if not already done */
    if (me->method == METHOD_INVALID) me->method = METHOD_GET;

    /* Should we keep the error stack or not? */
    if (!recursive && me->error_stack) {
	HTError_deleteAll(me->error_stack);
	me->error_stack = NULL;
    }

    /* Delete any old Response Object */
    if (me->response) {
	HTResponse_delete(me->response);
	me->response = NULL;
    }

    /*
    **  We set the start point of handling a request to here.
    **  This time will be used by the cache
    */
    HTRequest_setDate(me, time(NULL));

    /* Now start the Net Manager */
    return HTNet_newClient(me);
}

