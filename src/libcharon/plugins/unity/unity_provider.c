/*
 * Copyright (C) 2012 Martin Willi
 * Copyright (C) 2012 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "unity_provider.h"

#include <daemon.h>

typedef struct private_unity_provider_t private_unity_provider_t;

/**
 * Private data of an unity_provider_t object.
 */
struct private_unity_provider_t {

	/**
	 * Public unity_provider_t interface.
	 */
	unity_provider_t public;
};

/**
 * Attribute enumerator for traffic selector list
 */
typedef struct {
	/** Implements enumerator_t */
	enumerator_t public;
	/** list of traffic selectors to enumerate */
	linked_list_t *list;
	/** currently enumerating subnet */
	u_char subnet[4];
	/** currently enumerating subnet mask */
	u_char mask[4];
} attribute_enumerator_t;

METHOD(enumerator_t, attribute_enumerate, bool,
	attribute_enumerator_t *this, configuration_attribute_type_t *type,
	chunk_t *attr)
{
	traffic_selector_t *ts;
	u_int8_t i, mask;
	host_t *net;

	while (TRUE)
	{
		if (this->list->remove_first(this->list, (void**)&ts) != SUCCESS)
		{
			return FALSE;
		}
		if (ts->get_type(ts) == TS_IPV4_ADDR_RANGE &&
			!ts->is_dynamic(ts) &&
			ts->to_subnet(ts, &net, &mask))
		{
			if (mask > 0)
			{
				ts->destroy(ts);
				break;
			}
			net->destroy(net);
		}
		ts->destroy(ts);
	}

	memset(this->mask, 0, sizeof(this->mask));
	for (i = 0; i < sizeof(this->mask); i++)
	{
		if (mask < 8)
		{
			this->mask[i] = 0xFF << (8 - mask);
			break;
		}
		this->mask[i] = 0xFF;
		mask -= 8;
	}
	memcpy(this->subnet, net->get_address(net).ptr, sizeof(this->subnet));
	net->destroy(net);

	*type = UNITY_SPLIT_INCLUDE;
	*attr = chunk_create(this->subnet, sizeof(this->subnet) + sizeof(this->mask));

	return TRUE;
}

METHOD(enumerator_t, attribute_destroy, void,
	attribute_enumerator_t *this)
{
	this->list->destroy_offset(this->list, offsetof(traffic_selector_t, destroy));
	free(this);
}

METHOD(attribute_provider_t, create_attribute_enumerator, enumerator_t*,
	private_unity_provider_t *this, linked_list_t *pools, identification_t *id,
	linked_list_t *vips)
{
	attribute_enumerator_t *attr_enum;
	enumerator_t *enumerator;
	linked_list_t *list, *current;
	traffic_selector_t *ts;
	ike_sa_t *ike_sa;
	peer_cfg_t *peer_cfg;
	child_cfg_t *child_cfg;

	ike_sa = charon->bus->get_sa(charon->bus);
	if (!ike_sa || ike_sa->get_version(ike_sa) != IKEV1 ||
		!ike_sa->supports_extension(ike_sa, EXT_CISCO_UNITY) ||
		!vips->get_count(vips))
	{
		return NULL;
	}

	list = linked_list_create();
	peer_cfg = ike_sa->get_peer_cfg(ike_sa);
	enumerator = peer_cfg->create_child_cfg_enumerator(peer_cfg);
	while (enumerator->enumerate(enumerator, &child_cfg))
	{
		current = child_cfg->get_traffic_selectors(child_cfg, TRUE, NULL, NULL);
		while (current->remove_first(current, (void**)&ts) == SUCCESS)
		{
			list->insert_last(list, ts);
		}
		current->destroy(current);
	}
	enumerator->destroy(enumerator);

	if (list->get_count(list) == 0)
	{
		list->destroy(list);
		return NULL;
	}
	DBG1(DBG_CFG, "sending %N: %#R",
		 configuration_attribute_type_names, UNITY_SPLIT_INCLUDE, list);

	INIT(attr_enum,
		.public = {
			.enumerate = (void*)_attribute_enumerate,
			.destroy = _attribute_destroy,
		},
		.list = list,
	);

	return &attr_enum->public;
}

METHOD(unity_provider_t, destroy, void,
	private_unity_provider_t *this)
{
	free(this);
}

/**
 * See header
 */
unity_provider_t *unity_provider_create()
{
	private_unity_provider_t *this;

	INIT(this,
		.public = {
			.provider = {
				.acquire_address = (void*)return_null,
				.release_address = (void*)return_false,
				.create_attribute_enumerator = _create_attribute_enumerator,
			},
			.destroy = _destroy,
		},
	);

	return &this->public;
}
