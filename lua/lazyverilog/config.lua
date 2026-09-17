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

	-- No root_markers.  The server finds the project itself, by walking up from
	-- each opened file to the nearest lazyverilog.toml -- the same way clangd
	-- finds compile_commands.json.  An editor-side marker list could only
	-- disagree with it: vim.fs.root resolves markers by list order rather than
	-- proximity, so ".git" ahead of "lazyverilog.toml" rooted a monorepo at the
	-- top and the per-project config below was never read.
	--
	-- A root_markers passed to setup() is accepted and ignored, with one
	-- notification, so an existing config keeps working.

	-- Formatting and linting are configured in `lazyverilog.toml` at the
	-- project root, not here.  The server reads that file from disk and does
	-- not accept formatter options over LSP settings, so there is no plugin
	-- setting that mirrors them.  See the `[format]` section of
	-- lazyverilog.toml for the full list.

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
	if user.root_markers ~= nil and not M._warned_root_markers then
		-- Told once, not per buffer: resolve() runs on every attach.  Silence
		-- would be worse than a notification -- the option looks like it still
		-- works, and a user debugging why their config is not read would have no
		-- reason to suspect the setting they can plainly see.
		M._warned_root_markers = true
		vim.notify(
			"[LazyVerilog] root_markers is no longer used: the server finds the "
				.. "nearest lazyverilog.toml above each file itself. You can remove it.",
			vim.log.levels.WARN
		)
	end
	local cfg = vim.tbl_deep_extend("force", M.defaults, user)
	return cfg
end

return M
