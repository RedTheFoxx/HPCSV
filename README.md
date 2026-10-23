# HPCSV — High Performance CSV

![HPCSV, tableur natif pour fichiers CSV](docs/screenshot.png)

**HPCSV** rend n'importe quel `.csv` lisible et éditable comme un tableur, avec un cœur
écrit en **C pur**, sans dépendance, et une extension pour **Zed**.

Le projet est composé de trois binaires :

| Binaire | Rôle |
|---|---|
| `HPCSV.exe` | Le tableur natif (Win32, GDI, zéro dépendance). Grille virtualisée, édition, largeurs de colonnes, presse-papiers, annuler/rétablir. |
| `hpcs-lsp.exe` | Serveur LSP en C : diagnostics de lignes irrégulières, hover de cellule, action « ouvrir dans HPCSV ». |
| `hpcs.exe` | CLI et outil de diagnostic (`info`, `get`, `set`, `dump`, `bench`). |

---

## Le point à connaître sur Zed

Une extension Zed ne peut fournir que des **langages/tree-sitter, des LSP, des débogueurs,
des thèmes, des snippets et des serveurs MCP/ACP**. L'API ne permet pas de dessiner une vue
custom, un webview ou un panneau (le RFC [Visual Extension API](https://github.com/zed-industries/zed/discussions/53403)
est toujours ouvert, et la roadmap « Webview via Extensions » l'est aussi). Un onglet
tableur *dessiné par l'extension* est donc impossible aujourd'hui.

HPCSV prend ce problème par l'autre bout :

> **L'onglet CSV de Zed reste la source de vérité.** Le tableur natif édite **le même fichier**,
> et les deux se synchronisent en direct.

- vous éditez dans Zed → HPCSV recharge la grille en ~350 ms ;
- vous éditez dans la grille → HPCSV sauvegarde de façon atomique → Zed recharge l'onglet ;
- l'extension apporte la coloration CSV/TSV, les diagnostics, le hover et un bouton
  « HPCSV: open in spreadsheet » directement dans l'éditeur.

C'est la seule architecture qui donne un **vrai tableur** aujourd'hui, sans fork de Zed.

---

## Fonctionnalités

### Tableur natif (`HPCSV.exe`)

- Grille virtualisée : seules les cellules visibles sont dessinées, scrolling fluide même
  sur des millions de lignes.
- Édition en place (F2, Entrée ou frappe directe), validation par Entrée/Tab, annulation Échap.
- En-tête figé (première ligne du CSV), numéros de ligne, colonnes redimensionnables.
- Sélection rectangulaire à la souris, copier/coller TSV compatible Excel/LibreOffice.
- Annuler/rétablir multi-cellules (`Ctrl+Z` / `Ctrl+Y`).
- Largeurs de colonnes calculées automatiquement à l'ouverture.
- Alignement à droite et coloration des valeurs numériques.
- Lignes irrégulières signalées dans la gouttière.
- Sauvegarde atomique (fichier temporaire + `MoveFileEx`), BOM/CRLF/quoting préservés.
- Rechargement automatique si le fichier change sur le disque ; garde-fou si une édition
  locale est en cours.
- Détection du séparateur (`,`, `;`, tabulation, `|`).
- Dossiers/chemins Unicode, drag & drop de fichiers.
- Ajout de ligne (`Ctrl+Entrée`), fichiers vides éditables.

### Extension Zed

- Langages **CSV** et **TSV** (grammaire tree-sitter, highlighting).
- LSP `hpcs-lsp` :
  - **diagnostics** : nombre de champs différent de l'en-tête, guillemet non fermé ;
  - **hover** : adresse de la cellule, nom de colonne, type, longueur et tableau
    récapitulatif de la ligne ;
  - **code action** : `HPCSV: open in spreadsheet` (palette d'actions / `Ctrl+.`).
- Tâche projet prête à l'emploi dans `.zed/tasks.json`.

### Cœur C

- Parseur RFC 4180 tolérant : champs quotés, `""`, retours à la ligne embarqués, CRLF,
  BOM UTF-8, champs vides, ligne finale sans `\n`.
- Index zéro-copie : chaque cellule = offsets dans le buffer, aucun `malloc` par cellule.
- Édition par **splice** : seuls les octets de la cellule modifiée sont réécrits, puis les
  offsets suivants sont décalés — pas de reparse complet pour une édition.
- 1 000 001 lignes × 8 colonnes (80,26 Mio) : **~79 ms** de parsing, soit **~1,0 Gio/s**
  sur la machine de développement (voir [Performances](#performances)).

---

## Architecture

```
                 ┌──────────────────────────┐
   onglet Zed ──▶│  fichier .csv sur disque │◀── sauvegarde atomique
   (édition)     └────────────┬─────────────┘
                             │ watch mtime
                    ┌────────▼─────────┐
                    │ HPCSV.exe        │
                    │ C core + Win32   │
                    └──────────────────┘
                             ▲
                             │ hpcs.open (spawn)
                    ┌────────┴─────────┐
                    │ hpcs-lsp.exe     │  diagnostics / hover
                    │ C, JSON-RPC      │
                    └──────────────────┘
```

```
HPCSV/
├── core/        hpcs.h + hpcs.c      parseur, index, édition, I/O
├── core/tests/  suite de tests C
├── app/         tableur Win32 (main.c, render.c)
├── lsp/         serveur LSP en C
├── cli/         CLI hpcs
├── extension/   extension Zed (Rust → wasm32-wasip2)
├── docs/        capture + architecture
└── samples/     jeux de données de démonstration
```

Détails de conception : [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Compilation

Prérequis :

- **Zig 0.14.1** (le `build.zig` utilise l'API de cette version ; les versions 0.15+
  peuvent nécessiter des ajustements) ;
- Windows 10/11 pour le tableur natif (le cœur et le LSP sont portables C11) ;
- **Rust** uniquement pour compiler l’extension WASM (Zed sait aussi le faire à
  l'installation ; `wasm32-wasip2` requis).

```powershell
zig build                      # debug
zig build -Doptimize=ReleaseFast
zig build test                 # suite de tests du cœur
```

Binaires produits :

```
zig-out/bin/HPCSV.exe
zig-out/bin/hpcs-lsp.exe
zig-out/bin/hpcs.exe
```

Si Zig n'est pas dans le `PATH`, passez son chemin complet :

```powershell
& "C:\tools\zig\zig.exe" build -Doptimize=ReleaseFast
```

### Extension Zed

L'extension est un crate Rust compilé en WASM :

```powershell
cd extension
cargo build --target wasm32-wasip2 --release
```

Zed compile lui-même l'extension lors de l'installation. Deux pièges fréquents sous
Windows :

1. **`link.exe` introuvable** : Rust utilise par défaut le toolchain MSVC, qui exige
   Visual Studio. Utilisez le toolchain GNU (installé par rustup) :

   ```powershell
   rustup default stable-x86_64-pc-windows-gnu
   rustup target add wasm32-wasip2
   ```

2. **Chemin du dépôt avec accents** (`Dépots`, par exemple) : MinGW peut échouer à
   linker les proc-macros. Redirigez le répertoire de build vers un chemin ASCII :

   ```powershell
   $env:CARGO_TARGET_DIR = "$env:TEMP\hpcs-cargo-target"
   cargo build --target wasm32-wasip2 --release
   ```

   Pour que Zed hérite de cette variable, lancez-le depuis ce terminal.

3. **Chemin du dépôt avec accents** : Zed impose `--target-dir <extension>/target`.
   MinGW `ld` ne sait pas ouvrir un chemin comme `Dépots`, donc le build Rust échoue
   (`ld: cannot find ...`). Préparez une copie ASCII de l'extension et installez-la
   depuis cette copie :

   ```powershell
   .\scripts\prepare-dev-extension.ps1
   ```

   Puis dans Zed : **Install Dev Extension** → `C:\hpcs-extension` (et non
   `HPCSV\extension`). Relancez le script après toute modification de `extension/`.

Après l'installation de Rust, **redémarrez Zed** pour qu'il voie `cargo`/`rustup` dans le
`PATH`.

### Linter C (clangd)

Zed analyse le C avec **clangd**. Sur une machine sans Visual Studio / Windows SDK, clangd
ne trouve ni `windows.h`, ni `stdio.h`, ni les en-têtes du projet : il affiche alors une
avalanche de fausses erreurs. HPCSV compile avec Zig (cible MinGW), donc on donne à clangd
la même cible et les en-têtes de Zig via une base de compilation :

```powershell
# Zig dans le PATH…
.\scripts\gen_compile_commands.ps1

# …ou chemin explicite
.\scripts\gen_compile_commands.ps1 -Zig C:\tools\zig\zig.exe
```

Le script écrit `compile_commands.json` (gitignoré, spécifique à la machine) avec le bon
`--target=x86_64-w64-windows-gnu`, les chemins d'inclusion du projet et les en-têtes
MinGW livrés avec Zig. `.clangd` force la même cible pour les fichiers ouverts hors base.

Après génération, rechargez clangd (`editor: restart language server`) ou Zed. Une tâche
`HPCSV: regenerate clangd database` est fournie dans `.zed/tasks.json`.

Le code compile sans aucun warning avec un jeu strict :

```powershell
zig cc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion `
  -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror `
  -Icore/include -Iapp -c app/src/main.c -o "$env:TEMP\check.o"
```

---

## Utilisation

### 1. Tableur

```powershell
.\zig-out\bin\HPCSV.exe samples\demo.csv
```

Sans argument, une boîte de dialogue s'ouvre. On peut aussi glisser-déposer un fichier
sur la fenêtre.

| Raccourci | Action |
|---|---|
| `↑ ↓ ← →` | déplacer la sélection |
| `Maj + flèches` | étendre la sélection |
| `Ctrl + flèches` | aller au bord des données |
| `Début / Fin` | début / fin de ligne |
| `Ctrl + Début / Fin` | début / fin du document |
| `Entrée` / `F2` | éditer la cellule |
| frappe directe | remplacer la cellule |
| `Entrée` | valider et descendre |
| `Tab` / `Maj + Tab` | valider et aller à droite / gauche |
| `Échap` | annuler l'édition / la sélection |
| `Suppr` | vider la sélection |
| `Ctrl + C` / `Ctrl + V` | copier / coller en TSV |
| `Ctrl + Z` / `Ctrl + Y` | annuler / rétablir |
| `Ctrl + S` | sauvegarder immédiatement |
| `Ctrl + R` | recharger depuis le disque |
| `Ctrl + A` | tout sélectionner |
| `Ctrl + O` | ouvrir un fichier |
| `Ctrl + Entrée` | ajouter une ligne |

Clic-glisser : sélection rectangulaire. Double-clic : édition. Glisser la bordure d'un
en-tête : redimensionner la colonne. Molette : vertical ; `Maj + molette` : horizontal.

### 2. Extension Zed

1. Compilez le projet (`zig build`). L'extension trouve `hpcs-lsp` automatiquement :
   dans `zig-out\bin` du dépôt s'il est ouvert, sinon via `HPCSV_LSP` (chemin complet du
   serveur), via le dossier de `HPCSV_BIN`, ou via le `PATH`.
2. Dans Zed : `zed: extensions` → **Install Dev Extension** → sélectionnez le dossier
   `extension/`.
3. Ouvrez un `.csv` : coloration, diagnostics et hover sont actifs.
4. `Ctrl+.` sur une cellule → **HPCSV: open in spreadsheet**.

Pour lancer le tableur sans passer par l'action de code, une tâche est fournie dans
`.zed/tasks.json` (palette → `task: spawn` → *HPCSV: open current file in spreadsheet*).

> L'extension résout `hpcs-lsp` sans qu'il soit forcément dans le `PATH` : `HPCSV_LSP`
> (chemin du serveur), dossier de `HPCSV_BIN`, `PATH`, puis `zig-out\bin` du dépôt HPCSV
> ouvert. Si Zed affiche « hpcs-lsp was not found », le binaire n'a pas été compilé
> (`zig build`) et aucun des replis n'est disponible.

### 3. CLI

```powershell
hpcs info  samples\demo.csv
hpcs get   samples\demo.csv 2 3          # ligne 2, colonne 3 (1-based)
hpcs set   samples\demo.csv 2 3 "Paris"
hpcs dump  samples\demo.csv 10           # TSV des 10 premières lignes
hpcs append samples\demo.csv 8
hpcs bench samples\demo.csv 10
```

---

## Performances

Mesures locales, build `ReleaseFast`, Zig 0.14.1, fichier de 80,26 Mio
(1 000 001 lignes × 8 colonnes) :

| Opération | Temps |
|---|---|
| Parsing complet | **79 ms** (≈ 1,0 Gio/s) |
| Édition d’une cellule + réécriture atomique | **~190 ms** |
| Édition de la dernière cellule | **~195 ms** |
| Application : parsing + ouverture | ~0,2 s |

Le parsing est mono-passe et zéro-copie ; l’édition ne reparse pas le document : elle
remplace la plage brute de la cellule et décale les offsets suivants.

Reproduire :

```powershell
hpcs bench gros.csv 10
```

---

## Fidélité du fichier

Une édition de cellule ne réécrit **que** la représentation de cette cellule :

- les autres octets (BOM, fins de ligne CRLF/LF, guillemets, espaces, colonnes) sont
  conservés tels quels ;
- la cellule est quotée uniquement si nécessaire (séparateur, guillemet, retour ligne,
  espaces en début/fin) ;
- l’écriture est atomique (fichier temporaire dans le même dossier puis remplacement),
  ce qui évite les fichiers tronqués et les lectures partielles par Zed.

---

## Dépannage

- **« hpcs-lsp was not found »** : le binaire n'a pas été compilé. Lancez `zig build` :
  l'extension le détecte dans `zig-out\bin` du dépôt HPCSV ouvert. Sinon, ajoutez
  `zig-out\bin` au `PATH` (puis relancez Zed), définissez `HPCSV_LSP` avec le chemin
  complet du serveur, ou `HPCSV_BIN` avec le dossier contenant `hpcs-lsp`.
- **Le bouton « open in spreadsheet » ne fait rien** : `hpcs-lsp` cherche `HPCSV.exe`
  à côté de lui, puis `HPCSV_BIN`, puis dans le `PATH`. Définissez `HPCSV_BIN` avec le
  chemin complet si nécessaire.
- **Pas de diagnostics / coloration incohérente** : si l'extension communautaire `csv`
  est aussi installée, Zed a deux fournisseurs pour le langage CSV. Désactivez l'autre
  extension pour éviter l'ambiguïté.
- **Le build de l'extension échoue avec `ld: cannot find ...Dépots...`** : c'est le
  chemin accentué. Lancez `.\scripts\prepare-dev-extension.ps1` et installez l'extension
  depuis `C:\hpcs-extension`.
- **Zed ne compile pas l'extension** : voir la section « Extension Zed » (toolchain GNU
  et chemin ASCII pour `CARGO_TARGET_DIR`).

## Limites et feuille de route

- **Windows d'abord** : le tableur utilise Win32/GDI. Le cœur et le LSP sont portables ;
  les backends macOS/Linux restent à écrire.
- Pas encore d'insertion/suppression de lignes ou de colonnes, ni de tri par clic sur
  l'en-tête (l'ajout de ligne existe via `Ctrl+Entrée`).
- Pas de formules, de graphiques ni de types de données : HPCSV est un éditeur de CSV,
  pas un tableur de calcul.
- Le LSP utilise une synchronisation **full** : sur des fichiers > 20 Mio, la frappe peut
  devenir moins fluide (le parseur reste rapide, mais Zed renvoie tout le texte).
- Zed ne permet toujours pas de vue custom ; si l'API visuelle arrive, le cœur C est prêt
  à être consommé directement par une vue GPUI.

---

## Licence

MIT — voir [LICENSE](LICENSE).
