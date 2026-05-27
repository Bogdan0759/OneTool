/*
 * Built-in user-agent stylesheet.
 *
 * Provides reasonable defaults for raw HTML before author CSS lands. We
 * keep this small — the runs already have a `br_style_t` enum that the
 * paint module uses as a default look. The UA sheet exists so author
 * CSS that overrides "a { color: red }" works without us also having to
 * paint links with a hardcoded blue.
 *
 * Note: we intentionally don't set most defaults here (colours for
 * headings, code background, etc.) — the renderer already handles those
 * via br_style_t. The UA sheet only contributes things author CSS
 * commonly overrides.
 */
#include "internal.h"

#include <string.h>

static const char kUserAgentCss[] =
    /* Document defaults. */
    "html, body { color: #18181c; background-color: #f5f5f5; }"
    "body { max-width: 980px; }"
    "body, p, div, section, article, header, footer, main, aside, nav,"
    "li, ul, ol, dl, dt, dd, table, tr, td, th, form, blockquote, "
    "figure, figcaption, address, fieldset, details, summary { "
    "  color: inherit; "
    "}"
    /* Links. */
    "a { color: #1e5ac8; text-decoration: underline; }"
    /* Headings. */
    "h1 { font-size: x-large; font-weight: bold; color: #0c1220; }"
    "h2 { font-size: x-large; font-weight: bold; color: #0c1220; }"
    "h3, h4, h5, h6 { font-weight: bold; color: #1c202c; }"
    /* Emphasis. */
    "b, strong { font-weight: bold; }"
    "i, em, cite, var, dfn { font-style: italic; }"
    /* Code. */
    "code, tt, kbd, samp, pre { font-family: monospace; color: #1e1450; "
    "  background-color: #e6e6dc; }"
    /* Reading-mode cleanup for wiki-like pages. */
    ".navbox, .vertical-navbox, .toc, .toclimit-1, .metadata, .mw-editsection,"
    ".reference, .reflist, .portal, .sidebar, .sistersitebox, .thumbcaption:empty,"
    ".noprint, .mw-jump-link, .hatnote.navigation-not-searchable { display: none; }"
    ".mw-parser-output .shortdescription, .authority-control, .plainlinks,"
    ".ambox, .ombox, .tmbox, .cmbox, .fmbox, .navbox-styles { display: none; }"
    "table.infobox, table.sidebar { background-color: #eef3f8; }"
    /* Hidden by default. */
    "head, script, style, title, meta, link, [hidden] { display: none; }";

void br_css_apply_user_agent(br_stylesheet_t *ss) {
    if (ss == NULL) return;
    br_css_parse_into(ss, kUserAgentCss, sizeof(kUserAgentCss) - 1);
}
