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
#include <signal.h>
#include <errno.h>
#include <ctype.h>

#define INTERRUPTS "/proc/interrupts"
//#define INTERRUPTS "./interrupts"
#define MAX_IRQS 256
// this has been shown to be sufficient even on a system with 256 threads
#define MAX_LINE 4096
#define MAX_TOKENS 32

#define DEBUGGING 0

#define ERROR(...) \
	fprintf(stderr, "ERROR: %s:%d:%s: ", __FILE__, __LINE__, __func__); \
	fprintf(stderr, __VA_ARGS__)

#define DEBUG(...) \
	if (DEBUGGING) { \
		fprintf(stderr, "DEBUG: %s:%d:%s: ", __FILE__, __LINE__, __func__); \
		fprintf(stderr, __VA_ARGS__); \
	}

typedef struct irqstat_t {
	char *name;
	char *description;
	unsigned long hwirq;
	bool has_hwirq; // hwirq can be zero
	unsigned long count;
} irqstat_t;

bool isnumeric(char *string) {
	// NULL, or empty string
	if (!string || !*string)
		return false;

	// check that every character is a digit
	while (*string)
		if (!isdigit(*string++))
			return false;

	return true;
}

bool isempty(char *string) {
	// NULL, or empty string
	if (!string || !*string)
		return true;

	// check that it's not all whitespace
	while (*string)
		if (!isspace(*string++))
			return false;

	return true;
}

bool endswith(char *haystack, char *needle) {
	size_t haystack_length = 0, needle_length = 0;

	if (!haystack || !needle)
		return false;

	haystack_length = strlen(haystack);
	needle_length = strlen(needle);

	DEBUG("needle='%s', needle_length=%zu, haystack='%s', haystack_length=%zu\n", needle, needle_length, haystack, haystack_length)

	if (needle_length > haystack_length)
		return false;

	DEBUG("!strcmp((char *)(%s), %s)\n", (char *)(haystack + haystack_length - needle_length), needle);

	return !strcmp((char *)(haystack + haystack_length - needle_length), needle);
}

size_t read_interrupts(irqstat_t irqstats[], bool config) {
	FILE *interrupts;
	char line_buf[MAX_LINE] = {0};
	size_t irq_num = 0;
	size_t cpu_count = 0; // at least one cpu
	size_t line_num = 0;
	unsigned long hwirq = 0;

	interrupts = fopen(INTERRUPTS, "r");
	if (!interrupts) {
		ERROR("fopen: %m\n");

		return 0;
	}

	while (fgets(line_buf, sizeof(line_buf), interrupts)) {
		char *pos = NULL, *eol = NULL;
		char *tokens[MAX_TOKENS] = {0};
		size_t length = 0, token_num = 0, token_start = 0;

		DEBUG("parsing line_num %zu: '%s'", line_num, line_buf);

		if (irq_num == MAX_IRQS) {
			DEBUG("MAX_IRQS reached, discarding remainder\n");

			break;
		}

		// ensure eol is '\n'
		if ((eol = strchr(line_buf, '\n')) != NULL) {
			*eol = '\0';
		}
		// otherwise we've overrun line_buf (or the last line doesn't have a newline - unlikely)
		else {
			ERROR("line_num=%zu had line_buf overflow\n", line_num);

			return 0;
		}

		pos = strtok(line_buf, " ");
		// there should be no empty lines (i think?)
		if (!pos) {
			ERROR("line_num=%zu is empty\n", line_num);

			return 0;
		}

		DEBUG("line_num=%zu,irq_num=%zu,pos='%s'\n", line_num, irq_num, pos);

		// the first line has a column per cpu. count them.
		if (line_num == 0) {
			while (pos) {
				if (strncmp(pos, "CPU", 3)) {
					ERROR("expected CPU at line_num=%zu, got '%s'\n", line_num, pos);

					return 0;
				}

				cpu_count++;

				DEBUG("found cpu_count=%zu at pos='%s'\n", cpu_count, pos);

				pos = strtok(NULL, " "); // move on to next cpu
			}

			if (!cpu_count) {
				ERROR("no CPUs found\n");

				return 0;
			}

			DEBUG("found %zu cpus\n", cpu_count);

			cpu_count--; // arrays start at 0
			line_num++;

			continue;
		}

		// the rest of the lines should each contain an interrupt name, value(s), and optional description
		if (!strchr(pos, ':')) {
			ERROR("expected name '%s' is missing ':'\n", pos);

			return 0;
		}

		length = strlen(pos);
		irqstats[irq_num].name = malloc(length); // we're discarding the ':'
		if (!irqstats[irq_num].name) {
			ERROR("malloc: %m\n");

			return 0;
		}

		strncpy(irqstats[irq_num].name, pos, --length); // discard ':'
		irqstats[irq_num].name[length] = '\0';

		DEBUG("irqstats[%zu].name = '%s'\n", irq_num, irqstats[irq_num].name);

		// convert each counter value to long and add it to the total for this irq
		for (size_t cpu_num = 0; cpu_num <= cpu_count; cpu_num++) {
			unsigned long count = 0;
			pos = strtok(NULL, " ");
			// some interrupts, such as 'ERR' or 'MIS' will only have a single counter rather than one per CPU
			if (!pos) {
				DEBUG("eol at cpu_num=%zu, expected cpu_count=%zu\n", cpu_num, cpu_count);

				if (!cpu_num) {
					ERROR("irqstats[%zu].name = '%s' has no counters\n", irq_num, irqstats[irq_num].name);

					return 0;
				}

				break;
			}

			// if it's not a positive integer, we may have run into the description unexpectedly
			if (!isnumeric(pos)) {
				DEBUG("non-numeric char at cpu=num=%zu, pos='%s'\n", cpu_num, pos);

				if (!cpu_num) {
					ERROR("irqstats[%zu].name = '%s' has only garbage '%s'\n", irq_num, irqstats[irq_num].name, pos);

					return 0;
				}

				break;
			}

			errno = 0;
			count = strtol(pos, NULL, 10);
			if (!count && errno) {
				ERROR("strol of '%s' failed: %m\n", pos);

				return 0;
			}

			DEBUG("'%s' -> %lu\n", pos, count);

			irqstats[irq_num].count += count;

			DEBUG("cpu_num=%zu irqstats[%zu].count + %lu == %lu\n", cpu_num, irq_num, count, irqstats[irq_num].count);
		}

		DEBUG("irqstats[%zu].count = %lu\n", irq_num, irqstats[irq_num].count);

		// skip over the description parsing unless we're running 'config'
		if (!config)
			goto next_line;

		// if eol wasn't reached when parsing counters, try to grab the irq description
		if (!pos || (pos = (pos + strlen(pos))) == eol || isempty(++pos)) {
			DEBUG("eol reached\n");

			goto next_line;
		}

		DEBUG("pos=%p, eol=%p, difference=%td, length=%zu\n", pos, eol, (eol - pos), strlen(pos));

		// skip over leading whitespace. we know there's something there, per isempty() above
		for(; *pos && isblank(*pos); pos++);

		// null out any trailing whitespace
		for (char *eos = pos + strlen(pos) - 1; *eos && isspace(*eos); *eos-- = '\0');

		DEBUG("trimmed pos='%s'\n", pos);

		// it won't be any longer than this
		irqstats[irq_num].description = malloc(strlen(pos) + 1);
		if (!irqstats[irq_num].description) {
			ERROR("malloc: %m\n");

			return 0;
		}
		*irqstats[irq_num].description = '\0';

		// if it's not a numbered irq, then the description is simply everything that remains
		if (!isnumeric(irqstats[irq_num].name)) {
			strcpy(irqstats[irq_num].description, pos);

			DEBUG("irqstats[%zu].description = '%s'\n", irq_num, irqstats[irq_num].description);

			goto next_line;
		}

		tokens[token_num] = pos = strtok(pos, " ");
		while ((pos = strtok(NULL, " ")) != NULL && token_num < (MAX_TOKENS - 1)) {
			tokens[++token_num] = pos;

			DEBUG("tokens[%zu] = '%s'\n", token_num, tokens[token_num]);
		}

		// if we only have one token, there's nothing left to parse
		if (!token_num) {
			strcpy(irqstats[irq_num].description, pos);

			goto next_line;
		}

#ifdef __sparc__
		// sparc's interrupts layout differs
		if (token_num >= 2) {
			token_start = 2;
			// sparc has been seen to have many duplicate 'MSIQ' interrupts (one per thread). always show the irq number here to differentiate.
			if (!strcmp(tokens[2], "MSIQ")) {
				irqstats[irq_num].hwirq = atol(irqstats[irq_num].name);
				irqstats[irq_num].has_hwirq = true;
			}
#else
		if (token_num >= 2 && isnumeric(tokens[1])) {
			token_start = 2;

			//                                                   [0]        [1][2]       [3-]
			// 38:     150262          0          0          0   OpenPIC    38 Level     i2c-mpc, i2c-mpc
			// Interrupt 38, for device(s): i2c-mpc, i2c-mpc
			//
			//                     [0]   [1] [2-]
			//  3:  247552271      MIPS   3  ehci_hcd:usb1
			// Interrupt 3, for device(s): ehci_hcd:usb1
			//
			//                 [0]            [1][2]       [3-]
			// 33:     617373  f1010140.gpio  17 Edge      pps.-1
			// Interrupt 33, for device(s): pps.-1 [17]
			DEBUG("hwirq: discarding tokens[0] = '%s'\n", tokens[0]);
			DEBUG("hwirq: num is '%s'\n", tokens[1]);

			if ((hwirq = atol(tokens[1])) != atol(irqstats[irq_num].name)) {
				irqstats[irq_num].hwirq = hwirq;
				irqstats[irq_num].has_hwirq = true;

				DEBUG("irqstats[%zu].hwirq = %lu\n", irq_num, irqstats[irq_num].hwirq);
			}

			// MIPS has been to not show the type
			if (!strcmp(tokens[2], "Edge") || !strcmp(tokens[2], "Level") || !strcmp(tokens[2], "None")) {
				DEBUG("hwirq: discarding tokens[2] = '%s'\n", tokens[2]);
				token_start = 3;
			}
#endif
		}
		// most x86 interrupts, old arm
		else {
			token_start = 1;

			// strip away the text component from x86 APIC/PCI interrupts e.g. 18-fasteoi 1048579-edge
			if (isdigit(*tokens[1]) && (endswith(tokens[1], "-fasteoi") || endswith(tokens[1], "-edge"))) {
				token_start = 2;

				DEBUG("apic: probable x86 PCI/APIC irq '%s'\n", tokens[0]);

				if ((hwirq = atol(tokens[1])) != atol(irqstats[irq_num].name)) {
					irqstats[irq_num].hwirq = hwirq;
					irqstats[irq_num].has_hwirq = true;

					DEBUG("irqstats[%zu].hwirq = %lu\n", irq_num, irqstats[irq_num].hwirq);
				}
			}
		}

		DEBUG("hwirq: token_start=%zu, token_num=%zu\n", token_start, token_num);

		// concatenate the needed tokens with a space
		for (*irqstats[irq_num].description = '\0'; token_start <= token_num; token_start++) {
			DEBUG("hwirq: appending tokens[%zu] = '%s'\n", token_start, tokens[token_start]);

			strcat(irqstats[irq_num].description, tokens[token_start]);
			strcat(irqstats[irq_num].description, " ");
		}

		*(irqstats[irq_num].description + strlen(irqstats[irq_num].description) - 1) = '\0';

		DEBUG("irqstats[%zu].description = '%s'\n", irq_num, irqstats[irq_num].description);

next_line:
			line_num++;
			irq_num++;
	}

	fclose(interrupts);

	return irq_num;
}

bool autoconf() {
	if (!access(INTERRUPTS, R_OK)) {
		printf("yes\n");

		return false;
	}
	else {
		printf("no (%s isn't readable: %m)\n", INTERRUPTS);

		return true;
	}
}

bool config() {
	irqstat_t irqstats[MAX_IRQS] = {0};
	size_t irq_num = read_interrupts(irqstats, true);

	if (irq_num == 0) {
		ERROR("no interrupts found\n");

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
		// some, like ERR and MIS, do not have a description
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
		// NOTE: original perl plugin does a case insensitive substring match. '#define _GNU_SOURCE' and strcasestr() could imitate this.
		else if (!strcmp(irqstats[i].name, "NMI")) {
			printf("iNMI.info Non-maskable interrupt. Either 0 or quite high. If it's normally 0 then just one NMI will often mark some hardware failure.\n");
		}
		else if (!strcmp(irqstats[i].name, "LOC")) {
			printf("iLOC.info Local (per CPU core) APIC timer interrupt. Until 2.6.21 normally 250 or 1000 per second. On modern 'tickless' kernels it more or less reflects how busy the machine is.\n");
		}
		else {
			// don't show any info line
		}

		printf("i%s.type DERIVE\n"
			"i%s.min 0\n", irqstats[i].name, irqstats[i].name);

		free(irqstats[i].name);
		free(irqstats[i].description);
	}

	return true;
}

bool fetch() {
	irqstat_t irqstats[MAX_IRQS] = {0};
	size_t irq_num = read_interrupts(irqstats, false);

	if (irq_num == 0) {
		ERROR("no interrupts found\n");

		return false;
	}

	for (size_t i = 0; i < irq_num; i++) {
		printf("i%s.value %lu\n", irqstats[i].name, irqstats[i].count);

		free(irqstats[i].name);
		free(irqstats[i].description);
	}

	return true;
}

int main (int argc, char *argv[]) {
	if (argc == 1) {
		// fetch (fall through)
	}
	else if (argc == 2) {
		if (!strcmp(argv[1], "autoconf")) {
			return !autoconf();
		}
		else if (!strcmp(argv[1], "config")) {
			return !config();
		}
		else if (!strcmp(argv[1], "fetch")) {
			// fetch (fall through)
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

	return !fetch();
}
