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
#   3. Create a git commit and tag if needed, then run `git push --tags`.
#
# GitHub Actions (.github/workflows/release.yml) handles the build and upload:
# pushing the tag triggers a matrix build (linux-x64, linux-arm64, darwin-x64,
# darwin-arm64) on the correct runners and uploads the assets to the release.
#
# Assumptions:
#   - Release tags use a leading "v" SemVer-ish spelling, such as v1.0.2.
#   - The version file is lua/lazyverilog/version.lua and contains:
#         return "vX.Y.Z"
#
# Optional flags:
#   --version VERSION   Skip the interactive version prompt.
#   --no-commit        Update version file, but do not create a commit.
#   --no-tag           Do not create a git tag.
#   --no-push          Do not run git push --tags.
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

VERSION=""
DO_COMMIT=1
DO_TAG=1
DO_PUSH=1
DRY_RUN=0

usage() {
    sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'
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

    for tool in git; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing+=("$tool")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing required tool(s): ${missing[*]}"
    fi
}

show_existing_versions() {
    printf '\nExisting release / pre-release tags:\n'

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

    if [[ ! "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
        die "version '$VERSION' must look like vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-PRERELEASE"
    fi

    if git -C "$REPO_ROOT" rev-parse -q --verify "refs/tags/${VERSION}" >/dev/null; then
        die "tag '${VERSION}' already exists"
    fi
}

update_version_file() {
    [[ -f "$VERSION_FILE" ]] || die "missing version file: $VERSION_FILE"

    printf 'Updating %s -> %s\n' "${VERSION_FILE#$REPO_ROOT/}" "$VERSION"

    local tmp
    tmp="$(mktemp "${VERSION_FILE}.XXXXXX")"
    printf 'return "%s"\n' "$VERSION" > "$tmp"
    mv "$tmp" "$VERSION_FILE"
}

ensure_clean_enough_for_release() {
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

    confirm "Run 'git push --tags' now? (triggers GitHub Actions build)" || die "aborted before pushing tags"
    run git -C "$REPO_ROOT" push
    run git -C "$REPO_ROOT" push --tags
}

main() {
    parse_args "$@"
    require_tools

    show_existing_versions
    prompt_version
    validate_version

    printf 'Release version: %s\n' "$VERSION"
    printf '\n'

    confirm "Proceed with release?" || die "aborted by user"

    ensure_clean_enough_for_release
    update_version_file
    commit_version_file
    tag_release
    push_tags

    printf '\nRelease %s tagged and pushed.\n' "$VERSION"
    printf 'GitHub Actions will build and upload assets automatically.\n'
    printf 'Monitor: https://github.com/hxxdev/LazyVerilog/actions\n'
}

main "$@"
