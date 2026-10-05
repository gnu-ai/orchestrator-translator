<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

Kanban de l'Orchestrator Translator, dérivé du PLAN.md : une carte
par tâche. Déplacer une carte = la déplacer entre les sections
ci-dessous. Le détail des livrables et des critères d'acceptation
reste dans PLAN.md.
-->

# Orchestrator Translator — Kanban

Dérivé du [`PLAN.md`](PLAN.md). Chaque carte est préfixée par sa
phase ; les critères d'acceptation de chaque phase sont dans le plan.

## À faire

### Phase 2 — Supervisor et parallélisme
- [ ] Surveillance des instances : détection de non-réponse (timeout), redémarrage, incidents persistés dès la détection
- [ ] Exécution parallèle de plusieurs tâches ; file d'attente de descripteurs
- [ ] Limites de ressources : nombre max d'instances simultanées

### Phase 3 — Acquisition réseau : inference + httpfs
- [ ] Lecture de la requête structurée `/inference/request` (type 1) ; extraction des URL
- [ ] Montage dynamique de `httpfs` sur l'URL, lecture de `content` et `status`, gestion des cas non-200 (politique documentée)
- [ ] Transformation du contenu récupéré en vecteurs d'entrée (tokenisation minimale, normalisation)
- [ ] Archivage de chaque contenu dans `training_data` (URL, statut HTTP, contenu, empreinte SHA-256) avant utilisation
- [ ] Mode cluster : instances `neuron-translator` sur nœuds Hurd distants (2 à 5 machines) via SSH, failover du supervisor persisté comme incident

### Phase 4 — Rejouabilité : lecture et requêtes dans la base
- [ ] Relecture des jeux d'entraînement archivés pour rejouer une tâche sans re-télécharger
- [ ] Rejeu d'une exécution : mêmes descripteurs, mêmes entrées, comparaison avec les sorties historisées
- [ ] Historique consultable : performances par topologie à partir des `run_instances`

### Phase 5 — Evaluator avancé et boucle d'amélioration
- [ ] Scores par instance (accord inter-instances, comparaison à la référence), pondération de l'agrégation par score
- [ ] Sélection automatique de topologies : conserver celles qui performent, en réessayer d'autres
- [ ] Statistiques demandées à `neuron-translator` (fichier `stats`, graine) si les ajouts prévus en section 4 sont réalisés

### Phase 6 — Durcissement, tests, CI
- [ ] Suite de tests déterministes (serveur HTTP embarqué sur boucle locale), tests de charge (100+ instances)
- [ ] Documentation utilisateur et architecture (`docs/architecture.md`)

## En cours

_(rien)_

## Fait

### Phase 6 — Durcissement, tests, CI (entamée)
- [x] CI sous QEMU GNU/Hurd, pilotée par [gnu-ai/mistral-vm-debian-hurd](https://github.com/gnu-ai/mistral-vm-debian-hurd) (`.github/workflows/hurd.yml`, la pile complète réellement montée)

### Phase 1 — MVP : orchestration de base avec persistance PostgreSQL
- [x] Scheduler : démarrer/arrêter N `neuron-translator` sur `/llm1…/llmN` avec des topologies différentes
- [x] Distribution d'un vecteur d'entrée à toutes les instances, collecte des sorties
- [x] Aggregator : vote majoritaire et moyenne uniforme
- [x] Translator `/orchestrate` minimal : `run`, `status`, `result` (trio JSON types 2 et 3)
- [x] Persistance dès la première exécution : chaque `run` et `run_instances` écrit via `data-base-translator`

### Phase 0 — Spécification et contrats
- [x] Gel des contrats d'interface (section 3.3 du PLAN)
- [x] Format du descripteur de tâche (JSON simple en ligne)
- [x] Gel du schéma PostgreSQL (utilisé dès la phase 1)
- [x] Couche stockage `storage_write`/`storage_read` (backend `data-base-translator` par défaut, mémoire pour les tests)
- [x] Convention des points de montage : `/llm<N>`, `/web`, `/inference`, `/db`, `/orchestrate`
- [x] Gel du trio JSON `request`/`status`/`result` et des nœuds `/orchestrate/{status,result}` avec inference-translator
- [x] Livrable : `SPEC.md` + squelette de code compilable
