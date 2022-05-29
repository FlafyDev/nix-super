# Release X.Y (202?-??-??)

* Nix now provides better integration with zsh's run-help feature. It is now
  included in the Nix installation in the form of an autoloadable shell
  function, run-help-nix. It picks up Nix subcommands from the currently typed
  in command and directs the user to the associated man pages.

* `nix repl` has a new build-'n-link (`:bl`) command that builds a derivation
  while creating GC root symlinks.

* The path produced by `builtins.toFile` is now allowed to be imported or read
  even with restricted evaluation. Note that this will not work with a
  read-only store.

* `nix build` has a new `--print-out-paths` flag to print the resulting output paths.
  This matches the default behaviour of `nix-build`.

* You can now specify which outputs of a derivation `nix` should
  operate on using the syntax `installable^outputs`,
  e.g. `nixpkgs#glibc^dev,static` or `nixpkgs#glibc^*`. By default,
  `nix` will use the outputs specified by the derivation's
  `meta.outputsToInstall` attribute if it exists, or all outputs
  otherwise.

  Selecting derivation outputs using the attribute selection syntax
  (e.g. `nixpkgs#glibc.dev`) no longer works.

* Running nix with the new `--debugger` flag will cause it to start a repl session if
  there is an exception thrown during eval, or if `builtins.break` is called.  From
  there one can inspect symbol values and evaluate nix expressions.  In debug mode
  the following new repl commands are available:
  ```
  :env          Show env stack
  :bt           Show trace stack
  :st           Show current trace
  :st <idx>     Change to another trace in the stack
  :c            Go until end of program, exception, or builtins.break().
  :s            Go one step
  ```

* `builtins.fetchTree` (and flake inputs) can now be used to fetch plain files
  over the `http(s)` and `file` protocols in addition to directory tarballs.
