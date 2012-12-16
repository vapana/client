/*
 * Copyright (C) 2009 Tobias Brunner
 * Copyright (C) 2008 Martin Willi
 * Hochschule fuer Technik Rapperswil
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

#include "library.h"

#include <stdlib.h>

#include <utils/debug.h>
#include <threading/thread.h>
#include <utils/identification.h>
#include <networking/host.h>
#include <collections/hashtable.h>
#include <utils/backtrace.h>
#include <selectors/traffic_selector.h>

#define CHECKSUM_LIBRARY IPSEC_LIB_DIR"/libchecksum.so"

typedef struct private_library_t private_library_t;

/**
 * private data of library
 */
struct private_library_t {

	/**
	 * public functions
	 */
	library_t public;

	/**
	 * Hashtable with registered objects (name => object)
	 */
	hashtable_t *objects;

	/**
	 * Integrity check failed?
	 */
	bool integrity_failed;

	/**
	 * Number of times we have been initialized
	 */
	refcount_t ref;
};

/**
 * library instance
 */
library_t *lib = NULL;

/**
 * Deinitialize library
 */
void library_deinit()
{
	private_library_t *this = (private_library_t*)lib;
	bool detailed;

	if (!this || !ref_put(&this->ref))
	{	/* have more users */
		return;
	}

	detailed = lib->settings->get_bool(lib->settings,
								"libstrongswan.leak_detective.detailed", TRUE);

	/* make sure the cache is clear before unloading plugins */
	lib->credmgr->flush_cache(lib->credmgr, CERT_ANY);

	this->public.scheduler->destroy(this->public.scheduler);
	this->public.processor->destroy(this->public.processor);
	this->public.plugins->destroy(this->public.plugins);
	this->public.hosts->destroy(this->public.hosts);
	this->public.settings->destroy(this->public.settings);
	this->public.credmgr->destroy(this->public.credmgr);
	this->public.creds->destroy(this->public.creds);
	this->public.encoding->destroy(this->public.encoding);
	this->public.crypto->destroy(this->public.crypto);
	this->public.proposal->destroy(this->public.proposal);
	this->public.fetcher->destroy(this->public.fetcher);
	this->public.db->destroy(this->public.db);
	this->public.printf_hook->destroy(this->public.printf_hook);
	this->objects->destroy(this->objects);
	if (this->public.integrity)
	{
		this->public.integrity->destroy(this->public.integrity);
	}

	if (lib->leak_detective)
	{
		lib->leak_detective->report(lib->leak_detective, detailed);
		lib->leak_detective->destroy(lib->leak_detective);
	}

	threads_deinit();
	backtrace_deinit();

	free(this);
	lib = NULL;
}

METHOD(library_t, get, void*,
	private_library_t *this, char *name)
{
	return this->objects->get(this->objects, name);
}

METHOD(library_t, set, bool,
	private_library_t *this, char *name, void *object)
{
	if (object)
	{
		if (this->objects->get(this->objects, name))
		{
			return FALSE;
		}
		this->objects->put(this->objects, name, object);
		return TRUE;
	}
	return this->objects->remove(this->objects, name) != NULL;
}

/**
 * Hashtable hash function
 */
static u_int hash(char *key)
{
	return chunk_hash(chunk_create(key, strlen(key)));
}

/**
 * Hashtable equals function
 */
static bool equals(char *a, char *b)
{
	return streq(a, b);
}

/*
 * see header file
 */
bool library_init(char *settings)
{
	private_library_t *this;
	printf_hook_t *pfh;

	if (lib)
	{	/* already initialized, increase refcount */
		this = (private_library_t*)lib;
		ref_get(&this->ref);
		return !this->integrity_failed;
	}

	INIT(this,
		.public = {
			.get = _get,
			.set = _set,
		},
		.ref = 1,
	);
	lib = &this->public;

	backtrace_init();
	threads_init();

#ifdef LEAK_DETECTIVE
	lib->leak_detective = leak_detective_create();
#endif /* LEAK_DETECTIVE */

	pfh = printf_hook_create();
	this->public.printf_hook = pfh;

	pfh->add_handler(pfh, 'b', mem_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_INT,
					 PRINTF_HOOK_ARGTYPE_END);
	pfh->add_handler(pfh, 'B', chunk_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_END);
	pfh->add_handler(pfh, 'H', host_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_END);
	pfh->add_handler(pfh, 'N', enum_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_INT,
					 PRINTF_HOOK_ARGTYPE_END);
	pfh->add_handler(pfh, 'T', time_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_INT,
					 PRINTF_HOOK_ARGTYPE_END);
	pfh->add_handler(pfh, 'V', time_delta_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_POINTER,
					 PRINTF_HOOK_ARGTYPE_END);
	pfh->add_handler(pfh, 'Y', identification_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_END);
	pfh->add_handler(pfh, 'R', traffic_selector_printf_hook,
					 PRINTF_HOOK_ARGTYPE_POINTER, PRINTF_HOOK_ARGTYPE_END);

	this->objects = hashtable_create((hashtable_hash_t)hash,
									 (hashtable_equals_t)equals, 4);
	this->public.settings = settings_create(settings);
	this->public.hosts = host_resolver_create();
	this->public.proposal = proposal_keywords_create();
	this->public.crypto = crypto_factory_create();
	this->public.creds = credential_factory_create();
	this->public.credmgr = credential_manager_create();
	this->public.encoding = cred_encoding_create();
	this->public.fetcher = fetcher_manager_create();
	this->public.db = database_factory_create();
	this->public.processor = processor_create();
	this->public.scheduler = scheduler_create();
	this->public.plugins = plugin_loader_create();

	if (lib->settings->get_bool(lib->settings,
								"libstrongswan.integrity_test", FALSE))
	{
#ifdef INTEGRITY_TEST
		this->public.integrity = integrity_checker_create(CHECKSUM_LIBRARY);
		if (!lib->integrity->check(lib->integrity, "libstrongswan", library_init))
		{
			DBG1(DBG_LIB, "integrity check of libstrongswan failed");
			this->integrity_failed = TRUE;
		}
#else /* !INTEGRITY_TEST */
		DBG1(DBG_LIB, "integrity test enabled, but not supported");
		this->integrity_failed = TRUE;
#endif /* INTEGRITY_TEST */
	}

	return !this->integrity_failed;
}

