# The Flatpak

```
io.github.Pr1nted.OpenDoctrines.yml           the manifest
io.github.Pr1nted.OpenDoctrines.desktop       the launcher entry
io.github.Pr1nted.OpenDoctrines.metainfo.xml  AppStream data -- Flathub requires it
opendoctrines-launcher                        seeds a writable data dir, then runs the game
README.md                                     this
```

## Why bother

`README.md` tells anyone on Ubuntu 20.04, Debian 11 or RHEL 9 to build from
source, because the published binary is built on Ubuntu 22.04 and needs glibc
2.35. A Flatpak brings its own runtime, so on exactly those distributions this
turns "compile it yourself" into "install it". That is the entire justification;
if it did not fix that, a tarball would do.

## Build and run it locally

```bash
flatpak install -y flathub org.freedesktop.Platform//24.08 org.freedesktop.Sdk//24.08
flatpak-builder --force-clean --user --install build-flatpak \
    packaging/linux/io.github.Pr1nted.OpenDoctrines.yml
flatpak run io.github.Pr1nted.OpenDoctrines
```

Validate the metadata before submitting anywhere — Flathub runs the same checks
and a failure there is a slow round trip:

```bash
appstreamcli validate packaging/linux/io.github.Pr1nted.OpenDoctrines.metainfo.xml
desktop-file-validate packaging/linux/io.github.Pr1nted.OpenDoctrines.desktop
```

## Two things a reviewer will raise

**It unpacks a release instead of building from source.** flatpak-builder has no
network, and `CMakeLists.txt` fetches six dependencies with `FetchContent` at
configure time — raylib, harfbuzz, wamr, mbedtls, libdatachannel and dragoman —
each of which would need to become its own pinned module, followed by their
transitive dependencies. Extracting the release archive is one URL and one
checksum per version. Flathub prefers source builds and may push back; the
answer is that the alternative is a manifest nobody can maintain. A self-hosted
repo has no such opinion.

**The licence is not free software.** OpenDoctrines is free to play, modify and
share NON-COMMERCIALLY, which is not OSI-approved. Flathub accepts proprietary
applications and will label it accordingly. This is also why F-Droid and the
Debian and Fedora archives are closed to this project — a real cost of the
licence, worth knowing rather than discovering.

## Per-release maintenance

Two lines in the manifest:

```bash
url=https://github.com/Pr1nted/Open-Doctrines/releases/download/v<VERSION>/OpenDoctrines-linux-x64.zip
curl -sL "$url" | shasum -a 256
```

Update `url:`, `sha256:`, and add a `<release>` entry to the metainfo — Flathub
shows the newest one as the changelog, and a missing entry is a validation
warning.

## The writable-data problem, and why the launcher exists

The game treats its data directory as read-write: `config.json`, `saves/` and
the trained AI model all live inside it (`Game.cpp`, where `m_configPath` is
`m_dataDir + "config.json"`). Inside a Flatpak `/app` is read-only, so running
straight against the installed tree fails on the first settings change — and
fails *quietly*, because a config that cannot be written is indistinguishable
from one that has nothing in it.

So `opendoctrines-launcher` copies the shipped tree into `XDG_DATA_HOME` on
first run and points `OD_DATA_DIR` at the copy. On a version change it refreshes
the shipped parts and steps around `saves`, `mods`, `ai` and `config.json`,
which belong to the player and are never overwritten from `/app`.
