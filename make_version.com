$ set noon
$ pipe search 'p1' "#define SQLITE_VERSION" | -
	( read sys$pipe v ; -
	  v = f$element(2, " ", f$edit(v, "COMPRESS") - """" - """") ; -
	  define/job/nolog SQLITE_VERSION &v )
$ version = f$trnlnm("SQLITE_VERSION")
$ major = f$element(1, ".", version)
$ minor = f$element(2, ".", version)
$ edit  = f$element(3, ".", version)
$ if (edit .eqs. ".") then edit = ""
$ if (edit .nes. "") then edit = "-" + edit
$ close/nolog opt
$ open/write opt 'p2'
$ write opt "IDENT=""V''major'.''minor'''edit'""""
$ close/nolog opt
$ exit 1
