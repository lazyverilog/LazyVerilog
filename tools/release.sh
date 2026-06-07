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
#   3. Commit the version bump and push the branch.
#   4. Trigger a workflow_dispatch build test via `gh` and wait for it to pass.
#   5. Create a git tag and push --tags to trigger the release build.
#
# GitHub Actions (.github/workflows/release.yml) handles the build and upload:
# pushing the tag triggers a matrix build (linux-x64, linux-arm64, darwin-x64,
# darwin-arm64) on the correct runners and uploads the assets to the release.
#
# Assumptions:
#   - Release tags use a leading "v" SemVer-ish spelling, such as v1.0.2.
#   - The version file is lua/lazyverilog/version.lua and contains:
#         return "vX.Y.Z"
#   - `gh` (GitHub CLI) is installed and authenticated.
#
# Optional flags:
#   --version VERSION   Skip the interactive version prompt.
#   --no-commit        Update version file, but do not create a commit.
#   --no-tag           Do not create a git tag.
#   --no-push          Do not run git push --tags.
#   --no-build-test    Skip the workflow_dispatch build test.
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
DO_BUILD_TEST=1
DRY_RUN=0

usage() {
    sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'
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
            --no-build-test)
                DO_BUILD_TEST=0
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

    local tools=(git)
    if [[ "$DO_BUILD_TEST" == 1 && "$DO_PUSH" == 1 ]]; then
        tools+=(gh)
    fi

    for tool in "${tools[@]}"; do
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

push_branch() {
    [[ "$DO_PUSH" == 1 ]] || return 0

    confirm "Push branch now? (needed for build test)" || die "aborted before pushing branch"
    run git -C "$REPO_ROOT" push
}

trigger_build_test() {
    [[ "$DO_BUILD_TEST" == 1 && "$DO_PUSH" == 1 ]] || return 0

    confirm "Run pre-release build test via workflow_dispatch?" || return 0

    local branch
    branch="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)"

    printf 'Triggering build test via workflow_dispatch on branch %s...\n' "$branch"
    run gh workflow run release.yml --repo hxxdev/LazyVerilog --ref "$branch"

    if [[ "$DRY_RUN" == 1 ]]; then
        return 0
    fi

    # Wait for the run to appear (dispatch is async)
    printf 'Waiting for workflow run to start'
    local run_id=""
    local attempts=0
    while [[ $attempts -lt 15 ]]; do
        printf '.'
        sleep 3
        run_id="$(gh run list --repo hxxdev/LazyVerilog --workflow=release.yml \
            --limit=1 --json databaseId,status \
            --jq '.[] | select(.status != "completed") | .databaseId' 2>/dev/null || true)"
        [[ -n "$run_id" ]] && break
        ((attempts++))
    done
    printf '\n'

    [[ -n "$run_id" ]] || die "could not find workflow run after dispatch — check Actions tab manually"

    printf 'Watching run %s...\n' "$run_id"
    gh run watch "$run_id" --repo hxxdev/LazyVerilog --exit-status \
        || die "build test failed — fix the issue before releasing"

    printf 'Build test passed.\n\n'
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
    [[ "$DO_PUSH" == 1 && "$DO_TAG" == 1 ]] || return 0

    confirm "Push tag ${VERSION} now? (triggers GitHub Actions release build)" \
        || die "aborted before pushing tag"
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
    push_branch
    trigger_build_test
    tag_release
    push_tags

    printf '\nRelease %s tagged and pushed.\n' "$VERSION"
    printf 'GitHub Actions will build and upload assets automatically.\n'
    printf 'Monitor: https://github.com/hxxdev/LazyVerilog/actions\n'
}

main "$@"
