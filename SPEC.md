<!--
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Orchestrator Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# orchestrator-translator — SPEC (contrats gelés, phases 0–1)

Ce document reprend, tel qu'implémenté, les contrats gelés par le
`PLAN.md` (sections 3.3, 4 et 5 phases 0–1). Il sert de référence
croisée avec `neuron-translator` (contrat d'instance),
`data-base-translator` (contrat `orchestrator → database`) et
`inference-translator` (trio JSON).

## 1. Points de montage (gelés)

| Nœud | Rôle | Phase |
|---|---|---|
| `/orchestrate` | ce translator : commandes en `write`, `status`/`result` en `read` | 1 |
| `/llm<N>` | instances de `neuron-translator` montées par le scheduler | 1 |
| `/db` | persistance PostgreSQL via `data-base-translator` | 1 |
| `/web` | transport HTTP via `httpfs-translator` | 3 |
| `/inference` | requête structurée de l'interface | 3 |

## 2. Contrat `orchestrator → neuron` (gelé, conforme à l'implémentation)

Le scheduler monte chaque instance (`settrans -a /llm<N>
<hurd/sigmoid-neuron-translator>`, binaire configurable), puis pour
chaque instance :

1. `write` de la topologie : une ligne `10,20,5` (entiers ≥ 1
   séparés par virgules, ≥ 2 couches — format de
   `parse_config_string`).
2. `write` du vecteur d'entrée : une ligne `0.5,0.3` (flottants
   séparés par virgules — format de `parse_input_string` ; la passe
   avant ne s'exécute que si le vecteur est complet).
3. `read` du texte d'état : la sortie est extraite des lignes
   `[<i>]: <valeur>` (section `Output:` du texte servi par
   l'instance).

Un vecteur d'entrée est limité à **64 valeurs** en phase 1 :
l'interface de commande de `neuron-translator` traite une écriture
par ligne et limite une commande à 1023 octets (limiter le vecteur
est le choix MVP ; relever cette limite appartient à la revue
`stats`/graine de la phase 5).

## 3. Contrat `orchestrator → database` (gelé côté data-base)

Uniquement via `/db` et son protocole de lignes (voir le SPEC.md
de `data-base-translator`) :

- `{"table": "runs", "row": {"descriptor": <descripteur brut>,
  "aggregate_strategy": "<stratégie>"}}` → `{"ok": true, "id": N}` ;
- `{"table": "run_instances", "row": {"run_id": N, "topology":
  "<t>", "input": [<vecteur>], "output": [<vecteur>] ou null,
  "status": "ok" | "failed"}}` → `{"ok": true, "id": M}`.

Le `run` est inséré **avant** la première instance (durabilité dès
la première exécution), chaque `run_instances` **dès sa
collecte**. `final_output` reste NULL en phase 1 : le contrat de
`/db` n'offre pas encore d'opération de mise à jour (revue
conjointe avec la phase 2 de data-base-translator) ; le résultat
agrégé reste servi par `/orchestrate/result`.

## 4. Couche stockage : `storage_write` / `storage_read`

Interface unique (PLAN section 4), deux implémentations :

| Backend | Usage | Transport |
|---|---|---|
| `db` (défaut) | production | nœud POSIX `/db` (translator monté) — ou, pour l'intégration hors Hurd, le binaire de vérification `db-translator` lancé en sous-processus (`dbexec`), même protocole de lignes |
| `mem` | **tests unitaires uniquement, jamais en production** | mémoire |

## 5. Le nœud `/orchestrate` en phase 1 (trivfs)

MVP `trivfs` : un seul nœud, protocole « une ligne JSON en, une
ligne JSON servie en `read` », un curseur par lecteur (le même
mécanisme que `/db`). L'arborescence `/orchestrate/status` et
`/orchestrate/result` (netfs) servira le même JSON en phase 2+ ;
en phase 1, chaque rôle s'obtient par le protocole :

```
echo '{"run": {"instances": 2, "topologies": ["2,4,1"], "input": [0.5, 0.3], "aggregate": "majority"}}' | tee /orchestrate
cat /orchestrate          # → le résultat (type 3)
echo '{"command": "status"}' | tee /orchestrate && cat /orchestrate    # → l'état (type 2)
echo '{"command": "result"}' | tee /orchestrate && cat /orchestrate   # → le dernier résultat
```

Un descripteur nu (`{"instances": …}` sans `run`) est accepté
comme commande `run`. L'exécution est synchrone en phase 1 : le
`write` rend la main quand le run est terminé (parallélisme et
suivi en cours d'exécution : phase 2).

## 6. Descripteur de tâche (gelé)

Une ligne JSON, un objet unique :

```json
{"instances": 3, "topologies": ["10,20,5", "10,30,5"], "input": [0.5, 0.3], "aggregate": "majority"}
```

- `instances` : entier requis, 1..64 ;
- `topologies` : tableau de chaînes requis (≥ 1), chaque topologie
  ≤ 63 octets, cyclées si moins que `instances` ;
- `input` : tableau de nombres requis, 1..64 valeurs ;
- `aggregate` : `"majority"` | `"mean"`, requis ;
- clé inconnue, type inattendu, valeur hors bornes → réponse
  `{"invalid": "<raison>"}` (`line`, `json`, `field`, `type`,
  `value`, `key`, `toolong`), jamais une erreur POSIX.

Le descripteur est persisté **verbatim** dans
`runs.descriptor`.

## 7. Trio JSON `request`/`status`/result (gelé avec inference-translator)

**Type 2 — `status`** (servi par `{"command": "status"}`) :

```json
{"run_id": 12, "state": "done", "instances": [
  {"id": 1, "topology": "2,4,1", "state": "done"},
  {"id": 2, "topology": "2,4,1", "state": "failed"}]}
```

`state` du run : `idle` (avant tout run), `done`, `failed` (aucune
instance n'a rendu de sortie). `state` d'instance : `done`,
`failed`. (`running`/`busy` apparaîtront avec le supervisor de
phase 2.)

**Type 3 — `result`** (servi après un run, ou par
`{"command": "result"}`) :

```json
{"run_id": 12, "state": "done", "aggregate_strategy": "majority",
 "output": "[0.0, 1.0]", "confidence": 0.667}
```

- `output` : le vecteur agrégé, sérialisé en chaîne.
- `confidence` : part des instances **en accord complet** avec la
  sortie agrégée — pour `majority`, mêmes bits (seuil 0.5) sur
  toutes les composantes ; pour `mean`, écart < 0.25 sur toutes
  les composantes.
- sans résultat : `{"empty": true}`.
- échec de transport (instance ou `/db` injoignable) : `EIO` au
  `write`, le slot de lecture est inchangé.

## 8. Agrégation (phase 1)

- **`majority`** : par composante, bit = `valeur ≥ 0.5` ; le bit
  majoritaire l'emporte (égalité → 1) ; la composante agrégée
  vaut `1.0` ou `0.0`.
- **`mean`** : moyenne arithmétique uniforme par composante.
- Composantes manquantes d'une instance plus courte : comptées
  `0.0`. Les pondérations apprises arrivent en phase 5.

## 9. Limites assumées en phase 1

- Exécution des instances **séquentielle** (parallélisme :
  phase 2) ; serveur trivfs mono-thread : pas d'état mutable
  partagé sans verrou, un curseur de lecture par fichier ouvert.
- Une seule ligne de commande par `write` (limite 64 Kio) ; les
  erreurs de contrat sont des réponses, seul le transport échoue
  en `EIO`.
- `final_output` non persisté (voir section 3) ; `seed` et
  `score` des instances NULL (graine et évaluation : phases 5).
