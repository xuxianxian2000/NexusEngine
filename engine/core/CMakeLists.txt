add_library(nexus-core STATIC
    src/log.cpp
    src/memory.cpp
    src/timer.cpp
    src/event_bus.cpp
    src/vfs.cpp
    src/job_system.cpp
    src/application.cpp
    src/crash_handler.cpp
)

target_include_directories(nexus-core
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(nexus-core
    PUBLIC  spdlog::spdlog nlohmann_json::nlohmann_json
)

# Link GLM as SYSTEM to suppress warnings from its headers
set_target_properties(glm PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    "$<TARGET_PROPERTY:glm,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(nexus-core PUBLIC glm::glm)

# Enable GLM experimental extensions
target_compile_definitions(nexus-core PUBLIC GLM_ENABLE_EXPERIMENTAL)

nexus_set_compiler_flags(nexus-core)
nexus_set_build_defines(nexus-core)

add_library(Nexus::Core ALIAS nexus-core)
