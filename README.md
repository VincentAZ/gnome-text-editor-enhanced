# GNOME Text Editor Enhanced

GNOME Text Editor with **local Markdown, JSON and JSON Lines previews**, a source/preview split view, collapsible JSON trees, and undoable JSON formatting.

This is a community enhancement of [GNOME Text Editor](https://gitlab.gnome.org/GNOME/gnome-text-editor), based on the **50.1 release**. It is not a new editor written from scratch, and is not an official GNOME release.

## Current features

- **Markdown:** rendered headings, lists, tables, task lists and fenced code.
- **JSON:** collapsible object/array trees, syntax errors with line/column information, and an explicit **Format JSON** command. Formatting is one undo step and does not save the file automatically.
- **JSONL / NDJSON:** one structured record per line, with record and source-line labels.
- **Source / Preview / Split View:** keep editing the original source while viewing a read-only preview.
- **Local and restricted:** no preview JavaScript, plugins, remote page navigation or network resources. Raw Markdown HTML is not executed; JSON strings are escaped.
- **Native identity:** the default build remains `org.gnome.TextEditor`. An optional separate preview build is available with `-Dpreview_build=true`.

**Known limits in this first published enhancement:** preview and Format JSON have a 2 MiB input limit; the JSONL renderer allows 500 records at a time. Large-file paging and more flexible panel sizing are the next requested changes, not features claimed by this commit.

## Build and test

You need a recent GNOME development environment: GLib/GIO >= 2.80, GTK >= 4.19.4, libadwaita >= 1.8.rc, GtkSourceView >= 5.15, libspelling >= 0.4, WebKitGTK 6 >= 2.40, cmark-gfm and its core extensions, editorconfig, plus Meson, Ninja, a C compiler, pkg-config, gettext and the usual GNOME resource/documentation tools. Older Linux distributions may not provide these library versions. See `meson.build` and `src/meson.build` for authoritative dependency checks.

```sh
meson setup build --prefix=/usr -Ddevelopment=false -Dpreview_build=false --buildtype=debugoptimized
meson compile -C build
meson test -C build --print-errorlogs
```

The page/WebKit integration test requires `dbus-run-session`, `xvfb-run` and Xvfb. Its runner uses temporary settings, session and cache directories, not your normal editor session. The rendering test checks escaping, formatting, malformed input and resource limits.

For an isolated development identity, configure another build directory with `-Dpreview_build=true` and run its generated launcher. For system replacement, use your distribution's packaging workflow rather than copying files over package-managed files.

## Updates and rollback

The first commit and **`upstream-50.1` tag** preserve the pristine GNOME 50.1 source archive. Enhancements are separate commits. Inspect them with:

```sh
git log --oneline --decorate
git diff upstream-50.1 -- src meson.build meson_options.txt data
```

Before updating an installed editor, save documents and retain the original distribution package. Rebuild and run tests before installing an update. To roll back a packaged installation, reinstall your distribution's original `gnome-text-editor` package (allowing a package downgrade if necessary). Source files and editor session/settings backups are separate from the executable package: do not delete them during rollback. See [maintenance notes](docs/MAINTENANCE.md).

## Demo files and privacy

`examples/` contains synthetic demonstration text only. This repository deliberately excludes personal JSONL datasets, editor session backups, settings dumps, desktop screenshots, local package staging directories and build output. No screenshot of a personal document is published.

## License and attribution

**GPL-3.0-or-later**, following upstream. See [COPYING](COPYING) and retained file-level copyright/license notices. This fork is **not MIT-licensed**. GNOME, Text Editor and their artwork belong to their respective owners. The original project description is retained in [docs/UPSTREAM-README.md](docs/UPSTREAM-README.md).
