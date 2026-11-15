#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The `verify-script` of the shared publish workflow, run as `bash <this> <dist> <public key>`
# on the signed and attested files, before the release is created. It checks them with
# release_verify.py, which says what it looks for.

set -euo pipefail

exec python3 "$(dirname "${BASH_SOURCE[0]}")/release_verify.py" "$@"
