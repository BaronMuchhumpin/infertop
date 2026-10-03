/*
 * Host CPU / memory sampling for the --cpumem plot lines.
 * Part of the infertop fork (GPLv3). Reads /proc/stat and /proc/meminfo.
 */
#ifndef __INFERTOP_EXTRACT_CPUMEMINFO_H__
#define __INFERTOP_EXTRACT_CPUMEMINFO_H__

// Sample host CPU utilization (delta since previous call) and system memory
// usage as percentages 0..100. First call reports 0 for CPU (no baseline).
void cpumem_sample(unsigned *cpu_percent, unsigned *mem_percent);

#endif
