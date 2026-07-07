# ESP-IDF component: dtn_core
# Wraps the shared C99 sources so they compile with the IDF toolchain.

set(COMPONENT_SRCS
    "../dtn_core/types.c"
    "../dtn_core/bundle.c"
    "../dtn_core/contact.c"
    "../dtn_core/custody.c"
    "../dtn_core/queue.c"
    "../dtn_core/cgr.c"
    "../dtn_core/disruption.c"
    "../dtn_core/trace.c"
)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "../dtn_core"
)