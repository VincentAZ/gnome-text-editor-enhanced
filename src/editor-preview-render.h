/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

#define EDITOR_PREVIEW_SECTION_CHARS 65536
#define EDITOR_PREVIEW_SECTION_LINES 200

typedef enum {
  EDITOR_PREVIEW_NONE,
  EDITOR_PREVIEW_MARKDOWN,
  EDITOR_PREVIEW_JSON,
  EDITOR_PREVIEW_JSONL,
  EDITOR_PREVIEW_HTML
} EditorPreviewKind;

EditorPreviewKind editor_preview_detect (const char *filename, const char *language);
char *editor_preview_render (const char *text, EditorPreviewKind kind, gboolean dark, GError **error);
char *editor_preview_format_json (const char *text, GError **error);
char *editor_preview_error_html (const char *message, gboolean dark);

char *editor_preview_render_section (const char *text, EditorPreviewKind kind, gboolean dark,
                                     guint first_line, gboolean partial_document,
                                     gboolean starts_midline, gboolean ends_midline,
                                     int expand, GError **error);
