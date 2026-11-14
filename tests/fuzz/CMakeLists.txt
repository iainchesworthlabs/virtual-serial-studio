# SPDX-License-Identifier: GPL-3.0-or-later
#
# The fuzz harnesses live beside their libraries (libs/<lib>/fuzz/); they are
# declared here because enable_testing() is in effect in this directory.
#

vss_add_fuzz_target(fuzz_topology
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_config/fuzz/fuzz_topology.cpp"
  LIBS vss_config)

vss_add_fuzz_target(fuzz_endpoint_uri
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_ports/fuzz/fuzz_endpoint_uri.cpp"
  LIBS vss_ports)

vss_add_fuzz_target(fuzz_agent_frame
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_agent_proto/fuzz/fuzz_agent_frame.cpp"
  LIBS vss_agent_proto)

vss_add_fuzz_target(fuzz_plugin_manifest
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_plugin/fuzz/fuzz_plugin_manifest.cpp"
  LIBS vss_plugin)
vss_add_fuzz_target(fuzz_plugin_message
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_plugin/fuzz/fuzz_plugin_message.cpp"
  LIBS vss_plugin)
vss_add_fuzz_target(fuzz_rule_set
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_rules/fuzz/fuzz_rule_set.cpp"
  LIBS vss_rules)
vss_add_fuzz_target(fuzz_control_message
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_control_proto/fuzz/fuzz_control_message.cpp"
  LIBS vss_control_proto)
vss_add_fuzz_target(fuzz_licence
  SOURCE "${PROJECT_SOURCE_DIR}/libs/vss_license/fuzz/fuzz_licence.cpp"
  LIBS vss_license_test_keys)
