#!/bin/sh
# Set or check the version. It is kept in four places, which have to match:
#   ConfigSettings.h  FW_VERSION "vX.Y.Z"   firmware version
#   data/appversion   X.Y.Z                 application (LittleFS) version
#   data/index.js     appVersion = 'vX.Y.Z' version the web page expects
#   data/index.html   ?v=X.Y.Z              cache busting for the page's assets
#
# usage: tools/version.sh set X.Y.Z   update all four places
#        tools/version.sh check [tag] fail unless all four match, and match the tag (vX.Y.Z) if given
set -e
cd "$(dirname "$0")/.."

case "$1" in
  set)
    v=${2:?version, e.g. 2.4.10}
    echo "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+[a-z]?$' || { echo "Not a version: $v" >&2; exit 1; }
    sed -i "s/^#define FW_VERSION \"v[^\"]*\"/#define FW_VERSION \"v$v\"/" ConfigSettings.h
    printf '%s' "$v" > data/appversion
    sed -i "s/appVersion = 'v[^']*'/appVersion = 'v$v'/" data/index.js
    sed -i "s/?v=[0-9][0-9a-z.]*\"/?v=$v\"/g" data/index.html
    "$0" check
    ;;
  check)
    fw=$(sed -n 's/^#define FW_VERSION "v\([^"]*\)".*/\1/p' ConfigSettings.h)
    app=$(cat data/appversion)
    js=$(sed -n "s/.*appVersion = 'v\([^']*\)'.*/\1/p" data/index.js)
    html=$(grep -o '?v=[0-9][0-9a-z.]*"' data/index.html | sed 's/?v=//; s/"//' | sort -u)
    echo "ConfigSettings.h $fw, data/appversion $app, data/index.js $js, data/index.html $(echo $html)"
    ok=1
    for x in "$app" "$js" "$html"; do [ "$x" = "$fw" ] || ok=0; done
    [ -n "$fw" ] || ok=0
    if [ -n "$2" ] && [ "$2" != "v$fw" ]; then echo "The tag $2 does not match v$fw" >&2; ok=0; fi
    [ $ok = 1 ] || { echo "The versions do not match, run tools/version.sh set X.Y.Z" >&2; exit 1; }
    ;;
  *)
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 1
    ;;
esac
