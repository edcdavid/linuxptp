/**
 * @file metrics.h
 * @brief PTP metrics reporting via socket with TinyExpr edge-triggered reporting
 * @note Copyright (C) 2024 Linuxptp Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * EDGE-TRIGGERED REPORTING:
 * This module uses TinyExpr mathematical expressions for edge-triggered reporting.
 * Metrics are only reported when the expression result changes state (true→false or false→true).
 * TinyExpr is required for compilation.
 * 
 * Available variables for clock summary metrics:
 *   - rms: RMS offset value
 *   - max: Maximum absolute offset  
 *   - freq: Frequency estimate mean
 *   - freq_stddev: Frequency standard deviation
 *   - delay: Path delay mean
 *   - delay_stddev: Path delay standard deviation
 *   - timestamp: Unix timestamp (seconds)
 *
 * Available variables for clock metrics:
 *   - offset: Current offset value
 *   - freq: Current frequency adjustment
 *   - delay: Current path delay measurement
 *   - timestamp: Unix timestamp (seconds)
 *
 * Available variables for state change events:
 *   - port: Port number (for tracking states per port)
 *   - timestamp: Unix timestamp (seconds)
 *   - from_state: Hash of previous state string
 *   - to_state: Hash of new state string  
 *   - event: FSM event ID number
 *
 * Example expressions for edge-triggered reporting:
 *   Clock Summary: "rms > 100 || abs(freq) > 1000" (report when threshold is crossed)
 *   Clock: "abs(offset) > 50" (report when offset threshold is crossed) 
 *   State: "port == 1 && event >= 6" (report fault events for specific port)
 *
 * String hash values for common states:
 *   SLAVE = 2615068233, MASTER = 3984463656, LISTENING = 1836542579
 */

#ifndef HAVE_METRICS_H
#define HAVE_METRICS_H

#include "fsm.h"

struct metrics_reporter;

/**
 * Initialize metrics reporting
 * @param socket_path Path to Unix domain socket for metrics output
 * @return Pointer to metrics reporter or NULL on failure
 */
struct metrics_reporter *metrics_init(const char *socket_path);

/**
 * Initialize metrics reporting with edge-triggered reporting
 * @param socket_path Path to Unix domain socket for metrics output
 * @param clock_summary_filter Expression for edge-triggered clock summary reporting (NULL = always report)
 * @param clock_filter Expression for edge-triggered clock reporting (NULL = always report)
 * @param state_filter Expression for edge-triggered state change reporting (NULL = always report)
 * @return Pointer to metrics reporter or NULL on failure
 */
struct metrics_reporter *metrics_init_with_filters(const char *socket_path,
                                                  const char *clock_summary_filter,
                                                  const char *clock_filter, 
                                                  const char *state_filter);

/**
 * Clean up metrics reporting
 * @param m Metrics reporter to clean up
 */
void metrics_cleanup(struct metrics_reporter *m);

/**
 * Report clock summary metrics (overall PTP performance)
 * @param m Metrics reporter
 * @param rms RMS offset
 * @param max_abs Maximum absolute offset
 * @param freq_mean Frequency estimate mean
 * @param freq_stddev Frequency standard deviation
 * @param delay_mean Path delay mean
 * @param delay_stddev Path delay standard deviation
 */
void metrics_report_clock_summary(struct metrics_reporter *m, double rms, double max_abs,
                           double freq_mean, double freq_stddev, 
                           double delay_mean, double delay_stddev);

/**
 * Report real-time clock metrics (instantaneous values only)
 * @param m Metrics reporter
 * @param offset Current offset value
 * @param freq Current frequency adjustment
 * @param delay Current path delay measurement
 */
void metrics_report_clock(struct metrics_reporter *m, 
                         double offset, double freq, double delay);

/**
 * Report port state change events
 * @param m Metrics reporter
 * @param port_number Port number
 * @param from_state Previous state
 * @param to_state New state
 * @param event Event that triggered the change
 */
void metrics_report_state_change(struct metrics_reporter *m, int port_number,
                                const char *from_state, const char *to_state,
                                enum fsm_event event);

#endif 