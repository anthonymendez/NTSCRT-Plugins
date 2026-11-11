# Script mode (cmake -P) starts with old policies: use modern ones (IN_LIST etc.).
cmake_policy(VERSION 3.24)
# Run at build time (POST_BUILD of ntscrt_ofx) so whatever exists in
# resources/shaders *now* lands in the bundle.
#   -DBUNDLE=<.../NTSCRT.ofx.bundle> -DPRESETS=<dir> -DSHADERS=<dir>
#   -DLIBRASHADER=<file or empty> -DPLIST=<Info.plist or empty>
#   -DLICENSE_ROOT=<repo root: LICENSE, THIRD_PARTY_NOTICES.md, licenses/>
set(res "${BUNDLE}/Contents/Resources")
file(MAKE_DIRECTORY "${res}")

file(REMOVE_RECURSE "${res}/presets")
if(EXISTS "${PRESETS}")
  file(COPY "${PRESETS}/" DESTINATION "${res}/presets")
endif()

file(REMOVE_RECURSE "${res}/shaders")
if(IS_DIRECTORY "${SHADERS}")
  file(COPY "${SHADERS}/" DESTINATION "${res}/shaders")
else()
  file(MAKE_DIRECTORY "${res}/shaders")
endif()

if(LIBRASHADER AND EXISTS "${LIBRASHADER}")
  file(COPY "${LIBRASHADER}" DESTINATION "${res}")
endif()

# Plain-text version for installers and support ("which version do I have?").
if(VERSION_FILE AND EXISTS "${VERSION_FILE}")
  file(COPY "${VERSION_FILE}" DESTINATION "${res}")
endif()

# Licence texts (MIT for our code, the third-party notices, and every
# licence those notices refer to) travel with each binary copy.
file(REMOVE_RECURSE "${res}/licenses")
if(LICENSE_ROOT)
  foreach(f LICENSE THIRD_PARTY_NOTICES.md)
    if(NOT EXISTS "${LICENSE_ROOT}/${f}")
      message(FATAL_ERROR "bundle_resources: ${LICENSE_ROOT}/${f} missing")
    endif()
    file(COPY "${LICENSE_ROOT}/${f}" DESTINATION "${res}")
  endforeach()
  file(COPY "${LICENSE_ROOT}/licenses/" DESTINATION "${res}/licenses")
endif()

if(PLIST AND EXISTS "${PLIST}")
  file(COPY "${PLIST}" DESTINATION "${BUNDLE}/Contents")
endif()
