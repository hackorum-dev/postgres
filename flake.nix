# PostgreSQL Nix flake.
#
# What this is: Nix (https://nixos.org) is a package manager, available on
# Linux, macOS, and the *BSDs, that can assemble an exact set of build
# dependencies without installing them system-wide. A "flake" is the small
# manifest below describing those dependencies. You do NOT need Nix to build
# PostgreSQL; the usual `./configure`/`meson setup` instructions are
# unchanged. This file only helps developers who already use Nix.
#
#   nix develop     -- drop into a shell with the full build toolchain
#                      (meson/ninja, bison, flex, perl, and the optional
#                      libraries) on PATH; then `meson setup build && ninja
#                      -C build` or `./configure && make` as usual.
#   nix build       -- build the server directly.
#
# Local ignores: Nix writes a `flake.lock` and `result` symlinks into the
# working tree. These are intentionally NOT added to .gitignore (they are
# not relevant to non-Nix developers). If you use Nix, add them to your
# personal, non-committed ignore file instead:
#
#   printf '/flake.lock\n/result\n/result-*\n' >> "$(git rev-parse --git-dir)/info/exclude"
#
# Keeping this current: this flake does not track upstream automatically.
# When a new optional dependency is added to configure/meson, this file will
# simply not enable it (mesonAutoFeatures is "disabled"), so the flake keeps
# building, it just won't expose the new feature until a toggle is added
# here. Adding one is a two-line change (a boolean plus its buildInput/flag);
# patches to keep this file in sync are welcome on pgsql-hackers. Nothing in
# the normal build depends on this file, so it going briefly stale breaks
# nothing.
{
  description = "PostgreSQL, built from this source tree";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # A single builder covering every optional feature ./configure and
      # meson expose. Each feature is a boolean toggle wired to its
      # buildInput + flag; flip any of them via .override { ... }.
      # Platform-specific features default to their availability.
      postgresqlFor =
        pkgs:
        let
          inherit (pkgs) lib stdenv;
          onLinux = stdenv.hostPlatform.isLinux;
          onDarwin = stdenv.hostPlatform.isDarwin;
        in
        lib.makeOverridable (
          {
            # Build system: "meson" (ninja) or "autoconf" (configure/make).
            # Meson is the default; it is the project's eventual sole build system.
            buildSystem ? "meson",

            # Optional library features (buildInput + flag).
            icuSupport ? true,
            readlineSupport ? true,
            zlibSupport ? true,
            opensslSupport ? true,
            libxmlSupport ? true,
            libxsltSupport ? true,
            lz4Support ? true,
            zstdSupport ? true,
            uuidSupport ? true,
            curlSupport ? true,

            # Procedural languages.
            perlSupport ? true,
            pythonSupport ? true,
            tclSupport ? true,

            # Platform-gated features.
            systemdSupport ? onLinux,
            selinuxSupport ? false,
            liburingSupport ? onLinux,
            numaSupport ? onLinux,
            gssSupport ? true,
            ldapSupport ? false,
            pamSupport ? onLinux,
            bonjourSupport ? onDarwin,
            llvmSupport ? false,

            # Developer / build-shape toggles (no extra deps).
            cassert ? false,
            debugSymbols ? false,
            tapTests ? true,
            injectionPoints ? false,

            # Documentation build (SGML/DocBook toolchain). Enabled by
            # default; the docs targets are not part of the default build
            # (only of `world`/`install-world`), so this adds the toolchain
            # to the environment without slowing down a plain `nix build`.
            docs ? true,
          }:
          let
            useMeson = buildSystem == "meson";

            # feature -> { flag = configure arg; dep = buildInput or null; }
            featureFlag = cond: flag: lib.optional cond flag;
            featureDep = cond: dep: lib.optional cond dep;

            configureFeatures = lib.flatten [
              (featureFlag icuSupport "--with-icu")
              (featureFlag (!icuSupport) "--without-icu")
              (featureFlag (!readlineSupport) "--without-readline")
              (featureFlag (!zlibSupport) "--without-zlib")
              (featureFlag opensslSupport "--with-ssl=openssl")
              (featureFlag libxmlSupport "--with-libxml")
              (featureFlag libxsltSupport "--with-libxslt")
              (featureFlag lz4Support "--with-lz4")
              (featureFlag zstdSupport "--with-zstd")
              (featureFlag uuidSupport "--with-uuid=e2fs")
              (featureFlag curlSupport "--with-libcurl")
              (featureFlag perlSupport "--with-perl")
              (featureFlag pythonSupport "--with-python")
              (featureFlag tclSupport "--with-tcl")
              (featureFlag systemdSupport "--with-systemd")
              (featureFlag selinuxSupport "--with-selinux")
              (featureFlag liburingSupport "--with-liburing")
              (featureFlag numaSupport "--with-libnuma")
              (featureFlag gssSupport "--with-gssapi")
              (featureFlag ldapSupport "--with-ldap")
              (featureFlag pamSupport "--with-pam")
              (featureFlag bonjourSupport "--with-bonjour")
              (featureFlag llvmSupport "--with-llvm")
              (featureFlag cassert "--enable-cassert")
              (featureFlag debugSymbols "--enable-debug")
              (featureFlag tapTests "--enable-tap-tests")
              (featureFlag injectionPoints "--enable-injection-points")
            ];

            # meson uses -Dfeature=enabled/disabled instead of --with-*.
            mesonFeatures = lib.flatten [
              [ "-Dicu=${if icuSupport then "enabled" else "disabled"}" ]
              [ "-Dreadline=${if readlineSupport then "enabled" else "disabled"}" ]
              [ "-Dzlib=${if zlibSupport then "enabled" else "disabled"}" ]
              [ "-Dssl=${if opensslSupport then "openssl" else "none"}" ]
              [ "-Dlibxml=${if libxmlSupport then "enabled" else "disabled"}" ]
              [ "-Dlibxslt=${if libxsltSupport then "enabled" else "disabled"}" ]
              [ "-Dlz4=${if lz4Support then "enabled" else "disabled"}" ]
              [ "-Dzstd=${if zstdSupport then "enabled" else "disabled"}" ]
              [ "-Duuid=${if uuidSupport then "e2fs" else "none"}" ]
              [ "-Dlibcurl=${if curlSupport then "enabled" else "disabled"}" ]
              [ "-Dplperl=${if perlSupport then "enabled" else "disabled"}" ]
              [ "-Dplpython=${if pythonSupport then "enabled" else "disabled"}" ]
              [ "-Dpltcl=${if tclSupport then "enabled" else "disabled"}" ]
              [ "-Dsystemd=${if systemdSupport then "enabled" else "disabled"}" ]
              [ "-Dselinux=${if selinuxSupport then "enabled" else "disabled"}" ]
              [ "-Dliburing=${if liburingSupport then "enabled" else "disabled"}" ]
              [ "-Dlibnuma=${if numaSupport then "enabled" else "disabled"}" ]
              [ "-Dgssapi=${if gssSupport then "enabled" else "disabled"}" ]
              [ "-Dldap=${if ldapSupport then "enabled" else "disabled"}" ]
              [ "-Dpam=${if pamSupport then "enabled" else "disabled"}" ]
              [ "-Dbonjour=${if bonjourSupport then "enabled" else "disabled"}" ]
              [ "-Dllvm=${if llvmSupport then "enabled" else "disabled"}" ]
              [ "-Dcassert=${lib.boolToString cassert}" ]
              [ "-Dtap_tests=${if tapTests then "enabled" else "disabled"}" ]
              [ "-Dinjection_points=${lib.boolToString injectionPoints}" ]
            ];

            buildInputs = lib.flatten (with pkgs; [
              (featureDep icuSupport icu)
              (featureDep readlineSupport readline)
              (featureDep zlibSupport zlib)
              (featureDep opensslSupport openssl)
              (featureDep libxmlSupport libxml2)
              (featureDep libxsltSupport libxslt)
              (featureDep lz4Support lz4)
              (featureDep zstdSupport zstd)
              (featureDep uuidSupport libuuid)
              (featureDep curlSupport curl)
              (featureDep perlSupport perl)
              (featureDep pythonSupport python3)
              (featureDep tclSupport tcl)
              (featureDep systemdSupport systemdLibs)
              (featureDep selinuxSupport libselinux)
              (featureDep liburingSupport liburing)
              (featureDep numaSupport numactl)
              (featureDep gssSupport libkrb5)
              (featureDep ldapSupport openldap)
              (featureDep pamSupport linux-pam)
              (featureDep llvmSupport llvmPackages.llvm)
            ]);
          in
          stdenv.mkDerivation {
            pname = "postgresql";
            version = "20devel";
            src = self;

            strictDeps = true;

            nativeBuildInputs = lib.flatten (with pkgs; [
              bison
              flex
              perl
              pkg-config
              (featureDep useMeson meson)
              (featureDep useMeson ninja)
              (featureDep llvmSupport llvmPackages.llvm.dev)
              (featureDep docs docbook-xsl-nons)
              (featureDep docs docbook_xml_dtd_45)
              (featureDep docs libxslt)
            ]);

            inherit buildInputs;

            # ---- autoconf path ----
            configureFlags = lib.optionals (!useMeson) configureFeatures;

            # ---- meson path ----
            mesonBuildType = "release";
            mesonFlags = lib.optionals useMeson mesonFeatures;
            dontUseMesonConfigure = !useMeson;
            mesonAutoFeatures = "disabled";

            enableParallelBuilding = true;
            doCheck = tapTests;

            meta = with lib; {
              description = "Object-relational database management system, built from source";
              homepage = "https://www.postgresql.org/";
              license = licenses.postgresql;
              platforms = platforms.unix;
              mainProgram = "psql";
            };
          }
        )
        { };
    in
    {
      packages = forAllSystems (pkgs: rec {
        postgresql = postgresqlFor pkgs;
        # Common presets built on top of the fully-parameterized default.
        postgresql-autoconf = postgresql.override { buildSystem = "autoconf"; };
        postgresql-debug = postgresql.override {
          cassert = true;
          debugSymbols = true;
          injectionPoints = true;
        };
        postgresql-minimal = postgresql.override {
          icuSupport = false;
          readlineSupport = false;
          zlibSupport = false;
          opensslSupport = false;
          libxmlSupport = false;
          libxsltSupport = false;
          lz4Support = false;
          zstdSupport = false;
          uuidSupport = false;
          curlSupport = false;
          perlSupport = false;
          pythonSupport = false;
          tclSupport = false;
          systemdSupport = false;
          liburingSupport = false;
          numaSupport = false;
          gssSupport = false;
          pamSupport = false;
        };
        default = postgresql;
      });

      # `nix develop`: the postgresql package already brings in the full
      # build toolchain (meson, ninja, bison, flex, perl, python3, tcl, the
      # optional libraries, and, since docs are on by default, the DocBook
      # toolchain) via inputsFrom. We add only ccache on top.
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.postgresql ];
          packages = [ pkgs.ccache ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
