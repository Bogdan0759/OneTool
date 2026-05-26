/*
 * CSS engine — public API.
 *
 * The CSS engine processes a single textual stylesheet (typically the
 * accumulated <style> blocks + any external link sheets) into an
 * intermediate structure (br_stylesheet_t) that can then be applied to
 * a parsed br_doc_t. Applying writes a `br_computed_style_t` into each
 * element of the document.
 *
 * Responsibilities live in sibling files:
 *   - tokens.[ch]      : low-level tokenizer
 *   - value.[ch]       : value parsing (colours, lengths, keywords)
 *   - selector.[ch]    : selectors + matching
 *   - properties.[ch]  : property table + inheritance + initial values
 *   - parser.[ch]      : token-stream -> stylesheet
 *   - cascade.[ch]     : stylesheet -> br_computed_style_t per element
 */
#ifndef ONETOOL_TOOLS_GUI_BROWSER_PARSE_CSS_H
#define ONETOOL_TOOLS_GUI_BROWSER_PARSE_CSS_H

#include "../../browser.h"

typedef struct br_stylesheet br_stylesheet_t;

/* Allocate an empty stylesheet. NULL on OOM. */
br_stylesheet_t *br_css_stylesheet_create(void);

/* Free the stylesheet and all owned data. */
void br_css_stylesheet_destroy(br_stylesheet_t *ss);

/* Append rules from `css`/`len` into `ss`. Returns 0 on success. Parse
 * errors are non-fatal — malformed rules are skipped. */
int br_css_parse_into(br_stylesheet_t *ss, const char *css, size_t len);

/* Apply the user-agent stylesheet + the page stylesheet to the document.
 * Walks doc->elements in source order, computes a br_computed_style_t for
 * each by collecting matching declarations and applying the cascade
 * (specificity, !important, order) + inheritance from parent.
 * inline style="..." is parsed on demand per element as the highest-
 * specificity author stylesheet.
 *
 * Pass NULL for `ss` to apply only the user-agent stylesheet. */
void br_css_apply_to_doc(br_doc_t *doc, br_stylesheet_t *ss);

/* Lookup the computed style for the element a run belongs to. Returns
 * NULL for runs without an element index. */
const br_computed_style_t *br_css_run_style(const br_doc_t *doc,
                                            const br_run_t *run);

#endif
