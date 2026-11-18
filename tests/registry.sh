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
mk tpkg 1.2-1; mk tdup 1.0-1
mkdir -p $T/bin/tpkg $T/bin/tdup
cp $REL/tpkg-1.2-1-x86_64.pkg.tar.gz $T/bin/tpkg/
cp $REL/tdup-1.0-1-x86_64.pkg.tar.gz $T/bin/tdup/
cat > $T/repository.conf <<R
# packages stored in the registry repository
tpkg bin/tpkg/tpkg-1.2-1-x86_64.pkg.tar.gz Trusted test package
tdup bin/tdup/tdup-1.0-1-x86_64.pkg.tar.gz
tdup bin/tdup/tdup-1.0-1-x86_64.pkg.tar.gz
wrong bin/tpkg/tpkg-1.2-1-x86_64.pkg.tar.gz
evil ../../etc/evil-1.0-1-x86_64.pkg.tar.gz
foo bin/tpkg/tpkg-1.2-1-x86_64.pkg.tar.gz
R
$P -Sy > $T/sy4.log 2>&1
check "repository.conf lists packages" "grep -q 'tpkg 1.2-1' $T/sy4.log && grep -q 'tdup 1.0-1' $T/sy4.log"
check "repository.conf bad entries ignored" "grep -q 'repository.conf:6' $T/sy4.log && ! test -f $ROOT/var/lib/pkg/sync/wrong.src && ! test -f $ROOT/var/lib/pkg/sync/evil.src"
check "repository.conf duplicate skipped" "grep -q 'tdup is listed twice' $T/sy4.log"
check "package from repository.conf wins" "grep -q 'repo=registry' $ROOT/var/lib/pkg/sync/tpkg.src && grep -q 'url=file://$T/bin/tpkg/' $ROOT/var/lib/pkg/sync/tpkg.src"
check "install from repository.conf" "$P -S tpkg >/dev/null 2>&1 && test -f $ROOT/usr/share/tpkg.txt"
check "-Sl shows repository.conf" "$P -Sl 2>&1 | grep -A3 '^repository.conf' | grep -q 'tpkg'"
sed -i '/^tdup/d' $T/repository.conf
$P -Sy >/dev/null 2>&1
check "removed from repository.conf leaves sync" "! test -f $ROOT/var/lib/pkg/sync/tdup.src"
sed 's/SigLevel = Never/SigLevel = Required/' $T/pkg.conf > $T/pkg-req.conf
check "Required refuses unsigned repository.conf" "! $PKG --root $ROOT --config $T/pkg-req.conf --noconfirm -Sy >/dev/null 2>&1"
rm -f $T/repository.conf
$P -Sy > $T/sy5.log 2>&1
check "missing repository.conf keeps old entries" "grep -q 'not available' $T/sy5.log && test -f $ROOT/var/lib/pkg/sync/tpkg.src"
printf '# empty\n' > $T/registry.conf
mk emptyreg 1.0-1; mkdir -p $T/bin/emptyreg; cp $REL/emptyreg-1.0-1-x86_64.pkg.tar.gz $T/bin/emptyreg/
printf 'emptyreg bin/emptyreg/emptyreg-1.0-1-x86_64.pkg.tar.gz\n' > $T/repository.conf
check "empty registry.conf works with repository.conf" "$P -Sy > $T/sy6.log 2>&1 && grep -q 'emptyreg 1.0-1' $T/sy6.log && $P -S emptyreg >/dev/null 2>&1 && test -f $ROOT/usr/share/emptyreg.txt"
KEYDIR=$T/keys; mkdir -p $KEYDIR $T/sroot/etc/pkg/keys $T/sreg
python3 $MK/pkgsign.py genkey $KEYDIR/k.pem $KEYDIR/k.pub >/dev/null 2>&1
cp $KEYDIR/k.pub $T/sroot/etc/pkg/keys/test.pub
mk signed 1.0-1
python3 $MK/pkgsign.py sign $KEYDIR/k.pem $REL/signed-1.0-1-x86_64.pkg.tar.gz $REL/signed-1.0-1-x86_64.pkg.tar.gz.sig >/dev/null 2>&1
printf '# empty\n' > $T/sreg/registry.conf
python3 $MK/publish.py --registry $T/sreg --key $KEYDIR/k.pem $REL/signed-1.0-1-x86_64.pkg.tar.gz >/dev/null 2>&1
printf '[options]\nSigLevel = Required\nRegistry = file://%s/registry.conf\n' $T/sreg > $T/spkg.conf
SP="$PKG --root $T/sroot --config $T/spkg.conf --noconfirm"
check "embedded signature accepted with Required" "$SP -Sy > $T/ss1.log 2>&1 && grep -q 'signed 1.0-1' $T/ss1.log"
check "embedded signature: no .sig file needed" "! test -f $T/sreg/repository.conf.sig && ! test -f $T/sreg/registry.conf.sig"
check "legacy mode warns about unbound keys" "grep -q 'not bound to repositories' $T/ss1.log"
check "signed package installs" "$SP -S signed >/dev/null 2>&1 && test -f $T/sroot/usr/share/signed.txt"
sed -i 's/^signed /signed2 /' $T/sreg/repository.conf
check "tampered repository.conf refused" "! $SP -Sy > $T/ss2.log 2>&1 && grep -q 'signature is invalid' $T/ss2.log"
rm -f $T/sreg/repository.conf
printf 'signed bin/signed/signed-1.0-1-x86_64.pkg.tar.gz\n' > $T/sreg/repository.conf
check "unsigned repository.conf refused with Required" "! $SP -Sy > $T/ss3.log 2>&1 && grep -q 'no signature' $T/ss3.log"

PS="python3 $MK/pkgsign.py"
KD=$T/kr; mkdir -p $KD $T/kroot/etc/pkg/keys/master $T/kreg $T/kapi $T/crel
$PS genkey $KD/o.pem $KD/o.pub >/dev/null 2>&1
$PS genkey $KD/c.pem $KD/c.pub >/dev/null 2>&1
$PS genkey $KD/x.pem $KD/x.pub >/dev/null 2>&1
$PS genkey $KD/m1.pem $KD/m1.pub >/dev/null 2>&1
$PS genkey $KD/m2.pem $KD/m2.pub >/dev/null 2>&1
FPO=$($PS fingerprint $KD/o.pub); FPC=$($PS fingerprint $KD/c.pub)
cp $KD/m1.pub $T/kroot/etc/pkg/keys/master/m1.pub
cp $KD/o.pub $T/kroot/etc/pkg/keys/official.pub
signpkg() { python3 $MK/pkgsign.py sign $1 $REL/$2-x86_64.pkg.tar.gz $REL/$2-x86_64.pkg.tar.gz.sig; }
mk offpkg 1.0-1; mk rogue 1.0-1; mk ctool 1.0-1; mk other 1.0-1; mk offpkg 2.0-1
mv $REL/offpkg-2.0-1-x86_64.pkg.tar.gz $REL/offpkg-2.0-1-x86_64.pkg.tar.gz
signpkg $KD/o.pem offpkg-1.0-1; signpkg $KD/x.pem rogue-1.0-1
signpkg $KD/c.pem ctool-1.0-1; signpkg $KD/c.pem other-1.0-1; signpkg $KD/c.pem offpkg-2.0-1
$GH com/tools v1 $REL/ctool-1.0-1-x86_64.pkg.tar.gz $REL/ctool-1.0-1-x86_64.pkg.tar.gz.sig $REL/other-1.0-1-x86_64.pkg.tar.gz $REL/other-1.0-1-x86_64.pkg.tar.gz.sig $REL/offpkg-2.0-1-x86_64.pkg.tar.gz $REL/offpkg-2.0-1-x86_64.pkg.tar.gz.sig
mkdir -p $T/kapi; cp -r $T/api/repos $T/kapi/
printf '# registry\ncommunity https://github.com/com/tools\n' > $T/kreg/registry.conf
python3 $MK/publish.py --registry $T/kreg --key $KD/o.pem $REL/offpkg-1.0-1-x86_64.pkg.tar.gz $REL/rogue-1.0-1-x86_64.pkg.tar.gz >/dev/null 2>&1
mkkr() {
    { echo "# keyring"; echo "serial $1"; $PS keyring-entry $KD/o.pub official
      $PS keyring-entry $KD/c.pub repo=https://github.com/Com/Tools.git names='ctool*,offpkg'; shift; for l in "$@"; do echo "$l"; done; } > $T/kreg/keyring.conf
    $PS keyring-sign --key $KD/m1.pem $T/kreg/keyring.conf >/dev/null
}
mkkr 1
printf '[options]\nSigLevel = Required\nAllowCommunity = yes\nAPI = file://%s/kapi\nRegistry = file://%s/kreg/registry.conf\n' $T $T > $T/kpkg.conf
KP="$PKG --root $T/kroot --config $T/kpkg.conf --noconfirm"
$KP -Sy > $T/k1.log 2>&1
check "keyring accepted on refresh" "grep -q 'keyring.conf *serial 1' $T/k1.log && test -f $T/kroot/etc/pkg/keyring.conf"
check "keyring-verify tool" "$PS keyring-verify --masters $KD/ $T/kreg/keyring.conf >/dev/null 2>&1 || { mkdir -p $T/mdir; cp $KD/m1.pub $T/mdir/; $PS keyring-verify --masters $T/mdir $T/kreg/keyring.conf >/dev/null; }"
check "community listed and marked" "$KP -Ss ctool 2>&1 | grep -q 'ctool 1.0-1 \[community\]' && $KP -Si ctool 2>&1 | grep -q 'community, third-party'"
check "official package accepted with keyring" "$KP -S offpkg > $T/k2.log 2>&1 && test -f $T/kroot/usr/share/offpkg.txt"
check "signer and origin recorded" "$KP -Qi offpkg 2>&1 | grep -q \"Signed by *: $FPO\" && $KP -Qi offpkg 2>&1 | grep -q 'Origin *: official'"
check "community name shadowing official dropped" "grep -q 'community package offpkg has the name of an official package' $T/k1.log && grep -q 'repo=registry' $T/kroot/var/lib/pkg/sync/offpkg.src"
check "key outside the keyring rejected" "! $KP -S rogue > $T/k3.log 2>&1 && grep -q 'not trusted' $T/k3.log && ! test -f $T/kroot/usr/share/rogue.txt"
check "community key accepted for its repo and names" "$KP -S ctool > $T/k4.log 2>&1 && test -f $T/kroot/usr/share/ctool.txt && grep -q 'community package' $T/k4.log"
check "community signer recorded" "$KP -Qi ctool 2>&1 | grep -q \"Signed by *: $FPC\" && $KP -Qi ctool 2>&1 | grep -q 'Origin *: community'"
check "community key refused outside its names" "! $KP -S other > $T/k5.log 2>&1 && grep -q 'no trusted key' $T/k5.log && ! test -f $T/kroot/usr/share/other.txt"
printf '[options]\nSigLevel = Required\nAPI = file://%s/kapi\nRegistry = file://%s/kreg/registry.conf\n' $T $T > $T/kpkg-nc.conf
$PKG --root $T/kroot --config $T/kpkg-nc.conf --noconfirm -Sy > $T/k6.log 2>&1
check "AllowCommunity = no ignores community" "grep -q '1 community repositories are ignored' $T/k6.log && ! test -f $T/kroot/var/lib/pkg/sync/ctool.src"
mkkr 1 "revoke $FPC test revocation"
$KP -Sy > $T/k7.log 2>&1
check "revoked key: installed package flagged" "grep -q 'signed with a revoked key: ctool' $T/k7.log && $KP -Qu 2>&1 | grep -q 'ctool 1.0-1 REVOKED'"
check "revoked key rejected" "! $KP -S --force ctool >/dev/null 2>&1 && $KP -Qi ctool 2>&1 | grep -q '(REVOKED)'"
mkkr 0
$KP -Sy > $T/k8.log 2>&1
check "keyring serial rollback rejected" "grep -q 'older than the installed' $T/k8.log && grep -q '^serial 1$' $T/kroot/etc/pkg/keyring.conf"
cp $T/kroot/etc/pkg/keyring.conf $T/kreg/keyring.conf
sed -i 's/^serial 1$/serial 7/' $T/kreg/keyring.conf
$KP -Sy > $T/k9.log 2>&1
check "tampered keyring refused" "grep -q 'valid master signature' $T/k9.log && grep -q '^serial 1$' $T/kroot/etc/pkg/keyring.conf"
mkdir -p $T/kroot2/etc/pkg/keys/master $T/kreg2
cp $KD/m1.pub $KD/m2.pub $T/kroot2/etc/pkg/keys/master/
cp $T/kreg/registry.conf $T/kreg/repository.conf $T/kreg2/
{ echo "serial 1"; $PS keyring-entry $KD/o.pub official; } > $T/kreg2/keyring.conf
$PS keyring-sign --key $KD/m1.pem $T/kreg2/keyring.conf >/dev/null
printf '[options]\nSigLevel = Required\nKeyringThreshold = 2\nAPI = file://%s/kapi\nRegistry = file://%s/kreg2/registry.conf\n' $T $T > $T/kpkg2.conf
K2="$PKG --root $T/kroot2 --config $T/kpkg2.conf --noconfirm"
$K2 -Sy > $T/k10.log 2>&1
check "threshold 2 with one master signature refused" "grep -q '1 valid master signature(s), 2 required' $T/k10.log && ! test -f $T/kroot2/etc/pkg/keyring.conf"
$PS keyring-sign --key $KD/m2.pem $T/kreg2/keyring.conf >/dev/null
$K2 -Sy > $T/k11.log 2>&1
check "threshold 2 with two master signatures accepted" "grep -q 'keyring.conf *serial 1' $T/k11.log && test -f $T/kroot2/etc/pkg/keyring.conf"
echo "RESULT fails=$fail dir=$T"
exit $fail
