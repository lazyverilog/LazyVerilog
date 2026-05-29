#!/usr/bin/env bash
#
# Remove a lazyverilog release.
#
# This is the destructive counterpart to tools/release.sh.  It removes:
#
#   1. The GitHub release page for a selected version, if it exists.
#   2. The remote git tag from origin, if it exists.
#   3. The local git tag, if it exists.
#
# The script is intentionally interactive by default because deleting a release
# and tag is hard to undo.  Use --yes only when the exact version is already
# known, for example from a CI cleanup job.
#
# Requirements:
#   - git
#   - gh, authenticated with permission to delete releases / tags
#
# Examples:
#   tools/remove_release.sh
#   tools/remove_release.sh --version v1.0.1
#   tools/remove_release.sh --version v1.0.1 --yes
#
# Optional flags:
#   --version VERSION   Skip the interactive version prompt.
#   --yes              Do not ask for final confirmation.
#   --keep-remote-tag  Delete the GitHub release page but keep origin's tag.
#   --keep-local-tag   Delete the GitHub release page but keep the local tag.
#   --dry-run          Print destructive commands, but do not run them.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

VERSION=""
ASSUME_YES=0
KEEP_REMOTE_TAG=0
KEEP_LOCAL_TAG=0
DRY_RUN=0

usage() {
    sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
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

    if [[ "$ASSUME_YES" == 1 ]]; then
        return 0
    fi

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
            --yes|-y)
                ASSUME_YES=1
                shift
                ;;
            --keep-remote-tag)
                KEEP_REMOTE_TAG=1
                shift
                ;;
            --keep-local-tag)
                KEEP_LOCAL_TAG=1
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

    for tool in git gh; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing+=("$tool")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing required tool(s): ${missing[*]}"
    fi
}

show_versions() {
    printf '\nLocal tags:\n'
    local local_tags
    local_tags="$(git -C "$REPO_ROOT" tag --list 'v*' --sort=-v:refname)"
    if [[ -z "$local_tags" ]]; then
        printf '  (none)\n'
    else
        printf '%s\n' "$local_tags" | sed 's/^/  /'
    fi

    printf '\nGitHub releases:\n'
    if gh release list --limit 100 >/tmp/lazyverilog-gh-releases.$$ 2>/dev/null; then
        if [[ -s /tmp/lazyverilog-gh-releases.$$ ]]; then
            sed 's/^/  /' /tmp/lazyverilog-gh-releases.$$
        else
            printf '  (none)\n'
        fi
        rm -f /tmp/lazyverilog-gh-releases.$$
    else
        rm -f /tmp/lazyverilog-gh-releases.$$
        printf '  (unable to list; gh auth or network may be unavailable)\n'
    fi
    printf '\n'
}

prompt_version() {
    if [[ -n "$VERSION" ]]; then
        return
    fi

    read -r -p "Version to remove, for example v1.0.1: " VERSION
}

validate_version() {
    [[ -n "$VERSION" ]] || die "version must not be empty"

    # Keep validation permissive enough for release candidates and build
    # metadata, but restrictive enough to avoid accidentally passing arbitrary
    # refspecs or shell-looking strings to git / gh commands.
    if [[ ! "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
        die "version '$VERSION' must look like vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-PRERELEASE"
    fi
}

delete_github_release() {
    if gh release view "$VERSION" >/dev/null 2>&1; then
        # `--cleanup-tag` is intentionally not used here.  We delete local and
        # remote tags in explicit steps below so dry-run output and failures are
        # easier to understand.
        run gh release delete "$VERSION" --yes
    else
        printf 'GitHub release %s does not exist; skipping release-page deletion.\n' "$VERSION"
    fi
}

delete_remote_tag() {
    [[ "$KEEP_REMOTE_TAG" == 0 ]] || return 0

    # Deleting a non-existent remote tag with this refspec is harmless on most
    # servers, but GitHub still reports clearly what happened.  Keep it simple
    # and visible.
    run git -C "$REPO_ROOT" push origin ":refs/tags/${VERSION}"
}

delete_local_tag() {
    [[ "$KEEP_LOCAL_TAG" == 0 ]] || return 0

    if git -C "$REPO_ROOT" rev-parse -q --verify "refs/tags/${VERSION}" >/dev/null; then
        run git -C "$REPO_ROOT" tag -d "$VERSION"
    else
        printf 'Local tag %s does not exist; skipping local tag deletion.\n' "$VERSION"
    fi
}

main() {
    parse_args "$@"
    require_tools
    show_versions
    prompt_version
    validate_version

    printf 'About to remove release artifacts for: %s\n' "$VERSION"
    [[ "$KEEP_REMOTE_TAG" == 1 ]] && printf '  - keeping remote tag\n'
    [[ "$KEEP_LOCAL_TAG" == 1 ]] && printf '  - keeping local tag\n'
    [[ "$DRY_RUN" == 1 ]] && printf '  - dry run; no commands will be executed\n'
    printf '\n'

    confirm "Delete GitHub release and selected tags for ${VERSION}?" || die "aborted by user"

    delete_github_release
    delete_remote_tag
    delete_local_tag

    printf 'Remove-release helper finished for %s.\n' "$VERSION"
}

main "$@"
