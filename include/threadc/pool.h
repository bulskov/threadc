#pragma once
/* threadc — worker pool: N threads taking jobs from a tc_queue_t.
 * Portable: no OS code of its own.
 *
 * A job is a function and an argument.  It runs on one worker and gets that
 * worker's scratch allocator: a growing arena of the worker's own, reset
 * after every job.  Scratch is for the job's temporary data; results that
 * outlive the job go somewhere the job's argument says (an arena the job
 * fills and hands back, a queue, ...).  No locks are taken to allocate.
 *
 * The owner starts the pool, submits jobs, and stops it: stop runs the jobs
 * still queued, joins the workers and gives everything back.  There is no
 * cancel: a job that should stop early checks a flag in its argument.
 *
 * Memory: the workers' bookkeeping and the job queue come from the caller's
 * allocator in tc_pool_start and go back in tc_pool_stop, both on the
 * owner's thread.  The scratch arenas are the workers' own (OS pages).
 *
 * The struct is visible so it can be embedded anywhere; its fields are
 * private. */

#include "arena/allocator.h"
#include "threadc/queue.h"
#include "threadc/threadc.h"

/* scratch is valid for this call only: it is reset when the job returns. */
typedef void (*tc_job_fn)(void *arg, allocator_t scratch);

typedef struct tc_pool_worker tc_pool_worker_t;

typedef struct
{
    tc_queue_t jobs;
    tc_pool_worker_t *workers;
    int count;
    allocator_t alloc;
} tc_pool_t;

/* Starts workers threads (0: one per logical CPU) named name, with room
 * for queue_capacity waiting jobs.  scratch_block is the size each scratch
 * arena grows by (0: 64 KiB); a job may allocate more than that at once.
 *
 * TC_INVALID for a NULL p, a negative workers or a zero queue_capacity;
 * TC_RESOURCE when a has no room or the OS refuses a thread.  On failure
 * no thread is left running and p needs no stop.
 *
 * The workers keep a pointer to *p: do not move or copy the tc_pool_t
 * between start and stop. */
tc_err_t tc_pool_start(
    tc_pool_t *p,
    int workers,
    size_t queue_capacity,
    size_t scratch_block,
    allocator_t a,
    string_t name);

/* Runs the jobs still queued, joins the workers and gives back what start
 * took.  Call it on the owner's thread, never from a job. */
void tc_pool_stop(tc_pool_t *p);

/* Queues fn(arg, scratch), waiting while the queue is full.  False when the
 * pool is stopping: the job will not run.
 *
 * A job may submit more jobs, but must not wait for room: when every
 * worker does that at once, nobody is left to make room.  From a job, use
 * tc_pool_submit_timeout with 0 and handle TC_TIMEOUT. */
bool tc_pool_submit(tc_pool_t *p, tc_job_fn fn, void *arg);

/* As tc_pool_submit, for at most timeout_ns: TC_OK, TC_TIMEOUT (queue still
 * full) or TC_CLOSED (stopping).  0 only tries; TC_FOREVER waits with no
 * limit. */
tc_err_kind_t tc_pool_submit_timeout(
    tc_pool_t *p, tc_job_fn fn, void *arg, uint64_t timeout_ns);

/* Number of worker threads. */
int tc_pool_size(const tc_pool_t *p);
