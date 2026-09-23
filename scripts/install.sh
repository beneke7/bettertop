#!/bin/sh
set -eu

repo=beneke7/bettertop
system=$(uname -s)
machine=$(uname -m)
case "$system:$machine" in
	Linux:x86_64|Linux:amd64) platform=linux-x86_64 ;;
	Darwin:arm64) platform=macos-arm64 ;;
	Darwin:x86_64) platform=macos-x86_64 ;;
	*) printf 'Unsupported platform: %s %s\n' "$system" "$machine" >&2; exit 1 ;;
esac

tmp=$(mktemp -d "${TMPDIR:-/tmp}/bettertop-install.XXXXXX")
trap 'rm -rf "$tmp"' 0
trap 'exit 1' HUP INT TERM
curl -fsSL "https://api.github.com/repos/$repo/releases/latest" -o "$tmp/release.json"
tag=$(sed -n 's/^[[:space:]]*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' "$tmp/release.json" | sed -n '1p')
if [ -z "$tag" ]; then
	printf 'Could not find the latest BetterTop release.\n' >&2
	exit 1
fi
version=${tag#v}
asset="bettertop-${version}-${platform}.tar.gz"
release_url="https://github.com/$repo/releases/download/$tag"
curl -fsSL "$release_url/SHA256SUMS" -o "$tmp/SHA256SUMS"
curl -fsSL "$release_url/$asset" -o "$tmp/$asset"
expected=$(awk -v asset="$asset" '$2 == asset { print $1; exit }' "$tmp/SHA256SUMS")
if command -v sha256sum >/dev/null 2>&1; then
	actual=$(sha256sum "$tmp/$asset")
elif command -v shasum >/dev/null 2>&1; then
	actual=$(shasum -a 256 "$tmp/$asset")
else
	printf 'Need sha256sum or shasum to verify the download.\n' >&2
	exit 1
fi
actual=${actual%% *}
if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
	printf 'Checksum verification failed for %s.\n' "$asset" >&2
	exit 1
fi

mkdir "$tmp/unpacked"
tar -xzf "$tmp/$asset" -C "$tmp/unpacked"
package="$tmp/unpacked/bettertop-${version}-${platform}"
if [ ! -x "$package/bin/bettertop" ]; then
	printf 'The BetterTop archive is incomplete.\n' >&2
	exit 1
fi
if [ "$platform" = linux-x86_64 ] && [ ! -x "$package/bin/bettertop-gpu" ]; then
	printf 'The Linux archive is missing bettertop-gpu.\n' >&2
	exit 1
fi

prefix=${BETTERTOP_PREFIX:-"$HOME/.local"}
mkdir -p "$prefix/bin" "$prefix/share/btop/themes" "$prefix/share/doc/bettertop"
install -m 755 "$package/bin/bettertop" "$prefix/bin/bettertop"
if [ -x "$package/bin/bettertop-gpu" ]; then
	install -m 755 "$package/bin/bettertop-gpu" "$prefix/bin/bettertop-gpu"
fi
cp -R "$package/share/btop/themes/." "$prefix/share/btop/themes/"
cp "$package/README.md" "$package/THIRD_PARTY.md" "$package/LICENSE" "$prefix/share/doc/bettertop/"
cp "$package/licenses/LICENSE-nvtop-GPL-3.0-or-later" "$prefix/share/doc/bettertop/"
printf 'Installed BetterTop %s to %s/bin/bettertop\n' "$version" "$prefix"
case ":${PATH:-}:" in
	*":$prefix/bin:"*) printf 'Run: bettertop\n' ;;
	*) printf 'Add %s/bin to PATH, or run %s/bin/bettertop\n' "$prefix" "$prefix" ;;
esac
