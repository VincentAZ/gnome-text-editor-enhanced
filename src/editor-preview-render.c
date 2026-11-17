/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "editor-preview-render.h"
#include <gio/gio.h>
#include <cmark-gfm.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm-core-extensions.h>
#include <string.h>
#include <stdlib.h>

/* Keep original JSON lexemes: never round numbers, drop duplicate keys, or
 * normalize string escapes. The formatter only changes insignificant space. */
typedef struct _Value {
  char type;
  char *raw;
  char *key;
  GPtrArray *children;
} Value;

typedef struct {
  const char *start;
  const char *p;
  guint nodes;
  guint base_line;
  GError **error;
} Reader;

static void
value_free (gpointer data)
{
  Value *value = data;
  g_free (value->raw);
  g_free (value->key);
  g_clear_pointer (&value->children, g_ptr_array_unref);
  g_free (value);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Value, value_free)

static gboolean
fail (Reader *r, const char *message)
{
  guint line = r->base_line, column = 1;
  if (r->error && *r->error)
    return FALSE;
  for (const char *p = r->start; p < r->p; p++)
    if (*p == '\n') { line++; column = 1; } else column++;
  g_set_error (r->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "Line %u, column %u: %s", line, column, message);
  return FALSE;
}

static void
space (Reader *r)
{
  while (*r->p == ' ' || *r->p == '\t' || *r->p == '\r' || *r->p == '\n')
    r->p++;
}

static gboolean
hex4 (Reader *r, guint *code)
{
  *code = 0;
  for (guint i = 0; i < 4; i++)
    {
      int digit = g_ascii_xdigit_value (*r->p);
      if (digit < 0)
        return fail (r, "Expected four hexadecimal digits after \\u");
      *code = (*code << 4) | digit;
      r->p++;
    }
  return TRUE;
}

static char *
string_token (Reader *r)
{
  const char *start = r->p++;
  while (*r->p && *r->p != '"')
    {
      if ((guchar)*r->p < 0x20)
        { fail (r, "Unescaped control character in string"); return NULL; }
      if (*r->p++ == '\\')
        {
          char escape = *r->p;
          if (!escape)
            { fail (r, "Unfinished string escape"); return NULL; }
          r->p++;
          if (escape == 'u')
            {
              guint code, low;
              if (!hex4 (r, &code)) return NULL;
              if (code >= 0xd800 && code <= 0xdbff)
                {
                  if (r->p[0] != '\\' || r->p[1] != 'u')
                    { fail (r, "High surrogate must be followed by a low surrogate"); return NULL; }
                  r->p += 2;
                  if (!hex4 (r, &low)) return NULL;
                  if (low < 0xdc00 || low > 0xdfff)
                    { fail (r, "Invalid low surrogate"); return NULL; }
                }
              else if (code >= 0xdc00 && code <= 0xdfff)
                { fail (r, "Unpaired low surrogate"); return NULL; }
            }
          else if (!strchr ("\"\\/bfnrt", escape))
            { fail (r, "Invalid string escape"); return NULL; }
        }
    }
  if (*r->p != '"')
    { fail (r, "Unterminated string"); return NULL; }
  r->p++;
  return g_strndup (start, r->p - start);
}

static Value *parse_value (Reader *r, guint depth);

static Value *
parse_value (Reader *r, guint depth)
{
  g_autoptr(Value) value = g_new0 (Value, 1);
  const char *start;
  space (r);
  if (depth > 128 || ++r->nodes > 20000)
    { fail (r, "Preview limit reached (128 nesting levels or 20,000 values)"); return NULL; }
  value->type = *r->p;
  start = r->p;
  if (*r->p == '{' || *r->p == '[')
    {
      char end = *r->p++ == '{' ? '}' : ']';
      value->children = g_ptr_array_new_with_free_func (value_free);
      space (r);
      if (*r->p != end)
        while (TRUE)
          {
            g_autofree char *key = NULL;
            Value *child;
            if (value->type == '{')
              {
                if (*r->p != '"')
                  { fail (r, "Expected a quoted object key"); return NULL; }
                key = string_token (r);
                if (!key) return NULL;
                space (r);
                if (*r->p != ':')
                  { fail (r, "Expected ':' after object key"); return NULL; }
                r->p++;
              }
            child = parse_value (r, depth + 1);
            if (!child) return NULL;
            child->key = g_steal_pointer (&key);
            g_ptr_array_add (value->children, child);
            space (r);
            if (*r->p == end) break;
            if (*r->p != ',')
              { fail (r, "Expected ',' or closing bracket"); return NULL; }
            r->p++;
            space (r);
          }
      r->p++;
    }
  else if (*r->p == '"')
    {
      value->raw = string_token (r);
      if (!value->raw) return NULL;
    }
  else if (*r->p == '-' || g_ascii_isdigit (*r->p))
    {
      if (*r->p == '-') r->p++;
      if (*r->p == '0') r->p++;
      else if (*r->p >= '1' && *r->p <= '9')
        { do r->p++; while (g_ascii_isdigit (*r->p)); }
      else { fail (r, "Expected a digit"); return NULL; }
      if (*r->p == '.')
        {
          r->p++;
          if (!g_ascii_isdigit (*r->p))
            { fail (r, "Expected a digit after decimal point"); return NULL; }
          do r->p++; while (g_ascii_isdigit (*r->p));
        }
      if (*r->p == 'e' || *r->p == 'E')
        {
          r->p++;
          if (*r->p == '+' || *r->p == '-') r->p++;
          if (!g_ascii_isdigit (*r->p))
            { fail (r, "Expected an exponent digit"); return NULL; }
          do r->p++; while (g_ascii_isdigit (*r->p));
        }
      value->raw = g_strndup (start, r->p - start);
      value->type = 'n';
    }
  else
    {
      const char *word = NULL;
      if (g_str_has_prefix (r->p, "true")) word = "true";
      else if (g_str_has_prefix (r->p, "false")) word = "false";
      else if (g_str_has_prefix (r->p, "null")) word = "null";
      if (!word)
        { fail (r, "Expected a JSON value"); return NULL; }
      r->p += strlen (word);
      value->raw = g_strdup (word);
    }
  return g_steal_pointer (&value);
}

static Value *
parse (const char *text, guint line, GError **error)
{
  Reader r = { text, text, 0, line, error };
  g_autoptr(Value) value = parse_value (&r, 0);
  if (!value) return NULL;
  space (&r);
  if (*r.p)
    { fail (&r, "Unexpected content after JSON value"); return NULL; }
  return g_steal_pointer (&value);
}

static gboolean
check_input (const char *text, GError **error)
{
  if (!g_utf8_validate (text, -1, NULL))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Text is not valid UTF-8");
      return FALSE;
    }
  return TRUE;
}

EditorPreviewKind
editor_preview_detect (const char *filename, const char *language)
{
  g_autofree char *name = g_ascii_strdown (filename ? filename : "", -1);
  if (g_str_has_suffix (name, ".jsonl") || g_str_has_suffix (name, ".ndjson"))
    return EDITOR_PREVIEW_JSONL;
  if (g_str_has_suffix (name, ".json")) return EDITOR_PREVIEW_JSON;
  if (g_str_has_suffix (name, ".md") || g_str_has_suffix (name, ".markdown") ||
      g_str_has_suffix (name, ".mdown") || g_str_has_suffix (name, ".mkd"))
    return EDITOR_PREVIEW_MARKDOWN;
  if (g_str_has_suffix (name, ".html") || g_str_has_suffix (name, ".htm"))
    return EDITOR_PREVIEW_HTML;
  if (g_strcmp0 (language, "html") == 0) return EDITOR_PREVIEW_HTML;
  if (g_strcmp0 (language, "markdown") == 0 || g_strcmp0 (language, "commonmark") == 0)
    return EDITOR_PREVIEW_MARKDOWN;
  if (g_strcmp0 (language, "json") == 0) return EDITOR_PREVIEW_JSON;
  return EDITOR_PREVIEW_NONE;
}

static char *
number_token (Reader *r)
{
  const char *start = r->p;
      if (*r->p == '-') r->p++;
      if (*r->p == '0') r->p++;
      else if (*r->p >= '1' && *r->p <= '9')
        { do r->p++; while (g_ascii_isdigit (*r->p)); }
      else { fail (r, "Expected a digit"); return NULL; }
      if (*r->p == '.')
        {
          r->p++;
          if (!g_ascii_isdigit (*r->p))
            { fail (r, "Expected a digit after decimal point"); return NULL; }
          do r->p++; while (g_ascii_isdigit (*r->p));
        }
      if (*r->p == 'e' || *r->p == 'E')
        {
          r->p++;
          if (*r->p == '+' || *r->p == '-') r->p++;
          if (!g_ascii_isdigit (*r->p))
            { fail (r, "Expected an exponent digit"); return NULL; }
          do r->p++; while (g_ascii_isdigit (*r->p));
        }
  return g_strndup (start, r->p - start);
}

/* Iterative, token-preserving formatting: no whole-file byte/node/depth cap.
 * Only stack frames and the output are retained, not a second JSON value tree. */
static void
indent (GString *out, guint depth)
{
  for (guint i = 0; i < depth; i++) g_string_append (out, "  ");
}

typedef enum { A_FIRST, A_VALUE, A_AFTER, O_FIRST, O_KEY, O_COLON, O_VALUE, O_AFTER } FormatState;
typedef struct { FormatState state; } FormatFrame;

char *
editor_preview_format_json (const char *text, GError **error)
{
  Reader reader = { text, text, 0, 1, error };
  Reader *rr = &reader;
  g_autoptr(GArray) stack = g_array_new (FALSE, FALSE, sizeof (FormatFrame));
  g_autoptr(GString) out = g_string_new (NULL);
  gboolean root_done = FALSE;
  if (!check_input (text, error)) return NULL;
  for (;;)
    {
      FormatFrame *frame = stack->len ? &g_array_index (stack, FormatFrame, stack->len - 1) : NULL;
      space (rr);
      if (!frame && root_done)
        {
          if (*rr->p) { fail (rr, "Unexpected content after JSON value"); return NULL; }
          if (g_str_has_suffix (text, "\n")) g_string_append_c (out, '\n');
          return g_string_free (g_steal_pointer (&out), FALSE);
        }
      if (frame && (frame->state == A_AFTER || frame->state == O_AFTER))
        {
          char closing = frame->state == A_AFTER ? ']' : '}';
          if (*rr->p == closing)
            {
              rr->p++; g_array_set_size (stack, stack->len - 1);
              g_string_append_c (out, '\n'); indent (out, stack->len); g_string_append_c (out, closing);
              continue;
            }
          if (*rr->p != ',') { fail (rr, "Expected comma or closing bracket"); return NULL; }
          rr->p++; g_string_append (out, ",\n");
          frame->state = frame->state == A_AFTER ? A_VALUE : O_KEY;
          continue;
        }
      if (frame && frame->state == O_COLON)
        {
          if (*rr->p != ':') { fail (rr, "Expected colon after object key"); return NULL; }
          rr->p++; g_string_append (out, ": "); frame->state = O_VALUE; continue;
        }
      if (frame && (frame->state == O_FIRST || frame->state == O_KEY))
        {
          g_autofree char *key = NULL;
          if (*rr->p != '"') { fail (rr, "Object keys must be quoted strings"); return NULL; }
          if (!(key = string_token (rr))) return NULL;
          indent (out, stack->len); g_string_append (out, key); frame->state = O_COLON; continue;
        }
      if (frame && (frame->state == A_FIRST || frame->state == A_VALUE)) indent (out, stack->len);
      /* Consuming a value completes its parent slot before a nested frame is added. */
      if (frame) frame->state = frame->state == O_VALUE ? O_AFTER : A_AFTER;
      else root_done = TRUE;
      if (*rr->p == '[' || *rr->p == '{')
        {
          char opening = *rr->p++, closing = opening == '[' ? ']' : '}';
          FormatFrame next = { opening == '[' ? A_FIRST : O_FIRST };
          g_string_append_c (out, opening); space (rr);
          if (*rr->p == closing) { rr->p++; g_string_append_c (out, closing); continue; }
          g_string_append_c (out, '\n'); g_array_append_val (stack, next); continue;
        }
      if (*rr->p == '"')
        {
          g_autofree char *value = string_token (rr);
          if (!value) return NULL;
          g_string_append (out, value);
        }
      else if (*rr->p == '-' || g_ascii_isdigit (*rr->p))
        {
          g_autofree char *value = number_token (rr);
          if (!value) return NULL;
          g_string_append (out, value);
        }
      else
        {
          const char *literal = g_str_has_prefix (rr->p, "true") ? "true" :
                                g_str_has_prefix (rr->p, "false") ? "false" :
                                g_str_has_prefix (rr->p, "null") ? "null" : NULL;
          if (!literal) { fail (rr, "Expected a JSON value"); return NULL; }
          g_string_append (out, literal); rr->p += strlen (literal);
        }
    }
}

static void
html_value (Value *v, GString *out, guint depth)
{
  g_autofree char *key = v->key ? g_markup_escape_text (v->key, -1) : NULL;
  if (v->children)
    {
      g_string_append_printf (out, "<details%s><summary>", depth < 2 ? " open" : "");
      if (key) g_string_append_printf (out, "<span class='key'>%s</span>: ", key);
      g_string_append_printf (out, "%c <span class='muted'>%u %s</span> %c</summary><div class='children'>",
                              v->type, v->children->len,
                              v->type == '{' ? "members" : "items", v->type == '{' ? '}' : ']');
      for (guint i = 0; i < v->children->len; i++)
        html_value (g_ptr_array_index (v->children, i), out, depth + 1);
      g_string_append (out, "</div></details>");
    }
  else
    {
      g_autofree char *raw = g_markup_escape_text (v->raw, -1);
      g_string_append (out, "<div class='value'>");
      if (key) g_string_append_printf (out, "<span class='key'>%s</span>: ", key);
      g_string_append_printf (out, "<span class='%s'>%s</span></div>",
                              v->type == '"' ? "string" : v->type == 'n' ? "number" : "literal", raw);
    }
}

static char *
wrap_html (const char *body, gboolean dark)
{
  return g_strdup_printf (
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='Content-Security-Policy' content=\"default-src 'none'; style-src 'unsafe-inline'; form-action 'none'; base-uri 'none'\">"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Document Preview</title><style>"
    ":root{color-scheme:%s;--bg:%s;--fg:%s;--sub:%s;--line:%s;--code:%s}"
    "body{background:var(--bg);color:var(--fg);font:16px/1.65 system-ui,sans-serif;margin:0;padding:24px;overflow-wrap:anywhere}"
    "main{max-width:980px;margin:auto}h1,h2,h3{line-height:1.3}h1,h2{border-bottom:1px solid var(--line);padding-bottom:.3em}"
    "a,.key{color:%s}a{pointer-events:none}pre,code,.json{font-family:monospace;font-size:14px}"
    "pre{padding:16px;overflow:auto;background:var(--code);border-radius:8px;white-space:pre-wrap}"
    "code{background:var(--code);padding:.15em .3em;border-radius:4px}pre code{padding:0}"
    "blockquote{margin-left:0;border-left:4px solid var(--line);padding-left:18px;color:var(--sub)}"
    "table{border-collapse:collapse;display:block;overflow:auto}td,th{border:1px solid var(--line);padding:7px 12px}"
    "th{background:var(--code)}img{display:none}hr{border:0;border-top:1px solid var(--line)}"
    "details{margin:5px 0}summary{cursor:pointer;border-radius:4px}summary:hover{background:var(--code)}"
    ".children{margin-left:18px;padding-left:12px;border-left:1px solid var(--line)}"
    ".value{white-space:pre-wrap;margin:4px 0}.muted,.notice{color:var(--sub)}"
    ".notice{font-size:12px;margin-bottom:20px}.string{color:%s}.number{color:%s}.literal{color:%s}"
    ".record{padding:12px;border:1px solid var(--line);border-radius:8px;margin-bottom:16px}"
    ".error{border-left:4px solid #e66100;padding:12px;background:var(--code);white-space:pre-wrap}"
    "</style></head><body><main>%s</main></body></html>",
    dark ? "dark" : "light", dark ? "#242424" : "#ffffff", dark ? "#eeeeec" : "#202020",
    dark ? "#bbbbbb" : "#626262", dark ? "#505050" : "#dedede", dark ? "#303030" : "#f3f3f3",
    dark ? "#99c1f1" : "#1a5fb4", dark ? "#8ff0a4" : "#18752a",
    dark ? "#f8e45c" : "#8d4300", dark ? "#dc8add" : "#813d9c", body);
}

char *
editor_preview_error_html (const char *message, gboolean dark)
{
  g_autofree char *escaped = g_markup_escape_text (message, -1);
  g_autofree char *body = g_strdup_printf ("<h2>Preview unavailable</h2><div role='alert' class='error'>%s</div><p>Your source has not been changed.</p>", escaped);
  return wrap_html (body, dark);
}

/* HTML is a complete document, never a Markdown/JSON source section. The
 * opaque-origin iframe permits in-page scripts but cannot read the wrapper,
 * navigate its parent, submit forms, open windows or access persistent storage.
 * CSP is inherited by srcdoc and cannot be relaxed by the supplied document. */
static gboolean
inline_three (const GMatchInfo *match, GString *result, gpointer data)
{
  GBytes *bytes = data;
  gsize size;
  const char *script = g_bytes_get_data (bytes, &size);
  g_string_append (result, "<script>");
  g_string_append_len (result, script, size);
  g_string_append (result, "</script>");
  return FALSE;
}

static char *
render_html_document (const char *text, GError **error)
{
  /* Only this exact, bundled library is substituted. Arbitrary URL/file
   * fetching is never enabled, and the user's source buffer is never edited. */
  g_autoptr(GRegex) three = g_regex_new (
    "<script\\b[^>]*\\bsrc\\s*=\\s*(['\"])https://(?:cdnjs\\.cloudflare\\.com/ajax/libs/three\\.js/r128/three\\.min\\.js|cdn\\.jsdelivr\\.net/npm/three@0\\.128\\.0/build/three\\.min\\.js|unpkg\\.com/three@0\\.128\\.0/build/three\\.min\\.js)\\1[^>]*>\\s*</script\\s*>",
    G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
  g_autofree char *prepared = NULL;
  g_autofree char *escaped = NULL;
  if (g_regex_match (three, text, 0, NULL))
    {
      g_autoptr(GBytes) library = g_resources_lookup_data (
          "/org/gnome/TextEditor/preview/three-r128.min.js", G_RESOURCE_LOOKUP_FLAGS_NONE, error);
      if (!library) return NULL;
      prepared = g_regex_replace_eval (three, text, -1, 0, 0, inline_three, library, error);
      if (!prepared) return NULL;
    }
  escaped = g_markup_escape_text (prepared ? prepared : text, -1);
  return g_strdup_printf (
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='Content-Security-Policy' content=\"default-src 'none'; "
    "script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; "
    "font-src data:; media-src data: blob:; frame-src about:; connect-src 'none'; "
    "worker-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'\">"
    "<style>html,body{margin:0;width:100%%;height:100%%;overflow:hidden}"
    "iframe{display:block;border:0;width:100%%;height:100%%;background:white}</style>"
    "</head><body><iframe title='HTML document preview' sandbox='allow-scripts' "
    "referrerpolicy='no-referrer' srcdoc=\"%s\"></iframe></body></html>", escaped);
}

static char *
render_internal (const char *text, EditorPreviewKind kind, gboolean dark, guint first_line,
                 gboolean partial, gboolean starts_midline, gboolean ends_midline,
                 int expand, gboolean tolerant, GError **error)
{
  g_autoptr(GString) body = g_string_new ("<p class='notice'>Read-only, offline preview · Source is unchanged · External images and embedded HTML are blocked</p>");
  if (!check_input (text, error)) return NULL;
  if (kind == EDITOR_PREVIEW_HTML) return render_html_document (text, error);
  if (partial && kind == EDITOR_PREVIEW_MARKDOWN)
    g_string_append (body, "<p class='notice'>Markdown section: blocks crossing a section boundary may render differently. Use the section controls to read the complete document.</p>");
  if (kind == EDITOR_PREVIEW_MARKDOWN)
    {
      static gsize initialized;
      static const char *extensions[] = { "table", "strikethrough", "autolink", "tagfilter", "tasklist" };
      cmark_parser *parser;
      cmark_node *root;
      char *html;
      if (g_once_init_enter (&initialized))
        { cmark_gfm_core_extensions_ensure_registered (); g_once_init_leave (&initialized, 1); }
      parser = cmark_parser_new (CMARK_OPT_DEFAULT | CMARK_OPT_VALIDATE_UTF8);
      for (guint i = 0; i < G_N_ELEMENTS (extensions); i++)
        cmark_parser_attach_syntax_extension (parser, cmark_find_syntax_extension (extensions[i]));
      cmark_parser_feed (parser, text, strlen (text));
      root = cmark_parser_finish (parser);
      html = cmark_render_html (root, CMARK_OPT_DEFAULT, cmark_parser_get_syntax_extensions (parser));
      g_string_append (body, html);
      free (html);
      cmark_node_free (root);
      cmark_parser_free (parser);
    }
  else if (kind == EDITOR_PREVIEW_JSON)
    {
      g_autoptr(GError) local = NULL;
      g_autoptr(Value) v = partial ? NULL : parse (text, first_line, &local);
      if (!v && (partial || (tolerant && local &&
          (strstr (local->message, "128") || strstr (local->message, "20,000")))))
        {
          g_autofree char *safe = g_markup_escape_text (text, -1);
          g_string_append (body, "<p class='notice'>JSON source section — a partial section is not a complete JSON value. Whole-document validation is not implied. All source text remains accessible through section navigation.</p><pre>");
          g_string_append (body, safe); g_string_append (body, "</pre>");
        }
      else if (!v) { g_propagate_error (error, g_steal_pointer (&local)); return NULL; }
      else
        {
          g_string_append (body, "<div class='json'>");
          html_value (v, body, expand < 0 ? G_MAXUINT / 2 : 0);
          g_string_append (body, "</div>");
        }
    }
  else if (kind == EDITOR_PREVIEW_JSONL)
    {
      g_auto(GStrv) lines = g_strsplit (text, "\n", -1);
      guint records = 0;
      for (guint i = 0; lines[i]; i++)
        {
          g_autoptr(Value) value = NULL;
          const char *line = lines[i];
          while (*line == ' ' || *line == '\t' || *line == '\r') line++;
          if (!*line) continue;
          records++;
          {
            g_autoptr(GError) local = NULL;
            gboolean fragment = (i == 0 && starts_midline) || (lines[i + 1] == NULL && ends_midline);
            if (!fragment) value = parse (lines[i], first_line + i, &local);
            if (!value && !tolerant) { g_propagate_error (error, g_steal_pointer (&local)); return NULL; }
            if (tolerant) g_string_append_printf (body, "<article><h2>Record at Line %u</h2>", first_line + i);
            else g_string_append_printf (body, "<article><h2>Record %u · Line %u</h2>", records, first_line + i);
            if (value) { g_string_append (body, "<div class='json'>"); html_value (value, body, 0); g_string_append (body, "</div>"); }
            else
              {
                g_autofree char *safe = g_markup_escape_text (lines[i], -1);
                g_autofree char *message = g_markup_escape_text (fragment ? "Record continues in another section; showing the exact source fragment." : local->message, -1);
                g_string_append_printf (body, "<p class='notice'>%s</p><pre>%s</pre>", message, safe);
              }
            g_string_append (body, "</article>");
          }
          if (body->len > 16 * 1024 * 1024) break;
        }
      if (!records) g_string_append (body, "<p>No JSON Lines records yet.</p>");
    }
  else
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Choose a Markdown, JSON, or JSON Lines document to preview.");
      return NULL;
    }
  if (body->len > 16 * 1024 * 1024)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Rendered preview exceeds the 16 MiB display limit. Use Source view.");
      return NULL;
    }
  if (expand != 0)
    {
      g_autoptr(GString) adjusted = g_string_new (NULL);
      const char *cursor = body->str, *match;
      const char *from = expand > 0 ? "<details>" : "<details open>";
      const char *to = expand > 0 ? "<details open>" : "<details>";
      while ((match = strstr (cursor, from)))
        { g_string_append_len (adjusted, cursor, match - cursor); g_string_append (adjusted, to); cursor = match + strlen (from); }
      g_string_append (adjusted, cursor);
      return wrap_html (adjusted->str, dark);
    }
  return wrap_html (body->str, dark);
}

char *
editor_preview_render (const char *text, EditorPreviewKind kind, gboolean dark, GError **error)
{
  return render_internal (text, kind, dark, 1, FALSE, FALSE, FALSE, 0, FALSE, error);
}

char *
editor_preview_render_section (const char *text, EditorPreviewKind kind, gboolean dark,
                               guint first_line, gboolean partial_document,
                               gboolean starts_midline, gboolean ends_midline,
                               int expand, GError **error)
{
  return render_internal (text, kind, dark, first_line, partial_document,
                          starts_midline, ends_midline, expand, TRUE, error);
}
