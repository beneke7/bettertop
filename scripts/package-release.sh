#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$repo_dir/build"}
version=${2:?usage: package-release.sh BUILD_DIR VERSION [PLATFORM [OUTPUT_DIR]]}
platform=${3:-linux-x86_64}
output_dir=${4:-"$repo_dir/dist"}
package_name="bettertop-${version}-${platform}"
archive="$output_dir/${package_name}.tar.gz"
stage=$(mktemp -d "${TMPDIR:-/tmp}/bettertop-package.XXXXXX")
package_root="$stage/$package_name"
trap 'rm -rf "$stage"' EXIT

test -x "$build_dir/bettertop"
mkdir -p "$output_dir" "$package_root/bin" "$package_root/share/btop/themes" "$package_root/licenses"
install -m 755 "$build_dir/bettertop" "$package_root/bin/bettertop"
if [ "$platform" = linux-x86_64 ]; then
	test -x "$build_dir/bettertop-gpu"
	install -m 755 "$build_dir/bettertop-gpu" "$package_root/bin/bettertop-gpu"
elif [ "$platform" != macos-arm64 ] && [ "$platform" != macos-x86_64 ]; then
	printf 'Unsupported release platform: %s\n' "$platform" >&2
	exit 1
fi
cp -R "$repo_dir/themes/." "$package_root/share/btop/themes/"
cp "$repo_dir/README.md" "$repo_dir/THIRD_PARTY.md" "$repo_dir/LICENSE" "$package_root/"
cp "$repo_dir/src/bettertop_gpu/vendor/nvtop/LICENSE" "$package_root/licenses/LICENSE-nvtop-GPL-3.0-or-later"

tar -C "$stage" -czf "$archive" "$package_name"
(
	cd "$output_dir"
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "${package_name}.tar.gz"
	else
		shasum -a 256 "${package_name}.tar.gz"
	fi
) > "$output_dir/SHA256SUMS"
printf 'Created %s\n' "$archive"
