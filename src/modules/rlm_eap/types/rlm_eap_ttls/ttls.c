/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file ttls.c
 * @brief Library functions for EAP-TTLS as defined by RFC 5281
 *
 * @copyright 2003 Alan DeKok (aland@freeradius.org)
 * @copyright 2006 The FreeRADIUS server project
 */

RCSID("$Id$")

#include <freeradius-devel/eap/chbind.h>
#include <freeradius-devel/tls/log.h>
#include <freeradius-devel/tls/strerror.h>
#include "eap_ttls.h"

#define FR_DIAMETER_AVP_FLAG_VENDOR	0x80
#define FR_DIAMETER_AVP_FLAG_MANDATORY	0x40
/*
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                           AVP Code                            |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |V M r r r r r r|                  AVP Length                   |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                        Vendor-ID (opt)                        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |    Data ...
 *   +-+-+-+-+-+-+-+-+
 */

/*
 *	Verify that the diameter packet is valid.
 */
static int diameter_verify(request_t *request, uint8_t const *data, unsigned int data_len)
{
	uint32_t attr;
	uint32_t length;
	unsigned int hdr_len;
	unsigned int remaining = data_len;

	while (remaining > 0) {
		hdr_len = 12;

		if (remaining < hdr_len) {
		  RDEBUG2("Diameter attribute is too small (%u) to contain a Diameter header", remaining);
			return 0;
		}

		memcpy(&attr, data, sizeof(attr));
		attr = ntohl(attr);
		memcpy(&length, data + 4, sizeof(length));
		length = ntohl(length);

		if ((data[4] & 0x80) != 0) {
			if (remaining < 16) {
				RDEBUG2("Diameter attribute is too small to contain a Diameter header with Vendor-Id");
				return 0;
			}

			hdr_len = 16;
		}

		/*
		 *	Get the length.  If it's too big, die.
		 */
		length &= 0x00ffffff;

		/*
		 *	Too short or too long is bad.
		 */
		if (length <= (hdr_len - 4)) {
			RDEBUG2("Tunneled attribute %u is too short (%u < %u) to contain anything useful.", attr,
				length, hdr_len);
			return 0;
		}

		if (length > remaining) {
			RDEBUG2("Tunneled attribute %u is longer than room remaining in the packet (%u > %u).", attr,
				length, remaining);
			return 0;
		}

		/*
		 *	Check for broken implementations, which don't
		 *	pad the AVP to a 4-octet boundary.
		 */
		if (remaining == length) break;

		/*
		 *	The length does NOT include the padding, so
		 *	we've got to account for it here by rounding up
		 *	to the nearest 4-byte boundary.
		 */
		length += 0x03;
		length &= ~0x03;

		/*
		 *	If the rest of the diameter packet is larger than
		 *	this attribute, continue.
		 *
		 *	Otherwise, if the attribute over-flows the end
		 *	of the packet, die.
		 */
		if (remaining < length) {
			REDEBUG2("Diameter attribute overflows packet!");
			return 0;
		}

		/*
		 *	remaining > length, continue.
		 */
		remaining -= length;
		data += length;
	}

	/*
	 *	We got this far.  It looks OK.
	 */
	return 1;
}


/*
 *	Convert diameter attributes to our fr_pair_t's
 */
static ssize_t eap_ttls_decode_pair(request_t *request, TALLOC_CTX *ctx, fr_pair_list_t *out,
				    fr_dict_attr_t const *parent,
				    uint8_t const *data, size_t data_len,
				    void *decode_ctx)
{
	uint8_t const		*p = data, *end = p + data_len;

	fr_pair_t		*vp = NULL;
	SSL			*ssl = decode_ctx;
	fr_dict_t const		*dict_radius;
	fr_dict_attr_t const   	*attr_radius;
	fr_dict_attr_t const	*da;
	TALLOC_CTX		*tmp_ctx = NULL;

	dict_radius = fr_dict_by_protocol_name("radius");
	fr_assert(dict_radius != NULL);
	attr_radius = fr_dict_root(dict_radius);

	while (p < end) {
		ssize_t			ret;
		uint32_t		attr, vendor;
		uint64_t		value_len;
		uint8_t			flags;
		fr_dict_attr_t const	*our_parent = parent;

		if ((end - p) < 8) {
			fr_strerror_printf("Malformed diameter attribute at offset %zu.  Needed at least 8 bytes, got %zu bytes",
					   p - data, end - p);
		error:
			talloc_free(tmp_ctx);
			fr_pair_list_free(out);
			return -1;
		}

		RDEBUG3("%04zu %02x%02x%02x%02x %02x%02x%02x%02x ...", p - data,
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

		attr = fr_nbo_to_uint32(p);
		p += 4;

		flags = p[0];
		p++;

		value_len = fr_nbo_to_uint64v(p, 3);	/* Yes, that is a 24 bit length field */
		p += 3;

		if (value_len < 8) {
			fr_strerror_printf("Malformed diameter attribute at offset %zu.  Needed at least length of 8, got %u",
					   p - data, (unsigned int) value_len);
			goto error;
		}

		/*
		 *	Account for the 8 bytes we've already read from the packet.
		 */
		if ((p + ((value_len + 0x03) & ~0x03)) - 8 > end) {
			fr_strerror_printf("Malformed diameter attribute at offset %zu.  Value length %u overflows input",
					   p - data, (unsigned int) value_len);
			goto error;
		}

		value_len -= 8;	/* -= 8 for AVP code (4), flags (1), AVP length (3) */

		/*
		 *	Do we have a vendor field?
		 */
		if (flags & FR_DIAMETER_AVP_FLAG_VENDOR) {
			vendor = fr_nbo_to_uint32(p);
			p += 4;
			value_len -= 4;	/* -= 4 for the vendor ID field */

			our_parent = fr_dict_vendor_da_by_num(attr_vendor_specific, vendor);
			if (!our_parent) {
				if (flags & FR_DIAMETER_AVP_FLAG_MANDATORY) {
					fr_strerror_printf("Mandatory bit set and no vendor %u found", vendor);
					goto error;
				}

				if (!tmp_ctx) {
					fr_dict_attr_t *n;

					MEM(our_parent = n = fr_dict_attr_unknown_vendor_afrom_num(ctx, parent, vendor));
					tmp_ctx = n;
				} else {
					MEM(our_parent = fr_dict_attr_unknown_vendor_afrom_num(tmp_ctx, parent, vendor));
				}
			}
		} else {
			our_parent = attr_radius;
		}

		/*
		 *	Is the attribute known?
		 */
		da = fr_dict_attr_child_by_num(our_parent, attr);
		if (!da) {
			if (flags & FR_DIAMETER_AVP_FLAG_MANDATORY) {
				fr_strerror_printf("Mandatory bit set and no attribute %u defined for parent %s", attr, parent->name);
				goto error;
			}

			MEM(da = fr_dict_attr_unknown_raw_afrom_num(vp, our_parent, attr));
		}

		MEM(vp =fr_pair_afrom_da_nested(ctx, out, da));

		ret = fr_value_box_from_network(vp, &vp->data, vp->vp_type, vp->da,
						&FR_DBUFF_TMP(p, (size_t)value_len), value_len, true);
		if (ret < 0) {
			/*
			 *	Mandatory bit is set, and the attribute
			 *	is malformed. Fail.
			 */
			if (flags & FR_DIAMETER_AVP_FLAG_MANDATORY) {
				fr_strerror_const("Mandatory bit is set and attribute is malformed");
				goto error;
			}

			fr_pair_raw_afrom_pair(vp, p, value_len);
		}

		/*
		 *	The length does NOT include the padding, so
		 *	we've got to account for it here by rounding up
		 *	to the nearest 4-byte boundary.
		 */
		p += (value_len + 0x03) & ~0x03;

		if (vp->da->flags.is_unknown) continue;

		/*
		 *	Ensure that the client is using the correct challenge.
		 *
		 *	This weirdness is to protect against against replay
		 *	attacks, where anyone observing the CHAP exchange could
		 *	pose as that user, by simply choosing to use the same
		 *	challenge.
		 *	By using a challenge based on information from the
		 *	current session, we can guarantee that the client is
		 *	not *choosing* a challenge. We're a little forgiving in
		 *	that we have loose checks on the length, and we do NOT
		 *	check the Id (first octet of the response to the
		 *	challenge) But if the client gets the challenge correct,
		 *	we're not too worried about the Id.
		 */
		if ((vp->da == attr_chap_challenge) || (vp->da == attr_ms_chap_challenge)) {
			uint8_t	challenge[17];
			static const char label[] = "ttls challenge";

			if ((vp->vp_length < 8) || (vp->vp_length > 16)) {
				fr_strerror_const("Tunneled challenge has invalid length");
				goto error;
			}

			/*
			 *	TLSv1.3 exports a different key depending on the length
			 *	requested so ask for *exactly* what the spec requires
			 */
			if (SSL_export_keying_material(ssl, challenge, vp->vp_length + 1,
						       label, sizeof(label) - 1, NULL, 0, 0) != 1) {
				fr_tls_strerror_printf("Failed generating phase2 challenge");
				goto error;
			}

			if (memcmp(challenge, vp->vp_octets, vp->vp_length) != 0) {
				fr_strerror_const("Tunneled challenge is incorrect");
				goto error;
			}
		}

		/*
		 *	Diameter pads strings (i.e. User-Password) with trailing zeros.
		 */
		if (vp->vp_type == FR_TYPE_STRING) fr_pair_value_strtrim(vp);
	}

	/*
	 *	We got this far.  It looks OK.
	 */
	talloc_free(tmp_ctx);
	return p - data;
}

/*
 *	Convert fr_pair_t's to diameter attributes, and write them
 *	to an SSL session.
 *
 *	The ONLY fr_pair_t's which may be passed to this function
 *	are ones which can go inside of a RADIUS (i.e. diameter)
 *	packet.  So no server-configuration attributes, or the like.
 */
static int vp2diameter(request_t *request, fr_tls_session_t *tls_session, fr_pair_list_t *list)
{
	/*
	 *	RADIUS packets are no more than 4k in size, so if
	 *	we've got more than 4k of data to write, it's very
	 *	bad.
	 */
	uint8_t		buffer[4096];
	uint8_t		*p;
	uint32_t	attr;
	uint32_t	length;
	uint32_t	vendor;
	size_t		total;
	uint64_t	attr64;
	fr_pair_t	*vp;

	p = buffer;
	total = 0;

	for (vp = fr_pair_list_head(list);
	     vp;
	     vp = fr_pair_list_next(list, vp)) {
		/*
		 *	Too much data: die.
		 */
		if ((total + vp->vp_length + 12) >= sizeof(buffer)) {
			RDEBUG2("output buffer is full!");
			return 0;
		}

		/*
		 *	Hmm... we don't group multiple EAP-Messages
		 *	together.  Maybe we should...
		 */

		length = vp->vp_length;
		vendor = fr_dict_vendor_num_by_da(vp->da);
		if (vendor != 0) {
			attr = vp->da->attr & 0xffff;
			length |= ((uint32_t)1 << 31);
		} else {
			attr = vp->da->attr;
		}

		/*
		 *	Hmm... set the M bit for all attributes?
		 */
		length |= (1 << 30);

		attr = ntohl(attr);

		memcpy(p, &attr, sizeof(attr));
		p += 4;
		total += 4;

		length += 8;	/* includes 8 bytes of attr & length */

		if (vendor != 0) {
			length += 4; /* include 4 bytes of vendor */

			length = ntohl(length);
			memcpy(p, &length, sizeof(length));
			p += 4;
			total += 4;

			vendor = ntohl(vendor);
			memcpy(p, &vendor, sizeof(vendor));
			p += 4;
			total += 4;
		} else {
			length = ntohl(length);
			memcpy(p, &length, sizeof(length));
			p += 4;
			total += 4;
		}

		switch (vp->vp_type) {
		case FR_TYPE_DATE:
			attr = htonl(fr_unix_time_to_sec(vp->vp_date)); /* stored in host order */
			memcpy(p, &attr, sizeof(attr));
			length = 4;
			break;

		case FR_TYPE_UINT32:
			attr = htonl(vp->vp_uint32); /* stored in host order */
			memcpy(p, &attr, sizeof(attr));
			length = 4;
			break;

		case FR_TYPE_UINT64:
			attr64 = htonll(vp->vp_uint64); /* stored in host order */
			memcpy(p, &attr64, sizeof(attr64));
			length = 8;
			break;

		case FR_TYPE_IPV4_ADDR:
			memcpy(p, &vp->vp_ipv4addr, 4); /* network order */
			length = 4;
			break;

		case FR_TYPE_STRING:
		case FR_TYPE_OCTETS:
		default:
			memcpy(p, vp->vp_strvalue, vp->vp_length);
			length = vp->vp_length;
			break;
		}

		/*
		 *	Skip to the end of the data.
		 */
		p += length;
		total += length;

		/*
		 *	Align the data to a multiple of 4 bytes.
		 */
		if ((total & 0x03) != 0) {
			size_t i;

			length = 4 - (total & 0x03);
			for (i = 0; i < length; i++) {
				*p = '\0';
				p++;
				total++;
			}
		}
	} /* loop over the VP's to write. */

	/*
	 *	Write the data in the buffer to the SSL session.
	 */
	if (total > 0) {
		(tls_session->record_from_buff)(&tls_session->clean_in, buffer, total);

		/*
		 *	FIXME: Check the return code.
		 */
		fr_tls_session_send(request, tls_session);
	}

	/*
	 *	Everything's OK.
	 */
	return 1;
}

/*
 *	Use a reply packet to determine what to do.
 */
static unlang_action_t process_reply(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	eap_session_t		*eap_session = talloc_get_type_abort(mctx->rctx, eap_session_t);
	eap_tls_session_t	*eap_tls_session = talloc_get_type_abort(eap_session->opaque, eap_tls_session_t);
	fr_tls_session_t	*tls_session = eap_tls_session->tls_session;
	fr_pair_t		*vp = NULL;
	fr_pair_list_t		tunnel_vps;
	ttls_tunnel_t		*t = tls_session->opaque;
	fr_packet_t		*reply = request->reply;

	fr_pair_list_init(&tunnel_vps);
	fr_assert(eap_session->request == request->parent);

	/*
	 *	If the response packet was Access-Accept, then
	 *	we're OK.  If not, die horribly.
	 *
	 *	FIXME: Take MS-CHAP2-Success attribute, and
	 *	tunnel it back to the client, to authenticate
	 *	ourselves to the client.
	 *
	 *	FIXME: If we have an Access-Challenge, then
	 *	the Reply-Message is tunneled back to the client.
	 *
	 *	FIXME: If we have an EAP-Message, then that message
	 *	must be tunneled back to the client.
	 *
	 *	FIXME: If we have an Access-Challenge with a State
	 *	attribute, then do we tunnel that to the client, or
	 *	keep track of it ourselves?
	 *
	 *	FIXME: EAP-Messages can only start with 'identity',
	 *	NOT 'eap start', so we should check for that....
	 */
	switch (reply->code) {
	case FR_RADIUS_CODE_ACCESS_ACCEPT:
		RDEBUG2("Got tunneled Access-Accept");

		/*
		 *	Copy what we need into the TTLS tunnel and leave
		 *	the rest to be cleaned up.
		 */
		if ((vp = fr_pair_find_by_da_nested(&request->reply_pairs, NULL, attr_ms_chap2_success))) {
			RDEBUG2("Got MS-CHAP2-Success, tunneling it to the client in a challenge");
		} else {
			vp = fr_pair_find_by_da_nested(&request->reply_pairs, NULL, attr_eap_channel_binding_message);
		}
		if (vp) {
			t->authenticated = true;
			fr_pair_prepend(&tunnel_vps, fr_pair_copy(tls_session, vp));
			reply->code = FR_RADIUS_CODE_ACCESS_CHALLENGE;
			break;
		}

		/*
		 *	Success: Automatically return MPPE keys.
		 */
		return eap_ttls_success(p_result, request, eap_session);

	case FR_RADIUS_CODE_ACCESS_REJECT:
		REDEBUG("Got tunneled Access-Reject");
		eap_tls_fail(request, eap_session);
		RETURN_UNLANG_REJECT;

	/*
	 *	Handle Access-Challenge, but only if we
	 *	send tunneled reply data.  This is because
	 *	an Access-Challenge means that we MUST tunnel
	 *	a Reply-Message to the client.
	 */
	case FR_RADIUS_CODE_ACCESS_CHALLENGE:
		RDEBUG2("Got tunneled Access-Challenge");

		/*
		 *	Copy what we need into the TTLS tunnel and leave
		 *	the rest to be cleaned up.
		 */
		vp = NULL;
		while ((vp = fr_pair_list_next(&request->reply_pairs, vp))) {
		     	if ((vp->da == attr_eap_message) || (vp->da == attr_reply_message)) {
				fr_pair_prepend(&tunnel_vps, fr_pair_copy(tls_session, vp));
		     	} else if (vp->da == attr_eap_channel_binding_message) {
				fr_pair_prepend(&tunnel_vps, fr_pair_copy(tls_session, vp));
		     	}
		}
		break;

	default:
		REDEBUG("Unknown RADIUS packet type %d: rejecting tunneled user", reply->code);
		eap_tls_fail(request, eap_session);
		RETURN_UNLANG_INVALID;
	}


	/*
	 *	Pack any tunneled VPs and send them back
	 *	to the supplicant.
	 */
	if (!fr_pair_list_empty(&tunnel_vps)) {
		RDEBUG2("Sending tunneled reply attributes");
		log_request_pair_list(L_DBG_LVL_2, request, NULL, &tunnel_vps, NULL);

		vp2diameter(request, tls_session, &tunnel_vps);
		fr_pair_list_free(&tunnel_vps);
	}

	eap_tls_request(request, eap_session);
	RETURN_UNLANG_OK;
}

unlang_action_t eap_ttls_success(unlang_result_t *p_result, request_t *request, eap_session_t *eap_session)
{
	eap_tls_session_t	*eap_tls_session = talloc_get_type_abort(eap_session->opaque, eap_tls_session_t);
	fr_tls_session_t	*tls_session = eap_tls_session->tls_session;
	eap_tls_prf_label_t prf_label;

	eap_crypto_prf_label_init(&prf_label, eap_session,
				  "ttls keying material",
				  sizeof("ttls keying material") - 1);
	/*
	 *	Success: Automatically return MPPE keys.
	 */
	if (eap_tls_success(request, eap_session, &prf_label) < 0) RETURN_UNLANG_FAIL;

	/*
	 *	Result is always OK, even if we fail to persist the
	 *	session data.
	 */
	p_result->rcode = RLM_MODULE_OK;

	/*
	 *	Write the session to the session cache
	 *
	 *	We do this here (instead of relying on OpenSSL to call the
	 *	session caching callback), because we only want to write
	 *	session data to the cache if all phases were successful.
	 *
	 *	If we wrote out the cache data earlier, and the server
	 *	exited whilst the session was in progress, the supplicant
	 *	could resume the session (and get access) even if phase2
	 *	never completed.
	 */
	return fr_tls_cache_pending_push(request, tls_session);
}

/*
 *	Process the "diameter" contents of the tunneled data.
 */
unlang_action_t eap_ttls_process(unlang_result_t *p_result, request_t *request, eap_session_t *eap_session, fr_tls_session_t *tls_session)
{
	fr_pair_t		*vp = NULL;
	ttls_tunnel_t		*t;
	uint8_t			const *data;
	size_t			data_len;
	chbind_packet_t		*chbind;
	fr_pair_t		*username;

	/*
	 *	Just look at the buffer directly, without doing
	 *	record_to_buff.
	 */
	data_len = tls_session->clean_out.used;
	tls_session->clean_out.used = 0;
	data = tls_session->clean_out.data;

	t = (ttls_tunnel_t *) tls_session->opaque;

	/*
	 *	If there's no data, maybe this is an ACK to an
	 *	MS-CHAP2-Success.
	 */
	if (data_len == 0) {
		if (t->authenticated) {
			RDEBUG2("Got ACK, and the user was already authenticated");
			return eap_ttls_success(p_result, request, eap_session);
		} /* else no session, no data, die. */

		/*
		 *	FIXME: Call SSL_get_error() to see what went
		 *	wrong.
		 */
		RDEBUG2("SSL_read Error");
		return UNLANG_ACTION_FAIL;
	}

	if (!diameter_verify(request, data, data_len)) return UNLANG_ACTION_FAIL;

	/*
	 *	Add the tunneled attributes to the request request.
	 */
	if (eap_ttls_decode_pair(request, request->request_ctx, &request->request_pairs, fr_dict_root(fr_dict_internal()),
				 data, data_len, tls_session->ssl) < 0) {
		RPEDEBUG("Decoding TTLS TLVs failed");
		return UNLANG_ACTION_FAIL;
	}

	/*
	 *	Update other items in the request_t data structure.
	 */

	/*
	 *	No User-Name, try to create one from stored data.
	 */
	username = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_name);
	if (!username) {
		/*
		 *	No User-Name in the stored data, look for
		 *	an EAP-Identity, and pull it out of there.
		 */
		if (!t->username) {
			vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_eap_message);
			if (vp &&
			    (vp->vp_length >= EAP_HEADER_LEN + 2) &&
			    (vp->vp_strvalue[0] == FR_EAP_CODE_RESPONSE) &&
			    (vp->vp_strvalue[EAP_HEADER_LEN] == FR_EAP_METHOD_IDENTITY) &&
			    (vp->vp_strvalue[EAP_HEADER_LEN + 1] != 0)) {
				/*
				 *	Create & remember a User-Name
				 */
				MEM(t->username = fr_pair_afrom_da(t, attr_user_name));
				t->username->vp_tainted = true;

				fr_pair_value_bstrndup(t->username,
						       (char const *)vp->vp_octets + 5, vp->vp_length - 5, true);

				RDEBUG2("Got tunneled identity of %pV", &t->username->data);
			} else {
				/*
				 *	Don't reject the request outright,
				 *	as it's permitted to do EAP without
				 *	user-name.
				 */
				RWDEBUG2("No EAP-Identity found to start EAP conversation");
			}
		} /* else there WAS a t->username */

		if (t->username) {
			vp = fr_pair_copy(request->request_ctx, t->username);
			fr_pair_append(&request->request_pairs, vp);
		}
	} /* else the request ALREADY had a User-Name */

	/*
	 *	Process channel binding.
	 */
	chbind = eap_chbind_vp2packet(request, &request->request_pairs);
	if (chbind) {
		fr_radius_packet_code_t chbind_code;
		CHBIND_REQ *req = talloc_zero(request, CHBIND_REQ);

		RDEBUG2("received chbind request");
		req->request = chbind;
		if (username) {
			req->username = username;
		} else {
			req->username = NULL;
		}
		chbind_code = chbind_process(request, req);

		/* encapsulate response here */
		if (req->response) {
			RDEBUG2("sending chbind response");
			fr_pair_append(&request->reply_pairs,
				    eap_chbind_packet2vp(request->reply_ctx, req->response));
		} else {
			RDEBUG2("no chbind response");
		}

		/* clean up chbind req */
		talloc_free(req);

		if (chbind_code != FR_RADIUS_CODE_ACCESS_ACCEPT) return UNLANG_ACTION_FAIL;
	}

	/*
	 *	For this round, when the virtual server returns
	 *	we run the process reply function.
	 */
	if (unlikely(unlang_module_yield(request, process_reply, NULL, 0, eap_session) != UNLANG_ACTION_YIELD)) {
		return UNLANG_ACTION_FAIL;
	}

	/*
	 *	Call authentication recursively, which will
	 *	do PAP, CHAP, MS-CHAP, etc.
	 */
	return eap_virtual_server(request, eap_session, t->server_cs);
}
