/*
 * Host CPU / memory sampling for the --cpumem plot lines.
 * Part of the infertop fork (GPLv3), (c) 2026 BaronMuchhumpin.
 *
 * CPU: aggregate "cpu" line of /proc/stat, busy delta / total delta since the
 * previous sample. Mem: (MemTotal - MemAvailable) / MemTotal from /proc/meminfo.
 * Both are cheap single-line reads; safe to call at the UI refresh rate.
 */
#include "nvtop/extract_cpumeminfo.h"

#include <stdio.h>
#include <string.h>

void cpumem_sample(unsigned *cpu_percent, unsigned *mem_percent) {
  static unsigned long long prev_idle = 0, prev_total = 0;
  *cpu_percent = 0;
  *mem_percent = 0;

  FILE *f = fopen("/proc/stat", "r");
  if (f) {
    char line[256];
    if (fgets(line, sizeof(line), f) && strncmp(line, "cpu ", 4) == 0) {
      unsigned long long v[10] = {0};
      int n = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                     &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
      if (n >= 4) {
        unsigned long long idle = v[3] + (n > 4 ? v[4] : 0); // idle + iowait
        unsigned long long total = 0;
        for (int i = 0; i < n && i < 10; ++i)
          total += v[i];
        if (prev_total && total > prev_total) {
          unsigned long long dt = total - prev_total, di = idle - prev_idle;
          unsigned long long busy = dt > di ? dt - di : 0;
          *cpu_percent = (unsigned)((busy * 100 + dt / 2) / dt);
        }
        prev_idle = idle;
        prev_total = total;
      }
    }
    fclose(f);
  }

  f = fopen("/proc/meminfo", "r");
  if (f) {
    char line[128];
    unsigned long long total = 0, avail = 0;
    while (fgets(line, sizeof(line), f)) {
      if (sscanf(line, "MemTotal: %llu kB", &total) == 1)
        continue;
      if (sscanf(line, "MemAvailable: %llu kB", &avail) == 1)
        break;
    }
    fclose(f);
    if (total > avail)
      *mem_percent = (unsigned)(((total - avail) * 100 + total / 2) / total);
  }
}
