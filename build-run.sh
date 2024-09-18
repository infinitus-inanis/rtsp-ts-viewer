#!/bin/bash

pushd $(dirname $(realpath 0)) > /dev/null
  [ ! -f ./.build/rtsp-ts-viewer ] && {
    mkdir -p \.build
    pushd ./.build > /dev/null
      cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=1
      cmake --build . -j $(nproc --all)
    popd > /dev/null
  }
  [ ! -f ./.build/rtsp-ts-viewer ] && {
    echo "Build failed :(" >& 2
    exit 1
  }
  ./.build/rtsp-ts-viewer "$@"
popd > /dev/null