/*
 * Copyright (C) 2021 Matt Merhar <mattmerhar@protonmail.com> - All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2 or v.3.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include "common.h"
#include "plugins.h"

#define INTERRUPTS "/proc/interrupts"
/* Stop processing after this many IRQs have been seen */
#define MAX_IRQS 256
/* Sufficient even on a system with 256 threads */
#define MAX_LINE 4096
#define MAX_TOKENS 32

typedef struct irqstat_t {
	char *name;
	char *description;
	unsigned long hwirq;
	bool has_hwirq; /* hwirq can be zero */
	unsigned long count;
} irqstat_t;

bool isnumeric(char *string)
{
	if (!string || !*string)
		return false;

	/* Check that every character is a digit */
	while (*string)
		if (!isdigit(*string++))
			return false;

	return true;
}

bool isempty(char *string)
{
	if (!string || !*string)
		return true;

	/* Check that it's not all whitespace */
	while (*string)
		if (!isspace(*string++))
			return false;

	return true;
}

bool endswith(char *haystack, char *needle)
{
	size_t haystack_length = 0, needle_length = 0;

	if (!haystack || !needle)
		return false;

	haystack_length = strlen(haystack);
	needle_length = strlen(needle);

	if (needle_length > haystack_length)
		return false;

	return !strcmp((char *)(haystack + haystack_length - needle_length), needle);
}

size_t read_interrupts(irqstat_t irqstats[], bool config)
{
	FILE *interrupts;
	char line_buf[MAX_LINE] = {0};
	size_t irq_num = 0, cpu_num = 0, line_num = 0;
	unsigned long hwirq = 0;

	interrupts = fopen(INTERRUPTS, "r");
	if (!interrupts) {
		fprintf(stderr, "fopen: %m\n");
		return 0;
	}

	while (fgets(line_buf, sizeof(line_buf), interrupts)) {
		char *pos = NULL, *eol = NULL;
		char *tokens[MAX_TOKENS] = {0};
		size_t length = 0, token_num = 0, token_start = 0;

		if (irq_num == MAX_IRQS)
			break;

		/* Ensure EOL is '\n' */
		if ((eol = strchr(line_buf, '\n')) != NULL) {
			*eol = '\0';
		}
		/* Otherwise we've overrun line_buf (or the last line doesn't have a newline - unlikely) */
		else {
			fprintf(stderr, "line_num=%zu had line_buf overflow\n", line_num);
			return 0;
		}

		pos = strtok(line_buf, " ");
		/* There should be no empty lines */
		if (!pos) {
			fprintf(stderr, "line_num=%zu is empty\n", line_num);
			return 0;
		}

		/* The first line has a column per CPU - count them */
		if (line_num == 0) {
			while (pos) {
				if (strncmp(pos, "CPU", 3)) {
					fprintf(stderr, "expected CPU at line_num=%zu, got '%s'\n", line_num, pos);
					return 0;
				}

				cpu_num++;

				pos = strtok(NULL, " "); /* Move on to next CPU */
			}

			if (!cpu_num) {
				fprintf(stderr, "no CPUs found\n");
				return 0;
			}

			cpu_num--; /* Arrays start at 0 */
			line_num++;

			continue;
		}

		/* The rest of the lines should each contain an interrupt name, value(s), and optional description */
		if (!strchr(pos, ':')) {
			fprintf(stderr, "expected name '%s' is missing ':'\n", pos);
			return 0;
		}

		length = strlen(pos);
		irqstats[irq_num].name = malloc(length); /* We're discarding the ':' */
		if (!irqstats[irq_num].name) {
			fprintf(stderr, "malloc: %m\n");
			return 0;
		}

		strncpy(irqstats[irq_num].name, pos, --length); /* Discard ':' */
		irqstats[irq_num].name[length] = '\0';

		/* Convert each counter value to long and add it to the total for this IRQ */
		for (size_t cpu_idx = 0; cpu_idx <= cpu_num; cpu_idx++) {
			pos = strtok(NULL, " ");
			/* Some interrupts, such as 'ERR' or 'MIS' will only have a single counter rather than one per CPU */
			if (!pos) {
				if (!cpu_idx) {
					fprintf(stderr, "irqstats[%zu].name = '%s' has no counters\n", irq_num, irqstats[irq_num].name);
					return 0;
				}

				break;
			}

			/* If it's not a positive integer, we may have run into the description unexpectedly */
			if (!isnumeric(pos)) {
				if (!cpu_idx) {
					fprintf(stderr, "irqstats[%zu].name = '%s' has only garbage '%s'\n", irq_num, irqstats[irq_num].name, pos);
					return 0;
				}

				break;
			}

			irqstats[irq_num].count += strtol(pos, NULL, 10);
		}

		/* Skip over the description parsing unless we're running 'config' */
		if (!config)
			goto next_line;

		/* If EOL wasn't reached when parsing counters, try to grab the IRQ description */
		if (!pos || (pos = (pos + strlen(pos))) == eol || isempty(++pos))
			goto next_line;

		/* Skip over leading whitespace. We know there's something there, per isempty() above */
		for (; *pos && isblank(*pos); pos++);

		/* NULL out any trailing whitespace */
		for (char *eos = pos + strlen(pos) - 1; *eos && isspace(*eos); *eos-- = '\0');

		/* It won't be any longer than this */
		irqstats[irq_num].description = malloc(strlen(pos) + 1);
		if (!irqstats[irq_num].description) {
			fprintf(stderr, "malloc: %m\n");
			return 0;
		}
		*irqstats[irq_num].description = '\0';

		/* If it's not a numbered IRQ, then the description is simply everything that remains */
		if (!isnumeric(irqstats[irq_num].name)) {
			strcpy(irqstats[irq_num].description, pos);
			goto next_line;
		}

		tokens[token_num] = pos = strtok(pos, " ");
		while ((pos = strtok(NULL, " ")) != NULL && token_num < (MAX_TOKENS - 1)) {
			tokens[++token_num] = pos;
		}

		/* If we only have one token, there's nothing left to parse */
		if (!token_num) {
			strcpy(irqstats[irq_num].description, pos);
			goto next_line;
		}

#ifdef __sparc__
		/* SPARC's interrupts layout differs */
		if (token_num >= 2) {
			token_start = 2;
			/* SPARC has been seen to have many duplicate 'MSIQ' interrupts (one per thread), so always show the IRQ number here to differentiate */
			if (!strcmp(tokens[2], "MSIQ")) {
				irqstats[irq_num].hwirq = strtoul(irqstats[irq_num].name, NULL, 10);
				irqstats[irq_num].has_hwirq = true;
			}
#else
		/* Newer ARM, MIPS, some x86, etc. */
		if (token_num >= 2 && isnumeric(tokens[1])) {
			token_start = 2;

			/*                                                   [0]        [1][2]       [3-] */
			/* 38:     150262          0          0          0   OpenPIC    38 Level     i2c-mpc, i2c-mpc */
			/* Interrupt 38, for device(s): i2c-mpc, i2c-mpc */
			/* */
			/*                     [0]   [1] [2-] */
			/*  3:  247552271      MIPS   3  ehci_hcd:usb1 */
			/* Interrupt 3, for device(s): ehci_hcd:usb1 */
			/* */
			/*                 [0]            [1][2]       [3-] */
			/* 33:     617373  f1010140.gpio  17 Edge      pps.-1 */
			/* Interrupt 33, for device(s): pps.-1 [17] */

			if ((hwirq = strtoul(tokens[1], NULL, 10)) != strtoul(irqstats[irq_num].name, NULL, 10)) {
				irqstats[irq_num].hwirq = hwirq;
				irqstats[irq_num].has_hwirq = true;
			}

			/* MIPS has been seen to not show the type */
			if (!strcmp(tokens[2], "Edge") || !strcmp(tokens[2], "Level") || !strcmp(tokens[2], "None"))
				token_start = 3;
#endif
		}
		/* Most x86 interrupts, old ARM */
		else {
			token_start = 1;

			/* Strip away the text component from x86 APIC/PCI interrupts e.g. 18-fasteoi or 1048579-edge */
			if (isdigit(*tokens[1]) && (endswith(tokens[1], "-fasteoi") || endswith(tokens[1], "-edge"))) {
				token_start = 2;

				if ((hwirq = strtoul(tokens[1], NULL, 10)) != strtoul(irqstats[irq_num].name, NULL, 10)) {
					irqstats[irq_num].hwirq = hwirq;
					irqstats[irq_num].has_hwirq = true;
				}
			}
		}

		/* Concatenate the needed tokens with a space */
		for (*irqstats[irq_num].description = '\0'; token_start <= token_num; token_start++) {
			strcat(irqstats[irq_num].description, tokens[token_start]);
			strcat(irqstats[irq_num].description, " ");
		}

		*(irqstats[irq_num].description + strlen(irqstats[irq_num].description) - 1) = '\0';

next_line:
			line_num++;
			irq_num++;
	}

	fclose(interrupts);

	return irq_num;
}

bool irqstats_autoconf()
{
	if (!access(INTERRUPTS, R_OK)) {
		printf("yes\n");
		return false;
	}
	else {
		printf("no (%s isn't readable: %m)\n", INTERRUPTS);
		return true;
	}
}

bool irqstats_config() {
	irqstat_t irqstats[MAX_IRQS] = {0};
	size_t irq_num = read_interrupts(irqstats, true);

	if (irq_num == 0) {
		fprintf(stderr, "no interrupts found\n");
		return false;
	}

	printf("graph_title Individual interrupts\n"
		"graph_args --base 1000 --logarithmic\n"
		"graph_vlabel interrupts / ${graph_period}\n"
		"graph_category system\n"
		"graph_info Shows the number of different IRQs received by the kernel.  High disk or network traffic can cause a high number of interrupts (with good hardware and drivers this will be less so). Sudden high interrupt activity with no associated higher system activity is not normal.\n\n");

	printf("graph_order");
	for (size_t i = 0; i < irq_num; i++)
		printf(" i%s", irqstats[i].name);
	printf("\n");

	for (size_t i = 0; i < irq_num; i++) {
		/* Some, like ERR and MIS, do not have a description */
		printf("i%s.label %s", irqstats[i].name, (irqstats[i].description ? irqstats[i].description : irqstats[i].name));
		if (irqstats[i].has_hwirq)
			printf(" [%lu]", irqstats[i].hwirq);
		printf("\n");

		if (irqstats[i].description) {
			printf("i%s.info Interrupt %s, for device(s): %s", irqstats[i].name, irqstats[i].name, irqstats[i].description);
			if (irqstats[i].has_hwirq)
				printf(" [%lu]", irqstats[i].hwirq);
			printf("\n");
		}
		/* NOTE: The original Perl plugin does a case insensitive substring match. '#define _GNU_SOURCE' and strcasestr() could imitate this. */
		else if (!strcmp(irqstats[i].name, "NMI")) {
			printf("iNMI.info Non-maskable interrupt. Either 0 or quite high. If it's normally 0 then just one NMI will often mark some hardware failure.\n");
		}
		else if (!strcmp(irqstats[i].name, "LOC")) {
			printf("iLOC.info Local (per CPU core) APIC timer interrupt. Until 2.6.21 normally 250 or 1000 per second. On modern 'tickless' kernels it more or less reflects how busy the machine is.\n");
		}
		else {
			/* Don't show any info line */
		}

		printf("i%s.type DERIVE\n"
			"i%s.min 0\n", irqstats[i].name, irqstats[i].name);

		free(irqstats[i].name);
		free(irqstats[i].description);
	}

	return true;
}

bool irqstats_fetch()
{
	irqstat_t irqstats[MAX_IRQS] = {0};
	size_t irq_num = read_interrupts(irqstats, false);

	if (irq_num == 0) {
		fprintf(stderr, "no interrupts found\n");
		return false;
	}

	for (size_t i = 0; i < irq_num; i++) {
		printf("i%s.value %lu\n", irqstats[i].name, irqstats[i].count);

		free(irqstats[i].name);
		free(irqstats[i].description);
	}

	return true;
}

int irqstats(int argc, char **argv)
{
	if (argc == 1) {
		/* fetch (fall through) */
	}
	else if (argc == 2) {
		if (!strcmp(argv[1], "autoconf")) {
			return !irqstats_autoconf();
		}
		else if (!strcmp(argv[1], "config")) {
			return !irqstats_config();
		}
		else if (!strcmp(argv[1], "fetch")) {
			/* fetch (fall through) */
		}
		else {
			fprintf(stderr, "invalid mode '%s'\n", argv[1]);
			return 1;
		}
	}
	else {
		fprintf(stderr, "invalid parameters\n");
		return 1;
	}

	return !irqstats_fetch();
}
