# compile options
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release  LLVM_INSTALL_DIR=/usr/lib/llvm-8/cmake  ..
ninja all
