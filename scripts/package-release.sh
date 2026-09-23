#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$repo_dir/build"}
version=${2:?usage: package-release.sh BUILD_DIR VERSION [OUTPUT_DIR]}
output_dir=${3:-"$repo_dir/dist"}
package_name="bettertop-${version}-linux-x86_64"
archive="$output_dir/${package_name}.tar.gz"
stage=$(mktemp -d "${TMPDIR:-/tmp}/bettertop-package.XXXXXX")
package_root="$stage/$package_name"
trap 'rm -rf "$stage"' EXIT

test -x "$build_dir/bettertop"
test -x "$build_dir/bettertop-gpu"
mkdir -p "$output_dir" "$package_root/bin" "$package_root/share/btop/themes" "$package_root/licenses"
install -m 755 "$build_dir/bettertop" "$package_root/bin/bettertop"
install -m 755 "$build_dir/bettertop-gpu" "$package_root/bin/bettertop-gpu"
cp -R "$repo_dir/themes/." "$package_root/share/btop/themes/"
cp "$repo_dir/README.md" "$repo_dir/THIRD_PARTY.md" "$repo_dir/LICENSE" "$package_root/"
cp "$repo_dir/src/bettertop_gpu/vendor/nvtop/LICENSE" "$package_root/licenses/LICENSE-nvtop-GPL-3.0-or-later"

tar -C "$stage" -czf "$archive" "$package_name"
(cd "$output_dir" && sha256sum "${package_name}.tar.gz") > "$output_dir/SHA256SUMS"
printf 'Created %s\n' "$archive"
