/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * We maintain a number of worker thread pools, to spread lock contention.
 *
 * Pools can be added on the fly, as a means to mitigate lock contention,
 * but can only be removed again by a restart. (XXX: we could fix that)
 *
 * Two threads herd the pools, one eliminates idle threads and aggregates
 * statistics for all the pools, the other thread creates new threads
 * on demand, subject to various numerical constraints.
 *
 * The algorithm for when to create threads needs to be reactive enough
 * to handle startup spikes, but sufficiently attenuated to not cause
 * thread pileups.  This remains subject for improvement.
 */

#include "config.h"

#include <stdio.h>

#include "mgt/mgt.h"

#include "mgt/mgt_param.h"

/*--------------------------------------------------------------------
 * The min/max values automatically update the opposites appropriate
 * limit, so they don't end up crossing.
 */

static int
tweak_thread_pool_min(struct vsb *vsb, const struct parspec *par,
    const char *arg)
{
	if (tweak_uint(vsb, par, arg))
		return (-1);

	MCF_ParamConf(MCF_MINIMUM, "thread_pool_max",
	    "%u", mgt_param.wthread_min);
	MCF_ParamConf(MCF_MAXIMUM, "thread_pool_reserve",
	    "%u", mgt_param.wthread_min * 950 / 1000);
	return (0);
}

static int
tweak_thread_pool_max(struct vsb *vsb, const struct parspec *par,
    const char *arg)
{

	if (tweak_uint(vsb, par, arg))
		return (-1);

	MCF_ParamConf(MCF_MAXIMUM, "thread_pool_min",
	    "%u", mgt_param.wthread_max);
	return (0);
}

/*--------------------------------------------------------------------
 * The thread pool parameter definitions used to generate the varnishd
 * manual. Check the generated RST after updating.
 */

struct parspec WRK_parspec[] = {
	{ "thread_pools", tweak_uint, &mgt_param.wthread_pools,
		"1", NULL, /* maximum defined in mgt_param.c */ "2",
		"pools",
		"Number of worker thread pools.\n"
		"\n"
		"Increasing the number of worker pools decreases lock "
		"contention. Each worker pool also has a thread accepting "
		"new connections, so for very high rates of incoming new "
		"connections on systems with many cores, increasing the "
		"worker pools may be required.\n"
		"\n"
		"Too many pools waste CPU and RAM resources, and more than one "
		"pool for each CPU is most likely detrimental to performance.\n"
		"\n"
		"Can be increased on the fly, but decreases require a "
		"restart to take effect, unless the drop_pools experimental "
		"debug flag is set.",
		EXPERIMENTAL | DELAYED_EFFECT,
		NULL, "defined when Varnish is built" },
	{ "thread_pool_max", tweak_thread_pool_max, &mgt_param.wthread_max,
		NULL, NULL, "5000",
		"threads",
		"The maximum number of worker threads in each pool.\n"
		"\n"
		"Do not set this higher than you have to, since excess "
		"worker threads soak up RAM and CPU and generally just get "
		"in the way of getting work done.",
		DELAYED_EFFECT,
		"thread_pool_min" },
	{ "thread_pool_min", tweak_thread_pool_min, &mgt_param.wthread_min,
		"5" /* TASK_QUEUE__END */, NULL, "100",
		"threads",
		"The minimum number of worker threads in each pool.\n"
		"\n"
		"Increasing this may help ramp up faster from low load "
		"situations or when threads have expired.\n"
		"\n"
		"Technical minimum is 5 threads, " // TASK_QUEUE__END
		"but this parameter is strongly recommended to be "
		"at least 10", // 2 * TASK_QUEUE__END
		DELAYED_EFFECT,
		NULL, "thread_pool_max" },
	{ "thread_pool_reserve", tweak_uint,
		&mgt_param.wthread_reserve,
		NULL, NULL, "0",
		"threads",
		"The number of worker threads reserved for vital tasks "
		"in each pool.\n"
		"\n"
		"Tasks may require other tasks to complete (for example, "
		"client requests may require backend requests, http2 sessions "
		"require streams, which require requests). This reserve is to "
		"ensure that lower priority tasks do not prevent higher "
		"priority tasks from running even under high load.\n"
		"\n"
		"The effective value is at least 5 (the number of internal "
		//				 ^ TASK_QUEUE__END
		"priority classes), irrespective of this parameter.\n"
		"Default is 0 to auto-tune (5% of thread_pool_min).\n"
		"Minimum is 1 otherwise, maximum is 95% of thread_pool_min.",
		DELAYED_EFFECT,
		NULL, "95% of thread_pool_min" },
	{ "thread_pool_timeout",
		tweak_timeout, &mgt_param.wthread_timeout,
		"10", NULL, "300",
		"seconds",
		"Thread idle threshold.\n"
		"\n"
		"Threads in excess of thread_pool_min, which have been idle "
		"for at least this long, will be destroyed.",
		EXPERIMENTAL | DELAYED_EFFECT },
	{ "thread_pool_watchdog",
		tweak_timeout, &mgt_param.wthread_watchdog,
		"0.1", NULL, "60",
		"seconds",
		"Thread queue stuck watchdog.\n"
		"\n"
		"If no queued work have been released for this long,"
		" the worker process panics itself.",
		EXPERIMENTAL },
	{ "thread_pool_destroy_delay",
		tweak_timeout, &mgt_param.wthread_destroy_delay,
		"0.01", NULL, "1",
		"seconds",
		"Wait this long after destroying a thread.\n"
		"\n"
		"This controls the decay of thread pools when idle(-ish).",
		EXPERIMENTAL | DELAYED_EFFECT },
	{ "thread_pool_add_delay",
		tweak_timeout, &mgt_param.wthread_add_delay,
		"0", NULL, "0",
		"seconds",
		"Wait at least this long after creating a thread.\n"
		"\n"
		"Some (buggy) systems may need a short (sub-second) "
		"delay between creating threads.\n"
		"Set this to a few milliseconds if you see the "
		"'threads_failed' counter grow too much.\n"
		"\n"
		"Setting this too high results in insufficient worker threads.",
		EXPERIMENTAL },
	{ "thread_pool_fail_delay",
		tweak_timeout, &mgt_param.wthread_fail_delay,
		"10e-3", NULL, "0.2",
		"seconds",
		"Wait at least this long after a failed thread creation "
		"before trying to create another thread.\n"
		"\n"
		"Failure to create a worker thread is often a sign that "
		" the end is near, because the process is running out of "
		"some resource.  "
		"This delay tries to not rush the end on needlessly.\n"
		"\n"
		"If thread creation failures are a problem, check that "
		"thread_pool_max is not too high.\n"
		"\n"
		"It may also help to increase thread_pool_timeout and "
		"thread_pool_min, to reduce the rate at which treads are "
		"destroyed and later recreated.",
		EXPERIMENTAL },
	{ "thread_stats_rate",
		tweak_uint, &mgt_param.wthread_stats_rate,
		"0", NULL, "10",
		"requests",
		"Worker threads accumulate statistics, and dump these into "
		"the global stats counters if the lock is free when they "
		"finish a job (request/fetch etc.)\n"
		"This parameters defines the maximum number of jobs "
		"a worker thread may handle, before it is forced to dump "
		"its accumulated stats into the global counters.",
		EXPERIMENTAL },
	{ "thread_queue_limit", tweak_uint, &mgt_param.wthread_queue_limit,
		"0", NULL, "20",
		NULL,
		"Permitted request queue length per thread-pool.\n"
		"\n"
		"This sets the number of requests we will queue, waiting "
		"for an available thread.  Above this limit sessions will "
		"be dropped instead of queued.",
		EXPERIMENTAL },
	{ "thread_pool_stack",
		tweak_bytes, &mgt_param.wthread_stacksize,
		NULL, NULL, NULL,	// default set in mgt_param.c
		"bytes",
		"Worker thread stack size.\n"
		"This will likely be rounded up to a multiple of 4k"
		" (or whatever the page_size might be) by the kernel.\n"
		"\n"
		"The required stack size is primarily driven by the"
		" depth of the call-tree. The most common relevant"
		" determining factors in varnish core code are GZIP"
		" (un)compression, ESI processing and regular"
		" expression matches. VMODs may also require"
		" significant amounts of additional stack. The"
		" nesting depth of VCL subs is another factor,"
		" although typically not predominant.\n"
		"\n"
		"The stack size is per thread, so the maximum total"
		" memory required for worker thread stacks is in the"
		" order of size = thread_pools x thread_pool_max x"
		" thread_pool_stack.\n"
		"\n"
		"Thus, in particular for setups with many threads,"
		" keeping the stack size at a minimum helps reduce"
		" the amount of memory required by Varnish.\n"
		"\n"
		"On the other hand, thread_pool_stack must be large"
		" enough under all circumstances, otherwise varnish"
		" will crash due to a stack overflow. Usually, a"
		" stack overflow manifests itself as a segmentation"
		" fault (aka segfault / SIGSEGV) with the faulting"
		" address being near the stack pointer (sp).\n"
		"\n"
		"Unless stack usage can be reduced,"
		" thread_pool_stack must be increased when a stack"
		" overflow occurs. Setting it in 150%-200%"
		" increments is recommended until stack overflows"
		" cease to occur.",
		DELAYED_EFFECT,
		NULL, NULL, "sysconf(_SC_THREAD_STACK_MIN)" },
	{ NULL, NULL, NULL }
};
