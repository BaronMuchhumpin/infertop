/*
 *
 * Infertop extension: llama.cpp inference serving metrics.
 *
 * Discovers running llama-server processes, polls their Prometheus
 * /metrics endpoint (enabled with --metrics) and computes live
 * tokens/s rates from the token counters.
 *
 */

#ifndef LLAMA_METRICS_H__
#define LLAMA_METRICS_H__

#include <stdbool.h>
#include <sys/types.h>

struct list_head;

#define LLAMA_MAX_SERVERS 8

#define LLAMA_MAX_TRACKED_TASKS 8

// One generation task (prompt + decode) seen in the server log
struct llama_task_record {
  bool used;
  unsigned task;
  double prompt_tok; // From the prompt-eval summary line
  double gen_tok;    // Latest n_gen/n_decoded for this task
  double prog_last;  // Monotonic time of last throttled progress write
};

struct llama_server_metrics {
  bool found;               // Process discovered in /proc
  bool metrics_available;   // /metrics endpoint answered
  bool metrics_checked;     // A poll attempt has been made since discovery
  bool engine_busy;         // /health reports busy or in-flight work
  pid_t pid;
  char host[64];
  unsigned port;
  bool is_halogen;          // halogen container backend (logs via podman,
                            // state via /health; no /metrics, no log file)
  bool is_gufo;             // gufo-runtime container backend (same plumbing,
                            // event=completed log lines)
  // Prompt-cache hit rate and spec-decode acceptance (EMA over completed
  // requests). accept_val is a percentage unless accept_is_commit, in which
  // case it is halogen's commit/round.
  double cache_hit_pct;
  double cache_hit_last;
  double accept_val;
  double accept_last;
  bool accept_is_commit;
  // Log-tail state: server stdout/stderr redirected to a regular file.
  // llama-server prints per-slot "n_gen = N, tg = X t/s" live during
  // generation, which is a far more reliable rate source than the
  // Prometheus counters (they only update at request completion in
  // llama.cpp <= ~b2900).
  char log_path[256];
  ino_t log_ino;
  long long log_offset;
  bool log_active;
  double log_decode_tps;    // From "n_gen = ... tg(_3s) = X t/s" lines
  double log_prefill_tps;   // From "prompt eval time ... X tokens per second"
  double log_decode_last;   // Monotonic time of last generation line parsed
  double log_prefill_last;
  bool log_prefilling;      // launch_slot_ seen, prompt-eval not yet arrived
  double log_prefill_start;
  // CSV recorder: token accounting per task for depth-tagged history
  struct llama_task_record tasks[LLAMA_MAX_TRACKED_TASKS];
  // Counters (absolute) and time of last sample
  double predicted_total;
  double prompt_total;
  double monotonic_last;    // Seconds (CLOCK_MONOTONIC) of last sample
  bool had_last_sample;
  // Smoothed rates (tokens per second)
  double decode_tps;        // Generated tokens per second
  double prefill_tps;       // Prompt tokens per second
  unsigned idle_ticks;
};

struct llama_device_metrics {
  bool valid;               // True when at least one server produced traffic
  bool any_server;          // A llama-server process exists
  bool server_no_metrics;   // Server exists but /metrics did not answer
  bool prefill_in_progress; // Silent prompt-processing phase inferred
  double decode_tps;        // Aggregated over servers on this device
  double prefill_tps;
  double total_tps;
  double peak_total_tps;    // Slowly decaying peak for graph scaling
  unsigned num_servers;
  // Freshest engine-reported cache/accept values (valid < 120s)
  bool cache_valid, accept_valid;
  bool engine_busy;         // Any matched server has work in flight
  double cache_hit_pct, cache_hit_last;
  double accept_val, accept_last;
  bool accept_is_commit;
  // Image generation job (Qwen-Image-2.1). GUI-only: image generation
  // is never written to the CSV recorder.
  bool img_active;
  char img_state[24]; // "load…", "9/20", "vae…", "save"
};

// Must be called once per interface refresh, with the monitored device list.
// Internally rate-limits actual HTTP polling to ~4 Hz.
void llama_metrics_refresh(struct list_head *devices);

// Metrics for a device index (position in monitored device list); NULL if none.
const struct llama_device_metrics *llama_metrics_get(unsigned device_index);

#endif // LLAMA_METRICS_H__
