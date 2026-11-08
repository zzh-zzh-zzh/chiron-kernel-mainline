/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MSM8998 relative energy curves, derived from the Qualcomm tables in
 * LineageOS/android_kernel_xiaomi_msm8998, commit
 * 7231883a368604f90806e167890e00f515a9927c, msm8998.dtsi.
 * Original data: Copyright (c) 2016, The Linux Foundation.
 *
 * These are capacity/power pairs, NOT indexed frequency measurements.
 * Interpolate by normalized capacity using the current domain maximum.
 * A quarter of the shared cluster cost is apportioned to each core: this
 * approximates fully occupied clusters and underestimates shared costs
 * for a lightly occupied cluster. Idle costs are not modelled by EAS.
 * Values use one common abstract scale, not microwatts. Voltage margins
 * A provisional board calibration scales domain 1 relative to domain 0.
 * It does not calibrate absolute microwatts or thermal power limits.
 */
#ifndef _CHIRON_CPUFREQ_EM_H
#define _CHIRON_CPUFREQ_EM_H

#include <linux/kernel.h>
#include <linux/math64.h>

struct chiron_em_point {
	unsigned int capacity;
	unsigned int core;
	unsigned int cluster;
};

static const struct chiron_em_point chiron_em_0[] = {
	{ 65, 11, 17 },
	{ 80, 20, 18 },
	{ 96, 25, 18 },
	{ 113, 30, 20 },
	{ 130, 33, 21 },
	{ 147, 35, 22 },
	{ 164, 36, 23 },
	{ 181, 42, 24 },
	{ 194, 47, 27 },
	{ 211, 54, 29 },
	{ 228, 62, 30 },
	{ 243, 67, 32 },
	{ 258, 73, 33 },
	{ 275, 79, 35 },
	{ 292, 88, 38 },
	{ 308, 95, 39 },
	{ 326, 104, 42 },
	{ 342, 111, 46 },
	{ 368, 134, 48 },
	{ 384, 155, 53 },
	{ 401, 178, 59 },
	{ 419, 201, 66 },
};

static const struct chiron_em_point chiron_em_1[] = {
	{ 129, 56, 24 },
	{ 148, 76, 25 },
	{ 182, 91, 26 },
	{ 216, 105, 29 },
	{ 247, 118, 30 },
	{ 278, 135, 33 },
	{ 312, 150, 35 },
	{ 344, 162, 37 },
	{ 391, 181, 38 },
	{ 419, 196, 40 },
	{ 453, 214, 43 },
	{ 487, 229, 44 },
	{ 509, 248, 46 },
	{ 546, 280, 50 },
	{ 581, 316, 54 },
	{ 615, 354, 60 },
	{ 650, 392, 63 },
	{ 676, 439, 70 },
	{ 712, 495, 74 },
	{ 739, 565, 80 },
	{ 776, 622, 87 },
	{ 803, 691, 96 },
	{ 834, 792, 104 },
	{ 881, 889, 120 },
	{ 914, 1059, 130 },
	{ 957, 1244, 171 },
	{ 975, 1375, 178 },
	{ 996, 1549, 185 },
	{ 1016, 1617, 200 },
	{ 1021, 1677, 202 },
	{ 1024, 1683, 203 },
};

/* Keep fractional cluster shares and interpolation precision. */
#define CHIRON_EM_SCALE 1024U

static unsigned long chiron_em_point_power(const struct chiron_em_point *p)
{
	return (4 * p->core + p->cluster) * (CHIRON_EM_SCALE / 4);
}

static unsigned long chiron_em_reference_power(unsigned int domain, unsigned long freq,
									unsigned long max_freq)
{
	const struct chiron_em_point *points;
	unsigned int count, i;
	u64 target, low, high, power;

	if (domain > 1 || !freq || !max_freq || freq > max_freq)
		return 0;

	points = domain ? chiron_em_1 : chiron_em_0;
	count = domain ? ARRAY_SIZE(chiron_em_1) : ARRAY_SIZE(chiron_em_0);
	target = (u64)freq * points[count - 1].capacity;
	low = (u64)points[0].capacity * max_freq;
	if (target <= low)
		return chiron_em_point_power(&points[0]);

	for (i = 1; i < count; i++) {
		high = (u64)points[i].capacity * max_freq;
		if (target <= high) {
			power = chiron_em_point_power(&points[i - 1]);
			power += div64_u64((target - low) *
					 (chiron_em_point_power(&points[i]) - power),
					 high - low);
			return power;
		}
		low = high;
	}

	return chiron_em_point_power(&points[count - 1]);
}

/*
 * Battery-side calibration, 2026-10-24: the middle-frequency SHA256 fit
 * gives a relative domain-1 factor near 0.62; zlib supports the direction
 * but has greater baseline drift. Round up to 0.65 to limit compensation.
 * Keep the reference curve shape and capacities. Unmeasured frequencies,
 * especially boost, remain extrapolations. This candidate needs device A/B
 * validation; reducing model cost is not a measured reduction in watts.
 */
static unsigned long chiron_em_power(unsigned int domain, unsigned long freq,
				   unsigned long max_freq)
{
	unsigned long power;

	power = chiron_em_reference_power(domain, freq, max_freq);
	if (domain == 1)
		power = div64_u64((u64)power * 13, 20);

	return power;
}

/* Match modern EAS cost = 10 * power / current performance capacity. */
static unsigned long chiron_em_cost(unsigned long power, unsigned long freq,
								   unsigned long max_freq,
								   unsigned long capacity)
{
	unsigned long performance;

	if (!freq || !max_freq || freq > max_freq || !capacity)
		return 0;
	performance = div64_u64((u64)capacity * freq, max_freq);
	if (!performance)
		return 0;
	return div64_u64((u64)power * 10, performance);
}

#endif
