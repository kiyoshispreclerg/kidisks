/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_LOG_H
#define KD_LOG_H

#include <stdbool.h>

/* Messages go to stderr prefixed with "kidisks: ". */
void kd_log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kd_log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kd_log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Only with -v (kd_log_set_verbose). Not translated: for developers. */
void kd_log_set_verbose(bool on);
void kd_log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
