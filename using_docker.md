## Running docker

make -C docker <target>


## problems I had
for building black-parrot-tools/yosys-slang/, it throws errors because it is not using c++20 standard.

to fix this, I had to add `set(CMAKE_CXX_COMPILER "/usr/bin/g++")` into <...>/yosys-slang/third_party/slang/CmakeLists.txt