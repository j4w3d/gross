/*
 * Copyright (c) 2006 Eino Tuominen <eino@utu.fi>
 *                    Antti Siira <antti@utu.fi>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "common.h"
#include "srvutils.h"
#include "syncmgr.h"
#include "dnsblc.h"
#include "msgqueue.h"
#include "worker.h"

/* function must be implemented in worker_[proto].c */
int handle_connection(client_info_t *arg);

/*
 * destructor for client_info_t
 */
void
free_client_info(client_info_t *arg)
{
        free(arg->caddr);
	free(arg->ipstr);
#ifdef WORKER_PROTO_UDP
	free(arg->message);
#endif
        free(arg);
}

char *
ipstr(struct sockaddr_in *saddr)
{	
	char ipstr[INET_ADDRSTRLEN];

	if (inet_ntop(AF_INET, &saddr->sin_addr,
		ipstr, INET_ADDRSTRLEN) == NULL) {
		strncpy(ipstr, "UNKNOWN\0", INET_ADDRSTRLEN);
	}
	return strdup(ipstr);
}

/*
 * worker	- wrapper for process_connection()
 */
static void *
worker(void *arg)
{
        int ret;
	client_info_t *client_info;

	logstr(GLOG_DEBUG, "worker starting");

	client_info = (client_info_t *)arg;

        /* serve while good */
	handle_connection(client_info);

        /* tidy up */
        TIDY_UP:
#ifndef WORKER_PROTO_UDP
        close(client_info->connfd);
#endif
        free_client_info(client_info);
        sem_post(ctx->workercount_sem);
        logstr(GLOG_DEBUG, "worker returning");
        return 0;
}

#ifdef WORKER_PROTO_UDP
/*
 * The main worker thread for udp protocol. Listens for requests
 * and starts a new thread to handle each.
 */
static void *
udp_server(void *arg)
{
	int grossfd, ret, msglen;
	socklen_t clen;
	client_info_t *client_info;
	struct sockaddr_in caddr;
	char mesg[MAXLINELEN];

	grossfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (grossfd < 0) {
		/* ERROR */
		perror("socket");
		return NULL;
	}

	ret = bind(grossfd, (struct sockaddr *)&(ctx->config.gross_host),
			sizeof(struct sockaddr_in));
	if (ret < 0) {
		daemon_perror("bind");
	}

	/* server loop */
	for ( ; ; ) {
		/* client_info struct is free()d by the worker thread */
		client_info = Malloc(sizeof(client_info_t));
		client_info->caddr = Malloc(sizeof(struct sockaddr_in));

		clen = sizeof(struct sockaddr_in);
		msglen = recvfrom(grossfd, mesg, MAXLINELEN, 0,
					(struct sockaddr *)client_info->caddr, &clen);

		if (msglen < 0) {
			if (errno == EINTR)
				continue;
			perror("recvfrom");
			free_client_info(client_info);
			return NULL;
		} else {
			client_info->message = Malloc(msglen);
			client_info->connfd = grossfd;
			client_info->msglen = msglen;
			client_info->ipstr = ipstr(client_info->caddr);

			memcpy(client_info->message, mesg, msglen);
			Pthread_create(NULL, &worker, (void *)client_info);
		}
	}
	/* never reached */
}

#else

/*
 * The main worker thread for tcp_protocol. Listens for connections
 * and starts a new thread to handle each connection.
 */
static void *
tcp_server(void *arg)
{
        int ret;
        int grossfd;
        int opt;
        client_info_t *client_info;
        socklen_t clen;

        /* create socket for incoming requests */
        grossfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (grossfd < 0) {
                /* ERROR */
                perror("socket");
                return NULL;
        }
        opt = 1;
        ret = setsockopt(grossfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (ret < 0) {
                perror("setsockopt (SO_REUSEADDR)");
                return NULL;
        }

        ret = bind(grossfd, (struct sockaddr *)&(ctx->config.gross_host), sizeof(struct sockaddr_in));
        if (ret < 0) {
                daemon_perror("bind");
        }

        ret = listen(grossfd, MAXCONNQ);
        if (ret < 0) {
                perror("listen");
                return NULL;
        }

        /* server loop */
        for ( ; ; ) {
                /* client_info struct is free()d by the worker thread */
                client_info = Malloc(sizeof(client_info_t));
                client_info->caddr = Malloc(sizeof(struct sockaddr_in));

                clen = sizeof(struct sockaddr_in);

                client_info->connfd = accept(grossfd, (struct sockaddr *)client_info->caddr, &clen);
                if (client_info->connfd < 0) {
                        if (errno != EINTR) {
                                perror("accept()");
                                return NULL;
                        }
                } else {
                        ret = sem_trywait(ctx->workercount_sem);
                        if (ret < 0) {
                                /* error */
                                logstr(GLOG_ERROR, "thread count limit reached");
                                close(client_info->connfd);
                                free(client_info);
                        } else {
                                /* a client is connected, handle the
                                 * connection over to a worker thread
                                 */
				client_info->ipstr = ipstr(client_info->caddr);
                                Pthread_create(NULL, &worker, (void *)client_info);
                        }
                }
        }
}

#endif /* WORKER_PROTO_UDP */

/* 
 * destructor for gray_tuple_t
 */
void
free_request(gray_tuple_t *arg)
{
	free(arg->sender);
	free(arg->recipient);
	free(arg->client_address);
	free(arg);
}

int
test_tuple(gray_tuple_t *request, tmout_action_t *ta) {
	char tuple[MSGSZ];
	sha_256_t digest;
	update_message_t update;
	int ret;
	int retvalue = 0;
	oper_sync_t os;

	/* graylist */
	snprintf(tuple, MSGSZ, "%s %s %s",
			request->client_address,
			request->sender,
			request->recipient);
	digest = sha256_string(tuple);

	/* check status */
	if ( is_in_ring_queue(ctx->filter, digest) ) {
		logstr(GLOG_INFO, "match: %s", tuple);
		acctstr(ACCT_MATCH, "%s", tuple);
		retvalue = STATUS_MATCH;
	} else {
#ifndef DNSBL
		logstr(GLOG_INFO, "graylist: %s", tuple);
		acctstr(ACCT_GRAY, "%s", tuple);
		retvalue = STATUS_GRAY;
#else
		if (dnsblc(request->client_address, ta)) {
			logstr(GLOG_INFO, "graylist: %s", tuple);
			acctstr(ACCT_GRAY, "%s", tuple);
			retvalue = STATUS_GRAY;
		} else {
			logstr(GLOG_INFO, "trust: %s", tuple);
			acctstr(ACCT_TRUST, "%s", tuple);
			retvalue = STATUS_TRUST;
		}
#endif /* DNSBL */
	}

	if (((retvalue == STATUS_GRAY) || (retvalue == STATUS_MATCH)) 
		|| (ctx->config.flags & FLG_UPDATE_ALWAYS)) {
		/* update the filter */
		update.mtype = UPDATE;
		memcpy(update.mtext, &digest, sizeof(sha_256_t));
		ret = put_msg(ctx->update_q, &update, sizeof(sha_256_t), 0);
		if (ret < 0) {
			perror("update put_msg");
		}	

		/* update peer */
		if ( connected( &(ctx->config.peer) ) ) {
			os.digest = digest;
			// logstr(GLOG_DEBUG, "Sending oper sync");
			send_oper_sync( &(ctx->config.peer) , &os);
		}
	}

	return retvalue;
}


void
worker_init()
{
	int ret;

#ifdef WORKER_PROTO_TCP
	logstr(GLOG_DEBUG, "starting tcp server");
        Pthread_create(&ctx->process_parts.worker, &tcp_server, NULL);
#else
	logstr(GLOG_DEBUG, "starting udp server");
        Pthread_create(&ctx->process_parts.worker, &udp_server, NULL);
#endif /* WORKER_PROTO_TCP */

}
