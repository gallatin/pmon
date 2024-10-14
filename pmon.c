/******************************************************************************
SPDX-License-Identifier: BSD-2-Clause

Copyright (c) 2024, Netflix Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

 2. Neither the name of the Myricom Inc, nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

***************************************************************************/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#ifdef __FreeBSD__
#include <sys/cpuctl.h>
#include <x86/specialreg.h>
#else
/* cribbed from FreeBSD specialreg.h */
#define CPUID_MODEL 	 	0x000000f0
#define CPUID_FAMILY 	 	0x00000f00
#define CPUID_EXT_MODEL 	0x000f0000
#define CPUID_EXT_FAMILY 	0x0ff00000
#define CPUID_TO_MODEL(id) \
    ((((id) & CPUID_MODEL) >> 4) | \
    (((id) & CPUID_EXT_MODEL) >> 12))
#define CPUID_TO_FAMILY(id) \
    ((((id) & CPUID_FAMILY) >> 8) + \
    (((id) & CPUID_EXT_FAMILY) >> 20))
#endif



#define AMD_ENERGY_CORE_MSR 	0xC001029A
#define AMD_ENERGY_PKG_MSR 	0xC001029B
#define AMD_ENERGY_PWR_UNIT_MSR 0xC0010299
#define AMD_ENERGY_UNIT_MASK	0x01F00
#define AMD_ENERGY_MASK		0xFFFFFFFF


#define INTEL_ENERGY_PKG_MSR		0x611
#define INTEL_ENERGY_DRAM_MSR		0x619
#define INTEL_ENERGY_PWR_UNIT_MSR	0x606

static u_int	cpu_procinfo;
static u_int	cpu_id;
static u_int	cpu_high;
static u_int	cpu_feature;
static u_int	cpu_feature2;
static char	cpu_vendor[20];
static u_int	cpu_family;
static u_int	cpu_model;
static u_int	cpu_count;
static u_int	share_count;
static u_int	amd_energy_units;
static double	energy_units;
static double	dram_units;
static double	scale = 1.0;
static int 	verbose;
static u_int	pkg_msr;
static u_int	core_msr;
static u_int	dram_msr;

struct softc {
	int fd;
	double last;
};

struct softc *softc;

enum processor_type {
	AMD,
	INTEL
};
static enum processor_type cpu;

static __inline void
do_cpuid(u_int ax, u_int *p)
{
	__asm __volatile("cpuid"
	    : "=a" (p[0]), "=b" (p[1]), "=c" (p[2]), "=d" (p[3])
	    :  "0" (ax));
}

static __inline void
cpuid_count(u_int ax, u_int cx, u_int *p)
{
        __asm __volatile("cpuid"
            : "=a" (p[0]), "=b" (p[1]), "=c" (p[2]), "=d" (p[3])
            :  "0" (ax), "c" (cx));
}

#ifdef __FreeBSD__
static uint64_t
read_msr(struct softc *sc, int reg)
{
	cpuctl_msr_args_t msr;
	int err;

	bzero(&msr, sizeof(msr));
	msr.msr = reg;
	err = ioctl(sc->fd, CPUCTL_RDMSR, &msr);
	if (err != 0) {
		perror("ioctl");
		exit(1);
	}
	return (msr.data);
}
#else
static uint64_t
read_msr(struct softc *sc, u_int reg)
{
	uint64_t data;

	if (pread(sc->fd, &data, sizeof(data), reg) != sizeof(data)) {
		perror("rdmsr:pread");
		exit(1);
	}
	return (data);
}
#endif

static void
open_msrs(void)
{
	char path[MAXPATHLEN];
	struct softc *sc;
	u_int core, i;

	for (core = 0; core < cpu_count + 2; core++) {
		if (core >= cpu_count) {
			i = 0; /* for pkg & Intel dram power */
		} else {
			i = core;
			if (cpu == INTEL && core != 0)
				continue;
		}
		sc = &softc[core];
#ifdef __FreeBSD__
		sprintf(path, "/dev/cpuctl%d", i * share_count);
#else
		sprintf(path, "/dev/cpu/%d/msr", i * share_count);
#endif
		sc->fd = open(path, O_RDONLY);
		if (sc->fd == -1) {
			perror("open");
#ifdef __FreeBSD__
			printf("Did you remember to kldload cpuctl?\n");
#else
			printf("Did you remember to modprobe msr?\n");
#endif
			exit(1);
		}
	}
}
static void
identify_cpu(void)
{
	struct softc *sc;
	uint64_t data;
	u_int regs[4];

	do_cpuid(0, regs);
	cpu_high = regs[0];
	((u_int *)&cpu_vendor)[0] = regs[1];
	((u_int *)&cpu_vendor)[1] = regs[3];
	((u_int *)&cpu_vendor)[2] = regs[2];
	cpu_vendor[12] = '\0';

	do_cpuid(1, regs);
	cpu_id = regs[0];
	cpu_procinfo = regs[1];
	cpu_feature = regs[3];
	cpu_feature2 = regs[2];

	cpu_family = CPUID_TO_FAMILY(cpu_id);
	cpu_model = CPUID_TO_MODEL(cpu_id);

	if (!strncmp(cpu_vendor, "AuthenticAMD", sizeof(cpu_vendor))) {
		cpu = AMD;
		pkg_msr = AMD_ENERGY_PKG_MSR;
		core_msr = AMD_ENERGY_CORE_MSR;
		switch (cpu_family) {
		case 0x17:
			switch (cpu_model) {
			case 0x8:
			case 0x31:
				break;
			default:
				printf("unsupported CPU 0x%x 0x%x\n",
				    cpu_family, cpu_model);
				exit(1);
			}
			break;
		case 0x19:
			switch (cpu_model) {
			case 0x01:
			case 0x30:
			case 0x10:
			case 0x11:
			case 0xa0:
			case 0x19:
				break;
			default:
				printf("unsupported CPU 0x%x 0x%x\n",
				    cpu_family, cpu_model);
				exit(1);
			}
			break;
		case 0x1a:
			switch (cpu_model) {
			case 0x02:
			case 0x10:
			case 0x11:
				break;
			default:
				printf("unsupported CPU 0x%x 0x%x\n",
				    cpu_family, cpu_model);
				exit(1);
			}
			break;
		default:
			printf("unsupported CPU 0x%x 0x%x\n",
			    cpu_family, cpu_model);
			exit(1);
		}
	} else if (!strncmp(cpu_vendor, "GenuineIntel", sizeof(cpu_vendor))) {
		cpu = INTEL;
		if (cpu_family != 0x6) {
			printf("unsupported CPU 0x%x 0x%x\n",
			    cpu_family, cpu_model);
			exit(1);
		}
		pkg_msr = INTEL_ENERGY_PKG_MSR;
		dram_msr = INTEL_ENERGY_DRAM_MSR;
		switch (cpu_model) {
		case 0x4f: /*  Xeon(R) E5-2697A v4, c098.ord001.dev */
			break;
		case 0x55:  /* Xeon(R) Gold 6122, c004.mia005.dev */
			    /* Xeon(R)  D-2143IT c207.sjc002.dev */
			    /* Not a typo.. both have same model!! */
			break;
		case 0x56:  /* Xeon(R) D-1518, c025.sjc003.dev */
			    /* Xeon(R) D-1541, c620.sjc002.dev */
			    /* Not a typo.. both have same model!! */
			break;
		}
	} else {
		printf("Support for CPU vendor  %s not implemented\n",
		    cpu_vendor);
		exit(1);
	}
	cpuid_count(0x8000001e, 0, regs);
	share_count = ((regs[1] >> 8) & 0xff) + 1;
	cpu_count = sysconf(_SC_NPROCESSORS_CONF) / share_count;
	softc = calloc(cpu_count + 1, sizeof(*softc));
	if (softc == NULL) {
		perror("malloc");
		exit(1);
	}
	open_msrs();
	sc = &softc[0];
	if (cpu == AMD) {
		data = read_msr(sc,  AMD_ENERGY_PWR_UNIT_MSR);
		amd_energy_units = (data & AMD_ENERGY_UNIT_MASK) >> 8;
	} else { /* assume intel */
		data = read_msr(sc, INTEL_ENERGY_PWR_UNIT_MSR);
		energy_units = pow(0.5, (double)((data >> 8) & 0x1f));
		dram_units = pow(0.5, (double)16);
	}
	if (verbose > 1) {
		printf("%d threads, %d CPUs\n", cpu_count * share_count,
		    cpu_count);
		if (cpu == AMD)
			printf("energy_units %d\n", amd_energy_units);
		else
			printf("energy_units %lf\n", energy_units);
	}
}

static double
amd_add_power(uint64_t input)
{
	double energy;

	energy = (input * 1000000.0) / (1 << amd_energy_units);
	return (energy);
}

static double
intel_add_power(uint64_t input, double units)
{
	return (input * units);
}

static void
read_power(void)
{
	struct softc *sc;
	uint64_t data;
	u_int core, first_core, max;
	double core_sum, delta, dram, energy, watts;
	static bool first = true;

	core_sum = dram = 0.0;

	/* just read the pkg power by default */
	first_core = cpu_count;
	max = cpu_count + 1;
	if (verbose && core_msr != 0) {
		/* AMD: read power from each core */
		first_core = 0;
	} else if (verbose && dram_msr != 0) {
		/* Intel: Cant read core power, read Dimm using core N-1 */
		max++;
	}
	for (core = first_core; core < max; core++) {
		sc = &softc[core];
		if (core == cpu_count) {
			data = read_msr(sc, pkg_msr);
		} else if (core == cpu_count + 1) {
			data = read_msr(sc, dram_msr);
		} else {
			data = read_msr(sc, core_msr);
		}
		if (cpu == AMD) {
			energy = amd_add_power(data);
			/* convert from uJoules to watts */
			watts = energy / 1000000.0;
		} else {
			watts = intel_add_power(data,
			    core == cpu_count ? energy_units : dram_units);
		}
		delta = watts - sc->last;
		sc->last = watts;
		if (first && verbose < 2)
			continue;

		if (core == cpu_count) {
			if (!verbose)
				printf("%4.2lf", delta * scale);
			else
				printf("pkg: %4.2lf", delta * scale);
			if (verbose && amd_energy_units != 0)
				printf("  core sum=%4.2lf\n", core_sum * scale);
			if (core == max - 1)
				printf("\n");
		} else if (verbose && core == cpu_count + 1) {
			printf("\tdram: %4.2lf", delta * scale);
			if (core == max - 1)
				printf("\n");
		} else if (verbose && amd_energy_units != 0) {
			if (core == 0)
				printf("============================================================================\n");
			if (core % 8 == 0)
				printf("core %3d:\t", core);
			printf("%3.2lf\t",  delta * scale);
			if ((core % 8 == 7) || (core == cpu_count - 1))
				printf("\n");
			if (core == cpu_count - 1)
				printf("============================================================================\n");
		}
		core_sum += delta;
	}
	first = false;
	fflush(stdout);
}

static void
usage(char *name)
{
	fprintf(stderr, "usage: %s [-v] [interval]\n", name);
}

int
main(int argc, char **argv)
{
	int timeo = 1;
	char c;


	while ((c = getopt(argc, argv, "v")) != -1) {
		switch (c) {
		case 'v':
			verbose++;
			break;
		default:
			usage(argv[0]);
		}
	}
	argc -= optind;
	argv += optind;

	if (*argv)
		timeo = atof(*argv);

	scale = 1.0 / (double) timeo;
	identify_cpu();
	while (1) {
		read_power();
		sleep(timeo);
	}
}
