# Helix fixed-point AAC decoder (RealNetworks, RPSL/RCSL; see readme.txt and source headers),
# taken from the ESP8266Audio project's src/libhelix-aac. SBR is disabled by default.
file(GLOB HELIX_AAC_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
idf_component_register(
    SRCS ${HELIX_AAC_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "compat"
)
# ESP_PLATFORM (set by ESP-IDF) selects the portable C arithmetic path in assembly.h.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function
    -Wno-maybe-uninitialized -Wno-implicit-fallthrough -Wno-unused-but-set-variable)
