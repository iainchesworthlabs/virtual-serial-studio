#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The `pre-release-script` of the shared publish workflow, run as `bash <this> <dist> <version> <tag>`
# once the packages are downloaded and before SHA512SUMS is written. It adds the SBOM to <dist>, so
# the sums, the signatures and the attestations cover it like the packages (docs/releasing.md).

set -euo pipefail

dist=${1:?usage: release_prepare.sh <dist> <version> <tag>}
version=${2:?usage: release_prepare.sh <dist> <version> <tag>}
tag=${3:?usage: release_prepare.sh <dist> <version> <tag>}

python3 "$(dirname "${BASH_SOURCE[0]}")/release_sbom.py" --version "$version" --tag "$tag" --out "$dist/sbom.spdx.json"
