# shellcheck shell=bash
# Disposable application homes, merge-conflict repositories and installed-file
# checks, shared by the packaging checkers and the tools under tools/.
#
# Sourcing this file defines the functions below. It sets no shell options and
# runs nothing. Functions take everything they use as arguments, print their
# result on stdout, print diagnostics on stderr and return non-zero on failure.
#
#   fixture_safe_path PATH                 PATH is absolute, below a top-level
#                                          directory and free of "..", so it is
#                                          safe to empty
#   fixture_home HOME                      (re)creates HOME's data, config,
#                                          cache, tmp and runtime directories
#   fixture_env HOME                       NUL-separated NAME=VALUE pairs for
#                                          `env -i`: a minimal PATH, HOME, TMPDIR,
#                                          XDG directories under HOME and Git
#                                          reading only HOME/gitconfig
#   fixture_gitconfig HOME [LAUNCHER]      writes HOME/gitconfig with a commit
#                                          identity and, with LAUNCHER, the
#                                          `truce` mergetool that runs it
#   fixture_merge_repo HOME REPO FILE...   rebuilds REPO stopped on a merge of
#                                          branch side into main in which every
#                                          FILE conflicts
#   smoke_record_ready OUTPUT              OUTPUT of --smoke-test opened the
#                                          tracked.txt merge session
#   installed_files LIST [PREFIX]          the files the package installs, from
#                                          LIST (packaging/installed-files.txt),
#                                          below PREFIX (default /usr)
#   png_size FILE                          WIDTHxHEIGHT of a PNG file
#   icon_theme_entry INDEX DIR             "<Size> <Type>" when INDEX lists DIR
#   icon_lookup_ok INDEX RELATIVE          prints INDEX's entry for RELATIVE's
#                                          directory; true when it is declared
#                                          at its size, or as Scalable
#
# Fixture content follows the file name: *.md is Markdown, *.bin is binary (it
# holds NUL bytes) and anything else is a single line of text. Commits carry
# fixed dates, so commit ids repeat from run to run.

fixture_safe_path() {
    local path=${1-}
    case "$path" in
        /*/*) ;;
        *) echo "error: refusing to use '$path': not an absolute path below a top-level directory" >&2; return 2 ;;
    esac
    case "/$path/" in
        */../*|*/./*) echo "error: refusing to use '$path': it has a . or .. component" >&2; return 2 ;;
    esac
}

fixture_home() {
    local home=${1-} directory
    fixture_safe_path "$home" || return 2
    rm -rf -- "$home/data" "$home/config" "$home/cache" "$home/tmp" "$home/runtime" \
        "$home/mergetool.log" "$home/mergetool.status" || return 1
    for directory in data config cache tmp runtime; do
        mkdir -p -- "$home/$directory" || return 1
    done
    chmod 700 -- "$home/runtime"
}

fixture_env() {
    local home=${1-}
    fixture_safe_path "$home" || return 2
    printf '%s\0' "PATH=/usr/bin:/bin" "HOME=$home" "TMPDIR=$home/tmp" "LC_ALL=C.UTF-8" \
        "XDG_DATA_HOME=$home/data" "XDG_CONFIG_HOME=$home/config" "XDG_CACHE_HOME=$home/cache" \
        "XDG_RUNTIME_DIR=$home/runtime" "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=$home/gitconfig" \
        "GIT_TERMINAL_PROMPT=0"
}

fixture_gitconfig() {
    local home=${1-} launcher=${2-}
    fixture_safe_path "$home" || return 2
    {
        if [[ -n "$launcher" ]]; then
            printf '[mergetool "truce"]\n'
            printf '\tcmd = MERGED=\\"$MERGED\\" BASE=\\"$BASE\\" LOCAL=\\"$LOCAL\\" REMOTE=\\"$REMOTE\\" '\''%s'\'' --mergetool\n' "$launcher"
            printf '\ttrustExitCode = true\n[mergetool]\n\tkeepBackup = false\n\tkeepTemporaries = false\n'
        fi
        printf '[core]\n\tautocrlf = false\n[init]\n\tdefaultBranch = main\n'
        printf '[user]\n\tname = Branch Truce Fixture\n\temail = fixture@example.invalid\n'
    } > "$home/gitconfig"
}

# fixture_content FILE base|side|main
fixture_content() {
    case "$1" in
        *.md) printf '# Release notes\n\nThe heading and this line are the same on every branch.\n\n- %s wording for the summary\n- shared closing item\n' "$2" ;;
        *.bin) printf 'BT\0%s\0\n' "$2" ;;
        *) printf '%s\n' "$2" ;;
    esac
}

fixture_merge_repo() {
    local home=${1-} repo=${2-} label file conflicted expected
    shift 2 || { echo 'usage: fixture_merge_repo HOME REPO FILE...' >&2; return 2; }
    (($# > 0)) || { echo 'usage: fixture_merge_repo HOME REPO FILE...' >&2; return 2; }
    fixture_safe_path "$home" && fixture_safe_path "$repo" || return 2
    [[ -f "$home/gitconfig" ]] || fixture_gitconfig "$home" || return 1
    rm -rf -- "$repo" && mkdir -p -- "$repo" || return 1
    _fixture_git "$home" "$repo" init --quiet || return 1
    for label in base side main; do
        if [[ "$label" == side ]]; then
            _fixture_git "$home" "$repo" checkout --quiet -b side || return 1
        elif [[ "$label" == main ]]; then
            _fixture_git "$home" "$repo" checkout --quiet main || return 1
        fi
        for file in "$@"; do
            fixture_content "$file" "$label" > "$repo/$file" || return 1
        done
        _fixture_git "$home" "$repo" add -- "$@" && _fixture_git "$home" "$repo" commit --quiet -m "$label" || return 1
    done
    if _fixture_git "$home" "$repo" merge --no-edit side >/dev/null 2>&1; then
        echo "error: the fixture merge in $repo did not conflict" >&2
        return 1
    fi
    conflicted=$(_fixture_git "$home" "$repo" diff --name-only --diff-filter=U | LC_ALL=C sort)
    expected=$(printf '%s\n' "$@" | LC_ALL=C sort)
    [[ "$conflicted" == "$expected" ]] || {
        echo "error: the fixture merge in $repo left '${conflicted//$'\n'/ }' unmerged, not '${expected//$'\n'/ }'" >&2
        return 1
    }
}

# _fixture_git HOME REPO ARGS...: Git in REPO with only HOME's private
# environment and fixed commit dates, so commit ids repeat from run to run.
_fixture_git() {
    local home=$1 repo=$2
    shift 2
    local -a environment
    mapfile -d '' environment < <(fixture_env "$home")
    (cd -- "$repo" && env -i "${environment[@]}" GIT_AUTHOR_DATE=2000-01-01T00:00:00+0000 \
        GIT_COMMITTER_DATE=2000-01-01T00:00:00+0000 git "$@")
}

smoke_record_ready() {
    local record
    record=$(sed -n 's/^BRANCH_TRUCE_SESSION=//p' <<<"${1-}")
    [[ "$record" == *'"state":"ready"'* && "$record" == *'"operation":"Merge"'* \
        && "$record" == *'"path":"tracked.txt"'* ]]
}

installed_files() {
    local list=${1-} prefix=${2-/usr}
    [[ -f "$list" ]] || { echo "error: no installed-files list at '$list'" >&2; return 2; }
    awk -v prefix="$prefix" '!/^#/ && NF { print prefix "/" $0 }' "$list"
}

png_size() {
    local -a bytes
    [[ "$(od -An -tx1 -N16 -- "$1" 2>/dev/null | tr -d ' \n')" == 89504e470d0a1a0a0000000d49484452 ]] || return 1
    read -r -a bytes <<<"$(od -An -tu1 -j16 -N8 -- "$1" | tr '\n' ' ')"
    ((${#bytes[@]} == 8)) || return 1
    printf '%dx%d\n' $(((bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3])) \
        $(((bytes[4] << 24) | (bytes[5] << 16) | (bytes[6] << 8) | bytes[7]))
}

icon_theme_entry() {
    awk -v dir="$2" '
        /^\[.*\]$/ { section = substr($0, 2, length($0) - 2); next }
        section == "Icon Theme" && /^Directories=/ {
            n = split(substr($0, 13), names, ",")
            for (i = 1; i <= n; i++) if (names[i] == dir) listed = 1
        }
        section == dir && /^Size=/ { size = substr($0, 6) }
        section == dir && /^Type=/ { type = substr($0, 6) }
        END { if (listed) print size " " (type == "" ? "Threshold" : type) }' "$1"
}

icon_lookup_ok() {
    local dir=${2%/*} declared
    declared=$(icon_theme_entry "$1" "$dir")
    printf '%s\n' "$declared"
    if [[ "$2" == scalable/* ]]; then
        [[ "$declared" == *" Scalable" ]]
    else
        [[ "$declared" == "${dir%%x*} Fixed" || "$declared" == "${dir%%x*} Threshold" ]]
    fi
}
