# purec-os-pkg

`pkg` is the package manager of PureC OS. It works like `pacman`: it installs,
removes, upgrades and queries packages, and it keeps a database of what is
installed. There is no package index on a server. The list of packages comes
from a registry file, and every package is a release asset of its own GitHub
repository (or a file in the registry repository itself).

Current version: see [VERSION](VERSION). The man page is [pkg.8](pkg.8)
(`man pkg` on an installed system).

## Sources of packages

A package can come from three kinds of sources:

- **Official repositories.** Listed in `registry.conf` of the registry
  repository, which is named by `Registry` in `/etc/pkg/pkg.conf`. Their
  packages are signed by the official key.
- **Registry packages.** Files listed in `repository.conf` of the same registry
  repository. A name listed there wins over a release with the same name.
- **Community repositories.** Listed in `registry.conf` with the `community`
  keyword. They are used only when `AllowCommunity = yes`, and their packages
  must be signed by a key bound to that repository.
- **Local repositories.** The `[trusted]` section of `pkg.conf`, used only when
  `AllowLocalRepos = yes`. Intended for development.

The registry itself is maintained in a separate repository:
<https://github.com/PureC-OS/PureC-OS-Trusted-PKG-URL>. Third-party repositories
are onboarded there, not in this repository.

## Package format

A package is a gzip-compressed tar archive named
`NAME-VERSION-REL-ARCH.pkg.tar.gz`, for example `foo-1.0-1-x86_64.pkg.tar.gz`.
`pkg` accepts assets named `-x86_64` or `-any` for the configured
`Architecture`.

Archive contents:

- `.PKGINFO`: `key = value` metadata: `name`, `version`, `pkgdesc`, `arch`,
  `size`, and repeated `depend`, `conflict` and `backup` lines. Copied into the
  local database when the package is installed.
- Regular files, stored at their paths relative to the root directory.
- Hooks, stored as `.hook-post-install`, `.hook-post-upgrade`,
  `.hook-pre-remove` and `.hook-post-remove`. They are extracted into the
  package's database directory and run through `/bin/sh` with these environment
  variables set: `PKG_ROOT`, `PKG_NAME`, `PKG_VERSION` and `PKG_OLD_VERSION`
  (empty when there was no previous version). A hook that fails prints a warning.
- Backup files, listed by `backup` in `.PKGINFO`. These are configuration
  files. On upgrade, a file changed by the user is kept and the new version is
  written next to it as `FILE.pkgnew`. On removal, a changed file is kept as
  `FILE.pkgsave`.

## Command reference

Operations take one of the letters `S`, `R`, `Q`, `U` or `D`; only one of them
may be given at a time. Changing operations need root rights.

| Command | Meaning |
|---|---|
| `pkg -S TARGET...` | Install packages and their dependencies. A target is a package name or the GitHub URL of a trusted repository. |
| `pkg -Sy` | Refresh the registry and check the latest release of every repository. |
| `pkg -Su` | Upgrade every installed package that has a newer version. |
| `pkg -Syu` | Refresh, then upgrade. Shows the list and asks before changing anything. |
| `pkg -Ss TERM...` | Search the known packages by name and description. |
| `pkg -Si NAME...` | Show information about a repository package. Full information after it was downloaded. |
| `pkg -Sl` | List the registry, its repositories and their newest versions. |
| `pkg -Sc` | Delete cached package files that are not installed. |
| `pkg -R NAME...` | Remove packages. A package that another installed package needs is not removed. |
| `pkg -Rs NAME...` | Remove packages and the dependencies that nothing else needs. |
| `pkg -Rd NAME...` | Remove packages without checking dependencies (same as `--nodeps`). |
| `pkg -Q [NAME...]` | List installed packages (`name version`), or only the named ones. |
| `pkg -Qi NAME...` | Information about installed packages. |
| `pkg -Ql NAME...` | List the files of installed packages. |
| `pkg -Qo FILE...` | Show which installed package owns a file. |
| `pkg -Qe` | List explicitly installed packages. |
| `pkg -Qd` | List packages installed as dependencies. |
| `pkg -Qs TERM...` | Search installed packages by name and description. |
| `pkg -Qu` | List installed packages that have a newer release. Revoked signers are marked `REVOKED`. |
| `pkg -Qk NAME...` | Check that the files of installed packages exist. |
| `pkg -Qq` | Print names only (`-q` also changes `-Ql` output to bare paths). |
| `pkg -U FILE...` | Install package files from disk. |
| `pkg -D NAME... --asdeps` | Mark installed packages as dependencies. |
| `pkg -D NAME... --asexplicit` | Mark installed packages as explicitly installed. |

Options are combined with operations, for example `pkg -Syu --noconfirm` or
`pkg -Qe -q`. Single-letter options can be grouped (`-Syu`, `-Qeq`).

| Option | Meaning |
|---|---|
| `--root DIR` (or `--root=DIR`) | Operate on another root directory. The databases, the configuration and the installed files are all taken under it. |
| `--config FILE` (or `--config=FILE`) | Use another configuration file instead of `/etc/pkg/pkg.conf`. |
| `--noconfirm` | Do not ask questions. |
| `--needed` | Do not reinstall a package that is already up to date. |
| `--force` | Overwrite files that exist and belong to no package. |
| `--nodeps` | Do not check dependencies. |
| `--asdeps` | Mark installed packages as dependencies (with `-D` or during install). |
| `--asexplicit` | Mark installed packages as explicitly installed. |
| `-v`, `--version` | Show the version. |
| `-h`, `--help` | Show the usage text. |

Running `pkg` without an operation prints the usage text and exits with
status 1.

## Configuration

The configuration is `/etc/pkg/pkg.conf`. If it is missing, the old location
`/etc/pkg.conf` is read. A different file can be given with `--config`. The
file has an `[options]` section and a `[trusted]` section. Blank lines and lines
starting with `#` are ignored.

```
[options]
Architecture = x86_64
SigLevel = Required
Registry = https://github.com/PureC-OS/PureC-OS-Trusted-PKG-URL

[trusted]
# https://github.com/OWNER/REPO
```

### [options]

| Key | Default | Meaning |
|---|---|---|
| `Architecture` | `x86_64` | Architecture of the package assets to use. |
| `SigLevel` | `Optional` | `Never`: no signature checks. `Optional`: a signature is checked when the release has one, and a bad signature is always refused. `Required`: every package and every registry file must be signed. Any value other than `Never` or `Required` is treated as `Optional`. |
| `Registry` | none | The registry: a GitHub repository URL (its `registry.conf` and `repository.conf` are read from the `main` branch), an address ending in `.conf`, or a `file://` path. Needed for official repositories. |
| `API` | `https://api.github.com` | GitHub API address. A `file://` directory also works, which the tests use. |
| `Prerelease` | `no` | `yes` also uses prerelease releases. |
| `AllowLocalRepos` | `no` | `yes` enables the `[trusted]` repositories. They are shown as local. |
| `AllowCommunity` | `no` | `yes` enables the `community` lines of `registry.conf`. When `no`, they are counted and reported as ignored. |
| `KeyringThreshold` | `1` | Number of different master keys that must sign `keyring.conf`. Values below 1 are treated as 1. |

Boolean values accept `yes`, `true` and `1` as true.

### [trusted]

One GitHub repository URL per line, for example
`https://github.com/owner/repo`. Used only with `AllowLocalRepos = yes`.
A release is used when it has an asset named `NAME-VERSION-ARCH.pkg.tar.gz`
(or `-any`). Among all releases the newest version wins.

## Trust model

Packages and registry files are checked with ECDSA P-256 signatures over
SHA-512. The check uses files under `/etc/pkg`:

| Path | Contents |
|---|---|
| `/etc/pkg/keys/*.pub` | Public keys, one per file: 65-byte uncompressed P-256 point as hexadecimal text. Used directly in legacy mode, see below. |
| `/etc/pkg/keys/master/*.pub` | Master keys. They sign `keyring.conf`. |
| `/etc/pkg/keys/local/*.pub` | Keys for `[trusted]` (local) repositories. |
| `/etc/pkg/keyring.conf` | The installed keyring. |

### Keyring

`keyring.conf` is a text file:

```
serial 3
key FP HEXPOINT official
key FP HEXPOINT repo=https://github.com/owner/repo names=tool*,other
revoke FP reason
#sig MASTERFP HEXDER
```

- `serial N` is a counter that only goes up.
- `key FP HEXPOINT official` binds a key to the official repositories.
- `key FP HEXPOINT repo=URL names=GLOB,...` binds a key to one repository and
  to the package names matching the globs (shell-style patterns such as `tool*`).
- `revoke FP reason` revokes a key. A revoked key is never accepted.
- `FP` is the fingerprint of a key: the first 20 bytes of SHA-512 over its
  65-byte point, as 40 hexadecimal characters. `pkgsign.py fingerprint` prints it.
- `#sig MASTERFP HEXDER` lines are ECDSA signatures by master keys over every
  byte above the first `#sig` line.

The keyring is valid when at least `KeyringThreshold` different master keys
(from `/etc/pkg/keys/master/*.pub`) have signed it. A keyring that is not valid
is never used. If the installed keyring stops being valid, nothing verifies
until a valid one is installed.

`pkg -Sy` downloads `keyring.conf` from the registry, next to `registry.conf`,
and installs it only when it is valid and its `serial` is not lower than the
installed one. A lower serial is refused as a rollback and the old keyring stays.
The keyring is fetched only when `SigLevel` is not `Never`.

### Scopes

A key is trusted only for what the keyring says:

- Packages of official repositories, `registry.conf` and `repository.conf` must
  be signed by a key marked `official`.
- A package of a community repository must be signed by a key whose `repo` is
  that repository (case-insensitive, trailing slash and `.git` ignored) and whose
  `names` match the package name.
- A package of a `[trusted]` (local) repository may also be signed by a key in
  `/etc/pkg/keys/local/*.pub`.
- Community packages always need a valid signature unless `SigLevel = Never`.
- A community package whose name is the name of an official package is dropped
  with a warning. Official names are reserved.

A revoked key is recorded in the keyring. When the database is loaded, `pkg`
warns about installed packages signed by a revoked key, and `pkg -Qu` marks
them `REVOKED`.
`pkg -Qi` and `pkg -Si` show the signer and the origin of a package.

### Legacy mode

Without `/etc/pkg/keyring.conf`, every key in `/etc/pkg/keys/` is trusted for
everything. `pkg` warns once that the keys are not bound to repositories.
Keyring mode is the normal configuration.

### Signature files

A signature for a package is the release asset `NAME-VERSION-ARCH.pkg.tar.gz.sig`.
Registry files may carry their signature inside the file, as a trailing
`#sig HEX` line. A file that does not match its signature is refused, and the
last good copy is kept.

## Files and directories

| Path | Contents |
|---|---|
| `/etc/pkg/pkg.conf` | Configuration (fallback: `/etc/pkg.conf`). |
| `/etc/pkg/keys/`, `/etc/pkg/keys/master/`, `/etc/pkg/keys/local/` | Public keys, see [Trust model](#trust-model). |
| `/etc/pkg/keyring.conf` | Installed keyring. |
| `/var/lib/pkg/local/` | Local database: one directory per installed package, with its metadata, file list and `hooks/`. |
| `/var/lib/pkg/sync/` | Synced registry data: `registry.conf`, `repository.conf`, one `NAME.src` per known package, and HTTP cache files. |
| `/var/lib/pkg/db.lck` | Lock file holding the PID of the running `pkg`. A lock whose process no longer exists is removed automatically. If another `pkg` is running, the command stops. |
| `/var/cache/pkg/` | Downloaded package files and their signatures. `pkg -Sc` removes the ones that are not installed. |

## Building

The program is built inside the PureC OS tree. The top-level `Makefile` is in
this directory; the tree's `mk/toolchain.mk` is included from `ROOT_DIR`
(default `../..`).

| Target | Result |
|---|---|
| `make` (or `make all`) | Builds the `pkg` binary for PureC OS into the tree's build output (`BIN_DIR`). It needs the libraries of the tree built first: `make libraries kernel`, `make -C mk/netsurf zlib`, `make wolf`. |
| `make host` | Builds `build/host/pkg` for the development machine, linked with OpenSSL and zlib (`-lcrypto -lz`). Signature checks use OpenSSL. |
| `make test` | Builds the host binary and runs `tests/registry.sh` against it. Needs `python3` with the `cryptography` module. |
| `make package` | Builds `pkg-VERSION-PKGREL-x86_64.pkg.tar.gz` (`PKGREL` defaults to `1`) in the tree's repository output directory. If the file `SIGN_KEY` exists (default `~/.config/purec-pkg/signing.pem`), a signature `.sig` is written next to it. |
| `make image` | Runs `package`, then writes the local database entry for the OS image (`desc` and `files`) into the image output directory. |
| `make clean` | Removes the object files, `build/`, the binary and the image output. |

The package is built from `pkg.8` and the binary, installed as
`/usr/bin/pkg` and `/usr/share/man/pkg.8`.

For the development workflow and the rules for changes, see
[CONTRIBUTING.md](CONTRIBUTING.md).