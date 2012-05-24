/*-
 * Copyright (C) 2012 Michael Tuexen
 * Copyright (C) 2012 Irene Ruengeler
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: rtcweb.c,v 1.2 2012-05-24 07:46:14 tuexen Exp $
 */

/*
 * gcc -Wall -std=c99 -pedantic -o rtcweb rtcweb.c -lusrsctp
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <usrsctp.h>

#define LINE_LENGTH (1024)
#define BUFFER_SIZE (1<<16)
#define NUMBER_OF_CHANNELS (100)
#define NUMBER_OF_STREAMS (100)

#define DATA_CHANNEL_PPID_CONTROL   50
#define DATA_CHANNEL_PPID_DOMSTRING 51
#define DATA_CHANNEL_PPID_BINARY    52

#define DATA_CHANNEL_CLOSED     0
#define DATA_CHANNEL_CONNECTING 1
#define DATA_CHANNEL_OPEN       2
#define DATA_CHANNEL_CLOSING    3

struct channel {
	uint32_t id;
	uint32_t pr_value;
	uint16_t pr_policy;
	uint16_t i_stream;
	uint16_t o_stream;
	uint8_t unordered;
	uint8_t state;
};

struct peer_connection {
	struct channel channels[NUMBER_OF_CHANNELS];
	struct channel *i_stream_channel[NUMBER_OF_STREAMS];
	struct channel *o_stream_channel[NUMBER_OF_STREAMS];
	uint16_t o_stream_buffer[NUMBER_OF_STREAMS];
	uint32_t o_stream_buffer_counter;
	pthread_mutex_t mutex;
	struct socket *sock;
} peer_connection;

#define DATA_CHANNEL_OPEN_REQUEST  0
#define DATA_CHANNEL_OPEN_RESPONSE 1
#define DATA_CHANNEL_ACK           2

#define DATA_CHANNEL_RELIABLE                0
#define DATA_CHANNEL_RELIABLE_STREAM         1
#define DATA_CHANNEL_UNRELIABLE              2
#define DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT 3
#define DATA_CHANNEL_PARTIAL_RELIABLE_TIMED  4

#define DATA_CHANNEL_FLAG_OUT_OF_ORDER_ALLOWED 0x0001

struct rtcweb_datachannel_open_request {
	uint8_t msg_type; /* DATA_CHANNEL_OPEN_REQUEST */
	uint8_t channel_type;
	uint16_t flags;
	uint16_t reliability_params;
	int16_t priority;
	char label[];
}__attribute__((packed));

struct rtcweb_datachannel_open_response {
	uint8_t  msg_type; /* DATA_CHANNEL_OPEN_RESPONSE */
	uint8_t  error;
	uint16_t flags;
	uint16_t reverse_stream;
}__attribute__((packed));

struct rtcweb_datachannel_ack {
	uint8_t  msg_type; /* DATA_CHANNEL_ACK */
}__attribute__((packed));

static void
init_peer_connection(struct peer_connection *pc, struct socket *sock)
{
	uint32_t i;
	struct channel *channel;

	for (i = 0; i < NUMBER_OF_CHANNELS; i++) {
		channel = &(pc->channels[i]);
		channel->id = i;
		channel->state = DATA_CHANNEL_CLOSED;
		channel->pr_policy = SCTP_PR_SCTP_NONE;
		channel->pr_value = 0;
		channel->i_stream = 0;
		channel->o_stream = 0;
		channel->unordered = 0;
	}
	for (i = 0; i < NUMBER_OF_STREAMS; i++) {
		pc->i_stream_channel[i] = NULL;
		pc->o_stream_channel[i] = NULL;
		pc->o_stream_buffer[i] = 0;
	}
	pc->o_stream_buffer_counter = 0;
	pc->sock = sock;
	pthread_mutex_init(&pc->mutex, NULL);
}

static void
lock_peer_connection(struct peer_connection *pc)
{
	pthread_mutex_lock(&pc->mutex);
}

static void
unlock_peer_connection(struct peer_connection *pc)
{
	pthread_mutex_unlock(&pc->mutex);
}

static struct channel *
find_channel_by_i_stream(struct peer_connection *pc, uint16_t i_stream)
{
	if (i_stream < NUMBER_OF_STREAMS) {
		return (pc->i_stream_channel[i_stream]);
	} else {
		return (NULL);
	}
}

static struct channel *
find_channel_by_o_stream(struct peer_connection *pc, uint16_t o_stream)
{
	if (o_stream < NUMBER_OF_STREAMS) {
		return (pc->o_stream_channel[o_stream]);
	} else {
		return (NULL);
	}
}

static struct channel *
find_free_channel(struct peer_connection *pc)
{
	uint32_t i;

	for (i = 0; i < NUMBER_OF_CHANNELS; i++) {
		if (pc->channels[i].state == DATA_CHANNEL_CLOSED) {
			break;
		}
	}
	if (i == NUMBER_OF_CHANNELS) {
		return (NULL);
	} else {
		return (&(pc->channels[i]));
	}
}

static uint16_t
find_free_o_stream(struct peer_connection *pc)
{
	struct sctp_status status;
	uint32_t i, limit;
	socklen_t len;

	len = (socklen_t)sizeof(struct sctp_status);
	if (usrsctp_getsockopt(pc->sock, IPPROTO_SCTP, SCTP_STATUS, &status, &len) < 0) {
		perror("getsockopt");
		return (0);
	}
	if (status.sstat_outstrms < NUMBER_OF_STREAMS) {
		limit = status.sstat_outstrms;
	} else {
		limit = NUMBER_OF_STREAMS;
	}
	/* stream id 0 is reserved */
	for (i = 1; i < limit; i++) {
		if (pc->o_stream_channel[i] == NULL) {
			break;
		}
	}
	if (i == limit) {
		return (0);
	} else {
		return ((uint16_t)i);
	}
}

static void
request_more_o_streams(struct peer_connection *pc)
{
	struct sctp_status status;
	struct sctp_add_streams sas;
	uint32_t i, o_streams_needed;
	socklen_t len;

	o_streams_needed = 0;
	for (i = 0; i < NUMBER_OF_CHANNELS; i++) {
		if ((pc->channels[i].state == DATA_CHANNEL_CONNECTING) &&
		    (pc->channels[i].o_stream == 0)) {
			o_streams_needed++;
		}
	}
	len = (socklen_t)sizeof(struct sctp_status);
	if (usrsctp_getsockopt(pc->sock, IPPROTO_SCTP, SCTP_STATUS, &status, &len) < 0) {
		perror("getsockopt");
		return;
	}
	if (status.sstat_outstrms + o_streams_needed > NUMBER_OF_STREAMS) {
		o_streams_needed = NUMBER_OF_STREAMS - status.sstat_outstrms;
	}
	if (o_streams_needed == 0) {
		return;
	}
	memset(&sas, 0, sizeof(struct sctp_add_streams));
	sas.sas_instrms = 0;
	sas.sas_outstrms = (uint16_t)o_streams_needed; /* XXX eror handling */
	if (usrsctp_setsockopt(pc->sock, IPPROTO_SCTP, SCTP_ADD_STREAMS, &sas, (socklen_t)sizeof(struct sctp_add_streams)) < 0) {
		perror("setsockopt");
	}
	return;

}

static int
send_open_request_message(struct socket *sock, uint16_t o_stream, uint8_t unordered, uint16_t pr_policy, uint32_t pr_value)
{
	/* XXX: This should be encoded in a better way */
	struct rtcweb_datachannel_open_request req;
	struct sctp_sndinfo sndinfo;
	char buffer[BUFFER_SIZE];

	memset(&req, 0, sizeof(struct rtcweb_datachannel_open_request));
	req.msg_type = DATA_CHANNEL_OPEN_REQUEST;
	switch (pr_policy) {
	case SCTP_PR_SCTP_NONE:
		/* XXX: What about DATA_CHANNEL_RELIABLE_STREAM */
		req.channel_type = DATA_CHANNEL_RELIABLE;
		break;
	case SCTP_PR_SCTP_TTL:
		/* XXX: What about DATA_CHANNEL_UNRELIABLE */
		req.channel_type = DATA_CHANNEL_PARTIAL_RELIABLE_TIMED;
		break;
	case SCTP_PR_SCTP_RTX:
		req.channel_type = DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT;
		break;
	default:
		return (0);
	}
	req.flags = htons(0);
	if (unordered) {
		req.flags |= htons(DATA_CHANNEL_FLAG_OUT_OF_ORDER_ALLOWED);
	}
	req.reliability_params = htons((uint16_t)pr_value); /* XXX Why 16-bit */
	req.priority = htons(0); /* XXX: add support */
	memcpy(buffer, &req, sizeof(struct rtcweb_datachannel_open_request));
	memset(&sndinfo, 0, sizeof(struct sctp_sndinfo));
	sndinfo.snd_sid = o_stream;
	sndinfo.snd_flags = SCTP_EOR;
	sndinfo.snd_ppid = htonl(DATA_CHANNEL_PPID_CONTROL);
	if (usrsctp_sendv(sock,
	               buffer, sizeof(struct rtcweb_datachannel_open_request),
	               NULL, 0,
	               &sndinfo, (socklen_t)sizeof(struct sctp_sndinfo),
	               SCTP_SENDV_SNDINFO, 0) < 0) {
		perror("sctp_sendv");
		return (0);
	} else {
		return (1);
	}
}

static int
send_open_response_message(struct socket *sock, uint16_t o_stream, uint16_t i_stream)
{
	/* XXX: This should be encoded in a better way */
	struct rtcweb_datachannel_open_response rsp;
	struct sctp_sndinfo sndinfo;
	char buffer[BUFFER_SIZE];

	memset(&rsp, 0, sizeof(struct rtcweb_datachannel_open_response));
	rsp.msg_type = DATA_CHANNEL_OPEN_RESPONSE;
	rsp.error = 0;
	rsp.flags = htons(0);
	rsp.reverse_stream = htons(i_stream);
	memcpy(buffer, &rsp, sizeof(struct rtcweb_datachannel_open_response));
	memset(&sndinfo, 0, sizeof(struct sctp_sndinfo));
	sndinfo.snd_sid = o_stream;
	sndinfo.snd_flags = SCTP_EOR;
	sndinfo.snd_ppid = htonl(DATA_CHANNEL_PPID_CONTROL);
	if (usrsctp_sendv(sock,
	               buffer, sizeof(struct rtcweb_datachannel_open_response),
	               NULL, 0,
	               &sndinfo, (socklen_t)sizeof(struct sctp_sndinfo),
	               SCTP_SENDV_SNDINFO, 0) < 0) {
	        perror("sctp_sendv");
		return (0);
	} else {
		return (1);
	}
}

static int
send_open_ack_message(struct socket *sock, uint16_t o_stream)
{
	/* XXX: This should be encoded in a better way */
	struct rtcweb_datachannel_ack ack;
	struct sctp_sndinfo sndinfo;
	char buffer[BUFFER_SIZE];

	memset(&ack, 0, sizeof(struct rtcweb_datachannel_ack));
	ack.msg_type = DATA_CHANNEL_ACK;
	memcpy(buffer, &ack, sizeof(struct rtcweb_datachannel_ack));
	memset(&sndinfo, 0, sizeof(struct sctp_sndinfo));
	sndinfo.snd_sid = o_stream;
	sndinfo.snd_flags = SCTP_EOR;
	sndinfo.snd_ppid = htonl(DATA_CHANNEL_PPID_CONTROL);
	if (usrsctp_sendv(sock,
	               buffer, sizeof(struct rtcweb_datachannel_ack),
	               NULL, 0,
	               &sndinfo, (socklen_t)sizeof(struct sctp_sndinfo),
	               SCTP_SENDV_SNDINFO, 0) < 0) {
	        perror("sctp_sendv");
		return (0);
	} else {
		return (1);
	}
}

static struct channel *
open_channel(struct peer_connection *pc, uint8_t unordered, uint16_t pr_policy, uint32_t pr_value)
{
	struct channel *channel;
	uint16_t o_stream;

	if ((pr_policy != SCTP_PR_SCTP_NONE) &&
	    (pr_policy != SCTP_PR_SCTP_TTL) &&
	    (pr_policy != SCTP_PR_SCTP_RTX)) {
		return (NULL);
	}
	if ((unordered != 0) && (unordered != 1)) {
		return (NULL);
	}
	if ((pr_policy == SCTP_PR_SCTP_NONE) && (pr_value != 0)) {
		return (NULL);
	}
	if ((channel = find_free_channel(pc)) == NULL) {
		return (NULL);
	}
	o_stream = find_free_o_stream(pc);
	if ((o_stream == 0) ||
	    (send_open_request_message(pc->sock, o_stream, unordered, pr_policy, pr_value))) {
		channel->state = DATA_CHANNEL_CONNECTING;
		channel->unordered = unordered;
		channel->pr_policy = pr_policy;
		channel->pr_value = pr_value;
		channel->o_stream = o_stream;
		if (o_stream != 0) {
			pc->o_stream_channel[o_stream] = channel;
		} else {
			request_more_o_streams(pc);
		}
		return (channel);
	} else {
		return (NULL);
	}
}

static int
send_user_message(struct peer_connection *pc, struct channel *channel, char *message, size_t length)
{
	struct sctp_sendv_spa spa;

	if (channel == NULL) {
		return (0);
	}
	if ((channel->state != DATA_CHANNEL_OPEN) &&
	    (channel->state != DATA_CHANNEL_CONNECTING)) {
		/* XXX: What to do in other states */
		return (0);
	}

	memset(&spa, 0, sizeof(struct sctp_sendv_spa));
	spa.sendv_sndinfo.snd_sid = channel->o_stream;
	if ((channel->state == DATA_CHANNEL_OPEN) &&
	    (channel->unordered)) {
		spa.sendv_sndinfo.snd_flags = SCTP_EOR | SCTP_UNORDERED;
	} else {
		spa.sendv_sndinfo.snd_flags = SCTP_EOR;
	}
	spa.sendv_sndinfo.snd_ppid = htonl(DATA_CHANNEL_PPID_DOMSTRING);
	spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
	if ((channel->pr_policy == SCTP_PR_SCTP_TTL) ||
	    (channel->pr_policy == SCTP_PR_SCTP_RTX)) {
		spa.sendv_prinfo.pr_policy = channel->pr_policy;
		spa.sendv_prinfo.pr_value = channel->pr_value;
		spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
	}
	if (usrsctp_sendv(pc->sock,
	               message, length,
	               NULL, 0,
	               &spa, (socklen_t)sizeof(struct sctp_sendv_spa),
	               SCTP_SENDV_SPA, 0) < 0) {
	        perror("sctp_sendv");
		return (0);
	} else {
		return (1);
	}
}

static void
reset_outgoing_stream(struct peer_connection *pc, uint16_t o_stream)
{
	uint32_t i;

	for (i = 0; i < pc->o_stream_buffer_counter; i++) {
		if (pc->o_stream_buffer[i] == o_stream) {
			return;
		}
	}
	pc->o_stream_buffer[pc->o_stream_buffer_counter++] = o_stream;
	return;
}

static void
send_outgoing_stream_reset(struct peer_connection *pc)
{
	struct sctp_reset_streams *srs;
	uint32_t i;
	size_t len;

	if (pc->o_stream_buffer_counter == 0) {
		return;
	}
	len = sizeof(sctp_assoc_t) + (2 + pc->o_stream_buffer_counter) * sizeof(uint16_t);
	srs = (struct sctp_reset_streams *)malloc(len);
	if (srs == NULL) {
		return;
	}
	memset(srs, 0, len);
	srs->srs_flags = SCTP_STREAM_RESET_OUTGOING;
	srs->srs_number_streams = pc->o_stream_buffer_counter;
	for (i = 0; i < pc->o_stream_buffer_counter; i++) {
		srs->srs_stream_list[i] = pc->o_stream_buffer[i];
	}
	if (usrsctp_setsockopt(pc->sock, IPPROTO_SCTP, SCTP_RESET_STREAMS, srs, (socklen_t)len) < 0) {
		perror("setsockopt");
	} else {
		for (i = 0; i < pc->o_stream_buffer_counter; i++) {
			srs->srs_stream_list[i] = 0;
		}
		pc->o_stream_buffer_counter = 0;
	}
	free(srs);
	return;
}

static void
close_channel(struct peer_connection *pc, struct channel *channel)
{
	if (channel == NULL) {
		return;
	}
	if (channel->state != DATA_CHANNEL_OPEN) {
		return;
	}
	reset_outgoing_stream(pc, channel->o_stream);
	send_outgoing_stream_reset(pc);
	channel->state = DATA_CHANNEL_CLOSING;
	return;
}

static void
handle_open_request_message(struct peer_connection *pc,
                            struct rtcweb_datachannel_open_request *req,
                            size_t length,
                            uint16_t i_stream)
{
	struct channel *channel;
	uint32_t pr_value;
	uint16_t pr_policy;
	uint16_t o_stream;
	uint8_t unordered;

	if ((channel = find_channel_by_i_stream(pc, i_stream))) {
		printf("Hmm, channel %d is in state %d instead of CLOSED.\n",
		       channel->id, channel->state);
		return;
		/* XXX: some error handling */
	}
	if ((channel = find_free_channel(pc)) == NULL) {
		/* XXX: some error handling */
		return;
	}
	switch (req->channel_type) {
	case DATA_CHANNEL_RELIABLE:
		pr_policy = SCTP_PR_SCTP_NONE;
		break;
	/* XXX Doesn't make sense */
	case DATA_CHANNEL_RELIABLE_STREAM:
		pr_policy = SCTP_PR_SCTP_NONE;
		break;
	/* XXX Doesn't make sense */
	case DATA_CHANNEL_UNRELIABLE:
		pr_policy = SCTP_PR_SCTP_TTL;
		break;
	case DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT:
		pr_policy = SCTP_PR_SCTP_RTX;
		break;
	case DATA_CHANNEL_PARTIAL_RELIABLE_TIMED:
		pr_policy = SCTP_PR_SCTP_TTL;
		break;
	default:
		/* XXX error handling */
		break;
	}
	pr_value = ntohs(req->reliability_params);
	if (ntohs(req->flags) & DATA_CHANNEL_FLAG_OUT_OF_ORDER_ALLOWED) {
		unordered = 1;
	} else {
		unordered = 0;
	}
	o_stream = find_free_o_stream(pc);
	if ((o_stream == 0) || send_open_response_message(pc->sock, o_stream, i_stream)) {
		channel->state = DATA_CHANNEL_CONNECTING;
		channel->unordered = unordered;
		channel->pr_policy = pr_policy;
		channel->pr_value = pr_value;
		channel->i_stream = i_stream;
		pc->i_stream_channel[i_stream] = channel;
		if (o_stream != 0) {
			channel->o_stream = o_stream;
			pc->o_stream_channel[o_stream] = channel;
		} else {
			request_more_o_streams(pc);
		}
	} else {
		/* error handling */
	}
	return;
}

static void
handle_open_response_message(struct peer_connection *pc,
                             struct rtcweb_datachannel_open_response *rsp,
                             size_t length, uint16_t i_stream)
{
	uint16_t o_stream;
	struct channel *channel;

	o_stream = ntohs(rsp->reverse_stream);
	channel = find_channel_by_o_stream(pc, o_stream);
	if (channel == NULL) {
		/* XXX: some error handling */
	}
	if (channel->state != DATA_CHANNEL_CONNECTING) {
		/* XXX: some error handling */
	}
	if (find_channel_by_i_stream(pc, i_stream)) {
		/* XXX: some error handling */
	}
	channel->i_stream = i_stream;
	channel->state = DATA_CHANNEL_OPEN;
	pc->i_stream_channel[i_stream] = channel;
	send_open_ack_message(pc->sock, o_stream);
	return;
}

static void
handle_open_ack_message(struct peer_connection *pc,
                        struct rtcweb_datachannel_ack *ack,
                        size_t length, uint16_t i_stream)
{
	struct channel *channel;

	channel = find_channel_by_i_stream(pc, i_stream);
	if (channel == NULL) {
		/* XXX: some error handling */
	}
	if (channel->state == DATA_CHANNEL_OPEN) {
		return;
	}
	if (channel->state != DATA_CHANNEL_CONNECTING) {
		/* XXX: error handling */
		return;
	}
	channel->state = DATA_CHANNEL_OPEN;
	return;
}

static void
handle_unknown_message(char *msg, size_t length, uint16_t i_stream)
{
	/* XXX: Send an error message */
	return;
}

static void
handle_data_message(struct peer_connection *pc,
                    char *buffer, size_t length, uint16_t i_stream)
{
	struct channel *channel;

	channel = find_channel_by_i_stream(pc, i_stream);
	if (channel == NULL) {
		/* XXX: Some error handling */
		return;
	}
	if (channel->state == DATA_CHANNEL_CONNECTING) {
		/* Implicit ACK */
		channel->state = DATA_CHANNEL_OPEN;
	}
	if (channel->state != DATA_CHANNEL_OPEN) {
		/* XXX: What about other states? */
		/* XXX: Some error handling */
		return;
	} else {
		/* Assuming DATA_CHANNEL_PPID_DOMSTRING */
		/* XXX: Protect for non 0 terminated buffer */
		printf("Message received of length %lu on channel with id %d: %.*s\n",
		       length, channel->id, (int)length, buffer);
	}
	return;
}

static void
handle_message(struct peer_connection *pc, char *buffer, size_t length, uint32_t ppid, uint16_t i_stream)
{
	struct rtcweb_datachannel_open_request *req;
	struct rtcweb_datachannel_open_response *rsp;
	struct rtcweb_datachannel_ack *ack, *msg;

	switch (ppid) {
	case DATA_CHANNEL_PPID_CONTROL:
		if (length < sizeof(struct rtcweb_datachannel_ack)) {
			return;
		}
		msg = (struct rtcweb_datachannel_ack *)buffer;
		switch (msg->msg_type) {
		case DATA_CHANNEL_OPEN_REQUEST:
			if (length < sizeof(struct rtcweb_datachannel_open_request)) {
				/* XXX: error handling? */
				return;
			}
			req = (struct rtcweb_datachannel_open_request *)buffer;
			handle_open_request_message(pc, req, length, i_stream);
			break;
		case DATA_CHANNEL_OPEN_RESPONSE:
			if (length < sizeof(struct rtcweb_datachannel_open_response)) {
				/* XXX: error handling? */
				return;
			}
			rsp = (struct rtcweb_datachannel_open_response *)buffer;
			handle_open_response_message(pc, rsp, length, i_stream);
			break;
		case DATA_CHANNEL_ACK:
			if (length < sizeof(struct rtcweb_datachannel_ack)) {
				/* XXX: error handling? */
				return;
			}
			ack = (struct rtcweb_datachannel_ack *)buffer;
			handle_open_ack_message(pc, ack, length, i_stream);
			break;
		default:
			handle_unknown_message(buffer, length, i_stream);
			break;
		}
		break;
	case DATA_CHANNEL_PPID_DOMSTRING:
	case DATA_CHANNEL_PPID_BINARY:
		handle_data_message(pc, buffer, length, i_stream);
		break;
	default:
		printf("Message of length %lu, PPID %u on stream %u received.\n",
		       length, ppid, i_stream);
		break;
	}
}

static void
handle_association_change_event(struct sctp_assoc_change *sac)
{
	unsigned int i, n;

	printf("Association change ");
	switch (sac->sac_state) {
	case SCTP_COMM_UP:
		printf("SCTP_COMM_UP");
		break;
	case SCTP_COMM_LOST:
		printf("SCTP_COMM_LOST");
		break;
	case SCTP_RESTART:
		printf("SCTP_RESTART");
		break;
	case SCTP_SHUTDOWN_COMP:
		printf("SCTP_SHUTDOWN_COMP");
		break;
	case SCTP_CANT_STR_ASSOC:
		printf("SCTP_CANT_STR_ASSOC");
		break;
	default:
		printf("UNKNOWN");
		break;
	}
	printf(", streams (in/out) = (%u/%u)",
	       sac->sac_inbound_streams, sac->sac_outbound_streams);
	n = sac->sac_length - sizeof(struct sctp_assoc_change);
	if (((sac->sac_state == SCTP_COMM_UP) ||
	     (sac->sac_state == SCTP_RESTART)) && (n > 0)) {
		printf(", supports");
		for (i = 0; i < n; i++) {
			switch (sac->sac_info[i]) {
			case SCTP_ASSOC_SUPPORTS_PR:
				printf(" PR");
				break;
			case SCTP_ASSOC_SUPPORTS_AUTH:
				printf(" AUTH");
				break;
			case SCTP_ASSOC_SUPPORTS_ASCONF:
				printf(" ASCONF");
				break;
			case SCTP_ASSOC_SUPPORTS_MULTIBUF:
				printf(" MULTIBUF");
				break;
			case SCTP_ASSOC_SUPPORTS_RE_CONFIG:
				printf(" RE-CONFIG");
				break;
			default:
				printf(" UNKNOWN(0x%02x)", sac->sac_info[i]);
				break;
			}
		}
	} else if (((sac->sac_state == SCTP_COMM_LOST) ||
	            (sac->sac_state == SCTP_CANT_STR_ASSOC)) && (n > 0)) {
		printf(", ABORT =");
		for (i = 0; i < n; i++) {
			printf(" 0x%02x", sac->sac_info[i]);
		}
	}
	printf(".\n");
	return;
}

static void
handle_peer_address_change_event(struct sctp_paddr_change *spc)
{
	char addr_buf[INET6_ADDRSTRLEN];
	const char *addr;
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;

	switch (spc->spc_aaddr.ss_family) {
	case AF_INET:
		sin = (struct sockaddr_in *)&spc->spc_aaddr;
		addr = inet_ntop(AF_INET, &sin->sin_addr, addr_buf, INET6_ADDRSTRLEN);
		break;
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)&spc->spc_aaddr;
		addr = inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf, INET6_ADDRSTRLEN);
		break;
	default:
		break;
	}
	printf("Peer address %s is now ", addr);
	switch (spc->spc_state) {
	case SCTP_ADDR_AVAILABLE:
		printf("SCTP_ADDR_AVAILABLE");
		break;
	case SCTP_ADDR_UNREACHABLE:
		printf("SCTP_ADDR_UNREACHABLE");
		break;
	case SCTP_ADDR_REMOVED:
		printf("SCTP_ADDR_REMOVED");
		break;
	case SCTP_ADDR_ADDED:
		printf("SCTP_ADDR_ADDED");
		break;
	case SCTP_ADDR_MADE_PRIM:
		printf("SCTP_ADDR_MADE_PRIM");
		break;
	case SCTP_ADDR_CONFIRMED:
		printf("SCTP_ADDR_CONFIRMED");
		break;
	default:
		printf("UNKNOWN");
		break;
	}
	printf(" (error = 0x%08x).\n", spc->spc_error);
	return;
}

static void
handle_adaptation_indication(struct sctp_adaptation_event *sai)
{
	printf("Adaptation indication: %x.\n", sai-> sai_adaptation_ind);
	return;
}

static void
handle_shutdown_event(struct sctp_shutdown_event *sse)
{
	printf("Shutdown event.\n");
	/* XXX: notify all channels. */
	return;
}

static void
handle_stream_reset_event(struct peer_connection *pc, struct sctp_stream_reset_event *strrst)
{
	uint32_t n, i;
	struct channel *channel;

	if (!(strrst->strreset_flags & SCTP_STREAM_RESET_DENIED) &&
	    !(strrst->strreset_flags & SCTP_STREAM_RESET_FAILED)) {
		n = (strrst->strreset_length - sizeof(struct sctp_stream_reset_event)) / sizeof(uint16_t);
		for (i = 0; i < n; i++) {
			if (strrst->strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) {
				channel = find_channel_by_i_stream(pc, strrst->strreset_stream_list[i]);
				if (channel != NULL) {
					pc->i_stream_channel[channel->i_stream] = NULL;
					channel->i_stream = 0;
					if (channel->o_stream == 0) {
						channel->pr_policy = SCTP_PR_SCTP_NONE;
						channel->pr_value = 0;
						channel->unordered = 0;
						channel->state = DATA_CHANNEL_CLOSED;
					} else {
						reset_outgoing_stream(pc, channel->o_stream);
						channel->state = DATA_CHANNEL_CLOSING;
					}
				}
			}
			if (strrst->strreset_flags & SCTP_STREAM_RESET_OUTGOING_SSN) {
				channel = find_channel_by_o_stream(pc, strrst->strreset_stream_list[i]);
				if (channel != NULL) {
					pc->o_stream_channel[channel->o_stream] = NULL;
					channel->o_stream = 0;
					if (channel->i_stream == 0) {
						channel->pr_policy = SCTP_PR_SCTP_NONE;
						channel->pr_value = 0;
						channel->unordered = 0;
						channel->state = DATA_CHANNEL_CLOSED;
					}
				}
			}
		}
	}
	return;
}

static void
handle_stream_change_event(struct peer_connection *pc, struct sctp_stream_change_event *strchg)
{
	uint16_t o_stream;
	uint32_t i;
	struct channel *channel;

	for (i = 0; i < NUMBER_OF_CHANNELS; i++) {
		channel = &(pc->channels[i]);
		if ((channel->state == DATA_CHANNEL_CONNECTING) &&
		    (channel->o_stream == 0)) {
			if ((strchg->strchange_flags & SCTP_STREAM_CHANGE_DENIED) ||
			    (strchg->strchange_flags & SCTP_STREAM_CHANGE_FAILED)) {
				channel->state = DATA_CHANNEL_CLOSED;
				channel->unordered = 0;
				channel->pr_policy = SCTP_PR_SCTP_NONE;
				channel->pr_value = 0;
				channel->o_stream = 0;
			} else {
				o_stream = find_free_o_stream(pc);
				if (o_stream != 0) {
					if (channel->i_stream != 0) {
						if (send_open_response_message(pc->sock, o_stream, channel->i_stream)) {
							channel->o_stream = o_stream;
							pc->o_stream_channel[o_stream] = channel;
						} else {
							/* XXX: error handling */
						}
					} else {
						if (send_open_request_message(pc->sock, o_stream, channel->unordered, channel->pr_policy, channel->pr_value)) {
							channel->o_stream = o_stream;
							pc->o_stream_channel[o_stream] = channel;
						} else {
							channel->state = DATA_CHANNEL_CLOSED;
							channel->unordered = 0;
							channel->pr_policy = SCTP_PR_SCTP_NONE;
							channel->pr_value = 0;
							channel->o_stream = 0;
						}
					}
				} else {
					break;
				}
			}
		}
	}
	return;
}

static void
handle_remote_error_event(struct sctp_remote_error *sre)
{
	size_t i, n;

	n = sre->sre_length - sizeof(struct sctp_remote_error);
	printf("Remote Error (error = 0x%04x): ", sre->sre_error);
	for (i = 0; i < n; i++) {
		printf(" 0x%02x", sre-> sre_data[i]);
	}
	printf(".\n");
	return;
}

static void
handle_send_failed(struct sctp_send_failed_event *ssf)
{
	size_t i, n;

	if (ssf->ssfe_flags & SCTP_DATA_UNSENT) {
		printf("Unsent ");
	}
	if (ssf->ssfe_flags & SCTP_DATA_SENT) {
		printf("Sent ");
	}
	if (ssf->ssfe_flags & ~(SCTP_DATA_SENT | SCTP_DATA_UNSENT)) {
		printf("(flags = %x) ", ssf->ssfe_flags);
	}
	printf("message with PPID = %d, SID = %d, flags: 0x%04x due to error = 0x%08x",
	       ntohl(ssf->ssfe_info.snd_ppid), ssf->ssfe_info.snd_sid,
	       ssf->ssfe_info.snd_flags, ssf->ssfe_error);
	n = ssf->ssfe_length - sizeof(struct sctp_send_failed_event);
	for (i = 0; i < n; i++) {
		printf(" 0x%02x", ssf->ssfe_data[i]);
	}
	printf(".\n");
	return;
}

static void
handle_notification(struct peer_connection *pc, union sctp_notification *notif, size_t n)
{
	if (notif->sn_header.sn_length != (uint32_t)n) {
		return;
	}
	switch (notif->sn_header.sn_type) {
	case SCTP_ASSOC_CHANGE:
		handle_association_change_event(&(notif->sn_assoc_change));
		break;
	case SCTP_PEER_ADDR_CHANGE:
		handle_peer_address_change_event(&(notif->sn_paddr_change));
		break;
	case SCTP_REMOTE_ERROR:
		handle_remote_error_event(&(notif->sn_remote_error));
		break;
	case SCTP_SHUTDOWN_EVENT:
		handle_shutdown_event(&(notif->sn_shutdown_event));
		break;
	case SCTP_ADAPTATION_INDICATION:
		handle_adaptation_indication(&(notif->sn_adaptation_event));
		break;
	case SCTP_PARTIAL_DELIVERY_EVENT:
		break;
	case SCTP_AUTHENTICATION_EVENT:
		break;
	case SCTP_SENDER_DRY_EVENT:
		break;
	case SCTP_NOTIFICATIONS_STOPPED_EVENT:
		break;
	case SCTP_SEND_FAILED_EVENT:
		handle_send_failed(&(notif->sn_send_failed_event));
		break;
	case SCTP_STREAM_RESET_EVENT:
		handle_stream_reset_event(pc, &(notif->sn_strreset_event));
		send_outgoing_stream_reset(pc);
		request_more_o_streams(pc);
		break;
	case SCTP_ASSOC_RESET_EVENT:
		break;
	case SCTP_STREAM_CHANGE_EVENT:
		handle_stream_change_event(pc, &(notif->sn_strchange_event));
		send_outgoing_stream_reset(pc);
		request_more_o_streams(pc);
		break;
	default:
		break;
	}
}

static void
print_status(struct peer_connection *pc)
{
	struct sctp_status status;
	socklen_t len;
	uint32_t i;
	struct channel *channel;

	len = (socklen_t)sizeof(struct sctp_status);
	if (usrsctp_getsockopt(pc->sock, IPPROTO_SCTP, SCTP_STATUS, &status, &len) < 0) {
		perror("getsockopt");
		return;
	}
	printf("Association state: ");
	switch (status.sstat_state) {
	case SCTP_CLOSED:
		printf("CLOSED\n");
		break;
	case SCTP_BOUND:
		printf("BOUND\n");
		break;
	case SCTP_LISTEN:
		printf("LISTEN\n");
		break;
	case SCTP_COOKIE_WAIT:
		printf("COOKIE_WAIT\n");
		break;
	case SCTP_COOKIE_ECHOED:
		printf("COOKIE_ECHOED\n");
		break;
	case SCTP_ESTABLISHED:
		printf("ESTABLISHED\n");
		break;
	case SCTP_SHUTDOWN_PENDING:
		printf("SHUTDOWN_PENDING\n");
		break;
	case SCTP_SHUTDOWN_SENT:
		printf("SHUTDOWN_SENT\n");
		break;
	case SCTP_SHUTDOWN_RECEIVED:
		printf("SHUTDOWN_RECEIVED\n");
		break;
	case SCTP_SHUTDOWN_ACK_SENT:
		printf("SHUTDOWN_ACK_SENT\n");
		break;
	default:
		printf("UNKNOWN\n");
		break;
	}
	printf("Number of streams (i/o) = (%u/%u)\n",
	       status.sstat_instrms, status.sstat_outstrms);
	for (i = 0; i < NUMBER_OF_CHANNELS; i++) {
		channel = &(pc->channels[i]);
		if (channel->state == DATA_CHANNEL_CLOSED) {
			continue;
		}
		printf("Channel with id = %u: state ", channel->id);
		switch (channel->state) {
		case DATA_CHANNEL_CLOSED:
			printf("CLOSED");
			break;
		case DATA_CHANNEL_CONNECTING:
			printf("CONNECTING");
			break;
		case DATA_CHANNEL_OPEN:
			printf("OPEN");
			break;
		case DATA_CHANNEL_CLOSING:
			printf("CLOSING");
			break;
		default:
			printf("UNKNOWN(%d)", channel->state);
			break;
		}
		printf(", stream id (in/out): (%u/%u), ",
		       channel->i_stream,
		       channel->o_stream);
		if (channel->unordered) {
			printf("unordered, ");
		} else {
			printf("ordered, ");
		}
		switch (channel->pr_policy) {
		case SCTP_PR_SCTP_NONE:
			printf("reliable.\n");
			break;
		case SCTP_PR_SCTP_TTL:
			printf("unreliable (timeout %ums).\n", channel->pr_value);
			break;
		case SCTP_PR_SCTP_RTX:
			printf("unreliable (max. %u rtx).\n", channel->pr_value);
			break;
		default:
			printf("unkown policy %u.\n", channel->pr_policy);
			break;
		}
	}
}

static int
receive_cb(struct socket *sock, union sctp_sockstore addr, void *data,
           size_t datalen, struct sctp_rcvinfo rcv, int flags)
{
	if (data) {
		lock_peer_connection(&peer_connection);
		if (flags & MSG_NOTIFICATION) {
			handle_notification(&peer_connection, (union sctp_notification *)data, (int)datalen);
		} else {
			handle_message(&peer_connection, data, datalen, ntohl(rcv.rcv_ppid), rcv.rcv_sid);
		}
		unlock_peer_connection(&peer_connection);
	}
	return 1;
}


int
main(int argc, char *argv[])
{
	struct socket *sock;
	struct sockaddr_in addr;
	socklen_t addr_len;
	char line[LINE_LENGTH + 1];
	unsigned int unordered, policy, value, id, seconds;
	unsigned int i;
	struct channel *channel;
	const int on = 1;
	struct sctp_assoc_value av;
	struct sctp_event event;
	struct sctp_udpencaps encaps;
	uint16_t event_types[] = {SCTP_ASSOC_CHANGE,
	                          SCTP_PEER_ADDR_CHANGE,
	                          SCTP_REMOTE_ERROR,
	                          SCTP_SHUTDOWN_EVENT,
	                          SCTP_ADAPTATION_INDICATION,
	                          SCTP_SEND_FAILED_EVENT,
	                          SCTP_STREAM_RESET_EVENT,
	                          SCTP_STREAM_CHANGE_EVENT};
  if (argc > 1) {
		usrsctp_init(atoi(argv[1]));
	} else {
		usrsctp_init(9899);
	}

	usrsctp_sysctl_set_sctp_debug_on(0);
	usrsctp_sysctl_set_sctp_blackhole(2);

	if ((sock = usrsctp_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP, receive_cb, NULL, 0)) < 0) {
		perror("socket");
	}
	if (argc > 2) {
		memset(&encaps, 0, sizeof(struct sctp_udpencaps));
		encaps.sue_address.ss_family = AF_INET6;
		encaps.sue_port = htons(atoi(argv[2]));
		if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_REMOTE_UDP_ENCAPS_PORT, (const void*)&encaps, (socklen_t)sizeof(struct sctp_udpencaps)) < 0) {
			perror("setsockopt");
		}
	}
	if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(int)) < 0) {
		perror("setsockopt SCTP_RECVRCVINFO");
	}
	if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_EXPLICIT_EOR, &on, sizeof(int)) < 0) {
		perror("setsockopt SCTP_EXPLICIT_EOR");
	}
	/* Allow resetting streams. */
	av.assoc_id = SCTP_ALL_ASSOC;
	av.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ | SCTP_ENABLE_CHANGE_ASSOC_REQ;
	if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &av, sizeof(struct sctp_assoc_value)) < 0) {
		perror("setsockopt SCTP_ENABLE_STREAM_RESET");
	}
	/* Enable the events of interest. */
	memset(&event, 0, sizeof(event));
	event.se_assoc_id = SCTP_ALL_ASSOC;
	event.se_on = 1;
	for (i = 0; i < sizeof(event_types)/sizeof(uint16_t); i++) {
		event.se_type = event_types[i];
		if (usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) < 0) {
			perror("setsockopt SCTP_EVENT");
		}
	}

	if (argc > 4) {
		/* operating as client */
		memset(&addr, 0, sizeof(struct sockaddr_in));
		addr.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
		addr.sin_len = sizeof(struct sockaddr_in);
#endif
		addr.sin_addr.s_addr = inet_addr(argv[3]);
		addr.sin_port = htons(atoi(argv[4]));
		if (usrsctp_connect(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0) {
			perror("connect");
		}
		printf("Connected to %s:%d.\n",
		       inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
	} else if (argc > 3) {
		struct socket *conn_sock;

		/* operating as server */
		memset(&addr, 0, sizeof(struct sockaddr_in));
		addr.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
		addr.sin_len = sizeof(struct sockaddr_in);
#endif
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(atoi(argv[3]));
		if (usrsctp_bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0) {
			perror("bind");
		}
		if (usrsctp_listen(sock, 1) < 0) {
			perror("listen");
		}
		addr_len = (socklen_t)sizeof(struct sockaddr_in);
		memset(&addr, 0, sizeof(struct sockaddr_in));
		if ((conn_sock = usrsctp_accept(sock, (struct sockaddr *)&addr, &addr_len)) < 0) {
			perror("accept");
		}
		usrsctp_close(sock);
		sock = conn_sock;
		printf("Connected to %s:%d.\n",
		       inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
	} else if (argc < 2) {
		printf("Usage: %s local_udp_port remote_udp_port local_port when operating as server\n"
		       "       %s local_udp_port remote_udp_port remote_addr remote_port when operating as client\n",
		       argv[0], argv[0]);
		return (0);
	}

	init_peer_connection(&peer_connection, sock);

	for (;;) {
		if (fgets(line, LINE_LENGTH, stdin) == NULL) {
			break;
		}
		if (strncasecmp(line, "?", strlen("?")) == 0 ||
		    strncasecmp(line, "help", strlen("help")) == 0) {
			printf("Commands:\n"
			       "open unordered pr_policy pr_value - opens a channel\n"
			       "close channel - closes the channel\n"
			       "send channel:string - sends string using channel\n"
			       "status - prints the status\n"
			       "sleep n - sleep for n seconds\n"
			       "help - this message\n");
		} else if (strncasecmp(line, "status", strlen("status")) == 0) {
			lock_peer_connection(&peer_connection);
			print_status(&peer_connection);
			unlock_peer_connection(&peer_connection);
		} else if (sscanf(line, "open %u %u %u", &unordered, &policy, &value) == 3) {
			lock_peer_connection(&peer_connection);
			channel = open_channel(&peer_connection, (uint8_t)unordered, (uint16_t)policy, (uint32_t)value);
			unlock_peer_connection(&peer_connection);
			if (channel == NULL) {
				printf("Creating channel failed.\n");
			} else {
				printf("Channel with id %u created.\n", channel->id);
			}
		} else if (sscanf(line, "close %u", &id) == 1) {
			if (id < NUMBER_OF_CHANNELS) {
				lock_peer_connection(&peer_connection);
				close_channel(&peer_connection, &peer_connection.channels[id]);
				unlock_peer_connection(&peer_connection);
			}
		} else if (sscanf(line, "send %u", &id) == 1) {
			if (id < NUMBER_OF_CHANNELS) {
				char *msg;

				msg = strstr(line, ":");
				if (msg) {
					msg++;
					lock_peer_connection(&peer_connection);
					if (send_user_message(&peer_connection, &peer_connection.channels[id], msg, strlen(msg) - 1)) {
						printf("Message sent.\n");
					} else {
						printf("Message sending failed.\n");
					}
					unlock_peer_connection(&peer_connection);
				}
			}
		} else if (sscanf(line, "sleep %u", &seconds) == 1) {
				sleep(seconds);
		} else {
			printf("Unknown command: %s", line);
		}
	}
	usrsctp_close(sock);
	return (0);
}
