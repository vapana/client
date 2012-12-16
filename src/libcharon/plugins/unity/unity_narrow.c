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

#include "unity_narrow.h"

#include <daemon.h>

typedef struct private_unity_narrow_t private_unity_narrow_t;

/**
 * Private data of an unity_narrow_t object.
 */
struct private_unity_narrow_t {

	/**
	 * Public unity_narrow_t interface.
	 */
	unity_narrow_t public;

	/**
	 * Unity attribute handler
	 */
	unity_handler_t *handler;
};

/**
 * Narrow TS as initiator to Unity Split-Include/Local-LAN
 */
static void narrow_initiator(private_unity_narrow_t *this, ike_sa_t *ike_sa,
							 child_cfg_t *cfg, linked_list_t *remote)
{
	traffic_selector_t *current, *orig = NULL;
	linked_list_t *received, *selected;
	enumerator_t *enumerator;

	enumerator = this->handler->create_include_enumerator(this->handler,
											ike_sa->get_unique_id(ike_sa));
	while (enumerator->enumerate(enumerator, &current))
	{
		if (orig == NULL)
		{	/* got one, replace original TS */
			if (remote->remove_first(remote, (void**)&orig) != SUCCESS)
			{
				break;
			}
		}
		/* narrow received Unity TS with the child configuration */
		received = linked_list_create();
		received->insert_last(received, current);
		selected = cfg->get_traffic_selectors(cfg, FALSE, received, NULL);
		while (selected->remove_first(selected, (void**)&current) == SUCCESS)
		{
			remote->insert_last(remote, current);
		}
		selected->destroy(selected);
		received->destroy(received);
	}
	enumerator->destroy(enumerator);
	if (orig)
	{
		DBG1(DBG_CFG, "narrowed CHILD_SA to %N %#R",
			 configuration_attribute_type_names,
			 UNITY_SPLIT_INCLUDE, remote);
		orig->destroy(orig);
	}
}

/**
 * As initiator, bump up TS to 0.0.0.0/0 for on-the-wire bits
 */
static void narrow_initiator_pre(linked_list_t *list)
{
	traffic_selector_t *ts;

	while (list->remove_first(list, (void**)&ts) == SUCCESS)
	{
		ts->destroy(ts);
	}
	ts = traffic_selector_create_from_string(0, TS_IPV4_ADDR_RANGE,
											 "0.0.0.0", 0,
											 "255.255.255.255", 65535);
	if (ts)
	{
		list->insert_last(list, ts);
	}
}

/**
 * As responder, narrow down TS to configuration for installation
 */
static void narrow_responder_post(child_cfg_t *child_cfg, linked_list_t *local)
{
	traffic_selector_t *ts;
	linked_list_t *configured;

	while (local->remove_first(local, (void**)&ts) == SUCCESS)
	{
		ts->destroy(ts);
	}
	configured = child_cfg->get_traffic_selectors(child_cfg, TRUE, NULL, NULL);

	while (configured->remove_first(configured, (void**)&ts) == SUCCESS)
	{
		local->insert_last(local, ts);
	}
	configured->destroy(configured);
}

METHOD(listener_t, narrow, bool,
	private_unity_narrow_t *this, ike_sa_t *ike_sa, child_sa_t *child_sa,
	narrow_hook_t type, linked_list_t *local, linked_list_t *remote)
{
	if (ike_sa->get_version(ike_sa) == IKEV1 &&
		ike_sa->supports_extension(ike_sa, EXT_CISCO_UNITY))
	{
		switch (type)
		{
			case NARROW_INITIATOR_PRE_AUTH:
				narrow_initiator_pre(remote);
				break;
			case NARROW_INITIATOR_POST_AUTH:
				narrow_initiator(this, ike_sa,
								 child_sa->get_config(child_sa), remote);
				break;
			case NARROW_RESPONDER_POST:
				narrow_responder_post(child_sa->get_config(child_sa), local);
				break;
			default:
				break;
		}
	}
	return TRUE;
}

METHOD(unity_narrow_t, destroy, void,
	private_unity_narrow_t *this)
{
	free(this);
}

/**
 * See header
 */
unity_narrow_t *unity_narrow_create(unity_handler_t *handler)
{
	private_unity_narrow_t *this;

	INIT(this,
		.public = {
			.listener = {
				.narrow = _narrow,
			},
			.destroy = _destroy,
		},
		.handler = handler,
	);

	return &this->public;
}
