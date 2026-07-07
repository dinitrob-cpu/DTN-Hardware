/* node/main_pi.c — DTN node application for the Raspberry Pi 4.
 *
 * Single-threaded event loop using poll() on:
 *   - stdin (CLI commands, only on the ground station)
 *   - a timerfd for the per-tick DTN engine step
 * LoRa RX is handled by polling in node_tick() for the MVP; the IRQ-based
 * path can be added later.
 *
 * Usage: sudo ./dtn-node --node EART --plan ../config/contact_plan.json [--cli]
 *   --cli enables the interactive command REPL on stdin.
 */
#include "forwarding.h"
#include "../transport/lora_transport.h"
#include "../dtn_core/contact.h"
#include "../dtn_core/trace.h"
#include "../dtn_core/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <getopt.h>

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

static dtn_time_t wall_clock_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (dtn_time_t)ts.tv_sec + (dtn_time_t)ts.tv_nsec * 1e-9f;
}

/* Trace sink: print to stdout with a timestamp + node name. */
static void stdout_trace_sink(const trace_record_t *rec, void *user)
{
    (void)user;
    char node_str[5];
    node_id_to_str(rec->node, node_str);
    printf("[%.2f] %s %s b=%llu %s\n",
           (double)rec->t, node_str, trace_event_name(rec->event),
           (unsigned long long)rec->bundle_id, rec->detail);
    fflush(stdout);
}

/* --- CLI --- */

static void cli_help(void)
{
    printf("commands:\n");
    printf("  send <dst> <class> <msg>   send a bundle (class: cmd|sci|tlm|hk)\n");
    printf("  status                     show queue, custody, stats\n");
    printf("  contacts                   list active+future contacts from this node\n");
    printf("  quit\n");
}

static bundle_class_t parse_class(const char *s)
{
    if (!s) return BUNDLE_CLASS_TELEMETRY;
    if (strcmp(s, "cmd") == 0) return BUNDLE_CLASS_COMMAND;
    if (strcmp(s, "sci") == 0) return BUNDLE_CLASS_SCIENCE;
    if (strcmp(s, "tlm") == 0) return BUNDLE_CLASS_TELEMETRY;
    return BUNDLE_CLASS_HOUSEKEEPING;
}

static void cli_status(node_state_t *ns)
{
    char self_str[5]; node_id_to_str(ns->self_id, self_str);
    printf("node %s  queue=%u/%u  custody=%u  tx=%llu rx=%llu delivered=%llu  rssi=%d dBm\n",
           self_str, (unsigned)ns->queue.count, (unsigned)NODE_QUEUE_MAX,
           (unsigned)ns->custody.count,
           (unsigned long long)ns->bundles_tx,
           (unsigned long long)ns->bundles_rx,
           (unsigned long long)ns->bundles_delivered,
           ns->last_rssi);
}

static void cli_contacts(node_state_t *ns, dtn_time_t t)
{
    char self_str[5]; node_id_to_str(ns->self_id, self_str);
    const contact_t *out[CONTACT_PLAN_MAX];
    size_t n = contact_plan_from_node(&ns->plan, ns->self_id, out, CONTACT_PLAN_MAX);
    printf("contacts from %s at t=%.1f:\n", self_str, (double)t);
    for (size_t i = 0; i < n; ++i) {
        char to_str[5]; node_id_to_str(out[i]->to, to_str);
        const char *st =
            contact_is_active(out[i], t) ? "ACTIVE" :
            contact_is_future(out[i], t) ? "future" : "closed";
        printf("  ->%s [%g..%g] rate=%g owlt=%g conf=%g ber=%g  %s\n",
               to_str, (double)out[i]->t_start, (double)out[i]->t_end,
               (double)out[i]->rate, (double)out[i]->owlt,
               (double)out[i]->conf, (double)out[i]->ber, st);
    }
}

static void cli_handle(node_state_t *ns, char *line, dtn_time_t t)
{
    char *cmd = strtok(line, " \t\r\n");
    if (!cmd) return;
    if (strcmp(cmd, "send") == 0) {
        char *dst = strtok(NULL, " \t");
        char *cls = strtok(NULL, " \t");
        char *msg = strtok(NULL, "\r\n");
        if (!dst || !cls || !msg) { printf("usage: send <dst> <class> <msg>\n"); return; }
        node_id_t did = node_id_from_str(dst);
        node_generate_bundle(ns, did, parse_class(cls), msg, t);
        printf("bundle queued for %s\n", dst);
    } else if (strcmp(cmd, "status") == 0) {
        cli_status(ns);
    } else if (strcmp(cmd, "contacts") == 0) {
        cli_contacts(ns, t);
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cli_help();
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        g_running = 0;
    } else {
        printf("unknown command: %s (try 'help')\n", cmd);
    }
}

int main(int argc, char **argv)
{
    const char *node_str = NULL;
    const char *plan_path = "../config/contact_plan.json";
    int enable_cli = 0;
    int is_ground = 0;   /* ground station: broadcast time-sync */
    float buf_cap = 4096.0f;

    static struct option long_opts[] = {
        { "node",   required_argument, 0, 'n' },
        { "plan",   required_argument, 0, 'p' },
        { "cli",    no_argument,       0, 'c' },
        { "ground", no_argument,       0, 'g' },
        { "buf",    required_argument, 0, 'b' },
        { 0, 0, 0, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "n:p:cgb:", long_opts, 0)) != -1) {
        switch (opt) {
        case 'n': node_str   = optarg; break;
        case 'p': plan_path  = optarg; break;
        case 'c': enable_cli = 1;      break;
        case 'g': is_ground  = 1;      break;
        case 'b': buf_cap    = (float)atof(optarg); break;
        default:
            fprintf(stderr, "usage: %s --node <ID> --plan <path> [--cli] [--ground] [--buf bytes]\n", argv[0]);
            return 1;
        }
    }
    if (!node_str) {
        fprintf(stderr, "error: --node is required (e.g. EART)\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Load contact plan. */
    contact_plan_t plan;
    if (contact_plan_load_json(&plan, plan_path) <= 0) {
        fprintf(stderr, "error: failed to load contact plan from %s\n", plan_path);
        return 1;
    }
    printf("loaded %u contacts from %s\n", (unsigned)plan.count, plan_path);

    /* LoRa transport. */
    lora_config_t cfg;
    lora_config_default_868(&cfg, /*cs*/ 8, /*reset*/ 22, /*dio0*/ 25, /*dio1*/ 24);
    static pi_lora_state_t lora_state;   /* static so it's not stack-huge */
    lora_transport_t radio;
    lora_transport_init_pi(&radio, &lora_state);
    if (lora_init(&radio, &cfg) != 0) {
        fprintf(stderr, "error: LoRa init failed (running without radio)\n");
        /* Continue anyway so the CLI/trace path can be exercised. */
    }

    /* Get the LoRa IRQ fd for edge-driven receive. -1 means no edge events
     * available; we'll fall back to a periodic recv poll inside the loop. */
    int lora_irq_fd = lora_get_irq_fd(&radio);

    /* Node state. */
    node_state_t ns;
    node_state_init(&ns, node_id_from_str(node_str), buf_cap, "accept", &radio,
                    stdout_trace_sink, NULL);
    ns.plan      = plan;
    ns.boot_time = wall_clock_seconds();
    if (is_ground) {
        ns.time_synced = 1;   /* ground station is the clock source */
        printf("running as ground station — will broadcast time-sync\n");
    }

    /* Timerfd: fire every 100 ms for the DTN tick. */
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = { { 0, 100*1000*1000 }, { 0, 100*1000*1000 } };
    timerfd_settime(tfd, 0, &its, NULL);

    struct pollfd pfds[3];
    nfds_t nfds = 1;   /* always have tfd */
    pfds[0].fd     = tfd;
    pfds[0].events = POLLIN;
    pfds[1].fd     = -1;
    pfds[1].events = POLLIN;
    pfds[2].fd     = -1;
    pfds[2].events = POLLIN;

    if (enable_cli) {
        int sflags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, sflags | O_NONBLOCK);
        pfds[1].fd = STDIN_FILENO;
        nfds = 2;
        cli_help();
    }
    if (lora_irq_fd >= 0) {
        pfds[2].fd     = lora_irq_fd;
        pfds[2].events = POLLIN;
        nfds = 3;
    }

    char line_buf[256];
    size_t line_len = 0;
    float next_time_sync = 0.0f;   /* next DTN time to send a time-sync broadcast */

    while (g_running) {
        int r = poll(pfds, nfds, lora_irq_fd >= 0 ? 1000 : 100);
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        dtn_time_t t = wall_clock_seconds() - ns.boot_time + ns.time_offset;

        if (pfds[0].revents & POLLIN) {
            uint64_t exp; ssize_t rd = read(tfd, &exp, sizeof(exp)); (void)rd;
            node_tick(&ns, t);
            /* Ground station: broadcast time-sync every 5 seconds. */
            if (is_ground && t >= next_time_sync) {
                node_send_time_sync(&ns, t);
                next_time_sync = t + 5.0f;
            }
            /* Fallback: if no IRQ fd, do a non-blocking recv poll here. */
            if (lora_irq_fd < 0) {
                node_try_recv(&ns, t);
            }
        }

        if (enable_cli && (pfds[1].revents & POLLIN)) {
            char buf[64];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                for (ssize_t i = 0; i < n; ++i) {
                    if (line_len < sizeof(line_buf) - 1)
                        line_buf[line_len++] = buf[i];
                    if (buf[i] == '\n') {
                        line_buf[line_len] = '\0';
                        cli_handle(&ns, line_buf, t);
                        line_len = 0;
                    }
                }
            }
        }

        if (lora_irq_fd >= 0 && (pfds[2].revents & POLLIN)) {
            /* DIO0 rising edge — a frame arrived (or TX completed). */
            node_try_recv(&ns, t);
        }
    }

    printf("shutting down\n");
    lora_close(&radio);
    close(tfd);
    return 0;
}