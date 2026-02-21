# Entex Adventure Vision — Émulateur v15

Émulateur fidèle de la console portable **Entex Adventure Vision** (1982), la seule console à affichage LED à miroir rotatif jamais commercialisée.

## Matériel émulé

| Composant | Spécification |
|-----------|--------------|
| CPU | Intel 8048 @ 733 KHz (11 MHz ÷ 15) |
| RAM interne | 64 octets IRAM |
| RAM externe | 4 × 256 octets XRAM (banques via P1.0-P1.1) |
| ROM BIOS | 1 Ko (interne 8048) |
| ROM cartouche | 4 Ko max (adressage via P2.0-P2.3) |
| Affichage | 150 × 40 pixels LED rouges, miroir rotatif 15 fps |
| Son | COP411L @ ~54,4 KHz, DAC 2 bits, 13 effets + 16 notes |
| Entrées | Croix directionnelle + 4 boutons (matrice P1.3-P1.7) |

## Nouveautés v15

### Fidélité d'émulation
- **Savestate COP411L complet** : phase_acc, phase_inc, cur_step, step_samples_left, segments — reprise audio bit-exacte
- **Mode scan mid-frame** (F3) : capture VRAM en temps réel pendant l'exécution CPU, reproduisant le balayage progressif du miroir physique
- **Timing configurable** : T1_PULSE_START/END paramétrable dans `advision.ini` pour ajustement sur mesures hardware

### Audio
- **3 profils audio** (F4 pour cycler) : Raw (brut), Speaker (passe-bas ~4kHz + soft clip tanh), Headphone (~8kHz, sans distorsion)
- Filtre passe-bas configurable par profil avec saturation douce simulant le petit haut-parleur

### Vidéo et rendu
- **Gamma LED configurable** dans `advision.ini` (0.2–3.0, défaut 1.0)
- **Phosphor decay configurable** (0.0–1.0, défaut 0.45)
- **Effet scanlines** (F9) : assombrissement alterné des lignes LED
- **Integer scaling** (F6) : mise à l'échelle en multiples entiers stricts
- **Overlay statistiques** (touche `) : FPS mesuré, cycles CPU, pixels allumés

### Débogage et outils
- **Suite de tests intégrée** (`--test`) : 13 tests couvrant CPU (MOV, ADD, carry, JMP, DJNZ, DAA), timer/prescaler, COP411L (init, tone, noise), persistance phosphore, round-trip savestate
- **Mode headless enrichi** : `--frames N`, `--input UDLR1234`, `--dump` (ASCII art VRAM)
- **Dump VRAM ASCII** : visualisation texte du framebuffer pour debug et tests automatisés

### Héritées de v14
- Rewind (F8), enregistrement WAV (F2), capture d'écran BMP (F12)
- Drag & drop ROM, fichier `advision.ini`, CLI étendue
- Portabilité MSVC, indices de contrôle par jeu dans le menu

## Bibliothèque de jeux

| Jeu | Année | Genre | Contrôles |
|-----|-------|-------|-----------|
| **Defender** | 1982 | Shoot horizontal | Z:tir X:poussée A:bombe |
| **Super Cobra** | 1982 | Shoot horizontal | Z:tir X:bombe |
| **Space Force** | 1982 | Tir fixe | Z:tir |
| **Turtles** | 1982 | Labyrinthe | Flèches Z:boîte |
| **Table Tennis** | 2020 | Sport/Pong (homebrew, Ben Larson) | ↑↓:raquette Z:service |

## Compilation

```bash
# Standard (SDL2)
gcc -O2 -DUSE_SDL -o advision adventure_vision.c -lSDL2 -lm

# Complet (ROMs + jaquettes intégrées)
gcc -O2 -DUSE_SDL -DEMBED_ROMS -DEMBED_COVERS -o advision adventure_vision.c -lSDL2 -lm

# Headless (tests, automatisation)
gcc -O2 -o advision adventure_vision.c -lm

# MSVC (Windows)
cl /O2 /DUSE_SDL adventure_vision.c SDL2.lib SDL2main.lib
```

## Utilisation

```bash
./advision                              # Sélecteur de jeux
./advision bios.rom game.rom            # Chargement direct
./advision --fullscreen --volume 8      # Avec options
./advision --test                       # Suite de tests
./advision --frames 120 --dump bios.rom game.rom  # Headless + dump
```

## Contrôles en jeu

| Touche | Action |
|--------|--------|
| ↑ ↓ ← → | Croix directionnelle |
| Z / X / A / S | Boutons 1-4 |
| P | Pause |
| R | Reset |
| +/- | Volume |
| ` | Overlay statistiques (FPS/cycles/pixels) |
| F2 | Enregistrement WAV on/off |
| F3 | Mode scan mid-frame on/off |
| F4 | Cycler profil audio (Raw→Speaker→Headphone) |
| F5 / F7 | Sauvegarder / Charger état |
| F6 | Integer scaling on/off |
| F8 | Rewind (retour en arrière) |
| F9 | Scanlines on/off |
| F11 | Plein écran |
| F12 | Capture d'écran BMP |
| Échap | Menu / Quitter |
| Double-clic | Basculer plein écran |

## Configuration (`advision.ini`)

```ini
[advision]
volume=7
fullscreen=0
scale=0
audio_profile=1          # 0=Raw 1=Speaker 2=Headphone
gamma=1.00               # LED gamma (0.2-3.0)
phosphor=0.45            # Persistance POV (0.0-1.0)
scanlines=0
integer_scale=0
# Timing (avancé — ajuster si mesures hardware disponibles)
t1_pulse_start=200
t1_pulse_end=400
```

## Suite de tests (`--test`)

```
$ ./advision --test
=== Adventure Vision Self-Test Suite ===
State saved.
State loaded.

13 passed, 0 failed (13 total)
```

Tests couverts : MOV A, ADD carry, JMP, DJNZ loop, DAA (BCD), timer prescaler/overflow, COP411L init/tone/noise, persistance phosphore, round-trip savestate complet.

## Corrections v15.1 (audit de code)

- **Thread safety** : tout accès à `av->snd` (save/load state, rewind push/pop) est maintenant sous `SDL_LockAudioDevice` ; macros `AUDIO_LOCK`/`AUDIO_UNLOCK` pour cohérence
- **WAV ring buffer** : `audio_cb` écrit dans un tampon annulaire (8192 samples), vidé côté thread principal — plus aucune E/S disque dans le thread audio
- **Préservation de l'état persistant** : le retour au menu ne perd plus `rewind_buf`, `wav`, volume, gamma, config ; plus de fuite mémoire ni de fichier WAV orphelin
- **load_file robuste** : avertissement explicite si ROM tronquée, échec sur lecture partielle
- **CLI sûre** : `atoi` remplacé par `strtol` avec validation complète
- **Pas de deadlock** : les verrous internes redondants dans `load_state` sont supprimés (l'appelant verrouille)

## Corrections v15.3 (précision émulation hardware)

### Pipeline d'affichage LED (Daniel Boris doc §4.3)
- **Registres LED matériels** : émulation des 5 registres LED 8 bits du hardware réel. Chaque registre contrôle 8 LEDs (40 au total). L'écriture se fait comme effet de bord de la lecture XRAM (MOVX A,@Rr), exactement comme sur le vrai hardware
- **Décodage adresse P2.5-P2.7** : sélection des registres LED par les bits 5-7 du port P2 : `100→reg0 (LEDs 1-8)`, `010→reg1 (9-16)`, `110→reg2 (17-24)`, `001→reg3 (25-32)`, `101→reg4 (33-40)`
- **Strobe P2.4** : front montant de P2.4 = latch des registres LED vers la colonne d'affichage courante. Synchronisation colonne-par-colonne identique au BIOS réel
- **Compteur de colonnes** : remis à zéro sur le front montant T1 (sync miroir), puis incrémenté par chaque strobe P2.4 — timing cycle-exact
- **Mode hybride** : si le BIOS utilise les registres LED (P2.4 détecté), ils sont prioritaires. Sinon fallback vers lecture directe XRAM (compatibilité homebrews)

### CPU 8048
- **Délai post-EI** : l'instruction EI (0x05) impose un délai d'1 instruction avant que les IRQ soient acceptées (comportement hardware MCS-48 documenté)
- **Dispatch IRQ** : vérification du compteur `ei_delay` avant dispatch — corrige un race condition potentiel entre EI et timer overflow

### Stabilité
- **P2 tracking** : `prev_p2` sauvegardé pour détection de fronts (P2.4 strobe, protocole son)
- **État transient** : les registres LED et le compteur de colonnes sont réinitialisés à chaque début de trame
- **Restauration savestate** : `prev_p2` synchronisé avec P2 CPU après chargement

## Corrections v15.1 (précision émulation)

### CPU 8048
- **Horloge CPU** : 737280 → 733333 Hz (11 MHz ÷ 15 exact, doc §1.0) — 0.54% plus précis
- **Cycles/frame** : 49152 → 48889 (division corrigée)
- **Timer prescaler** : réinitialisé sur `STRT T`, `STRT CNT`, `STOP TCNT` et `MOV T,A` (MCS-48 manual + MAME)

### Son COP411L
- **Registre de contrôle** : bits 0/3 inversés — bit 0 = fast/slow, bit 3 = loop (doc §6.1-6.2)
- **Fréquences des tons** : remplacées par les fréquences nominales mesurées sur le hardware (doc §6.2, table Freq Nominal) au lieu du tempérament égal
- **Durées des segments** : seg1 et seg2 ont des durées distinctes (doc §6.2 : fast=0 → 117ms/240ms, fast=1 → 46ms/104ms)
- **Protocole son** : machine d'état 4 états, accepte toutes les valeurs de commande y compris $00/$C0 (routine BIOS $03A9)

### Affichage
- **Scan miroir sync-aware** : les colonnes sont capturées dans une fenêtre de ~2550 cycles après la fin du pulse T1 (front montant), au lieu d'être réparties linéairement sur toute la trame
- **Mid-frame scan par défaut** : activé par défaut pour plus de précision

## Corrections v15.2 (audit sécurité)

- **Savestate OOB** (critique) : `cur_step`, `step_count`, `segment` validés après chargement — un `.sav` malveillant ne peut plus provoquer d'accès hors bornes dans `steps[16]`
- **Savestate NaN/Inf** : `cur_freq`, `cur_vol`, `seg1_vol`, `seg2_vol` et toutes les fréquences/volumes des steps rejetés si non finis
- **Savestate portabilité** : `sizeof(bool)` et `sizeof(int)` remplacés par types à largeur fixe (`uint8_t`, `int32_t`) — format SAVE_VER 18
- **Garde OOB runtime** : `cop411_sample()` vérifie `cur_step < MAX_SND_STEPS` même en fonctionnement normal (défense en profondeur)
- **CLI `--volume`** : n'est plus écrasé silencieusement par `advision.ini` (appliqué après `config_load`)
- **Integer scaling** : le flag F6 fonctionne réellement — calcul en pixels natifs, letterbox centré, restauration du mode logique
- **Texture statique** : suivi du renderer pour invalidation si le contexte GPU change ; fuite mémoire éliminée
- **LUT gamma** : `powf()` éliminé de la boucle chaude render (table 256 entrées, recalculée uniquement si gamma change)
- **WAV batch writes** : `wav_flush` écrit par segments contigus au lieu de sample-par-sample ; détection d'overflow ring buffer
- **T1 pulse** : `t1_pulse_start >= t1_pulse_end` rejeté → plus de boucle infinie BIOS
- **Config INI** : `gamma` et `phosphor` rejettent `NaN`/`Inf` via `isfinite()`

## Architecture

Émulateur mono-fichier C (~3500 lignes), zéro dépendance externe hors SDL2.

| Module | Lignes | Description |
|--------|--------|-------------|
| Intel 8048 | ~450 | CPU cycle-exact, 105 opcodes, timer/IRQ |
| COP411L | ~300 | Son comportemental, LFSR 15 bits, 3 profils audio |
| Display | ~100 | Rendu LED POV, gamma, scanlines, phosphor configurable |
| Rewind | ~80 | Buffer circulaire 120 snapshots |
| Save/Load | ~120 | Savestate complet (CPU + COP411L playback) |
| Self-test | ~130 | 13 tests unitaires intégrés |
| Menu | ~500 | Sélecteur, jaquettes, infos, contrôles par jeu |
| Config | ~80 | INI persistant avec timing avancé |

## Références

- [Dan Boris — Adventure Vision Technical Info](http://www.intv.co/advision/)
- [MEGA — Entex Adventure Vision](http://www.intv.co/advision/mega.html)
- [Intel MCS-48 Datasheet](https://archive.org/details/intel-mcs-48)
- [COP411L Datasheet (National Semiconductor)](https://datasheets.chipdb.org/)
