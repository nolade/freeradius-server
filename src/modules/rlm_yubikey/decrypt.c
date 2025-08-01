/**
 * $Id$
 * @file decrypt.c
 * @brief Authentication for yubikey OTP tokens using the yubikey library.
 *
 * @author Arran Cudbard-Bell (a.cudbardb@networkradius.com)
 * @copyright 2013 The FreeRADIUS server project
 * @copyright 2013 Network RADIUS (legal@networkradius.com)
 */
#include "rlm_yubikey.h"

#ifdef HAVE_YUBIKEY

/** Decrypt a Yubikey OTP AES block
 *
 * @param[out] p_result		The result of attempt to decrypt the token.
 * @param[in] mctx		call data.
 * @param[in] request		The current request.
 * @param[in] passcode		string to decrypt.
 */
unlang_action_t rlm_yubikey_decrypt(unlang_result_t *p_result, module_ctx_t const *mctx, request_t *request, char const *passcode)
{
	rlm_yubikey_t const *inst = talloc_get_type_abort(mctx->mi->data, rlm_yubikey_t);
	uint32_t counter, timestamp;
	yubikey_token_st token;

	fr_pair_t *key, *vp;

	key = fr_pair_find_by_da_nested(&request->control_pairs, NULL, attr_yubikey_key);
	if (!key) {
		REDEBUG("Yubikey-Key attribute not found in control list, can't decrypt OTP data");
		RETURN_UNLANG_INVALID;
	}

	if (key->vp_length != YUBIKEY_KEY_SIZE) {
		REDEBUG("Yubikey-Key length incorrect, expected %u got %zu", YUBIKEY_KEY_SIZE, key->vp_length);
		RETURN_UNLANG_INVALID;
	}

	yubikey_parse((uint8_t const *) passcode + inst->id_len, key->vp_octets, &token);

	/*
	 *	Apparently this just uses byte offsets...
	 */
	if (!yubikey_crc_ok_p((uint8_t *) &token)) {
		REDEBUG("Decrypting OTP token data failed, rejecting");
		RETURN_UNLANG_REJECT;
	}

	RDEBUG2("Token data decrypted successfully");

	counter = (yubikey_counter(token.ctr) << 8) | token.use;
	timestamp = (token.tstph << 16) | token.tstpl;

	RDEBUG2("Private ID        : %pH", fr_box_octets(token.uid, YUBIKEY_UID_SIZE));
	RDEBUG2("Session counter   : %u", counter);

	RDEBUG2("Token timestamp   : %u", timestamp);

	RDEBUG2("Random data       : %u", token.rnd);
	RDEBUG2("CRC data          : 0x%x", token.crc);

	/*
	 *	Private ID used for validation purposes
	 */
	MEM(pair_update_request(&vp, attr_yubikey_private_id) >= 0);
	fr_pair_value_memdup(vp, token.uid, YUBIKEY_UID_SIZE, true);

	/*
	 *	Token timestamp
	 */
	MEM(pair_update_request(&vp, attr_yubikey_timestamp) >= 0);
	vp->vp_uint32 = timestamp;

	/*
	 *	Token random
	 */
	MEM(pair_update_request(&vp, attr_yubikey_random) >= 0);
	vp->vp_uint32 = token.rnd;

	/*
	 *	Combine the two counter fields together so we can do
	 *	replay attack checks.
	 */
	MEM(pair_update_request(&vp, attr_yubikey_counter) >= 0);
	vp->vp_uint32 = counter;

	/*
	 *	Now we check for replay attacks
	 */
	vp = fr_pair_find_by_da_nested(&request->control_pairs, NULL, attr_yubikey_counter);
	if (!vp) {
		RWDEBUG("Yubikey-Counter not found in control list, skipping replay attack checks");
		RETURN_UNLANG_OK;
	}

	if (counter <= vp->vp_uint32) {
		REDEBUG("Replay attack detected! Counter value %u, is lt or eq to last known counter value %u",
			counter, vp->vp_uint32);
		RETURN_UNLANG_REJECT;
	}

	RETURN_UNLANG_OK;
}
#endif
