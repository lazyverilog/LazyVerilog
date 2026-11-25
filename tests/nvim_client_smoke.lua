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

-- The rendered RtlTree buffer: its first line is the "RtlTree (Depth: …" title.
local function rtltree_buf()
	for _, b in ipairs(vim.api.nvim_list_bufs()) do
		if vim.api.nvim_buf_is_loaded(b) then
			local first = vim.api.nvim_buf_get_lines(b, 0, 1, false)[1] or ""
			if first:match("^RtlTree %(Depth") then return b end
		end
	end
end

-- Open `file` alone, run `cmd`, and wait for a fresh tree buffer.  `:only`
-- closes the previous tree window, which wipes its buffer (bufhidden=wipe).
local function open_tree(file, cmd)
	vim.cmd("only")
	vim.cmd("edit " .. vim.fn.fnameescape(file))
	vim.wait(200, function() return rtltree_buf() == nil end, 20)
	vim.cmd(cmd)
	local buf
	vim.wait(10000, function()
		buf = rtltree_buf()
		return buf ~= nil
	end, 50)
	return buf
end

-- Tree rows (after title, key guide, blank) with their highlight spans.
local function tree_rows(buf)
	local ns = vim.api.nvim_create_namespace("LazyVerilogRtlTreeGuide")
	local lines = vim.api.nvim_buf_get_lines(buf, 3, -1, false)
	local rows = {}
	for i, line in ipairs(lines) do
		rows[i] = { line = line, hl = {} }
	end
	for _, m in ipairs(vim.api.nvim_buf_get_extmarks(buf, ns, 0, -1, { details = true })) do
		local r = rows[m[2] - 2]
		if r and m[4].hl_group then
			r.hl[m[4].hl_group] = r.line:sub(m[3] + 1, m[4].end_col)
		end
	end
	return rows
end

-- Display column where the `[file]` part starts.
local function file_col(line)
	local s = line:find("[", 1, true)
	return s and vim.fn.strdisplaywidth(line:sub(1, s - 1))
end

local function check_rtltree(dir)
	local top = dir .. "/top.sv"

	local buf = open_tree(top, "RtlTree")
	if not buf then return "RtlTree: no tree buffer" end
	local rows = tree_rows(buf)
	if #rows < 2 then return "RtlTree: expected 2 rows, got " .. #rows end
	if rows[1].hl.LazyVerilogRtlTreeModule ~= "top" then
		return "RtlTree: root module not highlighted: " .. vim.inspect(rows[1])
	end
	local sub = rows[2]
	if sub.hl.LazyVerilogRtlTreeModule ~= "sub" or sub.hl.LazyVerilogRtlTreeInstance ~= "(u_sub)"
		or not (sub.hl.LazyVerilogRtlTreeFile or ""):match("sub%.sv%]$")
		or sub.hl.LazyVerilogRtlTreeGuide ~= "└─ " then
		return "RtlTree: child row highlights wrong: " .. vim.inspect(sub)
	end
	if file_col(rows[1].line) == file_col(rows[2].line) then
		return "RtlTree: unaligned layout unexpectedly aligned: " .. vim.inspect(rows)
	end

	buf = open_tree(top, "RtlTree align")
	if not buf then return "RtlTree align: no tree buffer" end
	rows = tree_rows(buf)
	if not file_col(rows[1].line) or file_col(rows[1].line) ~= file_col(rows[2].line) then
		return "RtlTree align: file column not aligned: " .. vim.inspect(rows)
	end
	if rows[2].hl.LazyVerilogRtlTreeInstance ~= "(u_sub)" then
		return "RtlTree align: instance highlight wrong: " .. vim.inspect(rows[2])
	end

	-- A bad argument reports usage and leaves the open tree alone.
	local notified
	local orig_notify = vim.notify
	vim.notify = function(msg) notified = msg end
	vim.cmd("RtlTree bogus")
	vim.notify = orig_notify
	if not (notified or ""):match("Usage: :RtlTree") then
		return "RtlTree bogus: expected usage message, got " .. tostring(notified)
	end
	if rtltree_buf() ~= buf then return "RtlTree bogus: tree buffer replaced" end

	buf = open_tree(dir .. "/sub.sv", "RtlTreeReverse")
	if not buf then return "RtlTreeReverse: no tree buffer" end
	rows = tree_rows(buf)
	if rows[1].hl.LazyVerilogRtlTreeModule ~= "sub" then
		return "RtlTreeReverse: root module wrong: " .. vim.inspect(rows)
	end
	local found_top = false
	for _, r in ipairs(rows) do
		if r.hl.LazyVerilogRtlTreeModule == "top" then found_top = true end
	end
	if not found_top then return "RtlTreeReverse: parent top missing: " .. vim.inspect(rows) end
	return nil
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

	local err = check_rtltree(dir)
	if err then return fail(err) end

	print("nvim-client-smoke: ok")
	vim.cmd("qa!")
end

-- An error must exit nonzero rather than leave headless nvim waiting.
local ok, err = xpcall(main, debug.traceback)
if not ok then
	fail(tostring(err))
end
