/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * neuron.h — The frozen orchestrator-to-neuron wire formats
 * (SPEC.md section 2).
 *
 * These are NOT free choices: they are the formats of
 * neuron-translator itself.  A topology line is parsed by its
 * parse_config_string (comma separated integers, >= 2 layers), an
 * input line by its parse_input_string (comma separated floats,
 * the forward pass only runs on a complete vector), and the
 * outputs are read from the state text the instance serves
 * ("[<i>]: <value>" lines).
 */

#ifndef ORC_NEURON_H
#define ORC_NEURON_H

#include <stddef.h>

/* Render the input vector as one neuron command line:
 * "0.5,0.3,..." (fits the 1023 byte command limit of the
 * instance: ORC_MAX_INPUT is bounded for exactly that). */
size_t orc_neuron_input_line (const double *v, int n,
                              char *buf, size_t cap);

/* Extract the output vector from the state text of an instance.
 * Fills OUT[0..n) from every "[<i>]: <value>" line, missing
 * components stay 0.0.  Returns the number of components (the
 * highest index seen + 1), or -1 when no output line was found. */
int orc_neuron_parse_outputs (const char *text, size_t text_len,
                              double *out, int max);

#endif /* ORC_NEURON_H */
