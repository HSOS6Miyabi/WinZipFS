#!/usr/bin/env bash
set -euo pipefail
repo="$GITHUB_REPOSITORY"
sha="$GITHUB_SHA"
version="$(python - <<'PY'
import json
import re
from pathlib import Path
cmake = re.search(r'project\(WinZipFS VERSION (\d+\.\d+\.\d+) ', Path('CMakeLists.txt').read_text()).group(1)
installer = re.search(r'#define AppVersion "([^"]+)"', Path('installer/WinZipFS.iss').read_text()).group(1)
package = json.loads(Path('vcpkg.json').read_text())['version']
if cmake != installer or cmake != package:
    raise SystemExit('Project, installer and package versions must match')
print(cmake)
PY
)"
tag="v$version"
if [[ "$(gh api "repos/$repo/git/ref/heads/main" --jq .object.sha)" != "$sha" ]]; then
    echo "A newer main commit exists; this run will not publish a release."
    exit 0
fi

# Published versions are immutable. Only an unfinished draft for this exact
# commit may be resumed.
existing="$(gh release view "$tag" --repo "$repo" --json isDraft,targetCommitish 2>/dev/null || true)"
if [[ -n "$existing" ]]; then
    if [[ "$(jq -r .isDraft <<<"$existing")" != true ]]; then
        echo "$tag is already published. Bump the project version for the next release."
        exit 0
    fi
    if [[ "$(jq -r .targetCommitish <<<"$existing")" != "$sha" ]]; then
        echo "::error::The release draft belongs to a different commit."
        exit 1
    fi
fi

test -f artifacts/app/WinZipFS.exe
test -f artifacts/app/WinZipFSCore.dll
test -f artifacts/app/7z.dll
test -f artifacts/installer/WinZipFS-Setup-x64.exe
mkdir -p release
cp README.md artifacts/app/README.md
cp LICENSE artifacts/app/LICENSE
portable="WinZipFS-$version-windows-x64.zip"
installer="WinZipFS-$version-Setup-x64.exe"
(cd artifacts/app && zip -q -r "../../release/$portable" .)
cp artifacts/installer/WinZipFS-Setup-x64.exe "release/$installer"
(cd release && unzip -tq "$portable" && sha256sum "$portable" "$installer" > SHA256SUMS.txt)
cat > release-notes.md <<EOF
WinZipFS $version for Windows x64.

- Mount archives as drives or local UNC paths without extracting files to a disk cache.
- Random reads, bounded memory caching, sparse memory writes and verified archive updates.
- WinUI 3 dashboard with responsive layout, notification area controls and Windows file associations.
- Automatic idle release, explicit read-only mounting, password prompts and safe export.

**Installation:** download the Setup executable, or extract the portable ZIP and install WinFsp 2.2 or later. Choose WinZipFS in Windows Default apps to enable double-click mounting.

Pending changes are held in memory until saved. Solid or sequential compression formats may require decoding earlier data. Encrypted archives are read-only. The binaries are unsigned.

Tested by [Windows CI]($GITHUB_SERVER_URL/$repo/actions/runs/$GITHUB_RUN_ID) at commit \`$sha\`. SHA-256 checksums are included.

See [the README]($GITHUB_SERVER_URL/$repo/blob/$sha/README.md) for format support, usage and third-party software.
EOF

if [[ -z "$existing" ]]; then
    gh release create "$tag" --repo "$repo" --target "$sha" --title "WinZipFS $tag" \
        --notes-file release-notes.md --draft
fi
gh release upload "$tag" "release/$portable" "release/$installer" release/SHA256SUMS.txt --repo "$repo" --clobber

# Download the draft's actual assets and verify them before making it public.
mkdir -p release-verified
gh release download "$tag" --repo "$repo" --dir release-verified --pattern '*'
(cd release-verified && sha256sum -c SHA256SUMS.txt)
cmp release/SHA256SUMS.txt release-verified/SHA256SUMS.txt
if [[ "$(gh api "repos/$repo/git/ref/heads/main" --jq .object.sha)" != "$sha" ]]; then
    echo "::error::Main advanced during publication. The verified release remains a draft."
    exit 1
fi
gh release edit "$tag" --repo "$repo" --draft=false --latest
actual="$(gh api "repos/$repo/git/ref/tags/$tag" --jq .object.sha)"
[[ "$actual" == "$sha" ]] || { echo "::error::Release tag does not identify the tested commit"; exit 1; }
gh release view "$tag" --repo "$repo" --json tagName,isDraft,url,assets
echo "Published $tag from $sha" >> "$GITHUB_STEP_SUMMARY"
