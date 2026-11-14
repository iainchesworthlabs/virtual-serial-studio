# SPDX-License-Identifier: GPL-3.0-or-later
#
# Language: choosing the catalogue from the system's languages, installing it
# and setting the layout direction.
#

add_library(vss_gui_language STATIC
  bidi.cpp
  bidi.hpp
  language_choice.cpp
  language_choice.hpp)

target_include_directories(vss_gui_language PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vss_gui_language PUBLIC Vss::core Qt6::Widgets)
target_compile_features(vss_gui_language PUBLIC cxx_std_23)
target_compile_definitions(vss_gui_language PRIVATE QT_NO_KEYWORDS)
vss_set_warnings(vss_gui_language)

add_subdirectory(tests)
