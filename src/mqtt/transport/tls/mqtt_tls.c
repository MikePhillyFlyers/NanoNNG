//
// Copyright 2022 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"
#include "nng/mqtt/mqtt_client.h"
#include "nng/supplemental/nanolib/log.h"
#include "nng/supplemental/tls/tls.h"
#include "supplemental/mqtt/mqtt_msg.h"
#include "nng/protocol/mqtt/mqtt_parser.h"

#ifdef SUPP_SCRAM
#define SCRAM_ITERATION_CNT_DEFAULT 4096
#define SCRAM_DIGEST_DEFAULT        SCRAM_SHA256
#define SCRAM_DIGEST_STR_DEFAULT    "SCRAM-SHA-256"
#include "supplemental/scram/scram.h"
#endif

// TLS Over TCP transport.   Platform specific TLS Over TCP operations must be
// supplied as well.

typedef struct mqtts_tcptran_pipe mqtts_tcptran_pipe;
typedef struct mqtts_tcptran_ep   mqtts_tcptran_ep;

// tcp_pipe is one end of a TCP connection.
struct mqtts_tcptran_pipe {
	nng_stream *      conn;
	nni_pipe *        npipe;
	nni_list_node     node;
	mqtts_tcptran_ep *ep;
	nni_atomic_flag   reaped;
	nni_reap_node     reap;
	uint32_t          packmax; // MQTT Maximum Packet Size (Max length)
	uint16_t          peer;    // broker info
	uint16_t          keepalive;
	uint16_t          sndmax; // MQTT Receive Maximum (QoS 1/2 packet)
	uint8_t           proto;   // MQTT version
	uint8_t           qosmax;
	uint8_t           txlen[sizeof(uint64_t)];
	uint8_t           rxlen[sizeof(uint64_t)]; // fixed header
	size_t            rcvmax;
	size_t            gottxhead;
	size_t            gotrxhead;
	size_t            wanttxhead;
	size_t            wantrxhead;
	nni_list          recvq;
	nni_list          sendq;
	nni_aio *         txaio;
	nni_aio *         rxaio;
	nni_aio *         negoaio;
	nni_aio *         rpaio;
	nni_msg *         rxmsg;
	nni_lmq           rslmq;
	nni_mtx           mtx;
	nni_atomic_bool   closed;
	bool              busy;
	nni_msg *         smsg;
#ifdef NNG_HAVE_MQTT_BROKER
	nni_msg         *connack;
	conn_param *     cparam;
#endif
};

struct mqtts_tcptran_ep {
	nni_mtx              mtx;
	uint16_t             proto; //socket's 16-bit protocol number
	nni_duration         backoff;
	nni_duration         backoff_max;
	bool                 fini;
	bool                 started;
	bool                 closed;
	nng_url *            url;
	const char *         host; // for dialers
	nng_sockaddr         src;
	nng_sockaddr         sa;
	int                  refcnt; // active pipes
	int                  authmode;
	reason_code          reason_code;
	nni_aio *            useraio;
	nni_aio *            connaio;
	nni_aio *            timeaio;
	nni_sock *			 nsock;
	nni_list             busypipes; // busy pipes -- ones passed to socket
	nni_list             waitpipes; // pipes waiting to match to socket
	nni_list             negopipes; // pipes busy negotiating
	nni_reap_node        reap;
	nng_stream_dialer *  dialer;
	nng_stream_listener *listener;
	nni_dialer *         ndialer;
	void *               property;  // property
	void *               connmsg;
	bool                 enable_scram;
#ifdef SUPP_SCRAM
	void *               scram_ctx;
	nni_msg *            authmsg;
#endif
};

static void mqtts_tcptran_pipe_send_start(mqtts_tcptran_pipe *);
static void mqtts_tcptran_pipe_recv_start(mqtts_tcptran_pipe *);
static void mqtts_tcptran_pipe_send_cb(void *);
static void mqtts_tcptran_pipe_rp_send_cb(void *arg);
static void mqtts_tcptran_pipe_recv_cb(void *);
static void mqtts_tcptran_pipe_nego_cb(void *);
static void mqtts_tcptran_ep_fini(void *);
static void mqtts_tcptran_pipe_fini(void *);

static nni_reap_list tcptran_ep_reap_list = {
	.rl_offset = offsetof(mqtts_tcptran_ep, reap),
	.rl_func   = mqtts_tcptran_ep_fini,
};

static nni_reap_list tcptran_pipe_reap_list = {
	.rl_offset = offsetof(mqtts_tcptran_pipe, reap),
	.rl_func   = mqtts_tcptran_pipe_fini,
};

static void
mqtts_tcptran_init(void)
{
}

static void
mqtts_tcptran_fini(void)
{
}

static void
mqtts_tcptran_pipe_close(void *arg)
{
	mqtts_tcptran_pipe *p = arg;

	nni_atomic_set_bool(&p->closed, true);

	nni_aio_close(p->rxaio);
	nni_aio_close(p->txaio);
	nni_aio_close(p->negoaio);
	nni_aio_close(p->rpaio);
	nng_stream_close(p->conn);
}

static void
mqtts_tcptran_pipe_stop(void *arg)
{
	mqtts_tcptran_pipe *p = arg;

	nni_aio_stop(p->rxaio);
	nni_aio_stop(p->txaio);
	nni_aio_stop(p->negoaio);
	nni_aio_stop(p->rpaio);
}

static int
mqtts_tcptran_pipe_init(void *arg, nni_pipe *npipe)
{
	mqtts_tcptran_pipe *p = arg;

	p->npipe = npipe;
	// nni_lmq_init(&p->rslmq, 16);
	nni_atomic_init_bool(&p->closed);
	nni_atomic_set_bool(&p->closed, false);
	// set max value by default
	p->packmax == 0 ? p->packmax = (uint32_t)0xFFFFFFFF : p->packmax;
	p->qosmax  == 0 ? p->qosmax  = 2 : p->qosmax;
	p->keepalive = 0;
	return (0);
}

static void
mqtts_tcptran_pipe_fini(void *arg)
{
	mqtts_tcptran_pipe *p = arg;
	mqtts_tcptran_ep *  ep;

	mqtts_tcptran_pipe_stop(p);
	if ((ep = p->ep) != NULL) {
		nni_mtx_lock(&ep->mtx);
		nni_list_node_remove(&p->node);
		ep->refcnt--;
		if (ep->fini && (ep->refcnt == 0)) {
			nni_reap(&tcptran_ep_reap_list, ep);
		}
		nni_mtx_unlock(&ep->mtx);
	}

	nng_stream_free(p->conn);
	nni_aio_free(p->rxaio);
	nni_aio_free(p->txaio);
	nni_aio_free(p->negoaio);
	nni_aio_free(p->rpaio);

	nni_msg_free(p->rxmsg);
	// nni_lmq_fini(&p->rslmq);
	nni_mtx_fini(&p->mtx);
#ifdef NNG_HAVE_MQTT_BROKER
	conn_param_free(p->cparam);
#endif
	NNI_FREE_STRUCT(p);
}

static void
mqtts_tcptran_pipe_reap(mqtts_tcptran_pipe *p)
{
	if (!nni_atomic_flag_test_and_set(&p->reaped)) {
		if (p->conn != NULL) {
			nng_stream_close(p->conn);
		}
		nni_reap(&tcptran_pipe_reap_list, p);
	}
}

static int
mqtts_tcptran_pipe_alloc(mqtts_tcptran_pipe **pipep)
{
	mqtts_tcptran_pipe *p;
	int                 rv;

	if ((p = NNI_ALLOC_STRUCT(p)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&p->mtx);
	if (((rv = nni_aio_alloc(&p->txaio, mqtts_tcptran_pipe_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->rxaio, mqtts_tcptran_pipe_recv_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->rpaio, mqtts_tcptran_pipe_rp_send_cb, p)) != 0) ||
	    ((rv = nni_aio_alloc(&p->negoaio, mqtts_tcptran_pipe_nego_cb, p)) != 0)) {
		mqtts_tcptran_pipe_fini(p);
		return (rv);
	}
	nni_aio_list_init(&p->recvq);
	nni_aio_list_init(&p->sendq);
	nni_atomic_flag_reset(&p->reaped);

	*pipep = p;

	return (0);
}

static void
mqtts_tcptran_ep_match(mqtts_tcptran_ep *ep)
{
	nni_aio *           aio;
	mqtts_tcptran_pipe *p;

	if (((aio = ep->useraio) == NULL) ||
	    ((p = nni_list_first(&ep->waitpipes)) == NULL)) {
		return;
	}
	nni_list_remove(&ep->waitpipes, p);
	nni_list_append(&ep->busypipes, p);
	ep->useraio = NULL;
#ifdef NNG_HAVE_MQTT_BROKER
	if (p->cparam == NULL) {
		p->cparam = nni_get_conn_param_from_msg(ep->connmsg);
		if (p->keepalive != 0)
			p->cparam->keepalive_mqtt = p->keepalive;
		nni_msg_set_conn_param(ep->connmsg, p->cparam);
	}
#endif
	nni_aio_set_output(aio, 0, p);
	nni_aio_finish(aio, 0, 0);
}

static void
mqtts_tcptran_pipe_nego_cb(void *arg)
{
	mqtts_tcptran_pipe *p   = arg;
	mqtts_tcptran_ep *  ep  = p->ep;
	nni_aio *           aio = p->negoaio;
	nni_aio *           uaio;
	int                 rv;
	int                 var_int;
	uint8_t             pos = 0;

	nni_mtx_lock(&ep->mtx);

	if ((rv = nni_aio_result(aio)) != 0) {
		log_info("aio result %s", nng_strerror(rv));
		rv = SERVER_UNAVAILABLE;
		goto error;
	}
	// We start transmitting before we receive.
	if (p->gottxhead < p->wanttxhead) {
		p->gottxhead += nni_aio_count(aio);
	} else if (p->gotrxhead < p->wantrxhead) {
		p->gotrxhead += nni_aio_count(aio);
	}

	if (p->gottxhead < p->wanttxhead) {
		nni_iov iov;
		iov.iov_len = p->wanttxhead - p->gottxhead;
		iov.iov_buf = &p->txlen[p->gottxhead];
		// send it down...
		nni_aio_set_iov(aio, 1, &iov);
		nng_stream_send(p->conn, aio);
		nni_mtx_unlock(&ep->mtx);
		return;
	}

	// receving fixed header
	if (p->gotrxhead == 0 ||
	    (p->gotrxhead <= 5 && p->rxlen[p->gotrxhead - 1] > 0x7f &&
	        p->rxmsg == NULL)) {
		nni_iov iov;
		iov.iov_buf = &p->rxlen[p->gotrxhead];
		if (p->gotrxhead == 0) {
			iov.iov_len = p->wantrxhead - p->gotrxhead;
		} else {
			iov.iov_len = 1;
		}
		nni_aio_set_iov(aio, 1, &iov);
		nng_stream_recv(p->conn, aio);
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	// only accept CONNACK/AUTH msg
	if (((p->rxlen[0] & CMD_CONNACK) != CMD_CONNACK) &&
	    ((p->rxlen[0] & CMD_AUTH_V5) != CMD_AUTH_V5)) {
		log_error("Invalid type received %x %x", p->rxlen[0], p->rxlen[1]);
		rv = PROTOCOL_ERROR;
		goto error;
	}
	// finish recevied fixed header
	if (p->rxmsg == NULL) {
		pos = 0;
		if ((rv = mqtt_get_remaining_length(p->rxlen, p->gotrxhead,
		         (uint32_t *) &var_int, &pos)) != 0) {
			rv = PAYLOAD_FORMAT_INVALID;
			goto error;
		}

		if ((rv = nni_mqtt_msg_alloc(&p->rxmsg, var_int)) != 0) {
			rv = NNG_ENOMEM;
			goto error;
		}

		nni_msg_header_append(p->rxmsg, p->rxlen, pos + 1);

		p->wantrxhead = var_int + 1 + pos;
		if (p->proto == MQTT_PROTOCOL_VERSION_v311 &&
		    ((rv = (p->wantrxhead <= 4) ? 0 : NNG_EPROTO) != 0)) {
			// Broker send a invalid CONNACK!
			rv = PROTOCOL_ERROR;
			goto error;
		}
	}
	// got remaining length
	if (p->gotrxhead < p->wantrxhead) {
		nni_iov iov;
		iov.iov_len = p->wantrxhead - p->gotrxhead;
		iov.iov_buf = nni_msg_body(p->rxmsg);
		nni_aio_set_iov(aio, 1, &iov);
		nng_stream_recv(p->conn, aio);
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	// Handle connack/auth
	if (p->gotrxhead >= p->wantrxhead) {
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			rv = nni_mqttv5_msg_decode(p->rxmsg);
			ep->reason_code = rv;
			if (rv != 0)
				goto mqtt_error;
#ifdef SUPP_SCRAM
			if (ep->scram_ctx &&
				nni_mqtt_msg_get_packet_type(p->rxmsg) == NNG_MQTT_AUTH) {
				property *prop = nni_mqtt_msg_get_auth_property(p->rxmsg);
				if (prop == NULL) {
					ep->reason_code = MQTT_ERR_MALFORMED;
					rv = MQTT_ERR_PROTOCOL;
					log_error("No property found in AUTH msg");
					goto mqtt_error;
				}
				uint8_t rc = nni_mqtt_msg_get_auth_reason_code(p->rxmsg);
				if (rc != 0x18) {
					ep->reason_code = MQTT_ERR_MALFORMED;
					rv = MQTT_ERR_PROTOCOL;
					log_error("Reason code in AUTH msg is invalid");
					goto mqtt_error;
				}
				property_data *data = property_get_value(prop, AUTHENTICATION_DATA);
				if (data == NULL || data->p_value.str.buf == NULL) {
					ep->reason_code = MQTT_ERR_MALFORMED;
					rv = MQTT_ERR_PROTOCOL;
					log_error("No auth data property found in AUTH msg");
					goto mqtt_error;
				}
				log_debug("auth:server_first_msg:%.*s",
					data->p_value.str.length, (char *)data->p_value.str.buf);
				char *client_final_msg = scram_handle_server_first_msg(
					ep->scram_ctx, (char *)data->p_value.str.buf, data->p_value.str.length);
				if (client_final_msg == NULL) {
					ep->reason_code = MQTT_ERR_MALFORMED;
					rv = MQTT_ERR_PROTOCOL;
					log_error("Error in handle scram server_first_msg");
					goto mqtt_error;
				}
				log_debug("auth:client_final_msg:%s", client_final_msg);
				// TODO 0x19 Re-authenticate
				// Prepare authmsg with client_final_msg
				nni_msg *authmsg;
				nni_mqtt_msg_alloc(&authmsg, 0);
				nni_mqtt_msg_set_packet_type(authmsg, NNG_MQTT_AUTH);
				nni_mqtt_msg_set_auth_reason_code(authmsg, 0x18);
				property *props = mqtt_property_alloc();
				property *prop_auth_method = property_set_value_str(
					AUTHENTICATION_METHOD, SCRAM_DIGEST_STR_DEFAULT,
					strlen(SCRAM_DIGEST_STR_DEFAULT), true);
				property *prop_auth_data   = property_set_value_str(
					AUTHENTICATION_DATA, client_final_msg, strlen(client_final_msg), true);
				property_append(props, prop_auth_method);
				property_append(props, prop_auth_data);
				nni_mqtt_msg_set_auth_property(authmsg, props);
				if (0 != nni_mqttv5_msg_encode(authmsg)) {
					ep->reason_code = MQTT_ERR_MALFORMED;
					rv = MQTT_ERR_PROTOCOL;
					log_error("Error in encode auth msg with client_final_msg");
					goto mqtt_error;
				}
				if (ep->authmsg)
					nng_msg_free(ep->authmsg);
				ep->authmsg = authmsg;
				nng_free(client_final_msg, 0);
				// Update got/want to send client_final_msg and recv connack
				nng_msg_free(p->rxmsg);
				p->gotrxhead  = 0;
				p->gottxhead  = 0;
				p->wantrxhead = 2;
				p->wanttxhead = nni_msg_header_len(authmsg) + nni_msg_len(authmsg);
				p->rxmsg      = NULL;

				nni_iov iov[2];
				int niov = 0;
				if (nni_msg_len(authmsg) > 0) {
					nni_msg_insert(authmsg, nni_msg_header(authmsg),
					    nni_msg_header_len(authmsg));
					iov[niov].iov_buf = nni_msg_body(authmsg);
					iov[niov].iov_len = nni_msg_len(authmsg);
					niov++;
				}
				nni_aio_set_iov(aio, niov, iov);
				nng_stream_send(p->conn, p->negoaio);
				nni_mtx_unlock(&ep->mtx);

				return;
			}
#endif
			property_free(ep->property);
			ep->property = NULL;
			property *prop = nni_mqtt_msg_get_connack_property(p->rxmsg);
			if (property_dup((property **) &ep->property, prop) != 0)
				goto mqtt_error;
			if (ep->property != NULL) {
				property_data *data;
				data = property_get_value(ep->property, RECEIVE_MAXIMUM);
				if (data) {
					if (data->p_value.u16 == 0) {
						rv = MQTT_ERR_PROTOCOL;
						ep->reason_code = rv;
						goto mqtt_error;
					} else {
						p->sndmax = data->p_value.u16;
					}
				}
				data = property_get_value(ep->property, MAXIMUM_PACKET_SIZE);
				if (data) {
					if (data->p_value.u32 == 0) {
						rv = MQTT_ERR_PROTOCOL;
						ep->reason_code = rv;
						goto mqtt_error;
					} else {
						p->packmax = data->p_value.u32;
						log_info("Set max packet size as %ld", p->packmax);
					}
				}
				data = property_get_value(ep->property, PUBLISH_MAXIMUM_QOS);
				if (data) {
					p->qosmax = data->p_value.u8;
				}
				data = property_get_value(ep->property, SERVER_KEEP_ALIVE);
				if (data) {
					p->keepalive = data->p_value.u16;
				}
#ifdef SUPP_SCRAM
				data = property_get_value(ep->property, AUTHENTICATION_DATA);
				if (data && data->p_value.str.buf && ep->scram_ctx) {
					char *server_final_msg = (char *)data->p_value.str.buf;
					log_debug("auth:server_final_msg:%.*s",
						data->p_value.str.length, server_final_msg);
					char *result = scram_handle_server_final_msg(
						ep->scram_ctx, server_final_msg, data->p_value.str.length);
					if (result == NULL) {
						log_error("Enhanced Authentication failed");
						rv = MQTT_ERR_PROTOCOL;
						ep->reason_code = rv;
						// Failed so closed the connection
						goto error;
					} else {
						log_info("Enhanced Authentication Passed");
					}
				} else if (ep->scram_ctx) {
					// We want a authenticate response. but not found
					log_error("Enhanced Authentication failed");
					rv = MQTT_ERR_PROTOCOL;
					ep->reason_code = rv;
					goto error;
				} else {
					// No more action
				}
#endif
			}
		} else {
			if ((rv = nni_mqtt_msg_decode(p->rxmsg)) != MQTT_SUCCESS) {
				ep->reason_code = rv;
				goto mqtt_error;
			}
			ep->property = NULL;
		}
		ep->reason_code = nni_mqtt_msg_get_connack_return_code(p->rxmsg);
	}
	// put 
#ifdef NNG_HAVE_MQTT_BROKER
	nni_msg_clone(p->rxmsg);
	p->connack = p->rxmsg;
#endif


mqtt_error:
	// We are ready now.  We put this in the wait list, and
	// then try to run the matcher.
	nni_list_remove(&ep->negopipes, p);
	nni_list_append(&ep->waitpipes, p);

	nni_msg_free(p->rxmsg);
	p->rxmsg = NULL;

	if (rv == MQTT_SUCCESS) {
		mqtts_tcptran_ep_match(ep);
	} else {
		// Fail but still match to let user know ack has arrived
		mqtts_tcptran_ep_match(ep);
		// send DISCONNECT
		nni_iov iov;
		p->txlen[0] = CMD_DISCONNECT;
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			p->txlen[1] = 0x02;
			p->txlen[2] = ep->reason_code;
			p->txlen[3] = 0; // length of property
			iov.iov_len = 4;
		} else {
			p->txlen[1] = 0x00;
			iov.iov_len = 2;
		}
		iov.iov_buf = p->txlen;
		nni_aio_set_iov(p->rpaio, 1, &iov);
		nng_stream_send(p->conn, p->rpaio);
	}
	nni_mtx_unlock(&ep->mtx);

	return;

error:
	// If the connection is closed, we need to pass back a different
	// error code.  This is necessary to avoid a problem where the
	// closed status is confused with the accept file descriptor
	// being closed.
	if (rv == NNG_ECLOSED) {
		rv = SERVER_SHUTTING_DOWN;
	}
	nng_stream_close(p->conn);

	if (p->rxmsg != NULL) {
		nni_msg_free(p->rxmsg);
		p->rxmsg = NULL;
	}

	if ((uaio = ep->useraio) != NULL) {
		ep->useraio = NULL;
		nni_aio_finish_error(uaio, rv);
	}
	nni_list_remove(&ep->negopipes, p);
	nni_mtx_unlock(&ep->mtx);
	mqtts_tcptran_pipe_reap(p);
}

static void
mqtts_tcptran_pipe_send_cb(void *arg)
{
	mqtts_tcptran_pipe *p = arg;
	int                 rv;
	nni_aio *           aio;
	size_t              n;
	nni_msg *           msg;
	nni_aio *           txaio = p->txaio;

	nni_mtx_lock(&p->mtx);
	aio = nni_list_first(&p->sendq);

	if ((rv = nni_aio_result(txaio)) != 0) {
		nni_pipe_bump_error(p->npipe, rv);
		log_info("aio result %s", nng_strerror(rv));
		// Intentionally we do not queue up another transfer.
		// There's an excellent chance that the pipe is no longer
		// usable, with a partial transfer.
		// The protocol should see this error, and close the
		// pipe itself, we hope.
		nni_aio_list_remove(aio);
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}

	n = nni_aio_count(txaio);
	nni_aio_iov_advance(txaio, n);
	if (nni_aio_iov_count(txaio) > 0) {
		nng_stream_send(p->conn, txaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}

	nni_aio_list_remove(aio);
	mqtts_tcptran_pipe_send_start(p);

	msg = nni_aio_get_msg(aio);
	n   = nni_msg_len(msg);
	nni_mtx_unlock(&p->mtx);
#ifdef NNG_ENABLE_STATS
	// nni_pipe_bump_tx(p->npipe, n);
	nni_sock_bump_tx(p->ep->nsock, n);
#endif
	nni_aio_set_msg(aio, NULL);
	nni_msg_free(msg);
	nni_aio_finish_sync(aio, rv, n);
}

static void
mqtts_tcptran_pipe_rp_send_cb(void *arg)
{
	mqtts_tcptran_pipe *p     = arg;
	nni_aio            *rpaio = p->rpaio;
	size_t              n;
	int                 rv;

	if ((rv = nni_aio_result(rpaio)) != 0) {
		log_warn(" send aio error %s", nng_strerror(rv));
		// pipe is reaped in nego_cb
		return;
	}

	nni_mtx_lock(&p->mtx);
	n = nni_aio_count(rpaio);
	nni_aio_iov_advance(rpaio, n);

	// more bytes to send
	if (nni_aio_iov_count(rpaio) > 0) {
		nng_stream_send(p->conn, rpaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	nni_mtx_unlock(&p->mtx);
	return;
}

static void
mqtts_tcptran_pipe_recv_cb(void *arg)
{
	nni_aio *           aio;
	nni_iov             iov;
	uint8_t             type, pos, flags;
	uint32_t            len = 0, rv;
	size_t              n;
	nni_msg *           msg;
	mqtts_tcptran_pipe *p     = arg;
	nni_aio *           rxaio = p->rxaio;
	bool                ack   = false;

	nni_mtx_lock(&p->mtx);

	aio = nni_list_first(&p->recvq);

	if ((rv = nni_aio_result(rxaio)) != 0) {
		log_info("aio result %s", nng_strerror(rv));
		rv = SERVER_UNAVAILABLE;
		goto recv_error;
	}

	n = nni_aio_count(rxaio);
	p->gotrxhead += n;

	// in case one aio is not enough for handling whole buffer
	nni_aio_iov_advance(rxaio, n);
	if (nni_aio_iov_count(rxaio) > 0) {
		nng_stream_recv(p->conn, rxaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}

	rv = mqtt_get_remaining_length(p->rxlen, p->gotrxhead, &len, &pos);
	p->wantrxhead = len + 1 + pos;
	if (p->gotrxhead <= 5 && p->rxlen[p->gotrxhead - 1] > 0x7f) {
		if (p->gotrxhead == NNI_NANO_MAX_HEADER_SIZE) {
			rv = PACKET_TOO_LARGE;
			goto recv_error;
		}
		// same packet, continue receving next byte of remaining length
		iov.iov_buf = &p->rxlen[p->gotrxhead];
		iov.iov_len = 1;
		nni_aio_set_iov(rxaio, 1, &iov);
		nng_stream_recv(p->conn, rxaio);
		nni_mtx_unlock(&p->mtx);
		return;
	}

	// fixed header finished
	if (NULL == p->rxmsg) {
		// Make sure the message payload is not too big.  If it is
		// the caller will shut down the pipe.
		if ((len > p->rcvmax) && (p->rcvmax > 0)) {
			rv = PACKET_TOO_LARGE;
			goto recv_error;
		}

		if ((rv = nni_msg_alloc(&p->rxmsg, (size_t) len)) != 0) {
			rv = UNSPECIFIED_ERROR;
			goto recv_error;
		}

		// Submit the rest of the data for a read -- seperate Fixed
		// header with variable header and so on
		//  we want to read the entire message now.
		if (len != 0) {
			iov.iov_buf = nni_msg_body(p->rxmsg);
			iov.iov_len = (size_t) len;

			nni_aio_set_iov(rxaio, 1, &iov);
			// second recv action
			nng_stream_recv(p->conn, rxaio);
			nni_mtx_unlock(&p->mtx);
			return;
		}
	}

	// We read a message completely.  Let the user know the good news. use
	// as application message callback of users
	nni_aio_list_remove(aio);
	nni_msg_header_append(p->rxmsg, p->rxlen, pos + 1);
	msg      = p->rxmsg;
	p->rxmsg = NULL;
	n        = nni_msg_len(msg);
	type     = p->rxlen[0] & 0xf0;
	flags    = p->rxlen[0] & 0x0f;

	// set the payload pointer of msg according to packet_type
	uint8_t   qos_pac;
	uint16_t  packet_id   = 0;
	uint8_t   reason_code = 0;
	property *prop        = NULL;
	uint8_t   ack_cmd     = 0;
	switch (type) {
	case CMD_PUBLISH:
		// should we seperate the 2 phase work of QoS into 2 aios?
		// TODO MQTT v5 qos
		qos_pac = nni_msg_get_pub_qos(msg);
		if (qos_pac > 0) {
			if (qos_pac == 1) {
				ack_cmd = CMD_PUBACK;
			} else if (qos_pac == 2) {
				ack_cmd = CMD_PUBREC;
			} else {
				log_warn("Wrong QoS level!");
				rv = PROTOCOL_ERROR;
				goto recv_error;
			}
			if ((packet_id = nni_msg_get_pub_pid(msg)) == 0) {
				rv = PROTOCOL_ERROR;
				goto recv_error;
			}
			ack = true;
		}
		break;
	case CMD_PUBREC:
		if (nni_mqtt_pubres_decode(msg, &packet_id, &reason_code, &prop,
		        p->proto) != 0) {
			rv = PROTOCOL_ERROR;
			goto recv_error;
		}
		ack_cmd = CMD_PUBREL;
		ack     = true;
		break;
	case CMD_PUBREL:
		if (flags == 0x02) {
			if (nni_mqtt_pubres_decode(msg, &packet_id, &reason_code,
			        &prop, p->proto) != 0) {
				rv = PROTOCOL_ERROR;
				goto recv_error;
			}
			ack_cmd = CMD_PUBCOMP;
			ack     = true;
			break;
		} else {
			rv = PROTOCOL_ERROR;
			goto recv_error;
		}
	case CMD_PUBACK:
		// TODO set property for user callback
	case CMD_PUBCOMP:
		if (nni_mqtt_pubres_decode(
		        msg, &packet_id, &reason_code, &prop, p->proto) != 0) {
			rv = PROTOCOL_ERROR;
			goto recv_error;
		}
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			p->sndmax++;
		}
		break;
	case CMD_PINGRESP:
		//free here?
		break;
	case CMD_DISCONNECT:
		break;
	case CMD_SUBACK:
	case CMD_UNSUBACK:
		break;
	default:
		log_warn("invalid packet type %d", type);
		goto recv_error;
	}

	if (ack == true) {
		// alloc a msg here costs memory. However we must do it for the
		// sake of compatibility with nng.
		nni_msg *qmsg;
		if ((rv = nni_msg_alloc(&qmsg, 0)) != 0) {
			ack = false;
			rv  = UNSPECIFIED_ERROR;
			goto recv_error;
		}
		// TODO set reason code or property here if necessary
		nni_mqtt_msgack_encode(
		    qmsg, packet_id, reason_code, prop, p->proto);
		nni_mqtt_pubres_header_encode(qmsg, ack_cmd);
		if (p->proto == MQTT_PROTOCOL_VERSION_v5) {
			property_free(prop);
		}
		// let protocol layer handle ack msg for us
		nni_aio_set_prov_data(aio, qmsg);
		ack = false;
	}

	// keep connection & Schedule next receive
	if (!nni_list_empty(&p->recvq)) {
		mqtts_tcptran_pipe_recv_start(p);
	}
#ifdef NNG_HAVE_MQTT_BROKER
	nni_msg_set_conn_param(msg, p->cparam);
#endif
	nni_aio_set_msg(aio, msg);
	nni_mtx_unlock(&p->mtx);
#ifdef NNG_ENABLE_STATS
	// nni_pipe_bump_rx(p->npipe, n);
	nni_sock_bump_rx(p->ep->nsock, n);
#endif
	nni_aio_finish_sync(aio, 0, n);
	return;

recv_error:
	nni_aio_list_remove(aio);
	msg      = p->rxmsg;
	p->rxmsg = NULL;
	nni_pipe_bump_error(p->npipe, rv);
	nni_mtx_unlock(&p->mtx);

	nni_msg_free(msg);
	nni_aio_finish_error(aio, SERVER_UNAVAILABLE);
}

static void
mqtts_tcptran_pipe_send_cancel(nni_aio *aio, void *arg, int rv)
{
	mqtts_tcptran_pipe *p = arg;

	nni_mtx_lock(&p->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&p->mtx);
		return;
	}
	// If this is being sent, then cancel the pending transfer.
	// The callback on the txaio will cause the user aio to
	// be canceled too.
	if (nni_list_first(&p->sendq) == aio) {
		nni_aio_abort(p->txaio, rv);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&p->mtx);

	nni_aio_finish_error(aio, rv);
}

static void
mqtts_tcptran_pipe_send_start(mqtts_tcptran_pipe *p)
{
	nni_aio *aio;
	nni_aio *txaio;
	nni_msg *msg;
	int      niov;
	nni_iov  iov[3];

	if (nni_atomic_get_bool(&p->closed)) {
		while ((aio = nni_list_first(&p->sendq)) != NULL) {
			nni_list_remove(&p->sendq, aio);
			nni_aio_finish_error(aio, SERVER_SHUTTING_DOWN);
		}
		return;
	}

	if ((aio = nni_list_first(&p->sendq)) == NULL) {
		return;
	}

	// This runs to send the message.
	msg = nni_aio_get_msg(aio);

	if (msg != NULL && p->proto == MQTT_PROTOCOL_VERSION_v5) {
		uint8_t *header = nni_msg_header(msg);
		if ((*header & 0XF0) == CMD_PUBLISH) {
			// check max qos
			uint8_t qos = nni_mqtt_msg_get_publish_qos(msg);
			if (qos > 0)
				p->sndmax --;
			if (qos > p->qosmax) {
				p->qosmax == 1 ? ((*header &= 0XF9), (*header |= 0X02)) : NNI_ARG_UNUSED(*header);
				p->qosmax == 0 ? *header &= 0XF9 : NNI_ARG_UNUSED(*header);
			}
		}
	}

	// check max packet size
	if (nni_msg_header_len(msg) + nni_msg_len(msg) > p->packmax) {
		txaio = p->txaio;
		nni_aio_finish_error(txaio, UNSPECIFIED_ERROR);
		return;
	}

	txaio = p->txaio;
	niov  = 0;

	if (nni_msg_header_len(msg) > 0) {
		iov[niov].iov_buf = nni_msg_header(msg);
		iov[niov].iov_len = nni_msg_header_len(msg);
		niov++;
	}
	if (nni_msg_len(msg) > 0) {
		iov[niov].iov_buf = nni_msg_body(msg);
		iov[niov].iov_len = nni_msg_len(msg);
		niov++;
	}
	nni_aio_set_iov(txaio, niov, iov);
	nng_stream_send(p->conn, txaio);
}

static void
mqtts_tcptran_pipe_send(void *arg, nni_aio *aio)
{
	mqtts_tcptran_pipe *p = arg;
	int                 rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	if (nni_atomic_get_bool(&p->closed)) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_mtx_lock(&p->mtx);
	if ((rv = nni_aio_schedule(aio, mqtts_tcptran_pipe_send_cancel, p)) != 0) {
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	nni_list_append(&p->sendq, aio);
	if (nni_list_first(&p->sendq) == aio) {
		mqtts_tcptran_pipe_send_start(p);
	}
	nni_mtx_unlock(&p->mtx);
}

static void
mqtts_tcptran_pipe_recv_cancel(nni_aio *aio, void *arg, int rv)
{
	mqtts_tcptran_pipe *p = arg;

	nni_mtx_lock(&p->mtx);
	if (!nni_aio_list_active(aio)) {
		nni_mtx_unlock(&p->mtx);
		return;
	}
	// If receive in progress, then cancel the pending transfer.
	// The callback on the rxaio will cause the user aio to
	// be canceled too.
	if (nni_list_first(&p->recvq) == aio) {
		nni_aio_abort(p->rxaio, rv);
		nni_mtx_unlock(&p->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	nni_mtx_unlock(&p->mtx);
	nni_aio_finish_error(aio, rv);
}

static void
mqtts_tcptran_pipe_recv_start(mqtts_tcptran_pipe *p)
{
	nni_aio *rxaio;
	nni_iov  iov;

	if (nni_atomic_get_bool(&p->closed)) {
		nni_aio *aio;
		while ((aio = nni_list_first(&p->recvq)) != NULL) {
			nni_list_remove(&p->recvq, aio);
			nni_aio_finish_error(aio, SERVER_SHUTTING_DOWN);
		}
		return;
	}
	if (nni_list_empty(&p->recvq)) {
		return;
	}

	// Schedule a read of the header.
	rxaio         = p->rxaio;
	p->gotrxhead  = 0;
	p->wantrxhead = 2;
	iov.iov_buf   = p->rxlen;
	iov.iov_len   = 2;
	memset(p->rxlen, '\0', NNI_NANO_MAX_HEADER_SIZE * sizeof(p->rxlen[0]));
	nni_aio_set_iov(rxaio, 1, &iov);
	nng_stream_recv(p->conn, rxaio);
}

static void
mqtts_tcptran_pipe_recv(void *arg, nni_aio *aio)
{
	mqtts_tcptran_pipe *p = arg;
	int                 rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	nni_mtx_lock(&p->mtx);
	if ((rv = nni_aio_schedule(aio, mqtts_tcptran_pipe_recv_cancel, p)) !=
	    0) {
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
#ifdef NNG_HAVE_MQTT_BROKER
	if (p->connack != NULL) {
		nni_aio_set_msg(aio, p->connack);
		nni_msg_set_conn_param(p->connack, p->cparam);
		p->connack = NULL;
		nni_mtx_unlock(&p->mtx);
		nni_aio_finish(aio, 0, 0);
		return;
	}
#endif
	nni_list_append(&p->recvq, aio);
	if (nni_list_first(&p->recvq) == aio) {
		mqtts_tcptran_pipe_recv_start(p);
	}
	nni_mtx_unlock(&p->mtx);
}

static uint16_t
mqtts_tcptran_pipe_peer(void *arg)
{
	mqtts_tcptran_pipe *p = arg;

	return (p->peer);
}

static int
mqtts_tcptran_pipe_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	mqtts_tcptran_pipe *p = arg;
	return (nni_stream_get(p->conn, name, buf, szp, t));
}

static void
mqtts_tcptran_pipe_start(
    mqtts_tcptran_pipe *p, nng_stream *conn, mqtts_tcptran_ep *ep)
{
	nni_iov  iov[2];
	nni_msg *connmsg = NULL;
	uint8_t mqtt_version;
	int      niov = 0;
	int      rv;

	ep->refcnt++;

	p->conn    = conn;
	p->ep      = ep;
	p->qosmax  = 0;
	p->packmax = 0;
	p->rcvmax  = 0;
	p->sndmax  = 65535;
#ifdef NNG_HAVE_MQTT_BROKER
	p->cparam = NULL;
#endif
	nni_dialer_getopt(ep->ndialer, NNG_OPT_MQTT_CONNMSG, &connmsg, NULL,
	    NNI_TYPE_POINTER);

	if (connmsg == NULL) {
		mqtt_version = 0;
		log_error("User forget to set CONNECT msg!");
	} else {
		mqtt_version = nni_mqtt_msg_get_connect_proto_version(connmsg);
	}

	if (mqtt_version == MQTT_PROTOCOL_VERSION_v311)
		rv = nni_mqtt_msg_encode(connmsg);
	else if (mqtt_version == MQTT_PROTOCOL_VERSION_v5) {
		property *prop = nni_mqtt_msg_get_connect_property(connmsg);
		property_data *data;
		data = property_get_value(prop, MAXIMUM_PACKET_SIZE);
		if (data)
			p->rcvmax = data->p_value.u32;

#ifdef SUPP_SCRAM
		char *pwd = NULL, *username = NULL;
		char *pwd2 = NULL, *username2 = NULL;
		int   pwdsz, usernamesz;
		if (ep->enable_scram == true &&
		   ((pwd = (char *)nni_mqtt_msg_get_connect_password(connmsg)) != NULL) &&
		   ((username = (char *)nni_mqtt_msg_get_connect_user_name(connmsg)) != NULL)) {
			pwdsz = nni_mqtt_msg_get_connect_password_len(connmsg);
			usernamesz = nni_mqtt_msg_get_connect_user_name_len(connmsg);
			pwd2      = strndup(pwd, pwdsz);
			username2 = strndup(username, usernamesz);
			if (ep->scram_ctx) {
				scram_ctx_free(ep->scram_ctx);
			}
			ep->scram_ctx = scram_ctx_create(pwd2, strlen(pwd2),
				SCRAM_ITERATION_CNT_DEFAULT, SCRAM_DIGEST_DEFAULT, 0);
		}
		if (ep->scram_ctx) {
			property *prop_auth_method = property_set_value_str(
				AUTHENTICATION_METHOD, SCRAM_DIGEST_STR_DEFAULT,
				strlen(SCRAM_DIGEST_STR_DEFAULT), true);
			char *client_first_msg     = scram_client_first_msg(ep->scram_ctx, username2);
			property *prop_auth_data   = property_set_value_str(
				AUTHENTICATION_DATA, client_first_msg, strlen(client_first_msg), true);
			if (prop == NULL)
				prop = mqtt_property_alloc();
			property_append(prop, prop_auth_method);
			property_append(prop, prop_auth_data);
			nni_mqtt_msg_set_connect_property(connmsg, prop);
			log_debug("auth:client_first_msg:%s", client_first_msg);
			//property_free(prop_auth_method);
			//property_free(prop_auth_data);
		}
		if (pwd2)
			nng_free(pwd2, 0);
		if (username2)
			nng_free(username2, 0);
#endif
		rv = nni_mqttv5_msg_encode(connmsg);
	} else {
		nni_plat_printf("Warning. MQTT protocol version is not specificed.\n");
		rv = 1;
	}

	if (rv != MQTT_SUCCESS ||
	   (mqtt_version != MQTT_PROTOCOL_VERSION_v311 &&
	    mqtt_version != MQTT_PROTOCOL_VERSION_v5)) {
		// Free the msg from user
		nni_msg_free(connmsg);
		log_error("Warning. Cancelled a illegal connnect msg from user.\n");
		// Using MQTT V311 as default protocol version
		mqtt_version = 4; // Default TODO Notify user as a warning
		nni_mqtt_msg_alloc(&connmsg, 0);
		nni_mqtt_msg_set_packet_type(connmsg, NNG_MQTT_CONNECT);
		nni_mqtt_msg_set_connect_proto_version(
		    connmsg, MQTT_PROTOCOL_VERSION_v311);
		nni_mqtt_msg_set_connect_keep_alive(connmsg, 60);
		nni_mqtt_msg_set_connect_clean_session(connmsg, true);
		ep->connmsg = connmsg;
	}

	p->gotrxhead  = 0;
	p->gottxhead  = 0;
	p->wantrxhead = 2;
	p->wanttxhead = nni_msg_header_len(connmsg) + nni_msg_len(connmsg);
	p->rxmsg      = NULL;
	p->proto      = mqtt_version;

	if (nni_msg_len(connmsg) > 0) {
		nni_msg_insert(connmsg, nni_msg_header(connmsg),
		    nni_msg_header_len(connmsg));
		iov[niov].iov_buf = nni_msg_body(connmsg);
		iov[niov].iov_len = nni_msg_len(connmsg);
		niov++;
	}
	nni_aio_set_iov(p->negoaio, niov, iov);
	nni_list_append(&ep->negopipes, p);

	nni_aio_set_timeout(p->negoaio, 10000); // 10 sec timeout to negotiate
	nng_stream_send(p->conn, p->negoaio);
}

static void
mqtts_tcptran_ep_fini(void *arg)
{
	mqtts_tcptran_ep *ep = arg;

	nni_mtx_lock(&ep->mtx);
	ep->fini = true;
	if (ep->refcnt != 0) {
		nni_mtx_unlock(&ep->mtx);
		return;
	}
	nni_mtx_unlock(&ep->mtx);
	// Free connmsg once
	if (ep->connmsg)
		nni_msg_free(ep->connmsg);
#ifdef SUPP_SCRAM
	if (ep->authmsg)
		nni_msg_free(ep->authmsg);
	ep->authmsg = NULL;
	if (ep->scram_ctx)
		scram_ctx_free(ep->scram_ctx);
#endif

	nni_aio_stop(ep->timeaio);
	nni_aio_stop(ep->connaio);
	nng_stream_dialer_free(ep->dialer);
	nng_stream_listener_free(ep->listener);
	nni_aio_free(ep->timeaio);
	nni_aio_free(ep->connaio);
	property_free(ep->property);

	nni_mtx_fini(&ep->mtx);
	NNI_FREE_STRUCT(ep);
}

static void
mqtts_tcptran_ep_close(void *arg)
{
	mqtts_tcptran_ep *  ep = arg;
	mqtts_tcptran_pipe *p;

	nni_mtx_lock(&ep->mtx);

	ep->closed = true;
	nni_aio_close(ep->timeaio);
	if (ep->dialer != NULL) {
		nng_stream_dialer_close(ep->dialer);
	}
	if (ep->listener != NULL) {
		nng_stream_listener_close(ep->listener);
	}
	NNI_LIST_FOREACH (&ep->negopipes, p) {
		mqtts_tcptran_pipe_close(p);
	}
	NNI_LIST_FOREACH (&ep->waitpipes, p) {
		mqtts_tcptran_pipe_close(p);
	}
	NNI_LIST_FOREACH (&ep->busypipes, p) {
		mqtts_tcptran_pipe_close(p);
	}
	if (ep->useraio != NULL) {
		nni_aio_finish_error(ep->useraio, NNG_ECLOSED);
		ep->useraio = NULL;
	}

	nni_mtx_unlock(&ep->mtx);
}

// This parses off the optional source address that this transport
// uses. The special handling of this URL format is quite honestly an
// historical mistake, which we would remove if we could.
static int
mqtts_tcptran_url_parse_source(
    nng_url *url, nng_sockaddr *sa, const nng_url *surl)
{
	int      af;
	char *   semi;
	char *   src;
	size_t   len;
	int      rv;
	nni_aio *aio;

	// We modify the URL.  This relies on the fact that the underlying
	// transport does not free this, so we can just use references.

	url->u_scheme   = surl->u_scheme;
	url->u_port     = surl->u_port;
	url->u_hostname = surl->u_hostname;

	if ((semi = strchr(url->u_hostname, ';')) == NULL) {
		memset(sa, 0, sizeof(*sa));
		return (0);
	}

	len             = (size_t) (semi - url->u_hostname);
	url->u_hostname = semi + 1;

	if (strcmp(surl->u_scheme, "tls+tcp") == 0) {
		af = NNG_AF_UNSPEC;
	} else if (strcmp(surl->u_scheme, "tls+tcp4") == 0) {
		af = NNG_AF_INET;
	} else if (strcmp(surl->u_scheme, "tls+tcp6") == 0) {
		af = NNG_AF_INET6;
	} else {
		return (NNG_EADDRINVAL);
	}

	if ((src = nni_alloc(len + 1)) == NULL) {
		return (NNG_ENOMEM);
	}
	memcpy(src, surl->u_hostname, len);
	src[len] = '\0';

	if ((rv = nni_aio_alloc(&aio, NULL, NULL)) != 0) {
		nni_free(src, len + 1);
		return (rv);
	}

	nni_resolv_ip(src, "0", af, true, sa, aio);
	nni_aio_wait(aio);
	rv = nni_aio_result(aio);
	nni_aio_free(aio);
	nni_free(src, len + 1);
	return (rv);
}

static void
mqtts_tcptran_timer_cb(void *arg)
{
	mqtts_tcptran_ep *ep = arg;
	if (nni_aio_result(ep->timeaio) == 0) {
		nng_stream_listener_accept(ep->listener, ep->connaio);
	}
}

static void
mqtts_tcptran_accept_cb(void *arg)
{
	mqtts_tcptran_ep *  ep  = arg;
	nni_aio *           aio = ep->connaio;
	mqtts_tcptran_pipe *p;
	int                 rv;
	nng_stream *        conn;

	nni_mtx_lock(&ep->mtx);

	if ((rv = nni_aio_result(aio)) != 0) {
		log_info("aio result %s", nng_strerror(rv));
		goto error;
	}

	conn = nni_aio_get_output(aio, 0);
	if ((rv = mqtts_tcptran_pipe_alloc(&p)) != 0) {
		nng_stream_free(conn);
		goto error;
	}

	if (ep->closed) {
		mqtts_tcptran_pipe_fini(p);
		nng_stream_free(conn);
		rv = NNG_ECLOSED;
		goto error;
	}
	mqtts_tcptran_pipe_start(p, conn, ep);
	nng_stream_listener_accept(ep->listener, ep->connaio);
	nni_mtx_unlock(&ep->mtx);
	return;

error:
	// When an error here occurs, let's send a notice up to the
	// consumer. That way it can be reported properly.
	if ((aio = ep->useraio) != NULL) {
		ep->useraio = NULL;
		nni_aio_finish_error(aio, rv);
	}
	switch (rv) {

	case NNG_ENOMEM:
	case NNG_ENOFILES:
		nng_sleep_aio(10, ep->timeaio);
		break;

	default:
		if (!ep->closed) {
			nng_stream_listener_accept(ep->listener, ep->connaio);
		}
		break;
	}
	nni_mtx_unlock(&ep->mtx);
}

static void
mqtts_tcptran_dial_cb(void *arg)
{
	mqtts_tcptran_ep *  ep  = arg;
	nni_aio *           aio = ep->connaio;
	mqtts_tcptran_pipe *p;
	int                 rv;
	nng_stream *        conn;

	if ((rv = nni_aio_result(aio)) != 0) {
		log_info("aio result %s", nng_strerror(rv));
		goto error;
	}

	conn = nni_aio_get_output(aio, 0);
	if ((rv = mqtts_tcptran_pipe_alloc(&p)) != 0) {
		nng_stream_free(conn);
		goto error;
	}
	nni_mtx_lock(&ep->mtx);
	if (ep->closed) {
		mqtts_tcptran_pipe_fini(p);
		nng_stream_free(conn);
		rv = NNG_ECLOSED;
		nni_mtx_unlock(&ep->mtx);
		goto error;
	} else {
		mqtts_tcptran_pipe_start(p, conn, ep);
	}
	nni_mtx_unlock(&ep->mtx);
	return;

error:
	// Error connecting.  We need to pass this straight back
	// to the user.
	nni_mtx_lock(&ep->mtx);
	if ((aio = ep->useraio) != NULL) {
		ep->useraio = NULL;
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ep->mtx);
}

static int
mqtts_tcptran_ep_init(mqtts_tcptran_ep **epp, nng_url *url, nni_sock *sock)
{
	mqtts_tcptran_ep *ep;

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&ep->mtx);
	NNI_LIST_INIT(&ep->busypipes, mqtts_tcptran_pipe, node);
	NNI_LIST_INIT(&ep->waitpipes, mqtts_tcptran_pipe, node);
	NNI_LIST_INIT(&ep->negopipes, mqtts_tcptran_pipe, node);

	ep->proto       = nni_sock_proto_id(sock);
	ep->nsock       = sock;
	ep->url         = url;
	ep->connmsg     = NULL;
	ep->reason_code = 0;
	ep->property    = NULL;
	ep->backoff     = 0;

	*epp = ep;
	return (0);
}

static int
mqtts_tcptran_dialer_init(void **dp, nng_url *url, nni_dialer *ndialer)
{
	mqtts_tcptran_ep *ep;
	int               rv;
	nng_sockaddr      srcsa;
	nni_sock *        sock = nni_dialer_sock(ndialer);
	nng_url           myurl;

	// Check for invalid URL components. only one dialer is allowed
	if ((strlen(url->u_path) != 0) && (strcmp(url->u_path, "/") != 0)) {
		return (NNG_EADDRINVAL);
	}
	if ((url->u_fragment != NULL) || (url->u_userinfo != NULL) ||
	    (url->u_query != NULL) || (strlen(url->u_hostname) == 0) ||
	    (strlen(url->u_port) == 0)) {
		return (NNG_EADDRINVAL);
	}

	if ((rv = mqtts_tcptran_url_parse_source(&myurl, &srcsa, url)) != 0) {
		return (rv);
	}

	if ((rv = mqtts_tcptran_ep_init(&ep, url, sock)) != 0) {
		return (rv);
	}
	ep->ndialer  = ndialer;
	ep->authmode = NNG_TLS_AUTH_MODE_REQUIRED;
	ep->enable_scram = false;
#ifdef SUPP_SCRAM
	ep->scram_ctx    = NULL;
	ep->authmsg      = NULL;
#endif

	if ((rv != 0) ||
	    ((rv = nni_aio_alloc(&ep->connaio, mqtts_tcptran_dial_cb, ep)) !=
	        0) ||
	    ((rv = nng_stream_dialer_alloc_url(&ep->dialer, &myurl)) != 0)) {
		mqtts_tcptran_ep_fini(ep);
		return (rv);
	}
	if ((srcsa.s_family != NNG_AF_UNSPEC) &&
	    ((rv = nni_stream_dialer_set(ep->dialer, NNG_OPT_LOCADDR, &srcsa,
	          sizeof(srcsa), NNI_TYPE_SOCKADDR)) != 0)) {
		mqtts_tcptran_ep_fini(ep);
		return (rv);
	}
	*dp = ep;
	return (0);
}

static int
mqtts_tcptran_listener_init(void **lp, nng_url *url, nni_listener *nlistener)
{
	mqtts_tcptran_ep *ep;
	uint16_t          af;
	char *            host = url->u_hostname;
	nni_aio *         aio;
	int               rv;
	nni_sock *        sock = nni_listener_sock(nlistener);

	if (strcmp(url->u_scheme, "tls+tcp") == 0) {
		af = NNG_AF_UNSPEC;
	} else if (strcmp(url->u_scheme, "tls+tcp4") == 0) {
		af = NNG_AF_INET;
	} else if (strcmp(url->u_scheme, "tls+tcp6") == 0) {
		af = NNG_AF_INET6;
	} else {
		return (NNG_EADDRINVAL);
	}

	// Check for invalid URL components.
	if ((strlen(url->u_path) != 0) && (strcmp(url->u_path, "/") != 0)) {
		return (NNG_EADDRINVAL);
	}
	if ((url->u_fragment != NULL) || (url->u_userinfo != NULL) ||
	    (url->u_query != NULL)) {
		return (NNG_EADDRINVAL);
	}

	if (((rv = mqtts_tcptran_ep_init(&ep, url, sock)) != 0) ||
	    ((rv = nni_aio_alloc(&ep->connaio, mqtts_tcptran_accept_cb, ep)) !=
	        0) ||
	    ((rv = nni_aio_alloc(&ep->timeaio, mqtts_tcptran_timer_cb, ep)) !=
	        0)) {
		return (rv);
	}

	ep->authmode = NNG_TLS_AUTH_MODE_NONE;

	if (strlen(host) == 0) {
		host = NULL;
	}

	// XXX: We are doing lookup at listener initialization.  There
	// is a valid argument that this should be done at bind time,
	// but that would require making bind asynchronous.  In some
	// ways this would be worse than the cost of just waiting here.
	// We always recommend using local IP addresses rather than
	// names when possible.

	if ((rv = nni_aio_alloc(&aio, NULL, NULL)) != 0) {
		mqtts_tcptran_ep_fini(ep);
		return (rv);
	}
	nni_resolv_ip(host, url->u_port, af, true, &ep->sa, aio);
	nni_aio_wait(aio);
	rv = nni_aio_result(aio);
	nni_aio_free(aio);

	if (((rv = nng_stream_listener_alloc_url(&ep->listener, url)) != 0) ||
	    ((rv = nni_stream_listener_set(ep->listener, NNG_OPT_TLS_AUTH_MODE,
	          &ep->authmode, sizeof(ep->authmode), NNI_TYPE_INT32)) !=
	        0)) {
		mqtts_tcptran_ep_fini(ep);
		return (rv);
	}

	*lp = ep;
	return (0);
}

static void
mqtts_tcptran_ep_cancel(nni_aio *aio, void *arg, int rv)
{
	mqtts_tcptran_ep *ep = arg;
	nni_mtx_lock(&ep->mtx);
	if (ep->useraio == aio) {
		ep->useraio = NULL;
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ep->mtx);
}

static void
mqtts_tcptran_ep_connect(void *arg, nni_aio *aio)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	if (ep->closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (ep->backoff != 0) {
		ep->backoff = ep->backoff * 2;
		ep->backoff = ep->backoff > ep->backoff_max
		    ? (nni_duration) (nni_random() % 1000)
		    : ep->backoff;
		log_warn("reconnect to %s in %ld", ep->url->u_host, ep->backoff);
		nni_msleep(ep->backoff);
	} else {
		ep->backoff = nni_random()%2000;
	}
	nni_mtx_lock(&ep->mtx);
	if (ep->closed) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (ep->useraio != NULL) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_EBUSY);
		return;
	}
	if ((rv = nni_aio_schedule(aio, mqtts_tcptran_ep_cancel, ep)) != 0) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	ep->useraio = aio;

	nng_stream_dialer_dial(ep->dialer, ep->connaio);
	nni_mtx_unlock(&ep->mtx);
}

static int
mqtts_tcptran_ep_get_url(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtts_tcptran_ep *ep = arg;
	char *            s;
	int               rv;
	int               port = 0;

	if (ep->listener != NULL) {
		(void) nng_stream_listener_get_int(
		    ep->listener, NNG_OPT_TCP_BOUND_PORT, &port);
	}

	if ((rv = nni_url_asprintf_port(&s, ep->url, port)) == 0) {
		rv = nni_copyout_str(s, v, szp, t);
		nni_strfree(s);
	}
	return (rv);
}

static int
mqtts_tcptran_ep_get_reasoncode(void *arg, void *v, size_t *sz, nni_opt_type t)
{
	NNI_ARG_UNUSED(sz);
	mqtts_tcptran_ep *ep = arg;
	int              rv;

	nni_mtx_lock(&ep->mtx);
	rv = nni_copyin_int(v, &ep->reason_code, sizeof(ep->reason_code), 0, 256, t);
	nni_mtx_unlock(&ep->mtx);
	return (rv);
}

static int
mqtts_tcptran_ep_get_connmsg(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	rv = nni_copyout_ptr(ep->connmsg, v, szp, t);

	return (rv);
}

static int
mqtts_tcptran_ep_get_property(void *arg, void *v, size_t *szp, nni_opt_type t)
{
	mqtts_tcptran_ep *ep = arg;
	int              rv;

	rv = nni_copyout_ptr(ep->property, v, szp, t);
	return (rv);
}

static int
mqtts_tcptran_ep_set_connmsg(
    void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtts_tcptran_ep *ep = arg;
	int              rv;

	nni_mtx_lock(&ep->mtx);
	rv = nni_copyin_ptr(&ep->connmsg, v, sz, t);
	nni_mtx_unlock(&ep->mtx);

	return (rv);
}

// NanoSDK use exponential backoff strategy as default
// Backoff for random time that exponentially curving
static int
mqtts_tcptran_ep_set_reconnect_backoff(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtts_tcptran_ep *ep = arg;
	nni_duration      tmp;
	int rv;

	// max backoff time cannot exceed 10min

	if ((rv = nni_copyin_ms(&tmp, v, sz, t)) == 0) {
		nni_mtx_lock(&ep->mtx);
		ep->backoff_max = tmp > 600000 ? 360000 : tmp;
		nni_mtx_unlock(&ep->mtx);
	}
	return (rv);
}

static int
mqtts_tcptran_ep_set_ep_closed(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtts_tcptran_ep *ep = arg;
	bool              tmp;
	int               rv;

	if ((rv = nni_copyin_bool(&tmp, v, sz, t)) == 0) {
		nni_mtx_lock(&ep->mtx);
		ep->closed = tmp;
		if (tmp == true) {
			mqtts_tcptran_pipe *p;
			NNI_LIST_FOREACH (&ep->busypipes, p) {
				nni_pipe_close(p->npipe);
			}
		}
		nni_mtx_unlock(&ep->mtx);
	}
	return (rv);
}

static int
mqtts_tcptran_ep_set_enable_scram(void *arg, const void *v, size_t sz, nni_opt_type t)
{
	mqtts_tcptran_ep *ep = arg;
	bool             tmp;
	int              rv;

	if ((rv = nni_copyin_bool(&tmp, v, sz, t)) == 0) {
		nni_mtx_lock(&ep->mtx);
		ep->enable_scram = tmp;
#ifdef SUPP_SCRAM
		log_info("Auth SCRAM status: %s", tmp == 1 ? "Enabled":"Disabled");
#else
		log_warn("Auth SCRAM Error. Try to compile with NNG_ENABLE_SCRAM");
#endif
		nni_mtx_unlock(&ep->mtx);
	}
	return (rv);
}

static int
mqtts_tcptran_ep_bind(void *arg)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	nni_mtx_lock(&ep->mtx);
	rv = nng_stream_listener_listen(ep->listener);
	nni_mtx_unlock(&ep->mtx);

	return (rv);
}

static void
mqtts_tcptran_ep_accept(void *arg, nni_aio *aio)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	nni_mtx_lock(&ep->mtx);
	if (ep->closed) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	if (ep->useraio != NULL) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, NNG_EBUSY);
		return;
	}
	if ((rv = nni_aio_schedule(aio, mqtts_tcptran_ep_cancel, ep)) != 0) {
		nni_mtx_unlock(&ep->mtx);
		nni_aio_finish_error(aio, rv);
		return;
	}
	ep->useraio = aio;
	if (!ep->started) {
		ep->started = true;
		nng_stream_listener_accept(ep->listener, ep->connaio);
	} else {
		mqtts_tcptran_ep_match(ep);
	}
	nni_mtx_unlock(&ep->mtx);
}

static nni_sp_pipe_ops mqtts_tcptran_pipe_ops = {
	.p_init   = mqtts_tcptran_pipe_init,
	.p_fini   = mqtts_tcptran_pipe_fini,
	.p_stop   = mqtts_tcptran_pipe_stop,
	.p_send   = mqtts_tcptran_pipe_send,
	.p_recv   = mqtts_tcptran_pipe_recv,
	.p_close  = mqtts_tcptran_pipe_close,
	.p_peer   = mqtts_tcptran_pipe_peer,
	.p_getopt = mqtts_tcptran_pipe_getopt,
};

static const nni_option mqtts_tcptran_ep_opts[] = {
	{
	    .o_name = NNG_OPT_MQTT_CONNECT_REASON,
	    .o_get  = mqtts_tcptran_ep_get_reasoncode,
	},
	{
	    .o_name = NNG_OPT_MQTT_CONNECT_PROPERTY,
	    .o_get  = mqtts_tcptran_ep_get_property,
	},
	{
	    .o_name = NNG_OPT_MQTT_CONNMSG,
	    .o_get  = mqtts_tcptran_ep_get_connmsg,
	    .o_set  = mqtts_tcptran_ep_set_connmsg,
	},
	{
	    .o_name = NNG_OPT_MQTT_RECONNECT_BACKOFF_MAX,
	    .o_set  = mqtts_tcptran_ep_set_reconnect_backoff,
	},
	{
	    .o_name = NNG_OPT_URL,
	    .o_get  = mqtts_tcptran_ep_get_url,
	},
	{
	    .o_name = NNG_OPT_BRIDGE_SET_EP_CLOSED,
	    .o_set  = mqtts_tcptran_ep_set_ep_closed,
	},
	{
	    .o_name = NNG_OPT_MQTT_ENABLE_SCRAM,
	    .o_set  = mqtts_tcptran_ep_set_enable_scram,
	},
	// terminate list
	{
	    .o_name = NULL,
	},
};

static int
mqtts_tcptran_dialer_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	rv = nni_stream_dialer_get(ep->dialer, name, buf, szp, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_getopt(mqtts_tcptran_ep_opts, name, ep, buf, szp, t);
	}
	return (rv);
}

static int
mqtts_tcptran_dialer_setopt(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	// TODO get mqtts dialer's option
	rv = nni_stream_dialer_set(ep->dialer, name, buf, sz, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_setopt(mqtts_tcptran_ep_opts, name, ep, buf, sz, t);
	}
	return (rv);
}

static int
mqtts_tcptran_listener_getopt(
    void *arg, const char *name, void *buf, size_t *szp, nni_type t)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	rv = nni_stream_listener_get(ep->listener, name, buf, szp, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_getopt(mqtts_tcptran_ep_opts, name, ep, buf, szp, t);
	}
	return (rv);
}

static int
mqtts_tcptran_listener_setopt(
    void *arg, const char *name, const void *buf, size_t sz, nni_type t)
{
	mqtts_tcptran_ep *ep = arg;
	int               rv;

	rv = nni_stream_listener_set(ep->listener, name, buf, sz, t);
	if (rv == NNG_ENOTSUP) {
		rv = nni_setopt(mqtts_tcptran_ep_opts, name, ep, buf, sz, t);
	}
	return (rv);
}

static nni_sp_dialer_ops mqtts_tcptran_dialer_ops = {
	.d_init    = mqtts_tcptran_dialer_init,
	.d_fini    = mqtts_tcptran_ep_fini,
	.d_connect = mqtts_tcptran_ep_connect,
	.d_close   = mqtts_tcptran_ep_close,
	.d_getopt  = mqtts_tcptran_dialer_getopt,
	.d_setopt  = mqtts_tcptran_dialer_setopt,
};

static nni_sp_listener_ops mqtts_tcptran_listener_ops = {
	.l_init   = mqtts_tcptran_listener_init,
	.l_fini   = mqtts_tcptran_ep_fini,
	.l_bind   = mqtts_tcptran_ep_bind,
	.l_accept = mqtts_tcptran_ep_accept,
	.l_close  = mqtts_tcptran_ep_close,
	.l_getopt = mqtts_tcptran_listener_getopt,
	.l_setopt = mqtts_tcptran_listener_setopt,
};

static nni_sp_tran mqtts_tcp_tran = {
	.tran_scheme   = "tls+mqtt-tcp",
	.tran_dialer   = &mqtts_tcptran_dialer_ops,
	.tran_listener = &mqtts_tcptran_listener_ops,
	.tran_pipe     = &mqtts_tcptran_pipe_ops,
	.tran_init     = mqtts_tcptran_init,
	.tran_fini     = mqtts_tcptran_fini,
};

static nni_sp_tran mqtts_tcp4_tran = {
	.tran_scheme   = "tls+mqtt-tcp4",
	.tran_dialer   = &mqtts_tcptran_dialer_ops,
	.tran_listener = &mqtts_tcptran_listener_ops,
	.tran_pipe     = &mqtts_tcptran_pipe_ops,
	.tran_init     = mqtts_tcptran_init,
	.tran_fini     = mqtts_tcptran_fini,
};

static nni_sp_tran mqtts_tcp6_tran = {
	.tran_scheme   = "tls+mqtt-tcp6",
	.tran_dialer   = &mqtts_tcptran_dialer_ops,
	.tran_listener = &mqtts_tcptran_listener_ops,
	.tran_pipe     = &mqtts_tcptran_pipe_ops,
	.tran_init     = mqtts_tcptran_init,
	.tran_fini     = mqtts_tcptran_fini,
};

#ifndef NNG_ELIDE_DEPRECATED
int
nng_mqtts_tcp_register(void)
{
	return (nni_init());
}
#endif

void
nni_mqtts_tcp_register(void)
{
	nni_mqtt_tran_register(&mqtts_tcp_tran);
	nni_mqtt_tran_register(&mqtts_tcp4_tran);
	nni_mqtt_tran_register(&mqtts_tcp6_tran);
}
