/*
 * Copyright (c) 2008, 2009, 2010, 2011 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VLOG_H
#define VLOG_H 1

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include "compiler.h"
#include "util.h"

#ifdef  __cplusplus
extern "C" {
#endif

/* Logging severity levels.
 *
 * ovs-appctl(8) defines each of the log levels. */
#define VLOG_LEVELS                             \
    VLOG_LEVEL(OFF, LOG_ALERT)                  \
    VLOG_LEVEL(EMER, LOG_ALERT)                 \
    VLOG_LEVEL(ERR, LOG_ERR)                    \
    VLOG_LEVEL(WARN, LOG_WARNING)               \
    VLOG_LEVEL(INFO, LOG_NOTICE)                \
    VLOG_LEVEL(DBG, LOG_DEBUG)
enum vlog_level {
#define VLOG_LEVEL(NAME, SYSLOG_LEVEL) VLL_##NAME,
    VLOG_LEVELS
#undef VLOG_LEVEL
    VLL_N_LEVELS
};

const char *vlog_get_level_name(enum vlog_level);
enum vlog_level vlog_get_level_val(const char *name);

/* Facilities that we can log to. */
#define VLOG_FACILITIES                                         \
    VLOG_FACILITY(SYSLOG, "%05N|%c|%p|%m")                      \
    VLOG_FACILITY(CONSOLE, "%D{%Y-%m-%dT%H:%M:%SZ}|%05N|%c|%p|%m")  \
    VLOG_FACILITY(FILE, "%D{%Y-%m-%dT%H:%M:%SZ}|%05N|%c|%p|%m")
enum vlog_facility {
#define VLOG_FACILITY(NAME, PATTERN) VLF_##NAME,
    VLOG_FACILITIES
#undef VLOG_FACILITY
    VLF_N_FACILITIES,
    VLF_ANY_FACILITY = -1
};

const char *vlog_get_facility_name(enum vlog_facility);
enum vlog_facility vlog_get_facility_val(const char *name);

/* A log module. */
struct vlog_module {
    const char *name;             /* User-visible name. */
    int levels[VLF_N_FACILITIES]; /* Minimum log level for each facility. */
    int min_level;                /* Minimum log level for any facility. */
};

/* Creates and initializes a global instance of a module named MODULE. */
#if USE_LINKER_SECTIONS
#define VLOG_DEFINE_MODULE(MODULE)                                      \
        VLOG_DEFINE_MODULE__(MODULE)                                    \
        extern struct vlog_module *vlog_module_ptr_##MODULE;            \
        struct vlog_module *vlog_module_ptr_##MODULE                    \
            __attribute__((section("vlog_modules"))) = &VLM_##MODULE
#else
#define VLOG_DEFINE_MODULE(MODULE) extern struct vlog_module VLM_##MODULE
#endif

const char *vlog_get_module_name(const struct vlog_module *);
struct vlog_module *vlog_module_from_name(const char *name);

/* Rate-limiter for log messages. */
struct vlog_rate_limit {
    /* Configuration settings. */
    unsigned int rate;          /* Tokens per second. */
    unsigned int burst;         /* Max cumulative tokens credit. */

    /* Current status. */
    unsigned int tokens;        /* Current number of tokens. */
    time_t last_fill;           /* Last time tokens added. */
    time_t first_dropped;       /* Time first message was dropped. */
    time_t last_dropped;        /* Time of most recent message drop. */
    unsigned int n_dropped;     /* Number of messages dropped. */
};

/* Number of tokens to emit a message.  We add 'rate' tokens per second, which
 * is 60 times the unit used for 'rate', thus 60 tokens are required to emit
 * one message. */
#define VLOG_MSG_TOKENS 60

/* Initializer for a struct vlog_rate_limit, to set up a maximum rate of RATE
 * messages per minute and a maximum burst size of BURST messages. */
#define VLOG_RATE_LIMIT_INIT(RATE, BURST)                   \
        {                                                   \
            RATE,                           /* rate */      \
            (MIN(BURST, UINT_MAX / VLOG_MSG_TOKENS)         \
             * VLOG_MSG_TOKENS),            /* burst */     \
            0,                              /* tokens */    \
            0,                              /* last_fill */ \
            0,                              /* first_dropped */ \
            0,                              /* last_dropped */ \
            0,                              /* n_dropped */ \
        }

/* Configuring how each module logs messages. */
enum vlog_level vlog_get_level(const struct vlog_module *, enum vlog_facility);
void vlog_set_levels(struct vlog_module *,
                     enum vlog_facility, enum vlog_level);
char *vlog_set_levels_from_string(const char *);
char *vlog_get_levels(void);
bool vlog_is_enabled(const struct vlog_module *, enum vlog_level);
bool vlog_should_drop(const struct vlog_module *, enum vlog_level,
                      struct vlog_rate_limit *);
void vlog_set_verbosity(const char *arg);

/* Configuring log facilities. */
void vlog_set_pattern(enum vlog_facility, const char *pattern);
const char *vlog_get_log_file(void);
int vlog_set_log_file(const char *file_name);
int vlog_reopen_log_file(void);

/* Initialization. */
void vlog_init(void);
void vlog_exit(void);

/* Functions for actual logging. */
void vlog(const struct vlog_module *, enum vlog_level, const char *format, ...)
    PRINTF_FORMAT (3, 4);
void vlog_valist(const struct vlog_module *, enum vlog_level,
                 const char *, va_list)
    PRINTF_FORMAT (3, 0);

void vlog_fatal(const struct vlog_module *, const char *format, ...)
    PRINTF_FORMAT (2, 3) NO_RETURN;
void vlog_fatal_valist(const struct vlog_module *, const char *format, va_list)
    PRINTF_FORMAT (2, 0) NO_RETURN;

void vlog_rate_limit(const struct vlog_module *, enum vlog_level,
                     struct vlog_rate_limit *, const char *, ...)
    PRINTF_FORMAT (4, 5);

/* Creates and initializes a global instance of a module named MODULE, and
 * defines a static variable named THIS_MODULE that points to it, for use with
 * the convenience macros below. */
#define VLOG_DEFINE_THIS_MODULE(MODULE)                                 \
        VLOG_DEFINE_MODULE(MODULE);                                     \
        static struct vlog_module *const THIS_MODULE = &VLM_##MODULE

/* Convenience macros.  These assume that THIS_MODULE points to a "struct
 * vlog_module" for the current module, as set up by e.g. the
 * VLOG_DEFINE_MODULE macro above.
 *
 * Guaranteed to preserve errno.
 */
#define VLOG_FATAL(...) vlog_fatal(THIS_MODULE, __VA_ARGS__)
#define VLOG_EMER(...) VLOG(VLL_EMER, __VA_ARGS__)
#define VLOG_ERR(...) VLOG(VLL_ERR, __VA_ARGS__)
#define VLOG_WARN(...) VLOG(VLL_WARN, __VA_ARGS__)
#define VLOG_INFO(...) VLOG(VLL_INFO, __VA_ARGS__)
#define VLOG_DBG(...) VLOG(VLL_DBG, __VA_ARGS__)

/* More convenience macros, for testing whether a given level is enabled in
 * THIS_MODULE.  When constructing a log message is expensive, this enables it
 * to be skipped. */
#define VLOG_IS_ERR_ENABLED() vlog_is_enabled(THIS_MODULE, VLL_ERR)
#define VLOG_IS_WARN_ENABLED() vlog_is_enabled(THIS_MODULE, VLL_WARN)
#define VLOG_IS_INFO_ENABLED() vlog_is_enabled(THIS_MODULE, VLL_INFO)
#define VLOG_IS_DBG_ENABLED() vlog_is_enabled(THIS_MODULE, VLL_DBG)

/* Convenience macros for rate-limiting.
 * Guaranteed to preserve errno.
 */
#define VLOG_ERR_RL(RL, ...) VLOG_RL(RL, VLL_ERR, __VA_ARGS__)
#define VLOG_WARN_RL(RL, ...) VLOG_RL(RL, VLL_WARN, __VA_ARGS__)
#define VLOG_INFO_RL(RL, ...) VLOG_RL(RL, VLL_INFO, __VA_ARGS__)
#define VLOG_DBG_RL(RL, ...) VLOG_RL(RL, VLL_DBG, __VA_ARGS__)

#define VLOG_DROP_ERR(RL) vlog_should_drop(THIS_MODULE, VLL_ERR, RL)
#define VLOG_DROP_WARN(RL) vlog_should_drop(THIS_MODULE, VLL_WARN, RL)
#define VLOG_DROP_INFO(RL) vlog_should_drop(THIS_MODULE, VLL_INFO, RL)
#define VLOG_DROP_DBG(RL) vlog_should_drop(THIS_MODULE, VLL_DBG, RL)

/* Macros for logging at most once per execution. */
#define VLOG_ERR_ONCE(...) VLOG_ONCE(VLL_ERR, __VA_ARGS__)
#define VLOG_WARN_ONCE(...) VLOG_ONCE(VLL_WARN, __VA_ARGS__)
#define VLOG_INFO_ONCE(...) VLOG_ONCE(VLL_INFO, __VA_ARGS__)
#define VLOG_DBG_ONCE(...) VLOG_ONCE(VLL_DBG, __VA_ARGS__)

/* Command line processing. */
#define VLOG_OPTION_ENUMS OPT_LOG_FILE
#define VLOG_LONG_OPTIONS                                   \
        {"verbose",     optional_argument, NULL, 'v'},         \
        {"log-file",    optional_argument, NULL, OPT_LOG_FILE}
#define VLOG_OPTION_HANDLERS                    \
        case 'v':                               \
            vlog_set_verbosity(optarg);         \
            break;                              \
        case OPT_LOG_FILE:                      \
            vlog_set_log_file(optarg);          \
            break;
void vlog_usage(void);

/* Implementation details. */
#define VLOG(LEVEL, ...)                                \
    do {                                                \
        enum vlog_level level__ = LEVEL;                \
        if (THIS_MODULE->min_level >= level__) {        \
            vlog(THIS_MODULE, level__, __VA_ARGS__);    \
        }                                               \
    } while (0)
#define VLOG_RL(RL, LEVEL, ...)                                     \
    do {                                                            \
        enum vlog_level level__ = LEVEL;                            \
        if (THIS_MODULE->min_level >= level__) {                    \
            vlog_rate_limit(THIS_MODULE, level__, RL, __VA_ARGS__); \
        }                                                           \
    } while (0)
#define VLOG_ONCE(LEVEL, ...)                       \
    do {                                            \
        static bool already_logged;                 \
        if (!already_logged) {                      \
            already_logged = true;                  \
            vlog(THIS_MODULE, LEVEL, __VA_ARGS__);  \
        }                                           \
    } while (0)

#define VLOG_DEFINE_MODULE__(MODULE)                                    \
        extern struct vlog_module VLM_##MODULE;                         \
        struct vlog_module VLM_##MODULE =                               \
        {                                                               \
            #MODULE,                                      /* name */    \
            { [ 0 ... VLF_N_FACILITIES - 1] = VLL_INFO }, /* levels */  \
            VLL_INFO,                                     /* min_level */ \
        };

#ifdef  __cplusplus
}
#endif


#endif /* vlog.h */
