-- LSP client setup — starts the LazyVerilog server and attaches it to buffers.

local M = {}

local RELEASE_VERSION = require("lazyverilog.version")
local RELEASE_CHECKSUMS = require("lazyverilog.checksums")
local RELEASE_BASE_URL = "https://github.com/lazyverilog/LazyVerilog/releases/download"

-- ---------------------------------------------------------------------------
-- lazyverilog.toml change notification
-- ---------------------------------------------------------------------------

-- The server decides which lazyverilog.toml governs a file, by walking up from
-- the file itself.  Neovim does not, and must not: with no root_dir there is
-- nothing to key a watcher on any more, and guessing one is exactly what this
-- change removes.
--
-- What is left is the precise half.  BufWritePost tells us the exact config the
-- user just saved, and the server works out which projects that affects.  A
-- config edited outside Neovim is picked up by the server's own freshness
-- window instead of by a watcher here.

local function _normalize_path(path)
	if vim.fs and vim.fs.normalize then
		return vim.fs.normalize(path)
	end
	return vim.fn.fnamemodify(path, ":p")
end

local function _send_config_changed_to_client(client, changed_path, reason)
	-- configFile names the exact file that changed.  The server does not read it
	-- as "the project root is now here" -- it resolves that per file -- only as
	-- "this config is stale".
	client:notify("workspace/didChangeConfiguration", {
		settings = {
			lazyverilog = {
				configFile = changed_path,
				reason = reason,
			},
		},
	})
end

-- ---------------------------------------------------------------------------
-- Binary helpers
-- ---------------------------------------------------------------------------

local function _bin_dir()
	return vim.fn.stdpath("data") .. "/lazyverilog/bin"
end

local function _is_windows()
	return vim.uv.os_uname().sysname:lower():find("windows") ~= nil
end

local function _managed_bin()
	-- Windows executable lookup is most reliable when the managed binary keeps
	-- its native .exe suffix.  Unix-like platforms intentionally keep the
	-- historical extensionless path so existing installations continue to work.
	local suffix = _is_windows() and ".exe" or ""
	return _bin_dir() .. "/lazyverilog-lsp" .. suffix
end

local function _remove_file(path)
	if path and path ~= "" then
		vim.fn.delete(path)
	end
end

local function _expected_checksum(asset_platform)
	local by_version = RELEASE_CHECKSUMS[RELEASE_VERSION]
	if type(by_version) ~= "table" then
		return nil
	end

	local digest = by_version[asset_platform]
	if type(digest) ~= "string" then
		return nil
	end

	return digest:lower()
end

local function _sha256_file(path, on_done)
	-- Neovim does not expose a portable file-hash API.  Prefer standard
	-- command-line hashers that are already available on the supported release
	-- platforms:
	--
	--   * Linux: sha256sum
	--   * macOS: shasum -a 256
	--   * Windows: certutil -hashfile ... SHA256
	--   * Fallback: openssl dgst -sha256 -r
	--
	-- The command output is intentionally parsed as "first 64 hex characters"
	-- because the filename may contain spaces, temporary suffixes, or platform
	-- characters that should not matter to checksum verification.
	local commands = {}
	if vim.fn.executable("sha256sum") == 1 then
		table.insert(commands, { "sha256sum", path })
	end
	if vim.fn.executable("shasum") == 1 then
		table.insert(commands, { "shasum", "-a", "256", path })
	end
	if vim.fn.executable("certutil") == 1 then
		table.insert(commands, { "certutil", "-hashfile", path, "SHA256" })
	end
	if vim.fn.executable("openssl") == 1 then
		table.insert(commands, { "openssl", "dgst", "-sha256", "-r", path })
	end

	local function try_command(index)
		local command = commands[index]
		if not command then
			on_done(nil, "no SHA-256 tool found; install sha256sum, shasum, or openssl")
			return
		end

		vim.system(command, {}, function(result)
			-- `vim.system()` callbacks run in a fast-event context.  Schedule
			-- before invoking the caller's continuation because the installer
			-- continuation uses regular Vim APIs such as `vim.fn.rename()` and
			-- `vim.fn.delete()`, which are illegal from fast events.
			vim.schedule(function()
				if result.code == 0 then
					-- Some tools print labels before the digest, for example
					-- certutil starts with "SHA256 hash of ...".  Do not trust
					-- the first hex-looking token; scan until a full 64-digit
					-- SHA-256 candidate is found.
					local output = (result.stdout or "") .. (result.stderr or "")
					for digest in output:gmatch("[0-9a-fA-F]+") do
						if #digest == 64 then
							on_done(digest:lower(), nil)
							return
						end
					end
				end

				try_command(index + 1)
			end)
		end)
	end

	try_command(1)
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
	elseif sys:find("windows") then
		os_part = "windows"
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

local function _expected_checksums_for_platform(platform)
	local checksums = {}
	local expected = _expected_checksum(platform)
	if expected then
		table.insert(checksums, expected)
	end

	-- Linux may have installed the static fallback into the same managed path.
	local static_expected = platform and platform:find("^linux") and _expected_checksum(platform .. "-static") or nil
	if static_expected then
		table.insert(checksums, static_expected)
	end

	return checksums
end

local function _managed_bin_matches_release(on_done)
	local bin_path = _managed_bin()
	if vim.fn.executable(bin_path) ~= 1 then
		on_done(false)
		return
	end

	local platform = _platform()
	if not platform then
		on_done(true)
		return
	end

	local expected = _expected_checksums_for_platform(platform)
	if #expected == 0 then
		on_done(true)
		return
	end

	_sha256_file(bin_path, function(actual, _hash_err)
		if not actual then
			on_done(false)
			return
		end
		for _, digest in ipairs(expected) do
			if actual == digest then
				on_done(true)
				return
			end
		end
		on_done(false)
	end)
end

-- ---------------------------------------------------------------------------
-- Auto install
-- ---------------------------------------------------------------------------

-- Several startup events can try to start the LSP at almost the same time:
-- setup() scans existing buffers, BufReadPost / BufWinEnter may fire, and a
-- FileType event may also arrive.  Before the server binary exists, each of
-- those paths would otherwise conclude "no LSP client is attached" and launch
-- its own curl process.  Keep auto-install as a single-flight operation: the
-- first caller starts the download, later callers enqueue callbacks and reuse
-- the same installed binary when it finishes.
local install_in_progress = false
local install_waiters = {}

local function _flush_install_waiters(bin_path)
	local waiters = install_waiters
	install_waiters = {}
	for _, waiter in ipairs(waiters) do
		waiter(bin_path)
	end
end

local function _fail_install_waiters(message)
	install_in_progress = false
	install_waiters = {}
	vim.schedule(function()
		vim.notify(message, vim.log.levels.ERROR)
	end)
end

local function _auto_install(on_done)
	table.insert(install_waiters, on_done)

	if install_in_progress then
		return
	end
	install_in_progress = true

	local platform = _platform()
	if not platform then
		_fail_install_waiters("[LazyVerilog] unsupported platform")
		return
	end

	local bin_dir  = _bin_dir()
	local bin_path = _managed_bin()

	vim.fn.mkdir(bin_dir, "p")

	local function _finish()
		install_in_progress = false
		vim.schedule(function()
			vim.notify("[LazyVerilog] server installed", vim.log.levels.INFO)
			_flush_install_waiters(bin_path)
		end)
	end

	local function _download(asset_platform, on_success, on_compat_fail)
		local expected = _expected_checksum(asset_platform)
		if not expected then
			_fail_install_waiters(
				"[LazyVerilog] no trusted checksum for "
				.. RELEASE_VERSION
				.. " "
				.. asset_platform
				.. "; refusing to install downloaded binary"
			)
			return
		end

		local asset_suffix = asset_platform:find("^windows") and ".exe" or ""
		local asset    = "lazyverilog-lsp-" .. RELEASE_VERSION .. "-" .. asset_platform .. asset_suffix
		local url      = RELEASE_BASE_URL .. "/" .. RELEASE_VERSION .. "/" .. asset
		local tmp_path = bin_path .. ".download." .. tostring(vim.uv.hrtime())

		vim.system({ "curl", "-fsSL", "-o", tmp_path, url }, {}, function(dl)
			-- `vim.system()` callbacks are fast events.  The installer below
			-- calls Vimscript-backed helpers (`delete`, later `rename`) and must
			-- therefore run on the scheduled main loop.
			vim.schedule(function()
				if dl.code ~= 0 then
					_remove_file(tmp_path)
					_fail_install_waiters(
						"[LazyVerilog] download failed: "
						.. (dl.stderr or "unknown error")
						.. " URL: " .. url
					)
					return
				end

				_sha256_file(tmp_path, function(actual, hash_err)
					if not actual then
						_remove_file(tmp_path)
						_fail_install_waiters("[LazyVerilog] checksum failed: " .. hash_err)
						return
					end

					if actual ~= expected then
						_remove_file(tmp_path)
						_fail_install_waiters(
							"[LazyVerilog] checksum mismatch for "
							.. asset
							.. "; expected "
							.. expected
							.. ", got "
							.. actual
						)
						return
					end

					if vim.fn.rename(tmp_path, bin_path) ~= 0 then
						_remove_file(tmp_path)
						_fail_install_waiters("[LazyVerilog] failed to install verified binary")
						return
					end

					-- Windows executable permissions are encoded by the .exe
					-- file type rather than POSIX mode bits, and chmod is not a
					-- required tool there.  Skip directly to the compatibility
					-- checks after installing the verified file.
					if _is_windows() then
						on_success()
						return
					end

					vim.system({ "chmod", "+x", bin_path }, {}, function(ch)
						vim.schedule(function()
							if ch.code ~= 0 then
								_remove_file(bin_path)
								_fail_install_waiters("[LazyVerilog] chmod +x failed")
								return
							end
							-- On Linux, verify the binary's shared-library dependencies are
							-- satisfied before declaring success.  This catches glibc version
							-- mismatches without executing the server (which reads JSON-RPC
							-- from stdin and would hang).  macOS ships dyld which handles
							-- compatibility differently; skip the check there.
							local uname = vim.uv.os_uname()
							if uname.sysname:lower():find("linux") then
								vim.system({ "ldd", bin_path }, {}, function(ldd)
									vim.schedule(function()
										local ldd_output = (ldd.stdout or "") .. (ldd.stderr or "")
										if ldd_output:find("not found") then
											_remove_file(bin_path)
											on_compat_fail("binary not compatible with this system (missing libs)")
											return
										end
										on_success()
									end)
								end)
							else
								on_success()
							end
						end)
					end)
				end)
			end)
		end)
	end

	-- Linux has static fallback builds; macOS does not.
	local static_platform = platform:find("^linux") and (platform .. "-static") or nil

	vim.notify("[LazyVerilog] downloading server binary…", vim.log.levels.INFO)
	_download(platform, _finish, function(err)
		if not static_platform then
			_fail_install_waiters("[LazyVerilog] " .. err)
			return
		end
		vim.schedule(function()
			vim.notify(
				"[LazyVerilog] " .. err .. ", trying static build…",
				vim.log.levels.WARN
			)
		end)
		_download(static_platform, _finish, function(err2)
			_fail_install_waiters("[LazyVerilog] " .. err2)
		end)
	end)
end

-- ---------------------------------------------------------------------------
-- Command resolver (canonical format)
-- ---------------------------------------------------------------------------

local function resolve_external_cmd(cfg)
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

	-- Case 3: PATH fallback. Managed binaries are resolved asynchronously below
	-- so we can verify their checksum against the current plugin release.
	local candidates = { "lazyverilog-lsp" }
	if _is_windows() then
		table.insert(candidates, 2, "lazyverilog-lsp.exe")
	end
	for _, candidate in ipairs(candidates) do
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
-- LSP start
-- ---------------------------------------------------------------------------

local function _configure_completion_options(bufnr)
	-- Option A for snippet completions:
	--
	-- Keep LazyVerilog free to return LSP Snippet completion items, but avoid
	-- Neovim's completion preview paths that can Tree-sitter-highlight transient
	-- snippet/documentation popup buffers while the user moves through the popup
	-- menu with arrow keys.
	--
	-- Why buffer-local?
	--   'completeopt' is global-local.  Applying this only to buffers where the
	--   LazyVerilog LSP attaches avoids changing unrelated languages.
	--
	-- Why remove both "popup" and "preview"?
	--   - "popup" asks Neovim's built-in LSP completion to show extra info in a
	--     floating popup and, for snippet items, synthesize snippet preview info.
	--   - "preview" uses the preview window for completion info.
	--   Both can redraw/highlight auxiliary completion text while candidates are
	--   selected, which is the path that exposed the nvim-treesitter
	--   `conceal_line` / `node:range()` crash.
	--
	-- Why add "noinsert,noselect"?
	--   They keep completion selection passive: opening or moving in the menu
	--   does not pre-insert candidate text into the source buffer.  Snippet
	--   expansion still happens when the user accepts a completion.
	vim.api.nvim_buf_call(bufnr, function()
		local drop = {
			popup   = true,
			preview = true,
		}
		local required_order = { "menu", "menuone", "noinsert", "noselect" }
		local required = {}
		for _, opt in ipairs(required_order) do
			required[opt] = true
		end

		local next_opts = {}
		local seen = {}

		-- Preserve unrelated user choices such as "fuzzy" or "nosort", but
		-- remove preview-producing options and avoid duplicating required ones.
		for _, opt in ipairs(vim.opt_local.completeopt:get()) do
			if opt ~= "" and not drop[opt] and not required[opt] and not seen[opt] then
				table.insert(next_opts, opt)
				seen[opt] = true
			end
		end

		for _, opt in ipairs(required_order) do
			if not seen[opt] then
				table.insert(next_opts, opt)
				seen[opt] = true
			end
		end

		vim.opt_local.completeopt = next_opts
	end)
end

local function _default_on_attach(cfg, client, bufnr)
	-- Inlay hints (Neovim >= 0.10)
	--
	-- Enabling this makes Neovim request hints for the whole buffer on every
	-- didChange, so it is a per-keystroke cost even when the server is
	-- configured to return none.  See `inlay_hints` in config.lua.
	if cfg.inlay_hints ~= false and vim.lsp.inlay_hint then
		vim.lsp.inlay_hint.enable(true, { bufnr = bufnr })
	end

	-- LSP-driven folding (Neovim >= 0.10)
	-- Sets foldmethod=expr so zM/za/zo work against LSP folding ranges.
	--
	-- Neovim asks for the whole file's folding ranges from the didChange
	-- notification itself, so this too is paid once per edit.  See `folding` in
	-- config.lua for why a large file on a busy machine may want it off.
	if cfg.folding ~= false and vim.lsp.foldexpr then
		vim.api.nvim_buf_call(bufnr, function()
			vim.wo.foldmethod = "expr"
			vim.wo.foldexpr   = "v:lua.vim.lsp.foldexpr()"
			vim.wo.foldlevel  = 99 -- start fully open; use zM to close all
		end)
	end

	_configure_completion_options(bufnr)
end

local function start_lsp(cfg, cmd, bufnr)
	local ok, err = validate_cmd(cmd)
	if not ok then
		vim.notify("[LazyVerilog] Invalid cmd: " .. err, vim.log.levels.ERROR)
		return
	end

	bufnr                = bufnr or vim.api.nvim_get_current_buf()

	-- Wrap user on_attach with our defaults
	local user_on_attach = cfg.on_attach
	local function combined_on_attach(client, buf)
		_default_on_attach(cfg, client, buf)
		if user_on_attach then
			user_on_attach(client, buf)
		end
	end

	vim.lsp.start({
		name         = "lazyverilog",
		cmd          = cmd,
		cmd_env      = cfg.cmd_env,
		-- No root_dir on purpose.  It is what the server used to be told its
		-- project was, and Neovim's answer was a guess from a marker list
		-- resolved by marker order rather than proximity -- a .git at the top of
		-- a monorepo outranking the lazyverilog.toml next to the file.  The
		-- server now walks up from each file itself.
		--
		-- Leaving it nil also means one client for every SystemVerilog buffer in
		-- the session rather than one per guessed root, which is what lets a
		-- single server serve files from several projects at once.
		filetypes    = cfg.filetypes,
		capabilities = cfg.capabilities,
		on_attach    = combined_on_attach,
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
	local cmd = resolve_external_cmd(cfg)

	if cmd then
		start_lsp(cfg, cmd, bufnr)
	else
		local bin_path = _managed_bin()
		_managed_bin_matches_release(function(matches)
			if matches then
				start_lsp(cfg, { bin_path }, bufnr) -- guaranteed flat
				return
			end

			if vim.fn.filereadable(bin_path) == 1 then
				_remove_file(bin_path)
			end
			_auto_install(function(installed_bin_path)
				start_lsp(cfg, { installed_bin_path }, bufnr) -- guaranteed flat
			end)
		end)
	end
end

--- Tell the server that a lazyverilog.toml was saved.
---
--- Every LazyVerilog client is notified, with no attempt to work out which one
--- the file belongs to.  That filtering used to live here, along with a
--- single-client recovery path for when it guessed wrong -- both of them
--- consequences of the editor owning the root.  The server resolves a config
--- to the projects it governs by walking the tree it is actually in, so the
--- only thing worth sending is which file changed.
function M.notify_config_changed_for_path(path, reason)
	if not path or path == "" then
		return
	end
	local changed_path = _normalize_path(path)
	if vim.fn.fnamemodify(changed_path, ":t") ~= "lazyverilog.toml" then
		return
	end

	for _, client in ipairs(vim.lsp.get_clients({ name = "lazyverilog" })) do
		_send_config_changed_to_client(client, changed_path, reason or "lazyverilog.toml saved")
	end
end

return M
