# vi:set sw=2 et:
# Oscairn Analog: Surge XT's engine as a CLAP instrument for the Oscairn host
# (doc/oscairn-plan.md). Build with SURGE_SKIP_LUA and SURGE_SKIP_ODDSOUND_MTS for the
# configuration the plan describes; the plugin refuses Formula modulators either way.
project(oscairn-analog)

add_library(oscairn-analog-core STATIC OscairnAnalog.cpp OscairnAnalog.h)
target_include_directories(oscairn-analog-core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SURGE_SOURCE_DIR}/libs/clap-juce-extensions/clap-libs/clap/include)
target_link_libraries(oscairn-analog-core PUBLIC surge::surge-common)

add_library(oscairn-analog MODULE OscairnAnalogEntry.cpp)
target_link_libraries(oscairn-analog PRIVATE oscairn-analog-core)
set_target_properties(oscairn-analog PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

add_executable(oscairn-analog-tests tests/OscairnAnalogTests.cpp)
target_link_libraries(oscairn-analog-tests PRIVATE oscairn-analog-core)
target_compile_definitions(oscairn-analog-tests PRIVATE
  SURGE_FACTORY_PATCHES="${SURGE_SOURCE_DIR}/resources/data/patches_factory")
