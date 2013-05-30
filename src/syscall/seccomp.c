/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2013 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#include "build.h"
#include "arch.h"

#if defined(HAVE_SECCOMP_FILTER)

#include <sys/prctl.h>     /* prctl(2), PR_* */
#include <linux/filter.h>  /* struct sock_*, */
#include <linux/seccomp.h> /* SECCOMP_MODE_FILTER, */
#include <linux/filter.h>  /* struct sock_*, */
#include <linux/audit.h>   /* AUDIT_, */
#include <sys/queue.h>     /* LIST_FOREACH, */
#include <sys/types.h>     /* size_t, */
#include <talloc.h>        /* talloc_*, */
#include <errno.h>         /* E*, */
#include <string.h>        /* memcpy(3), */
#include <stddef.h>        /* offsetof(3), */
#include <stdint.h>        /* uint*_t, UINT*_MAX, */

#include "syscall/seccomp.h"
#include "tracee/tracee.h"
#include "syscall/syscall.h"
#include "extension/extension.h"
#include "notice.h"

#include "compat.h"
#include "attribute.h"

#define DEBUG_FILTER(...) /* fprintf(stderr, __VA_ARGS__) */

/**
 * Allocate an empty @program->filter.  This function returns -errno
 * if an error occurred, otherwise 0.
 */
static int new_program_filter(struct sock_fprog *program)
{
	program->filter = talloc_array(NULL, struct sock_filter, 0);
	if (program->filter == NULL)
		return -ENOMEM;

	program->len = 0;
	return 0;
}

/**
 * Append to @program->filter the given @statements (@nb_statements
 * items).  This function returns -errno if an error occurred,
 * otherwise 0.
 */
static int add_statements(struct sock_fprog *program, size_t nb_statements,
			struct sock_filter *statements)
{
	size_t length;
	void *tmp;
	size_t i;

	length = talloc_array_length(program->filter);
	tmp  = talloc_realloc(NULL, program->filter, struct sock_filter, length + nb_statements);
	if (tmp == NULL)
		return -ENOMEM;
	program->filter = tmp;

	for (i = 0; i < nb_statements; i++, length++)
		memcpy(&program->filter[length], &statements[i], sizeof(struct sock_filter));

	return 0;
}

/**
 * Append to @program->filter the statements required to notify PRoot
 * about the given @syscall made by a tracee, with the given @flag.
 * This function returns -errno if an error occurred, otherwise 0.
 */
static int add_trace_syscall(struct sock_fprog *program, word_t syscall, int flag)
{
	int status;

	/* Sanity check.  */
	if (syscall > UINT32_MAX)
		return -ERANGE;

	#define LENGTH_TRACE_SYSCALL 2
	struct sock_filter statements[LENGTH_TRACE_SYSCALL] = {
		/* Compare the accumulator with the expected syscall:
		 * skip the next statement if not equal.  */
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, syscall, 0, 1),

		/* Notify the tracer.  */
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_TRACE + flag)
	};

	DEBUG_FILTER("FILTER:     trace if syscall == %ld\n", syscall);

	status = add_statements(program, LENGTH_TRACE_SYSCALL, statements);
	if (status < 0)
		return status;

	return 0;
}

/**
 * Append to @program->filter the statements that allow anything (if
 * unfiltered).  Note that @nb_traced_syscalls is used to make a
 * sanity check.  This function returns -errno if an error occurred,
 * otherwise 0.
 */
static int end_arch_section(struct sock_fprog *program, size_t nb_traced_syscalls)
{
	int status;

	#define LENGTH_END_SECTION 1
	struct sock_filter statements[LENGTH_END_SECTION] = {
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW)
	};

	DEBUG_FILTER("FILTER:     allow\n");

	status = add_statements(program, LENGTH_END_SECTION, statements);
	if (status < 0)
		return status;

	/* Sanity check, see start_arch_section().  */
	if (   talloc_array_length(program->filter) - program->len
	    != LENGTH_END_SECTION + nb_traced_syscalls * LENGTH_TRACE_SYSCALL)
		return -ERANGE;

	return 0;
}

/**
 * Append to @program->filter the statements that check the current
 * @architecture.  Note that @nb_traced_syscalls is used to make a
 * sanity check.  This function returns -errno if an error occurred,
 * otherwise 0.
 */
static int start_arch_section(struct sock_fprog *program, uint32_t architecture,
				size_t nb_traced_syscalls)
{
	const size_t arch_offset    = offsetof(struct seccomp_data, arch);
	const size_t syscall_offset = offsetof(struct seccomp_data, nr);
	const size_t section_length = LENGTH_END_SECTION +
					nb_traced_syscalls * LENGTH_TRACE_SYSCALL;
	int status;

	/* Sanity checks.  */
	if (   arch_offset    > UINT32_MAX
	    || syscall_offset > UINT32_MAX
	    || section_length > UINT8_MAX)
		return -ERANGE;

	#define LENGTH_START_SECTION 3
	struct sock_filter statements[LENGTH_START_SECTION] = {
		/* Load the current architecture into the
		 * accumulator.  */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS, arch_offset),

		/* Compare the accumulator with the expected
		 * architecture: skip the following "section" if not
		 * equal.  */
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, architecture, 0, section_length + 1),

		/* Load the current syscall into the accumulator.  */
		BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_offset)
	};

	DEBUG_FILTER("FILTER: if arch == %ld, up to %zdth statement\n",
		architecture, nb_traced_syscalls);

	status = add_statements(program, LENGTH_START_SECTION, statements);
	if (status < 0)
		return status;

	/* See the sanity check in end_arch_section().  */
	program->len = talloc_array_length(program->filter);

	return 0;
}

/**
 * Append to @program->filter the statements that forbid anything (if
 * unfiltered) and update @program->len.  This function returns -errno
 * if an error occurred, otherwise 0.
 */
static int finalize_program_filter(struct sock_fprog *program)
{
	int status;

	#define LENGTH_FINALIZE 1
	struct sock_filter statements[LENGTH_FINALIZE] = {
		BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_KILL)
	};

	DEBUG_FILTER("FILTER: kill\n");

	status = add_statements(program, LENGTH_FINALIZE, statements);
	if (status < 0)
		return status;

	program->len = talloc_array_length(program->filter);

	return 0;
}

/**
 * Free @program->filter and set @program->len to 0.
 */
static void free_program_filter(struct sock_fprog *program)
{
	if (program->filter != NULL)
		TALLOC_FREE(program->filter);
	program->len = 0;
}

/**
 * Assemble the given @filters according to the following pseudo-code,
 * then enabled them for the given @tracee and all of its future
 * children:
 *
 *     for each handled architectures
 *         for each handled syscall
 *             trace
 *         allow
 *     kill
 *
 * This function returns -errno if an error occurred, otherwise 0.
 */
static int set_seccomp_filters(const Tracee *tracee, const Filter *filters)
{
	struct sock_fprog program = { .len = 0, .filter = NULL };
	size_t nb_traced_syscalls;
	size_t i, j;
	int status;

	status = new_program_filter(&program);
	if (status < 0)
		goto end;

	for (i = 0; filters[i].architecture != 0; i++) {
		nb_traced_syscalls = 0;

		/* Pre-compute the number of traced syscalls for this architecture.  */
		for (j = 0; filters[i].syscalls[j].value != SYSCALL_AVOIDER; j++)
			if ((int) filters[i].syscalls[j].value >= 0)
				nb_traced_syscalls++;

		/* Filter: if handled architecture */
		status = start_arch_section(&program, filters[i].architecture, nb_traced_syscalls);
		if (status < 0)
			goto end;

		for (j = 0; filters[i].syscalls[j].value != SYSCALL_AVOIDER; j++) {
			if ((int) filters[i].syscalls[j].value < 0)
				continue;

			/* Filter: trace if handled syscall */
			status = add_trace_syscall(&program, filters[i].syscalls[j].value,
							filters[i].syscalls[j].flag);
			if (status < 0)
				goto end;
		}

		/* Filter: allow untraced syscalls for this architecture */
		status = end_arch_section(&program, nb_traced_syscalls);
		if (status < 0)
			goto end;
	}

	status = finalize_program_filter(&program);
	if (status < 0)
		goto end;

	status = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	if (status < 0) {
		notice(tracee, WARNING, SYSTEM, "prctl(PR_SET_NO_NEW_PRIVS)");
		goto end;
	}

	/* To output this BPF program for debug purpose:
	 *
	 *     write(2, program.filter, program.len * sizeof(struct sock_filter));
	 */

	status = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
	if (status < 0) {
		notice(tracee, WARNING, SYSTEM, "prctl(PR_SET_SECCOMP)");
		goto end;
	}

	status = 0;
end:
	free_program_filter(&program);
	return status;
}

/* List of syscalls handled by PRoot.  */
#if defined(ARCH_X86_64)
static const FilteredSyscall syscalls64[] = {
	#include SYSNUM_HEADER
	#include "syscall/filter.h"
	#include SYSNUM_HEADER3
	#include "syscall/filter.h"
	FILTERED_SYSCALL_END
};

static const FilteredSyscall syscalls32[] = {
	#include SYSNUM_HEADER2
	#include "syscall/filter.h"
	FILTERED_SYSCALL_END
};

static const Filter filters[] = {
	{ .architecture = AUDIT_ARCH_X86_64,
	  .syscalls     = syscalls64 },
	{ .architecture = AUDIT_ARCH_I386,
	  .syscalls     = syscalls32 },
	{ 0 }
};
#elif defined(AUDIT_ARCH_NUM)
static const FilteredSyscall syscalls[] = {
	#include SYSNUM_HEADER
	#include "syscall/filter.h"
	FILTERED_SYSCALL_END
};

static const Filter filters[] = {
	{ .architecture = AUDIT_ARCH_NUM,
	  .syscalls     = syscalls },
	{ 0 }
};
#else
static const Filter filters[] = { 0 };
#endif
#include "syscall/sysnum-undefined.h"

/**
 * Tell the kernel to trace only syscalls handled by PRoot and its
 * extensions.  This filter will be enabled for the given @tracee and
 * all of its future children.  This function returns -errno if an
 * error occurred, otherwise 0.
 */
int enable_syscall_filtering(const Tracee *tracee)
{
	Extension *extension;
	int status;

	status = set_seccomp_filters(tracee, filters);
	if (status < 0)
		return status;

	/* No more filters?  */
	if (tracee->extensions == NULL)
		return 0;

	/* Filters are evaluated with the following precedence order:
	 * KILL, TRAP, DATA, ERRNO, TRACE, then ALLOW.  For details,
	 * see linux/Documentation/prctl/seccomp_filter.txt.  */
	LIST_FOREACH(extension, tracee->extensions, link) {
		if (extension->filters == NULL)
			continue;

		status = set_seccomp_filters(tracee, extension->filters);
		if (status < 0)
			return status;
	}

	return 0;
}

#endif /* defined(HAVE_SECCOMP_FILTER) */
