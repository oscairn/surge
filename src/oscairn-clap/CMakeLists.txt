# vi:set sw=2 et:
# Oscairn Analog: Surge XT's engine as a CLAP instrument for the Oscairn host
# (doc/oscairn-plan.md). Build with SURGE_SKIP_LUA for the configuration the plan describes;
# the plugin refuses Formula modulators either way. MTS-ESP stays for now (plan, section 10).
project(oscairn-analog)

add_library(oscairn-analog-core STATIC OscairnAnalog.cpp OscairnAnalog.h)
target_include_directories(oscairn-analog-core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/oscairn-clap/include
  ${SURGE_SOURCE_DIR}/libs/clap-juce-extensions/clap-libs/clap/include)
target_link_libraries(oscairn-analog-core PUBLIC surge::surge-common)

add_library(oscairn-analog MODULE OscairnAnalogEntry.cpp)
target_link_libraries(oscairn-analog PRIVATE oscairn-analog-core)
set_target_properties(oscairn-analog PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

add_executable(oscairn-analog-tests tests/OscairnAnalogTests.cpp tools/RenderNote.cpp)
target_link_libraries(oscairn-analog-tests PRIVATE oscairn-analog-core)
target_compile_definitions(oscairn-analog-tests PRIVATE
  SURGE_FACTORY_PATCHES="${SURGE_SOURCE_DIR}/resources/data/patches_factory")

# Renders one note through the plugin, for measuring the engine (doc/oscairn-plan.md, M1).
add_executable(oscairn-analog-render tools/OscairnAnalogRender.cpp tools/RenderNote.cpp)
target_link_libraries(oscairn-analog-render PRIVATE oscairn-analog-core)

# Times every block of a patch file played as Oscairn plays it (CPU per voice, stalls).
add_executable(oscairn-analog-bench tools/OscairnAnalogBench.cpp)
target_link_libraries(oscairn-analog-bench PRIVATE oscairn-analog-core)

# Fits the filter cutoff rate match tables (src/common/dsp/FilterCutoffRateMatchTables.h).
add_executable(oscairn-analog-rate-match tools/OscairnAnalogRateMatch.cpp tools/RenderNote.cpp)
target_link_libraries(oscairn-analog-rate-match PRIVATE oscairn-analog-core)

# Opens the plugin as Oscairn does, for tests/privacy-check.sh: nothing may touch the user's
# Documents, Desktop or Downloads, or create anything under HOME.
add_executable(oscairn-analog-privacy-host tests/privacy-host.cpp)
target_include_directories(oscairn-analog-privacy-host PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/oscairn-clap/include
  ${SURGE_SOURCE_DIR}/libs/clap-juce-extensions/clap-libs/clap/include)
target_link_libraries(oscairn-analog-privacy-host PRIVATE ${CMAKE_DL_LIBS})
