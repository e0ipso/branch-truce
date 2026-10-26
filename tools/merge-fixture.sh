# Disposable merge-conflict fixture shared by tools/validate-installed-artifact.sh
# (inside its container) and tools/preview-app.sh (on the host). Sourcing this
# file defines the functions below and runs nothing.
#
#   make_merge_fixture LAUNCHER FILE...
#
# Empties the private home $home (data, config, cache, tmp and runtime
# directories and the mergetool logs), writes $home/gitconfig, and rebuilds
# $repo stopped on a merge of branch side into main in which every FILE
# conflicts. The content follows the name: *.md is Markdown, *.bin is binary
# (it holds NUL bytes) and anything else is a single line of text. LAUNCHER is
# the branch-truce executable that `git mergetool --tool truce` starts.
#
# The caller sets $home and $repo and defines `isolated`, which runs its
# arguments inside $repo with the environment of $home. Returns 1 when a step
# fails or the merge does not stop on a conflict.

# merge_fixture_content FILE base|side|main
merge_fixture_content() {
    case "$1" in
        *.md) printf '# Release notes\n\nThe heading and this line are the same on every branch.\n\n- %s wording for the summary\n- shared closing item\n' "$2" ;;
        *.bin) printf 'BT\0%s\0\n' "$2" ;;
        *) printf '%s\n' "$2" ;;
    esac
}

make_merge_fixture() {
    local launcher=$1 file label
    shift
    rm -rf -- "$home/data" "$home/config" "$home/cache" "$home/tmp" "$home/runtime" "$repo" \
        "$home/mergetool.log" "$home/mergetool.status" || return 1
    mkdir -p -- "$home/data" "$home/config" "$home/cache" "$home/tmp" "$home/runtime" "$repo" || return 1
    chmod 700 "$home/runtime" || return 1
    {
        printf '[mergetool "truce"]\n'
        printf '\tcmd = MERGED=\\"$MERGED\\" BASE=\\"$BASE\\" LOCAL=\\"$LOCAL\\" REMOTE=\\"$REMOTE\\" '\''%s'\'' --mergetool\n' "$launcher"
        cat <<'EOF'
	trustExitCode = true
[mergetool]
	keepBackup = false
	keepTemporaries = false
[core]
	autocrlf = false
[init]
	defaultBranch = main
[user]
	name = Branch Truce Validation
	email = validation@example.invalid
EOF
    } > "$home/gitconfig" || return 1
    isolated git init --quiet || return 1
    for label in base side main; do
        if [[ "$label" == side ]]; then
            isolated git checkout --quiet -b side || return 1
        elif [[ "$label" == main ]]; then
            isolated git checkout --quiet main || return 1
        fi
        for file in "$@"; do
            merge_fixture_content "$file" "$label" > "$repo/$file" || return 1
        done
        isolated git add -- "$@" && isolated git commit --quiet -m "$label" || return 1
    done
    ! isolated git merge --no-edit side >/dev/null 2>&1
}
