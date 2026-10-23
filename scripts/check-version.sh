#!/bin/sh
# Fail if a user-visible version string disagrees with the VERSION file.
set -eu
cd "$(dirname "$0")/.."
version="$(cat VERSION)"
status=0
for file in app/web.html app/worker.c native/main.c; do
    if ! grep -qF "$version" "$file"; then
        echo "$file does not mention version $version" >&2
        status=1
    fi
done
if [ -n "${GITHUB_REF_TYPE:-}" ] && [ "$GITHUB_REF_TYPE" = tag ] && [ "$GITHUB_REF_NAME" != "v$version" ]; then
    echo "tag $GITHUB_REF_NAME does not match VERSION $version" >&2
    status=1
fi
exit $status
