let SessionLoad = 1
if &cp | set nocp | endif
let s:cpo_save=&cpo
set cpo&vim
imap <Nul> <C-Space>
inoremap <expr> <Up> pumvisible() ? "\" : "\<Up>"
inoremap <expr> <S-Tab> pumvisible() ? "\" : "\<S-Tab>"
inoremap <expr> <Down> pumvisible() ? "\" : "\<Down>"
imap <M-l> <Right>
imap <M-h> <Left>
imap <M-j> <Down>
imap <M-k> <Up>
imap <silent> <M-o> o
imap <silent> <S-Down> <Down>
imap <silent> <S-Up> <Up>
imap <silent> <S-Right> <Right>
imap <silent> <S-Left> <Left>
imap <M-/> <Plug>NERDCommenterToggle
inoremap <C-U> u
map  :tabnext
vmap  '<o/*'>o*/
map  :tabp
nmap  :BottomExplorerWindow
nmap  :FirstExplorerWindow
nmap d :Cscope d =expand("<cword>") =expand("%")
nmap i :Cscope i ^=expand("<cfile>")$
nmap f :Cscope f =expand("<cfile>")
nmap e :Cscope e =expand("<cword>")
nmap t :Cscope t =expand("<cword>")
nmap c :Cscope c =expand("<cword>")
nmap g :Cscope g =expand("<cword>")
nmap s :Cscope s =expand("<cword>")
nnoremap   @=((foldclosed(line('.')) < 0) ? 'zc':'zo')
omap <silent> % <Plug>(MatchitOperationForward)
xmap <silent> % <Plug>(MatchitVisualForward)
nmap <silent> % <Plug>(MatchitNormalForward)
nmap ,t <Plug>view:switch_status_path_length
imap ¯ <Plug>NERDCommenterToggle
imap ì <Right>
imap è <Left>
imap ê <Down>
imap ë <Up>
imap <silent> ï o
nmap Q gq
xmap Q gq
omap Q gq
omap <silent> [% <Plug>(MatchitOperationMultiBackward)
xmap <silent> [% <Plug>(MatchitVisualMultiBackward)
nmap <silent> [% <Plug>(MatchitNormalMultiBackward)
nnoremap \d :YcmShowDetailedDiagnostic
nnoremap \yd :YcmCompleter GoToDefinitionElseDeclaration
map \in :call TitleDet()'s
vmap \cm :call CommentLines()
vnoremap <silent> \? y?=escape(@", '\\/.*$^~[]')
vnoremap <silent> \/ y/=escape(@", '\\/.*$^~[]')
map \tcw :call Open_new_tab_and_tags_locate_cursor_word()
nmap \sG :sign unplace 4
nmap \sR :sign unplace 3
nmap \sV :sign unplace 2
nmap \sC :sign unplace 1
nmap \sS :sign unplace *
nmap \jso :exe ":sign jump 14 file=" . expand("%:p")
nmap \so :exe ":sign place 14 line=" . line('.') . " name=sio file=" . expand("%:p")
nmap \jsv :exe ":sign jump 2 file=" . expand("%:p")
nmap \jsa :exe ":sign jump 1 file=" . expand("%:p")
nmap \sx :exe ":sign place 4 line=" . line('.') . " name=sx file=" . expand("%:p")
nmap \sr :exe ":sign place 3 line=" . line('.') . " name=sr file=" . expand("%:p")
nmap \sv :exe ":sign place 2 line=" . line('.') . " name=sv file=" . expand("%:p")
nmap \sa :exe ":sign place 1 line=" . line('.') . " name=sa file=" . expand("%:p")
nmap \pl :!perl /root/bin/Vim/tools/pltags.pl *.pl *.pm
nnoremap \pm :call LoadPerlModule()
nnoremap \ns :call Newsub()
map \pT :!prove -lv % \| less
map \pt :!prove -Iwork/ -v %
nmap \cs :cscope -b -R
map \j gJdw
map \mt :TMiniBufExplorer
nmap \fC :set foldcolumn=0
nmap \fc :set foldcolumn=1
nmap <silent> \nt :NERDTreeToggle
map \rN :set nornu
map \rn :set rnu
map \nn :set nonumber
map \nu :set number
noremap \mm mmHmt:%s///ge'tzt'm
nnoremap \G :Grep
nmap \gw gg=G:w
nmap \gg gg=G
map \me :message
map \aa :A
map \ev :e! $MYVIMRC
nmap \mc :make clean
nmap \mk :make
nmap \w :w!
map \bp :bp
map \bn :bn
map \vs :vsplit
map \hs :split
map \vn :vnew
nmap \bl <Right>:bd
nmap \bh <Left>:bd
nmap \bj <Down>:bd
nmap \bk <Up>:bd
map \bd :bd
map \to :tabonly
map \tp :tabp
map \tN :tabNext
map \tn :tabnext
map \tw :tabnew
map \tm :tabm
map \tl :tabl
map \tf :tabfirst
map \te :tabnew:e
map \tc :tabc
omap <silent> ]% <Plug>(MatchitOperationMultiForward)
xmap <silent> ]% <Plug>(MatchitVisualMultiForward)
nmap <silent> ]% <Plug>(MatchitNormalMultiForward)
xmap a% <Plug>(MatchitVisualTextObject)
omap <silent> g% <Plug>(MatchitOperationBackward)
xmap <silent> g% <Plug>(MatchitVisualBackward)
nmap <silent> g% <Plug>(MatchitNormalBackward)
xmap gx <Plug>(open-word-under-cursor)
nmap gx <Plug>(open-word-under-cursor)
nnoremap <expr> g| ''.float2nr(round((col('$')-1) * min([100, v:count])/ 100.0)) .'|'
nnoremap <silent> <Plug>(YCMFindSymbolInDocument) :call youcompleteme#finder#FindSymbol( 'document' )
nnoremap <silent> <Plug>(YCMFindSymbolInWorkspace) :call youcompleteme#finder#FindSymbol( 'workspace' )
nnoremap <silent> <Plug>(YCMCallHierarchy) <Cmd>call youcompleteme#hierarchy#StartRequest( 'call' )
nnoremap <silent> <Plug>(YCMTypeHierarchy) <Cmd>call youcompleteme#hierarchy#StartRequest( 'type' )
vmap <C-O> '<o/*'>o*/
xmap <silent> <Plug>(MatchitVisualTextObject) <Plug>(MatchitVisualMultiBackward)o<Plug>(MatchitVisualMultiForward)
onoremap <silent> <Plug>(MatchitOperationMultiForward) :call matchit#MultiMatch("W",  "o")
onoremap <silent> <Plug>(MatchitOperationMultiBackward) :call matchit#MultiMatch("bW", "o")
xnoremap <silent> <Plug>(MatchitVisualMultiForward) :call matchit#MultiMatch("W",  "n")m'gv``
xnoremap <silent> <Plug>(MatchitVisualMultiBackward) :call matchit#MultiMatch("bW", "n")m'gv``
nnoremap <silent> <Plug>(MatchitNormalMultiForward) :call matchit#MultiMatch("W",  "n")
nnoremap <silent> <Plug>(MatchitNormalMultiBackward) :call matchit#MultiMatch("bW", "n")
onoremap <silent> <Plug>(MatchitOperationBackward) :call matchit#Match_wrapper('',0,'o')
onoremap <silent> <Plug>(MatchitOperationForward) :call matchit#Match_wrapper('',1,'o')
xnoremap <silent> <Plug>(MatchitVisualBackward) :call matchit#Match_wrapper('',0,'v')m'gv``
xnoremap <silent> <Plug>(MatchitVisualForward) :call matchit#Match_wrapper('',1,'v'):if col("''") != col("$") | exe ":normal! m'" | endifgv``
nnoremap <silent> <Plug>(MatchitNormalBackward) :call matchit#Match_wrapper('',0,'n')
nnoremap <silent> <Plug>(MatchitNormalForward) :call matchit#Match_wrapper('',1,'n')
xnoremap <Plug>(open-word-under-cursor) <ScriptCmd>vim9.Open(getregion(getpos('v'), getpos('.'), { type: mode() })->join())
nnoremap <Plug>(open-word-under-cursor) <ScriptCmd>vim9.Open(GetWordUnderCursor())
nmap <C-@><C-@>d :vert split:Cscope d =expand("<cword>") =expand("%")
nmap <Nul><Nul>d :vert split:Cscope d =expand("<cword>") =expand("%")
nmap <C-@><C-@>i :vert split:Cscope i ^=expand("<cfile>")$
nmap <Nul><Nul>i :vert split:Cscope i ^=expand("<cfile>")$
nmap <C-@><C-@>f :vert split:Cscope f =expand("<cfile>")
nmap <Nul><Nul>f :vert split:Cscope f =expand("<cfile>")
nmap <C-@><C-@>e :vert split:Cscope e =expand("<cword>")
nmap <Nul><Nul>e :vert split:Cscope e =expand("<cword>")
nmap <C-@><C-@>t :vert split:Cscope t =expand("<cword>")
nmap <Nul><Nul>t :vert split:Cscope t =expand("<cword>")
nmap <C-@><C-@>c :vert split:Cscope c =expand("<cword>")
nmap <Nul><Nul>c :vert split:Cscope c =expand("<cword>")
nmap <C-@><C-@>g :vert split:Cscope g =expand("<cword>")
nmap <Nul><Nul>g :vert split:Cscope g =expand("<cword>")
nmap <C-@><C-@>s :vert split:Cscope s =expand("<cword>")
nmap <Nul><Nul>s :vert split:Cscope s =expand("<cword>")
nmap <C-@>d :split:Cscope d =expand("<cword>") =expand("%")
nmap <Nul>d :split:Cscope d =expand("<cword>") =expand("%")
nmap <C-@>i :split:Cscope i ^=expand("<cfile>")$
nmap <Nul>i :split:Cscope i ^=expand("<cfile>")$
nmap <C-@>f :split:Cscope f =expand("<cfile>")
nmap <Nul>f :split:Cscope f =expand("<cfile>")
nmap <C-@>e :split:Cscope e =expand("<cword>")
nmap <Nul>e :split:Cscope e =expand("<cword>")
nmap <C-@>t :split:Cscope t =expand("<cword>")
nmap <Nul>t :split:Cscope t =expand("<cword>")
nmap <C-@>c :split:Cscope c =expand("<cword>")
nmap <Nul>c :split:Cscope c =expand("<cword>")
nmap <C-@>g :split:Cscope g =expand("<cword>")
nmap <Nul>g :split:Cscope g =expand("<cword>")
nmap <C-@>s :split:Cscope s =expand("<cword>")
nmap <Nul>s :split:Cscope s =expand("<cword>")
nmap <C-Bslash>d :Cscope d =expand("<cword>") =expand("%")
nmap <C-Bslash>i :Cscope i ^=expand("<cfile>")$
nmap <C-Bslash>f :Cscope f =expand("<cfile>")
nmap <C-Bslash>e :Cscope e =expand("<cword>")
nmap <C-Bslash>t :Cscope t =expand("<cword>")
nmap <C-Bslash>c :Cscope c =expand("<cword>")
nmap <C-Bslash>g :Cscope g =expand("<cword>")
nmap <C-Bslash>s :Cscope s =expand("<cword>")
nmap <Plug>view:switch_status_path_length :let g:statusline_max_path = 200 - g:statusline_max_path
map <F6> :call CheckSyntax()
map <F5> :TagbarToggle
map <C-F6> :call CompileRunGpp()
map <C-F5> :call CompileRunGcc()
map <F3> :set spell!|:echo "Spell check: " . strpart("OffOn", 3 * &spell, 3)
nmap <silent> <F10> :Sexplore!
nmap <silent> <M-o> o
nmap <silent> <M-j> <Down>
vmap <silent> <M-j> <Down>
nmap <silent> <M-k> <Up>
vmap <silent> <M-k> <Up>
nmap <silent> <M-l> <Right>
vmap <silent> <M-l> <Right>
nmap <silent> <M-h> <Left>
vmap <silent> <M-h> <Left>
nmap <silent> <S-Down> <Down>
vmap <silent> <S-Down> <Down>
nmap <silent> <S-Up> <Up>
vmap <silent> <S-Up> <Up>
nmap <silent> <S-Right> <Right>
vmap <silent> <S-Right> <Right>
nmap <silent> <S-Left> <Left>
vmap <silent> <S-Left> <Left>
nmap <F8> :SrcExplToggle
map <M-/> <Plug>NERDCommenterToggle
map <C-F12> :!ctags -R --c++-kinds=+p --fields=+iaS --extras=+q .
map <F12> :!ctags -R --c-kinds=+p --fields=+iaS --extras=+q .
nmap <C-W><C-B> :BottomExplorerWindow
nmap <C-W><C-F> :FirstExplorerWindow
map <F7> :TlistToggle
nmap <M-O> o
nmap <C-Down> : move .+1
map <M-L> :tabl
map <M-e> :tabl
map <M-f> :tabfirst
map <M-n> :tabnext
map <C-N> :tabnext
map <C-P> :tabp
map <M-p> :tabp
inoremap <expr> 	 pumvisible() ? "\" : "\	"
inoremap  u
map ¯ <Plug>NERDCommenterToggle
nmap <silent> ï o
nmap <silent> ê <Down>
vmap <silent> ê <Down>
nmap <silent> ë <Up>
vmap <silent> ë <Up>
nmap <silent> ì <Right>
vmap <silent> ì <Right>
nmap <silent> è <Left>
vmap <silent> è <Left>
nmap Ï o
map Ì :tabl
map å :tabl
map æ :tabfirst
map î :tabnext
map ð :tabp
inoremap \pu<M-j> #!/usr/bin/env perl# TimeStamp: <now>use 5.010;use strict;use warnings;
inoremap \puê #!/usr/bin/env perl# TimeStamp: <now>use 5.010;use strict;use warnings;
inoremap jj 
cabbr csm colorscheme ManShow
cabbr csc colorscheme Celibate
let &cpo=s:cpo_save
unlet s:cpo_save
set autochdir
set autoindent
set background=dark
set cindent
set cinoptions=>4,e0,n0,f0,{0,}0,^0,:s,=s,l0,gs,hs,ps,ts,+s,c3,C0,(2s,us,U0,w0,m0,j0,)20,*30
set completeopt=menuone
set confirm
set cpoptions=aAceFszB
set cscopequickfix=s-,g-,d-,c-,t-,e-,f-,i-
set cscopetag
set cscopeverbose
set display=truncate
set fileencodings=utf-8,chinese,ucs-bom,gb18030,cp936,big5,euc-jp,euc-kr,latin-1
set fillchars=vert:\ ,stl:\ ,stlnc:\\
set foldopen=block,hor,mark,percent,quickfix,tag
set formatprg=astyle\ -T8\ -p\ -b\ -A2\ --brackets=break\ --indent=spaces\ --indent-cases\ --indent-preprocessor\ --pad-header\ --pad-oper\ --unpad-paren\ --delete-empty-lines\ --suffix=none
set grepprg=grep\ -nH
set helplang=cn
set hlsearch
set ignorecase
set incsearch
set keymodel=startsel,stopsel
set langnoremap
set nolangremap
set laststatus=2
set lazyredraw
set listchars=tab:|\ ,trail:.,extends:>,precedes:<,eol:$
set matchpairs=(:),{:},[:],<:>
set nomodeline
set mouse=a
set mousemodel=popup
set nrformats=bin,hex
set path=.,/usr/include,,,/usr/src/linux-source-2.6.32/include/linux/*,/usr/src/linux-source-2.6.32/include/scsi/*,/usr/src/linux-source-2.6.32/drivers/scsi/*
set printencoding=utf-8
set printmbcharset=ISO10646
set printmbfont=r:MicrosoftYaHei,b:SimSun,i:KaiTi,o:FangSong,c:yes
set printoptions=formfeed:y,header:5,paper:A4,duplex:on,syntax:y
set report=0
set rulerformat=%20(%2*%<%f%=\ %m%r\ %3l\ %c\ %p%%%)
set runtimepath=~/.vim,~/.vim/bundle/Vundle.vim,~/.vim/bundle/YouCompleteMe,~/.vim/bundle/tagbar,~/.vim/bundle/ctags.vim,~/.vim/bundle/cscope.vim,~/.vim/bundle/taglist.vim,/opt/vim/share/vim/vimfiles,/opt/vim/share/vim/vim91,/opt/vim/share/vim/vim91/pack/dist/opt/netrw,/opt/vim/share/vim/vim91/pack/dist/opt/matchit,/opt/vim/share/vim/vimfiles/after,~/.vim/after,~/.vim/bundle/Vundle.vim,~/.vim/bundle/Vundle.vim/after,~/.vim/bundle/YouCompleteMe/after,~/.vim/bundle/tagbar/after,~/.vim/bundle/ctags.vim/after,~/.vim/bundle/cscope.vim/after,~/.vim/bundle/taglist.vim/after
set scrolloff=3
set selection=exclusive
set selectmode=mouse,key
set sessionoptions=blank,buffers,curdir,folds,options,tabpages,winsize,terminal,unix,slash
set shiftwidth=4
set shortmess=filnxtToOSc
set showmatch
set smartcase
set smartindent
set smarttab
set softtabstop=4
set tabline=%!ShortTabLine()
set tabstop=4
set tags=./tags,./TAGS,tags,TAGS,/usr/src/linux-source-2.6.32/include/linux/tags,/usr/src/linux-source-2.6.32/include/scsi/tags,/usr/src/linux-source-2.6.32/drivers/scsi/tags
set title
set titlelen=100
set titlestring=%<%(%{Tlist_Get_Tag_Prototype_By_Line()}\ \ \ %)%([%M]%)%f%{FileInfo()}\ %{getcwd()}\ %=%l/%L-%P
set ttimeout
set ttimeoutlen=100
set undofile
set updatetime=500
set wildmode=list:longest,full
let s:so_save = &g:so | let s:siso_save = &g:siso | setg so=0 siso=0 | setl so=-1 siso=-1
let v:this_session=expand("<sfile>:p")
silent only
silent tabonly
cd ~/src/bless/src
if expand('%') == '' && !&modified && line('$') <= 1 && getline(1) == ''
  let s:wipebuf = bufnr('%')
endif
set shortmess+=aoO
badd +0 ~/src/bless/src/main.c
argglobal
%argdel
$argadd main.c
edit ~/src/bless/src/main.c
argglobal
let s:cpo_save=&cpo
set cpo&vim
inoremap <buffer> <silent> <M--> =EchoFuncP()
inoremap <buffer> <silent> <M-=> =EchoFuncN()
inoremap <buffer> <silent> ­ =EchoFuncP()
inoremap <buffer> <silent> ½ =EchoFuncN()
inoremap <buffer> <silent> ( (=EchoFunc()
inoremap <buffer> <silent> ) =EchoFuncClear())
let &cpo=s:cpo_save
unlet s:cpo_save
setlocal keymap=
setlocal noarabic
setlocal autoindent
setlocal backupcopy=
setlocal balloonexpr=
setlocal nobinary
setlocal nobreakindent
setlocal breakindentopt=
setlocal bufhidden=hide
setlocal buflisted
setlocal buftype=
setlocal cindent
setlocal cinkeys=0{,0},0),0],:,0#,!^F,o,O,e
setlocal cinoptions=>4,e0,n0,f0,{0,}0,^0,:s,=s,l0,gs,hs,ps,ts,+s,c3,C0,(2s,us,U0,w0,m0,j0,)20,*30
setlocal cinscopedecls=public,protected,private
setlocal cinwords=if,else,while,do,for,switch
setlocal colorcolumn=
setlocal comments=sO:*\ -,mO:*\ \ ,exO:*/,s1:/*,mb:*,ex:*/,:///,://
setlocal commentstring=/*\ %s\ */
setlocal complete=.,w,b,u,t,i
setlocal completefunc=
setlocal completeopt=
setlocal concealcursor=
setlocal conceallevel=0
setlocal nocopyindent
setlocal cryptmethod=
setlocal nocursorbind
setlocal nocursorcolumn
set cursorline
setlocal cursorline
setlocal cursorlineopt=both
setlocal define=^\\s*#\\s*define
setlocal dictionary=~/.vim/c-support/wordlists/c-c++-keywords.list,~/.vim/c-support/wordlists/k+r.list,~/.vim/c-support/wordlists/stl_index.list
setlocal nodiff
setlocal diffanchors=
setlocal equalprg=
setlocal errorformat=
setlocal eventignorewin=
setlocal noexpandtab
if &filetype != 'c'
setlocal filetype=c
endif
setlocal fillchars=
setlocal findfunc=
setlocal fixendofline
setlocal foldcolumn=0
setlocal foldenable
setlocal foldexpr=0
setlocal foldignore=#
set foldlevel=100
setlocal foldlevel=100
setlocal foldmarker={{{,}}}
set foldmethod=syntax
setlocal foldmethod=syntax
setlocal foldminlines=1
setlocal foldnestmax=20
setlocal foldtext=foldtext()
setlocal formatexpr=
setlocal formatlistpat=^\\s*\\d\\+[\\]:.)}\\t\ ]\\s*
setlocal formatoptions=croql
setlocal formatprg=
setlocal grepformat=
setlocal grepprg=
setlocal iminsert=0
setlocal imsearch=-1
setlocal include=^\\s*#\\s*include
setlocal includeexpr=
setlocal indentexpr=
setlocal indentkeys=0{,0},0),0],:,0#,!^F,o,O,e
setlocal noinfercase
setlocal isexpand=
setlocal iskeyword=@,48-57,_,192-255
setlocal keywordprg=
setlocal lhistory=10
set linebreak
setlocal linebreak
setlocal nolisp
setlocal lispoptions=
setlocal lispwords=
setlocal nolist
setlocal listchars=
setlocal makeencoding=
setlocal makeprg=
setlocal matchpairs=(:),{:},[:],<:>
setlocal nomodeline
setlocal modifiable
setlocal nrformats=bin,hex
set number
setlocal number
setlocal numberwidth=4
setlocal omnifunc=ccomplete#Complete
setlocal path=
setlocal nopreserveindent
setlocal nopreviewwindow
setlocal quoteescape=\\
setlocal noreadonly
setlocal norelativenumber
setlocal norightleft
setlocal rightleftcmd=search
setlocal noscrollbind
setlocal scrolloff=-1
setlocal shiftwidth=4
setlocal noshortname
setlocal showbreak=
setlocal sidescrolloff=-1
setlocal signcolumn=auto
setlocal smartindent
setlocal nosmoothscroll
setlocal softtabstop=4
setlocal nospell
setlocal spellcapcheck=[.?!]\\_[\\])'\"\	\ ]\\+
setlocal spellfile=
setlocal spelllang=en_gb
setlocal spelloptions=
setlocal statusline=%h%#StatuslineFlag#%m%r%w\ %#StatuslineFileName#%t\ %#StatuslineFileEnc#%{&fileencoding}\ %#StatuslineFileFormat#%{&fileformat}\ %#StatuslineFileType#%{strlen(&ft)?'.'.&ft:'**'}\ %#GetTagName#%{GetTagName(line('.'))}%0*\ %=%#StatuslinePosition#%c-%v\ %l\ %#StatuslinePercent#%L\ %P\ %#StatuslineHostName#%{hostname()}local
setlocal suffixesadd=
setlocal noswapfile
setlocal synmaxcol=3000
if &syntax != 'c'
setlocal syntax=c
endif
setlocal tabstop=4
setlocal tagcase=
setlocal tagfunc=
setlocal tags=
setlocal termwinkey=
setlocal termwinscroll=10000
setlocal termwinsize=
setlocal textwidth=78
setlocal thesaurus=
setlocal thesaurusfunc=
setlocal undofile
setlocal undolevels=-123456
setlocal varsofttabstop=
setlocal vartabstop=
setlocal virtualedit=
setlocal wincolor=
setlocal nowinfixbuf
setlocal nowinfixheight
setlocal nowinfixwidth
setlocal wrap
setlocal wrapmargin=0
let s:l = 908 - ((17 * winheight(0) + 17) / 35)
if s:l < 1 | let s:l = 1 | endif
keepjumps exe s:l
normal! zt
keepjumps 908
normal! 05|
tabnext 1
if exists('s:wipebuf') && len(win_findbuf(s:wipebuf)) == 0
  silent exe 'bwipe ' . s:wipebuf
endif
unlet! s:wipebuf
set winheight=1 winwidth=20
set shortmess=filnxtToOSc
let s:sx = expand("<sfile>:p:r")."x.vim"
if filereadable(s:sx)
  exe "source " . fnameescape(s:sx)
endif
let &g:so = s:so_save | let &g:siso = s:siso_save
nohlsearch
doautoall SessionLoadPost
unlet SessionLoad
" vim: set ft=vim :
