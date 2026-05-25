#ifndef ONETOOL_TOOLS_GUI_BROWSER_PARSE_HTML_H
#define ONETOOL_TOOLS_GUI_BROWSER_PARSE_HTML_H

#include "../browser.h"

/* Allocate, parse `html`/`len` into doc, return 0 on success. */
br_doc_t *br_doc_create(void);
void      br_doc_destroy(br_doc_t *doc);
int       br_doc_parse_html(br_doc_t *doc, const char *html, size_t len);
void      br_doc_clear(br_doc_t *doc);

#endif
