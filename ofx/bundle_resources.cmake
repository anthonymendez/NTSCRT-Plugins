# Run at build time (POST_BUILD of ntscrt_ofx) so whatever exists in
# resources/shaders *now* lands in the bundle.
#   -DBUNDLE=<.../NTSCRT.ofx.bundle> -DPRESETS=<dir> -DSHADERS=<dir>
#   -DLIBRASHADER=<file or empty> -DPLIST=<Info.plist or empty>
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

if(PLIST AND EXISTS "${PLIST}")
  file(COPY "${PLIST}" DESTINATION "${BUNDLE}/Contents")
endif()
