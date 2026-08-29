idf_component_register(
    SRCS
        "bmi270/bmi2.c"
        "bmi270/bmi270.c"
        "bmm150/bmm150.c"
    INCLUDE_DIRS
        "bmi270"
        "bmm150"
)

# The upstream files are kept byte-for-byte so Bosch release updates remain
# auditable. Keep project warning policy from turning vendor diagnostics into
# local source obligations.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
target_compile_definitions(${COMPONENT_LIB} PUBLIC BMM150_USE_FLOATING_POINT)
