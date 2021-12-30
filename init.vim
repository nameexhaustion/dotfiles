filetype plugin indent on
syntax on
set t_Co=256

set clipboard=unnamedplus
set completeopt=noinsert,menuone,noselect
set cursorline
set inccommand=split
set number
set splitbelow splitright
set title
set ttimeoutlen=0
set wildmenu
set signcolumn=yes
set updatetime=100
set splitright
set splitbelow

set tabstop=4
set shiftwidth=4
set expandtab
set background=light
set mouse=a

" File browser
let g:netrw_banner=0
let g:netrw_liststyle=0
let g:netrw_browse_split=4
let g:netrw_altv=1
let g:netrw_winsize=25
let g:netrw_keepdir=0
let g:netrw_localcopydircmd='cp -r'

autocmd VimEnter * NERDTree
autocmd BufWinEnter * NERDTreeMirror
autocmd VimEnter * wincmd l
autocmd BufWinEnter * wincmd l

call plug#begin()
Plug 'dracula/vim'
Plug 'sonph/onehalf', {'rtp': 'vim'}
Plug 'ryanoasis/vim-devicons'
Plug 'SirVer/ultisnips'
Plug 'honza/vim-snippets'
Plug 'scrooloose/nerdtree'
Plug 'preservim/nerdcommenter'
Plug 'neoclide/coc.nvim', {'branch': 'release'}
Plug 'sheerun/vim-polyglot'
Plug 'airblade/vim-gitgutter'
Plug 'itchyny/lightline.vim'
Plug 'junegunn/fzf', { 'do': { -> fzf#install() } }
Plug 'junegunn/fzf.vim'
call plug#end()

let g:lightline= {
    \ 'colorscheme': 'one',
    \ }

let g:NERDTreeShowHidden=1

colorscheme onehalflight

