/**
 * @file metrics.c
 * @brief PTP metrics reporting via socket
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
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "tinyexpr.h"

#include "metrics.h"
#include "print.h"

struct metrics_reporter {
    int sockfd;
    struct sockaddr_un addr;
    char *socket_path;

    /* Filter expressions */
    char *clock_summary_filter_expr;
    char *clock_filter_expr;
    char *state_filter_expr;

    /* Compiled TinyExpr expressions */
    te_expr *clock_summary_filter;
    te_expr *clock_filter;
    te_expr *state_filter;

    /* Variables for expression evaluation */
    te_variable clock_summary_vars[8];  /* rms, max, freq, freq_stddev, delay, delay_stddev, timestamp, port */
    te_variable clock_vars[5]; /* offset, freq, delay, timestamp */
    te_variable state_vars[6];    /* port, timestamp, from_state, to_state, event, event_id */

    /* Previous states for edge-triggered reporting */
    int clock_summary_prev_state;       /* Previous evaluation result for clock summary filter */
    int clock_prev_state;     /* Previous evaluation result for clock filter */
    int state_prev_state;         /* Previous evaluation result for state filter */

    /* Statistics */
    unsigned long dropped_messages;     /* Count of messages dropped due to full socket buffer */
};

static const char *fsm_event_str[] = {
    [EV_NONE] = "NONE",
    [EV_POWERUP] = "POWERUP",
    [EV_INITIALIZE] = "INITIALIZE",
    [EV_DESIGNATED_ENABLED] = "DESIGNATED_ENABLED",
    [EV_DESIGNATED_DISABLED] = "DESIGNATED_DISABLED",
    [EV_FAULT_CLEARED] = "FAULT_CLEARED",
    [EV_FAULT_DETECTED] = "FAULT_DETECTED",
    [EV_STATE_DECISION_EVENT] = "STATE_DECISION_EVENT",
    [EV_QUALIFICATION_TIMEOUT_EXPIRES] = "QUALIFICATION_TIMEOUT_EXPIRES",
    [EV_ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES] = "ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES",
    [EV_SYNCHRONIZATION_FAULT] = "SYNCHRONIZATION_FAULT",
    [EV_MASTER_CLOCK_SELECTED] = "MASTER_CLOCK_SELECTED",
    [EV_INIT_COMPLETE] = "INIT_COMPLETE",
    [EV_RS_MASTER] = "RS_MASTER",
    [EV_RS_GRAND_MASTER] = "RS_GRAND_MASTER",
    [EV_RS_SLAVE] = "RS_SLAVE",
    [EV_RS_PASSIVE] = "RS_PASSIVE",
};

/* Forward declarations */
static int should_report_clock_summary(struct metrics_reporter *m, double rms, double max_abs,
                               double freq_mean, double freq_stddev,
                               double delay_mean, double delay_stddev, double timestamp);
static int should_report_clock(struct metrics_reporter *m,
                              double offset, double freq, double delay, double timestamp);
static int should_report_state_change(struct metrics_reporter *m, int port_number,
                                     const char *from_state, const char *to_state,
                                     enum fsm_event event, double timestamp);

/* Hash function for string values */
static double string_hash(const char *str) {
    if (!str) return 0.0;
    double hash = 5381.0;
    int c;
    while ((c = *str++)) {
        hash = ((hash * 33.0) + c);
    }
    return hash;
}
/* Initialize TinyExpr variables for clock summary metrics */
static void init_clock_summary_vars(struct metrics_reporter *m) {
    m->clock_summary_vars[0] = (te_variable){"rms", &(double){0}, TE_VARIABLE, NULL};
    m->clock_summary_vars[1] = (te_variable){"max", &(double){0}, TE_VARIABLE, NULL};
    m->clock_summary_vars[2] = (te_variable){"freq", &(double){0}, TE_VARIABLE, NULL};
    m->clock_summary_vars[3] = (te_variable){"freq_stddev", &(double){0}, TE_VARIABLE, NULL};
    m->clock_summary_vars[4] = (te_variable){"delay", &(double){0}, TE_VARIABLE, NULL};
    m->clock_summary_vars[5] = (te_variable){"delay_stddev", &(double){0}, TE_VARIABLE, NULL};
    m->clock_summary_vars[6] = (te_variable){"timestamp", &(double){0}, TE_VARIABLE, NULL};
    m->clock_summary_vars[7] = (te_variable){NULL, NULL, 0, NULL};
}

/* Initialize TinyExpr variables for clock metrics */
static void init_clock_vars(struct metrics_reporter *m) {
    m->clock_vars[0] = (te_variable){"offset", &(double){0}, TE_VARIABLE, NULL};
    m->clock_vars[1] = (te_variable){"freq", &(double){0}, TE_VARIABLE, NULL};
    m->clock_vars[2] = (te_variable){"delay", &(double){0}, TE_VARIABLE, NULL};
    m->clock_vars[3] = (te_variable){"timestamp", &(double){0}, TE_VARIABLE, NULL};
    m->clock_vars[4] = (te_variable){NULL, NULL, 0, NULL};
}

/* Initialize TinyExpr variables for state change events */
static void init_state_vars(struct metrics_reporter *m) {
    m->state_vars[0] = (te_variable){"port", &(double){0}, TE_VARIABLE, NULL};
    m->state_vars[1] = (te_variable){"timestamp", &(double){0}, TE_VARIABLE, NULL};
    m->state_vars[2] = (te_variable){"from_state", &(double){0}, TE_VARIABLE, NULL};
    m->state_vars[3] = (te_variable){"to_state", &(double){0}, TE_VARIABLE, NULL};
    m->state_vars[4] = (te_variable){"event", &(double){0}, TE_VARIABLE, NULL};
    m->state_vars[5] = (te_variable){NULL, NULL, 0, NULL};
}

/* Compile TinyExpr filter expressions */
static int compile_filters(struct metrics_reporter *m) {
    int err = 0;

    if (m->clock_summary_filter_expr) {
        init_clock_summary_vars(m);
        /* Use correct variable count - TinyExpr will include built-in functions automatically */
        m->clock_summary_filter = te_compile(m->clock_summary_filter_expr, m->clock_summary_vars, 7, &err);
        if (err || !m->clock_summary_filter) {
            pr_err("Failed to compile clock summary filter expression: %s (error code: %d)", m->clock_summary_filter_expr, err);
            return -1;
        }
    }

    if (m->clock_filter_expr) {
        init_clock_vars(m);
        m->clock_filter = te_compile(m->clock_filter_expr, m->clock_vars, 4, &err);
        if (err || !m->clock_filter) {
            pr_err("Failed to compile clock filter expression: %s (error code: %d)", m->clock_filter_expr, err);
            return -1;
        }
    }

    if (m->state_filter_expr) {
        init_state_vars(m);
        m->state_filter = te_compile(m->state_filter_expr, m->state_vars, 5, &err);
        if (err || !m->state_filter) {
            pr_err("Failed to compile state filter expression: %s (error code: %d)", m->state_filter_expr, err);
            return -1;
        }
    }

    return 0;
}

/* Clean up compiled expressions */
static void cleanup_filters(struct metrics_reporter *m) {
    if (m->clock_summary_filter) {
        te_free(m->clock_summary_filter);
        m->clock_summary_filter = NULL;
    }
    if (m->clock_filter) {
        te_free(m->clock_filter);
        m->clock_filter = NULL;
    }
    if (m->state_filter) {
        te_free(m->state_filter);
        m->state_filter = NULL;
    }
}

struct metrics_reporter *metrics_init(const char *socket_path)
{
    return metrics_init_with_filters(socket_path, NULL, NULL, NULL);
}

struct metrics_reporter *metrics_init_with_filters(const char *socket_path,
                                                  const char *clock_summary_filter,
                                                  const char *clock_filter,
                                                  const char *state_filter)
{
    struct metrics_reporter *m;

    if (!socket_path) {
        return NULL;
    }

    m = calloc(1, sizeof(*m));
    if (!m) {
        pr_err("failed to allocate metrics reporter");
        return NULL;
    }

    /* Initialize previous states to -1 (unknown) so first evaluation triggers report */
    m->clock_summary_prev_state = -1;
    m->clock_prev_state = -1;
    m->state_prev_state = -1;
    m->dropped_messages = 0;

    m->socket_path = strdup(socket_path);
    if (!m->socket_path) {
        pr_err("failed to allocate socket path");
        free(m);
        return NULL;
    }

    /* Store filter expressions */
    if (clock_summary_filter) {
        m->clock_summary_filter_expr = strdup(clock_summary_filter);
        if (!m->clock_summary_filter_expr) {
            pr_err("failed to allocate clock summary filter expression");
            free(m->socket_path);
            free(m);
            return NULL;
        }
    }

    if (clock_filter) {
        m->clock_filter_expr = strdup(clock_filter);
        if (!m->clock_filter_expr) {
            pr_err("failed to allocate clock filter expression");
            free(m->clock_summary_filter_expr);
            free(m->socket_path);
            free(m);
            return NULL;
        }
    }

    if (state_filter) {
        m->state_filter_expr = strdup(state_filter);
        if (!m->state_filter_expr) {
            pr_err("failed to allocate state filter expression");
            free(m->clock_filter_expr);
            free(m->clock_summary_filter_expr);
            free(m->socket_path);
            free(m);
            return NULL;
        }
    }

    m->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (m->sockfd < 0) {
        pr_err("failed to create metrics socket: %s", strerror(errno));
        free(m->state_filter_expr);
        free(m->clock_filter_expr);
        free(m->clock_summary_filter_expr);
        free(m->socket_path);
        free(m);
        return NULL;
    }

    /* Set socket to non-blocking to prevent ptp4l from blocking on slow readers */
    if (fcntl(m->sockfd, F_SETFL, O_NONBLOCK) < 0) {
        pr_warning("failed to set metrics socket non-blocking: %s", strerror(errno));
        /* Continue anyway - this is not fatal */
    }

    memset(&m->addr, 0, sizeof(m->addr));
    m->addr.sun_family = AF_UNIX;
    strncpy(m->addr.sun_path, socket_path, sizeof(m->addr.sun_path) - 1);

    /* Compile filter expressions */
    if (compile_filters(m) < 0) {
        close(m->sockfd);
        free(m->state_filter_expr);
        free(m->clock_filter_expr);
        free(m->clock_summary_filter_expr);
        free(m->socket_path);
        free(m);
        return NULL;
    }

    /* For UDP sockets, we don't bind - we send to the target address */

    pr_info("metrics reporting initialized to %s", socket_path);
    if (clock_summary_filter) {
        pr_info("clock summary edge-trigger: %s", clock_summary_filter);
    }
    if (clock_filter) {
        pr_info("clock edge-trigger: %s", clock_filter);
    }
    if (state_filter) {
        pr_info("state edge-trigger: %s", state_filter);
    }

    return m;
}

void metrics_cleanup(struct metrics_reporter *m)
{
    if (!m) {
        return;
    }

    cleanup_filters(m);

    if (m->sockfd >= 0) {
        close(m->sockfd);
    }

    free(m->clock_summary_filter_expr);
    free(m->clock_filter_expr);
    free(m->state_filter_expr);
    free(m->socket_path);

    free(m);
}

static void metrics_send(struct metrics_reporter *m, const char *message)
{
    ssize_t len;

    if (!m || m->sockfd < 0 || !message) {
        return;
    }

    len = sendto(m->sockfd, message, strlen(message), 0,
                 (struct sockaddr *)&m->addr, sizeof(m->addr));
    if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Socket buffer full - drop message to avoid blocking ptp4l */
            m->dropped_messages++;
            if (m->dropped_messages % 100 == 1) {
                pr_warning("metrics socket buffer full, %lu messages dropped", m->dropped_messages);
            }
        } else {
            pr_debug("failed to send metric: %s", strerror(errno));
        }
    }
}

void metrics_report_clock_summary(struct metrics_reporter *m, double rms, double max_abs,
                           double freq_mean, double freq_stddev,
                           double delay_mean, double delay_stddev)
{
    char buffer[512];
    struct timespec ts;
    double timestamp;

    if (!m) {
        return;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    timestamp = ts.tv_sec + ts.tv_nsec / 1000000000.0;

    /* Check filter before proceeding */
    if (!should_report_clock_summary(m, rms, max_abs, freq_mean, freq_stddev,
                               delay_mean, delay_stddev, timestamp)) {
        return;
    }

    if (delay_mean >= 0) {
        snprintf(buffer, sizeof(buffer),
            "{"
            "\"type\":\"clock_summary\","
            "\"timestamp\":%ld.%03ld,"
            "\"rms\":%.0f,"
            "\"max\":%.0f,"
            "\"freq\":%.0f,"
            "\"freq_stddev\":%.0f,"
            "\"delay\":%.0f,"
            "\"delay_stddev\":%.0f"
            "}",
            ts.tv_sec, ts.tv_nsec / 1000000,
            rms, max_abs, freq_mean, freq_stddev, delay_mean, delay_stddev);
    } else {
        snprintf(buffer, sizeof(buffer),
            "{"
            "\"type\":\"clock_summary\","
            "\"timestamp\":%ld.%03ld,"
            "\"rms\":%.0f,"
            "\"max\":%.0f,"
            "\"freq\":%.0f,"
            "\"freq_stddev\":%.0f"
            "}",
            ts.tv_sec, ts.tv_nsec / 1000000,
            rms, max_abs, freq_mean, freq_stddev);
    }

    metrics_send(m, buffer);
}

void metrics_report_clock(struct metrics_reporter *m,
                         double offset, double freq, double delay)
{
    char buffer[512];
    struct timespec ts;
    double timestamp;

    if (!m) {
        return;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    timestamp = ts.tv_sec + ts.tv_nsec / 1000000000.0;

    /* Check filter before proceeding */
    if (!should_report_clock(m, offset, freq, delay, timestamp)) {
        return;
    }

    if (delay >= 0) {
        snprintf(buffer, sizeof(buffer),
            "{"
            "\"type\":\"clock\","
            "\"timestamp\":%ld.%03ld,"
            "\"offset\":%.0f,"
            "\"freq\":%.0f,"
            "\"delay\":%.0f"
            "}",
            ts.tv_sec, ts.tv_nsec / 1000000,
            offset, freq, delay);
    } else {
        snprintf(buffer, sizeof(buffer),
            "{"
            "\"type\":\"clock\","
            "\"timestamp\":%ld.%03ld,"
            "\"offset\":%.0f,"
            "\"freq\":%.0f"
            "}",
            ts.tv_sec, ts.tv_nsec / 1000000,
            offset, freq);
    }

    metrics_send(m, buffer);
}

void metrics_report_state_change(struct metrics_reporter *m, int port_number,
                                const char *from_state, const char *to_state,
                                enum fsm_event event)
{
    char buffer[512];
    struct timespec ts;
    const char *event_str = "UNKNOWN";
    double timestamp;

    if (!m || !from_state || !to_state) {
        return;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    timestamp = ts.tv_sec + ts.tv_nsec / 1000000000.0;

    /* Check filter before proceeding */
    if (!should_report_state_change(m, port_number, from_state, to_state, event, timestamp)) {
        return;
    }

    if (event < sizeof(fsm_event_str) / sizeof(fsm_event_str[0]) && fsm_event_str[event]) {
        event_str = fsm_event_str[event];
    }

    snprintf(buffer, sizeof(buffer),
        "{"
        "\"type\":\"state_change\","
        "\"timestamp\":%ld.%03ld,"
        "\"port\":%d,"
        "\"from_state\":\"%s\","
        "\"to_state\":\"%s\","
        "\"event\":\"%s\","
        "\"event_id\":%d"
        "}",
        ts.tv_sec, ts.tv_nsec / 1000000,
        port_number, from_state, to_state, event_str, (int)event);

    metrics_send(m, buffer);
}

/* Evaluate clock summary metrics filter for edge-triggered reporting */
static int should_report_clock_summary(struct metrics_reporter *m, double rms, double max_abs,
                                double freq_mean, double freq_stddev,
                                double delay_mean, double delay_stddev, double timestamp)
{
    if (!m->clock_summary_filter) {
        return 1; /* No filter, always report */
    }

    /* Update variable values */
    *(double*)m->clock_summary_vars[0].address = rms;
    *(double*)m->clock_summary_vars[1].address = max_abs;
    *(double*)m->clock_summary_vars[2].address = freq_mean;
    *(double*)m->clock_summary_vars[3].address = freq_stddev;
    *(double*)m->clock_summary_vars[4].address = delay_mean;
    *(double*)m->clock_summary_vars[5].address = delay_stddev;
    *(double*)m->clock_summary_vars[6].address = timestamp;

    /* Evaluate expression */
    double result = te_eval(m->clock_summary_filter);
    int current_state = (result != 0.0) ? 1 : 0;

    /* Check for state change (edge-triggered) */
    if (current_state != m->clock_summary_prev_state) {
        m->clock_summary_prev_state = current_state;
        pr_debug("clock summary rms:%f max:%f freq:%f freq_stddev:%f delay:%f delay_stddev:%f changed to %d", rms, max_abs, freq_mean, freq_stddev, delay_mean, delay_stddev, current_state);
        return 1; /* State changed, report it */
    }
    pr_debug("clock summary rms:%f max:%f freq:%f freq_stddev:%f delay:%f delay_stddev:%f did not change to %d", rms, max_abs, freq_mean, freq_stddev, delay_mean, delay_stddev, current_state);
    return 0; /* No state change, don't report */
}

/* Evaluate clock metrics filter for edge-triggered reporting */
static int should_report_clock(struct metrics_reporter *m,
                              double offset, double freq, double delay, double timestamp)
{
    if (!m->clock_filter) {
        return 1; /* No filter, always report */
    }

    /* Update variable values */
    *(double*)m->clock_vars[0].address = offset;
    *(double*)m->clock_vars[1].address = freq;
    *(double*)m->clock_vars[2].address = delay;
    *(double*)m->clock_vars[3].address = timestamp;

    /* Evaluate expression */
    double result = te_eval(m->clock_filter);
    int current_state = (result != 0.0) ? 1 : 0;

    /* Check for state change (edge-triggered) */
    if (current_state != m->clock_prev_state) {
        m->clock_prev_state = current_state;
	pr_debug("clock offset:%f freq:%f delay:%f changed to %d", offset, freq, delay, current_state);
        return 1; /* State changed, report it */
    }
    pr_debug("clock offset:%f freq:%f delay:%f did not change to %d", offset, freq, delay, current_state);
    return 0; /* No state change, don't report */
}

/* Evaluate state change filter for edge-triggered reporting */
static int should_report_state_change(struct metrics_reporter *m, int port_number,
                                     const char *from_state, const char *to_state,
                                     enum fsm_event event, double timestamp)
{
    if (!m->state_filter) {
        return 1; /* No filter, always report */
    }

    /* Update variable values */
    *(double*)m->state_vars[0].address = (double)port_number;
    *(double*)m->state_vars[1].address = timestamp;
    *(double*)m->state_vars[2].address = string_hash(from_state);
    *(double*)m->state_vars[3].address = string_hash(to_state);
    *(double*)m->state_vars[4].address = (double)event;

    /* Evaluate expression */
    double result = te_eval(m->state_filter);
    int current_state = (result != 0.0) ? 1 : 0;

    /* Check for state change (edge-triggered) */
    if (current_state != m->state_prev_state) {
        m->state_prev_state = current_state;
	    pr_debug("state change %s to %s did change: %d", from_state, to_state, current_state);
        return 1; /* State changed, report it */
    }
    pr_debug("state change %s to %s did not change: %d", from_state, to_state, current_state);
    return 0; /* No state change, don't report */
}
