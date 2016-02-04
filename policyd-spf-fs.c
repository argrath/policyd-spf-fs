/*
 * policyd-spf-fs - SPF Policy Deamon for Postfix
 *
 *  Author: Matthias Cramer <cramer@freestone.net>
 *
 * $Id: policyd-spf-fs.c 27 2007-09-11 11:06:29Z cramer $
 *
 *  Based on spfquery from libspf2 by Wayne Schlitt <wayne@midwestcs.com>
 *
 *  Postfix interface functions based on policyd-1.0.1 by Shevek <spf@anarres.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of either:
 *
 *   a) The GNU Lesser General Public License as published by the Free
 *	  Software Foundation; either version 2.1, or (at your option) any
 *	  later version,
 *
 *   OR
 *
 *   b) The two-clause BSD license.
 *
 *
 * The two-clause BSD license:
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *	notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	notice, this list of conditions and the following disclaimer in the
 *	documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define SPF_TEST_VERSION  "3.0"

#include <stdio.h>
#include <stdlib.h>	   /* malloc / free */
#include <sys/types.h>	/* types (u_char .. etc..) */
#include <inttypes.h>
#include <string.h>	   /* strstr / strdup */
#include <sys/socket.h>   /* inet_ functions / structs */
#include <netinet/in.h>   /* inet_ functions / structs */
#include <arpa/inet.h>	/* in_addr struct */
#include <getopt.h>
#include <syslog.h>
#include <unistd.h>
#include <netdb.h>

extern int h_errno;    /* for netdb */

#include "spf.h"
#include "spf_dns.h"
#include "spf_dns_null.h"
#include "spf_dns_test.h"
#include "spf_dns_cache.h"
#include "spf_dns_resolv.h"



#define TRUE 1
#define FALSE 0

#define REQUEST_LIMIT 100
#define RESULTSIZE      1024

#define POSTFIX_DUNNO   "DUNNO"
#define POSTFIX_REJECT  "REJECT"

#define DEFAULT_EXPLANATION "Please see http://www.openspf.org/Why?id=%{S}&ip=%{C}&receiver=%{R}"

#define FREE(x, f) do { if ((x)) (f)((x)); (x) = NULL; } while(0)
#define FREE_REQUEST(x) FREE((x), SPF_request_free)
#define FREE_RESPONSE(x) FREE((x), SPF_response_free)

#define CONTINUE_ERROR { \
	res = 255; \
	sprintf(pf_result, "450 temporary failure: please contact postmaster if the error remains"); \
	printf("action=%s\n\n", pf_result); \
	fflush(stdout); \
	if (opts->debug) \
          syslog(LOG_INFO, "action=%s (ip=%s from=%s helo=%s to=%s)\n", pf_result, req->ip, req->sender, req->helo, req->rcpt_to); \
	continue; \
}

#define CONTINUE_DUNNO(s) { \
	printf("action=PREPEND X-Received-SPF: %s\n", s); \
	printf("action=%s\n\n", POSTFIX_DUNNO); \
	fflush(stdout); res=255; \
	if (opts->debug) \
          syslog(LOG_INFO, "action=%s %s (ip=%s from=%s helo=%s to=%s)\n", POSTFIX_DUNNO, s, req->ip, req->sender, req->helo, req->rcpt_to); \
	continue; \
}

#define WARN_ERROR do { res = 255; } while(0)
#define FAIL_ERROR do { res = 255; goto error; } while(0)
#define EXIT_OK do { res = 0; goto error; } while(0)

#define RESIZE_RESULT(n) do { \
	if (result == NULL) { \
		result_len = 256 + n; \
		result = malloc(result_len); \
		result[0] = '\0'; \
	} \
	else if (strlen(result) + n >= result_len) { \
		result_len = result_len + (result_len >> 1) + 8 + n; \
		result = realloc(result, result_len); \
	} \
} while(0)
#define APPEND_RESULT(n) do { \
	partial_result = SPF_strresult(n); \
	RESIZE_RESULT(strlen(partial_result)); \
	strcat(result, partial_result); \
} while(0)

#define X_OR_EMPTY(x) ((x) ? (x) : "")

                        
static const char               *progname;

static struct option long_options[] = {
	{"file", 1, 0, 'f'},

	{"ip", 1, 0, 'i'},
	{"sender", 1, 0, 's'},
	{"helo", 1, 0, 'h'},
	{"rcpt-to", 1, 0, 'r'},

	{"debug", 2, 0, 'd'},
	{"local", 1, 0, 'l'},
	{"trusted", 1, 0, 't'},
	{"guess", 1, 0, 'g'},
	{"default-explanation", 1, 0, 'e'},
	{"max-lookup", 1, 0, 'm'},
	{"sanitize", 1, 0, 'c'},
	{"name", 1, 0, 'n'},
	{"override", 1, 0, 'a'},
	{"fallback", 1, 0, 'z'},

	{"keep-comments", 0, 0, 'k'},
	{"version", 0, 0, 'v'},
	{"help", 0, 0, '?'},

	{0, 0, 0, 0}
};

static void
unimplemented(const char flag)
{
	struct option	*opt;
	int				 i;

	for (i = 0; (opt = &long_options[i])->name; i++) {
		if (flag == opt->val) {
			fprintf(stderr, "Unimplemented option: -%s or -%c\n",
							opt->name, flag);
			return;
		}
	}

	fprintf(stderr, "Unimplemented option: -%c\n", flag);
}


static void
help()
{
	fprintf(
	stderr,
	"Usage:\n"
	"\n"
	"policyd-spf-fs [options] ...\n"
	"\n"
	"Valid options are:\n"
	"	--debug [debug level]	   debug level.\n"
	"	--local <SPF mechanisms>	Local policy for whitelisting.\n"
	"	--trusted <0|1>			 Should trusted-forwarder.org be checked?\n"
	"	--guess <SPF mechanisms>	Default checks if no SPF record is found.\n"
	"	--default-explanation <str> Default explanation string to use.\n"
	"	--max-lookup <number>	   Maximum number of DNS lookups to allow\n"
	"	--sanitize <0|1>			Clean up invalid characters in output?\n"
	"	--name <domain name>		The name of the system doing the SPF\n"
	"							   checking\n"
	"	--override <...>			Override SPF records for domains\n"
	"	--fallback <...>			Fallback SPF records for domains\n"
	"\n"
	"	--version				   Print version of spfquery.\n"
	"	--help					  Print out these options.\n"
	"\n"
	);
}


static void
response_print_errors(const char *context,
				SPF_response_t *spf_response, SPF_errcode_t err)
{
	SPF_error_t		*spf_error;;
	int				 i;

	syslog(LOG_CRIT,"StartError\n");

	if (context != NULL)
		syslog(LOG_CRIT,"Context: %s\n", context);
	if (err != SPF_E_SUCCESS)
		syslog(LOG_CRIT,"ErrorCode: (%d) %s\n", err, SPF_strerror(err));

	if (spf_response != NULL) {
		for (i = 0; i < SPF_response_messages(spf_response); i++) {
			spf_error = SPF_response_message(spf_response, i);
			syslog(LOG_CRIT,"%s: %s%s\n",
					SPF_error_errorp(spf_error) ? "Error" : "Warning",
					// SPF_error_code(spf_error),
					// SPF_strerror(SPF_error_code(spf_error)),
					((SPF_error_errorp(spf_error) && (!err))
							? "[UNRETURNED] "
							: ""),
					SPF_error_message(spf_error) );
		}
	}
	else {
		syslog(LOG_CRIT,"libspf2 gave a NULL spf_response\n");
	}
	syslog(LOG_CRIT,"EndError\n");
}

static void
response_print(const char *context, SPF_response_t *spf_response)
{
	syslog(LOG_DEBUG,"--vv--\n");
	syslog(LOG_DEBUG,"Context: %s\n", context);
	if (spf_response == NULL) {
		syslog(LOG_DEBUG, "NULL RESPONSE!\n");
	}
	else {
		syslog(LOG_DEBUG, "Response result: %s\n",
					SPF_strresult(SPF_response_result(spf_response)));
		syslog(LOG_DEBUG, "Response reason: %s\n",
					SPF_strreason(SPF_response_reason(spf_response)));
		syslog(LOG_DEBUG, "Response err: %s\n",
					SPF_strerror(SPF_response_errcode(spf_response)));
		response_print_errors(NULL, spf_response,
						SPF_response_errcode(spf_response));
	}
	syslog(LOG_DEBUG,"--^^--\n");
}

typedef
struct SPF_client_options_struct {
	// void		*hook;
	char		*localpolicy;
	const char	*explanation;
	const char	*fallback;
	const char	*rec_dom;
	int 		 use_trusted;
	int			 max_lookup;
	int			 sanitize;
	int			 debug;
} SPF_client_options_t;

typedef
struct SPF_client_request_struct {
	char		*ip;
	char		*sender;
	char		*helo;
	char		*rcpt_to;
} SPF_client_request_t;


static char read_request_from_pf(SPF_client_options_t *opts, SPF_client_request_t *req)
{
        char    line[BUFSIZ];
        char	args=0;

        while (fgets(line, BUFSIZ, stdin) != NULL) {
                line[strcspn(line, "\r\n")] = '\0'; 
                if (opts->debug > 1) syslog(LOG_DEBUG, "--> %s", line); /* DBG */
                switch (line[0]) {
                        case '\0':
                                if (args > 0) return(0);
                                break;
                        case 'c':
                                if (strncasecmp(line, "client_address=", 15) == 0) {
                                        req->ip = strdup(&line[15]);
                                        if (opts->debug > 1) syslog(LOG_DEBUG, "[ip %s]", req->ip); /* DBG */
                                        args++;
                                        continue;
                                }
                                break;
                        case 's':
                                if (strncasecmp(line, "sender=", 7) == 0) {
                                        req->sender = strdup(&line[7]);
                                        if (opts->debug > 1) syslog(LOG_DEBUG, "[sender %s]", req->sender); /* DBG */
                                        args++;
                                        continue;
                                }
                                break;
                        case 'h':
                                if (strncasecmp(line, "helo_name=", 10) == 0) {
                                        req->helo = strdup(&line[10]);
                                        if (opts->debug > 1) syslog(LOG_DEBUG, "[helo %s]", req->helo); /* DBG */
                                        args++;
                                        continue;
                                }
                                break;
			case 'r':
                                if (strncasecmp(line, "recipient=", 10) == 0) {
                                        req->rcpt_to = strdup(&line[10]);
                                        if (opts->debug > 1) syslog(LOG_DEBUG, "[recipient %s]", req->rcpt_to); /* DBG */
                                        args++;
                                        continue;
                                }
                                break;
                }
                /* Ignore line. */
        }
        if (feof(stdin)) {
          return(1);
        } else {
          return(0);
        }
}

static void pf_response(SPF_client_options_t    *opts, SPF_response_t *spf_response, SPF_client_request_t *req)
{
      char                     result[RESULTSIZE];
      char                     spf_comment[RESULTSIZE];

      switch (spf_response->result) {
                case SPF_RESULT_PASS:
                        strcpy(result, POSTFIX_DUNNO);
                        printf("action=PREPEND X-%s\n",SPF_response_get_received_spf(spf_response));
                        snprintf(spf_comment, RESULTSIZE, SPF_response_get_received_spf(spf_response));
                        break;
                case SPF_RESULT_FAIL:
                	strcpy(result, POSTFIX_REJECT);
                        snprintf(spf_comment, RESULTSIZE,"SPF Reject: %s",
                                                        (spf_response->smtp_comment
                                                                ? spf_response->smtp_comment
                                                                : (spf_response->header_comment
                                                                        ? spf_response->header_comment
                                                                        : "")));
                        break;
                case SPF_RESULT_TEMPERROR:
                case SPF_RESULT_PERMERROR:
                case SPF_RESULT_INVALID:
                        snprintf(result, RESULTSIZE,
                                                        "450 temporary failure: %s",
                                                        (spf_response->smtp_comment
                                                                ? spf_response->smtp_comment
                                                                : ""));
			spf_comment[0]='\0';
                        break;
                case SPF_RESULT_SOFTFAIL:
                case SPF_RESULT_NEUTRAL: 
                case SPF_RESULT_NONE:    
                default:
                        strcpy(result, POSTFIX_DUNNO);
                        printf("action=PREPEND X-%s\n",SPF_response_get_received_spf(spf_response));
                        snprintf(spf_comment, RESULTSIZE, SPF_response_get_received_spf(spf_response));
                        break;
        }
        
        result[RESULTSIZE - 1] = '\0';
	if (opts->debug > 1)
		syslog(LOG_DEBUG, "<-- action=%s\n", result);
	if (strcmp(result,POSTFIX_REJECT) == 0) {
          printf("action=%s %s\n\n", result, spf_comment);
        } else {
          printf("action=%s\n\n", result);
        }
        fflush(stdout);
        if (opts->debug)
          syslog(LOG_INFO, "action=%s %s (ip=%s from=%s helo=%s to=%s)\n", result, spf_comment, req->ip, req->sender, req->helo, req->rcpt_to);
}


int main( int argc, char *argv[] )
{
	SPF_client_options_t	*opts;
	SPF_client_request_t	*req = NULL;

	SPF_server_t	*spf_server = NULL;
	SPF_request_t	*spf_request = NULL;
	SPF_response_t	*spf_response = NULL;
	SPF_response_t	*spf_response_2mx = NULL;
	SPF_errcode_t	 err;

	int  			 opt_keep_comments = 0;

#ifdef TO_MX
	char			*p, *p_end;
#endif

	int 			 request_limit=0;
	int				 major, minor, patch;

	int				 res = 0;
	int				 c;

	const char		*partial_result;
	char			*result = NULL;
	int				 result_len = 0;
	char			hostname[255];
	char			pf_result[100];
	struct hostent		*fullhostname;

        /* Figure out our name */
        progname = strrchr(argv[0], '/');
        if (progname != NULL)                                                 
                ++progname;
        else
                progname = argv[0];

	/*open syslog*/
	openlog(progname, LOG_PID|LOG_CONS|LOG_NDELAY|LOG_NOWAIT, LOG_MAIL);

	/* Redefine libSPF2 output routines to go to syslog */
	SPF_error_handler = SPF_error_syslog;
	SPF_warning_handler = SPF_warning_syslog;
	SPF_info_handler = SPF_info_syslog;
	SPF_debug_handler = SPF_debug_syslog;
	
	opts = (SPF_client_options_t *)malloc(sizeof(SPF_client_options_t));
	memset(opts, 0, sizeof(SPF_client_options_t));

	/*
	 * check the arguments
	 */

	for (;;) {
		int option_index;	/* Largely unused */

		c = getopt_long_only (argc, argv, "f:i:s:h:r:lt::gemcnd::kz:a:v",
				  long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
			case 'l':
				opts->localpolicy = optarg;
				break;

			case 't':
				if (optarg == NULL)
					opts->use_trusted = 1;
				else
					opts->use_trusted = atoi(optarg);
				break;

			case 'g':
				opts->fallback = optarg;
				break;

			case 'e':
				opts->explanation = optarg;
				break;

			case 'm':
				opts->max_lookup = atoi(optarg);
				break;

			case 'c':		/* "clean"		*/
				opts->sanitize = atoi(optarg);
				break;

			case 'n':		/* name of host doing SPF checking */
				opts->rec_dom = optarg;
				break;

			case 'a':
				unimplemented('a');
				break;

			case 'z':
				unimplemented('z');
				break;


			case 'v':
				fprintf( stderr, "policyd-spf-fs version information:\n" );
				fprintf( stderr, "policyd-spf-fs version: $Rev: 27 $\n");
				fprintf( stderr, "SPF test system version: %s\n",
				 SPF_TEST_VERSION );
				fprintf( stderr, "Compiled with SPF library version: %d.%d.%d\n",
				 SPF_LIB_VERSION_MAJOR, SPF_LIB_VERSION_MINOR,
				 SPF_LIB_VERSION_PATCH );
				SPF_get_lib_version( &major, &minor, &patch );
				fprintf( stderr, "Running with SPF library version: %d.%d.%d\n",
				 major, minor, patch );
				fprintf( stderr, "\n" );
				FAIL_ERROR;
				break;

			case 0:
			case '?':
				help();
				FAIL_ERROR;
				break;

			case 'k':
				opt_keep_comments = 1;
				break;

			case 'd':
				if (optarg == NULL)
					opts->debug = 1;
				else
					opts->debug = atoi( optarg );
				break;

			default:
				fprintf( stderr, "Error: getopt returned character code 0%o ??\n", c);
				FAIL_ERROR;
		}
	}

	if (optind != argc) {
		help();
		FAIL_ERROR;
	}

	if (!opts->rec_dom) {
  	  gethostname(hostname, 255);
	  fullhostname = gethostbyname(hostname);
	
	  if (opts->debug > 1)
	    syslog(LOG_DEBUG, "Hostname: %s\n",fullhostname->h_name);

	  opts->rec_dom = fullhostname->h_name;
	}

	/*
	 * set up the SPF configuration
	 */

	spf_server = SPF_server_new(SPF_DNS_CACHE, opts->debug > 2 ? opts->debug-2 : 0);

	if ( opts->rec_dom )
		SPF_server_set_rec_dom( spf_server, opts->rec_dom );
	if ( opts->sanitize )
		SPF_server_set_sanitize( spf_server, opts->sanitize );
	if ( opts->max_lookup )
		SPF_server_set_max_dns_mech(spf_server, opts->max_lookup);

	if (opts->localpolicy) {
		err = SPF_server_set_localpolicy( spf_server, opts->localpolicy, opts->use_trusted, &spf_response);
		if ( err ) {
			response_print_errors("Error setting local policy",
							spf_response, err);
			WARN_ERROR;
		}
		FREE_RESPONSE(spf_response);
	}


	if ( !opts->explanation ) {
	  opts->explanation = DEFAULT_EXPLANATION;
	}
	
	err = SPF_server_set_explanation( spf_server, opts->explanation, &spf_response );
	if ( err ) {
	  response_print_errors("Error setting default explanation",
	         spf_response, err);
	  WARN_ERROR;
	}
	FREE_RESPONSE(spf_response);

	/*
	 * process the SPF request
	 */

	request_limit=0;

	while ( request_limit < REQUEST_LIMIT ) {
		request_limit++;	                                
	        if (request_limit == 0) {
  		  free(req);
  		}
		req = (SPF_client_request_t *)malloc(sizeof(SPF_client_request_t));
		memset(req, 0, sizeof(SPF_client_request_t));
		
		if (read_request_from_pf(opts, req)) {
		  syslog(LOG_WARNING, "IO Closed while reading, exiting");
		  EXIT_OK;
		}
		
		if (opts->debug > 1)
			syslog(LOG_DEBUG, "Reincarnation %d\n", request_limit);


		/* We have to do this here else we leak on CONTINUE_ERROR */
		FREE_REQUEST(spf_request);
		FREE_RESPONSE(spf_response);

		spf_request = SPF_request_new(spf_server);

		if (SPF_request_set_ipv4_str(spf_request, req->ip) && SPF_request_set_ipv6_str(spf_request, req->ip)) {
			syslog(LOG_WARNING, "Invalid IP address.\n" );
			CONTINUE_ERROR;
		}

	  if (req->helo) {
		if (SPF_request_set_helo_dom( spf_request, req->helo ) ) {
			syslog(LOG_WARNING, "Invalid HELO domain.\n" );
			CONTINUE_ERROR;
		}
	  }

	  	if (strchr(req->sender, '@') > 0) {
  		  if (SPF_request_set_env_from( spf_request, req->sender ) ) {
			syslog(LOG_WARNING, "Invalid envelope from address.\n" );
			CONTINUE_ERROR;
		  }
		} else { /* This is something we can not check*/ 
                  CONTINUE_DUNNO("no valid email address found");
                }

		err = SPF_request_query_mailfrom(spf_request, &spf_response);
		if (opts->debug > 1) 
			response_print("Main query", spf_response);
		if (err) {
		  if (opts->debug > 1)
			response_print_errors("Failed to query MAIL-FROM",
							spf_response, err);

			CONTINUE_DUNNO("no SPF record found");
		}

		if (result != NULL)
			result[0] = '\0';
		APPEND_RESULT(SPF_response_result(spf_response));
		
#ifdef TO_MX /* This code returns usualy neutral and oferwrites a fail from the above spf code
                which is not what we like. So we disable it for the time deing ... */
                
		if (req->rcpt_to != NULL  && *req->rcpt_to != '\0' ) {
			p = req->rcpt_to;
			p_end = p + strcspn(p, ",;");

			/* This is some incarnation of 2mx mode. */
			while (SPF_response_result(spf_response)!=SPF_RESULT_PASS) {
				if (*p_end)
					*p_end = '\0';
				else
					p_end = NULL;	/* Note this is last rcpt */

				err = SPF_request_query_rcptto(spf_request,
								&spf_response_2mx, p);
				if (opts->debug > 1)
					response_print("2mx query", spf_response_2mx);
				if (err) {
					response_print_errors("Failed to query RCPT-TO",
									spf_response, err);
					CONTINUE_ERROR;
				}

				/* append the result */
				APPEND_RESULT(SPF_response_result(spf_response_2mx));

				spf_response = SPF_response_combine(spf_response,
								spf_response_2mx);

				if (!p_end)
					break;
				p = p_end + 1;
			}
		}
#endif /* TO_MX */
		/* We now have an option to call SPF_request_query_fallback */
		if (opts->fallback) {
			err = SPF_request_query_fallback(spf_request,
							&spf_response, opts->fallback);
			if (opts->debug > 1)
				response_print("fallback query", spf_response_2mx);
			if (err) {
				response_print_errors("Failed to query best-guess",
								spf_response, err);
				CONTINUE_ERROR;
			}

			/* append the result */
			APPEND_RESULT(SPF_response_result(spf_response_2mx));

			spf_response = SPF_response_combine(spf_response,
							spf_response_2mx);
		}

/*		printf( "R: %s\nSC: %s\nHC: %s\nRS: %s\n",
			result,
			X_OR_EMPTY(SPF_response_get_smtp_comment(spf_response)),
			X_OR_EMPTY(SPF_response_get_header_comment(spf_response)),
			X_OR_EMPTY(SPF_response_get_received_spf(spf_response))
			);
*/			
		pf_response(opts, spf_response, req);
			
		res = SPF_response_result(spf_response);

		fflush(stdout);
	}

  error:
	FREE(result, free);
	FREE_RESPONSE(spf_response);
	FREE_REQUEST(spf_request);
	FREE(spf_server, SPF_server_free);

	syslog(LOG_INFO, "Terminating with result %d, Reincarnation: %d\n", res, request_limit);
	return res;
}
