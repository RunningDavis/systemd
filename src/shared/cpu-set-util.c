/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <syslog.h>

#include "alloc-util.h"
#include "cpu-set-util.h"
#include "extract-word.h"
#include "log.h"
#include "macro.h"
#include "memory-util.h"
#include "parse-util.h"
#include "string-util.h"

char* cpu_set_to_string(const CPUSet *a) {
        _cleanup_free_ char *str = NULL;
        size_t allocated = 0, len = 0;
        int i, r;

        for (i = 0; (size_t) i < a->allocated * 8; i++) {
                if (!CPU_ISSET_S(i, a->allocated, a->set))
                        continue;

                if (!GREEDY_REALLOC(str, allocated, len + 1 + DECIMAL_STR_MAX(int)))
                        return NULL;

                r = sprintf(str + len, len > 0 ? " %d" : "%d", i);
                assert_se(r > 0);
                len += r;
        }

        return TAKE_PTR(str) ?: strdup("");
}

cpu_set_t* cpu_set_malloc(unsigned *ncpus) {
        cpu_set_t *c;
        unsigned n = 1024;

        /* Allocates the cpuset in the right size */

        for (;;) {
                c = CPU_ALLOC(n);
                if (!c)
                        return NULL;

                if (sched_getaffinity(0, CPU_ALLOC_SIZE(n), c) >= 0) {
                        CPU_ZERO_S(CPU_ALLOC_SIZE(n), c);

                        if (ncpus)
                                *ncpus = n;

                        return c;
                }

                CPU_FREE(c);

                if (errno != EINVAL)
                        return NULL;

                n *= 2;
        }
}

static int cpu_set_realloc(CPUSet *cpu_set, unsigned ncpus) {
        size_t need;

        assert(cpu_set);

        need = CPU_ALLOC_SIZE(ncpus);
        if (need > cpu_set->allocated) {
                cpu_set_t *t;

                t = realloc(cpu_set->set, need);
                if (!t)
                        return -ENOMEM;

                memzero((uint8_t*) t + cpu_set->allocated, need - cpu_set->allocated);

                cpu_set->set = t;
                cpu_set->allocated = need;
        }

        return 0;
}

static int cpu_set_add(CPUSet *cpu_set, unsigned cpu) {
        int r;

        if (cpu >= 8192)
                /* As of kernel 5.1, CONFIG_NR_CPUS can be set to 8192 on PowerPC */
                return -ERANGE;

        r = cpu_set_realloc(cpu_set, cpu + 1);
        if (r < 0)
                return r;

        CPU_SET_S(cpu, cpu_set->allocated, cpu_set->set);
        return 0;
}

int cpu_set_add_all(CPUSet *a, const CPUSet *b) {
        int r;

        /* Do this backwards, so if we fail, we fail before changing anything. */
        for (unsigned cpu_p1 = b->allocated * 8; cpu_p1 > 0; cpu_p1--)
                if (CPU_ISSET_S(cpu_p1 - 1, b->allocated, b->set)) {
                        r = cpu_set_add(a, cpu_p1 - 1);
                        if (r < 0)
                                return r;
                }

        return 0;
}

int parse_cpu_set_full(
                const char *rvalue,
                CPUSet *cpu_set,
                bool warn,
                const char *unit,
                const char *filename,
                unsigned line,
                const char *lvalue) {

        _cleanup_(cpu_set_reset) CPUSet c = {};
        const char *p = rvalue;

        assert(p);

        for (;;) {
                _cleanup_free_ char *word = NULL;
                unsigned cpu_lower, cpu_upper;
                int r;

                r = extract_first_word(&p, &word, WHITESPACE ",", EXTRACT_QUOTES);
                if (r == -ENOMEM)
                        return warn ? log_oom() : -ENOMEM;
                if (r < 0)
                        return warn ? log_syntax(unit, LOG_ERR, filename, line, r, "Invalid value for %s: %s", lvalue, rvalue) : r;
                if (r == 0)
                        break;

                r = parse_range(word, &cpu_lower, &cpu_upper);
                if (r < 0)
                        return warn ? log_syntax(unit, LOG_ERR, filename, line, r, "Failed to parse CPU affinity '%s'", word) : r;

                if (cpu_lower > cpu_upper) {
                        if (warn)
                                log_syntax(unit, LOG_WARNING, filename, line, 0, "Range '%s' is invalid, %u > %u, ignoring.",
                                           word, cpu_lower, cpu_upper);

                        /* Make sure something is allocated, to distinguish this from the empty case */
                        r = cpu_set_realloc(&c, 1);
                        if (r < 0)
                                return r;
                }

                for (unsigned cpu_p1 = MIN(cpu_upper, UINT_MAX-1) + 1; cpu_p1 > cpu_lower; cpu_p1--) {
                        r = cpu_set_add(&c, cpu_p1 - 1);
                        if (r < 0)
                                return warn ? log_syntax(unit, LOG_ERR, filename, line, r,
                                                         "Cannot add CPU %u to set: %m", cpu_p1 - 1) : r;
                }
        }

        /* On success, transfer ownership to the output variable */
        *cpu_set = c;
        c = (CPUSet) {};

        return 0;
}

int parse_cpu_set_extend(
                const char *rvalue,
                CPUSet *old,
                bool warn,
                const char *unit,
                const char *filename,
                unsigned line,
                const char *lvalue) {

        _cleanup_(cpu_set_reset) CPUSet cpuset = {};
        int r;

        r = parse_cpu_set_full(rvalue, &cpuset, true, unit, filename, line, lvalue);
        if (r < 0)
                return r;

        if (!cpuset.set) {
                /* An empty assignment resets the CPU list */
                cpu_set_reset(old);
                return 0;
        }

        if (!old->set) {
                *old = cpuset;
                cpuset = (CPUSet) {};
                return 0;
        }

        return cpu_set_add_all(old, &cpuset);
}
