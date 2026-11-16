#!/bin/bash
set -u
T=$(mktemp -d)
MK=${MK:-$(cd "$(dirname "$0")/../../../mk/pkg" && pwd)}
GH="python3 $(dirname "$0")/ghmock.py $T/api"
PKG=${PKG:-$(dirname "$0")/../build/host/pkg}
REL=$T/rel; ROOT=$T/root; SRC=$T/src
mkdir -p $REL $ROOT $SRC
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=$((fail+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1 :: $2"; fi; }
mk() { printf "$1\n" > $SRC/$1.txt; python3 $MK/mkpkg.py --name $1 --version $2 ${3:+--depend $3} --file usr/share/$1.txt=$SRC/$1.txt -o $REL >/dev/null; }
mk libfoo 1.0-1; mk foo 1.0-1 "libfoo>=1.0"; mk alpha 2.0-1; mk beta 3.0-1; mk dup 1.0-1; mk localpkg 0.5-1; mk gone 1.0-1
$GH me/foo v1.0 $REL/foo-1.0-1-x86_64.pkg.tar.gz
$GH me/libfoo v1.0 $REL/libfoo-1.0-1-x86_64.pkg.tar.gz
$GH me/multi v2 $REL/alpha-2.0-1-x86_64.pkg.tar.gz $REL/beta-3.0-1-x86_64.pkg.tar.gz $REL/gone-1.0-1-x86_64.pkg.tar.gz
cp $REL/dup-1.0-1-x86_64.pkg.tar.gz $REL/dup-copy.tgz
$GH me/dupper v1 $REL/dup-1.0-1-x86_64.pkg.tar.gz $REL/foo-1.0-1-x86_64.pkg.tar.gz
$GH me/localrepo v0.5 $REL/localpkg-0.5-1-x86_64.pkg.tar.gz
mkdir -p $T/api/repos/me/empty; echo '[]' > "$T/api/repos/me/empty/releases?per_page=30"
cat > $T/registry.conf <<R
# official repositories
https://github.com/me/foo
https://github.com/me/libfoo.git
https://github.com/me/multi
https://github.com/me/dupper
https://github.com/me/empty
R
cat > $T/pkg.conf <<CONF
[options]
SigLevel = Never
API = file://$T/api
Registry = file://$T/registry.conf
[trusted]
https://github.com/me/localrepo
CONF
P="$PKG --root $ROOT --config $T/pkg.conf --noconfirm"
$P -Sy > $T/sy.log 2>&1
check "refresh lists packages" "grep -q 'alpha 2.0-1, beta 3.0-1' $T/sy.log"
check "registry cached" "test -f $ROOT/var/lib/pkg/sync/registry.conf"
check "duplicate name skipped" "grep -q 'package foo is already provided' $T/sy.log && grep -q 'repo=me/foo' $ROOT/var/lib/pkg/sync/foo.src"
check "empty repo reported" "grep -q 'me/empty.*no releases' $T/sy.log"
check "local ignored without flag" "grep -q 'AllowLocalRepos' $T/sy.log && ! test -f $ROOT/var/lib/pkg/sync/localpkg.src"
check "install with dependency" "$P -S foo > $T/i.log 2>&1 && test -f $ROOT/usr/share/foo.txt && test -f $ROOT/usr/share/libfoo.txt"
check "install from multi repo" "$P -S beta >/dev/null 2>&1 && test -f $ROOT/usr/share/beta.txt"
check "url single package" "$P -S --needed https://github.com/me/libfoo >/dev/null 2>&1"
check "url multi package refused" "! $P -S https://github.com/me/multi > $T/u.log 2>&1 && grep -q 'provides 3 packages' $T/u.log"
check "url outside registry refused" "! $P -S https://github.com/me/localrepo > $T/u2.log 2>&1 && grep -q 'not in the registry' $T/u2.log"
check "list" "$P -Sl 2>&1 | grep -q 'https://github.com/me/multi' && $P -Sl 2>&1 | grep -q '    alpha'"
sed -i 's/SigLevel = Never/SigLevel = Never\nAllowLocalRepos = yes/' $T/pkg.conf
$P -Sy > $T/sy2.log 2>&1
check "local allowed with flag" "grep -q 'localpkg 0.5-1  (local)' $T/sy2.log"
check "local marked in -Si" "$P -Si localpkg 2>&1 | grep -q 'local, not from the registry'"
check "install local" "$P -S localpkg >/dev/null 2>&1 && test -f $ROOT/usr/share/localpkg.txt"
rm -rf $T/api/repos/me/multi; $GH me/multi v3 $REL/alpha-2.0-1-x86_64.pkg.tar.gz $REL/beta-3.0-1-x86_64.pkg.tar.gz
$P -Sy >/dev/null 2>&1
check "package gone from repo removed from sync" "! test -f $ROOT/var/lib/pkg/sync/gone.src"
sed -i '/me\/multi/d' $T/registry.conf
$P -Sy >/dev/null 2>&1
check "repo dropped from registry" "! test -f $ROOT/var/lib/pkg/sync/alpha.src && ! $P -S alpha >/dev/null 2>&1"
mkdir -p $T/api/repos/me/down
rm -rf "$T/api/repos/me/libfoo"
$P -Sy > $T/sy3.log 2>&1; rc=$?
check "unreachable repo keeps old entries" "[ $rc -ne 0 ] && test -f $ROOT/var/lib/pkg/sync/libfoo.src"
check "upgrade continues" "$P -Syu > $T/up.log 2>&1; grep -q 'continuing with the others' $T/up.log"
echo "RESULT fails=$fail dir=$T"
exit $fail
