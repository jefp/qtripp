/*
 * qtripp
 * Copyright (C) 2017 Jan-Piet Mens <jp@mens.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <fcntl.h>
#include <mosquitto.h>
#include "conf.h"
#include "mongoose.h"
#include "uthash.h"
#include "util.h"
#include "tline.h"
#ifdef WITH_BEAN
# include "bean.h"
#endif
#ifdef STATSD
# include <statsd/statsd-client.h>
#endif
#include "models.h"
#include "devices.h"
#include "reports.h"
#include "ignores.h"

#define SSL_VERIFY_PEER (1)
#define SSL_VERIFY_NONE (0)

static config cf = {
        .host           = "localhost",
        .port           = 1883,
#ifdef STATSD
	.statsdhost	= "127.0.0.1",
#endif
#ifdef WITH_BEAN
	.bean_host	= "127.0.0.1",
	.bean_port	= 11300,
	.bean_tube	= "qtripp",
#endif
};

/*
 * We'll be hashing connection information using alternate keys: one is
 * the socket (sock) and the other is by imei.
 */

struct conndata {
	int sock;		/* key */
	char *imei;
	char *client_ip;
	struct mbuf *mb;		/* Connection-specific mbuf */
	struct mg_connection *nc;
	UT_hash_handle hh;		/* makes this hashable for key sock */
	UT_hash_handle hh_imei;		/* makes this hashable for alternate key imei */
};
struct conndata *conns_by_sock = NULL;
struct conndata *conns_by_imei = NULL;

struct conndata *find_conn(int sock)
{
	struct conndata *co;

	HASH_FIND_INT(conns_by_sock, &sock, co);
	return (co);
}

struct conndata *find_imei(char *imei)
{
	struct conndata *co;

	HASH_FIND(hh_imei, conns_by_imei, imei, strlen(imei), co);
	return (co);
}


struct conndata *add_conn(int sock)
{
	struct conndata *co;

	HASH_FIND_INT(conns_by_sock, &sock, co);		/* imei already in hash? */
	if (co == NULL) {
		co = (struct conndata *)malloc(sizeof (struct conndata));
		co->sock = sock;
		HASH_ADD_INT(conns_by_sock, sock, co);
	}
	co->imei	= NULL;
	co->client_ip	= NULL;
	co->nc		= NULL;
	co->mb		= NULL;
	return (co);
}

void delete_conn(struct conndata *co)
{
#if 0
	struct conndata *s;
	if (co->imei) {
		HASH_FIND(hh_imei, conns_by_imei, co->imei, strlen(co->imei), s);
		if (s) {
			HASH_DEL(conns_by_imei, s);
		}
	}
#endif

	if (co->imei) free(co->imei);
	if (co->client_ip) free(co->client_ip);
	if (co->mb) mbuf_free(co->mb);

	HASH_DEL(conns_by_sock, co);
	// free(co);
	co = NULL;
}

void print_conns(struct udata *ud)
{
	struct conndata *co;

	for (co = conns_by_sock; co != NULL; co = (struct conndata *)(co->hh.next)) {
		char buf[BUFSIZ];

		snprintf(buf, sizeof(buf), "sock %d: %s (%s)", co->sock,
			co->imei ? co->imei : "nil",
			co->client_ip ? co->client_ip : "nil");

		xlog(ud, "%s\n", buf);
		if (ud->cf->reporttopic)
			pub(ud, (char *)ud->cf->reporttopic, buf, false);
	}
}

int count_conns(char *imei)
{
        struct conndata *co;
        int count = 0;

        for (co = conns_by_sock; co != NULL; co = (struct conndata *)(co->hh.next)) {
                if (co->imei && strcmp(co->imei, imei) == 0) {
                        count++;
                }
        }
        return count;
}

/*
 * We've obtained a "line" of text from a tracker via TCP in `buf' (0-terminated).
 * If `response' is non-Null, write its content back to the device.
 */

char *process(struct udata *ud, char *buf, size_t buflen, struct mg_connection *nc)
{
	char *imei, *response = NULL, rt[5120];
	struct mbuf mcopy;

	STATSD_INC(ud->cf->sd, "line.process");

	/* I have to turn mbuf into a string and must not modify it; copy  yes,
	 * ineffective, but ok for now. I could, alternatively chop the $ here,
	 * and replace by 0
	 */

	mbuf_init(&mcopy, buflen+16);

	mbuf_append(&mcopy, buf, buflen);
	mbuf_append(&mcopy, "\0\0", 2);

	imei = handle_report(ud, mcopy.buf, &response);
	if (response != NULL) {
		xlog(ud, "Responding to terminal: %s\n", response);
		mg_printf(nc, "%s", response);
		free(response);
	}

	/* If we have an IMEI, publish the RAW string
	 * to MQTT as a backup
	 */

	snprintf(rt, sizeof(rt), "%s/%s", (char *)ud->cf->rawtopic, imei);
	pub(ud, rt, mcopy.buf, false);

	mbuf_free(&mcopy);
	return (imei);
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	struct mbuf *io = &nc->recv_mbuf;
	char *imei = NULL;
	char buf[512];
	struct conndata *co;
	struct udata *ud = (struct udata *)nc->mgr->user_data;
	size_t ml;	/* mb len */
	struct mbuf *mb;

	/*
	 * On a new connection (EV_ACCEPT), add an an entry hashed by socket number
	 * and store that conndata in mongoose's nc user_data. Then, when required,
	 * obtain nc->user_data which points to conndata structure and use it.
	 * As soon as we receive an IMEI number we add it to conndata structure *and*
	 * add an alternate hash to it.
	 */

	/*
	 * The principle here is that on EV_RECV we fill our own buffer (called
	 * `mb', and stored in connection data) and clear out what we received.
	 * Then, on EV_POLL, we search through the buffer looking for our
	 * records (+....$) and process each individually.
	 */

	switch (ev) {
		case MG_EV_POLL:
			/*
			 * If we haven't recorded a connection to us yet for the current
			 * socket, abandon as we can't do anything anyway.
			 */

			if ((co = find_conn(nc->sock)) == NULL) {
				return;
			}

			/*
			 * If we don't receive anything from a device for more than
			 * 20 minutes, we can assume the connection to be dead.
			 * (Our devices are configured to a 15 minute heartbeat interval)
			 */
			if (time(NULL) - nc->last_io_time > 20 * 60) {
				xlog(ud, "Closing inactive connection on socket %d: IP is %s\n", co->sock, co->client_ip);
				STATSD_INC(ud->cf->sd, "connection.forceclose");
				nc->flags |= MG_F_CLOSE_IMMEDIATELY;
			}

			mb = co->mb;

			/*
			 * A record is +...$
			 * Search for the '$', cut there, process, and remove what we've
			 * done thus far from `mb' and await more data.
			 */

			for (ml = 0; ml < mb->len; ml++) {
				if (mb->buf[ml] == '$') {
					size_t nbytes = ml + 1;

					if (ud->datalog) {
						off_t pos;

						write(ud->datalog, mb->buf, nbytes);
						write(ud->datalog, "\n", 1);

						pos = lseek(ud->datalog, 0, SEEK_CUR);
						if (pos > (10 * 1024*1024)) {
							char path[BUFSIZ];

							close(ud->datalog);
							snprintf(path, BUFSIZ, "%s.%lld", ud->cf->datalog, (long long)time(0));
							link(ud->cf->datalog, path);
							unlink(ud->cf->datalog);
							ud->datalog = open(ud->cf->datalog, O_WRONLY | O_CREAT, 0666);
						}

					}

					imei = process(ud, mb->buf, nbytes, nc);

					if (imei != NULL && ud->cf->datadir != NULL) {
						char path[BUFSIZ];
						int fd;

						snprintf(path, sizeof(path), "%s/data-%s",
							ud->cf->datadir, imei);
						if ((fd = open(path, O_CREAT|O_WRONLY|O_APPEND, 0644)) != -1) {
							write(fd, mb->buf, nbytes);
							write(fd, "\n", 1);
							close(fd);
						}



						if ((co = (struct conndata *)nc->user_data) != NULL) {
							xlog(ud, "Found connection on socket %d: IP is %s: IMEI <%s>\n", co->sock, co->client_ip, imei);
							STATSD_INC(ud->cf->sd, "connection.reuse");
							if (co->imei == NULL) {
								co->imei = strdup(imei);
								HASH_ADD_KEYPTR(hh_imei, conns_by_imei, co->imei, strlen(co->imei), co);
							}
						}
						free(imei);
					}

					/* Clear what we've used from `mb' */
					mbuf_remove(mb, nbytes);
					break;
				}
			}
			break;

		case MG_EV_ACCEPT:
			mg_sock_addr_to_str(ev_data, buf, sizeof(buf), MG_SOCK_STRINGIFY_IP);

			if ((co = find_conn(nc->sock)) == NULL) {
				co = add_conn(nc->sock);
				co->client_ip = strdup(buf);
				co->nc = nc;
				co->mb = (struct mbuf *)malloc(sizeof (struct mbuf));
				mbuf_init(co->mb, 1024);
				xlog(ud, "Adding connection on socket %d: IP is %s\n", nc->sock, co->client_ip);
				STATSD_INC(ud->cf->sd, "connection.new");
			}

			nc->user_data = co;
			break;

		case MG_EV_RECV:

			co = find_conn(nc->sock);	/* If we can't find a connection, panic */
			assert(co != NULL);


			if (ud->cf->debughex) {
				mg_hexdump_connection(nc, ud->cf->debughex, io->buf, io->len, ev);
			}

			/*
			 * shove the bytes into our `mb' buffer, and return, after
			 * clearing our working buffer;
			 * we do the real work during POLL.
			 */

			mbuf_append(co->mb, io->buf, io->len);
			mbuf_remove(io, io->len);
			break;

		case MG_EV_CLOSE:
			if ((co = (struct conndata *)nc->user_data) != NULL) {
				if (co->imei) {
					STATSD_INC(ud->cf->sd, "connection.close");
					xlog(ud, "Disconnected connection on socket %d: IP was %s, IMEI <%s>\n",
						co->sock,
						co->client_ip ? co->client_ip : "unknown",
						co->imei ? co->imei : "");
				}

				if (co->imei && strcmp(co->imei, "123456789012345") != 0 && count_conns(co->imei) < 2) {
					pseudo_lwt(ud, co->imei);
				}

				delete_conn(co);
				nc->user_data = NULL;
			}
			break;
		default:
			break;
	}
}

#if 0
static void coco_ev_handler(struct mg_connection *nc, int ev, void *p)
{
	struct udata *ud = (struct udata *)nc->mgr->user_data;

	switch (ev) {
	case MG_EV_ACCEPT:
	case MG_EV_CONNECT:
		// puts("EV_CONNECT/ACCEPT");
		ud->cocorun = true;
		// mg_send(nc, "helLO", 5);
		// mg_printf(nc, "Hi %s, how is it?", "Julie");
		break;

	case MG_EV_CLOSE:
		ud->cocorun = false;
		nc->flags |= MG_F_CLOSE_IMMEDIATELY;
		// puts("EV_CLOSE");
		break;

	case MG_EV_RECV:
		puts("EV_RECV");
		fwrite(nc->recv_mbuf.buf, 1, nc->recv_mbuf.len, stdout);
		mbuf_remove(&nc->recv_mbuf, nc->recv_mbuf.len);

		break;

#if 0
	case MG_EV_TIMER:
		puts("TIMER!!!!");
		nc->flags |= MG_F_CLOSE_IMMEDIATELY;
		break;
#endif

	default:
		break;
	}
}
#endif

/*
 * Return a pointer to a malloced device name contained in
 * the bit before "/cmd" of an MQTT topic. Caller must free.
 *
 * "owntracks/gv/92939391/cmd" => "92939391"
 */

char *topic_to_device(char *topic)
{
        char *copy = strdup(topic);
        char *end = copy + strlen(copy) - strlen("/cmd");
        char *ptr;

        *end = 0;       /* Chop copy at last slash, cutting off "/cmd" */

        /* put ptr past last slash or on begin of string if no slash */
        if ((ptr = strrchr(copy, '/')) != NULL)
                ++ptr;
        else ptr = copy;

	ptr = strdup(ptr);
	free(copy);
	return (ptr);
}

void write_to_connection(struct mg_mgr *mgr, char *imei, char *payload)
{
	struct conndata *co;
	struct mg_connection *c;
	struct udata *ud = (struct udata *)mgr->user_data;

	if ((co = find_imei(imei)) == NULL) {
		xlog(ud, "Can't stab for imei %s\n", imei);
		return;
	}

	if ((c = co->nc) == NULL) {
		xlog(ud, "No connection information for imei %s\n", imei);
		return;
	}

	xlog(ud, "sock=%d = %s. Sending %s\n", c->sock, co->imei, payload);
	mg_printf(c, "%s", payload);
}

void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *m)
{
	struct udata *ud = (struct udata *)userdata;
	struct mg_mgr *mgr = ud->mgr;
	char *device_id;


        /*
         * mosquitto_message->
         *       int mid;
         *       char *topic;
         *       void *payload;
         *       int payloadlen;
         *       int qos;
         *       bool retain;
         */

	STATSD_INC(ud->cf->sd, "mqtt.message.in");

        xlog(ud, "MQTT on_message: %s %s\n", (char *)m->topic, (char *)m->payload);
	device_id = topic_to_device((char *)m->topic);

	if (!strcmp(device_id, "*")) {
		if (strcmp((char *)m->payload, "list") == 0)
			print_conns(ud);
		else if (strcmp((char *)m->payload, "stats") == 0)
			print_stats(ud);
		else if (strcmp((char *)m->payload, "dump") == 0)
			dump_stats(ud);
		else if (strcmp((char *)m->payload, "ping") == 0)
			pong(ud);

		return;
	}
	write_to_connection(mgr, device_id, (char *)m->payload);
	free(device_id);
}

void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
	struct udata *ud = (struct udata *)userdata;
	char err[1024];
	JsonNode *j;
	int mid;
	if (rc) {
		if (rc == MOSQ_ERR_ERRNO) {
			strerror_r(errno, err, 1024);
			xlog(ud, "connecting to MQTT broker on %s:%d Error: %s\n",
				ud->cf->host, ud->cf->port,
				err);
		} else {
			xlog(ud, "Unable to connect to MQTT (%d).\n", rc);
		}
		return;
	}

	xlog(ud, "Connack string: %s\n", mosquitto_connack_string(rc));
	xlog(ud, "Connected to MQTT broker on %s:%d\n",
			ud->cf->host, ud->cf->port);

	json_foreach(j, ud->cf->subscriptions) {
		xlog(ud, "subscribing to %s\n", j->string_);
		mosquitto_subscribe(mosq, &mid, j->string_, 0);
	}
}

int main(int argc, char **argv)
{
	struct mg_mgr mgr;
	struct mg_connection *c;
	struct mg_bind_opts bind_opts;
	struct udata udata, *ud = &udata;
	struct mosquitto *mosq;
	bool clean_session = true;
	const char *e = NULL;
	struct my_device *d, *tmp;
	int rc;

        if (ini_parse("qtripp.ini", ini_handler, &cf) < 0) {
		xlog(NULL, "Can't load/parse ini file.\n");
                return (1);
        }

	memset(&udata, 0, sizeof(udata));
    ud->debugging           = true;
	ud->cf			= &cf;
	ud->logfp		= fopen(cf.logfile, "a");

        load_models();
        load_reports();
        load_devices();
        load_ignores();

#ifdef WITH_BEAN
	if ((ud->bean_socket = bs_connect(cf.bean_host, cf.bean_port)) == BS_STATUS_FAIL) {
		xlog(ud, "Cannot conect to beanstalkd on %s:%d: %s\n",
			cf.bean_host, cf.bean_port, strerror(errno));
		exit(7);
	}

	if (bs_use(ud->bean_socket, cf.bean_tube) != BS_STATUS_OK)
		xlog(ud, "Cannot use tube %s\n", ud->cf->bean_tube);
	if (bs_watch(ud->bean_socket, cf.bean_tube) != BS_STATUS_OK)
		xlog(ud, "Cannot watch tube %s\n", ud->cf->bean_tube);
	if (bs_ignore(ud->bean_socket, "default") != BS_STATUS_OK)
		xlog(ud, "Cannot ignore tube default\n");

	xlog(ud, "Connected to beanstalkd on %s:%d for tube %s\n",
			cf.bean_host, cf.bean_port, cf.bean_tube);
#endif

	mosquitto_lib_init();
    xlog(ud, "Connecting to client_id  %s\n",cf.client_id);
	
	mosq = mosquitto_new(cf.client_id, clean_session, &udata);
	if (!mosq) {
		fprintf(stderr, "Error: mosquitto_new() says 'out of memory'.\n");
		mosquitto_lib_cleanup();
		return (-1);
	}

#ifdef STATSD
	if (cf.statsdhost) {
		cf.sd = statsd_init_with_namespace(cf.statsdhost, 8125, "qtripp");
		STATSD_INC(cf.sd, "program.launched");
	}
#endif

	if (cf.username || cf.password) {
		mosquitto_username_pw_set(mosq, cf.username, cf.password);
	}

	mosquitto_message_callback_set(mosq, on_message);
	mosquitto_connect_callback_set(mosq, on_connect);


    xlog(ud, "Connecting to client_id  %s\n",cf.client_id);
	
	if (cf.cafile && *cf.cafile) {

                        rc = mosquitto_tls_set(mosq,
                                cf.cafile,             /* cafile */
                                cf.capath,             /* capath */
                                cf.certfile,           /* certfile */
                                cf.keyfile,            /* keyfile */
                                NULL                    /* pw_callback() */
                                );
                        if (rc != MOSQ_ERR_SUCCESS) {
                                xlog(ud, "Cannot set TLS CA: %s (check path names)\n",
                                        mosquitto_strerror(rc));
                                exit(3);
                        }

                        mosquitto_tls_opts_set(mosq,
                                SSL_VERIFY_PEER,
                                NULL,                   /* tls_version: "tlsv1.2", "tlsv1" */
                                NULL                    /* ciphers */
                                );

	}
	if  (!strcmp(cf.protocol_version, "mqttv31")){
		cf.protocol=MQTT_PROTOCOL_V31;

	} else if(!strcmp(cf.protocol_version, "mqttv311")){
		cf.protocol=MQTT_PROTOCOL_V311;
	}
	else 
	{
		   xlog(ud, "Error: Invalid protocol version argument given.\n\n");
           exit(3);

	}

	rc = mosquitto_opts_set(mosq, MOSQ_OPT_PROTOCOL_VERSION, &(cf.protocol));

	if (!rc) {
		fprintf(stderr, "Error: mosquitto_opts_set() error.\n");
		mosquitto_lib_cleanup();
		return (-1);
	}

	rc = mosquitto_connect_async(mosq, cf.host, cf.port, 60);

	udata.mosq	= mosq;
	udata.datalog 	= 0;

	if (cf.datalog) {
		udata.datalog	= open(cf.datalog, O_WRONLY | O_APPEND | O_CREAT, 0666);
	}

	udata.cf  = &cf;

	if (argc == 2) {
		FILE *fp = fopen(argv[1], "r");

		if (fp == NULL) {
			perror(argv[1]);
			exit(3);
		}
		handle_file_reports(ud, fp);
		mosquitto_disconnect(ud->mosq);
		mosquitto_destroy(ud->mosq);
		exit(0);
	}

	mg_mgr_init(&mgr, NULL);

	memset(&bind_opts, 0, sizeof(bind_opts));
#if 0
	bind_opts.ssl_cert = certfile;
	bind_opts.ssl_key = keyfile;
	bind_opts.ssl_ca_cert = NULL;
#endif
	bind_opts.error_string = &e;
	bind_opts.user_data = NULL;

	xlog(ud, "Listening for GPRS on port %s\n", cf.listen_port);

	c = mg_bind_opt(&mgr, cf.listen_port, ev_handler, bind_opts);
	if (c == NULL) {
		xlog(ud, "Error starting server: %s\n", *bind_opts.error_string);
		exit(1);
	}


	udata.mgr = &mgr;
	mgr.user_data = &udata; // experiment

#if 0
	const char *address = "127.0.0.1:8881";		// FIXME: config
	struct mg_connect_opts conn_opts;

#define COCO_CONN do { \
	memset(&conn_opts, 0, sizeof(conn_opts)); \
	conn_opts.error_string = &e; \
	ud->cocorun = true; \
	if ((ud->coco = mg_connect_opt(&mgr, address, coco_ev_handler, conn_opts)) == NULL) { \
		fprintf(stderr, "mg_connect(%s) failed: %s\n", address, *conn_opts.error_string); \
		exit(EXIT_FAILURE); \
	} \
	} while (0) 

	COCO_CONN;
#endif

	while (1) {
		mg_mgr_poll(&mgr, 1000);
		mosquitto_loop(mosq, 0, 1);
#if 0
		fprintf(stderr, "Loop.. cocorun == %d\n", ud->cocorun); // FIXME
		if (ud->cocorun == false) {
			COCO_CONN;
		}
#endif
	}

	mg_mgr_free(&mgr);

	HASH_ITER(hh, cf.devices, d, tmp) {
		// printf("\t%s => %s\n", d->did, d->topic);
		free(d->did);
		free(d->topic);
		HASH_DEL(cf.devices, d);
		free(d);
	}

	json_delete(cf.subscriptions);
	return (0);
}
