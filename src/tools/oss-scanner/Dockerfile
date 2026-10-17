# OSS Scanner (red.anthropic.com/oss-scanner) build of PostgreSQL.  This
# should remain aligned with CI, building PostgreSQL with as many optional
# features as practical.
#
# github.com/anthropics/oss-scanner/blob/main/projects/postgresql/project.yaml
# directs the scanner to use this file.  Authority on writing this file:
# github.com/anthropics/oss-scanner/blob/main/templates/Dockerfile
#
# Test changes to this file as follows, editing the MY_* vars.
# oss-scanner/tools/check insists on an https:// URL, hence the use of the git
# insteadOf mechanism to trick it into testing a local repo.
#
# MY_REPO=$HOME/src/pg/postgresql
# MY_BRANCH=dockerfile-oss-scanner
# git clone https://github.com/anthropics/oss-scanner/
# sed -i "s|^repo:.*|repo: https://local.invalid/postgresql#$MY_BRANCH|" \
#   oss-scanner/projects/postgresql/project.yaml
# GIT_CONFIG_COUNT=2 \
#   GIT_CONFIG_KEY_0="url.file://$MY_REPO.insteadOf" \
#   GIT_CONFIG_VALUE_0="https://local.invalid/postgresql" \
#   GIT_CONFIG_KEY_1="protocol.file.allow" \
#   GIT_CONFIG_VALUE_1="always" \
#   oss-scanner/tools/check postgresql

FROM debian:trixie
ENV DEBIAN_FRONTEND=noninteractive

# Install CI "base" and "normal" OS package sets.  Source:
# github.com/anarazel/pg-vm-images
RUN apt-get update && apt-get install -y --no-install-recommends \
      bison \
      build-essential \
      ca-certificates \
      clang \
      flex \
      g++ \
      gcc \
      gdb \
      gettext \
      git \
      krb5-admin-server \
      krb5-kdc \
      krb5-user \
      ldap-utils \
      libcurl4-openssl-dev \
      libicu-dev \
      libio-pty-perl \
      libipc-run-perl \
      libkrb5-dev \
      libldap2-dev \
      liblz4-dev \
      libnuma-dev \
      libpam-dev \
      libperl-dev \
      libpython3-dev \
      libreadline-dev \
      libselinux-dev \
      libssl-dev \
      libsystemd-dev \
      libxml2-dev \
      libxslt1-dev \
      libzstd-dev \
      llvm-dev \
      locales-all \
      lz4 \
      make \
      meson \
      ninja-build \
      openssl \
      perl \
      pkg-config \
      procps \
      python3 \
      slapd \
      tcl-dev \
      time \
      uuid-dev \
      zlib1g-dev \
      zstd \
    && rm -rf /var/lib/apt/lists/*

# PostgreSQL won't start as root, so make an unprivileged user for build and
# smoke test.  The scan runs git as root, so configure it to accept the
# non-root-owned /src as safe.
RUN useradd --create-home --shell /bin/bash postgres \
    && git config --system --add safe.directory /src

# scanner contract: the checkout lives inside the image, at /src
COPY --chown=postgres:postgres . /src
WORKDIR /src
USER postgres

# Include various settings from the CI configuration, even ones for tools and
# tests this file isn't running.  If the scanner opts to runs affected tools
# and tests, it might benefit.
ENV ASAN_OPTIONS=print_stacktrace=1:disable_coredump=0:abort_on_error=1:detect_leaks=0 \
    UBSAN_OPTIONS=print_stacktrace=1:disable_coredump=0:abort_on_error=1:verbosity=2 \
    PG_TEST_EXTRA="kerberos ldap ssl libpq_encryption load_balance oauth" \
    PGCTLTIMEOUT=120 \
    TEMP_CONFIG=/src/src/tools/ci/pg_ci_base.conf \
    MESON_COMMON_PG_CONFIG_ARGS='-Dcassert=true -Dinjection_points=true' \
    MTEST_ARGS='--print-errorlogs --no-rebuild -C build'

# Per scanner contract, use -g -fno-omit-frame-pointer
ENV CFLAGS='-O2 -g -fno-omit-frame-pointer'
ENV CXXFLAGS=$CFLAGS
# Build like CI job "Linux - Meson (64-bit)", minus io_method=io_uring
# (oss-scanner seccomp blocks it), minus sanitizers.  Use "testprep" so the
# tree gets, e.g., src/test/modules binaries.
RUN meson setup \
         $MESON_COMMON_PG_CONFIG_ARGS \
         -Duuid=e2fs \
         -Dliburing=disabled \
         --buildtype=debug \
         -Dllvm=enabled \
         build && \
    ninja -C build -j"$(nproc)" all testprep

# Smoke test like CI SanityCheck
RUN meson test $MTEST_ARGS --suite setup --logbase setup
RUN meson test $MTEST_ARGS cube/regress pg_ctl/001_start_stop || \
    echo "WARNING: SanityCheck reported failures"

USER root
