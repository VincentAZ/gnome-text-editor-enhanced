# GNOME Text Editor Enhanced

GNOME Text Editor with **local Markdown, JSON, JSON Lines and HTML previews**, editable source, collapsible JSON trees, and undoable JSON formatting.

This is a community enhancement of [GNOME Text Editor](https://gitlab.gnome.org/GNOME/gnome-text-editor), based on the **50.1 release**. It is not a new editor written from scratch, and is not an official GNOME release.

## Current features

- **HTML:** full-document rendering for `.html`, `.htm` and the HTML Document Type. Inline CSS, SVG, canvas and JavaScript controls run in an isolated, opaque-origin frame. Source / Preview / Split and live updates work without saving. The exact Three.js r128 CDN dependency is bundled for offline 3D previews; other online resources and local-file requests stay blocked.
- **Markdown:** rendered headings, lists, tables, task lists and fenced code.
- **JSON:** collapsible object/array trees, syntax errors with line/column information, and an explicit **Format JSON** command. Formatting runs in the background, is one undo step, and never saves automatically. If the source changes during formatting, an obsolete result is discarded.
- **JSONL / NDJSON:** structured records with record and source-line labels.
- **Source / Preview / Split View:** edit the original source beside a read-only preview. **Stacked** and **Side by Side** change the split direction.
- **Large documents:** first/previous/next/last section controls, **Go to line in preview**, and **Preview at Cursor** navigate beyond the initial section. Expand/collapse controls apply to the current JSON or JSONL section.
- **Resizable Properties:** drag the divider; close and reopen the panel without losing document access.
- **Recoverable previews:** if the separate preview process stops, a native **Preview Unavailable** screen offers **Retry Preview**. The source stays unchanged and remains available through **Source**.
- **Local and restricted:** no remote navigation, network resources, plugins, file access, permission prompts, form submission or persistent web storage. HTML inline scripts run only inside a sandboxed frame; Markdown and JSON still have JavaScript disabled. Raw Markdown HTML is not executed; JSON strings are escaped.
- **Native identity:** the default build remains `org.gnome.TextEditor`, using the original desktop entry, icon and `gnome-text-editor` executable. An optional separate edition is available with `-Dpreview_build=true`.

## Scope and limits

- Rendered previews support Markdown, JSON, JSONL, NDJSON and HTML. HTML is an offline document preview, not an unrestricted browser: external fonts, arbitrary web libraries, relative file assets, external links, downloads and requests to online services are blocked. Self-contained HTML works best. In-page changes are temporary and never written back to the source.
- Large Markdown/JSON/JSONL previews use bounded sections (up to roughly 65,536 characters and 200 lines), not a full-document DOM. Navigation reaches later sections, including long single-line files, without splitting UTF-8 characters.
- A JSON/JSONL render is limited to 128 nesting levels and 20,000 values per section. Partial sections or exceeded rendering limits are explained rather than changing the source. Markdown structures spanning a section boundary may be incomplete in that section.
- Very long source lines use a simplified source display to avoid pathological wrapping/highlighting. A visible status identifies that mode; normal wrapping is restored for ordinary text.
- HTML uses a full-document DOM in the separate preview process, not source sections; extremely complex HTML can consume substantial memory. Source mode unloads the interactive page.
- No destructive JSONL formatting or preview-link navigation.
- Formatting preserves JSON number tokens, duplicate keys and original string escapes. Invalid or cancelled formatting does not replace the source. Extremely large formatted output is bounded to protect memory.
- Source editing and loading still use the upstream in-memory text buffer; preview paging does not make the editor a disk-backed large-file database.

## Build and test

You need a recent GNOME development environment: GLib/GIO >= 2.80, GTK >= 4.19.4, libadwaita >= 1.8.rc, GtkSourceView >= 5.15, libspelling >= 0.4, WebKitGTK 6 >= 2.40, cmark-gfm and its core extensions, editorconfig, plus Meson, Ninja, a C compiler, pkg-config, gettext and the usual GNOME resource/documentation tools. Older Linux distributions may not provide these versions. See `meson.build` and `src/meson.build` for authoritative dependency checks.

```sh
meson setup build --prefix=/usr -Ddevelopment=false -Dpreview_build=false --buildtype=debugoptimized
meson compile -C build
meson test -C build --print-errorlogs
```

The page/WebKit integration test requires `dbus-run-session`, `xvfb-run` and Xvfb. Its runner uses temporary settings, session and cache directories, not your normal editor session. Tests exercise live updates, view modes, Properties resizing, stacked splits, tree controls, sections beyond 3 MB and 6,500 records, long-line UTF-8 navigation, asynchronous formatting and stale-result protection, forced preview-process termination and recovery, and unchanged source after read-only operations. Renderer tests check escaping, formatting, malformed input, HTML sandbox/CSP construction and resource limits. HTML integration checks inline scripts and button interaction, opaque-origin isolation, blocked network/file/navigation attempts, full-document loading, live updates and unchanged source.

For an isolated development identity, configure another build directory with `-Dpreview_build=true` and run its generated launcher. For system replacement, use your distribution's packaging workflow rather than copying over package-managed files. Automated success is not proof that every graphics driver or live desktop window works; verify the installed app separately.

## Updates and rollback

The first commit and **`upstream-50.1` tag** preserve the pristine GNOME 50.1 source archive. Enhancements are separate commits. Inspect them with:

```sh
git log --oneline --decorate
git diff upstream-50.1 -- src meson.build meson_options.txt data
```

Before updating an installed editor, save documents and retain the original distribution package. Rebuild and run tests before installing an update. To roll back a packaged installation, reinstall your distribution's original `gnome-text-editor` package (allowing a downgrade if necessary). Source files and editor session/settings backups are separate from the executable package: do not delete them during rollback. See [maintenance notes](docs/MAINTENANCE.md).

## Demo files and privacy

`examples/` contains synthetic demonstration text only. This repository deliberately excludes personal JSONL datasets, editor session backups, settings dumps, desktop screenshots, local package staging directories and build output. No screenshot of a personal document is published.

## License and attribution

**GPL-3.0-or-later**, following upstream. The bundled Three.js r128 library retains its MIT license in `src/preview-vendor/THREE-LICENSE`. See [COPYING](COPYING) and retained file-level copyright/license notices. This fork is **not MIT-licensed**. GNOME, Text Editor and their artwork belong to their respective owners. The original project description is retained in [docs/UPSTREAM-README.md](docs/UPSTREAM-README.md).
