# What is this?

Wake is a build orchestration tool and language.
If you have a build whose steps cannot be adequately expressed in
make/tup/basel/etc, you probably need wake.
If you don't want to waste time rebuilding things that don't need it,
or that your colleagues already built, you might appreciate wake.

# Wake features:
  - dependent job execution

    Which jobs to run next can depend on the results of previous jobs.  For
    example, when you run configure in a traditional automake system, this
    typically affects what will be built by make.  Similarly cmake.  These
    two-stage build systems are necessary because make job selection cannot
    depend on the result of a prior build step.  In complicated builds,
    two-stages are sometimes not enough. In wake, all jobs may be dependent.

  - dependency analysis

    In classic build systems, you must specify all the inputs and outputs of
    a job you want to run.  If you under-specify the inputs, the build is
    not reproducible; it might fail to compile files that need recompilation
    and the job might fail non-deterministically on systems which run more
    jobs simultaneously.  If you over-specify the inputs, the build performs
    unnecessary recompilation when those inputs change.  In wake, if you
    under-specify the inputs, the build fails every time.  If you
    over-specify the inputs, wake automatically prunes the unused
    dependencies so the job will not be re-run unless it must.  You almost
    never need to tell wake what files a job builds; it knows.

  - build introspection

    When you have a built workspace, it is helpful to be able to trace the
    provenance of build artefacts.  Wake keeps a database to record what it
    did.  You can query that database at any time to find out exactly how a
    file in your workspace got there.

  - intrinsically-parallel language

    While your build orchestration files describe a sequence of compilation
    steps, the wake language automatically extracts parallelism.  Everything
    runs at once.  Only true data dependencies cause wake to sequence jobs. 
    Wake handles parallelism for you, so you don't need to think about it.

  - shared build caching

    You just checked-out the master branch and started a build.  However,
    your system runs the same software as your colleague who authored that
    commit.  If wake can prove it's safe, it will just copy the prebuilt
    files and save you time.  This can also translate into pull requests
    whose regression tests pass immediately, increasing productivity.

# Installing dependencies

On Mac OS with Mac Ports installed:

    sudo port install osxfuse sqlite3 gmp re2 ncurses pkgconfig
    brew link re2
    # maybe this next command gets rid of the gmp export below. If it does then remove the export below. Otherwise remove the brew link gmp
    brew link gmp

On Mac OS with Home Brew installed:

    brew update
    sudo chown $USER /usr/local/Caskroom/
    brew tap homebrew/cask
    brew cask install osxfuse
    sudo chown root:wheel /usr/local/Caskroom/
    brew install sqlite3 gmp re2 ncurses pkgconfig gmp

    Set up .pc files - [Installation instructions](https://mrcook.uk/how-to-install-go-ncurses-on-mac-osx)

    export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:~/Dropbox/ncurses-pc-files/
    export PKG_CONFIG_PATH=/usr/local/Cellar/ncurses/6.1/lib/pkgconfig:$PKG_CONFIG_PATH

    export CPATH=/usr/local/Cellar/gmp/6.1.2_2/include:$CPATH

    You must reboot for the installation of osxfuse to take effect.

    System Extension Blocked
    "The system extension required for mounting FUSE volumes could not be loaded.
    Please open the Security & Privacy System Preferences pane, go to the General preferences and allow loading system software from developer "Benjamin Fleischer".

    Then try mounting the volume again."


On Debian/Ubuntu:

    sudo apt-get install libfuse-dev libsqlite3-dev libgmp-dev libre2-dev libncurses5-dev pkg-config

# Building wake

    git clone https://github.com/sifive/wake.git
    cd wake
    git tag                 # See what versions exist
    #git checkout master    # Use development branch (e.g. recent bug fix)
    #git checkout v0.9      # Check out a specific version, like v0.9
    make
    ./bin/wake 'install "/usr/local"' # or wherever

External dependencies:
 - c++ 11		GPLv3		https://www.gnu.org/software/gcc/
 - sqlite3-dev		public domain	https://www.sqlite.org/
 - libgmp-dev		LGPL v3		https://gmplib.org
 - libfuse-dev		LGPL v2.1	https://github.com/libfuse/libfuse
 - libre2-dev		BSD 3-clause	https://github.com/google/re2
 - libncurses5-dev	MIT		https://www.gnu.org/software/ncurses/

Optional dependencies:
 - re2c			public domain	http://re2c.org
 - utf8proc		MIT 		https://juliastrings.github.io/utf8proc/

Internal dependencies:
 - gopt			TFL		http://www.purposeful.co.uk/software/gopt/
 - SipHash		public domain	https://github.com/veorq/SipHash
 - BLAKE2		public domain	https://github.com/BLAKE2/libb2
 - whereami		WTFPLV2		https://github.com/gpakosz/whereami
