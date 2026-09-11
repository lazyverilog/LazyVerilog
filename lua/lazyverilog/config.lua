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

	-- Formatting options forwarded to the LazyVerilog server.
	-- All fields are optional; unset fields use server defaults.
	format = {
		-- indent_size          = 4,
		-- use_tabs             = false,
		-- spaces_around_operators = true,
		-- space_after_comma    = true,
		-- align_port_declarations = true,
		-- port_newline         = true,
		-- max_line_length      = 120,
		-- blank_lines_between_items = 1,
	},

	-- Editor-side inlay hints (Neovim >= 0.10).
	--
	-- Separate from the server's own `[inlay_hint].enable` in lazyverilog.toml:
	-- that one decides whether the server produces hints, this one decides
	-- whether Neovim asks for them at all.  Turning the server's off while this
	-- stays on leaves Neovim requesting hints on every change and rendering an
	-- empty reply.
	inlay_hints = true,

	-- LSP-driven folding (Neovim >= 0.10).
	--
	-- Sets 'foldmethod=expr' with vim.lsp.foldexpr(), so zM/za/zo fold against
	-- the server's folding ranges instead of Vim's indent heuristic.  Neovim
	-- re-requests the whole file's folds from every didChange, so on very large
	-- RTL files -- or on a machine with little CPU to spare, such as a shared
	-- HPC node -- setting this to false is the cheapest way to take fold
	-- computation out of the edit loop entirely.
	folding = true,

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
