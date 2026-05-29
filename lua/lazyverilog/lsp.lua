-- LSP client setup — starts the LazyVerilog server and attaches it to buffers.

local M = {}

local RELEASE_VERSION = require("lazyverilog.version")
local RELEASE_BASE_URL = "https://github.com/hxxdev/LazyVerilog/releases/download"

-- ---------------------------------------------------------------------------
-- Binary helpers
-- ---------------------------------------------------------------------------

local function _bin_dir()
	return vim.fn.stdpath("data") .. "/lazyverilog/bin"
end

local function _managed_bin()
	return _bin_dir() .. "/lazyverilog-lsp"
end

local function _platform()
	local uname = vim.uv.os_uname()
	local sys   = uname.sysname:lower()
	local arch  = uname.machine:lower()

	local os_part
	if sys:find("linux") then
		os_part = "linux"
	elseif sys:find("darwin") then
		os_part = "darwin"
	else
		return nil
	end

	local arch_part
	if arch == "x86_64" or arch == "amd64" then
		arch_part = "x64"
	elseif arch == "aarch64" or arch == "arm64" then
		arch_part = "arm64"
	else
		return nil
	end

	return os_part .. "-" .. arch_part
end

-- ---------------------------------------------------------------------------
-- Auto install
-- ---------------------------------------------------------------------------

local function _auto_install(on_done)
	local platform = _platform()
	if not platform then
		vim.notify("[LazyVerilog] unsupported platform", vim.log.levels.ERROR)
		return
	end

	local bin_dir  = _bin_dir()
	local bin_path = _managed_bin()
	local asset    = "lazyverilog-lsp-" .. RELEASE_VERSION .. "-" .. platform
	local url      = RELEASE_BASE_URL .. "/" .. RELEASE_VERSION .. "/" .. asset

	vim.fn.mkdir(bin_dir, "p")
	vim.notify("[LazyVerilog] downloading server binary…", vim.log.levels.INFO)
	vim.system({ "curl", "-fsSL", "-o", bin_path, url }, {}, function(dl)
		if dl.code ~= 0 then
			vim.schedule(function()
				vim.notify(
					"[LazyVerilog] download failed: " .. (dl.stderr or "unknown error"),
					vim.log.levels.ERROR
				)
			end)
			return
		end

		vim.system({ "chmod", "+x", bin_path }, {}, function(ch)
			vim.schedule(function()
				if ch.code ~= 0 then
					vim.notify("[LazyVerilog] chmod +x failed", vim.log.levels.ERROR)
					return
				end
				vim.notify("[LazyVerilog] server installed", vim.log.levels.INFO)
				on_done(bin_path)
			end)
		end)
	end)
end

-- ---------------------------------------------------------------------------
-- Command resolver (canonical format)
-- ---------------------------------------------------------------------------

local function resolve_cmd(cfg)
	-- Case 1: already a full command
	if type(cfg.cmd) == "table" then
		if type(cfg.cmd[1]) == "string" and vim.fn.executable(cfg.cmd[1]) == 1 then
			return cfg.cmd
		end
		return nil
	end

	-- Case 2: string executable
	if type(cfg.cmd) == "string" and cfg.cmd ~= "" then
		if vim.fn.executable(cfg.cmd) == 1 then
			local cmd = { cfg.cmd }
			vim.list_extend(cmd, cfg.cmd_args or {})
			return cmd
		end
		return nil
	end

	-- Case 3: fallback
	for _, candidate in ipairs({ "lazyverilog-lsp", _managed_bin() }) do
		if vim.fn.executable(candidate) == 1 then
			local cmd = { candidate }
			vim.list_extend(cmd, cfg.cmd_args or {})
			return cmd
		end
	end

	return nil
end

-- ---------------------------------------------------------------------------
-- Validation (prevents nested-table bug)
-- ---------------------------------------------------------------------------

local function validate_cmd(cmd)
	if type(cmd) ~= "table" then
		return false, "cmd must be a table"
	end

	if type(cmd[1]) ~= "string" then
		return false, "cmd[1] must be executable string"
	end

	for _, v in ipairs(cmd) do
		if type(v) ~= "string" then
			return false, "cmd must be flat string array (no nesting)"
		end
	end

	return true
end

-- ---------------------------------------------------------------------------
-- Root detection
-- ---------------------------------------------------------------------------

local function find_root(bufnr, markers)
	local path = vim.api.nvim_buf_get_name(bufnr)
	if path == "" then
		return vim.fn.getcwd()
	end
	local dir = vim.fn.fnamemodify(path, ":h")
	return vim.fs.root(dir, markers) or dir
end

-- ---------------------------------------------------------------------------
-- LSP start
-- ---------------------------------------------------------------------------

local function _default_on_attach(client, bufnr)
	local opts = { buffer = bufnr, silent = true }

	-- Inlay hints (Neovim >= 0.10)
	if vim.lsp.inlay_hint then
		vim.lsp.inlay_hint.enable(true, { bufnr = bufnr })
	end
end

local function start_lsp(cfg, cmd, bufnr)
	local ok, err = validate_cmd(cmd)
	if not ok then
		vim.notify("[LazyVerilog] Invalid cmd: " .. err, vim.log.levels.ERROR)
		return
	end

	bufnr                = bufnr or vim.api.nvim_get_current_buf()
	local root           = find_root(bufnr, cfg.root_markers)

	-- Wrap user on_attach with our defaults
	local user_on_attach = cfg.on_attach
	local function combined_on_attach(client, buf)
		_default_on_attach(client, buf)
		if user_on_attach then
			user_on_attach(client, buf)
		end
	end

	vim.lsp.start({
		name         = "lazyverilog",
		cmd          = cmd,
		cmd_env      = cfg.cmd_env,
		root_dir     = root,
		filetypes    = cfg.filetypes,
		capabilities = cfg.capabilities,
		on_attach    = combined_on_attach,
		settings     = {
			lazyverilog = {
				format = cfg.format,
			},
		},
		handlers     = {
			["window/showMessage"] = function(_, result, ctx, _)
				local name = "lazyverilog"
				local level_map = {
					[1] = vim.log.levels.ERROR,
					[2] = vim.log.levels.WARN,
					[3] = vim.log.levels.INFO,
					[4] = vim.log.levels.DEBUG,
				}
				local level = level_map[result.type] or vim.log.levels.INFO
				local hl = (result.type == 1) and "ErrorMsg" or "WarningMsg"
				-- Defer past the "N bytes written" cmdline message that fires after BufWritePre.
				vim.schedule(function()
					vim.notify(("[%s] %s"):format(name, result.message), level)
					vim.api.nvim_echo(
						{ { ("[%s] %s"):format(name, result.message), hl } },
						true, {}
					)
				end)
			end,
		},
		flags        = {
			debounce_text_changes = 150,
		},
	}, {
		bufnr = bufnr,
	})
end

-- ---------------------------------------------------------------------------
-- Public API
-- ---------------------------------------------------------------------------

function M.start(cfg, bufnr)
	local cmd = resolve_cmd(cfg)

	if cmd then
		start_lsp(cfg, cmd, bufnr)
	else
		_auto_install(function(bin_path)
			start_lsp(cfg, { bin_path }, bufnr) -- guaranteed flat
		end)
	end
end

return M
