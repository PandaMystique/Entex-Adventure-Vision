# Émulateur Adventure Vision

Émulateur fidèle de l'**Entex Adventure Vision** (1982), l'unique console de jeu de table à affichage LED par miroir rotatif. Écrit en un seul fichier C portable avec SDL2.

## Le matériel

L'Adventure Vision était la tentative d'Entex de créer une console de salon. Elle utilisait un miroir rotatif et une colonne de 40 LEDs rouges pour produire un affichage de 150×40 pixels — un principe plus proche du tube cathodique que du Game Boy. Seules cinq cartouches ont été commercialisées, ce qui en fait l'une des consoles les plus rares de l'histoire.

| Composant | Détails |
|---|---|
| CPU | Intel 8048 @ 733 KHz (11 MHz ÷ 15) |
| RAM | 64 octets internes + 4 × 256 octets externes (banques commutées) |
| Son | National COP411L, microcontrôleur 4 bits @ 54,4 kHz |
| Affichage | 150 × 40 LEDs rouges, balayage miroir à 15 ips |
| Entrées | 4 boutons de chaque côté de l'appareil |

## Fonctionnalités

**Précision de l'émulation**

- CPU Intel 8048 complet avec comptage de cycles, timings conditionnels et gestion des interruptions
- Rendu colonne par colonne synchronisé avec la rotation du miroir et timing dynamique de l'impulsion T1
- Émulation comportementale complète du COP411L : générateur de bruit LFSR, glissements de fréquence, enveloppes de volume multi-segments, les 13 effets sonores documentés
- Commutation de banques cartouche via P2 et décodage de la matrice de boutons

**Interface**

- Menu de sélection avec jaquettes intégrées et descriptions des jeux
- Tri alphabétique et support de la souris (clic pour sélectionner, double-clic pour lancer)
- Rendu LED avec persistance phosphorescente par pixel et dégradé rouge chaud
- Sauvegardes d'état (F5/F7), pause (P), reset (R), contrôle du volume (+/−)
- Plein écran au double-clic pendant le jeu
- Affichage à l'écran des messages d'état

## Ludothèque complète

| Jeu | Développeur | Genre |
|---|---|---|
| Defender | Entex / Williams | Shoot'em up horizontal |
| Super Cobra | Entex / Konami | Shoot'em up horizontal |
| Space Force | Entex | Tir fixe |
| Turtles | Entex / Stern / Konami | Labyrinthe / sauvetage |
| Table Tennis | Entex | Sport / Pong |

## Compilation

L'émulateur tient en un seul fichier C. Seuls **SDL2** et la bibliothèque mathématique standard sont nécessaires.

```bash
# Compilation de base
gcc -O2 -DUSE_SDL -o advision adventure_vision.c -lSDL2 -lm

# Avec ROMs et jaquettes intégrées (binaire autonome)
gcc -O2 -DUSE_SDL -DEMBED_ROMS -DEMBED_COVERS -o advision adventure_vision.c -lSDL2 -lm

# Mode sans affichage (test/débogage, sans SDL)
gcc -O2 -o advision adventure_vision.c -lm
```

### Options de compilation

| Option | Effet |
|---|---|
| `-DUSE_SDL` | Active l'affichage, le son et les entrées SDL2 |
| `-DEMBED_ROMS` | Intègre le BIOS et les ROMs dans le binaire (nécessite `embedded_roms.h`) |
| `-DEMBED_COVERS` | Intègre les jaquettes des boîtes de jeu (nécessite `cover_art.h`) |

### Dépendances

- **SDL2** ≥ 2.0.18 — `apt install libsdl2-dev` (Debian/Ubuntu) ou `brew install sdl2` (macOS)
- Un compilateur C99 (GCC, Clang, MSVC)

## Utilisation

```bash
# Lancer le sélecteur de jeux (scanne le répertoire courant)
./advision

# Lancement direct avec fichiers ROM explicites
./advision bios.rom jeu.rom
```

Placez le BIOS (1 Ko) et les ROMs cartouche (4 Ko, `.bin` ou `.rom`) dans le répertoire de travail. Le menu listera automatiquement tous les jeux détectés.

## Contrôles

### En jeu

| Touche | Action |
|---|---|
| ↑ ↓ ← → | Croix directionnelle |
| Z | Bouton 1 (tir) |
| X | Bouton 2 |
| A | Bouton 3 |
| S | Bouton 4 |
| P | Pause / reprise |
| R | Reset |
| +/− | Volume haut / bas |
| F5 | Sauvegarder l'état |
| F7 | Charger l'état |
| F1 | Activer/désactiver la sortie de débogage |
| Échap | Retour au menu |
| Double-clic | Basculer en plein écran |

### Menu

| Touche / Action | Effet |
|---|---|
| ↑ ↓ | Naviguer dans la liste |
| Entrée / Z | Lancer le jeu sélectionné |
| Clic | Sélectionner un jeu |
| Double-clic | Lancer le jeu |
| Échap | Quitter |

## Architecture

Tout tient dans un seul fichier `adventure_vision.c` (~2400 lignes) :

- **Cœur Intel 8048** — jeu d'instructions complet, commutation de banques, entrées T0/T1, contrôleur d'interruptions
- **Sous-système mémoire** — IRAM, XRAM 4 banques avec décodage spécifique AV, ROM cartouche (jusqu'à 4 Ko)
- **Moteur d'affichage** — matrice 150×40 pixels avec rotation miroir synchronisée colonne par colonne, simulation de persistance phosphorescente
- **Son COP411L** — émulation comportementale du microcontrôleur son 4 bits incluant génération de tons à phase précise, bruit LFSR et séquencement d'effets
- **Frontend SDL2** — rendu fenêtré/plein écran, callback audio, mapping des entrées, sauvegardes d'état
- **Menu de jeu** — scanner de ROMs, jaquettes intégrées, base de données des jeux, interface souris avec render target haute résolution

## Références techniques

- Dan Boris — Documentation matérielle de l'Adventure Vision
- MEGA — Recherches complémentaires sur le matériel et le timing d'affichage
- National Semiconductor — Datasheet COP410L/COP411L

## Licence

Projet open source indépendant à des fins éducatives et de préservation.
