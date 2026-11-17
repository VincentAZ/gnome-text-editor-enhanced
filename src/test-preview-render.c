/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "editor-preview-render.h"
#include <gio/gio.h>
#include <string.h>

static void test_detect (void)
{
  g_assert_cmpint (editor_preview_detect ("README.MD", NULL), ==, EDITOR_PREVIEW_MARKDOWN);
  g_assert_cmpint (editor_preview_detect ("a.markdown", NULL), ==, EDITOR_PREVIEW_MARKDOWN);
  g_assert_cmpint (editor_preview_detect ("a.json", NULL), ==, EDITOR_PREVIEW_JSON);
  g_assert_cmpint (editor_preview_detect ("a.JSONL", "json"), ==, EDITOR_PREVIEW_JSONL);
  g_assert_cmpint (editor_preview_detect ("a.ndjson", NULL), ==, EDITOR_PREVIEW_JSONL);
  g_assert_cmpint (editor_preview_detect (NULL, "markdown"), ==, EDITOR_PREVIEW_MARKDOWN);
  g_assert_cmpint (editor_preview_detect (NULL, "json"), ==, EDITOR_PREVIEW_JSON);
  g_assert_cmpint (editor_preview_detect ("a.HTML", NULL), ==, EDITOR_PREVIEW_HTML);
  g_assert_cmpint (editor_preview_detect ("a.htm", NULL), ==, EDITOR_PREVIEW_HTML);
  g_assert_cmpint (editor_preview_detect (NULL, "html"), ==, EDITOR_PREVIEW_HTML);
  g_assert_cmpint (editor_preview_detect ("a.txt", NULL), ==, EDITOR_PREVIEW_NONE);
}

static void test_html (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *html = editor_preview_render (
      "<!doctype html><title>A & B</title><style>body{background:red}</style>"
      "<canvas></canvas><script>document.body.dataset.ready='yes'</script>",
      EDITOR_PREVIEW_HTML, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (html, "sandbox='allow-scripts'"));
  g_assert_null (strstr (html, "allow-same-origin"));
  g_assert_nonnull (strstr (html, "connect-src 'none'"));
  g_assert_nonnull (strstr (html, "form-action 'none'"));
  g_assert_nonnull (strstr (html, "&lt;script&gt;"));
  g_assert_nonnull (strstr (html, "A &amp; B"));
  g_assert_nonnull (strstr (html, "&lt;canvas&gt;"));
}

static void test_markdown (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *html = editor_preview_render (
    "# Heading\n\n**bold** and *italic* and ~~deleted~~\n\n- item\n\n"
    "| Name | Value |\n| --- | --- |\n| A | B |\n\n```json\n{\"x\":1}\n```\n\n- [x] Done\n",
    EDITOR_PREVIEW_MARKDOWN, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (html, "<h1>Heading</h1>"));
  g_assert_nonnull (strstr (html, "<strong>bold</strong>"));
  g_assert_nonnull (strstr (html, "<em>italic</em>"));
  g_assert_nonnull (strstr (html, "<del>deleted</del>"));
  g_assert_nonnull (strstr (html, "<table>"));
  g_assert_nonnull (strstr (html, "language-json"));
  g_assert_nonnull (strstr (html, "type=\"checkbox\""));
}

static void test_security (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *html = editor_preview_render (
    "<script>alert('bad')</script>\n\n<img src='file:///etc/passwd'>\n\n"
    "[click](javascript:alert%281%29)\n\n![remote](https://example.invalid/track.png)\n",
    EDITOR_PREVIEW_MARKDOWN, TRUE, &error);
  g_assert_no_error (error);
  g_assert_null (strstr (html, "<script>"));
  g_assert_null (strstr (html, "src='file:///etc/passwd'"));
  g_assert_null (strstr (html, "href=\"javascript:"));
  g_assert_nonnull (strstr (html, "default-src 'none'"));
  g_assert_nonnull (strstr (html, "form-action 'none'"));
  g_assert_nonnull (strstr (html, "color-scheme:dark"));
}

static void test_tree (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *html = editor_preview_render (
    "{\"greeting\":\"<script>& hello\",\"large\":123456789012345678901234567890,\"array\":[true,null,{\"nested\":2}]}",
    EDITOR_PREVIEW_JSON, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (html, "<details open>"));
  g_assert_nonnull (strstr (html, "3 members"));
  g_assert_nonnull (strstr (html, "123456789012345678901234567890"));
  g_assert_nonnull (strstr (html, "&lt;script&gt;&amp; hello"));
  g_assert_null (strstr (html, "<script>"));
}

static void test_format (void)
{
  const char *input = "{ \"id\":123456789012345678901234567890, \"f\":1.2300e+004, \"same\":1,\"same\":2, \"str\":\"\\uD83D\\uDE00\\n\", \"a\":[true,null,{}] }\n";
  const char *expected = "{\n  \"id\": 123456789012345678901234567890,\n  \"f\": 1.2300e+004,\n  \"same\": 1,\n  \"same\": 2,\n  \"str\": \"\\uD83D\\uDE00\\n\",\n  \"a\": [\n    true,\n    null,\n    {}\n  ]\n}\n";
  g_autoptr(GError) error = NULL;
  g_autofree char *formatted = editor_preview_format_json (input, &error);
  g_autofree char *again = NULL;
  g_assert_no_error (error);
  g_assert_cmpstr (formatted, ==, expected);
  again = editor_preview_format_json (formatted, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (again, ==, formatted);
}

static void test_invalid (void)
{
  const char *invalid[] = { "", "{", "[1,]", "{\"a\":1,}", "{a:1}", "01", "-", "1.", "1e+", "NaN", "true false", "\"a\n\"", "\"\\q\"", "\"\\uD800\"", "\"\\uDC00\"", "\"\\u12\"", "\"\\uD800\\u0041\"", "\"unterminated", "{\"x\":}", "[true false]", "nullz", "{\"x\" 1}" };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autoptr(GError) error = NULL;
      g_autofree char *out = editor_preview_format_json (invalid[i], &error);
      g_assert_null (out);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_assert_nonnull (strstr (error->message, "Line 1"));
    }
}

static void test_scalars (void)
{
  const char *valid[] = { "null", "true", "false", "0", "-0", "-0.01E-99", "\"hello 日本語\"", "[]", "{}", " [ 1 , 2 ] ", "\"\\u0000\"", "\"\\\"\\/\\\\\\b\\f\\n\\r\\t\"" };
  for (guint i = 0; i < G_N_ELEMENTS (valid); i++)
    {
      g_autoptr(GError) error = NULL;
      g_autofree char *out = editor_preview_format_json (valid[i], &error);
      g_assert_no_error (error);
      g_assert_nonnull (out);
    }
}

static void test_jsonl (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *html = editor_preview_render ("{\"role\":\"user\"}\n\n{\"role\":\"assistant\"}\r\n42\n", EDITOR_PREVIEW_JSONL, FALSE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strstr (html, "Record 1 · Line 1"));
  g_assert_nonnull (strstr (html, "Record 2 · Line 3"));
  g_assert_nonnull (strstr (html, "Record 3 · Line 4"));
}

static void test_jsonl_error (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *html = editor_preview_render ("{}\n\n{\"broken\":}\n", EDITOR_PREVIEW_JSONL, FALSE, &error);
  g_assert_null (html);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (error->message, "Line 3"));
}

static void test_large_input (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GString) huge = g_string_new ("{\"payload\":\"");
  g_autofree char *payload = g_strnfill (3 * 1024 * 1024, 'x');
  g_autofree char *formatted = NULL;
  g_autofree char *html = NULL;
  g_string_append (huge, payload); g_string_append (huge, "\",\"last\":true}");
  formatted = editor_preview_format_json (huge->str, &error);
  g_assert_no_error (error); g_assert_nonnull (formatted);
  g_assert_nonnull (strstr (formatted, "\"last\": true"));
  html = editor_preview_render (huge->str, EDITOR_PREVIEW_JSON, FALSE, &error);
  g_assert_no_error (error); g_assert_nonnull (html);
  g_clear_pointer (&html, g_free);
  html = editor_preview_render ("\xff", EDITOR_PREVIEW_JSON, FALSE, &error);
  g_assert_null (html); g_assert_nonnull (error);
}

static void test_many_records (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GString) lines = g_string_new (NULL);
  g_autofree char *html = NULL;
  for (guint i = 0; i < 1200; i++) g_string_append (lines, "{\"ok\":true}\n");
  html = editor_preview_render (lines->str, EDITOR_PREVIEW_JSONL, FALSE, &error);
  g_assert_no_error (error); g_assert_nonnull (strstr (html, "Record 1200 · Line 1200"));
  g_clear_pointer (&html, g_free);
  html = editor_preview_render_section ("{\"ok\":true}\n{\"next\":1}\n", EDITOR_PREVIEW_JSONL,
      FALSE, 5001, TRUE, FALSE, FALSE, 0, &error);
  g_assert_no_error (error); g_assert_nonnull (strstr (html, "Record at Line 5002"));
}

static void test_format_large_structure (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GString) text = g_string_new ("[");
  g_autofree char *formatted = NULL;
  g_autofree char *html = NULL;
  for (guint i = 0; i < 25000; i++) g_string_append (text, i ? ",1" : "1");
  g_string_append_c (text, ']');
  formatted = editor_preview_format_json (text->str, &error);
  g_assert_no_error (error); g_assert_nonnull (formatted);
  html = editor_preview_render_section (text->str, EDITOR_PREVIEW_JSON, FALSE, 1,
                                       FALSE, FALSE, FALSE, 0, &error);
  g_assert_no_error (error); g_assert_nonnull (strstr (html, "JSON source section"));
  g_clear_pointer (&formatted, g_free); g_string_set_size (text, 0);
  for (guint i = 0; i < 256; i++) g_string_append_c (text, '[');
  g_string_append (text, "0");
  for (guint i = 0; i < 256; i++) g_string_append_c (text, ']');
  formatted = editor_preview_format_json (text->str, &error);
  g_assert_no_error (error); g_assert_nonnull (formatted);
}

static void test_format_invalid (void)
{
  const char *invalid[] = { "", "[1,]", "{\"a\":1,}", "{a:1}", "{\"a\" 1}", "true false", "01", "1e+", "[", "{", "\"bad\\x\"", "\"raw\nline\"" };
  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      g_autoptr(GError) error = NULL;
      g_autofree char *formatted = editor_preview_format_json (invalid[i], &error);
      g_assert_null (formatted); g_assert_nonnull (error);
    }
}

static void test_section_safety_and_controls (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *html = editor_preview_render_section ("{\"incomplete\":\"<script>alert(1)</script>",
      EDITOR_PREVIEW_JSON, FALSE, 1, TRUE, FALSE, TRUE, 0, &error);
  g_assert_no_error (error); g_assert_null (strstr (html, "<script>alert"));
  g_assert_nonnull (strstr (html, "&lt;script&gt;"));
  g_clear_pointer (&html, g_free);
  html = editor_preview_render_section ("{\"a\":[{\"b\":[1]}]}", EDITOR_PREVIEW_JSON, FALSE, 1,
                                       FALSE, FALSE, FALSE, 1, &error);
  g_assert_no_error (error); g_assert_null (strstr (html, "<details>"));
  g_assert_nonnull (strstr (html, "<details open>"));
  g_clear_pointer (&html, g_free);
  html = editor_preview_render_section ("{\"a\":[{\"b\":[1]}]}", EDITOR_PREVIEW_JSON, FALSE, 1,
                                       FALSE, FALSE, FALSE, -1, &error);
  g_assert_no_error (error); g_assert_null (strstr (html, "<details open>"));
  g_clear_pointer (&html, g_free);
  html = editor_preview_render_section ("unfinished\n{\"valid\":true}\n", EDITOR_PREVIEW_JSONL,
      FALSE, 900, TRUE, TRUE, FALSE, 0, &error);
  g_assert_no_error (error); g_assert_nonnull (strstr (html, "Record at Line 901"));
  g_assert_nonnull (strstr (html, "continues in another section"));
}

static void test_errors_safe (void)
{
  g_autofree char *html = editor_preview_error_html ("<script> & broken", FALSE);
  g_assert_null (strstr (html, "<script>"));
  g_assert_nonnull (strstr (html, "&lt;script&gt; &amp; broken"));
  g_assert_nonnull (strstr (html, "source has not been changed"));
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/preview/detect", test_detect);
  g_test_add_func ("/preview/html", test_html);
  g_test_add_func ("/preview/markdown", test_markdown);
  g_test_add_func ("/preview/security", test_security);
  g_test_add_func ("/preview/json-tree", test_tree);
  g_test_add_func ("/preview/format-preserves-lexemes", test_format);
  g_test_add_func ("/preview/invalid-json", test_invalid);
  g_test_add_func ("/preview/scalars-unicode", test_scalars);
  g_test_add_func ("/preview/json-lines", test_jsonl);
  g_test_add_func ("/preview/json-lines-error-location", test_jsonl_error);
  g_test_add_func ("/preview/large-input", test_large_input);
  g_test_add_func ("/preview/many-records", test_many_records);
  g_test_add_func ("/preview/format-large-structure", test_format_large_structure);
  g_test_add_func ("/preview/format-invalid", test_format_invalid);
  g_test_add_func ("/preview/section-safety-controls", test_section_safety_and_controls);
  g_test_add_func ("/preview/error-escaping", test_errors_safe);
  return g_test_run ();
}
