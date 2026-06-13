-- LSP client setup — starts the LazyVerilog server and attaches it to buffers.

local M = {}

local RELEASE_VERSION = require("lazyverilog.version")
local RELEASE_CHECKSUMS = require("lazyverilog.checksums")
local RELEASE_BASE_URL = "https://github.com/lazyverilog/LazyVerilog/releases/download"

-- ---------------------------------------------------------------------------
-- lazyverilog.toml change notification
-- ---------------------------------------------------------------------------

-- One filesystem watcher per LSP root.  Neovim may attach the same LazyVerilog
-- client to many buffers in the same project, so the watcher must be keyed by
-- root directory rather than by buffer.  The watcher only sends an LSP
-- notification; the server owns the actual config reload and project reindex.
local config_watchers = {}

-- Debounce table keyed by root directory.  A single write often produces
-- several fs events (temporary file rename, chmod, content write, mtime update),
-- and BufWritePost can fire near the fs event as well.  Coalescing avoids
-- asking the server to reload the same lazyverilog.toml repeatedly.
local config_reload_pending = {}

local function _join_path(...)
	local parts = { ... }
	return table.concat(parts, "/")
end

local function _normalize_path(path)
	if vim.fs and vim.fs.normalize then
		return vim.fs.normalize(path)
	end
	return vim.fn.fnamemodify(path, ":p")
end

local function _path_is_at_or_under(path, root)
	path = path and _normalize_path(path)
	root = root and _normalize_path(root)
	if not path or path == "" or not root or root == "" then
		return false
	end
	if path == root then
		return true
	end
	return path:sub(1, #root + 1) == (root .. "/")
end

local function _client_root(client)
	if not client then
		return nil
	end
	local root = client.root_dir
		or (client.config and client.config.root_dir)
		or (client.workspace_folders
			and client.workspace_folders[1]
			and client.workspace_folders[1].name)
	if not root or root == "" then
		return nil
	end
	return _normalize_path(root)
end

local function _send_config_changed_to_client(client, changed_path, reason)
	-- Keep the LSP payload shape in one place so both notification sources use
	-- the exact same server contract:
	--
	--   * BufWritePost: explicit user save, sent immediately.
	--   * fs_event: external/noisy change source, sent through debounce.
	--
	-- The server reloads from disk; configFile only selects the correct
	-- lazyverilog.toml root when the LSP root and config root differ.
	client:notify("workspace/didChangeConfiguration", {
		settings = {
			lazyverilog = {
				configFile = changed_path,
				reason = reason,
			},
		},
	})
end

local function _notify_config_changed(root, changed_path, reason)
	root = root and _normalize_path(root)
	if not root or root == "" then
		return
	end

	if config_reload_pending[root] then
		return
	end
	config_reload_pending[root] = true

	vim.defer_fn(function()
		config_reload_pending[root] = nil

		local clients = vim.lsp.get_clients({ name = "lazyverilog" })
		for _, client in ipairs(clients) do
			if _client_root(client) == root then
				_send_config_changed_to_client(client, changed_path, reason)
			end
		end
	end, 150)
end

local function _start_config_watcher_for_root(root)
	root = root and _normalize_path(root)
	if not root or root == "" or config_watchers[root] then
		return
	end

	local config_path = _join_path(root, "lazyverilog.toml")
	if vim.fn.filereadable(config_path) ~= 1 then
		-- No file exists yet.  BufWritePost below still covers the common case
		-- where the user creates lazyverilog.toml inside Neovim.  Avoid watching
		-- the whole root directory here: large shared/HPC project directories can
		-- be noisy, and root-directory watchers are less portable than file
		-- watchers.
		return
	end

	local watcher = vim.uv.new_fs_event()
	if not watcher then
		return
	end

	local ok = watcher:start(config_path, {}, function(err, _filename, _events)
		if err then
			return
		end
		vim.schedule(function()
			_notify_config_changed(root, config_path, "lazyverilog.toml changed")
		end)
	end)

	if not ok then
		watcher:close()
		return
	end

	config_watchers[root] = watcher
end

-- ---------------------------------------------------------------------------
-- Binary helpers
-- ---------------------------------------------------------------------------

local function _bin_dir()
	return vim.fn.stdpath("data") .. "/lazyverilog/bin"
end

local function _managed_bin()
	return _bin_dir() .. "/lazyverilog-lsp"
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
					local digest = (result.stdout or ""):match("([0-9a-fA-F]+)")
					if digest and #digest == 64 then
						on_done(digest:lower(), nil)
						return
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

		local asset    = "lazyverilog-lsp-" .. RELEASE_VERSION .. "-" .. asset_platform
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

local function _default_on_attach(client, bufnr)
	local opts = { buffer = bufnr, silent = true }

	-- Inlay hints (Neovim >= 0.10)
	if vim.lsp.inlay_hint then
		vim.lsp.inlay_hint.enable(true, { bufnr = bufnr })
	end

	-- LSP-driven folding (Neovim >= 0.10)
	-- Sets foldmethod=expr so zM/za/zo work against LSP folding ranges.
	if vim.lsp.foldexpr then
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
	local root           = find_root(bufnr, cfg.root_markers)

	-- Wrap user on_attach with our defaults
	local user_on_attach = cfg.on_attach
	local function combined_on_attach(client, buf)
		_default_on_attach(client, buf)
		_start_config_watcher_for_root(_client_root(client))
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

--- Notify active LazyVerilog clients that a lazyverilog.toml file changed.
---
--- This is used by the plugin-level BufWritePost fallback in addition to the
--- libuv file watcher above.  It notifies clients whose detected root contains
--- the changed config file:
---
---   /proj/lazyverilog.toml       -> clients rooted at /proj
---   /proj/sub/lazyverilog.toml   -> clients rooted at /proj or /proj/sub
---
--- This is intentionally broader than exact `<root>/lazyverilog.toml` matching.
--- Neovim may root the LSP at a repository marker such as `.git`, while the
--- server discovers a nested lazyverilog.toml from an opened SystemVerilog
--- file.  If we kept exact matching, BufWritePost for that nested TOML would
--- silently drop the notification and formatter options would stay stale.
---
--- The notification carries the exact configFile path, so the server can switch
--- to that TOML's parent before reloading.  We still avoid unrelated workspaces
--- by requiring the changed TOML to live under the client's root.
function M.notify_config_changed_for_path(path, reason)
	if not path or path == "" then
		return
	end
	local changed_path = _normalize_path(path)
	if vim.fn.fnamemodify(changed_path, ":t") ~= "lazyverilog.toml" then
		return
	end

	local clients = vim.lsp.get_clients({ name = "lazyverilog" })
	local sent = false
	for _, client in ipairs(clients) do
		local root = _client_root(client)
		if root and _path_is_at_or_under(changed_path, root) then
			_start_config_watcher_for_root(root)
			-- BufWritePost is a precise save event from Neovim, not a noisy
			-- filesystem event.  Send it immediately and do not consult the
			-- debounce table used by libuv watchers; otherwise a stale/replaced
			-- file watcher or a burst of fs events can suppress a later explicit
			-- save and make the second config edit appear not to reload.
			_send_config_changed_to_client(
				client,
				changed_path,
				reason or "lazyverilog.toml saved"
			)
			sent = true
		end
	end

	if not sent and #clients == 1 then
		-- Recovery path for the common single-workspace case.  Some root
		-- combinations are hard to predict:
		--
		--   * user starts Neovim outside the repository,
		--   * the server later discovers lazyverilog.toml from didOpen,
		--   * client.root_dir remains the old start/root marker,
		--   * or path normalization differs across symlinks / mounted dirs.
		--
		-- Dropping the notification is worse than sending one explicit reload:
		-- the payload contains the exact configFile, and the server uses that
		-- path to select the TOML root before reloading.  Keep this fallback to
		-- one active LazyVerilog client so unrelated multi-workspace sessions do
		-- not get redirected accidentally.
		_send_config_changed_to_client(
			clients[1],
			changed_path,
			reason or "lazyverilog.toml saved"
		)
	end
end

return M
