-- Headless Neovim smoke: the shipped plugin names the opened file's git
-- checkout as the workspace folder (not the working directory), adds the
-- checkout of a buffer opened later, and the server's didChangeWatchedFiles
-- registration reaches a watcher on each of them.
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

	-- Two checkouts, and nvim started in neither: the workspace folder is the
	-- opened file's checkout, not the working directory.
	local base = vim.fn.tempname()
	local a, b, elsewhere = base .. "/a", base .. "/b", base .. "/elsewhere"
	for _, d in ipairs({ a .. "/.git", a .. "/rtl", b .. "/.git", elsewhere }) do
		vim.fn.mkdir(d, "p")
	end
	vim.fn.writefile({ "module top;", "endmodule" }, a .. "/rtl/top.sv")
	vim.fn.writefile({ "module other;", "endmodule" }, b .. "/other.sv")
	vim.cmd("cd " .. vim.fn.fnameescape(elsewhere))

	vim.opt.rtp:prepend(repo)
	require("lazyverilog").setup({ cmd = server, inlay_hints = false, folding = false })
	vim.cmd("edit " .. vim.fn.fnameescape(a .. "/rtl/top.sv"))

	local client
	if not vim.wait(10000, function()
		client = vim.lsp.get_clients({ name = "lazyverilog" })[1]
		return client ~= nil and client.initialized
	end, 50) then
		return fail("no initialized lazyverilog client")
	end

	local function folder_names()
		return vim.tbl_map(function(f)
			return vim.fs.normalize(vim.uri_to_fname(f.uri)):lower()
		end, client.workspace_folders or {})
	end
	local want_a = vim.fs.normalize(a):lower()
	local want_b = vim.fs.normalize(b):lower()
	if not vim.deep_equal(folder_names(), { want_a }) then
		return fail("expected the file's git checkout as the one workspace folder, got "
			.. vim.inspect(folder_names()))
	end

	local dynamic = vim.tbl_get(client.capabilities, "workspace", "didChangeWatchedFiles",
		"dynamicRegistration")
	local function watching(dir)
		for _, path in ipairs(watched) do
			if vim.fs.normalize(path):lower() == dir then
				return true
			end
		end
		return false
	end
	-- Let the first registration land before the second checkout opens, so
	-- the second watcher can only come from the server registering again.
	if dynamic and not vim.wait(10000, function() return watching(want_a) end, 50) then
		return fail("the server's file watcher registration started no watcher")
	end

	-- A buffer from a second checkout adds that checkout.
	vim.cmd("edit " .. vim.fn.fnameescape(b .. "/other.sv"))
	if not vim.wait(5000, function() return #folder_names() == 2 end, 50) then
		return fail("the second checkout was not added, got " .. vim.inspect(folder_names()))
	end
	if folder_names()[2] ~= want_b then
		return fail("expected " .. want_b .. " as the second folder, got "
			.. vim.inspect(folder_names()))
	end

	if not dynamic then
		print("nvim-watch-smoke: skipped watchers (this Neovim offers no dynamic watcher registration)")
		return vim.cmd("qa!")
	end
	if not vim.wait(10000, function() return watching(want_b) end, 50) then
		return fail("the second checkout is not watched: " .. vim.inspect(watched))
	end

	print("nvim-watch-smoke: ok (" .. #watched .. " watcher(s))")
	vim.cmd("qa!")
end

-- An error must exit nonzero rather than leave headless nvim waiting.
local ok, err = xpcall(main, debug.traceback)
if not ok then
	fail(tostring(err))
end
