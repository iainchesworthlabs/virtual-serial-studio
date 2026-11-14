# SPDX-License-Identifier: GPL-3.0-or-later
#
# Adapters of the control API for other languages.  The Python one is a client
# in adapters/python; its tests start real vss_agent and vss_gate processes
# (and make a token with vss_cli), so the test is added only when the tests
# and the programs are built and a Python 3.9 or later is found.
#

if(VSS_BUILD_TESTS AND TARGET vss_agent AND TARGET vss_gate AND TARGET vss_cli)
  find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
  # An interpreter that is found is not always one that runs: Windows has a
  # stub named python3 that only opens the Store.
  set(vss_python_runs 1)
  if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import sys"
      RESULT_VARIABLE vss_python_runs OUTPUT_QUIET ERROR_QUIET)
  endif()
  if(Python3_FOUND AND vss_python_runs EQUAL 0)
    # `cmake -E env` hands the programs to the tests, and keeps Python from
    # writing __pycache__ directories into the source tree.
    add_test(NAME python_adapter
      COMMAND ${CMAKE_COMMAND} -E env
              PYTHONDONTWRITEBYTECODE=1
              VSS_AGENT_EXE=$<TARGET_FILE:vss_agent>
              VSS_GATE_EXE=$<TARGET_FILE:vss_gate>
              VSS_CLI_EXE=$<TARGET_FILE:vss_cli>
              ${Python3_EXECUTABLE} -m unittest discover -v
              -s ${CMAKE_CURRENT_SOURCE_DIR}/python/tests)
    set_tests_properties(python_adapter PROPERTIES TIMEOUT 120)
  endif()
endif()
