#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct DesignConfig {
    std::string vcode;               // .f filelist path
    std::vector<std::string> define; // preprocessor defines
};

struct CompilationConfig {
    bool background_compilation{false};
    int background_compilation_threads{1};
    int background_compilation_debounce_ms{1500};
    int nice_value{10};
    bool log_timing{false};
};

struct InlayHintConfig {
    bool enable{true};
};

struct StatementOptions {
    bool align{false};
    bool align_adaptive{false};
    int lhs_min_width{1};
    bool begin_newline{false};
    bool wrap_end_else_clauses{false};
};

struct PortDeclarationOptions {
    bool align{true};
    bool align_adaptive{false};
    int section1_min_width{10};
    int section2_min_width{20};
    int section3_min_width{20};
    int section4_min_width{30};
    int section5_min_width{30};
};

struct VarDeclarationOptions {
    bool align{false};
    bool align_adaptive{false};
    int section1_min_width{0};
    int section2_min_width{30};
    int section3_min_width{30};
    int section4_min_width{0};
};

struct InstanceOptions {
    bool align{false};
    int port_indent_level{1};
    int instance_port_name_width{1};
    int instance_port_between_paren_width{0};
    bool align_adaptive{false};
};

struct FunctionOptions {
    std::string break_policy{"auto"};
    int line_length{100};
    int arg_count{-1};
    std::string layout{"block"};
    bool space_before_paren{false};
    bool space_inside_paren{false};
};

struct FunctionDeclarationOptions {
    std::string layout{"block"};
    int line_length{100};
    bool space_before_paren{false};
};

struct ModuleOptions {
    bool non_ansi_port_per_line_enabled{false};
    int non_ansi_port_per_line{1};
    bool non_ansi_port_max_line_length_enabled{false};
    int non_ansi_port_max_line_length{80};
    std::string parameter_layout{"block"};
};

struct EnumOptions {
    bool align{false};
    bool align_adaptive{false};
    int enum_name_min_width{1};
    int enum_value_min_width{0};
};

struct ModportOptions {
    bool align{false};
    bool align_adaptive{false};
    int direction_min_width{1};
    int signal_min_width{0};
};

struct SpacingOptions {
    bool control_keyword_space{true};
    bool space_inside_parens{false};
    bool space_inside_dimension_brackets{false};
    std::string binary_operator_spacing{"both"};
    std::string dimension_binary_operator_spacing{"none"};
    std::string semicolon_spacing{"after"};
    std::string range_colon_spacing{"none"};
    std::string indexed_part_select_spacing{"both"};
    std::string procedural_event_control_at_spacing{"before"};
    bool space_inside_event_control_parens{false};
    std::string assignment_operator_spacing{"both"};
};

struct MacroOptions {
    std::vector<std::string> object_like_expr;
    std::vector<std::string> function_like_expr;
    std::vector<std::string> statement_like{
        "uvm_info",
        "uvm_warning",
        "uvm_error",
        "uvm_fatal",
        "uvm_create",
        "uvm_create_on",
        "uvm_send",
        "uvm_send_pri",
        "uvm_rand_send",
        "uvm_rand_send_with",
        "uvm_rand_send_pri",
        "uvm_rand_send_pri_with",
        "uvm_do",
        "uvm_do_with",
        "uvm_do_on",
        "uvm_do_on_with",
        "uvm_do_pri",
        "uvm_do_pri_with",
        "uvm_do_on_pri",
        "uvm_do_on_pri_with",
        "uvm_field_int",
        "uvm_field_enum",
        "uvm_field_object",
        "uvm_field_array_int",
        "uvm_field_array_object",
        "uvm_field_queue_int",
        "uvm_field_queue_object",
        "uvm_field_string"};
    std::vector<std::string> declaration_like{
        "uvm_object_utils",          "uvm_component_utils",
        "uvm_object_param_utils",    "uvm_component_param_utils",
        "uvm_sequence_utils"};
    std::vector<std::string> control_flow_like;
    std::vector<std::string> block_begin_like{
        "uvm_object_utils_begin",       "uvm_component_utils_begin",
        "uvm_object_param_utils_begin", "uvm_component_param_utils_begin",
        "uvm_sequence_utils_begin"};
    std::vector<std::string> block_end_like{
        "uvm_object_utils_end", "uvm_component_utils_end", "uvm_object_param_utils_end",
        "uvm_component_param_utils_end", "uvm_sequence_utils_end"};
    std::vector<std::string> whitespace_sensitive{"DV_CHECK_FATAL"};
};

struct FormatOptions {
    int indent_size{2};
    int blank_lines_between_items{1};
    int default_indent_level_inside_outmost_block{1};
    bool tab_align{false};
    bool enable_format_on_save{false};
    bool safe_mode{false};
    std::string format_off_comment_pattern{R"(//\s*verilog[-_]format\s*:\s*off\b)"};
    std::string format_on_comment_pattern{R"(//\s*verilog[-_]format\s*:\s*on\b)"};
    std::string log_path;
    StatementOptions statement;
    PortDeclarationOptions port_declaration;
    VarDeclarationOptions var_declaration;
    InstanceOptions instance;
    FunctionOptions function_call;
    FunctionDeclarationOptions function_declaration;
    ModuleOptions module;
    EnumOptions enum_declaration;
    ModportOptions modport;
    SpacingOptions spacing;
    MacroOptions macros;
};

struct LintRuleConfig {
    bool enable{false};
    std::string severity{"warning"};
};

struct NamingConfig : LintRuleConfig {
    std::string module_pattern;
    std::string input_port_pattern;
    std::string output_port_pattern;
    std::string signal_pattern;
    std::string interface_pattern;
    std::string struct_pattern;
    std::string union_pattern;
    std::string enum_pattern;
    std::string parameter_pattern;
    std::string localparam_pattern;
    std::string register_pattern;
    bool check_module_filename{false};
    bool check_package_filename{false};
};

struct ModuleLintConfig : LintRuleConfig {
    bool one_module_per_file{false};
    std::string module_instantiation_style; // "positional" | "named" | "both" | ""
    bool stale_autoinst_diagnostic{false};
};

struct StatementLintConfig : LintRuleConfig {
    bool case_missing_default{false};
    bool latch_inference_detection{false};
    bool explicit_begin{false};
    bool no_raw_always{false};
    bool blocking_nonblocking_assignments{false};
};

struct FunctionLintConfig : LintRuleConfig {
    bool functions_automatic{false};
    std::string function_call_style; // "positional" | "named" | "both" | ""
    bool explicit_function_lifetime{false};
    bool explicit_task_lifetime{false};
};

struct StyleLintConfig {
    bool trailing_whitespace{false};
};

struct LintConfig {
    bool enable{true};
    NamingConfig naming;
    ModuleLintConfig module;
    StatementLintConfig statement;
    FunctionLintConfig function;
    StyleLintConfig style;
};

struct RtltreeOptions {
    bool show_instance_name{true};
    bool show_file{true};
};

/// Formatting options reserved for synthesized AutoInst text.
///
/// This is intentionally empty today: AutoInst currently inherits its concrete
/// text layout from the main formatter after generating the replacement block.
/// Keep the named option object in the public config shape so call sites remain
/// explicit about where future AutoInst-only knobs would flow, instead of
/// passing an anonymous placeholder or overloading unrelated formatter config.
struct AutoinstOptions {};

struct AutoargOptions {
    bool autoarg_on_save{false};
};

struct AutowireOptions {
    bool group_by_instance{false};
    bool sort_by_name{false};
};

struct AutoFuncOptions {
    int indent_size{4};
    bool use_named_arguments{true};
};

struct AutoffOptions {
    std::string register_pattern;
};

struct ConfigWarning {
    std::filesystem::path path;
    uint32_t line{0};   // 1-based; 0 when unavailable
    uint32_t column{0}; // 1-based; 0 when unavailable
    std::string message;
};

struct Config {
    DesignConfig design;
    CompilationConfig compilation;
    InlayHintConfig inlay_hint;
    FormatOptions format;
    LintConfig lint;
    RtltreeOptions rtltree;
    AutoinstOptions autoinst;
    AutoargOptions autoarg;
    AutowireOptions autowire;
    AutoFuncOptions autofunc;
    AutoffOptions autoff;
};

/// Load lazyverilog.toml from root directory. Returns defaults if not found.
/// Unknown keys are silently ignored. On TOML syntax error, returns defaults
/// and sets *warning to a human-readable message (if warning != nullptr).
/// If warning_detail is provided, it receives the TOML source position when available.
Config load_config(const std::filesystem::path& root, std::string* warning = nullptr,
                   ConfigWarning* warning_detail = nullptr);

/// Walk up from `start` (file or directory) looking for lazyverilog.toml.
/// Returns the directory containing it, or empty path if not found.
std::filesystem::path find_config_root(const std::filesystem::path& start);
