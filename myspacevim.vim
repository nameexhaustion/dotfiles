function! myspacevim#after() abort
    autocmd FileType javascript setlocal formatprg=prettier\ --single-quote
    " Use formatprg when available
    let g:neoformat_try_formatprg = 1
endfunction
