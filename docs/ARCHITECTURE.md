# Architecture HPCSV

## 1. Contraintes

Zed n'expose pas d'API de rendu aux extensions. HPCSV sépare donc :

1. **le modèle** : un fichier CSV sur disque, édité par Zed ou par le tableur ;
2. **la vue tableur** : une application native C qui lit/écrit ce fichier ;
3. **l'intégration éditeur** : une extension Zed qui apporte le langage, les diagnostics,
   le hover et le lancement du tableur.

La synchronisation est assurée par le système de fichiers : chaque partie surveille
`mtime + taille` et recharge quand l'autre a écrit.

## 2. Cœur C (`core/`)

### Modèle

```c
typedef struct HpCell {
    uint32_t off;      // début du contenu (après le guillemet ouvrant si quoté)
    uint32_t len;      // longueur du contenu, "" non décodés
    uint32_t raw_len;  // longueur brute, guillemets inclus
    uint8_t  quoted;
} HpCell;

typedef struct HpRow {
    uint32_t first;    // index de la première cellule
    uint32_t count;
} HpRow;

typedef struct HpDoc {
    char    *buf;      // fichier complet, NUL-terminé
    size_t   len, cap;
    HpCell  *cells;    // table plate de toutes les cellules
    HpRow   *rows;     // index des lignes dans `cells`
    char     delim;
    ...
} HpDoc;
```

- Aucune allocation par cellule : `HpCell` fait 16 octets, les lignes 8 octets.
- Les offsets sont des `uint32_t` : limite assumée à 4 Gio - 1 par fichier.
- Les cellules non quotées pointent directement dans `buf` (zéro-copie).
- Les guillemets doublés (`""`) ne sont décodés qu'à la demande, dans un scratch
  réutilisable (`hpcs_cell_text`).

### Parsing

Une seule passe :

```
pour chaque enregistrement :
    pour chaque champ :
        si "..."  → scanner jusqu'au guillemet fermant, gérer ""
        sinon     → scanner jusqu'au séparateur / \r / \n
    consommer \r\n ou \n
    enregistrer la ligne
```

Points notables :

- le séparateur est **sniffé** sur les 64 premiers enregistrements (max 256 Kio) :
  score = régularité du nombre de champs, puis largeur ; `,` par défaut ;
- BOM UTF-8 conservé (les offsets commencent après) ;
- un champ quoté non fermé est accepté, compté dans `unterminated`, et son offset
  est mémorisé pour le diagnostic LSP ;
- la ligne finale sans `\n` est valide ;
- les octets entre le guillemet fermant et le séparateur sont tolérés (et conservés
  dans la plage brute) pour ne pas casser les fichiers imparfaits.

### Édition sans reparse

`hpcs_set_cells` prend un lot d'éditions :

1. pour chaque cellule, calcul de la plage brute `[start, end)` et sérialisation de la
   nouvelle valeur (quotée si nécessaire) ;
2. tri des éditions par offset, détection de chevauchement ;
3. reconstruction du buffer en une passe : copie des segments inchangés + insertion des
   nouvelles valeurs ;
4. mise à jour des offsets de **toutes** les cellules en une passe, via un accumulateur
   de delta (les cellules sont ordonnées par offset).

Conséquences :

- pas de reparse complet pour une édition de cellule ;
- coût O(n) en mémoire pour la copie, O(n_cellules) pour le décalage — sur 8 M de cellules,
  l'édition reste sous la barre des 200 ms, écriture disque incluse ;
- les octets hors cellule sont préservés.

`hpcs_append_row` est l'exception structurelle : elle ajoute une ligne et reparse.

### I/O

- Lecture `CreateFileW` + `ReadFile` (partage lecture/écriture/suppression, pour lire
  pendant que Zed écrit), sur Windows ; `fopen` ailleurs.
- Chemins UTF-8 convertis en UTF-16 côté Win32.
- Écriture atomique : `.hpcs-<pid>.tmp` dans le même dossier, `FlushFileBuffers`, puis
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.

## 3. Tableur (`app/`)

- Fenêtre Win32, painting GDI double-bufferisé (`app_paint`).
- **Virtualisation** : seules les colonnes/lignes visibles sont dessinées ; les largeurs
  de colonnes sont des préfixes (`colx[]`) permettant une recherche binaire.
- Grille : en-tête figé (ligne 0 du CSV), gouttière des numéros, zébrures, lignes de
  grille, alignement à droite des nombres.
- Édition : contrôle `EDIT` enfant positionné sur la cellule, subclassé pour
  `Entrée`/`Tab`/`Échap`, commit sur perte de focus.
- Sélection rectangulaire, presse-papiers TSV avec quoting Excel.
- Undo/redo : pile de lots `(row, col, ancien texte, nouveau texte)`, 64 niveaux.
- Sauvegarde différée de 350 ms après une modification.
- Surveillance disque toutes les 350 ms ; rechargement auto si aucune édition locale
  n'est en attente, sinon message et `Ctrl+R` pour forcer.
- DPI : `SetProcessDPIAware`, métriques et polices mises à l'échelle.

## 4. LSP (`lsp/`)

JSON-RPC 2.0 sur stdio, framing `Content-Length`. Mini-parseur JSON (recherche de clés
parcimonieuse, décodage des chaînes/`\uXXXX`) pour éviter toute dépendance.

Capacités annoncées :

- `textDocumentSync: 1` (full) ;
- `hoverProvider` ;
- `codeActionProvider` ;
- `executeCommandProvider.commands = ["hpcs.open"]`.

Diagnostics :

- lignes dont le nombre de champs diffère de l'en-tête (200 max) ;
- champ quoté non fermé ;
- positions calculées en **UTF-16** (unités LSP), avec gestion des séquences UTF-8.

Hover : conversion position → offset, recherche de la cellule par plages brutes, markdown
avec adresse, type, longueur et tableau de la ligne.

`hpcs.open` : l'URI est décodée (`%xx`, `file://`), puis `CreateProcessW` lance `HPCSV.exe`
(trouvé à côté du LSP, via `HPCSV_BIN`, ou dans le `PATH`).

## 5. Extension (`extension/`)

- `extension.toml` déclare deux grammaires (sous-dossiers `csv` et `tsv` du dépôt
  tree-sitter-grammars, via le champ `path`) et un serveur de langage commun.
- `src/lib.rs` : `language_server_command` résout `hpcs-lsp` dans l'ordre `HPCSV_LSP`,
  dossier de `HPCSV_BIN`, `$PATH` (`Worktree::which`), puis `zig-out/bin` du dépôt ouvert
  (détecté via `build.zig`) ; Zed compile le crate en `wasm32-wasip2`.
- `languages/*/highlights.scm` : chaînes, nombres, flottants, booléens, séparateurs.

## 6. Synchronisation et conflits

| Événement | Réaction |
|---|---|
| édition dans HPCSV | buffer modifié → sauvegarde atomique après 350 ms |
| sauvegarde HPCSV | Zed voit le changement et recharge l'onglet |
| édition dans Zed | HPCSV voit `mtime` changer → recharge (si pas d'édition locale) |
| édition des deux côtés | HPCSV conserve ses éditions, affiche un avertissement, `Ctrl+R` force le rechargement |

## 7. Performances

- Parsing mono-passe, boucles byte-à-byte sans allocation.
- Tableaux plats, croissance géométrique.
- `hpcs_cell_text` ne décode que les guillemets doublés.
- Rendu limité au viewport.
- Mesure : 80,26 Mio / 1 000 001 lignes → ~79 ms (≈ 1,0 Gio/s), édition + sauvegarde ~190 ms.

## 8. Pistes

- offsets 64 bits (fichiers > 4 Gio) ;
- index incrémental pour l'insertion/suppression de lignes et colonnes ;
- tri/filtre par colonne, recherche ;
- backend macOS (AppKit) et Linux (GTK/X11) pour le tableur ;
- LSP incrémental et diagnostics différés sur très gros fichiers ;
- si Zed ajoute une API visuelle, brancher directement le cœur C.
