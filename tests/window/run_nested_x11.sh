#!/usr/bin/env bash
# Isolate native input injection from the user's desktop. Prefer xvfb-run in
# CMake; Xephyr provides the same isolation when only a desktop X server exists.
set -eu
if [[ -z "${DISPLAY:-}" ]]; then
    echo "Native wheel test skipped: Xephyr requires DISPLAY"
    exit 77
fi
test_dir=$(mktemp -d)
server_pid=
cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    # Only the two files made by this test, in its unique directory.
    rm -f "$test_dir/display" "$test_dir/server.log"
    rmdir "$test_dir"
}
trap cleanup EXIT
"$1" -displayfd 3 -screen 640x480 -noreset -nolisten tcp \
    3>"$test_dir/display" >"$test_dir/server.log" 2>&1 &
server_pid=$!
for ((attempt = 0; attempt < 100; ++attempt)); do
    if [[ -s "$test_dir/display" ]]; then
        read -r display_number < "$test_dir/display"
        DISPLAY=":$display_number" "$2"
        exit $?
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "Native wheel test skipped: nested X server could not start"
        sed -n '1,30p' "$test_dir/server.log"
        exit 77
    fi
    sleep .05
done
echo "Native wheel test skipped: nested X server did not become ready"
exit 77
