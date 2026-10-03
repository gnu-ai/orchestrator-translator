# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

AC_INIT([orchestrator-translator], [0.1.0], [claire@gnu-ai.org])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AC_CANONICAL_HOST

# System extensions (POSIX/XSI): defines _GNU_SOURCE etc. in config.h.
# Required by the POSIX.1-2008 interfaces used from phase 1.  This
# macro must run before anything that compiles.
AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

# Le projet est écrit en C23 ; certains compilateurs nomment encore ce
# mode « c2x ».  Exiger l'un des deux (même politique que
# httpfs-translator, pour une pile homogène).
AC_CACHE_CHECK([whether the C compiler accepts C23], [orch_cv_c23], [
  orch_cv_c23=no
  for orch_flag in -std=c23 -std=c2x; do
    orch_save_CFLAGS=$CFLAGS
    CFLAGS="$CFLAGS $orch_flag"
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[
        constexpr int orch_c23_test = 1;
        static void *orch_c23_p = nullptr;]],
        [[return orch_c23_p == nullptr ? orch_c23_test : 0;]])],
      [orch_cv_c23=$orch_flag])
    CFLAGS=$orch_save_CFLAGS
    test "x$orch_cv_c23" != xno && break
  done])
AS_IF([test "x$orch_cv_c23" = xno],
      [AC_MSG_ERROR([a C23 compiler is required (GCC 13 or newer)])])
AC_SUBST([C23_CFLAGS], [$orch_cv_c23])

AC_CONFIG_HEADERS([config.h])

# Phase 0 : le squelette n'a aucune dépendance externe.  La phase 1
# coordonne des instances de neuron-translator par settrans et
# écrit dans /db ; les bibliothèques Hurd (trivfs/netfs) n'arrivent
# qu'avec le translator lui-même.

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT
