# SmartCross - Smart City Traffic Simulator

SmartCross est un simulateur C/Linux d'un carrefour intelligent concu pour demontrer les notions de systemes d'exploitation : processus, threads, IPC, memoire partagee, files de messages, pipes, semaphores, ordonnancement et benchmarking.

## Idee

Le carrefour contient jusqu'a 4 routes : Nord, Sud, Est et Ouest. Un processus controleur joue le role du scheduler central. Chaque route est geree par un processus separe et chaque processus route lance plusieurs threads pour generer et faire traverser les voitures.

Le programme montre l'analogie suivante :

| Systeme d'exploitation | SmartCross |
| --- | --- |
| CPU | Zone centrale du carrefour |
| Scheduler | Controleur des feux |
| Processus | Routes |
| Threads | Voitures traitees en parallele |
| File ready | Voitures en attente |
| Semaphore | Zone critique du carrefour |
| Deadlock | Blocage potentiel entre vehicules |
| Priorite | Ambulance |

## Fonctionnalites

| Critere | Implementation |
| --- | --- |
| Multiprocessus configurable | `-p` cree 2 a 5 processus, controleur inclus |
| Multithreading configurable | `-t` definit les threads par route |
| Pipes | Le controleur envoie les feux verts aux routes |
| Memoire partagee | Etat global du carrefour et statistiques |
| Message queue System V | Evenements voiture arrivee, voiture passee, ambulance |
| Semaphores POSIX | Protection des zones critiques du carrefour |
| Probleme classique | Philosophes dineurs adapte aux zones du carrefour |
| Ordonnancement | FCFS, Round Robin, Priorite, SJF adapte, Dynamique |
| Benchmarking | CSV, throughput, attente moyenne, speedup |
| `exec()` | Lance `scripts/plot_results.py` apres le benchmark |

## Structure du code

```text
src/smartcross.h   Types partages et prototypes publics
src/main.c         Parsing CLI et point d'entree
src/common.c       Fonctions utilitaires communes
src/route.c        Processus route, threads, files de voitures, semaphores
src/simulation.c   Controleur central, scheduler, IPC, dashboard
src/benchmark.c    Benchmark, CSV et lancement du script avec exec()
scripts/plot_results.py  Generation du graphique ou resume texte
```

## Compilation

Le projet utilise des API POSIX et doit etre compile sous Linux ou WSL.

Sous Windows, vous pouvez lancer directement l'application avec :

```bat
launch_smartcross.bat
```

Ce script ouvre WSL dans le dossier du projet, compile avec `make`, puis lance `./smartcross`.

Prerequis Ubuntu/WSL :

```bash
sudo apt-get update
sudo apt-get install -y build-essential python3
```

```bash
make
```

Nettoyage :

```bash
make clean
```

## Interface graphique type jeu

Par defaut, `./smartcross` ouvre une fenetre graphique type jeu. La simulation ne demarre pas immediatement : vous choisissez d'abord vos parametres, puis vous cliquez sur `START SIMULATION`.

La fenetre affiche :

1. un ecran de configuration avec mode, strategie, voitures, processus, threads, quantum et vitesse ;
2. un bouton `START SIMULATION` ;
3. un carrefour stylise avec routes, voies, passages pietons et feux ;
4. des voitures animees de facon fluide ;
5. une ambulance en violet en mode priorite ;
6. une salle de controle avec progression, debit, attente moyenne, collisions evitees et evenements temps reel ;
7. une explication en direct de la decision du controleur ;
8. un panneau pedagogique reliant la simulation aux notions OS : processus, threads, IPC, memoire partagee et semaphores ;
9. un bouton `BACK TO SETUP` a la fin pour revenir a l'ecran de configuration.

La fenetre reste ouverte jusqu'a ce que vous la fermiez. Le mode `-quiet` desactive la fenetre graphique, ce qui est utile pour les benchmarks ou les tests rapides. Le mode `-no-gui` lance seulement le moteur C et sert au lanceur graphique.

## Exemples

Simulation normale avec Round Robin :

```bash
./smartcross
```

Dans l'interface, choisissez `Normal`, `Round Robin`, puis cliquez sur `START SIMULATION`.

Simulation avec ambulance prioritaire :

```bash
./smartcross -mode priority -cars 120 -p 5 -t 3
```

Cette commande ouvre aussi l'interface graphique. Pour lancer directement le moteur C sans interface :

```bash
./smartcross -mode priority -cars 120 -p 5 -t 3 -speed 1 -no-gui
```

Demo graphique lente et lisible pour presentation :

```bash
./smartcross -mode priority -cars 120 -p 5 -t 3 -speed 1
```

Demo plus rapide :

```bash
./smartcross -mode normal -strategy dynamic -cars 180 -p 5 -t 4 -speed 3
```

Comparaison des strategies :

```bash
./smartcross -mode benchmark -cars 200 -p 5 -t 4
```

Mode benchmark sans graphique :

```bash
./smartcross -mode benchmark -cars 200 -p 5 -t 4 -no-plot
```

## Options

```text
-mode normal|priority|benchmark
-strategy fcfs|rr|priority|sjf|dynamic
-cars N          nombre total de voitures
-p N             nombre total de processus, 2..5, controleur inclus
-t N             threads par processus route, 1..16
-quantum N       nombre de voitures autorisees par feu vert
-speed N         vitesse demo: 1 tres lent, 5 moyen, 10 rapide
-seed N          graine pseudo-aleatoire
-quiet           sortie compacte
-no-gui          n'ouvre pas la fenetre graphique
-no-plot         ne lance pas le script Python en benchmark
```

## Sorties du benchmark

`./smartcross -mode benchmark` genere :

```text
results.csv
benchmark.png ou benchmark.txt
```

`benchmark.png` est cree si `matplotlib` est disponible. Sinon, le script genere un resume texte `benchmark.txt`.

## Points techniques a presenter

1. Le controleur agit comme un scheduler : il choisit la prochaine route selon FCFS, Round Robin, priorite, SJF ou une strategie dynamique.
2. Les routes sont des processus fils crees avec `fork()`.
3. Les voitures sont traitees par des threads POSIX avec `pthread_create()`.
4. Les feux verts sont envoyes par pipe du controleur vers les routes.
5. Les evenements de simulation transitent par une message queue System V.
6. Les statistiques globales sont dans une memoire partagee `mmap()`.
7. Les zones du carrefour sont protegees par des semaphores POSIX pour eviter les collisions.
8. Les ressources du carrefour sont toujours verrouillees dans un ordre croissant, ce qui illustre la prevention d'interblocage.
9. Le mode benchmark calcule le temps total, l'attente moyenne, le debit, l'utilisation CPU approximative et le speedup.
10. Le programme utilise `exec()` pour lancer un outil externe de generation de graphes.
