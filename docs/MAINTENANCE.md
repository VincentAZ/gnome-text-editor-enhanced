# Maintenance and provenance

- Upstream: https://gitlab.gnome.org/GNOME/gnome-text-editor
- Release archive: `gnome-text-editor-50.1.tar.xz`
- Archive SHA-256: `f68036b09d378faa883bfe936e479c6ff37027c2ffed101daf912df70c51d0e6`
- Unmodified baseline: Git tag `upstream-50.1`.
- The next commit imports the already-built local Markdown/JSON/JSONL enhancement. Later fixes should remain separate commits.

## Updating

Start from a clean working tree. Review upstream changes and merge/rebase them without discarding license notices or the enhancement tests. Build and run `meson test -C build --print-errorlogs`. Test both normal and optional preview identities if changing packaging. Do not copy a pre-existing build directory to another checkout: reconfigure a fresh build there.

## Installing and rolling back

The default target is the existing GNOME Text Editor, not an unofficial web wrapper. Use the distribution package manager to install a replacement package and retain a known-good original package first. Close the running editor only after saving documents; never kill it to replace the process. Reopen after installation to load the new executable.

A source rollback can be explored without changing main using `git worktree add ../text-editor-upstream upstream-50.1`. Build in that worktree using the upstream build options. This does not itself roll back an installed package.

An installed package rollback must reinstall the original distribution package with the package manager. Keep user documents and session/settings backups outside this public repository. Never publish those backups or raw user data as demo material.
