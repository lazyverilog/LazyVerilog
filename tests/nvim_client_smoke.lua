-- Headless Neovim smoke for the shipped plugin against the real server.
--
--   LAZYVERILOG_REPO=<repo> LAZYVERILOG_LSP=<lazyverilog-lsp> \
--     nvim --headless -u NONE -i NONE -c "luafile tests/nvim_client_smoke.lua"
--
-- Exits 0 on success, 1 with a message on failure.  Registered with ctest
-- only when nvim is on PATH (CMakeLists.txt).

local function fail(msg)
	io.stderr:write("FAIL: " .. msg .. "\n")
	vim.cmd("cquit 1")
end

local function clients()
	return vim.lsp.get_clients({ name = "lazyverilog" })
end

local function main()
	local repo = assert(os.getenv("LAZYVERILOG_REPO"), "LAZYVERILOG_REPO unset")
	local server = assert(os.getenv("LAZYVERILOG_LSP"), "LAZYVERILOG_LSP unset")

	local dir = vim.fn.tempname()
	vim.fn.mkdir(dir, "p")
	local files = {
		["top.sv"] = { "module top(input logic clk);", "  sub u_sub(.clk(clk));", "endmodule" },
		["sub.sv"] = { "module sub(input logic clk);", "endmodule" },
		["leaf.sv"] = { "module leaf;", "endmodule" },
	}
	for name, lines in pairs(files) do
		vim.fn.writefile(lines, dir .. "/" .. name)
	end

	vim.opt.rtp:prepend(repo)
	require("lazyverilog").setup({ cmd = server, inlay_hints = false, folding = false })

	vim.cmd("edit " .. vim.fn.fnameescape(dir .. "/top.sv"))
	if not vim.wait(10000, function() return #clients() > 0 end, 50) then
		return fail("no lazyverilog client started")
	end
	vim.cmd("edit " .. vim.fn.fnameescape(dir .. "/sub.sv"))
	vim.cmd("split " .. vim.fn.fnameescape(dir .. "/leaf.sv"))
	-- Give every FileType/BufReadPost/BufWinEnter start a chance to run.
	vim.wait(1500, function() return false end, 50)

	local cl = clients()
	if #cl ~= 1 then
		return fail(("expected one lazyverilog client for three buffers, got %d"):format(#cl))
	end
	local attached = vim.tbl_count(cl[1].attached_buffers)
	if attached ~= 3 then
		return fail(("expected the client attached to three buffers, got %d"):format(attached))
	end

	print("nvim-client-smoke: ok")
	vim.cmd("qa!")
end

-- An error must exit nonzero rather than leave headless nvim waiting.
local ok, err = xpcall(main, debug.traceback)
if not ok then
	fail(tostring(err))
end
