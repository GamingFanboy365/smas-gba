# Bare-metal GBA toolchain: clang and lld only, no sysroot, no GCC.
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/gba-toolchain.cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# The compiler: -DGBA_CLANG=clang-20 (a name on the PATH, or a full path)
# picks one; otherwise the first of these found.
if(GBA_CLANG AND NOT IS_ABSOLUTE "${GBA_CLANG}")
  find_program(GBA_CLANG_FOUND NAMES "${GBA_CLANG}" REQUIRED NO_CACHE)
  set(GBA_CLANG "${GBA_CLANG_FOUND}" CACHE FILEPATH "clang for the GBA build" FORCE)
endif()
find_program(GBA_CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 REQUIRED)
set(CMAKE_C_COMPILER ${GBA_CLANG})
set(CMAKE_ASM_COMPILER ${GBA_CLANG})
set(CMAKE_C_COMPILER_TARGET armv4t-none-eabi)
set(CMAKE_ASM_COMPILER_TARGET armv4t-none-eabi)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT "-mcpu=arm7tdmi -ffreestanding -fomit-frame-pointer -fno-common")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=arm7tdmi")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -nostdlib -static")

find_program(GBA_OBJCOPY NAMES llvm-objcopy llvm-objcopy-20 llvm-objcopy-19 llvm-objcopy-18
             llvm-objcopy-17 REQUIRED)
find_program(GBA_NM NAMES llvm-nm llvm-nm-20 llvm-nm-19 llvm-nm-18 llvm-nm-17)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
