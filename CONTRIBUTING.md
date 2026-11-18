# Contributing to purec-os-pkg

Thank you for helping with `pkg`, the PureC OS package manager. This document
describes how to build and test the code, the conventions it follows, and what
a change needs before it is merged. The user-facing behaviour is described in
[README.md](README.md) and [pkg.8](pkg.8).

## Building and testing

The program lives inside the PureC OS tree. Build the tree's libraries first
(see [Building](README.md#building)), then:

```
make host      # build/host/pkg, linked with OpenSSL and zlib
make test      # build the host binary and run tests/registry.sh
```

`make test` needs `python3` with the `cryptography` module, because
`mk/pkg/pkgsign.py` and `mk/pkg/publish.py` are used to create keys, sign
packages and build registries. The test script works in a temporary directory
and does not touch the system.

A passing run ends with a line of this form:

```
RESULT fails=0 dir=/tmp/...
```

Every test prints `ok` or `FAIL`. Any change must leave `fails=0`. Run the
tests on the host before you send a pull request. If your change affects the
PureC OS build (`make`), say in the pull request whether you built and ran it
there.

To run a single check by hand, use the same environment as `make test`:

```
PKG=$PWD/build/host/pkg MK=$PWD/../../mk/pkg tests/registry.sh
```

## Code style

- C, with 4-space indentation and no tabs.
- No comments in the code. Names and small functions should explain the code.
- Small, focused functions. A function does one step of the operation.
- Follow the style of the file you are editing: `snprintf` with explicit buffer
  sizes, `die` for fatal errors, `warn` for warnings, `rp()` for every path
  under the root directory.
- Keep both signature back ends compiling. The host build defines
  `PKG_SIG_OPENSSL` and uses OpenSSL. The PureC OS build uses wolfSSL through
  `src/sig.c` (`#ifdef PKG_SIG_OPENSSL` and its `#else` branch). A change to
  verification must be built and tested in both configurations. If you cannot
  build the target configuration, say so in the pull request.
- Compile with `-Wall` cleanly. `make host` uses `-Wall`; new warnings are not
  acceptable.

## Changes to signature verification

Signature handling is the part of `pkg` that protects users from malicious
packages, so these rules are strict.

- Every new accept or reject path needs a test in `tests/registry.sh`. A
  rejected signature should be tested for the reason it is rejected (for
  example, the message that the test expects in the log).
- Never weaken the legacy fallback (no `keyring.conf`, keys in `/etc/pkg/keys`)
  silently. Any change to it must be documented in `pkg.8` and `README.md`, and
  must print a warning that the user can see.
- A keyring that is not valid must never be used. A downgrade of the keyring
  `serial` must stay refused.
- Do not add a path that accepts a package when a signature exists but fails
  to verify.
- Keep the scope rules intact: official keys for official repositories, a key
  bound to its repository and name globs for community packages, revoked keys
  never accepted.
- Keys, signatures and test material must never be committed. Test keys are
  generated in the temporary directory by the test script.

## Documentation

`pkg.8` and `README.md` describe the same behaviour. When a change alters an
option, an operation, a configuration key, a file path, a default or the trust
model, update both files in the same commit. The usage text printed by
`pkg --help` (in `src/main.c`) must match the operations and options in
`pkg.8` and in the README command table.

## Versioning

The version is in the `VERSION` file (`MAJOR.MINOR.PATCH`). It is read by the
Makefile and printed by `pkg -v`. Bump it when a change alters user-visible
behaviour: a new option, a changed default, a changed file format or a fix that
users should notice. Put the bump in the same commit as the change, or in a
separate `Bump version to X.Y.Z` commit when several changes are released
together.

## Commit messages

- Subject: a short English imperative sentence, for example
  `Add embedded signature verification for downloaded packages`. Keep it
  under about 70 characters, with no trailing period.
- Body: explain what changed and why, in full sentences, wrapped at about 72
  columns. Mention the affected operations, options or files, and any
  behaviour change that users will see. Mention the tests that cover the change.

## Pull request checklist

Before you open a pull request, check that:

- [ ] `make host` builds without warnings.
- [ ] `make test` prints `RESULT fails=0`.
- [ ] New or changed signature paths have tests in `tests/registry.sh`.
- [ ] `pkg.8` and `README.md` are updated for any behaviour change.
- [ ] `VERSION` is bumped when user-visible behaviour changed.
- [ ] No comments were added to the code, and the style matches the file.
- [ ] The wolfSSL and OpenSSL signature paths both still compile, or the pull
  request says which one was not built.
- [ ] The commit messages follow the style above.
- [ ] No keys, tokens, signatures of real packages or machine-specific
  identifiers are included in the change.

## Security reports

Do not report security problems in public issues or pull requests. Report them
privately through GitHub security advisories:
<https://github.com/PureC-OS/purec-os-pkg/security/advisories/new>.

Include the version (`pkg -v`), the configuration (`SigLevel`, keyring state,
without any private key), the steps to reproduce, and what an attacker can
do. Please give the maintainers time to work on a fix before any public
disclosure.

## Third-party repositories and packages

Third-party package repositories are not added to this repository. They are
onboarded through the PureC OS registry, which holds `registry.conf`,
`repository.conf` and `keyring.conf`:
<https://github.com/PureC-OS/PureC-OS-Trusted-PKG-URL>. Follow the instructions
in that repository's `CONTRIBUTING.md`. Community repositories need a
`community` line and a key bound to the repository and its package names, signed
by the maintainers.

## License

By contributing you agree that your changes are published under the license in
[LICENSE](LICENSE).
