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

#define PROC_INTERRUPTS "/proc/interrupts"
/* Stop processing after this many IRQs have been seen */
#define MAX_IRQS 256
/* Sufficient even on a system with 256 threads */
#define MAX_LINE 4096
#define MAX_TOKENS 16

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
		if (!xisdigit(*string++))
			return false;

	return true;
}

bool isempty(char *string)
{
	if (!string || !*string)
		return true;

	/* Check that it's not all whitespace */
	while (*string)
		if (!xisspace(*string++))
			return false;

	return true;
}

bool endswith(char *haystack, char *needle)
{
	size_t haystack_length, needle_length;

	if (!haystack || !needle)
		return false;

	haystack_length = strlen(haystack);
	needle_length = strlen(needle);

	if (needle_length > haystack_length)
		return false;

	return !strcmp((char *)(haystack + haystack_length - needle_length), needle);
}

size_t read_interrupts(irqstat_t irqs[], bool config)
{
	FILE *interrupts;
	char line_buf[MAX_LINE] = {0};
	size_t irq_num = 0, cpu_num = 0, line_num = 0;

	interrupts = fopen(PROC_INTERRUPTS, "r");
	if (!interrupts) {
		perror("fopen");
		return 0;
	}

	while (fgets(line_buf, sizeof(line_buf), interrupts)) {
		char *pos, *eol;
		char *tokens[MAX_TOKENS + 1];
		size_t token_num = 0, token_start;
		irqstat_t *irq = &irqs[irq_num];

		/* Ensure EOL is '\n' */
		if ((eol = strchr(line_buf, '\n')) != NULL) {
			*eol = '\0';
		/* Otherwise we've overrun line_buf (or the last line doesn't have a newline - unlikely) */
		} else {
			fprintf(stderr, "line %zu had overflow\n", line_num);
			return 0;
		}

		pos = strtok(line_buf, " ");
		/* There should be no empty lines */
		if (!pos) {
			fprintf(stderr, "line %zu is empty\n", line_num);
			return 0;
		}

		/* The first line has a column per CPU - count them */
		if (line_num == 0) {
			while (pos) {
				if (strncmp(pos, "CPU", 3)) {
					fprintf(stderr, "expected CPU at line 0, got '%s'\n", pos);
					return 0;
				}

				cpu_num++;

				pos = strtok(NULL, " "); /* Move on to next CPU */
			}

			if (!cpu_num) {
				fprintf(stderr, "no CPUs found\n");
				return 0;
			}

			cpu_num--; /* CPU0 -> 0 */
			line_num++;

			continue;
		}

#if defined(__aarch64__) || defined(__arm__)
		/* Some ARM devices, such as Raspberry Pi, have a line beginning with 'FIQ:' that contains only a list of device names */
		if (!strcmp(pos, "FIQ:"))
			continue;
#endif

		{
			size_t length = strlen(pos);

			/* The rest of the lines should each contain an interrupt name, value(s), and optional description */
			if (length < 2 || strchr(pos, ':') != (pos + length - 1)) {
				fprintf(stderr, "irq '%s' is invalid\n", pos);
				return 0;
			}

			irq->name = malloc(length); /* We're discarding the ':' */
			if (!irq->name) {
				perror("malloc");
				return 0;
			}

			strncpy(irq->name, pos, --length); /* Discard ':' */
			irq->name[length] = '\0';
		}

		/* Convert each counter value to unsigned long and add it to the total for this IRQ */
		for (size_t c = 0; c <= cpu_num; c++) {
			pos = strtok(NULL, " ");
			/* Some interrupts, such as 'ERR' or 'MIS' will only have a single counter rather than one per CPU */
			if (!pos) {
				if (!c) {
					fprintf(stderr, "irq '%s' has no counters\n", irq->name);
					return 0;
				}

				break;
			}

			/* If it's not a positive integer, we may have run into the description unexpectedly */
			if (!isnumeric(pos)) {
				char *eos;

				if (!c) {
					fprintf(stderr, "irq '%s' has garbage '%s'\n", irq->name, pos);
					return 0;
				}

				/* Backtrack */
				if ((eos = pos + strlen(pos)) < eol)
					*eos = ' ';
				while (*--pos);

				break;
			}

			irq->count += strtoul(pos, NULL, 10);
		}

		/* Skip over the description parsing unless we're running 'config' */
		if (!config)
			goto next_line;

		/* If EOL wasn't reached when parsing counters, try to grab the IRQ description */
		if (!pos || (pos = (pos + strlen(pos))) == eol || isempty(++pos))
			goto next_line;

		/* Skip over leading whitespace. We know there's something there, per isempty() above */
		for (; *pos && xisblank(*pos); pos++);

		/* NULL out any trailing whitespace */
		for (char *eos = pos + strlen(pos) - 1; *eos && xisspace(*eos); *eos-- = '\0');

		/* It won't be any longer than this */
		irq->description = malloc(strlen(pos) + 1);
		if (!irq->description) {
			perror("malloc");
			return 0;
		}

		/* If it's not a numbered IRQ, then the description is simply everything that remains */
		if (!isnumeric(irq->name)) {
			strcpy(irq->description, pos);
			goto next_line;
		}

		/* Split the non-counter portion of the line into MAX_TOKENS words */
		*tokens = strtok(pos, " ");
		while (token_num < (MAX_TOKENS - 1) && (pos = strtok(NULL, " ")) != NULL)
			tokens[++token_num] = pos;

		/* If we only have one token, there's nothing left to parse */
		if (!token_num) {
			strcpy(irq->description, *tokens);
			goto next_line;
		}

		/* Add a pointer to the remainder of the string, if it exists */
		if (pos && (pos + strlen(pos)) != eol)
			tokens[++token_num] = pos = pos + strlen(pos) + 1;

#if defined(__sparc__)
		/* SPARC's interrupts layout differs
		 *
		 * sun4u:
		 *                    [0]      [1]        [2-]
		 *  1:          0     sun4u    -IVEC      SCHIZO_PCIERR
		 * Interrupt 1, for device(s): SCHIZO_PCIERR [1]
		 *
		 * sun4u:
		 *                     [0]    [1-]
		 *  8:        191      sun4u  pata_cmd64x
		 * Interrupt 8, for device(s): pata_cmd64x
		 *
		 * sun4v:
		 *                     [0]      [1-]
		 * 42:          0      sun4v    MSIQ
		 * Interrupt 42, for device(s): MSIQ [42]
		 *
		 * vsun4v:
		 *                              [0]        [1]        [2-]
		 * 16:          0          0    vsun4v     -IVEC      MSIQ
		 * Interrupt 16, for device(s): MSIQ [16]
		 */
		token_start = (token_num >= 2 && *tokens[1] == '-') ? 2 : 1;

		/* (v)sun4v has been seen to have many duplicate 'MSIQ' interrupts (one per thread), so always show the IRQ number here to differentiate
		 * Additionally, there are ambiguous descriptions beginning with 'SCHIZO_' and 'PSYCHO_' on older (i.e. sun4u) machines
		 */
		if (!strcmp(tokens[token_start], "MSIQ") ||
			!strncmp(tokens[token_start], "SCHIZO_", sizeof("SCHIZO_") - 1) ||
			!strncmp(tokens[token_start], "PSYCHO_", sizeof("PSYCHO_") - 1)) {
			irq->hwirq = strtoul(irq->name, NULL, 10);
			irq->has_hwirq = true;
		}
#else
		/* Newer ARM, MIPS, some x86, etc. */
		if (token_num >= 2 && isnumeric(tokens[1])) {
			token_start = 2;

			/* PowerPC:
			 *                                                   [0]        [1][2]       [3-]
			 * 38:     150262          0          0          0   OpenPIC    38 Level     i2c-mpc, i2c-mpc
			 * Interrupt 38, for device(s): i2c-mpc, i2c-mpc
			 *
			 * ARM:
			 *                 [0]            [1][2]       [3-]
			 * 33:     617373  f1010140.gpio  17 Edge      pps.-1
			 * Interrupt 33, for device(s): pps.-1 [17]
			 */
			if ((irq->hwirq = strtoul(tokens[1], NULL, 10)) != strtoul(irq->name, NULL, 10))
				irq->has_hwirq = true;

			/* MIPS has been seen to not show the type
			 *
			 * MIPS:
			 *                     [0]    [1][2-]
			 * 10:        122      MISC   3  ttyS0
			 * Interrupt 10, for device(s): ttyS0 [3]
			 */
			if (!strcmp(tokens[2], "Edge") || !strcmp(tokens[2], "Level") || !strcmp(tokens[2], "None"))
				token_start = 3;

		/* Most x86 interrupts, old ARM
		 *
		 * ARM:
		 *                   [0]       [1-]
		 * 64:         21    MXC_GPIO  baby_buttons
		 * Interrupt 64, for device(s): baby_buttons
		 */
		} else {
			token_start = 1;

# if defined(__x86_64__) || defined(__i386__)
			/* Strip away the text component from x86 APIC/PCI interrupts e.g. 18-fasteoi or 1048579-edge
			 *
			 * x86:
			 *                                                   [0]     [1]              [2-]
			 * 30:          0   21780097          0          0   PCI-MSI 512000-edge      ahci[0000:00:1f.2]
			 * Interrupt 30, for device(s): ahci[0000:00:1f.2] [512000]
			 */
			if (xisdigit(*tokens[1]) && (endswith(tokens[1], "-fasteoi") || endswith(tokens[1], "-edge"))) {
				token_start = 2;

				if ((irq->hwirq = strtoul(tokens[1], NULL, 10)) != strtoul(irq->name, NULL, 10))
					irq->has_hwirq = true;
			}
# endif
		}
#endif

		/* Concatenate the needed tokens with a space */
		for (*irq->description = '\0'; token_start <= token_num; token_start++) {
			strcat(irq->description, tokens[token_start]);
			strcat(irq->description, " ");
		}
		*(irq->description + strlen(irq->description) - 1) = '\0';

next_line:
		line_num++;
		irq_num++;

		if (irq_num == MAX_IRQS)
			break;
	}

	fclose(interrupts);

	return irq_num;
}

bool irqstats_config()
{
	irqstat_t irqs[MAX_IRQS] = {0};
	size_t irq_num = read_interrupts(irqs, true);

	if (irq_num == 0) {
		fprintf(stderr, "no irqs found\n");
		return false;
	}

	/* Text taken from munin.git/plugins/node.d.linux/irqstats.in */
	printf("graph_title Individual interrupts\n"
		"graph_args --base 1000 --logarithmic\n"
		"graph_vlabel interrupts / ${graph_period}\n"
		"graph_category system\n"
		"graph_info Shows the number of different IRQs received by the kernel. High disk or network traffic can cause a high number of interrupts (with good hardware and drivers this will be less so). Sudden high interrupt activity with no associated higher system activity is not normal.\n\n");

	printf("graph_order");
	for (size_t i = 0; i < irq_num; i++)
		printf(" i%s", irqs[i].name);
	printf("\n");

	for (size_t i = 0; i < irq_num; i++) {
		/* Some, like ERR and MIS, do not have a description */
		printf("i%s.label %s", irqs[i].name, (irqs[i].description ? irqs[i].description : irqs[i].name));
		if (irqs[i].has_hwirq)
			printf(" [%lu]", irqs[i].hwirq);
		printf("\n");

		if (irqs[i].description) {
			printf("i%s.info Interrupt %s, for device(s): %s", irqs[i].name, irqs[i].name, irqs[i].description);
			if (irqs[i].has_hwirq)
				printf(" [%lu]", irqs[i].hwirq);
			printf("\n");
		/* NOTE: The original Perl plugin does a case insensitive substring match. '#define _GNU_SOURCE' and strcasestr() could imitate this */
		} else if (!strcmp(irqs[i].name, "NMI")) {
			printf("iNMI.info Non-maskable interrupt. Either 0 or quite high. If it's normally 0 then just one NMI will often mark some hardware failure.\n");
		} else if (!strcmp(irqs[i].name, "LOC")) {
			printf("iLOC.info Local (per CPU core) APIC timer interrupt. Until 2.6.21 normally 250 or 1000 per second. On modern 'tickless' kernels it more or less reflects how busy the machine is.\n");
		} else {
			/* Don't show any info line */
		}

		printf("i%s.type DERIVE\n"
			"i%s.min 0\n", irqs[i].name, irqs[i].name);

		free(irqs[i].name);
		free(irqs[i].description);
	}

	return true;
}

bool irqstats_fetch()
{
	irqstat_t irqs[MAX_IRQS] = {0};
	size_t irq_num = read_interrupts(irqs, false);

	if (irq_num == 0) {
		fprintf(stderr, "no irqs found\n");
		return false;
	}

	for (size_t i = 0; i < irq_num; i++) {
		printf("i%s.value %lu\n", irqs[i].name, irqs[i].count);

		free(irqs[i].name);
		free(irqs[i].description);
	}

	return true;
}

int irqstats(int argc, char **argv)
{
	if (argc == 1) {
		/* fetch (fall through) */
	} else if (argc == 2) {
		if (!strcmp(argv[1], "autoconf")) {
			return autoconf_check_readable(PROC_INTERRUPTS);
		} else if (!strcmp(argv[1], "config")) {
			return !irqstats_config();
		} else if (!strcmp(argv[1], "fetch")) {
			/* fetch (fall through) */
		} else {
			fprintf(stderr, "invalid mode '%s'\n", argv[1]);
			return 1;
		}
	} else {
		fprintf(stderr, "invalid parameters\n");
		return 1;
	}

	return !irqstats_fetch();
}
