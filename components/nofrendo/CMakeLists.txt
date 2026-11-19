file(GLOB SRCS *.c cpu/*.c libsnss/*.c nes/*.c sndhrdw/*.c mappers/*.c)

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS . cpu libsnss nes sndhrdw
                       LDFRAGMENTS linker.lf)

# Codigo C de 2001: silenciar avisos de compiladores modernos
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/nof_prelude.h
    -w -Wno-error -O3
    -fno-strict-aliasing
    -Wno-implicit-function-declaration
    -Wno-int-conversion
    -Wno-incompatible-pointer-types)
