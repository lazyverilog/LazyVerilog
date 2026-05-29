#!/usr/bin/env bash
#
# Release helper for lazyverilog.
#
# This script intentionally keeps the release flow explicit and interactive:
#
#   0. Show the user existing release / pre-release tags.
#   1. Ask for the version to release, for example: v1.2.3 or v1.2.3-rc.1.
#   2. Update lua/lazyverilog/version.lua so the Neovim plugin downloads the
#      matching GitHub release asset.
#   3. Configure and build the LSP server in Release mode.
#   4. Copy the built lazyverilog-lsp binary to:
#
#          dist/lazyverilog-lsp-${version}-${distribution}
#
#      where distribution is one of linux-x64, linux-arm64, darwin-x64, or
#      darwin-arm64.
#   5. Strip the copied release asset, leaving the build tree binary untouched.
#   6. Create a git commit and tag if needed, then run `git push --tags`.
#   7. Create/update the GitHub release page and upload the built asset.
#
# Why copy before stripping?
#   Developers often want the build tree binary to remain useful for local
#   debugging.  Stripping only the copied asset keeps the release artifact small
#   without mutating build-release/lazyverilog-lsp.
#
# Assumptions:
#   - Release tags use a leading "v" SemVer-ish spelling, such as v1.0.2.
#   - The version file is lua/lazyverilog/version.lua and contains:
#         return "vX.Y.Z"
#   - GitHub release assets are uploaded with the GitHub CLI (`gh`).
#
# Optional flags:
#   --version VERSION   Skip the interactive version prompt.
#   --no-commit        Update/build/package, but do not create a commit.
#   --no-tag           Do not create a git tag.
#   --no-push          Do not run git push --tags.
#   --no-upload        Do not upload the asset to the GitHub release page.
#   --dry-run          Print commands that would mutate git, but do not run them.
#
# Examples:
#   tools/release.sh
#   tools/release.sh --version v1.2.3-rc.1 --no-push
#   tools/release.sh --version v1.2.3 --dry-run

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

VERSION_FILE="${REPO_ROOT}/lua/lazyverilog/version.lua"
BUILD_DIR="${REPO_ROOT}/build-release"
DIST_DIR="${REPO_ROOT}/dist"

VERSION=""
DO_COMMIT=1
DO_TAG=1
DO_PUSH=1
DO_UPLOAD=1
DRY_RUN=0
RELEASE_ASSET=""

usage() {
    # Print the leading comment block without mangling the shebang.
    # The first line is "#!/usr/bin/env bash", so start at line 2.
    sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'

    if [[ "$DRY_RUN" == 1 ]]; then
        return 0
    fi

    "$@"
}

confirm() {
    local prompt="$1"
    local answer

    read -r -p "${prompt} [y/N] " answer
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --version)
                [[ $# -ge 2 ]] || die "--version requires an argument"
                VERSION="$2"
                shift 2
                ;;
            --no-commit)
                DO_COMMIT=0
                shift
                ;;
            --no-tag)
                DO_TAG=0
                shift
                ;;
            --no-push)
                DO_PUSH=0
                shift
                ;;
            --no-upload)
                DO_UPLOAD=0
                shift
                ;;
            --dry-run)
                DRY_RUN=1
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "unknown argument: $1"
                ;;
        esac
    done
}

require_tools() {
    local missing=()
    local tool

    for tool in git cmake strip; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing+=("$tool")
        fi
    done

    if [[ "$DO_UPLOAD" == 1 ]] && ! command -v gh >/dev/null 2>&1; then
        missing+=("gh")
    fi

    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing required tool(s): ${missing[*]}"
    fi
}

show_existing_versions() {
    printf '\nExisting release / pre-release tags:\n'

    # List local tags only.  Fetching remote tags here would add network and
    # authentication requirements before the user has even confirmed a release.
    # The final `git push --tags` will still fail visibly if the remote rejects
    # or already has a conflicting tag.
    local tags
    tags="$(git -C "$REPO_ROOT" tag --list 'v*' --sort=-v:refname)"

    if [[ -z "$tags" ]]; then
        printf '  (none)\n'
    else
        printf '%s\n' "$tags" | sed 's/^/  /'
    fi
    printf '\n'
}

prompt_version() {
    if [[ -n "$VERSION" ]]; then
        return
    fi

    read -r -p "Version to release (for example v1.2.3 or v1.2.3-rc.1): " VERSION
}

validate_version() {
    [[ -n "$VERSION" ]] || die "version must not be empty"

    # Accept standard release and pre-release spellings:
    #   v1.2.3
    #   v1.2.3-rc.1
    #   v1.2.3-beta.2
    if [[ ! "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
        die "version '$VERSION' must look like vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-PRERELEASE"
    fi

    if git -C "$REPO_ROOT" rev-parse -q --verify "refs/tags/${VERSION}" >/dev/null; then
        die "tag '${VERSION}' already exists"
    fi
}

distribution_name() {
    local os
    local arch

    os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    arch="$(uname -m | tr '[:upper:]' '[:lower:]')"

    case "$os" in
        linux*) os="linux" ;;
        darwin*) os="darwin" ;;
        *) die "unsupported release OS from uname -s: $os" ;;
    esac

    case "$arch" in
        x86_64|amd64) arch="x64" ;;
        aarch64|arm64) arch="arm64" ;;
        *) die "unsupported release architecture from uname -m: $arch" ;;
    esac

    printf '%s-%s\n' "$os" "$arch"
}

update_version_file() {
    [[ -f "$VERSION_FILE" ]] || die "missing version file: $VERSION_FILE"

    printf 'Updating %s -> %s\n' "${VERSION_FILE#$REPO_ROOT/}" "$VERSION"

    # Use a temporary file and mv to avoid depending on GNU/BSD sed -i
    # differences.  The file is intentionally tiny, but this style is portable
    # and easy to audit.
    local tmp
    tmp="$(mktemp "${VERSION_FILE}.XXXXXX")"
    printf 'return "%s"\n' "$VERSION" > "$tmp"
    mv "$tmp" "$VERSION_FILE"
}

build_release() {
    run cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    run cmake --build "$BUILD_DIR" "-j$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 4)"
}

package_binary() {
    local distribution
    local built_binary
    local asset

    distribution="$(distribution_name)"
    built_binary="${BUILD_DIR}/lazyverilog-lsp"
    asset="${DIST_DIR}/lazyverilog-lsp-${VERSION}-${distribution}"

    [[ -x "$built_binary" ]] || die "expected built binary not found or not executable: $built_binary"

    mkdir -p "$DIST_DIR"

    run cp "$built_binary" "$asset"
    run strip "$asset"
    run chmod +x "$asset"
    RELEASE_ASSET="$asset"

    printf '\nRelease asset prepared:\n  %s\n\n' "${asset#$REPO_ROOT/}"
}

ensure_clean_enough_for_release() {
    # Do not require a fully clean tree: this script itself is often developed
    # alongside source changes.  Instead, show the pending diff summary and make
    # the human confirm before committing/tagging/pushing.
    if ! git -C "$REPO_ROOT" diff --quiet || ! git -C "$REPO_ROOT" diff --cached --quiet; then
        printf '\nCurrent git changes:\n'
        git -C "$REPO_ROOT" status --short
        printf '\n'

        if [[ "$DO_COMMIT" == 1 || "$DO_TAG" == 1 || "$DO_PUSH" == 1 ]]; then
            confirm "Continue release with these working-tree changes?" ||
                die "aborted by user"
        fi
    fi
}

commit_version_file() {
    [[ "$DO_COMMIT" == 1 ]] || return 0

    run git -C "$REPO_ROOT" add "$VERSION_FILE"

    if git -C "$REPO_ROOT" diff --cached --quiet -- "$VERSION_FILE"; then
        printf 'No staged version-file change to commit.\n'
        return 0
    fi

    run git -C "$REPO_ROOT" commit -m "Release ${VERSION}"
}

tag_release() {
    [[ "$DO_TAG" == 1 ]] || return 0

    if git -C "$REPO_ROOT" rev-parse -q --verify "refs/tags/${VERSION}" >/dev/null; then
        printf 'Tag %s already exists; not creating it again.\n' "$VERSION"
        return 0
    fi

    run git -C "$REPO_ROOT" tag -a "$VERSION" -m "Release ${VERSION}"
}

push_tags() {
    [[ "$DO_PUSH" == 1 ]] || return 0

    confirm "Run 'git push --tags' now?" || die "aborted before pushing tags"
    run git -C "$REPO_ROOT" push --tags
}

upload_release_asset() {
    [[ "$DO_UPLOAD" == 1 ]] || return 0

    [[ -n "$RELEASE_ASSET" ]] || die "release asset path was not set"
    [[ -f "$RELEASE_ASSET" ]] || die "release asset does not exist: $RELEASE_ASSET"

    # GitHub marks pre-releases at the release object level, not from the tag
    # name automatically.  Treat versions like v1.2.3-rc.1 as pre-releases.
    local prerelease_args=()
    if [[ "$VERSION" == *-* ]]; then
        prerelease_args+=(--prerelease)
    fi

    if gh release view "$VERSION" >/dev/null 2>&1; then
        printf 'GitHub release %s already exists; uploading asset with --clobber.\n' "$VERSION"
        run gh release upload "$VERSION" "$RELEASE_ASSET" --clobber
    else
        printf 'Creating GitHub release %s and uploading asset.\n' "$VERSION"
        run gh release create "$VERSION" "$RELEASE_ASSET" \
            --title "$VERSION" \
            --notes "Release ${VERSION}" \
            "${prerelease_args[@]}"
    fi
}

main() {
    parse_args "$@"
    require_tools

    show_existing_versions
    prompt_version
    validate_version

    printf 'Release version: %s\n' "$VERSION"
    printf 'Distribution:    %s\n' "$(distribution_name)"
    printf 'Build dir:       %s\n' "${BUILD_DIR#$REPO_ROOT/}"
    printf 'Dist dir:        %s\n' "${DIST_DIR#$REPO_ROOT/}"
    printf '\n'

    confirm "Proceed with release packaging?" || die "aborted by user"

    ensure_clean_enough_for_release
    update_version_file
    build_release
    package_binary
    commit_version_file
    tag_release
    push_tags
    upload_release_asset

    printf 'Release helper finished for %s.\n' "$VERSION"
}

main "$@"
