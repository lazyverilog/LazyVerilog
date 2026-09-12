--- Default configuration for LazyVerilog.
--- Users override these via require('lazyverilog').setup({ … })

local M = {}

M.defaults = {
	-- Path to the lazyverilog-lsp executable.
	-- If nil, the plugin looks for it on $PATH.
	cmd = nil,

	-- Extra arguments passed to the server process.
	cmd_args = {},

	-- File types handled by this LSP.
	filetypes = { "systemverilog", "verilog" },

	-- Root directory markers used to detect the project root.
	root_markers = { ".git", "lazyverilog.toml" },

	-- Formatting and linting are configured in `lazyverilog.toml` at the
	-- project root, not here.  The server reads that file from disk and does
	-- not accept formatter options over LSP settings, so there is no plugin
	-- setting that mirrors them.  See the `[format]` section of
	-- lazyverilog.toml for the full list.

	-- nvim-lspconfig / vim.lsp.start options forwarded verbatim.
	on_attach = nil,
	capabilities = nil,
}

--- Merge user config on top of defaults (shallow for nested tables).
---@param user table
---@return table
function M.resolve(user)
	user = user or {}
	local cfg = vim.tbl_deep_extend("force", M.defaults, user)
	return cfg
end

return M
