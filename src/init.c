/*
 * Copyright (c) 2016-2018  Timescale, Inc. All Rights Reserved.
 *
 * This file is licensed under the Apache License,
 * see LICENSE-APACHE at the top level directory.
 */
#include <postgres.h>
#include <pg_config.h>
#include <access/xact.h>
#include <commands/extension.h>
#include <miscadmin.h>
#include <utils/guc.h>

#include "extension.h"
#include "bgw/launcher_interface.h"
#include "guc.h"
#include "catalog.h"
#include "version.h"
#include "compat.h"
#include "config.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

extern void _hypertable_cache_init(void);
extern void _hypertable_cache_fini(void);

extern void _cache_invalidate_init(void);
extern void _cache_invalidate_fini(void);

extern void _cache_init(void);
extern void _cache_fini(void);

extern void _planner_init(void);
extern void _planner_fini(void);

extern void _process_utility_init(void);
extern void _process_utility_fini(void);

extern void _event_trigger_init(void);
extern void _event_trigger_fini(void);

extern void _conn_plain_init();
extern void _conn_plain_fini();

#ifdef TS_USE_OPENSSL
extern void _conn_ssl_init();
extern void _conn_ssl_fini();
#endif

#ifdef TS_DEBUG
extern void _conn_mock_init();
extern void _conn_mock_fini();
#endif

extern void PGDLLEXPORT _PG_init(void);
extern void PGDLLEXPORT _PG_fini(void);

void
_PG_init(void)
{
	/*
	 * Check extension_is loaded to catch certain errors such as calls to
	 * functions defined on the wrong extension version
	 */
	extension_check_version(TIMESCALEDB_VERSION_MOD);
	extension_check_server_version();
	bgw_check_loader_api_version();

	_cache_init();
	_hypertable_cache_init();
	_cache_invalidate_init();
	_planner_init();
	_event_trigger_init();
	_process_utility_init();
	_guc_init();
	_conn_plain_init();
#ifdef TS_USE_OPENSSL
	_conn_ssl_init();
#endif
#ifdef TS_DEBUG
	_conn_mock_init();
#endif
}

void
_PG_fini(void)
{
	/*
	 * Order of items should be strict reverse order of _PG_init. Please
	 * document any exceptions.
	 */
#ifdef TS_DEBUG
	_conn_mock_fini();
#endif
#ifdef TS_USE_OPENSSL
	_conn_ssl_fini();
#endif
	_conn_plain_fini();
	_guc_fini();
	_process_utility_fini();
	_event_trigger_fini();
	_planner_fini();
	_cache_invalidate_fini();
	_hypertable_cache_fini();
	_cache_fini();
}
