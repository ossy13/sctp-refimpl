#include <incast_fmt.h>
#include <net/if_dl.h>
#include <arpa/inet.h>

#ifndef STRING_BUF_SZ
#define STRING_BUF_SZ 1024
#endif

void
print_an_address(struct sockaddr *a)
{
	char stringToPrint[STRING_BUF_SZ];
	u_short prt;
	char *srcaddr, *txt;

	if (a == NULL) {
		printf("NULL\n");
		return;
	}
	if (a->sa_family == AF_INET) {
		srcaddr = (char *)&((struct sockaddr_in *)a)->sin_addr;
		txt = "IPv4 Address: ";
		prt = ntohs(((struct sockaddr_in *)a)->sin_port);
	} else if (a->sa_family == AF_INET6) {
		srcaddr = (char *)&((struct sockaddr_in6 *)a)->sin6_addr;
		prt = ntohs(((struct sockaddr_in6 *)a)->sin6_port);
		txt = "IPv6 Address: ";
	} else if (a->sa_family == AF_LINK) {
		int i;
		char tbuf[STRING_BUF_SZ];
		u_char adbuf[STRING_BUF_SZ];
		struct sockaddr_dl *dl;

		dl = (struct sockaddr_dl *)a;
		strncpy(tbuf, dl->sdl_data, dl->sdl_nlen);
		tbuf[dl->sdl_nlen] = 0;
		printf("Intf:%s (len:%d)Interface index:%d type:%x(%d) ll-len:%d ",
		    tbuf,
		    dl->sdl_nlen,
		    dl->sdl_index,
		    dl->sdl_type,
		    dl->sdl_type,
		    dl->sdl_alen
		    );
		memcpy(adbuf, LLADDR(dl), dl->sdl_alen);
		for (i = 0; i < dl->sdl_alen; i++) {
			printf("%2.2x", adbuf[i]);
			if (i < (dl->sdl_alen - 1))
				printf(":");
		}
		printf("\n");
		return;
	} else {
		return;
	}
	if (inet_ntop(a->sa_family, srcaddr, stringToPrint, sizeof(stringToPrint))) {
		if (a->sa_family == AF_INET6) {
			printf("%s%s:%d scope:%d\n",
			    txt, stringToPrint, prt,
			    ((struct sockaddr_in6 *)a)->sin6_scope_id);
		} else {
			printf("%s%s:%d\n", txt, stringToPrint, prt);
		}

	} else {
		printf("%s unprintable?\n", txt);
	}
}


#undef STRING_BUF_SZ

int
translate_ip_address(char *host, struct sockaddr_in *sa)
{
	struct hostent *hp;
	int len, cnt, i;

	if (sa == NULL) {
		return (-1);
	}
	len = strlen(host);
	cnt = 0;
	for (i = 0; i < len; i++) {
		if (host[i] == '.')
			cnt++;
	}
	if (cnt < 3) {
		/* make it treat it like a host name */
		sa->sin_addr.s_addr = 0xffffffff;
	}
	sa->sin_len = sizeof(struct sockaddr_in);
	sa->sin_family = AF_INET;
	if (sa->sin_addr.s_addr == 0xffffffff) {
		hp = gethostbyname(host);
		if (hp == NULL) {
			return (htonl(strtoul(host, NULL, 0)));
		}
		memcpy(&sa->sin_addr, hp->h_addr_list[0], sizeof(sa->sin_addr));
	} else {
		sa->sin_addr.s_addr = htonl(inet_network(host));
	}
	if (sa->sin_addr.s_addr == 0xffffffff) {
		return (-1);
	}
	return (0);
}

static int
send_req_to_servers(struct incast_control *ctrl)
{
	struct incast_peer *peer;
	struct incast_msg_req req;

	req.number_of_packets = htonl(ctrl->cnt_req);
	req.size = htonl(ctrl->size);
	ctrl->completed_server_cnt = 0;
	LIST_FOREACH(peer, &ctrl->master_list, next) {
		if(send(peer->sd, (void *)&req, sizeof(req), 0) < sizeof(req)) {
			printf("Send error:%d to", errno);
			print_an_address((struct sockaddr *)&peer->addr);
			return (-1);
		}
		peer->state = SRV_STATE_REQ_SENT;
	}
	return (0);
}

static int
build_conn_into_kq(int kq, struct incast_control *ctrl)
{
	struct kevent ke;
	struct incast_peer *peer;
	int proto, sockopt, optval;
	socklen_t optlen;
	/* First pass open up all the client sockets
	 * and prepare them.
	 */
	if (ctrl->sctp_on) {
		proto = IPPROTO_SCTP;
		sockopt = SCTP_NODELAY;
	} else {
		proto = IPPROTO_TCP;
		sockopt = TCP_NODELAY;
	}
	LIST_FOREACH(peer, &ctrl->master_list, next) {
		peer->sd = socket(AF_INET, SOCK_STREAM, proto);
		if (peer->sd == -1) {
			printf("Can't open a socket for a peer err:%d\n",
			       errno);
			return(-1);
		}
		/* Set no delay */
		optval = 1;
		optlen = sizeof(optval);
		if (setsockopt(peer->sd, proto, sockopt, 
			       &optval, optlen) == -1) {
			printf("Can't set no-delay err:%d\n", errno);
			return (-1);
		}
		/* Now bind the local address */
		optlen = sizeof(struct sockaddr_in);
		if (bind(peer->sd, (struct sockaddr *)&ctrl->bind_addr, 
			 optlen)) {
			printf("Can't bind err:%d\n", errno);
			return (-1);
		}
	}
	/* Ok all connections are built, time to connect */
	LIST_FOREACH(peer, &ctrl->master_list, next) {
		optlen = sizeof(struct sockaddr_in);
		if(connect(peer->sd, (struct sockaddr *)&peer->addr,
			   optlen)) {
			printf("Connect err:%d for address", errno);
			print_an_address((struct sockaddr *)&peer->addr);
			return (-1);
		}
	}
	/* Now stick it all in the kqueue */
	LIST_FOREACH(peer, &ctrl->master_list, next) {
		EV_SET(&ke, peer->sd, EVFILT_READ, (EV_ADD|EV_CLEAR),
		       0, 0, (void *)peer);
		if (kevent(kq, &ke, 1, NULL, 0, NULL) < 0) {
			printf("Failed to add kqueue event for peer err:%d\n",
				errno);
			printf("Peer ");
			print_an_address((struct sockaddr *)&peer->addr);
			return(-1);
		}
	}
	return (0);
}

static void
clean_up_conn(struct incast_control *ctrl)
{
	struct incast_peer *peer;
	LIST_FOREACH(peer, &ctrl->master_list, next) {
		if (peer->sd != -1) {
			close(peer->sd);
			peer->sd =-1;
		}
		peer->state = SRV_STATE_NEW;
	}
}

static void
incast_read_from(struct incast_control *ctrl, 
		 struct incast_peer *peer, int to_read)
{
	char buf[MAX_SINGLE_MSG];
	int read_am, r_ret, tot_read;
	tot_read = 0;
again:
	if ((to_read-tot_read) > MAX_SINGLE_MSG) {
		to_read = MAX_SINGLE_MSG;
	} else {
		read_am = (to_read-tot_read);
	}
	r_ret = recv(peer->sd, buf, read_am, 0);
	if (r_ret < 1) {
		printf("Error in socket read err:%d\n", errno);
		return;
	}
	if (peer->state == SRV_STATE_REQ_SENT) {
		/* First read get start time */
		if(clock_gettime(CLOCK_MONOTONIC_PRECISE, 
				 &peer->start)) {
			printf("Warning - can't get clock err:%d\n", errno);
			peer->state = SRV_STATE_ERROR;
			ctrl->completed_server_cnt++;
			close(peer->sd);
			peer->sd = -1;
		} else {
			peer->state = SRV_STATE_READING;
		}
	}
	peer->msg_cnt++;
	peer->byte_cnt += r_ret;
	tot_read += r_ret;
	if (tot_read < to_read) {
		/* Consume all kqueue told us to */
		goto again;
	}
	return;
}

static int
gather_kq_results(int kq, struct incast_control *ctrl)
{
	/* Question:
	 *  Should we run a guard timer and leave behind
	 *  connections that don't respond?
	 */
	int read_cmpl;
	struct kevent ke;
	struct incast_peer *peer;
	int not_done, kret;

	read_cmpl = (ctrl->size * ctrl->cnt_req);
	not_done = 1;
	while (not_done) {
		kret = kevent(kq, NULL, 0, &ke, 1, NULL);
		if (kret < 1) {
			printf("Kq returned %d errno:%d\n",
			       kret, errno);
			return (-1);
		}
		if (ke.filter != EVFILT_READ) {
			printf("Kevent filter is %d errno:%d ??\n - aborting",
			       ke.filter, errno);
			return (-1);
		}
		peer = (struct incast_peer *)ke.udata;
		if (ke.data) { 
			/*(number of bytes)*/
			incast_read_from(ctrl, peer, ke.data);
		}
		if (ke.flags & EV_EOF) {
			close(peer->sd);
			peer->sd = -1;
			ctrl->completed_server_cnt++;
			if (peer->state == SRV_STATE_READING) {
				if(clock_gettime(CLOCK_MONOTONIC_PRECISE, 
						 &peer->end)) {
					peer->state = SRV_STATE_ERROR;
				} else {
					if (peer->byte_cnt >= read_cmpl) {
						peer->state = SRV_STATE_COMPLETE;
					} else {
						peer->state = SRV_STATE_ERROR;
					}
				}
			} else  if ((peer->state != SRV_STATE_ERROR) &&
				    (peer->state != SRV_STATE_COMPLETE)){
				/* peer closes without any data coming in? */
				printf("Peer EV_EOF and no data?\n");
				peer->state = SRV_STATE_ERROR;
			}
		} else {
			/* Are we done? */
			if (peer->byte_cnt >= read_cmpl) {
				peer->state = SRV_STATE_COMPLETE;
				if(clock_gettime(CLOCK_MONOTONIC_PRECISE, 
						 &peer->end)) {
					peer->state = SRV_STATE_ERROR;
				}
				close(peer->sd);
				peer->sd = -1;
				ctrl->completed_server_cnt++;
			}
		}
		/* Are we done with all servers? */
		if (ctrl->completed_server_cnt >= ctrl->number_server) {
			not_done = 0;
		}
	}
	return (0);
}
void
display_results(struct incast_control *ctrl, int pass)
{
	int peerno;
	struct incast_peer *peer;
	peerno = 0;
	LIST_FOREACH(peer, &ctrl->master_list, next) {
		peerno++;
		if(peer->state == SRV_STATE_ERROR) {
			printf("Peer:%d Pass:%d - Peer Error\n", peerno, pass);
		} else if (peer->state != SRV_STATE_COMPLETE) {
			printf("Peer:%d Pass:%d - Peer left in state:%d?\n", 
			       peerno, pass, peer->state);
		} else {
			timespecsub(&peer->end, &peer->start);
			printf("Peer:%d Pass:%d %ld.%9.9ld\n",
			       peerno, pass, peer->end.tv_sec, 
			       peer->end.tv_nsec);
		}
	}
}

void 
incast_run_clients(struct incast_control *ctrl)
{
	int kq, pass;

	kq = kqueue();
	if (kq == -1) {
		printf("Fatal error - can't open kqueue err:%d\n", errno);
		return;
	}
	pass = 0;
	while (ctrl->cnt_of_times > 0) {
		ctrl->cnt_of_times -= ctrl->decrement_amm;
		pass++;
		/* Build the connections */
		if(build_conn_into_kq(kq, ctrl)) {
			printf("Can't build all the connections\n");
			clean_up_conn(ctrl);
			break;
		}

		/* Send out the request */
		if(send_req_to_servers(ctrl)) {
			printf("Can't send all results - exit cleanup\n");
			clean_up_conn(ctrl);
			break;
		}

		/* Now gather responses of data from all */
		if(gather_kq_results(kq, ctrl) ){
			printf("Gather results fails!!\n");
			clean_up_conn(ctrl);
			break;
		}

		/* Dispaly results */
		display_results(ctrl, pass);

		/* Now assure everyone is back to the new state */
		clean_up_conn(ctrl);
	}
	close(kq);
	return;
}

