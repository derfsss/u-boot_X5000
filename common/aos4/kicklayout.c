// SPDX-License-Identifier: GPL-2.0+
/*
 * Kicklayout parser, for boota (AmigaOne X5000, A1222).
 *
 * The format, as each Kickstart/Kicklayout describes it: a file lists one
 * or more configurations. Each starts with LABEL <name>, has exactly one
 * EXEC <file> (the kernel loader) and any number of MODULE <file> lines.
 * An empty line ends a configuration. Lines starting with ';' are
 * comments. The rest of a line after the keyword is the file name.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include "aos4.h"

/* Return the text after keyword kw (case-insensitive), or NULL. */
static char *keyword(char *line, const char *kw)
{
	size_t n = strlen(kw);

	if (strncasecmp(line, kw, n) || (line[n] != ' ' && line[n] != '\t'))
		return NULL;
	line += n;
	while (*line == ' ' || *line == '\t')
		line++;
	return *line ? line : NULL;
}

/*
 * Parse text (NUL-terminated, modified in place) into at most max
 * configurations. Returns how many were found; errors are reported and
 * the offending configuration is dropped.
 */
int aos4_parse_kicklayout(char *text, struct aos4_config *cfgs, int max)
{
	struct aos4_config *cur = NULL;
	int n = 0, lineno = 0;
	char *line, *next, *arg;

	for (line = text; line && *line; line = next) {
		char *end;

		lineno++;
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		end = line + strlen(line);
		while (end > line && isspace(end[-1]))
			*--end = '\0';
		while (*line == ' ' || *line == '\t')
			line++;

		if (!*line) {			/* empty line ends a config */
			cur = NULL;
			continue;
		}
		if (*line == ';')
			continue;

		if ((arg = keyword(line, "LABEL"))) {
			if (n == max) {
				printf("Kicklayout: more than %d configurations, ignoring the rest\n",
				       max);
				break;
			}
			cur = &cfgs[n++];
			memset(cur, 0, sizeof(*cur));
			cur->label = arg;
		} else if ((arg = keyword(line, "EXEC"))) {
			if (!cur) {
				printf("Kicklayout line %d: EXEC outside a configuration\n",
				       lineno);
			} else if (cur->exec) {
				printf("Kicklayout line %d: second EXEC in \"%s\"\n",
				       lineno, cur->label);
			} else {
				cur->exec = arg;
			}
		} else if ((arg = keyword(line, "MODULE"))) {
			if (!cur) {
				printf("Kicklayout line %d: MODULE outside a configuration\n",
				       lineno);
			} else if (cur->nmods == AOS4_MAX_MODULES) {
				printf("Kicklayout line %d: more than %d modules\n",
				       lineno, AOS4_MAX_MODULES);
			} else {
				cur->mods[cur->nmods++] = arg;
			}
		} else {
			printf("Kicklayout line %d: unknown line \"%s\"\n",
			       lineno, line);
		}
	}

	/* A configuration without its kernel loader cannot boot. */
	for (int i = 0; i < n; i++) {
		if (cfgs[i].exec)
			continue;
		printf("Kicklayout: \"%s\" has no EXEC line, skipped\n",
		       cfgs[i].label);
		memmove(&cfgs[i], &cfgs[i + 1], (n - i - 1) * sizeof(*cfgs));
		n--;
		i--;
	}
	return n;
}
