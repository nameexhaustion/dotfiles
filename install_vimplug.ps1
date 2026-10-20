mkdir $env:localappdata/nvim/autoload

iwr -useb https://raw.githubusercontent.com/junegunn/vim-plug/master/plug.vim |`
    ni $env:localappdata/nvim/autoload/plug.vim -Force
