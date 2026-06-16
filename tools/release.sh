#!/usr/bin/env bash
#
# Release helper for lazyverilog.
#
# The GitHub Actions release workflow is the source of truth for publishing a
# release.  This helper keeps the local steps explicit:
#
#   0. Show existing release / pre-release tags.
#   1. Ask for the version to release, for example: v1.2.3 or v1.2.3-rc.1.
#   2. Stage and commit docs/releases/<version>.md when it exists locally.
#   3. Push the current branch so workflow_dispatch uses the latest workflow and
#      release note.
#   4. Trigger .github/workflows/release.yml with the requested version.
#   5. Watch the workflow.  The workflow builds binaries, computes checksums,
#      updates Lua and VS Code release metadata, builds the VSIX from that
#      updated metadata, commits the metadata, tags that new commit, and uploads
#      the binaries plus VSIX to the GitHub Release.
#   6. After a successful watched workflow, fast-forward the local branch to the
#      workflow's release-metadata commit and print the manual Visual Studio
#      Marketplace upload instructions for the VSIX built by GitHub Actions.
#
# Assumptions:
#   - Release tags use a leading "v" SemVer-ish spelling, such as v1.0.2.
#   - `gh` (GitHub CLI) is installed and authenticated.
#
# Optional flags:
#   --version VERSION   Skip the interactive version prompt.
#   --no-commit        Do not commit the release note.
#   --no-push          Do not push the branch before dispatching the workflow.
#   --no-watch         Trigger the workflow but do not wait for completion.
#   --dry-run          Print commands that would mutate git/GitHub, but do not run them.
#
# Examples:
#   tools/release.sh
#   tools/release.sh --version v1.2.6
#   tools/release.sh --version v1.2.6 --dry-run

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

RELEASE_WORKFLOW="release.yml"
RELEASE_REPO="lazyverilog/LazyVerilog"
MARKETPLACE_PUBLISHER="lazyverilog"
MARKETPLACE_MANAGE_URL="https://marketplace.visualstudio.com/manage/publishers/lazyverilog"

VERSION=""
DO_COMMIT=1
DO_PUSH=1
DO_WATCH=1
DRY_RUN=0

usage() {
    sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
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
            --no-push)
                DO_PUSH=0
                shift
                ;;
            --no-watch)
                DO_WATCH=0
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


vscode_semver() {
    printf '%s\n' "${VERSION#v}"
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
        die "tag '${VERSION}' already exists locally"
    fi

    if [[ "$DRY_RUN" == 0 ]] && gh release view "$VERSION" --repo "$RELEASE_REPO" >/dev/null 2>&1; then
        die "GitHub Release '${VERSION}' already exists"
    fi
}

check_release_note() {
    local note="${REPO_ROOT}/docs/releases/${VERSION}.md"
    if [[ -f "$note" ]]; then
        printf 'Release note found: %s\n' "${note#$REPO_ROOT/}"
    else
        printf 'WARNING: no release note found at docs/releases/%s.md\n' "$VERSION"
        confirm "Continue without a release note?" || die "aborted — add docs/releases/${VERSION}.md first"
    fi
}

ensure_clean_enough_for_release() {
    if ! git -C "$REPO_ROOT" diff --quiet || ! git -C "$REPO_ROOT" diff --cached --quiet; then
        printf '\nCurrent git changes:\n'
        git -C "$REPO_ROOT" status --short
        printf '\n'
        confirm "Continue release with these working-tree changes?" || die "aborted by user"
    fi
}

commit_release_note() {
    [[ "$DO_COMMIT" == 1 ]] || return 0

    local note="${REPO_ROOT}/docs/releases/${VERSION}.md"
    [[ -f "$note" ]] || return 0

    run git -C "$REPO_ROOT" add "$note"

    if git -C "$REPO_ROOT" diff --cached --quiet -- "$note"; then
        printf 'No staged release-note change to commit.\n'
        return 0
    fi

    run git -C "$REPO_ROOT" commit -m "Add release notes for ${VERSION}"
}

push_branch() {
    [[ "$DO_PUSH" == 1 ]] || return 0

    local branch
    branch="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)"
    [[ "$branch" != "HEAD" ]] || die "cannot dispatch release from detached HEAD"

    confirm "Push branch ${branch} now? (required so workflow_dispatch sees current files)" ||
        die "aborted before pushing branch"
    run git -C "$REPO_ROOT" push origin "HEAD:${branch}"
}

trigger_release_workflow() {
    local branch
    local workflow_dry_run
    branch="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)"
    [[ "$branch" != "HEAD" ]] || die "cannot dispatch release from detached HEAD"

    confirm "Trigger release workflow for ${VERSION} on ${branch}?" || die "aborted before workflow dispatch"

    # The workflow defaults dry_run to true for safety when users click
    # "Run workflow" in the GitHub UI.  This release helper is the intentional
    # publish path, so pass dry_run=false explicitly unless this script itself is
    # running in --dry-run mode.
    workflow_dry_run=false
    if [[ "$DRY_RUN" == 1 ]]; then
        workflow_dry_run=true
    fi

    run gh workflow run "$RELEASE_WORKFLOW" --repo "$RELEASE_REPO" --ref "$branch" \
        -f "version=${VERSION}" \
        -f "dry_run=${workflow_dry_run}"

    if [[ "$DRY_RUN" == 1 || "$DO_WATCH" == 0 ]]; then
        return 0
    fi

    printf 'Waiting for workflow run to start'
    local run_id=""
    local attempts=0
    while [[ $attempts -lt 20 ]]; do
        printf '.'
        sleep 3
        run_id="$(gh run list --repo "$RELEASE_REPO" --workflow="$RELEASE_WORKFLOW" \
            --limit=10 --json databaseId,status,event,headBranch \
            --jq '.[] | select(.event == "workflow_dispatch" and .headBranch == "'"$branch"'" and .status != "completed") | .databaseId' \
            2>/dev/null | head -n 1 || true)"
        [[ -n "$run_id" ]] && break
        ((attempts++))
    done
    printf '\n'

    [[ -n "$run_id" ]] || die "could not find workflow run after dispatch — check Actions tab manually"

    printf 'Watching run %s...\n' "$run_id"
    gh run watch "$run_id" --repo "$RELEASE_REPO" --exit-status \
        || die "release workflow failed"
}

refresh_release_metadata_and_print_marketplace_instructions() {
    # The workflow is responsible for computing checksums, updating release
    # metadata, building the VSIX, and uploading that VSIX to the GitHub Release.
    # After a watched workflow completes, keep the local branch in sync and point
    # the manual Marketplace step at the GitHub Release asset.  Do not rebuild
    # the VSIX locally; that duplicates CI output and requires unnecessary local
    # Node/npm tooling.
    if [[ "$DRY_RUN" == 1 ]]; then
        return 0
    fi

    if [[ "$DO_WATCH" == 1 ]]; then
        local branch
        branch="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)"
        [[ "$branch" != "HEAD" ]] || die "cannot refresh release metadata from detached HEAD"

        printf '\nRefreshing local branch with workflow-generated release metadata...\n'
        run git -C "$REPO_ROOT" pull --ff-only origin "$branch"
    else
        printf '\nVS Code Marketplace publish step skipped locally because --no-watch was used.\n'
        printf 'After the GitHub Release finishes, publish the VSIX asset manually from:\n'
        printf '  https://github.com/%s/releases/tag/%s\n' "$RELEASE_REPO" "$VERSION"
    fi

    print_marketplace_publish_instructions "lazyverilog-${VERSION}.vsix" "GitHub Release asset"
}

print_marketplace_publish_instructions() {
    local vsix_ref="$1"
    local source_label="$2"

    printf '\nVS Code Marketplace manual publish\n'
    printf '---------------------------------\n'
    printf 'Publisher: %s\n' "$MARKETPLACE_PUBLISHER"
    printf 'Manage page: %s\n' "$MARKETPLACE_MANAGE_URL"
    printf 'VSIX (%s): %s\n' "$source_label" "$vsix_ref"
    printf '\nManual steps:\n'
    printf '  1. Open %s\n' "$MARKETPLACE_MANAGE_URL"
    printf '  2. Choose the %s publisher.\n' "$MARKETPLACE_PUBLISHER"
    printf '  3. Upload the VSIX above as the new Visual Studio Code extension version.\n'
    printf '  4. Verify the Marketplace version is %s before announcing the release.\n' "$(vscode_semver)"
}

main() {
    parse_args "$@"
    require_tools

    show_existing_versions
    prompt_version
    validate_version

    printf 'Release version: %s\n\n' "$VERSION"

    check_release_note
    confirm "Proceed with release?" || die "aborted by user"

    ensure_clean_enough_for_release
    commit_release_note
    push_branch
    trigger_release_workflow
    refresh_release_metadata_and_print_marketplace_instructions

    printf '\nRelease workflow dispatched for %s.\n' "$VERSION"
    printf 'The workflow will commit checksums, create the tag, and publish the GitHub Release.\n'
    printf 'Monitor: https://github.com/%s/actions\n' "$RELEASE_REPO"
}

main "$@"
