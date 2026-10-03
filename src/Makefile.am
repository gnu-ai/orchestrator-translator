# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# --- Le cœur de l'orchestration ----------------------------------------
# contract.c (descripteur et commandes), neuron.c (formats du
# contrat orchestrator->neuron), instio.c (transports
# d'instances : nœud settrans ou fichiers de test), storage.c
# (storage_write/storage_read : dispatch, backend mémoire pour
# les tests uniquement, backend /db), scheduler.c, aggregator.c
# et orchestrate.c (le moteur) : du C23/POSIX pur, lié dans tous
# les cas.  La bibliothèque est aussi ce que lie la suite de
# tests.
noinst_LIBRARIES = liborchcore.a

liborchcore_a_SOURCES = \
    contract.c \
    neuron.c \
    instio.c \
    storage.c \
    storage_db.c \
    scheduler.c \
    aggregator.c \
    orchestrate.c

noinst_HEADERS = \
    contract.h \
    neuron.h \
    instio.h \
    storage.h \
    storage_db.h \
    scheduler.h \
    aggregator.h \
    orchestrate.h

AM_CPPFLAGS = -I$(top_srcdir)/src
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra

bin_PROGRAMS = orchestrator-translator

# --- La face du binaire, selon le système -------------------------------
# Sur GNU/Hurd, main-hurd.c suit la structure canonique d'un
# translator trivfs et trivfs-hooks.c implémente les trivfs_S_* ;
# ailleurs, main.c construit le même moteur en mode vérification
# sur les flux standards (même convention que la pile).
if ON_HURD
orchestrator_translator_SOURCES = trivfs-hooks.c main-hurd.c
orchestrator_translator_CPPFLAGS = $(AM_CPPFLAGS) -DON_HURD
orchestrator_translator_LDADD = liborchcore.a -ltrivfs -lfshelp \
                               -lports -lshouldbeinlibc
else
orchestrator_translator_SOURCES = main.c
orchestrator_translator_CPPFLAGS = $(AM_CPPFLAGS)
orchestrator_translator_LDADD = liborchcore.a
endif
