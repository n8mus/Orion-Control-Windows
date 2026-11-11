#!/bin/sh
# Git Bash: fetch (SHA-256 checked), extract and patch rade_c + Opus + the
# Opus DNN model data for the Windows (MSVC) build of RADE.
#   sh fetch-and-patch.sh <work-dir>
# Leaves <work-dir>/src/rade_c and <work-dir>/src/opus ready to build.
set -e
W=${1:?usage: fetch-and-patch.sh <work-dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
RADE_REV=c8a3dc156045cae2cd251e1a4be0c304c9ddf2f9
RADE_SHA=bc89be47a141cca9da1e8cc73e1de9b6e96f05984fda5c30056760d69ba23bff
OPUS_REV=940d4e5af64351ca8ba8390df3f555484c567fbb
OPUS_SHA=20e37f9079ac2b80e3235cd8ce2547829e147bf44a2f5dd28a888fa3e9c24341
# The hash Opus's own autogen.sh hands to dnn/download_model.sh at this pin.
MODEL_SHA=4ed9445b96698bad25d852e912b41495ddfa30c8dbc8a55f9cde5826ed793453

fetch() {  # url file sha256
  [ -f "$2" ] || curl -fL --retry 3 -o "$2" "$1"
  echo "$3 *$2" | sha256sum -c -
}
mkdir -p "$W/dl" "$W/src"
cd "$W/dl"
fetch https://github.com/freedv/rade_c/archive/$RADE_REV.tar.gz rade_c.tar.gz $RADE_SHA
fetch https://github.com/xiph/opus/archive/$OPUS_REV.zip opus.zip $OPUS_SHA
fetch https://media.xiph.org/opus/models/opus_data-$MODEL_SHA.tar.gz opus_data-$MODEL_SHA.tar.gz $MODEL_SHA

cd "$W/src"
[ ! -e rade_c ] && [ ! -e opus ] || { echo "$W/src/rade_c or opus already exists"; exit 1; }
tar xzf ../dl/rade_c.tar.gz && mv rade_c-$RADE_REV rade_c
unzip -q ../dl/opus.zip && mv opus-$OPUS_REV opus

# Opus: the model data (what autogen.sh -> dnn/download_model.sh does), then
# rade_c's two patches (what rade_c's cmake/BuildOpus.cmake does).
cd opus
grep -q "$MODEL_SHA" autogen.sh
tar xzomf ../../dl/opus_data-$MODEL_SHA.tar.gz
patch dnn/nnet.h -i ../rade_c/src/opus-nnet.h.diff
patch dnn/nnet.c -i ../rade_c/src/opus-nnet.c.diff
cd ..

# rade_c: the one local patch (RADE_EXPORT overridable; see README.md).
patch -p1 -d rade_c -i "$HERE/rade_c-rade_api.h-static-export.diff"
echo "sources ready in $W/src"
