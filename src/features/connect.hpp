#pragma once

#include "analyzer.hpp"
#include <string>
#include <vector>

/// Helpers for the interactive Connect / Interface Neovim commands.
///
/// These functions deliberately return already-serialized JSON because the
/// server's executeCommand implementation mostly emits command-specific JSON
/// strings manually. Keeping that boundary here avoids leaking LspCpp protocol
/// types into the feature logic and makes the code easy to unit-test by parsing
/// the returned JSON if needed.
std::string connect_info_json(const Analyzer& analyzer, const std::string& uri);
std::string connect_apply_preview_json(const Analyzer& analyzer, const std::string& uri,
                                       const std::string& source_path,
                                       const std::string& source_port,
                                       const std::string& dest_path,
                                       const std::string& dest_port,
                                       const std::string& wire_name,
                                       const std::vector<std::string>& source_boundary_ports = {},
                                       const std::vector<std::string>& dest_boundary_ports = {});
std::string connect_apply_edit_json(const Analyzer& analyzer, const std::string& uri,
                                    const std::string& source_path,
                                    const std::string& source_port,
                                    const std::string& dest_path,
                                    const std::string& dest_port,
                                    const std::string& wire_name,
                                    const std::vector<std::string>& source_boundary_ports = {},
                                    const std::vector<std::string>& dest_boundary_ports = {});

std::string interface_json(const Analyzer& analyzer, const std::string& uri,
                           const std::string& inst1_name, const std::string& inst2_name);
std::string single_interface_json(const Analyzer& analyzer, const std::string& uri,
                                  const std::string& inst_name);
std::string interface_connect_edit_json(const Analyzer& analyzer, const std::string& uri,
                                        const std::string& inst1_name,
                                        const std::string& inst2_name,
                                        const std::string& inst1_port,
                                        const std::string& inst2_port,
                                        const std::string& wire_name,
                                        const std::string& wire_type);
std::string interface_disconnect_edit_json(const Analyzer& analyzer, const std::string& uri,
                                           const std::string& inst1_name,
                                           const std::string& inst2_name,
                                           const std::string& inst1_port,
                                           const std::string& inst2_port,
                                           const std::string& signal_name);
