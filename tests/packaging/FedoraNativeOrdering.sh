#!/usr/bin/env bash
# Exercise our upgrade boundary with Fedora RPM's actual version comparator.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
source "$root/packaging/scripts/check-fedora.sh"
for pair in '1.2.3-rc.2 1.2.3-rc.10' '1.2.3-rc.10 1.2.3' '1.2.3 1.10.0'; do
    read -r old new <<<"$pair"
    old="$("$root/packaging/scripts/native-version.sh" "$old")-1.fc44"
    new="$("$root/packaging/scripts/native-version.sh" "$new")-1.fc44"
    older "$old" "$new" || die "rejected older $old -> $new"
    if older "$new" "$old"; then die "accepted downgrade $new -> $old"; fi
    if older "$old" "$old"; then die "accepted equal version $old"; fi
done
older '1.2.3-1.fc44' '1.2.3-2.fc44' || die 'native release revision ordering was lost'
echo 'all Fedora upgrade ordering checks passed'
