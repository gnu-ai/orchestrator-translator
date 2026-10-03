<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Orchestrator Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# Orchestrator Translator — Projet et feuille de route

`orchestrator-translator` est la **couche de coordination** de la pile
GNU AI pour GNU/Hurd. Il ne fait aucun calcul neuronal lui-même : il
démarre, pilote, supervise, compare et fusionne plusieurs instances de
[neuron-translator](https://github.com/gnu-ai/neuron-translator), les
alimente avec des données récupérées sur le Web via
[httpfs-translator](https://github.com/gnu-ai/httpfs-translator) et
[inference-translator](https://github.com/gnu-ai/inference-translator),
et archive données d'entraînement, résultats et historique dans
PostgreSQL via
[data-base-translator](https://github.com/gnu-ai/data-base-translator).
L'utilisateur dialogue avec la pile par l'interface
`inference-translator`, en local (fichiers POSIX) ou à travers une
session SSH quand GNU AI tourne dans un datacenter ou un cluster
local — l'orchestrateur, lui, ne voit jamais la différence.

Licence : GPLv3 ou version ultérieure. Langage : C23, POSIX.1-2008,
interfaces Hurd (`trivfs`/`netfs`).

---

## 1. Rôle et positionnement

L'esprit Hurd est respecté : chaque responsabilité reste dans un
translator dédié, l'orchestrateur ne fait que **coordonner**.

| Composant | Responsabilité | Lien |
|---|---|---|
| `orchestrator-translator` | coordination : scheduler, supervisor, evaluator, aggregator | ce dépôt |
| `neuron-translator` | unité de calcul : réseau sigmoïde feedforward, piloté par POSIX | gnu-ai/neuron-translator |
| `inference-translator` | interface de dialogue : prompts, trio JSON `request`/`status`/`result`, mode distant SSH | gnu-ai/inference-translator |
| `httpfs-translator` | transport pur HTTP → système de fichiers (`content`, `headers`, `status`) | gnu-ai/httpfs-translator |
| `data-base-translator` | persistance PostgreSQL : données d'entraînement, résultats, historique | gnu-ai/data-base-translator |

### Définitions des quatre rôles de l'orchestrateur

- **Scheduler (ordonnanceur)** : décide *quand* et *combien* d'instances
  de `neuron-translator` exécutent une tâche, et *avec quels paramètres*
  (topologie, configuration). Il démarre et arrête les instances
  (montage de nœuds `settrans`, allocation des points de montage) et
  répartit la charge entre elles.
- **Supervisor (superviseur)** : surveille les instances en cours
  d'exécution — détection de plantage, de non-réponse, de dépassement de
  temps — et les redémarre ou les remplace. Il maintient l'état des
  workers (libre, occupé, défaillant).
- **Evaluator (évaluateur)** : compare les sorties des différentes
  instances entre elles (divergence, accord, confiance) et, quand une
  référence existe, par rapport à un résultat attendu. Il produit un
  score par instance.
- **Aggregator (agrégateur)** : à partir des sorties et des scores,
  produit le résultat final : vote majoritaire, moyenne pondérée,
  sélection du meilleur, ou fusion. C'est la seule sortie "officielle"
  du système.

---

## 2. Fonctionnalités principales

1. **Multi-instanciation** : lancer N `neuron-translator` sur des
   points de montage distincts (`/llm1`, `/llm2`, …) avec des topologies
   et paramètres différents, à partir d'un simple descripteur de tâche.
2. **Distribution des entrées** : envoyer le même vecteur d'entrée
   (ou des lots différents) à chaque instance via les opérations POSIX
   d'écriture.
3. **Collecte et comparaison** : lire les sorties de chaque instance,
   les horodatées, et les comparer (évaluateur).
4. **Agrégation** : produire un résultat final pondéré ou voté.
5. **Acquisition de données par le réseau** : une URL fournie *dans le
   prompt* est extraite par `inference-translator` (requête `request`
   lue dans `/inference/request`), le contenu est lu
   via `httpfs-translator` (`/web/<chemin>/content`), sans que
   l'orchestrateur n'interprète lui-même du HTML ni ne gère de socket.
6. **Persistance PostgreSQL** : les données d'entraînement récupérées
   sur le net, les paramètres de chaque exécution, les sorties brutes,
   les scores et les décisions d'agrégation sont écrits via
   `data-base-translator`.
7. **Interface translator** : l'orchestrateur est lui-même un
   translator monté (par ex. `/orchestrate`), pilotable par des
   commandes POSIX et interrogeable (`cat`) : `/orchestrate/status`
   (suivi de l'exécution) et `/orchestrate/result` (résultat final) —
   les types 2 et 3 du trio JSON consommés par l'interface
   `inference`.

---

## 3. Architecture et flux de données

### 3.1 Vue d'ensemble

```
                    prompt (texte + URL)
                            │
                            ▼
                ┌───────────────────────┐
                │  inference-translator │  extraction prompt/URL
                └──────────┬────────────┘
                           │  /inference/request
                           ▼
                ┌───────────────────────┐
                │   httpfs-translator   │  transport HTTP → fichiers
                └──────────┬────────────┘
                           │  /web/.../content
                           ▼
        ┌──────────────────────────────────────┐
        │       orchestrator-translator        │
        │  scheduler / supervisor /            │
        │  evaluator / aggregator             │
        └───┬───────────────┬──────────────┬───┘
            │ settrans      │ write/read   │ write/read
            ▼               ▼              ▼
      /llm1 … /llmN   data-base-      /orchestrate/status
      neuron-         translator      /orchestrate/result
      translator      PostgreSQL      (interface inference)
```

### 3.2 Flux nominal d'une requête

1. Un prompt contenant une ou plusieurs URL est soumis à
   `inference-translator` (par ex. `echo "résume https://…" |
   tee /inference/prompt`).
2. `inference-translator` expose la requête structurée (type 1,
   `request` : prompt, tâche, URL, paramètres) dans
   `/inference/request` ; l'orchestrateur la lit.
3. L'orchestrateur monte (ou réutilise) une instance `httpfs` avec
   l'URL de base et lit `/web/.../content` et `/web/.../status`.
   Un statut autre que `200` n'est pas une erreur POSIX : c'est à
   l'orchestrateur de décider de la politique (réessai, autre URL,
   abandon).
4. Le contenu récupéré est normalisé en vecteurs d'entrée et archivé
   comme donnée d'entraînement via `data-base-translator`.
5. Le scheduler démarre ou sélectionne N instances de
   `neuron-translator` avec des paramètres différents, leur envoie
   les entrées, le supervisor surveille leur exécution.
6. L'evaluator compare les sorties, l'aggregator produit le résultat
   final.
7. Exécution, sorties et décision sont archivés dans PostgreSQL ;
   le résultat final est lisible dans `/orchestrate/result` (type 3),
   et l'interface `inference` suit l'exécution en streaming via
   `/orchestrate/status` (type 2).

### 3.3 Contrats d'interface (principe clé)

Chaque interaction passe par le système de fichiers, jamais par des
sockets ni des API propriétaires. Les contrats suivants sont gelés dès
la phase 0 pour que les phases ultérieures n'invalident pas le code
existant :

| Contract | Échange |
|---|---|
| `orchestrator → neuron` | `write` de la topologie, puis `write` du vecteur d'entrée ; `read` de la sortie. Conforme à l'interface actuelle de `neuron-translator`. |
| `orchestrator → httpfs` | `settrans` d'un nœud avec l'URL de base ; `read` de `…/content`, `…/headers`, `…/status`. |
| `orchestrator → inference` | `read` de la requête structurée `/inference/request` (type 1 : URL, tâche, paramètres) ; c'est l'utilisateur qui soumet le prompt, jamais l'orchestrateur. |
| `inference → orchestrator` | `read` de `/orchestrate/status` (type 2) et `/orchestrate/result` (type 3) par l'interface `inference` ; elle n'écrit jamais dans l'orchestrateur. |
| `orchestrator → database` | `write` des enregistrements (données d'entraînement, exécutions, résultats) ; `read` des requêtes (jeux d'entraînement, historique). Schéma SQL défini en phase 0. |
| `utilisateur → orchestrator` | `write` de commandes (`run`, `status`, `set …`) ; `read` de l'état (`status`) et du résultat (`result`). |

Le **trio JSON** `request`/`status`/`result` est gelé en phase 0 avec
`inference-translator` : en local, ce sont des fichiers
(`/inference/request`, `/orchestrate/status`, `/orchestrate/result`) ;
en mode distant (datacenter/cluster), l'IHM les transporte par une
session SSH. L'orchestrateur ne voit jamais la différence : il ne
connaît que les fichiers.

---

## 4. Décisions de conception (réponses aux points en suspens)

### Faut-il modifier `neuron-translator` ?

Non pour le MVP. Son interface POSIX (écrire la topologie, écrire
l'entrée, lire la sortie) suffit à l'orchestrateur, qui n'a pas besoin
d'être "appelé" par le neurone : c'est lui qui interroge. Deux
ajouts pourront être demandés *plus tard*, sans casser le contrat :

- un fichier `stats` lisible (compteurs de passes, erreurs) pour
  aider l'evaluator ;
- la possibilité de fixer/lire la graine des poids initiaux, pour la
  reproductibilité des expériences archivées.

Le principe reste : `neuron-translator` est une unité de calcul sans
connaissance de l'orchestrateur.

### Ne pas prévoir la base de données dès le départ n'est-il risqué ?

Oui — aussi la décision inverse est prise : **PostgreSQL est intégré dès
le MVP** (phase 1). Chaque exécution est persistée dès la première
ligne de code fonctionnel : plus aucun chemin "sans BDD" ne pourra se
creuser. Pour que l'intégration ne fige pas l'architecture, la
persistance passe par une interface unique (`storage_write` /
`storage_read`) :

- l'implémentation **par défaut** s'appuie sur `data-base-translator`
  (contrat de la phase 0, schéma de la section 6) ;
- une implémentation **en mémoire** existe uniquement pour les tests
  unitaires sans serveur PostgreSQL — jamais en production.

Si `data-base-translator` n'est pas encore prêt, la phase 1 démarre
avec son périmètre minimal (création du schéma + écriture de `runs` et
`run_instances`) ; l'orchestrateur, lui, est écrit directement contre
le contrat, sans stub temporaire à remplacer plus tard.

### Que faut-il savoir du mode distant (datacenter/cluster) ?

Rien. La pile peut tourner dans un datacenter ou un cluster local ;
l'IHM de l'utilisateur s'y connecte par SSH avec une clé nominative
(registre servi par `data-base-translator`). L'orchestrateur n'est
pas concerné : il ne voit que des fichiers locaux (`/inference`,
`/llm<N>`, `/db`, `/orchestrate`), n'écoute sur aucun port, ne gère
ni clé ni chiffrement. Sa seule obligation est de respecter le trio
JSON gelé — servir `status` et `result` en `read` propre et complet —
pour que l'interface puisse suivre une exécution et lire le résultat
en streaming, en local comme à distance.

### Politique d'agrégation initiale

Le MVP implémente le vote majoritaire (sorties discrètes) et la
moyenne pondérée uniforme (sorties réelles). Les pondérations appris
par l'evaluator (score des instances) arrivent en phase 5.

---

## 5. Phases

Chaque phase a un livrable, des critères d'acceptation et une
dépendance explicite sur la précédente. La phase 3 dépend en outre
des phases 0–1 d'`inference-translator` (translator `/inference` et
requête structurée).

### Phase 0 — Spécification et contrats (avant tout code)

- Gel des contrats d'interface de la section 3.3.
- Format du descripteur de tâche (JSON simple en ligne) :
  `{ "instances": 3, "topologies": ["10,20,5", "10,30,5", "10,20,10"],
     "input": [0.5, 0.3, …], "aggregate": "majority" }`.
- Schéma PostgreSQL (voir section 6) : gelé ici, **utilisé dès la
  phase 1**.
- Couche stockage : interface unique `storage_write`/`storage_read`,
  backend `data-base-translator` par défaut, backend mémoire réservé
  aux tests.
- Convention des points de montage : `/llm<N>`, `/web`, `/inference`,
  `/db`, `/orchestrate`.
- Gel du trio JSON `request`/`status`/`result` avec
  `inference-translator` et des nœuds `/orchestrate/status` et
  `/orchestrate/result` que l'interface lit en streaming.
- **Livrable** : `SPEC.md` + squelette de code compilable.
- **Acceptation** : revue des contrats avec les dépôts voisins,
  notamment le périmètre minimal de `data-base-translator` (création du
  schéma + écriture) requis pour la phase 1, et revue croisée du trio
  JSON et des nœuds `/orchestrate/{status,result}` avec
  `inference-translator`.

### Phase 1 — MVP : orchestration de base avec persistance PostgreSQL

- Scheduler : démarrer/arrêter N `neuron-translator` sur `/llm1…
  /llmN` avec des topologies différentes.
- Distribution d'un vecteur d'entrée à toutes les instances, collecte
  des sorties.
- Aggregator : vote majoritaire et moyenne uniforme.
- Translator `/orchestrate` minimal : `run` (lancer un descripteur de
  tâche), `status`, `result` — ces deux derniers nœuds servent le trio
  JSON (types 2 et 3) consommé par l'interface `inference`.
- **Persistance dès la première exécution** : chaque `run`
  (descripteur, date, stratégie) et chaque `run_instances`
  (topologie, entrée, sortie, statut, score) est écrit dans
  PostgreSQL via `data-base-translator`.
- **Livrable** : `orchestrator-translator` compilable sous Hurd, testé
  avec 2 et 8 instances, données visibles dans PostgreSQL.
- **Acceptation** : une exécution complète sur Debian GNU/Hurd
  (VM QEMU), `make check` vert, et la trace de l'exécution
  retrouvée dans les tables `runs`/`run_instances`.

### Phase 2 — Supervisor et parallelisme

- Surveillance des instances : détection de non-réponse (timeout),
  redémarrage, journal des défaillances — chaque incident est
  persisté dans la table `incidents` dès sa détection.
- Exécution parallèle de plusieurs tâches ; file d'attente de
  descripteurs.
- Limites de ressources : nombre max d'instances simultanées.
- **Livrable** : supervisor opérationnel + tests de défaillance
  (instance tuée en cours de tâche, instance lente), incidents
  visibles dans PostgreSQL.
- **Acceptation** : une tâche survit à la perte d'une instance et
  rend quand même un résultat agrégé ; l'incident et le résultat
  coexistent dans la base.

### Phase 3 — Acquisition réseau : inference + httpfs

- Lecture de la requête structurée `/inference/request` (type 1) via
  `inference-translator` ; extraction des URL.
- Montage dynamique de `httpfs` sur l'URL, lecture de `content` et
  `status`, gestion des cas non-200 (politique : réessai, saut,
  abandon documenté).
- Transformation du contenu récupéré en vecteurs d'entrée
  (tokenisation minimale, normalisation).
- Chaque contenu récupéré est archivé comme donnée d'entraînement
  dans `training_data` (URL, statut HTTP, contenu, empreinte
  SHA-256 anti-doublon) **avant** d'être utilisé.
- **Livrable** : une tâche complète "prompt → URL → contenu →
  N réseaux → résultat agrégé", données d'entraînement persistées.
- **Acceptation** : démonstration de bout en bout avec une URL réelle
  et une URL volontairement en erreur (404) ; les deux traces
  (contenu et 404) sont dans `training_data`.

### Phase 4 — Rejouabilité : lecture et requêtes dans la base

- Relecture des jeux d'entraînement archivés pour rejouer une tâche
  sans re-télécharger.
- Rejeu d'une exécution : mêmes descripteurs, mêmes entrées,
  comparaison des nouvelles sorties avec les sorties historisées.
- Historique consultable : performances par topologie à partir des
  `run_instances` persistés depuis la phase 1.
- **Livrable** : commandes `replay` et `history` du translator
  `/orchestrate`.
- **Acceptation** : rejouer une tâche de la phase 3 à partir des
  données archivées uniquement (réseau coupé).

### Phase 5 — Evaluator avancé et boucle d'amélioration

- Scores par instance (accord inter-instances, comparaison à la
  référence), pondération de l'agrégation par score.
- Sélection automatique de topologies : conserver les configurations
  qui performent, en réessayer d'autres.
- Statistiques demandées à `neuron-translator` (fichier `stats`,
  graine) si les ajouts prévus en section 4 sont réalisés.
- **Livrable** : agrégation pondérée apprise.
- **Acceptation** : sur un jeu de test étiqueté, la sortie agrégée
  pondérée bat la moyenne uniforme.

### Phase 6 — Durcissement, tests, CI

- Suite de tests déterministes (serveur HTTP embarqué sur boucle
  locale, comme `httpfs-translator`), tests de charge (100+ instances).
- CI sous QEMU GNU/Hurd, pilotée par le sandbox
  [gnu-ai/mistral-vm-debian-hurd](https://github.com/gnu-ai/mistral-vm-debian-hurd).
- Documentation utilisateur et architecture (`docs/architecture.md`).
- **Livrable** : version 1.0.

---

## 6. Schéma PostgreSQL (gelé en phase 0, utilisé dès la phase 1)

```sql
-- Données d'entraînement récupérées sur le net
CREATE TABLE training_data (
    id          BIGSERIAL PRIMARY KEY,
    source_url  TEXT NOT NULL,
    fetched_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    http_status INT,
    content     TEXT NOT NULL,
    checksum    CHAR(64) NOT NULL UNIQUE      -- SHA-256, anti-doublon
);

-- Chaque exécution orchestrée
CREATE TABLE runs (
    id          BIGSERIAL PRIMARY KEY,
    started_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    descriptor  JSONB NOT NULL,               -- descripteur de tâche
    final_output JSONB,
    aggregate_strategy TEXT NOT NULL
);

-- Une ligne par instance de neuron-translator dans une exécution
CREATE TABLE run_instances (
    id          BIGSERIAL PRIMARY KEY,
    run_id      BIGINT NOT NULL REFERENCES runs(id),
    topology    TEXT NOT NULL,                -- ex. "10,20,5"
    seed        BIGINT,
    input       JSONB NOT NULL,
    output      JSONB,
    score       REAL,
    status      TEXT NOT NULL                 -- ok | failed | timeout
);

-- Historique des défaillances (supervisor)
CREATE TABLE incidents (
    id          BIGSERIAL PRIMARY KEY,
    run_id      BIGINT REFERENCES runs(id),
    instance_id BIGINT REFERENCES run_instances(id),
    kind        TEXT NOT NULL,                -- crash | timeout | restart
    detected_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

Les tables du registre de clés SSH du mode distant (`users`,
`access_keys`, `auth_failures`) sont gelées dans le `PLAN.md` de
`data-base-translator` (sa section 6), qui en est la source de
vérité ; l'orchestrateur ne les lit ni ne les écrit — le mode
distant est porté par `inference-translator`.

---

## 7. Contraintes et conventions techniques

- **Langue du code et des commentaires** : anglais, style Claude
  Delannoy (commentaires abondants), en cohérence avec
  `neuron-translator`.
- **C23 / POSIX.1-2008**, bibliothèques Hurd (`trivfs` pour le MVP,
  `netfs` si l'arborescence de commandes s'étoffe).
- **Zéro allocation dans les chemins chauds**, mémoire contiguë,
  cohérent avec les choix de performance de `neuron-translator`.
- **Multitâche et multi-utilisateurs** : l'orchestrateur doit piloter
  plusieurs runs **simultanément** pour plusieurs utilisateurs —
  exécutions concurrentes sur des topologies distinctes, état
  cloisonné par `run_id` (descripteurs, `/llm<N>`, agrégats), aucun
  état global non protégé ne doit sérialiser les utilisateurs entre
  eux ; le scheduler et le supervisor deviennent le point de
  passage obligé de cette exigence, testé avec des runs concurrents.
- **Erreurs réseau ≠ erreurs POSIX** : un statut HTTP non-200 est une
  donnée exploitable (philosophie `httpfs`), seule la couche transport
  peut échouer.
- **Mode distant sans impact** : ni SSH, ni clés, ni port d'écoute —
  ces responsabilités appartiennent à `inference-translator` et au
  registre de `data-base-translator`.
- Chaque translator reste remplaçable : l'orchestrateur ne connaît que
  les points de montage et les contrats, jamais les binaires.

### Stratégie de tests unitaires

- **Harnais maison minimal** : macros `CHECK` et compteurs en
  C23/POSIX, zéro framework externe — même convention que
  `tests/test_neuron.c` (`neuron-translator`) et la suite
  d'`httpfs-translator` ; `make check` est la cible standard, exigée
  par les critères d'acceptation.
- **Un rôle, une suite** : scheduler, supervisor, evaluator et
  aggregator sont testés isolément, avec des instances de
  `neuron-translator` simulées par fichiers de test — l'orchestrateur
  ne connaissant que les contrats, un fichier qui se comporte comme
  `/llm<N>` suffit.
- **Stockage mémoire pour les tests** : le backend `storage_write` /
  `storage_read` en mémoire (section 4) sert tous les tests
  unitaires sans serveur PostgreSQL ; la persistance réelle est
  vérifiée en intégration sur instance jetable.
- **Réseau simulé, jamais réel** : serveur HTTP embarqué sur boucle
  locale, à la manière d'`httpfs-translator` ; contenus et statuts
  non-200 sont des fichiers de test.
- **Déterminisme** : descripteurs et entrées figés ; aucune horloge
  ni aléa non semé n'entre dans la suite.

---

## 8. Jalons synthétiques

| Phase | Contenu | Dépend de |
|---|---|---|
| 0 | Spécification, contrats, schéma SQL | — |
| 1 | MVP : N neuron-translator, agrégation simple, persistance PostgreSQL | 0 |
| 2 | Supervisor, parallelisme, résilience (incidents persistés) | 1 |
| 3 | Prompt → URL → httpfs → entrées réseau (training_data persisté) | 1, inference 0–1 |
| 4 | Rejouabilité : rejeu, historique, requêtes en base | 1, 3 |
| 5 | Évaluation pondérée, sélection de topologies | 2, 4 |
| 6 | Durcissement, CI Hurd, v1.0 | 1–5 |

Deux fils de travail (1→2 et 1→3) peuvent avancer en parallèle après
le MVP. La base de données est intégrée dès la phase 1 : elle n'est
plus un chantier en soi mais une propriété permanente du système —
chaque phase qui suit ne fait qu'écrire ou lire davantage dans les
tables déjà créées.

Le mode distant (datacenter/cluster) est porté par
`inference-translator` (SSH, clés SSH nominatives) et le registre de
`data-base-translator` : l'orchestrateur n'y participe pas, il ne fait
que respecter le trio JSON `request`/`status`/`result`.
