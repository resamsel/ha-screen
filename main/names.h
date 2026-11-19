/* Shortening row names that don't fit. */
#pragma once

/* If any of the n names is wider than max_w (per width()), drop the whole words that all
 * names share at the start and at the end ("Temperatur OG/EG/UG" -> "OG/EG/UG"). Names are
 * left alone when that would empty one of them or make them all equal. Anything still too
 * wide is left for the label's own ellipsis. */
void names_abbreviate(char names[][32], int n, int max_w, int (*width)(const char *s, void *ctx), void *ctx);
