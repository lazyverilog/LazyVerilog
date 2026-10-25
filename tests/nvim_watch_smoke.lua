-- Headless Neovim smoke: the server's didChangeWatchedFiles registration
-- reaches a running watcher when started through the shipped plugin.
--
--   LAZYVERILOG_REPO=<repo> LAZYVERILOG_LSP=<lazyverilog-lsp> \
--     nvim --headless -u NONE -i NONE -c "luafile tests/nvim_watch_smoke.lua"
--
-- Exits 0 on success or when this Neovim cannot watch (no dynamic
-- registration offered, as on Linux with 0.10), 1 with a message on failure.
-- Registered with ctest only when nvim is on PATH (CMakeLists.txt).

local function fail(msg)
	io.stderr:write("FAIL: " .. msg .. "\n")
	vim.cmd("cquit 1")
end

local function main()
	local repo = assert(os.getenv("LAZYVERILOG_REPO"), "LAZYVERILOG_REPO unset")
	local server = assert(os.getenv("LAZYVERILOG_LSP"), "LAZYVERILOG_LSP unset")

	-- Count the directories Neovim starts watching instead of watching them.
	local ok_watch, watchfiles = pcall(require, "vim.lsp._watchfiles")
	if not ok_watch or watchfiles._watchfunc == nil then
		print("nvim-watch-smoke: skipped (no vim.lsp._watchfiles._watchfunc in this Neovim)")
		return vim.cmd("qa!")
	end
	local watched = {}
	watchfiles._watchfunc = function(path)
		table.insert(watched, path)
		return function() end
	end

	local dir = vim.fn.tempname()
	vim.fn.mkdir(dir, "p")
	vim.fn.writefile({ "module top;", "endmodule" }, dir .. "/top.sv")
	vim.cmd("cd " .. vim.fn.fnameescape(dir))

	vim.opt.rtp:prepend(repo)
	require("lazyverilog").setup({ cmd = server, inlay_hints = false, folding = false })
	vim.cmd("edit " .. vim.fn.fnameescape(dir .. "/top.sv"))

	local client
	if not vim.wait(10000, function()
		client = vim.lsp.get_clients({ name = "lazyverilog" })[1]
		return client ~= nil and client.initialized
	end, 50) then
		return fail("no initialized lazyverilog client")
	end

	local folders = client.workspace_folders or {}
	if #folders ~= 1 or folders[1].uri ~= vim.uri_from_fname(vim.fn.getcwd()) then
		return fail("expected the working directory as the one workspace folder, got "
			.. vim.inspect(folders))
	end

	local dynamic = vim.tbl_get(client.capabilities, "workspace", "didChangeWatchedFiles",
		"dynamicRegistration")
	if not dynamic then
		print("nvim-watch-smoke: skipped (this Neovim offers no dynamic watcher registration)")
		return vim.cmd("qa!")
	end

	if not vim.wait(10000, function() return #watched > 0 end, 50) then
		return fail("the server's file watcher registration started no watcher")
	end

	print("nvim-watch-smoke: ok (" .. #watched .. " watcher(s))")
	vim.cmd("qa!")
end

-- An error must exit nonzero rather than leave headless nvim waiting.
local ok, err = xpcall(main, debug.traceback)
if not ok then
	fail(tostring(err))
end
