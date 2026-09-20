-- Neovim profile used for every recording.  Paths come from the environment that
-- nvrender.py sets, so nothing here is machine specific:
--   LV_REPO   the repository root (its lua/ directory is the plugin under recording)
--   LV_THEME  the catppuccin/nvim checkout
--   LV_LSP    optional lazyverilog-lsp binary; unset lets the plugin resolve its own
vim.opt.rtp:prepend(os.getenv("LV_REPO"))
vim.opt.rtp:prepend(os.getenv("LV_THEME"))

vim.opt.termguicolors = true
require("catppuccin").setup({ flavour = "mocha" })
vim.cmd.colorscheme("catppuccin")

vim.opt.number = true
vim.opt.signcolumn = "yes"
vim.opt.laststatus = 2
vim.opt.ruler = false
vim.opt.cursorline = false
vim.opt.cmdheight = 1
vim.opt.updatetime = 200
vim.opt.shortmess:append("cI")
vim.opt.expandtab = true
vim.opt.shiftwidth = 4
vim.opt.tabstop = 4
vim.opt.autoindent = true
vim.o.statusline = " %t %m%=%l:%c "

vim.filetype.add({ extension = { sv = "systemverilog", svh = "systemverilog" } })
require("lazyverilog").setup({ cmd = os.getenv("LV_LSP") })

-- Diagnostics are off unless a scene turns them on, so only the lint recording shows them.
vim.diagnostic.enable(false)

-- The mappings a recording relies on.  `gra` (code action), `grn` (rename) and `<C-x><C-o>`
-- (completion) are Neovim's own defaults and are not redefined.
vim.keymap.set("n", "gd", vim.lsp.buf.definition)
vim.keymap.set("n", "gr", vim.lsp.buf.references)
vim.keymap.set("n", "K", vim.lsp.buf.hover)
vim.api.nvim_create_user_command("Symbols", function(o) vim.lsp.buf.workspace_symbol(o.args) end, { nargs = 1 })
-- Signature help, redrawn from scratch each time so a stale float never hides the new one.
vim.keymap.set("i", "<C-s>", function()
  for _, w in ipairs(vim.api.nvim_list_wins()) do
    if vim.api.nvim_win_get_config(w).relative ~= "" then pcall(vim.api.nvim_win_close, w, true) end
  end
  vim.lsp.buf.signature_help()
end)
