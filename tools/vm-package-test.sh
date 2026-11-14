#!/usr/bin/env bash
set -euo pipefail
mode=${1:-}
[[ $mode == --plan || $mode == --install ]] || { echo 'Usage: vm-package-test.sh --plan first.deb | --install first.deb upgrade.deb' >&2; exit 2; }
source /etc/os-release
[[ $ID == ubuntu && $VERSION_ID == 26.04 ]] || { echo 'Ubuntu 26.04 VM required.' >&2; exit 2; }
systemd-detect-virt --vm --quiet || { echo 'Refusing non-VM environment.' >&2; exit 2; }
first=$(realpath -- "${2:?first deb required}")
package=$(dpkg-deb -f "$first" Package)
[[ $package == keepassxc-gnome-development || $package == keepassxc-gnome ]] || exit 2
if [[ $mode == --plan ]]; then
    exec apt-get --simulate --no-remove install "$first"
fi
[[ $EUID == 0 ]] || { echo 'Run installation test with sudo inside the VM.' >&2; exit 2; }
second=$(realpath -- "${3:?upgrade deb required}")
[[ $(dpkg-deb -f "$second" Package) == "$package" ]] || exit 2
dpkg --compare-versions "$(dpkg-deb -f "$second" Version)" gt "$(dpkg-deb -f "$first" Version)" || exit 2
if dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null | grep -q installed; then
    echo 'Target package already installed; use a disposable baseline VM.' >&2; exit 2
fi
record=$(mktemp -d /tmp/keepass-gnome-package.XXXXXX)
exec > >(tee "$record/result.log") 2>&1
printf 'Record: %s\n' "$record"
check_original() {
    dpkg-query -W -f='${Package} ${Version} ${db:Status-Status}\n' keepassxc 2>/dev/null || true
    if dpkg-query -W keepassxc >/dev/null 2>&1; then dpkg --verify keepassxc || true; fi
}
check_original > "$record/original.before"
mkdir -p "$record/config/keepassxc-gnome" "$record/state/keepassxc-gnome/recovery"
printf 'synthetic settings sentinel\n' > "$record/config/keepassxc-gnome/sentinel"
printf 'synthetic state sentinel\n' > "$record/state/keepassxc-gnome/recovery/sentinel"
sha256sum "$record"/{config/keepassxc-gnome,state/keepassxc-gnome/recovery}/sentinel > "$record/sentinels"
apt-get --yes --no-remove install "$first"
[[ $(dpkg-query -W -f='${Version}' "$package") == "$(dpkg-deb -f "$first" Version)" ]]
env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH keepassxc-gnome --version
env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH LC_ALL=C.UTF-8 LANGUAGE=de keepassxc-gnome --language-check | grep -Fx Ready
for item in keepassxc-gnome keepassxc-gnome-worker keepassxc-gnome-picker; do
    [[ -x /usr/libexec/keepassxc-gnome/$item ]]
    libraries=$(env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH ldd "/usr/libexec/keepassxc-gnome/$item")
    ! grep -q 'not found' <<< "$libraries"
done
[[ -f /usr/share/locale/ja/LC_MESSAGES/keepassxc-gnome.mo ]]
desktop-file-validate /usr/share/applications/org.keepassxc.Gnome.Development.desktop
if [[ -n ${KPXC_VM_DOMAIN_TEST:-} && -n ${KPXC_VM_REFERENCE_TOOL:-} ]]; then
    env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH XDG_CONFIG_HOME="$record/config" XDG_CACHE_HOME="$record/cache" XDG_STATE_HOME="$record/state" \
        "$KPXC_VM_DOMAIN_TEST" "$KPXC_VM_REFERENCE_TOOL" /usr/libexec/keepassxc-gnome/keepassxc-gnome-worker
fi
apt-get --yes --no-remove install "$second"
[[ $(dpkg-query -W -f='${Version}' "$package") == "$(dpkg-deb -f "$second" Version)" ]]
dpkg --verify "$package"
sha256sum --check "$record/sentinels"
apt-get --yes purge "$package"
[[ ! -e /usr/bin/keepassxc-gnome && ! -L /usr/bin/keepassxc-gnome ]]
[[ ! -e /usr/libexec/keepassxc-gnome/keepassxc-gnome-worker ]]
sha256sum --check "$record/sentinels"
check_original > "$record/original.after"
cmp "$record/original.before" "$record/original.after"
printf 'PASS VM install, upgrade, purge, original package state unchanged and synthetic sentinels retained\n'
