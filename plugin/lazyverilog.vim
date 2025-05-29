if exists("g:loaded_lazyverilog")
    finish
endif
let g:loaded_lazyverilog= 1

" Exposes the plugin's functions for use as commands in Vim.
command! -nargs=0 DisplayTime call lazyverilog#DisplayTime()
command! -nargs=0 DefineWord call lazyverilog#DefineWord()
command! -nargs=0 AspellCheck call lazyverilog#AspellCheck()
