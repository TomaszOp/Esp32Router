FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/include")

# Hak IP4_INPUT (zapora, ping z WAN, źródło przekierowań) – include/lwip_hooks.h.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_options(${lwip_lib} PRIVATE "-I${CMAKE_SOURCE_DIR}/include")
target_compile_definitions(${lwip_lib} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"lwip_hooks.h\"")
