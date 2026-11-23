# Whole-plugin tests: processor response / stress tests + editor snapshots (PNG).
#   StEqPluginTests [--snapshot <dir>] [--video <dir> [scene ...]]
# Links the plugin's shared-code library, so it exercises the real processor.
juce_add_console_app(StEqPluginTests PRODUCT_NAME "StEqPluginTests")
target_sources(StEqPluginTests PRIVATE PluginTests.cpp VideoRender.cpp)
target_compile_features(StEqPluginTests PRIVATE cxx_std_20)
target_link_libraries(StEqPluginTests PRIVATE STEQ)
target_include_directories(StEqPluginTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../Source")
target_include_directories(StEqPluginTests PRIVATE $<TARGET_PROPERTY:STEQ,INCLUDE_DIRECTORIES>)
target_compile_definitions(StEqPluginTests PRIVATE $<TARGET_PROPERTY:STEQ,COMPILE_DEFINITIONS> $<TARGET_PROPERTY:STEQ,INTERFACE_COMPILE_DEFINITIONS>)
if(APPLE)
    target_link_libraries(StEqPluginTests PRIVATE "-framework AudioToolbox")
endif()
