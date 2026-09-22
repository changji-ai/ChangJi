#!/usr/bin/env bash
# Set, in one go, the six GitHub secrets this repository's pipelines need.
#
#     bash tools/setup_signing_secrets.sh
#
# ⚠️ **You run this yourself. Not an assistant.**
#
# The password, the Issuer ID and the access token are **typed in here and
# now**: `read -s` does not echo, the variables live only in this process, they
# are never written to a file and never enter shell history. Have somebody else
# type them — including an assistant that records its output — and all three
# are in a transcript, and transcripts get pasted into chat windows and
# screenshotted.
#
# Prerequisite:
#
#     brew install gh && gh auth login     # needs admin on this repository
#
# You can also set the same six by hand on the website
# (Settings → Secrets and variables → Actions):
#
#   MACOS_CERT_P12          ← base64 -i ~/Downloads/<your>.p12 | pbcopy
#   MACOS_CERT_PW           ← the p12 password
#   MACOS_NOTARY_KEY        ← base64 -i ~/Downloads/AuthKey_XXXXXXXXXX.p8 | pbcopy
#   MACOS_NOTARY_KEY_ID     ← XXXXXXXXXX (the part in the p8 filename)
#   MACOS_NOTARY_ISSUER     ← App Store Connect → Users and Access →
#                             Integrations, the UUID at the top of that page
#   CHANGJI_PRODUCT_TOKEN   ← a fine-grained personal access token with
#                             read-only Contents on the private product repo.
#                             Every pipeline here checks that repo out for
#                             webapp/, desktop/ and brand/.
#
# ---
#
# **Why each one is verified locally before it is uploaded.** Get any of these
# wrong and CI has to run an entire build — install Qt, build sd.cpp and
# llama.cpp, tens of minutes across three platforms — before failing at
# `security import` or `notarytool`. What you see then is one line, "The
# specified item could not be found in the keychain", which does not
# distinguish a wrong password from the wrong certificate. Typing it once more
# here saves those tens of minutes.

set -euo pipefail

REPO="${REPO:-changji-ai/ChangJi}"
PRODUCT_REPO="${PRODUCT_REPO:-integemjack/changji}"
P12="${P12:-}"
P8="${P8:-}"

# Find the certificate and the notarisation key without hard-coding a
# filename — the p12 is whatever you exported it as, and the p8 carries its
# key id in its name.
find_one() {
  local what="$1" pat="$2" hits
  hits=$(ls -1 "$HOME/Downloads"/$pat 2>/dev/null || true)
  case "$(printf '%s' "$hits" | grep -c . || true)" in
    0) echo "No $what found in ~/Downloads matching $pat." >&2
       echo "Point at it explicitly, e.g.  P12=/path/to/cert.p12 bash $0" >&2
       return 1 ;;
    1) printf '%s' "$hits" ;;
    *) echo "More than one $what in ~/Downloads:" >&2
       printf '%s\n' "$hits" >&2
       echo "Pick one explicitly, e.g.  P12=/path/to/cert.p12 bash $0" >&2
       return 1 ;;
  esac
}

[ -n "$P12" ] || P12="$(find_one 'signing certificate' '*.p12')"
[ -n "$P8" ]  || P8="$(find_one 'notarisation key' 'AuthKey_*.p8')"
for f in "$P12" "$P8"; do
  [ -f "$f" ] || { echo "not found: $f"; exit 1; }
done

# The key id is the part between AuthKey_ and .p8. Deriving it beats asking for
# it: a mistyped key id fails ten minutes into notarisation with "Unable to
# validate your credentials", which does not name the field.
KEY_ID="${KEY_ID:-}"
if [ -z "$KEY_ID" ]; then
  base="$(basename "$P8")"; base="${base#AuthKey_}"; KEY_ID="${base%.p8}"
fi

# Say what was picked before anything else can fail: if discovery chose the
# wrong file, that is the thing you need to see first, not a complaint about gh.
echo "certificate : $P12"
echo "notary key  : $P8  (key id $KEY_ID)"
echo "repository  : $REPO"
echo

command -v gh >/dev/null || {
  echo "gh is not installed. Run:  brew install gh && gh auth login"
  echo "(or set the six by hand on the website; see the top of this file)"
  exit 1
}
gh auth status >/dev/null 2>&1 || { echo "gh is not logged in. Run:  gh auth login"; exit 1; }

# ---- Check the p8 first ----
#
# The App Store Connect key is an EC private key. A file that downloaded wrong,
# or only half-downloaded, gives itself away here.
openssl pkey -in "$P8" -noout >/dev/null 2>&1 \
  || { echo "$P8 is not a readable private key (bad download?)"; exit 1; }
echo "✅ the notarisation key is readable"

# ---- Then the p12 ----
#
# A wrong password uploads nothing. The `-legacy` path is for older p12 files
# (OpenSSL 3 will not read RC2 by default, and that is often exactly what
# Keychain Access exports).
read -r -s -p "p12 password (not echoed): " CERT_PW; echo
CERTS=""
if CERTS="$(openssl pkcs12 -in "$P12" -nokeys -clcerts -passin pass:"$CERT_PW" -legacy 2>/dev/null)"; then
  :
elif CERTS="$(openssl pkcs12 -in "$P12" -nokeys -clcerts -passin pass:"$CERT_PW" 2>/dev/null)"; then
  :
else
  echo "wrong password (it cannot read a certificate). Nothing was uploaded."
  exit 1
fi
echo "✅ the password is correct"

# ---- Is it the right kind of certificate ----
#
# ⚠️ **It has to be a `Developer ID Application`.** A keychain holds several
# other kinds (`Apple Development`, `Mac Developer`, `3rd Party Mac
# Developer`); they all look like certificates, but **only this one can sign
# something distributed outside the App Store**. Getting it wrong shows up in
# CI as "this certificate holds no Developer ID Application identity" — twenty
# minutes in.
SUBJ="$(printf '%s' "$CERTS" | openssl x509 -noout -subject 2>/dev/null || true)"
case "$SUBJ" in
  *"Developer ID Application"*) echo "✅ it is a Developer ID Application" ;;
  "") echo "the p12 is readable but holds no certificate?"; exit 1 ;;
  *)  echo "this is not a Developer ID Application certificate. It is:"
      echo "    $SUBJ"
      echo "Anything distributed outside the App Store can only be signed with"
      echo "a Developer ID Application certificate. Nothing was uploaded."
      exit 1 ;;
esac

# ---- Issuer ----
read -r -p "App Store Connect Issuer ID (UUID): " ISSUER
case "$ISSUER" in
  [0-9a-fA-F]*) : ;;
  *) echo "that does not look like a UUID. Stopping."; exit 1 ;;
esac
[ "${#ISSUER}" = "36" ] || { echo "a UUID is 36 characters; this is ${#ISSUER}. Stopping."; exit 1; }

# ---- The token for the private product repo ----
#
# **Verified against the real repository before it is uploaded.** A token with
# the wrong scope, or one that has expired, fails at the very first checkout
# step of every pipeline — with "Repository not found", which reads like a
# typo in the repository name rather than a permissions problem. One API call
# here answers it now.
echo
echo "A fine-grained token with read-only Contents on $PRODUCT_REPO."
echo "Create one at: https://github.com/settings/personal-access-tokens"
read -r -s -p "CHANGJI_PRODUCT_TOKEN (not echoed): " PRODUCT_TOKEN; echo
[ -n "$PRODUCT_TOKEN" ] || { echo "empty. Stopping."; exit 1; }
if GH_TOKEN="$PRODUCT_TOKEN" gh api "repos/$PRODUCT_REPO" --silent 2>/dev/null; then
  echo "✅ the token can read $PRODUCT_REPO"
else
  echo "this token cannot read $PRODUCT_REPO."
  echo "It needs Contents: Read-only on that repository, and it must not have expired."
  echo "Nothing was uploaded."
  exit 1
fi

echo
echo "==> writing into $REPO"
base64 -i "$P12"            | gh secret set MACOS_CERT_P12        --repo "$REPO"
printf '%s' "$CERT_PW"      | gh secret set MACOS_CERT_PW         --repo "$REPO"
base64 -i "$P8"             | gh secret set MACOS_NOTARY_KEY      --repo "$REPO"
printf '%s' "$KEY_ID"       | gh secret set MACOS_NOTARY_KEY_ID   --repo "$REPO"
printf '%s' "$ISSUER"       | gh secret set MACOS_NOTARY_ISSUER   --repo "$REPO"
printf '%s' "$PRODUCT_TOKEN"| gh secret set CHANGJI_PRODUCT_TOKEN --repo "$REPO"

unset CERT_PW ISSUER PRODUCT_TOKEN

echo
echo "==> set:"
gh secret list --repo "$REPO" | grep -E "MACOS_|CHANGJI_" || true
echo
echo "Now push a branch, or tag a release:"
echo "    git tag v1.2.0 && git push origin v1.2.0      the engine"
echo "    git tag desktop-v1.2 && git push origin desktop-v1.2   the desktop app"
echo
echo "The macOS binaries will be signed and notarised. If either is missing, a"
echo "beta warns and a tagged release stops — an unsigned build tells every"
echo "macOS user that the download is damaged."
