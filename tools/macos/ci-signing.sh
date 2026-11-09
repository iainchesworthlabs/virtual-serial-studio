#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Release workflow's macOS signing step (docs/releasing.md). It signs and
# notarises the staged app and command line tool when the owner has turned
# signing on and supplied what it needs, and otherwise says the package is
# unsigned and succeeds, so a repository without the secrets still builds.
#
#   tools/macos/ci-signing.sh stage/vss_gui.app stage/bin/vss_cli
#
# Turned on by the repository variable SIGN_MACOS=true. It reads, from the
# environment:
#
#   SIGN_MACOS                 "true" to sign; anything else leaves the files unsigned
#   MACOS_SIGN_IDENTITY        the certificate's name, "Developer ID Application: ..."
#   MACOS_SIGN_CERT_BASE64     the certificate and private key as a base64 .p12
#   MACOS_SIGN_CERT_PASSWORD   the .p12 password
#   MACOS_NOTARY_KEY_BASE64    an App Store Connect API key (.p8) as base64
#   MACOS_NOTARY_KEY_ID        its key id
#   MACOS_NOTARY_ISSUER        its issuer id
#
# On success it puts VSS_SIGN_IDENTITY and VSS_NOTARY_PROFILE in GITHUB_ENV, so
# the later cpack step signs the copy of the app that goes into the disk image
# (cmake/packaging/VssPackage-darwin-sign.cmake). Nothing secret is printed.

set -euo pipefail

app="${1:?path of vss_gui.app}"
cli="${2:?path of vss_cli}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

summary() {
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    printf '\n### macOS signing skipped\n\n%s\n' "$1" >> "${GITHUB_STEP_SUMMARY}"
  fi
}

if [ "${SIGN_MACOS:-false}" != "true" ]; then
  echo "SIGN_MACOS is not true: the macOS app is not signed or notarised."
  summary "The macOS app and disk image are **unsigned and not notarised** (\`SIGN_MACOS\` is not \`true\`). Gatekeeper asks the user to allow the app on first launch."
  exit 0
fi

missing=""
for name in MACOS_SIGN_IDENTITY MACOS_SIGN_CERT_BASE64 MACOS_SIGN_CERT_PASSWORD \
            MACOS_NOTARY_KEY_BASE64 MACOS_NOTARY_KEY_ID MACOS_NOTARY_ISSUER; do
  if [ -z "${!name:-}" ]; then
    missing="${missing} ${name}"
  fi
done
if [ -n "${missing}" ]; then
  reason="SIGN_MACOS is true, but these are not available to this run (not created yet, or a pull request from a fork):${missing}. The macOS app and disk image are left unsigned and not notarised."
  echo "::warning title=macOS signing skipped::${reason}"
  summary "${reason}"
  exit 0
fi

work="$(mktemp -d)"
keychain="${work}/vss-signing.keychain-db"
trap 'rm -f "${work}/cert.p12" "${work}/notary.p8"' EXIT
keychain_password="$(uuidgen)"

security create-keychain -p "${keychain_password}" "${keychain}"
security set-keychain-settings -lut 21600 "${keychain}"
security unlock-keychain -p "${keychain_password}" "${keychain}"
printf '%s' "${MACOS_SIGN_CERT_BASE64}" | base64 --decode > "${work}/cert.p12"
security import "${work}/cert.p12" -P "${MACOS_SIGN_CERT_PASSWORD}" \
  -A -t cert -f pkcs12 -k "${keychain}"
security set-key-partition-list -S apple-tool:,apple: -s \
  -k "${keychain_password}" "${keychain}" > /dev/null
# codesign finds the certificate through the search list.
# shellcheck disable=SC2046
security list-keychains -d user -s "${keychain}" $(security list-keychains -d user | tr -d '"')

printf '%s' "${MACOS_NOTARY_KEY_BASE64}" | base64 --decode > "${work}/notary.p8"
xcrun notarytool store-credentials vss-notary \
  --key "${work}/notary.p8" --key-id "${MACOS_NOTARY_KEY_ID}" \
  --issuer "${MACOS_NOTARY_ISSUER}" --keychain "${keychain}" > /dev/null

export VSS_SIGN_IDENTITY="${MACOS_SIGN_IDENTITY}"
export VSS_NOTARY_PROFILE="vss-notary"
"${here}/sign-and-notarize.sh" --app "${app}" --cli "${cli}"

if [ -n "${GITHUB_ENV:-}" ]; then
  {
    echo "VSS_SIGN_IDENTITY=${VSS_SIGN_IDENTITY}"
    echo "VSS_NOTARY_PROFILE=${VSS_NOTARY_PROFILE}"
  } >> "${GITHUB_ENV}"
fi
