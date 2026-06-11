#!/bin/bash

echo "Building RetroMux..."
clang++ -std=c++23 -fsanitize=address -O3 -Werror main.cpp -o out \
  -ldrogon -ltrantor -ldatachannel -ltbb -lcrypto -lssl -ljsoncpp -lspdlog -lfmt

# Only execute if the compilation succeeded
if [ $? -eq 0 ]; then
    echo "Executing RetroMux..."
    # ⚡ Add the path right here before running the binary
    LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH ./out
else
    echo "[-] Build failed!"
fi
