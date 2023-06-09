/*
 * uhttpd - Tiny single-threaded httpd - Main component
 *
 *   Copyright (C) 2010 Jo-Philipp Wich <xm@subsignal.org>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define _XOPEN_SOURCE 500	/* crypt() */

#include "uhttpd.h"
#include "uhttpd-utils.h"
#include "uhttpd-file.h"

#ifdef HAVE_CGI
#include "uhttpd-cgi.h"
#endif

#ifdef HAVE_LUA
#include "uhttpd-lua.h"
#endif

#ifdef HAVE_TLS
#include "uhttpd-tls.h"
#endif


static int run = 1;

static void uh_sigterm(int sig)
{
	run = 0;
}

static void uh_sigchld(int sig)
{
	while( waitpid(-1, NULL, WNOHANG) > 0 ) { }
}

static void uh_config_parse(const char *path)
{
	FILE *c;
	char line[512];
	char *user = NULL;
	char *pass = NULL;
	char *eol  = NULL;

	if( (c = fopen(path ? path : "/etc/httpd.conf", "r")) != NULL )
	{
		memset(line, 0, sizeof(line));

		while( fgets(line, sizeof(line) - 1, c) )
		{
			if( (line[0] == '/') && (strchr(line, ':') != NULL) )
			{
				if( !(user = strchr(line, ':')) || (*user++ = 0) ||
				    !(pass = strchr(user, ':')) || (*pass++ = 0) ||
					!(eol = strchr(pass, '\n')) || (*eol++  = 0) )
						continue;

				if( !uh_auth_add(line, user, pass) )
				{
					fprintf(stderr,
						"Can not manage more than %i basic auth realms, "
						"will skip the rest\n", UH_LIMIT_AUTHREALMS
					);

					break;
				} 
			}
		}

		fclose(c);
	}
}

static int uh_socket_bind(
	fd_set *serv_fds, int *max_fd, const char *host, const char *port,
	struct addrinfo *hints, int do_tls, struct config *conf
) {
	int sock = -1;
	int yes = 1;
	int status;
	int bound = 0;

	struct listener *l = NULL;
	struct addrinfo *addrs = NULL, *p = NULL;

	if( (status = getaddrinfo(host, port, hints, &addrs)) != 0 )
	{
		fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(status));
	}

	/* try to bind a new socket to each found address */
	for( p = addrs; p; p = p->ai_next )
	{
		/* get the socket */
		if( (sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1 )
		{
			perror("socket()");
			goto error;
		}

		/* "address already in use" */
		if( setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1 )
		{
			perror("setsockopt()");
			goto error;
		}

		/* required to get parallel v4 + v6 working */
		if( p->ai_family == AF_INET6 )
		{
			if( setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes)) == -1 )
			{
				perror("setsockopt()");
				goto error;
			}
		}

		/* bind */
		if( bind(sock, p->ai_addr, p->ai_addrlen) == -1 )
		{
			perror("bind()");
			goto error;
		}

		/* listen */
		if( listen(sock, UH_LIMIT_CLIENTS) == -1 )
		{
			perror("listen()");
			goto error;
		}

		/* add listener to global list */
		if( ! (l = uh_listener_add(sock, conf)) )
		{
			fprintf(stderr,
				"uh_listener_add(): Can not create more than "
				"%i listen sockets\n", UH_LIMIT_LISTENERS
			);

			goto error;
		}

#ifdef HAVE_TLS
		/* init TLS */
		l->tls = do_tls ? conf->tls : NULL;
#endif

		/* add socket to server fd set */
		FD_SET(sock, serv_fds);
		fd_cloexec(sock);
		*max_fd = max(*max_fd, sock);

		bound++;
		continue;

		error:
		if( sock > 0 )
			close(sock);
	}

	freeaddrinfo(addrs);

	return bound;
}

static struct http_request * uh_http_header_parse(struct client *cl, char *buffer, int buflen)
{
	char *method  = &buffer[0];
	char *path    = NULL;
	char *version = NULL;

	char *headers = NULL;
	char *hdrname = NULL;
	char *hdrdata = NULL;

	int i;
	int hdrcount = 0;

	static struct http_request req;

	memset(&req, 0, sizeof(req));


	/* terminate initial header line */
	if( (headers = strfind(buffer, buflen, "\r\n", 2)) != NULL )
	{
		buffer[buflen-1] = 0;

		*headers++ = 0;
		*headers++ = 0;

		/* find request path */
		if( (path = strchr(buffer, ' ')) != NULL )
			*path++ = 0;

		/* find http version */
		if( (path != NULL) && ((version = strchr(path, ' ')) != NULL) )
			*version++ = 0;


		/* check method */
		if( strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST") )
		{
			/* invalid method */
			uh_http_response(cl, 405, "Method Not Allowed");
			return NULL;
		}
		else
		{
			switch(method[0])
			{
				case 'G':
					req.method = UH_HTTP_MSG_GET;
					break;

				case 'H':
					req.method = UH_HTTP_MSG_HEAD;
					break;

				case 'P':
					req.method = UH_HTTP_MSG_POST;
					break;
			}
		}

		/* check path */
		if( !path || !strlen(path) )
		{
			/* malformed request */
			uh_http_response(cl, 400, "Bad Request");
			return NULL;
		}
		else
		{
			req.url = path;
		}

		/* check version */
		if( strcmp(version, "HTTP/0.9") && strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1") )
		{
			/* unsupported version */
			uh_http_response(cl, 400, "Bad Request");
			return NULL;
		}
		else
		{
			req.version = strtof(&version[5], NULL);
		}


		/* process header fields */
		for( i = (int)(headers - buffer); i < buflen; i++ )
		{
			/* found eol and have name + value, push out header tuple */
			if( hdrname && hdrdata && (buffer[i] == '\r' || buffer[i] == '\n') )
			{
				buffer[i] = 0;

				/* store */
				if( (hdrcount + 1) < array_size(req.headers) )
				{
					req.headers[hdrcount++] = hdrname;
					req.headers[hdrcount++] = hdrdata;

					hdrname = hdrdata = NULL;
				}

				/* too large */
				else
				{
					uh_http_response(cl, 413, "Request Entity Too Large");
					return NULL;
				}
			}

			/* have name but no value and found a colon, start of value */
			else if( hdrname && !hdrdata && ((i+2) < buflen) &&
				(buffer[i] == ':') && (buffer[i+1] == ' ')
			) {
				buffer[i] = 0;
				hdrdata = &buffer[i+2];
			}

			/* have no name and found [A-Z], start of name */
			else if( !hdrname && isalpha(buffer[i]) && isupper(buffer[i]) )
			{
				hdrname = &buffer[i];
			}
		}

		/* valid enough */
		return &req;
	}

	/* Malformed request */
	uh_http_response(cl, 400, "Bad Request");
	return NULL;
}


static struct http_request * uh_http_header_recv(struct client *cl)
{
	static char buffer[UH_LIMIT_MSGHEAD];
	char *bufptr = &buffer[0];
	char *idxptr = NULL;

	struct timeval timeout;

	fd_set reader;

	ssize_t blen = sizeof(buffer)-1;
	ssize_t rlen = 0;


	memset(buffer, 0, sizeof(buffer));

	while( blen > 0 )
	{
		FD_ZERO(&reader);
		FD_SET(cl->socket, &reader);

		/* fail after 0.1s */
		timeout.tv_sec  = 0;
		timeout.tv_usec = 100000;

		/* check whether fd is readable */
		if( select(cl->socket + 1, &reader, NULL, NULL, &timeout) > 0 )
		{
			/* receive data */
			rlen = uh_tcp_peek(cl, bufptr, blen);

			if( rlen > 0 )
			{
				if( (idxptr = strfind(buffer, sizeof(buffer), "\r\n\r\n", 4)) )
				{
					blen -= uh_tcp_recv(cl, bufptr, (int)(idxptr - bufptr) + 4);

					/* header read complete ... */
					return uh_http_header_parse(cl, buffer, sizeof(buffer) - blen - 1);
				}
				else
				{
					rlen = uh_tcp_recv(cl, bufptr, rlen);
					blen -= rlen;
					bufptr += rlen;
				}
			}
			else
			{
				/* invalid request (unexpected eof/timeout) */
				uh_http_response(cl, 408, "Request Timeout");
				return NULL;
			}
		}
		else
		{
			/* invalid request (unexpected eof/timeout) */
			uh_http_response(cl, 408, "Request Timeout");
			return NULL;
		}
	}

	/* request entity too large */
	uh_http_response(cl, 413, "Request Entity Too Large");
	return NULL;
}

static int uh_path_match(const char *prefix, const char *url)
{
	if( (strstr(url, prefix) == url) &&
	    ((prefix[strlen(prefix)-1] == '/') ||
		 (strlen(url) == strlen(prefix))   ||
		 (url[strlen(prefix)] == '/'))
	) {
		return 1;
	}

	return 0;
}


int main (int argc, char **argv)
{
#ifdef HAVE_LUA
	/* Lua runtime */
	lua_State *L = NULL;
#endif

	/* master file descriptor list */
	fd_set used_fds, serv_fds, read_fds;

	/* working structs */
	struct addrinfo hints;
	struct http_request *req;
	struct path_info *pin;
	struct client *cl;
	struct sigaction sa;
	struct config conf;

	/* signal mask */
	sigset_t ss;

	/* maximum file descriptor number */
	int new_fd, cur_fd, max_fd = 0;

	int tls = 0;
	int keys = 0;
	int bound = 0;
	int nofork = 0;

	/* args */
	int opt;
	char bind[128];
	char *port = NULL;

	/* library handles */
	void *tls_lib;
	void *lua_lib;

	/* clear the master and temp sets */
	FD_ZERO(&used_fds);
	FD_ZERO(&serv_fds);
	FD_ZERO(&read_fds);

	/* handle SIGPIPE, SIGINT, SIGTERM, SIGCHLD */
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	sa.sa_handler = uh_sigchld;
	sigaction(SIGCHLD, &sa, NULL);

	sa.sa_handler = uh_sigterm;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* defer SIGCHLD */
	sigemptyset(&ss);
	sigaddset(&ss, SIGCHLD);
	sigprocmask(SIG_BLOCK, &ss, NULL);

	/* prepare addrinfo hints */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags    = AI_PASSIVE;

	/* parse args */
	memset(&conf, 0, sizeof(conf));
	memset(bind, 0, sizeof(bind));

#ifdef HAVE_TLS
	/* load TLS plugin */
	if( ! (tls_lib = dlopen("uhttpd_tls.so", RTLD_LAZY | RTLD_GLOBAL)) )
	{
		fprintf(stderr,
			"Notice: Unable to load TLS plugin - disabling SSL support! "
			"(Reason: %s)\n", dlerror()
		);
	}
	else
	{
		/* resolve functions */
		if( !(conf.tls_init   = dlsym(tls_lib, "uh_tls_ctx_init"))      ||
		    !(conf.tls_cert   = dlsym(tls_lib, "uh_tls_ctx_cert"))      ||
		    !(conf.tls_key    = dlsym(tls_lib, "uh_tls_ctx_key"))       ||
		    !(conf.tls_free   = dlsym(tls_lib, "uh_tls_ctx_free"))      ||
			!(conf.tls_accept = dlsym(tls_lib, "uh_tls_client_accept")) ||
			!(conf.tls_close  = dlsym(tls_lib, "uh_tls_client_close"))  ||
			!(conf.tls_recv   = dlsym(tls_lib, "uh_tls_client_recv"))   ||
			!(conf.tls_send   = dlsym(tls_lib, "uh_tls_client_send"))
		) {
			fprintf(stderr,
				"Error: Failed to lookup required symbols "
				"in TLS plugin: %s\n", dlerror()
			);
			exit(1);
		}

		/* init SSL context */
		if( ! (conf.tls = conf.tls_init()) )
		{
			fprintf(stderr, "Error: Failed to initalize SSL context\n");
			exit(1);
		}
	}
#endif

	while( (opt = getopt(argc, argv, "fC:K:p:s:h:c:l:L:d:r:m:x:t:")) > 0 )
	{
		switch(opt)
		{
			/* [addr:]port */
			case 'p':
			case 's':
				if( (port = strrchr(optarg, ':')) != NULL )
				{
					if( (optarg[0] == '[') && (port > optarg) && (port[-1] == ']') )
						memcpy(bind, optarg + 1,
							min(sizeof(bind), (int)(port - optarg) - 2));
					else
						memcpy(bind, optarg,
							min(sizeof(bind), (int)(port - optarg)));

					port++;
				}
				else
				{
					port = optarg;
				}

				if( opt == 's' )
				{
					if( !conf.tls )
					{
						fprintf(stderr,
							"Notice: TLS support is disabled, "
							"ignoring '-s %s'\n", optarg
						);
						continue;
					}

					tls = 1;
				}

				/* bind sockets */
				bound += uh_socket_bind(
					&serv_fds, &max_fd, bind[0] ? bind : NULL, port,
					&hints,	(opt == 's'), &conf
				);

				break;

#ifdef HAVE_TLS
			/* certificate */
			case 'C':
				if( conf.tls )
				{
					if( conf.tls_cert(conf.tls, optarg) < 1 )
					{
						fprintf(stderr,
							"Error: Invalid certificate file given\n");
						exit(1);
					}

					keys++;
				}

				break;

			/* key */
			case 'K':
				if( conf.tls )
				{
					if( conf.tls_key(conf.tls, optarg) < 1 )
					{
						fprintf(stderr,
							"Error: Invalid private key file given\n");
						exit(1);
					}

					keys++;
				}

				break;
#endif

			/* docroot */
			case 'h':
				if( ! realpath(optarg, conf.docroot) )
				{
					fprintf(stderr, "Error: Invalid directory %s: %s\n",
						optarg, strerror(errno));
					exit(1);
				}
				break;

#ifdef HAVE_CGI
			/* cgi prefix */
			case 'x':
				conf.cgi_prefix = optarg;
				break;
#endif

#ifdef HAVE_LUA
			/* lua prefix */
			case 'l':
				conf.lua_prefix = optarg;
				break;

			/* lua handler */
			case 'L':
				conf.lua_handler = optarg;
				break;
#endif

#if defined(HAVE_CGI) || defined(HAVE_LUA)
			/* script timeout */
			case 't':
				conf.script_timeout = atoi(optarg);
				break;
#endif

			/* no fork */
			case 'f':
				nofork = 1;
				break;

			/* urldecode */
			case 'd':
				if( (port = malloc(strlen(optarg)+1)) != NULL )
				{
					memset(port, 0, strlen(optarg)+1);
					uh_urldecode(port, strlen(optarg), optarg, strlen(optarg));
					printf("%s", port);
					free(port);
					exit(0);
				}
				break;

			/* basic auth realm */
			case 'r':
				conf.realm = optarg;
				break;

			/* md5 crypt */
			case 'm':
				printf("%s\n", crypt(optarg, "$1$"));
				exit(0);
				break;

			/* config file */
			case 'c':
				conf.file = optarg;
				break;

			default:
				fprintf(stderr,
					"Usage: %s -p [addr:]port [-h docroot]\n"
					"	-f              Do not fork to background\n"
					"	-c file         Configuration file, default is '/etc/httpd.conf'\n"
					"	-p [addr:]port  Bind to specified address and port, multiple allowed\n"
#ifdef HAVE_TLS
					"	-s [addr:]port  Like -p but provide HTTPS on this port\n"
					"	-C file         ASN.1 server certificate file\n"
					"	-K file         ASN.1 server private key file\n"
#endif
					"	-h directory    Specify the document root, default is '.'\n"
#ifdef HAVE_LUA
					"	-l string       URL prefix for Lua handler, default is '/lua'\n"
					"	-L file         Lua handler script, omit to disable Lua\n"
#endif
#ifdef HAVE_CGI
					"	-x string       URL prefix for CGI handler, default is '/cgi-bin'\n"
#endif
#if defined(HAVE_CGI) || defined(HAVE_LUA)
					"	-t seconds      CGI and Lua script timeout in seconds, default is 60\n"
#endif
					"	-d string       URL decode given string\n"
					"	-r string       Specify basic auth realm\n"
					"	-m string       MD5 crypt given string\n"
					"\n", argv[0]
				);

				exit(1);
		}
	}

#ifdef HAVE_TLS
	if( (tls == 1) && (keys < 2) )
	{
		fprintf(stderr, "Error: Missing private key or certificate file\n");
		exit(1);
	}
#endif

	if( bound < 1 )
	{
		fprintf(stderr, "Error: No sockets bound, unable to continue\n");
		exit(1);
	}

	/* default docroot */
	if( !conf.docroot[0] && !realpath(".", conf.docroot) )
	{
		fprintf(stderr, "Error: Can not determine default document root: %s\n",
			strerror(errno));
		exit(1);
	}

	/* default realm */
	if( ! conf.realm )
		conf.realm = "Protected Area";

	/* config file */
	uh_config_parse(conf.file);

#if defined(HAVE_CGI) || defined(HAVE_LUA)
	/* default script timeout */
	if( conf.script_timeout <= 0 )
		conf.script_timeout = 60;
#endif

#ifdef HAVE_CGI
	/* default cgi prefix */
	if( ! conf.cgi_prefix )
		conf.cgi_prefix = "/cgi-bin";
#endif

#ifdef HAVE_LUA
	/* load Lua plugin */
	if( ! (lua_lib = dlopen("uhttpd_lua.so", RTLD_LAZY | RTLD_GLOBAL)) )
	{
		fprintf(stderr,
			"Notice: Unable to load Lua plugin - disabling Lua support! "
			"(Reason: %s)\n", dlerror()
		);
	}
	else
	{
		/* resolve functions */
		if( !(conf.lua_init    = dlsym(lua_lib, "uh_lua_init"))    ||
		    !(conf.lua_close   = dlsym(lua_lib, "uh_lua_close"))   ||
		    !(conf.lua_request = dlsym(lua_lib, "uh_lua_request"))
		) {
			fprintf(stderr,
				"Error: Failed to lookup required symbols "
				"in Lua plugin: %s\n", dlerror()
			);
			exit(1);
		}

		/* init Lua runtime if handler is specified */
		if( conf.lua_handler )
		{
			/* default lua prefix */
			if( ! conf.lua_prefix )
				conf.lua_prefix = "/lua";

			L = conf.lua_init(conf.lua_handler);
		}
	}
#endif

	/* fork (if not disabled) */
	if( ! nofork )
	{
		switch( fork() )
		{
			case -1:
				perror("fork()");
				exit(1);

			case 0:
				/* daemon setup */
				if( chdir("/") )
					perror("chdir()");

				if( (cur_fd = open("/dev/null", O_WRONLY)) > -1 )
					dup2(cur_fd, 0);

				if( (cur_fd = open("/dev/null", O_RDONLY)) > -1 )
					dup2(cur_fd, 1);

				if( (cur_fd = open("/dev/null", O_RDONLY)) > -1 )
					dup2(cur_fd, 2);

				break;

			default:
				exit(0);
		}
	}

	/* backup server descriptor set */
	used_fds = serv_fds;

	/* loop */
	while(run)
	{
		/* create a working copy of the used fd set */
		read_fds = used_fds;

		/* sleep until socket activity */
		if( select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1 )
		{
			perror("select()");
			exit(1);
		}

		/* run through the existing connections looking for data to be read */
		for( cur_fd = 0; cur_fd <= max_fd; cur_fd++ )
		{
			/* is a socket managed by us */
			if( FD_ISSET(cur_fd, &read_fds) )
			{
				/* is one of our listen sockets */
				if( FD_ISSET(cur_fd, &serv_fds) )
				{
					/* handle new connections */
					if( (new_fd = accept(cur_fd, NULL, 0)) != -1 )
					{
						/* add to global client list */
						if( (cl = uh_client_add(new_fd, uh_listener_lookup(cur_fd))) != NULL )
						{
#ifdef HAVE_TLS
							/* setup client tls context */
							if( conf.tls )
								conf.tls_accept(cl);
#endif

							/* add client socket to global fdset */
							FD_SET(new_fd, &used_fds);
							fd_cloexec(new_fd);
							max_fd = max(max_fd, new_fd);
						}

						/* insufficient resources */
						else
						{
							fprintf(stderr,
								"uh_client_add(): Can not manage more than "
								"%i client sockets, connection dropped\n",
								UH_LIMIT_CLIENTS
							);

							close(new_fd);
						}
					}
				}

				/* is a client socket */
				else
				{
					if( ! (cl = uh_client_lookup(cur_fd)) )
					{
						/* this should not happen! */
						fprintf(stderr,
							"uh_client_lookup(): No entry for fd %i!\n",
							cur_fd);

						goto cleanup;
					}

					/* parse message header */
					if( (req = uh_http_header_recv(cl)) != NULL )
					{
#ifdef HAVE_LUA
						/* Lua request? */
						if( L && uh_path_match(conf.lua_prefix, req->url) )
						{
							conf.lua_request(cl, req, L);
						}
						else
#endif
						/* dispatch request */
						if( (pin = uh_path_lookup(cl, req->url)) != NULL )
						{
							/* auth ok? */
							if( uh_auth_check(cl, req, pin) )
							{
#ifdef HAVE_CGI
								if( uh_path_match(conf.cgi_prefix, pin->name) )
								{
									uh_cgi_request(cl, req, pin);
								}
								else
#endif
								{
									uh_file_request(cl, req, pin);
								}
							}
						}

						/* 404 */
						else
						{
							uh_http_sendhf(cl, 404, "Not Found",
								"No such file or directory");
						}
					}

					/* 400 */
					else
					{
						uh_http_sendhf(cl, 400, "Bad Request",
							"Malformed request received");
					}

#ifdef HAVE_TLS
					/* free client tls context */
					if( conf.tls )
						conf.tls_close(cl);
#endif

					cleanup:

					/* close client socket */
					close(cur_fd);
					FD_CLR(cur_fd, &used_fds);

					/* remove from global client list */
					uh_client_remove(cur_fd);
				}
			}
		}
	}

#ifdef HAVE_LUA
	/* destroy the Lua state */
	if( L != NULL )
		conf.lua_close(L);
#endif

	return 0;
}

