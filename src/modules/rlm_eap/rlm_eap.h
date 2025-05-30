#pragma once
/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
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
 * @file rlm_eap.h
 * @brief Implements the EAP framework.
 *
 * @copyright 2000-2003,2006 The FreeRADIUS server project
 * @copyright 2001 hereUare Communications, Inc. (raghud@hereuare.com)
 * @copyright 2003 Alan DeKok (aland@freeradius.org)
 */
RCSIDH(rlm_eap_h, "$Id$")

#include <freeradius-devel/server/modpriv.h>
#include <freeradius-devel/server/state.h>
#include <freeradius-devel/radius/radius.h>
#include <freeradius-devel/eap/base.h>
#include <freeradius-devel/eap/types.h>

/** Different settings for realm issues
 */
typedef enum {
	REQUIRE_REALM_YES = 0,			//!< Require the EAP-Identity string contain an NAI realm
						///< or that Stripped-User-Domain is present in the request.
	REQUIRE_REALM_NO,			//!< Don't require that the identity is qualified.
	REQUIRE_REALM_NAI			//!< Require the EAP-Identity contains an NAI domain.
} rlm_eap_require_realm_t;

/** Instance data for rlm_eap
 *
 */
typedef struct {
	char const			*name;				//!< Name of this instance.

	module_instance_t		**type_submodules;		//!< Submodules we loaded.
	rlm_eap_method_t 		methods[FR_EAP_METHOD_MAX];	//!< Array of loaded (or not), submodules.

	char const			*default_method_name;		//!< Default method to attempt to start.

	eap_type_t			default_method;			//!< Resolved default_method_name.
	bool				default_method_is_set;		//!< Whether the user specified a default
									///< eap method.

	module_instance_t const		**type_identity_submodule;	//!< List of submodules which have a
									///< method identity callback, i.e. those
									///< which may set themselves to be the default
									///< EAP-Type based on the identity provided.
	size_t				type_identity_submodule_len;	//!< How many submodules are in the list.

	bool				ignore_unknown_types;		//!< Ignore unknown types (for later proxying).

	rlm_eap_require_realm_t		require_realm;			//!< Whether we require the outer identity
									///< to contain a realm.
	fr_dict_enum_value_t const	*auth_type;

	fr_randctx			rand_pool;			//!< Pool of random data.
} rlm_eap_t;
