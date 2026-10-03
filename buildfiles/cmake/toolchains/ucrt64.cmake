# Use from the MSYS2 UCRT64 environment, or pass MSYS2_UCRT64_ROOT explicitly.
set(MSYS2_UCRT64_ROOT "C:/msys64/ucrt64" CACHE PATH "MSYS2 UCRT64 installation")
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER "${MSYS2_UCRT64_ROOT}/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "${MSYS2_UCRT64_ROOT}/bin/g++.exe")
set(CMAKE_RC_COMPILER "${MSYS2_UCRT64_ROOT}/bin/windres.exe")
list(PREPEND CMAKE_PREFIX_PATH "${MSYS2_UCRT64_ROOT}")
# This selects the compiler only. AppContainer compatibility requires a
# separate import/API audit and cannot be inferred from the UCRT runtime.
