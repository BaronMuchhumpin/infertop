INFERTOP
========

What is INFERTOP?
-----------------

INFERTOP is a fork of [nvtop](https://github.com/Syllo/nvtop) — the (h)top-like
GPU monitor — extended for **local LLM inference servers**. On top of everything
nvtop does (GPU utilization, memory, per-process list, all the vendor backends),
infertop understands the inference engine sitting on the GPU:

* **Decode / prefill throughput** as two separate plot lines (tokens/s), split
  automatically when the engine enters its prompt-eval phase
* **Prefix-cache hit rate** and **speculative-decode acceptance** tiles
* **Container-aware engine discovery**: llama.cpp-style servers running under
  podman are found automatically, with per-request metrics streamed from the
  container log when no `/metrics` endpoint exists
* **Request-history CSV recorder** (depth-tagged rows: prefill, progress,
  task-done, accept) for offline analysis; disable with `--nocsv`
* **Host CPU + memory lines** (): aggregate CPU% and RAM% plotted alongside the GPU — a feature upstream nvtop removed in 3.0
* **Integrated-GPU honesty**: on APUs (e.g. ROCm gfx1151) where engines pin host
  RAM through `/dev/kfd`, system RAM is reported as device memory so the memory
  plot matches reality

Because a picture is worth a thousand words:

![INFERTOP interface](/screenshot/NVTOP_ex1.png)

(upstream nvtop screenshot; infertop adds the engine tiles and split plots)

Configuration
-------------

Everything works out of the box with defaults tuned for llama.cpp servers. The
cluster-specific bits are environment variables:

| Variable | Default | Purpose |
|---|---|---|
| `INFERTOP_HALOGEN_CONTAINER` | `halogen` | podman container-name substring for the primary engine backend (empty string disables) |
| `INFERTOP_HALOGEN_PORT` | `8731` | host port for its `/health` polling |
| `INFERTOP_GUFO_CONTAINER` | `gufo` | second container backend (event=completed schema) |
| `INFERTOP_GUFO_PORT` | `8081` | its host port |
| `INFERTOP_RECORD_CSV` | `~/.local/share/infertop/...` | request-history CSV path |
| `INFERTOP_ROCM_SYSTEMRAM` | on | set `0` to disable system-RAM-as-device-memory on integrated GPUs |
| `INFERTOP_IMG_STATUS` | `~/.local/share/infertop/img_status` | legend-only image-gen job status file (empty disables) |

Request-history CSV recorder
----------------------------

While infertop runs, it appends one row per inference event to
`~/.local/share/infertop/llama_records.csv` (override path with
`INFERTOP_RECORD_CSV`; empty string or `--nocsv` disables). Append-only,
created with a header on first write:

```
unix_ts,iso,host,port,pid,slot,task,event,prompt_tok,gen_tok,pp_tps,tg_tps,accept,mean_len,depth_tok,mem_avail_mib,swap_free_mib
```

| Column | Meaning |
|---|---|
| `unix_ts`, `iso` | sample time (epoch seconds, ISO-8601 local) |
| `host`, `port`, `pid` | which engine server the row came from |
| `slot`, `task` | llama.cpp slot and monotonically increasing task id — group rows of one request by `(slot, task)` |
| `event` | `prefill` (prompt processed), `progress` (3 s timing tick), `taskdone` (request finished), `accept` (spec-decode draft acceptance) |
| `prompt_tok`, `gen_tok` | prompt / generated token counts for the event |
| `pp_tps`, `tg_tps` | prefill rate (prompt eval) and decode rate (generation), tokens/s |
| `accept`, `mean_len` | speculative-decode draft acceptance rate and mean accepted length |
| `depth_tok` | context depth at the event (prompt + generated tokens) |
| `mem_avail_mib`, `swap_free_mib` | system memory pressure at sample time |

Empty cells mean the event did not carry that metric. Typical request: one
`prefill` row, zero or more `progress` rows, one `accept` row when spec-decode
is active, and a final `taskdone` row. The recorder is what powers offline
weekly/period analysis (throughput distributions, cache-hit trends, memory
pressure correlation).

Building
--------

Same requirements as nvtop (ncurses, cmake, a C99 compiler; libdrm/libgbm for
the AMD/Intel backends). See upstream docs for distribution-specific packages.

```sh
git clone https://github.com/BaronMuchhumpin/infertop.git
cd infertop
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
```

Relationship to nvtop
---------------------

INFERTOP is a derivative work of NVTOP by Maxime Schmitt, licensed under the
GNU GPLv3 (or any later version). All original copyright notices are retained;
the fork's changes are described above and in the source. See `COPYING` for the
full license text.

The goal is to keep rebasing on upstream nvtop and keep the engine-metric layer
clean enough that upstream might one day take pieces of it back.
