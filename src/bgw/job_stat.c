/*
 * Copyright (c) 2016-2018  Timescale, Inc. All Rights Reserved.
 *
 * This file is licensed under the Apache License,
 * see LICENSE-APACHE at the top level directory.
 */
#include <postgres.h>
#include <access/xact.h>

#include "job_stat.h"
#include "scanner.h"
#include "compat.h"
#include "timer.h"
#include "utils.h"

#if PG10
#include <utils/fmgrprotos.h>
#endif
#define MAX_INTERVALS_BACKOFF 5
#define MIN_WAIT_AFTER_CRASH_MS (5 * 60 * 1000)

static bool
bgw_job_stat_next_start_was_set(FormData_bgw_job_stat *fd)
{
	return fd->next_start != DT_NOBEGIN;
}


static bool
bgw_job_stat_tuple_found(TupleInfo *ti, void *const data)
{
	BgwJobStat **job_stat_pp = data;

	*job_stat_pp = STRUCT_FROM_TUPLE(ti->tuple, ti->mctx, BgwJobStat, FormData_bgw_job_stat);

	/* return true because used with scan_one */
	return true;
}

static bool
bgw_job_stat_scan_one(int indexid, ScanKeyData scankey[], int nkeys,
					  tuple_found_func tuple_found, tuple_filter_func tuple_filter, void *data, LOCKMODE lockmode)
{
	Catalog    *catalog = catalog_get();
	ScannerCtx	scanctx = {
		.table = catalog->tables[BGW_JOB_STAT].id,
		.index = CATALOG_INDEX(catalog, BGW_JOB_STAT, indexid),
		.nkeys = nkeys,
		.scankey = scankey,
		.tuple_found = tuple_found,
		.filter = tuple_filter,
		.data = data,
		.lockmode = lockmode,
		.scandirection = ForwardScanDirection,
	};

	return scanner_scan_one(&scanctx, false, "bgw job stat");
}

static inline bool
bgw_job_stat_scan_job_id(int32 bgw_job_id, tuple_found_func tuple_found, tuple_filter_func tuple_filter, void *data, LOCKMODE lockmode)
{
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0],
				Anum_bgw_job_stat_pkey_idx_job_id,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(bgw_job_id));
	return bgw_job_stat_scan_one(BGW_JOB_STAT_PKEY_IDX,
								 scankey, 1, tuple_found, tuple_filter, data, lockmode);
}

BgwJobStat *
bgw_job_stat_find(int32 bgw_job_id)
{
	BgwJobStat *job_stat = NULL;

	bgw_job_stat_scan_job_id(bgw_job_id, bgw_job_stat_tuple_found, NULL, &job_stat, AccessShareLock);

	return job_stat;
}

/* Mark the start of a job. This should be done in a separate transaction by the scheduler
*  before the bgw for a job is launched. This ensures that the job is counted as started
* before /any/ job specific code is executed. A job that has been started but never ended
* is assumed to have crashed. We use this conservative design since no process in the database
* instance can write once a crash happened in any job. Therefore our only choice is to deduce
* a crash from the lack of a write (the marked end write in this case).
*/
static bool
bgw_job_stat_tuple_mark_start(TupleInfo *ti, void *const data)
{
	HeapTuple	tuple = heap_copytuple(ti->tuple);
	FormData_bgw_job_stat *fd = (FormData_bgw_job_stat *) GETSTRUCT(tuple);

	fd->last_start = timer_get_current_timestamp();
	fd->last_finish = DT_NOBEGIN;
	fd->next_start = DT_NOBEGIN;

	fd->total_runs++;

	/*
	 * This is undone by any of the end marks. This is so that we count
	 * crashes conservatively. Pretty much the crash is incremented in the
	 * beginning and then decremented during `bgw_job_stat_tuple_mark_end`.
	 * Thus, it only remains incremented if the job is never marked as having
	 * ended. This happens when: 1) the job crashes 2) another process crashes
	 * while the job is running 3) the scheduler gets a SIGTERM while the job
	 * is running
	 *
	 * Unfortunately, 3 cannot be helped because when a scheduler gets a
	 * SIGTERM it sends SIGTERMS to it's any running jobs as well. Since you
	 * aren't supposed to write to the DB Once you get a sigterm, neither the
	 * job nor the scheduler can mark the end of a job.
	 */
	fd->last_run_success = false;
	fd->total_crashes++;
	fd->consecutive_crashes++;

	catalog_update(ti->scanrel, tuple);
	heap_freetuple(tuple);

	/* scans with catalog_update must return false */
	return false;
}


typedef struct
{
	JobResult	result;
	BgwJob	   *job;
} JobResultCtx;


static
TimestampTz
calculate_next_start_on_success(TimestampTz last_finish, BgwJob *job)
{
	/* TODO: add randomness here? Do we need a range or just a percent? */
	TimestampTz ts = DatumGetTimestampTz(DirectFunctionCall2(timestamptz_pl_interval,
															 TimestampTzGetDatum(last_finish),
															 IntervalPGetDatum(&job->fd.schedule_interval)));

	return ts;
}

/* For failures we have standard exponential backoff based on consecutive failures
 * along with a ceiling at schedule_interval * MAX_INTERVALS_BACKOFF */
static
TimestampTz
calculate_next_start_on_failure(TimestampTz last_finish, int consecutive_failures, BgwJob *job)
{
	/* TODO: add randomness here? Do we need a range or just a percent? */
	/* consecutive failures includes this failure */
	float8		multiplier = 1 << (consecutive_failures - 1);

	/* ival = retry_period * 2^(consecutive_failures - 1)  */
	Datum		ival = DirectFunctionCall2(interval_mul, IntervalPGetDatum(&job->fd.retry_period), Float8GetDatum(multiplier));

	/* ival_max is the ceiling = MAX_INTERVALS_BACKOFF * schedule_interval */
	Datum		ival_max = DirectFunctionCall2(interval_mul, IntervalPGetDatum(&job->fd.schedule_interval), Float8GetDatum(MAX_INTERVALS_BACKOFF));

	if (DatumGetInt32(DirectFunctionCall2(interval_cmp, ival, ival_max)) > 0)
		ival = ival_max;

	return DatumGetTimestampTz(DirectFunctionCall2(timestamptz_pl_interval, TimestampTzGetDatum(last_finish), ival));
}

/* For crashes, the logic is the similar as for failures except we also have
*  a minimum wait after a crash that we wait, so that if an operator needs to disable the job,
*  there will be enough time before another crash.
*/
static TimestampTz
calculate_next_start_on_crash(int consecutive_crashes, BgwJob *job)
{
	TimestampTz now = timer_get_current_timestamp();
	TimestampTz failure_calc = calculate_next_start_on_failure(now, consecutive_crashes, job);
	TimestampTz min_time = TimestampTzPlusMilliseconds(now, MIN_WAIT_AFTER_CRASH_MS);

	if (min_time > failure_calc)
		return min_time;
	return failure_calc;
}

static bool
bgw_job_stat_tuple_mark_end(TupleInfo *ti, void *const data)
{
	JobResultCtx *result_ctx = data;
	HeapTuple	tuple = heap_copytuple(ti->tuple);
	FormData_bgw_job_stat *fd = (FormData_bgw_job_stat *) GETSTRUCT(tuple);
	Interval   *duration;

	fd->last_finish = timer_get_current_timestamp();

	duration = DatumGetIntervalP(DirectFunctionCall2(timestamp_mi, TimestampTzGetDatum(fd->last_finish), TimestampTzGetDatum(fd->last_start)));
	fd->total_duration = *DatumGetIntervalP(DirectFunctionCall2(interval_pl, IntervalPGetDatum(&fd->total_duration), IntervalPGetDatum(duration)));

	/* undo marking created by start marks */
	fd->last_run_success = result_ctx->result == JOB_SUCCESS ? true : false;
	fd->total_crashes--;
	fd->consecutive_crashes = 0;

	if (result_ctx->result == JOB_SUCCESS)
	{
		fd->total_success++;
		fd->consecutive_failures = 0;
		/* Mark the next start at the end if the job itself hasn't */
		if (!bgw_job_stat_next_start_was_set(fd))
			fd->next_start = calculate_next_start_on_success(fd->last_finish, result_ctx->job);
	}
	else
	{
		fd->total_failures++;
		fd->consecutive_failures++;

		/*
		 * Mark the next start at the end if the job itself hasn't (this may
		 * have happened before failure)
		 */
		if (!bgw_job_stat_next_start_was_set(fd))
			fd->next_start = calculate_next_start_on_failure(fd->last_finish, fd->consecutive_failures, result_ctx->job);
	}

	catalog_update(ti->scanrel, tuple);
	heap_freetuple(tuple);

	/* scans with catalog_update must return false */
	return false;
}

static bool
bgw_job_stat_tuple_set_next_start(TupleInfo *ti, void *const data)
{
	TimestampTz *next_start = data;
	HeapTuple	tuple = heap_copytuple(ti->tuple);
	FormData_bgw_job_stat *fd = (FormData_bgw_job_stat *) GETSTRUCT(tuple);

	fd->next_start = *next_start;

	catalog_update(ti->scanrel, tuple);
	heap_freetuple(tuple);

	/* scans with catalog_update must return false */
	return false;
}

static bool
bgw_job_stat_insert_mark_start_relation(Relation rel,
										int32 bgw_job_id)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum		values[Natts_bgw_job_stat];
	bool		nulls[Natts_bgw_job_stat] = {false};
	CatalogSecurityContext sec_ctx;
	Interval	zero_ival = {.time = 0,};

	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_job_id)] = Int32GetDatum(bgw_job_id);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_last_start)] = TimestampGetDatum(timer_get_current_timestamp());
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_last_finish)] = TimestampGetDatum(DT_NOBEGIN);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_next_start)] = TimestampGetDatum(DT_NOBEGIN);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_total_runs)] = Int64GetDatum(1);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_total_duration)] = IntervalPGetDatum(&zero_ival);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_total_success)] = Int64GetDatum(0);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_total_failures)] = Int64GetDatum(0);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_consecutive_failures)] = Int32GetDatum(0);

	/* This is udone by any of the end marks */
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_last_run_success)] = BoolGetDatum(false);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_total_crashes)] = Int64GetDatum(1);
	values[AttrNumberGetAttrOffset(Anum_bgw_job_stat_consecutive_crashes)] = Int32GetDatum(1);

	catalog_become_owner(catalog_get(), &sec_ctx);
	catalog_insert_values(rel, desc, values, nulls);
	catalog_restore_user(&sec_ctx);

	return true;
}

static bool
bgw_job_stat_insert_mark_start(int32 bgw_job_id)
{
	Catalog    *catalog = catalog_get();
	Relation	rel;
	bool		result;

	rel = heap_open(catalog->tables[BGW_JOB_STAT].id, RowExclusiveLock);
	result = bgw_job_stat_insert_mark_start_relation(rel, bgw_job_id);
	heap_close(rel, RowExclusiveLock);

	return result;
}


void
bgw_job_stat_mark_start(int32 bgw_job_id)
{
	if (!bgw_job_stat_scan_job_id(bgw_job_id, bgw_job_stat_tuple_mark_start, NULL, NULL, RowExclusiveLock))
		bgw_job_stat_insert_mark_start(bgw_job_id);

}

void
bgw_job_stat_mark_end(BgwJob *job, JobResult result)
{
	JobResultCtx res =
	{
		.job = job,
		.result = result,
	};

	if (!bgw_job_stat_scan_job_id(job->fd.id, bgw_job_stat_tuple_mark_end, NULL, &res, RowExclusiveLock))
		elog(ERROR, "unable to find job statistics for job %d", job->fd.id);

}

bool
bgw_job_stat_end_was_marked(BgwJobStat *jobstat)
{
	return !TIMESTAMP_IS_NOBEGIN(jobstat->fd.last_finish);
}

void
bgw_job_stat_set_next_start(BgwJob *job, TimestampTz next_start)
{
	/* Cannot use DT_NOBEGIN as that's the value used to indicate "not set" */
	if (next_start == DT_NOBEGIN)
		elog(ERROR, "cannot set next start to -infinity");

	if (!bgw_job_stat_scan_job_id(job->fd.id, bgw_job_stat_tuple_set_next_start, NULL, &next_start, RowExclusiveLock))
		elog(ERROR, "unable to find job statistics for job %d", job->fd.id);

}

bool
bgw_job_stat_should_execute(BgwJobStat *jobstat, BgwJob *job)
{
	/*
	 * Stub to allow the system to disable jobs based on the number of crashes
	 * or failures.
	 */
	return true;
}

TimestampTz
bgw_job_stat_next_start(BgwJobStat *jobstat, BgwJob *job)
{
	if (jobstat == NULL)
		/* Never previously run - run right away */
		return DT_NOBEGIN;

	if (jobstat->fd.consecutive_crashes > 0)
		return calculate_next_start_on_crash(jobstat->fd.consecutive_crashes, job);

	return jobstat->fd.next_start;
}
