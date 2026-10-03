/*
 *
 * Infertop extension: llama.cpp serving metrics.
 *
 * Discovers running llama-server processes through /proc, fetches their
 * Prometheus /metrics endpoint (enabled with --metrics) over a minimal
 * blocking HTTP/1.0 GET (no libcurl dependency), and derives smoothed
 * tokens/s rates from the absolute token counters.
 *
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nvtop/llama_metrics.h"

#include "list.h"
#include "nvtop/extract_gpuinfo.h"
#include "nvtop/time.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LLAMA_POLL_MIN_INTERVAL 0.25 // Seconds between HTTP polls
#define LLAMA_DISCOVER_INTERVAL 2.0  // Seconds between /proc scans
#define LLAMA_HTTP_TIMEOUT_MS 300
#define LLAMA_IDLE_RESET 5.0      // Zero rates after this long without counter moves
#define LLAMA_LOG_IDLE 3.0        // Zero log-sourced rates after this long of silence
#define LLAMA_LOG_READ_CAP (128 * 1024)
#define LLAMA_PEAK_HALFLIFE 5.0   // Graph scale decays with this half-life
#define LLAMA_PEAK_FLOOR 10.0     // Minimum graph full-scale value

// Time-seeded EMA: first sample (or >30s gap) seeds, later samples blend.
static void ema_update(double *ema, double *last, double v, double now) {
  if (*last <= 0. || now - *last > 30.)
    *ema = v;
  else
    *ema += (v - *ema) * 0.25;
  *last = now;
}

static double monotonic_seconds(void) {
  nvtop_time t;
  nvtop_get_current_time(&t);
  return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

// ---------------------------------------------------------------------------
// Minimal HTTP/1.0 GET returning the body in a heap buffer (false on failure)
// ---------------------------------------------------------------------------

static void socket_set_timeouts(int fd) {
  struct timeval tv = {LLAMA_HTTP_TIMEOUT_MS / 1000, (LLAMA_HTTP_TIMEOUT_MS % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static bool read_all(int fd, char *buf, size_t cap, size_t *total) {
  *total = 0;
  while (*total + 1 < cap) {
    ssize_t n = read(fd, buf + *total, cap - 1 - *total);
    if (n > 0) {
      *total += (size_t)n;
    } else if (n == 0) {
      break; // EOF: HTTP/1.0 closes the connection at body end
    } else if (errno != EINTR) {
      return false;
    }
  }
  buf[*total] = '\0';
  return true;
}

static bool connect_with_poll(int fd, const struct sockaddr *addr, socklen_t addrlen) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  bool connected = false;
  if (connect(fd, addr, addrlen) == 0) {
    connected = true;
  } else if (errno == EINPROGRESS) {
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pfd, 1, LLAMA_HTTP_TIMEOUT_MS) == 1) {
      int err = 0;
      socklen_t len = sizeof(err);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
        connected = true;
    }
  }
  if (connected)
    fcntl(fd, F_SETFL, flags);
  return connected;
}

// Returns true on HTTP 200; the body is left in buf (NUL terminated)
static bool http_get_path(const char *host, unsigned port, const char *path, char *buf, size_t cap) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  // Dotted-quad only: monitoring stays local, no DNS resolution
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    return false;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  bool ok = false;
  if (connect_with_poll(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    socket_set_timeouts(fd);
    char request[256];
    int len = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: infertop\r\n"
                       "Accept: text/plain\r\nConnection: close\r\n\r\n",
                       path, host, port);
    if (len > 0 && write(fd, request, (size_t)len) == len) {
      size_t total = 0;
      if (read_all(fd, buf, cap, &total)) {
        char *body = strstr(buf, "\r\n\r\n");
        // Accept HTTP/1.0 and HTTP/1.1 (cpp-httplib answers 1.1 to 1.0 requests)
        if (body && strncmp(buf, "HTTP/1.", 7) == 0 && strncmp(buf + 9, "200", 3) == 0) {
          body += 4;
          memmove(buf, body, strlen(body) + 1);
          ok = true;
        }
      }
    }
  }
  close(fd);
  return ok;
}

static bool http_get_body(const char *host, unsigned port, char *buf, size_t cap) {
  return http_get_path(host, port, "/metrics", buf, cap);
}

static bool parse_metric_counter(const char *body, const char *name, double *value) {
  const char *p = body;
  size_t namelen = strlen(name);
  while ((p = strstr(p, name)) != NULL) {
    if ((p == body || p[-1] == '\n') && p[namelen] == ' ') {
      if (sscanf(p + namelen, "%lf", value) == 1)
        return true;
    }
    p += namelen;
  }
  return false;
}

// ---------------------------------------------------------------------------
// llama-server discovery through /proc
// ---------------------------------------------------------------------------

struct discovered_server {
  pid_t pid;
  char host[64];
  unsigned port;
  bool is_halogen;
  bool is_gufo;
};

// Extract the value of "--port 8080", "--port=8080", "--host H", "--host=H"
// from a /proc cmdline buffer (tokens separated by '\0').
static void cmdline_get_opt(const char *cmdline, size_t len, const char *opt, char *out, size_t outsz, bool numeric) {
  out[0] = '\0';
  size_t i = 0;
  size_t optlen = strlen(opt);
  while (i < len && out[0] == '\0') {
    const char *tok = cmdline + i;
    size_t toklen = strnlen(tok, len - i);
    if (toklen > 0) {
      if (strcmp(tok, opt) == 0) {
        const char *val = tok + toklen + 1;
        if (val < cmdline + len && *val)
          snprintf(out, outsz, "%s", val);
      } else if (strncmp(tok, opt, optlen) == 0 && tok[optlen] == '=') {
        snprintf(out, outsz, "%s", tok + optlen + 1);
      }
    }
    i += toklen + 1;
  }
  if (numeric && out[0] != '\0') {
    for (size_t k = 0; out[k]; ++k) {
      if (out[k] < '0' || out[k] > '9') {
        out[0] = '\0';
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Log tail: live rates from llama-server's own slot timing lines
// ---------------------------------------------------------------------------

// Resolve /proc/<pid>/fd/{2,1} to a regular file path, if any (stderr first).
static bool find_log_for_pid(pid_t pid, char *path, size_t pathsz, ino_t *ino) {
  char link[64], target[4096];
  for (int fd = 2; fd >= 1; --fd) {
    snprintf(link, sizeof(link), "/proc/%d/fd/%d", pid, fd);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0)
      continue;
    target[n] = '\0';
    if (target[0] != '/')
      continue; // pipe:[...] / socket:[...] — not a file
    struct stat st;
    if (stat(target, &st) == 0 && S_ISREG(st.st_mode)) {
      snprintf(path, pathsz, "%s", target);
      *ino = st.st_ino;
      return true;
    }
  }
  return false;
}

// Parse one print_timing line for a generation rate; returns t/s or -1.
// Prefers the recent-window tg_3s over the cumulative average tg.
static double parse_decode_line(const char *line) {
  if (strstr(line, "print_timing") == NULL)
    return -1.;
  // Only slot progress lines (n_gen / n_decoded), not end-of-task summaries
  const char *n = strstr(line, "n_gen ");
  if (!n)
    n = strstr(line, "n_decoded ");
  if (!n)
    return -1.;
  const char *t3 = strstr(line, "tg_3s =");
  const char *tg = t3 ? NULL : strstr(line, "tg =");
  const char *src = t3 ? t3 + 7 : (tg ? tg + 4 : NULL);
  if (!src)
    return -1.;
  double val = 0;
  if (sscanf(src, " %lf t/s", &val) == 1 && val >= 0.)
    return val;
  return -1.;
}

// Prompt-eval summary line: "... (X tokens per second)"
static double parse_prefill_line(const char *line) {
  if (strstr(line, "prompt eval time") == NULL)
    return -1.;
  const char *p = strstr(line, "tokens per second)");
  if (!p)
    return -1.;
  double val = 0;
  if (sscanf(p - 7, "%lf", &val) == 1 && val > 0.)
    return val;
  return -1.;
}

// ---------------------------------------------------------------------------
// CSV recorder: per-task history so rates can be correlated offline with
// context depth and memory pressure (one row per event, append only).
// Enabled by default; override path with $INFERTOP_RECORD_CSV, disable
// with INFERTOP_RECORD_CSV="".
// ---------------------------------------------------------------------------

#define LLAMA_REC_PROGRESS_INTERVAL 1.0 // Seconds between progress rows per task

static char rec_path[PATH_MAX] = "";
static bool rec_init_done = false;

static void recorder_init(void) {
  if (rec_init_done)
    return;
  rec_init_done = true;
  const char *env = getenv("INFERTOP_RECORD_CSV");
  if (env) {
    if (env[0] == '\0')
      return; // Explicitly disabled
    snprintf(rec_path, sizeof(rec_path), "%s", env);
  } else {
    const char *home = getenv("HOME");
    if (!home)
      return;
    snprintf(rec_path, sizeof(rec_path), "%s/.local/share/infertop/llama_records.csv", home);
  }
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", rec_path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    for (char *p = dir + 1; *p; ++p) {
      if (*p == '/') {
        *p = '\0';
        mkdir(dir, 0775);
        *p = '/';
      }
    }
    mkdir(dir, 0775);
  }
}

static void mem_stats(double *avail_mib, double *swap_free_mib) {
  static double cache_ts = -100.;
  static double avail = 0, swapfree = 0;
  double now = monotonic_seconds();
  if (now - cache_ts >= 1.0) {
    cache_ts = now;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
      char line[128];
      while (fgets(line, sizeof(line), fp)) {
        double kb = 0;
        if (sscanf(line, "MemAvailable: %lf kB", &kb) == 1)
          avail = kb / 1024.;
        else if (sscanf(line, "SwapFree: %lf kB", &kb) == 1)
          swapfree = kb / 1024.;
      }
      fclose(fp);
    }
  }
  *avail_mib = avail;
  *swap_free_mib = swapfree;
}

static void fmt_opt(char *out, size_t outsz, double v, const char *fmt) {
  if (isnan(v))
    out[0] = '\0';
  else
    snprintf(out, outsz, fmt, v);
}

static void recorder_write(struct llama_server_metrics *srv, long slot, long task, const char *event,
                           double prompt_tok, double gen_tok, double pp_tps, double tg_tps, double accept,
                           double mean_len, double depth_tok) {
  recorder_init();
  if (rec_path[0] == '\0')
    return;
  FILE *fp = fopen(rec_path, "a");
  if (!fp)
    return;
  if (ftell(fp) == 0)
    fputs("unix_ts,iso,host,port,pid,slot,task,event,prompt_tok,gen_tok,pp_tps,tg_tps,accept,mean_len,"
          "depth_tok,mem_avail_mib,swap_free_mib\n",
          fp);
  time_t now_s = time(NULL);
  struct tm tmv;
  localtime_r(&now_s, &tmv);
  char iso[32];
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tmv);
  char sb_prompt[24], sb_gen[24], sb_pp[24], sb_tg[24], sb_acc[24], sb_len[24], sb_depth[24];
  fmt_opt(sb_prompt, sizeof(sb_prompt), prompt_tok, "%.0f");
  fmt_opt(sb_gen, sizeof(sb_gen), gen_tok, "%.0f");
  fmt_opt(sb_pp, sizeof(sb_pp), pp_tps, "%.2f");
  fmt_opt(sb_tg, sizeof(sb_tg), tg_tps, "%.2f");
  fmt_opt(sb_acc, sizeof(sb_acc), accept, "%.4f");
  fmt_opt(sb_len, sizeof(sb_len), mean_len, "%.2f");
  fmt_opt(sb_depth, sizeof(sb_depth), depth_tok, "%.0f");
  double avail = NAN, swapfree = NAN;
  mem_stats(&avail, &swapfree);
  char sb_avail[24], sb_swap[24];
  fmt_opt(sb_avail, sizeof(sb_avail), avail, "%.0f");
  fmt_opt(sb_swap, sizeof(sb_swap), swapfree, "%.0f");
  char taskstr[16];
  if (task < 0)
    snprintf(taskstr, sizeof(taskstr), "-");
  else
    snprintf(taskstr, sizeof(taskstr), "%ld", task);
  fprintf(fp, "%ld,%s,%s,%u,%d,%ld,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", (long)now_s, iso, srv->host, srv->port,
          srv->pid, slot, taskstr, event, sb_prompt, sb_gen, sb_pp, sb_tg, sb_acc, sb_len, sb_depth, sb_avail,
          sb_swap);
  fclose(fp);
}

static struct llama_task_record *task_rec(struct llama_server_metrics *srv, unsigned task) {
  struct llama_task_record *free_slot = NULL, *oldest = NULL;
  for (unsigned i = 0; i < LLAMA_MAX_TRACKED_TASKS; ++i) {
    struct llama_task_record *t = &srv->tasks[i];
    if (t->used && t->task == task)
      return t;
    if (!t->used && !free_slot)
      free_slot = t;
    if (!oldest || t->prog_last < oldest->prog_last)
      oldest = t;
  }
  struct llama_task_record *t = free_slot ? free_slot : oldest;
  memset(t, 0, sizeof(*t));
  t->used = true;
  t->task = task;
  t->prompt_tok = 0;
  return t;
}

// Extract "id N" and "task N" from a slot log line (token starts at a space)
static bool line_ids(const char *line, long *slot, long *task) {
  *slot = -1;
  *task = -1;
  for (const char *p = line; *p; ++p) {
    if (*p != ' ' && !((*p == '|' || p == line)))
      continue;
    if (p[1] == 'i' && p[2] == 'd' && p[3] == ' ') {
      char *e;
      *slot = strtol(p + 4, &e, 10);
      if (e == p + 4)
        return false;
      p = e - 1;
    } else if (p[1] == 't' && p[2] == 'a' && p[3] == 's' && p[4] == 'k' && p[5] == ' ') {
      char *e;
      *task = strtol(p + 6, &e, 10);
      if (e == p + 6)
        return false;
      p = e - 1;
    }
  }
  return *slot >= 0;
}

// "... = X ms / N tokens ..." -> N (prompt-eval and eval-time summaries)
static bool parse_ms_tokens(const char *line, double *tokens) {
  const char *p = strstr(line, "ms /");
  if (!p)
    return false;
  double ms = 0;
  if (sscanf(p, "ms / %lf tokens", &ms) == 1) {
    *tokens = ms;
    return true;
  }
  return false;
}

// Parse one raw log line for the recorder. Independent of the live-rate
// parsers so a format miss degrades to "no row", never to wrong numbers.
static void record_line(struct llama_server_metrics *srv, const char *line, double now) {
  long slot, task;
  if (strstr(line, "print_timing") == NULL)
    return;

  if (strstr(line, "prompt eval time") != NULL && strstr(line, "tokens per second") != NULL) {
    if (!line_ids(line, &slot, &task))
      return;
    double pp = parse_prefill_line(line);
    double ptok = NAN;
    parse_ms_tokens(line, &ptok);
    struct llama_task_record *t = (task >= 0) ? task_rec(srv, (unsigned)task) : NULL;
    double gtok = NAN;
    if (t) {
      t->prompt_tok = isnan(ptok) ? 0 : ptok;
      gtok = t->gen_tok > 0 ? t->gen_tok : NAN;
    }
    double depth = isnan(ptok) ? NAN : ptok + (gtok > 0 ? gtok : 0);
    recorder_write(srv, slot, task, "prefill", ptok, gtok, pp < 0 ? NAN : pp, NAN, NAN, NAN, depth);
    return;
  }

  if (strstr(line, "draft acceptance") != NULL) {
    if (!line_ids(line, &slot, &task))
      return;
    const char *a = strstr(line, "draft acceptance =");
    double accept = NAN, mean_len = NAN, gen = NAN;
    if (a)
      sscanf(a + 18, " %lf", &accept);
    const char *m = strstr(line, "mean len =");
    if (m)
      sscanf(m + 10, " %lf", &mean_len);
    if (a) {
      double acc2 = 0, gen2 = 0;
      if (sscanf(a, "draft acceptance = %*lf ( %lf accepted / %lf generated)", &acc2, &gen2) == 2)
        gen = gen2;
    }
    struct llama_task_record *t = (task >= 0) ? task_rec(srv, (unsigned)task) : NULL;
    double ptok = (t && t->prompt_tok > 0) ? t->prompt_tok : NAN;
    double depth = (isnan(ptok) || isnan(gen)) ? NAN : ptok + gen;
    if (!isnan(accept)) {
      srv->accept_is_commit = false;
      ema_update(&srv->accept_val, &srv->accept_last, 100. * accept, now);
    }
    recorder_write(srv, slot, task, "accept", NAN, gen, NAN, NAN, accept, mean_len, depth);
    return;
  }

  // End-of-task decode summary (not the n_gen progress lines)
  if (strstr(line, "eval time") != NULL && strstr(line, "prompt eval time") == NULL &&
      strstr(line, "n_gen") == NULL && strstr(line, "n_decoded") == NULL) {
    if (!line_ids(line, &slot, &task))
      return;
    double tok = NAN;
    parse_ms_tokens(line, &tok);
    const char *pps = strstr(line, "tokens per second)");
    double tg = NAN;
    if (pps)
      sscanf(pps - 7, "%lf", &tg);
    struct llama_task_record *t = (task >= 0) ? task_rec(srv, (unsigned)task) : NULL;
    double ptok = (t && t->prompt_tok > 0) ? t->prompt_tok : NAN;
    double depth = (isnan(ptok) || isnan(tok)) ? NAN : ptok + tok;
    recorder_write(srv, slot, task, "taskdone", NAN, tok, NAN, tg, NAN, NAN, depth);
    return;
  }

  // Live progress: reuse the rate parser, plus the token counter
  double tg = parse_decode_line(line);
  if (tg < 0)
    return;
  if (!line_ids(line, &slot, &task))
    return;
  const char *n = strstr(line, "n_gen ");
  if (!n)
    n = strstr(line, "n_decoded ");
  double gen = NAN;
  if (n)
    sscanf(n + (n == strstr(line, "n_gen ") ? 6 : 10), " = %lf", &gen);
  struct llama_task_record *t = (task >= 0) ? task_rec(srv, (unsigned)task) : NULL;
  double ptok = NAN;
  if (t) {
    t->gen_tok = gen;
    ptok = t->prompt_tok > 0 ? t->prompt_tok : NAN;
    if (now - t->prog_last < LLAMA_REC_PROGRESS_INTERVAL)
      return; // Throttled
    t->prog_last = now;
  }
  double depth = (isnan(ptok) || isnan(gen)) ? NAN : ptok + gen;
  recorder_write(srv, slot, task, "progress", NAN, gen, NAN, tg, NAN, NAN, depth);
}

// ---------------------------------------------------------------------------
// halogen container backend
//
// halogen-flash-server (Qwen3.8-Flash-Next on gfx1151) runs in a container:
// no /metrics endpoint and its log goes to the podman log driver. Two data
// sources replace them:
//   * "podman logs -f --tail 0 <name>" streamed non-blocking: its serve_api
//     request-completion lines carry gen tokens, decode t/s, prompt tokens
//     (with cached prefix) and prefill seconds, i.e. everything the recorder
//     needs; commit/round maps onto the spec-decode mean_len column.
//   * GET /health (1 s) for busy / in_flight / queued liveness.
// The podman child is spawned with a non-blocking pipe; it follows until the
// container exits, so teardown must SIGTERM it before reaping.
// ---------------------------------------------------------------------------

#define HALOGEN_DISCOVER_INTERVAL 10.0 // podman is too slow for the 2 s scan
#define HALOGEN_HEALTH_INTERVAL 1.0
#define HALOGEN_LOG_READ_CAP 256

extern char **environ;

struct halogen_backend {
  bool active;
  bool gone;        // follow stream ended: retry discovery from scratch
  char name[64];
  pid_t child;
  FILE *pipe;
  pid_t init_pid;
  unsigned next_task;
  double last_health;
  bool health_busy;
  int health_in_flight;
  unsigned host_port; // mapped host port of engine's 8731 (from podman port)
};

enum { CB_HALOGEN = 0, CB_GUFO, CB_COUNT };

static struct halogen_backend hlg[CB_COUNT] = {{.child = -1}, {.child = -1}};

static void halogen_teardown(unsigned idx) {
  if (hlg[idx].pipe) {
    kill(hlg[idx].child, SIGTERM);
    pclose(hlg[idx].pipe); // child has been signaled, cannot hang
    hlg[idx].pipe = NULL;
  }
  hlg[idx].active = false;
  hlg[idx].gone = false;
  hlg[idx].child = -1;
  hlg[idx].name[0] = '\0';
}

static bool halogen_start(unsigned idx, const char *name) {
  // Redirect the child's stdout+stderr into a pipe, non-blocking on our side
  int fds[2];
  if (pipe(fds) != 0)
    return false;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);
  char cmd_name[64];
  snprintf(cmd_name, sizeof(cmd_name), "%s", name);
  char *argv[] = {(char *)"podman", (char *)"logs", (char *)"-f", (char *)"--tail", (char *)"0", cmd_name, NULL};
  pid_t child;
  int rc = posix_spawnp(&child, "podman", &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return false;
  }
  int flags = fcntl(fds[0], F_GETFL, 0);
  if (flags >= 0)
    fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
  hlg[idx].pipe = fdopen(fds[0], "r");
  if (!hlg[idx].pipe) {
    close(fds[0]);
    kill(child, SIGTERM);
    return false;
  }
  hlg[idx].child = child;
  snprintf(hlg[idx].name, sizeof(hlg[idx].name), "%s", name);
  hlg[idx].active = true;
  hlg[idx].gone = false;
  hlg[idx].next_task = 0;
  // Container init pid on the host, for the CSV pid column (best effort)
  hlg[idx].init_pid = -1;
  char inspect_cmd[160];
  snprintf(inspect_cmd, sizeof(inspect_cmd), "podman inspect -f {{.State.Pid}} %.60s 2>/dev/null", name);
  FILE *ip = popen(inspect_cmd, "r");
  if (ip) {
    long v = -1;
    if (fscanf(ip, "%ld", &v) == 1 && v > 0)
      hlg[idx].init_pid = (pid_t)v;
    pclose(ip);
  }
  return true;
}

static void halogen_handle_line(struct llama_server_metrics *srv, const char *line, double now) {
  // serve_api: mtp 128 tok in 3.78s = 33.83 t/s | 82 rounds, commit 1.55/round | prompt 95998, prefill 111.14s | detok 125us/tok
  // serve_api: mtp 1 tok in 0.00s = n/a | prompt 75298, prefill 85.39s | detok 79us/tok
  // A cached follow-up adds "(N cached)" after the prompt token count.
  const char *api = strstr(line, "serve_api:");
  if (!api)
    return;
  const char *tok = strstr(api, " tok in ");
  const char *pr = strstr(api, "| prompt ");
  if (!tok || !pr)
    return;
  double gen = 0;
  if (sscanf(api, "serve_api: %*s %lf tok in", &gen) != 1)
    return;
  double tg = NAN;
  const char *eq = strstr(tok, "= ");
  if (eq && strncmp(eq + 2, "n/a", 3) != 0)
    if (sscanf(eq + 2, "%lf t/s", &tg) != 1)
      tg = NAN;
  if (tg < 0)
    tg = NAN;
  double prompt = 0, cached = 0, prefill_s = NAN;
  if (sscanf(pr, "| prompt %lf", &prompt) != 1)
    return;
  const char *par = strstr(pr, " (");
  if (par)
    sscanf(par, " (%lf cached)", &cached);
  const char *pf = strstr(pr, "prefill ");
  if (pf)
    sscanf(pf, "prefill %lfs", &prefill_s);
  double pp = NAN;
  if (prefill_s > 0.001)
    pp = (prompt - cached) / prefill_s;
  double commit = NAN;
  const char *cm = strstr(api, "commit ");
  if (cm)
    sscanf(cm, "commit %lf/round", &commit);
  if (prompt >= 256.)
    ema_update(&srv->cache_hit_pct, &srv->cache_hit_last, 100. * cached / prompt, now);
  if (!isnan(commit)) {
    srv->accept_is_commit = true;
    ema_update(&srv->accept_val, &srv->accept_last, commit, now);
  }
  unsigned task = ++hlg[CB_HALOGEN].next_task;
  double depth = prompt + gen;
  // Two rows per request, matching the llama event schema offline
  recorder_write(srv, -1, (long)task, "prefill", prompt, NAN, pp, NAN, NAN, NAN, prompt);
  recorder_write(srv, -1, (long)task, "taskdone", prompt, gen, pp, tg, NAN, commit, depth);
  if (!isnan(tg)) {
    srv->log_decode_tps = tg;
    srv->log_decode_last = now;
  }
  if (!isnan(pp)) {
    srv->log_prefill_tps = pp;
    srv->log_prefill_last = now;
  }
  srv->log_prefilling = false;
}

static void gufo_handle_line(struct llama_server_metrics *srv, const char *line, double now) {
  // [http] request=r98 event=completed ... prompt_tokens=99421 cached_tokens=99383
  //        prefill_tps=130.6 decode_tps=32.5 acceptance_pct=65.0 ...
  if (strstr(line, "event=completed") == NULL)
    return;
  double prompt = 0, cached = 0, pp = 0, tg = 0, acc = 0;
  bool have_acc = false;
  const char *p;
  if ((p = strstr(line, "prompt_tokens=")))
    sscanf(p + 14, "%lf", &prompt);
  if ((p = strstr(line, "cached_tokens=")))
    sscanf(p + 14, "%lf", &cached);
  if ((p = strstr(line, " prefill_tps=")))
    sscanf(p + 13, "%lf", &pp);
  if ((p = strstr(line, " decode_tps=")))
    sscanf(p + 12, "%lf", &tg);
  double gen = 0;
  if ((p = strstr(line, "generated_tokens=")))
    sscanf(p + 17, "%lf", &gen);
  if ((p = strstr(line, "acceptance_pct="))) {
    sscanf(p + 15, "%lf", &acc);
    have_acc = true;
  }
  if (prompt >= 256.)
    ema_update(&srv->cache_hit_pct, &srv->cache_hit_last, 100. * cached / prompt, now);
  if (have_acc) {
    srv->accept_is_commit = false;
    ema_update(&srv->accept_val, &srv->accept_last, acc, now);
  }
  unsigned task = ++hlg[CB_GUFO].next_task;
  double depth = prompt + gen;
  // Two rows per request, matching the halogen/llama event schema.
  // gufo reports acceptance as a percentage; the CSV accept column is a
  // fraction, like llama.cpp's.
  recorder_write(srv, -1, (long)task, "prefill", prompt, NAN, pp, NAN, NAN, NAN, prompt);
  recorder_write(srv, -1, (long)task, "taskdone", prompt, gen, pp, tg, have_acc ? acc / 100. : NAN, NAN, depth);
  if (tg > 0) {
    srv->log_decode_tps = tg;
    srv->log_decode_last = now;
  }
  if (pp > 0) {
    srv->log_prefill_tps = pp;
    srv->log_prefill_last = now;
  }
  srv->log_prefilling = false;
}

static void halogen_drain(struct llama_server_metrics *srv, double now, unsigned idx) {
  if (!hlg[idx].pipe)
    return;
  char buf[1024];
  unsigned reads = 0;
  while (reads++ < HALOGEN_LOG_READ_CAP) {
    errno = 0;
    if (fgets(buf, sizeof(buf), hlg[idx].pipe) == NULL) {
      if (feof(hlg[idx].pipe)) { // follow stream ended: container gone
        hlg[idx].gone = true;
        return;
      }
      clearerr(hlg[idx].pipe); // EAGAIN (non-blocking) or real error: retry next tick
      return;
    }
    size_t l = strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
      buf[--l] = '\0';
    if (idx == CB_GUFO)
      gufo_handle_line(srv, buf, now);
    else
      halogen_handle_line(srv, buf, now);
  }
}

static void halogen_health(struct llama_server_metrics *srv, double now, unsigned idx) {
  char body[8192];
  if (!http_get_path(srv->host, srv->port, "/health", body, sizeof(body))) {
    hlg[idx].health_busy = false;
    hlg[idx].health_in_flight = -1;
    return;
  }
  hlg[idx].health_busy = strstr(body, "\"busy\":true") != NULL;
  int n = -1;
  const char *p = strstr(body, "\"in_flight\":");
  if (p && sscanf(p + 12, "%d", &n) == 1)
    hlg[idx].health_in_flight = n;
  srv->engine_busy = hlg[idx].health_busy || hlg[idx].health_in_flight > 0;
  p = strstr(body, "\"queued\":");
  if (p)
    sscanf(p + 9, "%d", &n);
  // Liveness: /health proves the engine exists, but serves no rates. Use
  // the busy flag to mark prompt-processing / generation in flight so the
  // TUI shows activity between the request-completion serve_api lines.
  if (hlg[idx].health_busy) {
    srv->metrics_checked = true;
    if (srv->log_decode_tps < 0.01 && srv->log_prefill_tps < 0.01) {
      if (!srv->log_prefilling) {
        srv->log_prefilling = true;
        srv->log_prefill_start = now;
      }
    }
  } else {
    srv->log_prefilling = false;
  }
}

static void halogen_poll(struct llama_server_metrics *srv, double now, unsigned idx) {
  srv->metrics_checked = true;
  srv->metrics_available = false;
  if (hlg[idx].active && hlg[idx].name[0] && srv->log_path[0] == '\0')
    snprintf(srv->log_path, sizeof(srv->log_path), "podman:%s", hlg[idx].name);
  halogen_drain(srv, now, idx);
  if (now - hlg[idx].last_health >= HALOGEN_HEALTH_INTERVAL) {
    hlg[idx].last_health = now;
    halogen_health(srv, now, idx);
  }
  if (!hlg[idx].health_busy) {
    // serve_api rates are request averages: keep them visible until the
    // engine has been quiet well past one typical request, then decay.
    if (srv->log_decode_tps > 0 && now - srv->log_decode_last > 300.)
      srv->log_decode_tps = 0;
    if (srv->log_prefill_tps > 0 && now - srv->log_prefill_last > 300.)
      srv->log_prefill_tps = 0;
  }
}

// Discover the halogen container via podman (slow: call rarely).
static unsigned halogen_port(unsigned idx) {
  if (hlg[idx].host_port)
    return hlg[idx].host_port;
  const char *env = getenv(idx == CB_GUFO ? "INFERTOP_GUFO_PORT" : "INFERTOP_HALOGEN_PORT");
  unsigned p = env ? (unsigned)strtoul(env, NULL, 10) : 0;
  return p ? p : (idx == CB_GUFO ? 8081u : 8731u);
}

static bool halogen_discover(unsigned idx, char *name, size_t namesz) {
  const char *env = getenv(idx == CB_GUFO ? "INFERTOP_GUFO_CONTAINER" : "INFERTOP_HALOGEN_CONTAINER");
  if (env && env[0] == '\0')
    return false; // explicitly disabled
  if (access("/usr/bin/podman", X_OK) != 0 && access("/usr/local/bin/podman", X_OK) != 0)
    return false;
  FILE *fp = popen("podman ps --format {{.Names}} 2>/dev/null", "r");
  if (!fp)
    return false;
  char line[256];
  bool found = false;
  const char *pattern = env ? env : (idx == CB_GUFO ? "gufo" : "halogen");
  while (fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (strstr(line, pattern) != NULL) {
      snprintf(name, namesz, "%s", line);
      found = true;
      break;
    }
  }
  pclose(fp);
  if (found) { // resolve the host port mapped to the engine's 8731
    char cmd[300], pl[96];
    unsigned hp = 0;
    snprintf(cmd, sizeof(cmd), "podman port %.60s 8731/tcp 2>/dev/null", name);
    FILE *pp = popen(cmd, "r");
    if (pp) {
      while (fgets(pl, sizeof(pl), pp)) {
        char *c = strrchr(pl, ':');
        if (c)
          hp = (unsigned)strtoul(c + 1, NULL, 10);
      }
      pclose(pp);
    }
    hlg[idx].host_port = hp; // 0 → halogen_port() falls back to env/default
  }
  return found;
}

static void tail_log_update(struct llama_server_metrics *srv, double now) {
  if (!srv->log_active) {
    ino_t ino;
    if (!find_log_for_pid(srv->pid, srv->log_path, sizeof(srv->log_path), &ino))
      return;
    srv->log_ino = ino;
    struct stat st;
    srv->log_offset = (stat(srv->log_path, &st) == 0) ? (long long)st.st_size : 0;
    srv->log_active = true;
    return; // Attach silently: skip backlog, wait for fresh lines
  }
  struct stat st;
  if (stat(srv->log_path, &st) != 0) {
    srv->log_active = false;
    return;
  }
  if (st.st_ino != srv->log_ino) { // rotated
    srv->log_ino = st.st_ino;
    srv->log_offset = 0;
  }
  if ((long long)st.st_size < srv->log_offset) // truncated
    srv->log_offset = 0;
  if ((long long)st.st_size == srv->log_offset) {
    // Silence: decay the log-sourced rate once generation clearly stopped
    if (srv->log_decode_tps > 0 && now - srv->log_decode_last > LLAMA_LOG_IDLE)
      srv->log_decode_tps = 0;
    if (srv->log_prefill_tps > 0 && now - srv->log_prefill_last > 8.0)
      srv->log_prefill_tps = 0;
    return;
  }
  FILE *fp = fopen(srv->log_path, "r");
  if (!fp) {
    srv->log_active = false;
    return;
  }
  fseeko(fp, (off_t)srv->log_offset, SEEK_SET);
  long long consumed = 0;
  double max_decode = -1, max_prefill = -1;
  bool got_decode = false, got_prefill = false;
  char linebuf[1024];
  size_t carry = 0;
  // llama-server logs often separate records with '\r' (progress lines),
  // so split on both '\n' and '\r'.
  while (fgets(linebuf + carry, (int)sizeof(linebuf) - carry, fp)) {
    size_t linelen = strlen(linebuf);
    // A "line" without trailing separator may be a partial record, but when
    // the separator is '\r' fgets still stops there. Handle both:
    bool terminated = linelen > 0 && (linebuf[linelen - 1] == '\n' || linebuf[linelen - 1] == '\r');
    if (!terminated) { // partial trailing line: rewind, keep for next poll
      consumed -= 0; // nothing consumed beyond what's kept
      carry = 0;
      break;
    }
    consumed += (long long)linelen;
    carry = 0;
    linebuf[linelen - 1] = '\0';
    if (linelen == 1)
      continue; // lone separator
    record_line(srv, linebuf, now);
    double d = parse_decode_line(linebuf);
    if (d >= 0) {
      got_decode = true;
      if (d > max_decode)
        max_decode = d;
      srv->log_prefilling = false; // generation resumed: prefill done
    } else if (strstr(linebuf, "launch_slot_") && strstr(linebuf, "processing task")) {
      // Request started: llama.cpp is prompt-processing now, but logs
      // nothing until the prompt-eval summary at decode start.
      if (!srv->log_prefilling) {
        srv->log_prefilling = true;
        srv->log_prefill_start = now;
      }
    } else if (strstr(linebuf, "release:") || strstr(linebuf, "abort")) {
      srv->log_prefilling = false;
    } else {
      double p = parse_prefill_line(linebuf);
      if (p >= 0) {
        got_prefill = true;
        if (p > max_prefill)
          max_prefill = p;
        srv->log_prefilling = false; // prompt-eval summary arrived
      }
    }
  }
  fclose(fp);
  srv->log_offset += consumed;
  if (got_decode) {
    srv->log_decode_tps = max_decode;
    srv->log_decode_last = now;
  } else if (srv->log_decode_tps > 0 && now - srv->log_decode_last > LLAMA_LOG_IDLE) {
    srv->log_decode_tps = 0;
  }
  // Guard: if a launched request never produced any timing line (client
  // disconnect, error), clear the prefilling flag after 5min of silence.
  // Normal endings are caught by release:/abort lines; long-context
  // prefill legitimately takes minutes, so keep this generous.
  if (srv->log_prefilling && now - srv->log_prefill_start > 300.)
    srv->log_prefilling = false;
  if (got_prefill) {
    srv->log_prefill_tps = max_prefill;
    srv->log_prefill_last = now;
  } else if (srv->log_prefill_tps > 0 && now - srv->log_prefill_last > 8.0) {
    srv->log_prefill_tps = 0;
  }
}

static unsigned discover_servers(struct discovered_server *found, size_t found_cap) {
  DIR *proc = opendir("/proc");
  if (!proc)
    return 0;
  unsigned count = 0;
  struct dirent *entry;
  while ((entry = readdir(proc)) != NULL && count < found_cap) {
    if (entry->d_type != DT_DIR)
      continue;
    bool digits = entry->d_name[0] != '\0';
    for (const char *c = entry->d_name; *c && digits; ++c)
      digits = (*c >= '0' && *c <= '9');
    if (!digits)
      continue;
    pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
    if (pid <= 0)
      continue;
    char path[96];
    snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
    FILE *comm = fopen(path, "r");
    if (!comm)
      continue;
    char comm_name[64];
    bool match = fgets(comm_name, sizeof(comm_name), comm) && strstr(comm_name, "llama-server") != NULL;
    fclose(comm);
    if (!match)
      continue;
    snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
    FILE *cmdline_f = fopen(path, "rb");
    if (!cmdline_f)
      continue;
    char cmdline[4096];
    size_t len = fread(cmdline, 1, sizeof(cmdline) - 1, cmdline_f);
    fclose(cmdline_f);
    cmdline[len] = '\0';

    char port_str[16] = "", host_str[64] = "";
    cmdline_get_opt(cmdline, len, "--port", port_str, sizeof(port_str), true);
    cmdline_get_opt(cmdline, len, "--host", host_str, sizeof(host_str), false);
    if (port_str[0] == '\0')
      snprintf(port_str, sizeof(port_str), "%s", "8080"); // llama-server default
    if (host_str[0] == '\0' || strcmp(host_str, "0.0.0.0") == 0 || strcmp(host_str, "::") == 0)
      snprintf(host_str, sizeof(host_str), "%s", "127.0.0.1");

    found[count].pid = pid;
    found[count].port = (unsigned)strtoul(port_str, NULL, 10);
    snprintf(found[count].host, sizeof(found[count].host), "%s", host_str);
    count++;
  }
  closedir(proc);
  return count;
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

struct llama_metrics_state {
  struct llama_server_metrics servers[LLAMA_MAX_SERVERS];
  struct llama_device_metrics per_dev_copy[64]; // Aggregated result per device slot
  double *peaks;                                // Persistent graph full-scale per device slot
  unsigned num_dev_with_peaks;
  unsigned num_dev;
  double last_discover;
  double last_hlg_discover; // podman is on its own slower timer
  double last_guf_discover;
  double last_poll;
  bool initialized;
};

static struct llama_metrics_state state = {0};

static void poll_server(struct llama_server_metrics *srv, double now) {
  if (srv->is_halogen) {
    halogen_poll(srv, now, CB_HALOGEN);
    return;
  }
  if (srv->is_gufo) {
    halogen_poll(srv, now, CB_GUFO);
    return;
  }
  srv->metrics_checked = true;
  tail_log_update(srv, now);
  char body[65536];
  if (!http_get_body(srv->host, srv->port, body, sizeof(body))) {
    srv->metrics_available = false;
    return;
  }
  double predicted = 0, prompt = 0;
  bool have_predicted = parse_metric_counter(body, "llamacpp:tokens_predicted_total", &predicted);
  bool have_prompt = parse_metric_counter(body, "llamacpp:prompt_tokens_total", &prompt);
  if (!have_predicted && !have_prompt) {
    srv->metrics_available = false; // Answered, but not a llama.cpp metrics page
    return;
  }
  srv->metrics_available = true;

  if (srv->had_last_sample) {
    double dt = now - srv->monotonic_last;
    if (dt > 0.01) {
      if (predicted < srv->predicted_total || prompt < srv->prompt_total) {
        // Server restarted: counters reset
        srv->decode_tps = 0;
        srv->prefill_tps = 0;
      } else {
        double inst_decode = (predicted - srv->predicted_total) / dt;
        double inst_prefill = (prompt - srv->prompt_total) / dt;
        srv->decode_tps = 0.7 * srv->decode_tps + 0.3 * inst_decode;
        srv->prefill_tps = 0.7 * srv->prefill_tps + 0.3 * inst_prefill;
        if (inst_decode > 0.05 || inst_prefill > 0.05)
          srv->idle_ticks = 0;
        else
          srv->idle_ticks++;
      }
    }
  }
  srv->predicted_total = predicted;
  srv->prompt_total = prompt;
  srv->monotonic_last = now;
  srv->had_last_sample = true;

  // A finished generation would otherwise leave the last rate frozen forever
  if (srv->idle_ticks * LLAMA_POLL_MIN_INTERVAL > LLAMA_IDLE_RESET) {
    srv->decode_tps = 0;
    srv->prefill_tps = 0;
  }
}

// ---------------------------------------------------------------------------
// Image generation job (Qwen-Image-2.1 via qi-gen.py)
//
// The generator publishes an atomic key=value status file
// (~/.local/share/infertop/img_status, override with
// $INFERTOP_IMG_STATUS; empty string disables). GUI-only: nothing
// here ever touches the CSV recorder.
// ---------------------------------------------------------------------------

#define IMG_READ_INTERVAL 1.0
#define IMG_STALE_SECONDS 7200 // Leftover file from a dead run

static struct {
  pid_t pid;
  bool active;
  char state[24];
  double last_read;
} img = {.pid = -1};

static void img_reset(void) {
  img.active = false;
  img.pid = -1;
  img.state[0] = '\0';
}

static void img_update(double now) {
  if (now - img.last_read < IMG_READ_INTERVAL)
    return;
  img.last_read = now;

  const char *env = getenv("INFERTOP_IMG_STATUS");
  char path[PATH_MAX];
  if (env) {
    if (env[0] == '\0') {
      img_reset();
      return;
    }
    snprintf(path, sizeof(path), "%s", env);
  } else {
    const char *home = getenv("HOME");
    if (!home) {
      img_reset();
      return;
    }
    snprintf(path, sizeof(path), "%s/.local/share/infertop/img_status", home);
  }

  struct stat st;
  if (stat(path, &st) != 0 || time(NULL) - st.st_mtime > IMG_STALE_SECONDS) {
    img_reset();
    return;
  }
  FILE *fp = fopen(path, "r");
  if (!fp) {
    img_reset();
    return;
  }
  char buf[512];
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  if (n == 0) {
    img_reset();
    return;
  }
  buf[n] = '\0';

  long pid = -1, step = 0, steps = 0;
  char phase[16] = "";
  const char *p = strstr(buf, "pid=");
  if (p)
    sscanf(p + 4, "%ld", &pid);
  p = strstr(buf, "phase=");
  if (p)
    sscanf(p + 6, "%15s", phase);
  p = strstr(buf, "step=");
  if (p)
    sscanf(p + 5, "%ld", &step);
  p = strstr(buf, "steps=");
  if (p)
    sscanf(p + 6, "%ld", &steps);

  if (pid <= 0 || strcmp(phase, "done") == 0) {
    img_reset();
    return;
  }
  char pidpath[64];
  snprintf(pidpath, sizeof(pidpath), "/proc/%ld", pid);
  DIR *check = opendir(pidpath);
  if (!check) {
    img_reset(); // Generator died without clearing its status
    return;
  }
  closedir(check);

  img.pid = (pid_t)pid;
  img.active = true;
  if (strcmp(phase, "load") == 0)
    snprintf(img.state, sizeof(img.state), "load\xe2\x80\xa6");
  else if (strcmp(phase, "vae") == 0)
    snprintf(img.state, sizeof(img.state), "vae\xe2\x80\xa6");
  else if (strcmp(phase, "save") == 0)
    snprintf(img.state, sizeof(img.state), "save");
  else
    snprintf(img.state, sizeof(img.state), "%ld/%ld", step, steps);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void llama_metrics_refresh(struct list_head *devices) {
  double now = monotonic_seconds();
  if (!state.initialized) {
    // Backdate so the very first refresh already runs discovery + poll
    state.last_discover = now - LLAMA_DISCOVER_INTERVAL;
    state.last_poll = now - LLAMA_POLL_MIN_INTERVAL;
    state.initialized = true;
  }

  // Device count tracking (the monitored GPU set can change at runtime)
  unsigned dev_count = 0;
  struct gpu_info *device;
  list_for_each_entry(device, devices, list) { dev_count++; }
  if (dev_count != state.num_dev_with_peaks) {
    free(state.peaks);
    state.peaks = calloc(dev_count ? dev_count : 1, sizeof(*state.peaks));
    state.num_dev_with_peaks = dev_count;
  }

  // Discovery: rebuild the server list, preserving rate state for pids that
  // survive with the same host:port.
  if (now - state.last_discover >= LLAMA_DISCOVER_INTERVAL) {
    struct discovered_server found[LLAMA_MAX_SERVERS];
    memset(found, 0, sizeof(found));
    unsigned found_count = discover_servers(found, LLAMA_MAX_SERVERS);
    static double last_stale;
    if (now - last_stale >= HALOGEN_DISCOVER_INTERVAL) {
      last_stale = now;
      for (unsigned b = 0; b < CB_COUNT; ++b) {
        if (!hlg[b].active || hlg[b].gone)
          continue;
        // `podman logs -f` can keep tailing a dead container's log file after
        // the container is recreated under the same name (no EOF). Compare the
        // live init pid; a mismatch means we are following a corpse.
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "podman inspect -f {{.State.Pid}} %.60s 2>/dev/null", hlg[b].name);
        FILE *ip = popen(cmd, "r");
        long v = -1;
        if (ip) {
          if (fscanf(ip, "%ld", &v) != 1)
            v = -1;
          pclose(ip);
        }
        if (v < 0 || (pid_t)v != hlg[b].init_pid)
          hlg[b].gone = true; // recreated or removed: teardown, then re-discover
      }
    }
    if (hlg[CB_HALOGEN].gone)
      halogen_teardown(CB_HALOGEN); // follow stream hit EOF: container stopped
    if (hlg[CB_GUFO].gone)
      halogen_teardown(CB_GUFO);
    if (!hlg[CB_HALOGEN].active && now - state.last_hlg_discover >= HALOGEN_DISCOVER_INTERVAL) {
      char hname[64];
      state.last_hlg_discover = now;
      if (halogen_discover(CB_HALOGEN, hname, sizeof(hname)) && halogen_start(CB_HALOGEN, hname))
        state.last_hlg_discover = now - HALOGEN_DISCOVER_INTERVAL; // attached: keep tracking
    }
    if (!hlg[CB_GUFO].active && now - state.last_guf_discover >= HALOGEN_DISCOVER_INTERVAL) {
      char gname[64];
      state.last_guf_discover = now;
      if (halogen_discover(CB_GUFO, gname, sizeof(gname)) && halogen_start(CB_GUFO, gname))
        state.last_guf_discover = now - HALOGEN_DISCOVER_INTERVAL;
    }
    bool tracked[CB_COUNT] = {false, false};
    if (hlg[CB_HALOGEN].active && found_count < LLAMA_MAX_SERVERS) {
      found[found_count].pid = hlg[CB_HALOGEN].init_pid > 0 ? hlg[CB_HALOGEN].init_pid : hlg[CB_HALOGEN].child;
      found[found_count].port = halogen_port(CB_HALOGEN);
      snprintf(found[found_count].host, sizeof(found[found_count].host), "%s", "127.0.0.1");
      found[found_count].is_halogen = true;
      found_count++;
      tracked[CB_HALOGEN] = true;
    }
    if (hlg[CB_GUFO].active && found_count < LLAMA_MAX_SERVERS) {
      found[found_count].pid = hlg[CB_GUFO].init_pid > 0 ? hlg[CB_GUFO].init_pid : hlg[CB_GUFO].child;
      found[found_count].port = halogen_port(CB_GUFO);
      snprintf(found[found_count].host, sizeof(found[found_count].host), "%s", "127.0.0.1");
      found[found_count].is_gufo = true;
      found_count++;
      tracked[CB_GUFO] = true;
    }
    struct llama_server_metrics next[LLAMA_MAX_SERVERS];
    memset(next, 0, sizeof(next));
    for (unsigned f = 0; f < found_count; ++f) {
      struct llama_server_metrics *slot = &next[f];
      // Carry over previous state for this pid if host:port unchanged
      for (unsigned i = 0; i < LLAMA_MAX_SERVERS; ++i) {
        struct llama_server_metrics *prev = &state.servers[i];
        if (prev->found && prev->pid == found[f].pid && prev->port == found[f].port &&
            prev->is_halogen == found[f].is_halogen && prev->is_gufo == found[f].is_gufo &&
            strcmp(prev->host, found[f].host) == 0) {
          *slot = *prev;
          break;
        }
      }
      slot->found = true;
      slot->is_halogen = found[f].is_halogen;
      slot->is_gufo = found[f].is_gufo;
      slot->pid = found[f].pid;
      slot->port = found[f].port;
      snprintf(slot->host, sizeof(slot->host), "%s", found[f].host);
    }
    for (unsigned b = 0; b < CB_COUNT; ++b)
      if (!tracked[b] && hlg[b].active)
        halogen_teardown(b); // more engines than slots: container backends drop out
    memcpy(state.servers, next, sizeof(state.servers));
    state.last_discover = now;
  }

  // Poll (rate limited)
  for (unsigned i = 0; i < LLAMA_MAX_SERVERS; ++i) {
    struct llama_server_metrics *srv = &state.servers[i];
    if (!srv->found)
      continue;
    char pidpath[64];
    snprintf(pidpath, sizeof(pidpath), "/proc/%d", srv->pid);
    DIR *check = opendir(pidpath);
    if (check == NULL) {
      memset(srv, 0, sizeof(*srv));
      continue;
    }
    closedir(check);
    if (now - state.last_poll >= LLAMA_POLL_MIN_INTERVAL || !srv->had_last_sample)
      poll_server(srv, now);
  }
  if (now - state.last_poll >= LLAMA_POLL_MIN_INTERVAL)
    state.last_poll = now;

  img_update(now);

  // Aggregate per monitored device. Single-GPU systems attribute every
  // server to the only device; multi-GPU systems match on nvtop's process
  // table (fdinfo attribution).
  unsigned dev_index = 0;
  list_for_each_entry(device, devices, list) {
    struct llama_device_metrics agg = {0};
    for (unsigned i = 0; i < LLAMA_MAX_SERVERS; ++i) {
      struct llama_server_metrics *srv = &state.servers[i];
      if (!srv->found)
        continue;
      bool match = (dev_count == 1);
      if (!match) {
        for (unsigned p = 0; p < device->processes_count && !match; ++p) {
          if (device->processes[p].pid == srv->pid)
            match = true;
        }
      }
      if (!match)
        continue;
      agg.any_server = true;
      // Live rate: /metrics deltas when available; otherwise the tailed
      // server log (flashnext-style: no --metrics flag).
      double dec = srv->decode_tps, pre = srv->prefill_tps;
      if (srv->log_decode_tps > dec)
        dec = srv->log_decode_tps;
      if (srv->log_prefill_tps > pre)
        pre = srv->log_prefill_tps;
      // Silent prompt-processing: request launched, no rates logged yet
      if (srv->log_prefilling && dec < 0.01 && pre < 0.01)
        agg.prefill_in_progress = true;
      if (srv->engine_busy)
        agg.engine_busy = true;
      if (!srv->metrics_available && dec < 0.01 && pre < 0.01) {
        // Truly untrackable: HTTP rejected and no log attached. A log-tailed
        // server that is simply idle should fall through to "idle".
        if (srv->metrics_checked && srv->log_path[0] == '\0')
          agg.server_no_metrics = true;
        continue;
      }
      if (dec > 0.01 || pre > 0.01) {
        agg.decode_tps += dec;
        agg.prefill_tps += pre;
        agg.total_tps += dec + pre;
        agg.num_servers++;
      }
      // Freshest cache/accept within 120s
      if (srv->cache_hit_last > 0. && now - srv->cache_hit_last < 120.) {
        if (!agg.cache_valid || srv->cache_hit_last > agg.cache_hit_last) {
          agg.cache_valid = true;
          agg.cache_hit_pct = srv->cache_hit_pct;
          agg.cache_hit_last = srv->cache_hit_last;
        }
      }
      if (srv->accept_last > 0. && now - srv->accept_last < 120.) {
        if (!agg.accept_valid || srv->accept_last > agg.accept_last) {
          agg.accept_valid = true;
          agg.accept_val = srv->accept_val;
          agg.accept_last = srv->accept_last;
          agg.accept_is_commit = srv->accept_is_commit;
        }
      }
    }
    if (img.active) {
      bool img_match = (dev_count == 1);
      if (!img_match) {
        for (unsigned p = 0; p < device->processes_count && !img_match; ++p) {
          if (device->processes[p].pid == img.pid)
            img_match = true;
        }
      }
      if (img_match) {
        agg.img_active = true;
        snprintf(agg.img_state, sizeof(agg.img_state), "%s", img.state);
      }
    }
    // Graph scale: slowly decaying peak (prefill spikes dominate prefill
    // stretches; keep both visible by tracking the instantaneous max too)
    double instant = agg.total_tps > agg.prefill_tps ? agg.total_tps : agg.decode_tps;
    instant = instant > agg.prefill_tps ? instant : agg.prefill_tps;
    double prev_peak = state.peaks ? state.peaks[dev_index] : LLAMA_PEAK_FLOOR;
    double decayed = prev_peak * pow(0.5, LLAMA_POLL_MIN_INTERVAL / LLAMA_PEAK_HALFLIFE);
    double peak = fmax(instant, fmax(decayed, LLAMA_PEAK_FLOOR));
    if (state.peaks)
      state.peaks[dev_index] = peak;
    agg.peak_total_tps = peak;
    agg.valid = agg.any_server;
    state.per_dev_copy[dev_index] = agg;
    dev_index++;
  }
  state.num_dev = dev_count;
  if (getenv("LLAMA_DBG")) {
    fprintf(stderr, "llama dbg: dev_count=%u\n", dev_count);
    for (unsigned i = 0; i < LLAMA_MAX_SERVERS; ++i) {
      struct llama_server_metrics *s = &state.servers[i];
      if (!s->found)
        continue;
      fprintf(stderr, "  srv pid=%d %s:%u hal=%d gufo=%d avail=%d checked=%d had=%d dec=%.2f pre=%.2f pred=%.0f log=%s ldec=%.2f lpre=%.2f cachep=%.1f accp=%.1f\n", s->pid,
              s->host, s->port, s->is_halogen, s->is_gufo, s->metrics_available, s->metrics_checked, s->had_last_sample, s->decode_tps,
              s->prefill_tps, s->predicted_total, s->log_active ? s->log_path : "-", s->log_decode_tps,
              s->log_prefill_tps, s->cache_hit_pct, s->accept_val);
    }
    for (unsigned d = 0; d < dev_count && d < 2; ++d) {
      struct llama_device_metrics *a = &state.per_dev_copy[d];
      fprintf(stderr, "  dev%u any=%d nom=%d valid=%d dec=%.2f pre=%.2f tot=%.2f cv=%d c=%.1f av=%d a=%.1f cm=%d\n", d, a->any_server,
              a->server_no_metrics, a->valid, a->decode_tps, a->prefill_tps, a->total_tps,
              a->cache_valid, a->cache_hit_pct, a->accept_valid, a->accept_val, a->accept_is_commit);
    }
  }
}

const struct llama_device_metrics *llama_metrics_get(unsigned device_index) {
  if (!state.initialized || device_index >= state.num_dev)
    return NULL;
  return &state.per_dev_copy[device_index];
}
