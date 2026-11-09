# a9tool | A-Train 9 Archive Toolkit (macOS / Linux / Windows x64)

Ein funktionsfähiger, plattformunabhängiger **C11-Nachbau** der relevanten Funktionen aus `a9codec.exe` vom **17.10.2013**. Die Binärdatei wurde über COFF-Funktionssymbole und x86-Disassembly analysiert. **Dies ist rekonstruierter C-Quellcode, nicht der ursprüngliche Autor-Quellcode.** Die folgenden Routinen sind funktional reproduziert:

| 2013er EXE-Symbol | C-Portierung |
| --- | --- |
| `_a9_extract` | `decode_archive()` |
| `_a9_create` | `encode_archive()` |
| `_a9_bufferextract` | `a9_lzss_decode()` |
| `_compress_buffer`, `_init_tree`, `_insert_node`, `_delete_node` | `a9_lzss_encode()` und interne Baumfunktionen |
| `_MD5_Init`, `_MD5_Update`, `_MD5_Final` | `a9_md5()` (RFC-1321-kompatibel) |
| XOR/Subtraktion mit Konstante in `.data` | `crypt()`, `A9_TABLE` |

Das Tool besteht aus nativem C11 und benötigt keine externen Codec-Pakete. Es ist nicht auf das .NET-Framework angewiesen. Unter Windows benutzt es Win32-Datei-APIs, unter macOS/Linux POSIX.

## Programm bauen

**macOS:**

```bash
cd /Users/Shared/a9tool
make
./a9tool --help
```

**Linux (GCC oder Clang):**

```bash
make
./a9tool --help
```

Alternativ mit CMake auf **beiden** Betriebssystemen:

```bash
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

Der macOS-Build verwendet **immer arm64 + x86_64 Universal** und als Mindestversion **macOS 12.0** (in `Makefile` und `CMakeLists.txt` festgesetzt). Die ausführbare Mac-Datei liegt unter `dist/a9tool`.

## Windows x64: mit Visual Studio / .NET-IDE bauen

`a9tool` ist **kein .NET-/C#-Programm**, sondern ein **natives C11-Konsolenprogramm**. Mit Visual Studio 2022 oder neuer funktioniert der Build trotzdem direkt. Benötigt werden **Desktopentwicklung mit C++**, der **MSVC-C/C++-Compiler**, das **Windows SDK** und **CMake**. Nicht nur die .NET-Workloads installieren.

1. Quellcode-ZIP auf Windows entpacken (z.B. nach `C:\a9tool`).
2. **„x64 Native Tools Command Prompt for VS 2022“** öffnen.
3. Im Projektordner diese Befehle ausführen:

```bat
cd /d C:\a9tool
build-windows-x64.cmd
```

Das Skript startet intern:

```bat
cmake -S . -B build-win64 -G "Visual Studio 17 2022" -A x64
cmake --build build-win64 --config Release
ctest --test-dir build-win64 -C Release --output-on-failure
```

Das Ergebnis liegt unter `C:\a9tool\build-win64\Release\a9tool.exe`. Die **x64-Architektur** wird durch `-A x64` erzwungen; `/MT` bindet die MSVC-Runtime statisch ein. Zum Öffnen in der IDE danach `build-win64\a9tool.sln` laden und `Release / x64` wählen.

```bat
build-win64\Release\a9tool.exe --help
build-win64\Release\a9tool.exe check C:\csvp\data
build-win64\Release\a9tool.exe compare C:\old\csvp.bin C:\new\csvp.bin --only-added
```

Die Windows-Codepfade sind implementiert, aber hier nur unter macOS kompiliert und getestet. **Ein tatsächlicher MSVC-Build auf Windows wurde noch nicht ausgeführt.** Das Windows-CMake-Projekt enthält den plattformunabhängigen MD5-Selbsttest; die Unix-Shelltests werden unter Windows nicht ausgeführt.

## Originale csvp.bin dekodieren

```bash
cd /Users/Shared/a9tool
./a9tool -x '/Users/luigi/Desktop/at9/csvp.bin' -o out/csvp
```

Die Ausgabe enthält:

```text
out/csvp/
├── files.txt               # 35 Dateinamen in der Archiv-Reihenfolge
└── data/
    ├── automobile.csv
    ├── gametext.csv
    ├── train_pattern.csv
    └── ...                  # alle 35 Dateien
```

Mit `--intermediate` gibt es zudem `xor.key`, `decrypted.data` und `decrypted.extracted`.

## Zwei csvp.bin vergleichen: neue CSV-Zeilen und geänderte Einträge

Der Vergleich dekodiert **beide `csvp.bin` direkt im RAM**. Zum Vergleichen müssen sie nicht entpackt werden:

```bash
./a9tool compare /Pfad/ALT/csvp.bin /Pfad/NEU/csvp.bin
./a9tool compare ALT.bin NEU.bin --only-added
./a9tool compare ALT.bin NEU.bin -o vergleich.txt
```

Einträge werden nach identischen Datensätzen und erkennbaren ID-Spalten abgeglichen. Die Angabe `DATEI:ZEILE` bezeichnet die **physische Zeilennummer** in der alten oder neuen CSV, auch wenn ein CSV-Feld mehrere Zeilen Text enthält.

```text
CSV: weather.csv
+ weather.csv:4: NEU 99 | 1

CSV: train_comment.csv
+ train_comment.csv:2: NEU FORM_ADDED_DEMO | New data\r\ncontinued
```

`+` bedeutet neu in der zweiten Datei, `~` bedeutet inhaltlich geändert, `-` bedeutet in der zweiten Datei entfernt. Mit `--only-added` erscheinen nur die neuen Datensätze. Beim gleichzeitigen Einfügen/Löschen ohne stabile ID-Spalte kann die automatische Zuordnung mehrere mögliche Ergebnisse haben. Für die echten `csvp.bin`-Daten und für mehrzeilige CSV-Einträge wurde die Funktion getestet.

## CSV-Validator: manueller Check mit Datei + Zeilennummer

Der Validator ist direkt eingebaut. Er akzeptiert sowohl einen **entpackten Archivordner** (mit `files.txt` und `data/`) als auch einen **Ordner, in dem die CSV-Dateien direkt liegen**:

```bash
cd /Users/Shared/a9tool
./a9tool check out/csvp
./a9tool check out/csvp/data
# Allgemein:
./a9tool check FOLDER
```

Er prüft:

- **Zeichenkodierung:** UTF-16LE-BOM, ungerade Bytezahl, ungültige Surrogatpaare, NUL und unerlaubte Steuerzeichen
- **CSV-Syntax:** korrekt geschlossene Anführungszeichen, escaped Quotes, gültige Feldtrenner, auch **mehrzeilige zitierte Texte**
- **Spaltenzahl/Abschnitte:** Vergleich mit dem aus der echten `csvp.bin` gewonnenen Schema, inklusive bewusst variabler Spaltenzahl pro Datei und Abschnitt. Bekannte Abschnitts-IDs werden abgeglichen und fehlende, unbekannte oder doppelte IDs als **Warnung** gemeldet
- **Zeilenlänge:** Warnung bei ungewöhnlich langen physischen Zeilen im Vergleich zum Original, Fehler ab 32.768 UTF-16-Zeichen je physischer Zeile
- **Archivliste:** fehlende Dateien, doppelte Einträge in `files.txt`, nicht gelistete CSV-Dateien

Meldungen zeigen **Dateiname und physische Zeilennummer** (bei Mehrzeilen-Feldern die Position des betroffenen Problems). Beispiel:

```text
out/csvp/data/weather.csv:4: FEHLER: Falsche Spaltenzahl in CSV-Abschnitt: 14 gefunden, 13 erwartet (Datensatz 4)
out/csvp/data/weather.csv:12: WARNUNG: Physische Zeile ungewöhnlich lang (> 512 UTF-16-Zeichen; Referenzmaximum 79)
CSV-Check abgeschlossen: 35 Dateien, 24379 Datensätze, 1 Fehler, 1 Warnungen
```

**Exit-Status:** `0` bei keinen Fehlern (Warnungen sind möglich), `1` bei Fehlern. Bei zu vielen Folgefehlern pro Datei werden nach 30 Meldungen weitere Meldungen unterdrückt, aber weitergezählt.

**Repacking prüft jetzt automatisch**, wenn `files.txt` CSV-Dateien nennt. Ein fehlerhaftes CSV **verhindert die Erstellung** einer neuen `.bin`. Wer ein bewusst abweichendes Format hat, kann den Check explizit mit `--no-check` umgehen:

```bash
./a9tool -c out/csvp -o out/csvp_modified.bin
./a9tool -c out/csvp -o out/csvp_unchecked.bin --no-check
```

**Referenzschema:** Die Datei `src/csvp_schema.c` wurde aus deiner geprüften Original-`csvp.bin` erzeugt und enthält lediglich Spaltenstruktur und Hashes der Abschnitts-IDs, keine CSV-Texte. Mit `tools/generate_csv_schema.py <data-ordner> src/csvp_schema.c` lässt sich das Referenzschema für ein anderes *bekannt gültiges* Archiv neu erzeugen (zum Generieren wird Python 3 benötigt). Für andere nicht erfasste CSV-Dateinamen prüft der Validator die CSV-Syntax und Kodierung, aber er kann die Spaltenstruktur nicht sicher beurteilen.

Die bestehenden Originaldateien ergeben **0 Fehler, 0 Warnungen (35 CSVs)**. Mehrzeilige Texte und variable Spaltenanzahl in `msg_sec.csv`, `train_comment.csv` und weiteren Dateien sind dabei explizit berücksichtigt.

## Geänderte CSV-Dateien wieder enkodieren

```bash
# CSVs in out/csvp/data/ mit deinem Editor ändern; ihre UTF-16LE-Dateikodierung erhalten!
./a9tool check out/csvp
./a9tool -c out/csvp -o out/csvp_modified.bin
./a9tool -i out/csvp_modified.bin
```

Die neue Datei ist wieder ein gültiges A9-Spielarchiv. Du kannst die Daten wieder entpacken oder nach einem Backup der Game-Daten verwenden. **Das Programm überschreibt nichts ohne `--force`.**

Falls du die extrahierten Dateien nicht veränderst:

```bash
./a9tool -c out/csvp -o out/csvp_rebuilt.bin
cmp '/Users/luigi/Desktop/at9/csvp.bin' out/csvp_rebuilt.bin
shasum -a 256 '/Users/luigi/Desktop/at9/csvp.bin' out/csvp_rebuilt.bin
```

Beide Archive sind **100 % byteidentisch**, nicht nur inhaltlich gleich. Ergebnis für dein Original:

```text
Dateien:          35
Originalgröße:    893280 Bytes
LZSS-Daten:       893264 Bytes
Entpackte Daten: 3979050 Bytes
Original SHA256:  270d5dc1280aa2c8964cbcee938753d4c0047efbfbabe216a302b5f0beb0ed59
Neu SHA256:       270d5dc1280aa2c8964cbcee938753d4c0047efbfbabe216a302b5f0beb0ed59
```

## Weitere Optionen

| Kommando | Wirkung |
| --- | --- |
| `-x archive.bin` | Entpacken |
| `-c directory` | Aus `files.txt` und `data/` ein Archiv erstellen |
| `-i archive.bin` | Datei überprüfen, MD5 verifizieren, Inhalt anzeigen |
| `check FOLDER` | Alle CSVs des Ordners manuell prüfen, Ausgabe mit `Datei:Zeile` |
| `compare ALT.bin NEU.bin` | Hinzugekommene/geänderte/entfernte CSV-Datensätze vergleichen |
| `--only-added` | Beim Vergleich nur neue Datensätze anzeigen |
| `-t 0` | Normales `cs*`/`csvp.bin`-Layout, **Standard** |
| `-t 1` | Zusätzliche 8-Byte-`*.a9codec_data`-Metadaten pro Datei (z. B. `mt*`) |
| `-o path` | Ausgabepfad |
| `--intermediate` | Codec-Zwischendaten sichern |
| `--force` | Vorhandene Ausgabedateien ausdrücklich überschreiben |
| `--no-check` | Nur beim Packen: CSV-Validierung ausdrücklich umgehen |
| `-v`, `-vv` | Mehr Ausgabe |

**Hinweis zum Archivtyp 1:** Der Test mit selbst erzeugten Metadaten besteht. Hier wurde bisher **kein reales `mt*.bin`** des Spiels gegengeprüft. `csvp.bin` dagegen wurde mit einem echten Original erfolgreich getestet.

## Dateiformat (rekonstruiert)

```text
A9-Archiv:
  0x00  16 Byte       MD5(LZSS-Komprimat), auch XOR-Schlüssel
  0x10  Rest          (LZSS-Komprimat XOR MD5[i%16]) - Konstante[i%256] mod 256

LZSS:   4 KiB Ring-Puffer; 18 Byte Lookahead; 8 Steuerbits je Gruppe.
         Literal bei gesetztem Bit, sonst 12-bit Offset + 4-bit Länge.

Dekomprimiertes Archiv:
  0x00  uint32_le     Dateianzahl
  N-mal:
          512 Byte     Dateiname in UTF-16LE, mit Null-Zeichen aufgefüllt
          [8 Byte]     nur bei -t 1: unveränderte Metadaten
          uint32_le    Dateigröße
          ...          Dateiinhalt
```

Die Datei-Inhalte (z.B. CSV mit UTF-16LE-BOM) werden **bitgenau** übernommen, nicht textuell konvertiert. Dateinamen werden über UTF-8 und UTF-16LE umgerechnet. Dateien aus nicht vertrauenswürdigen Archiven werden auf Pfad-Traversal geprüft.

## Tests

```bash
make check
./tests/roundtrip.sh /Users/luigi/Desktop/at9/csvp.bin
./tests/csv_validation.sh
./tests/compare.sh
```

Enthaltene Prüfungen: RFC-1321-MD5-Testvektoren, Archivtypen 0 und 1 (inklusive Original-Metadaten), UTF-8-Dateinamen, MD5-Beschädigungen, Schutz vor Überschreiben, byteidentischer CSVP-Roundtrip und CSV-Fehlerfälle (Datei/Zeile, falsche Spaltenzahl, UTF-16, Quote-Syntax, Multiline, Zeilenlängen, automatisches Blockieren beim Packen). Das macOS-Binary wurde direkt getestet. Die Linux- und Windows-Build-Dateien sind vorbereitet; **ein Windows-/Linux-Lauf wurde hier auf dem macOS-System nicht durchgeführt**. Nur die synthetischen CSV-Testfixtures benötigen Python 3, das eigentliche Codec-Programm nicht.

## Reverse-Engineering-Umfang und Lizenz

Die Original-EXE wurde nur als Input analysiert und wird nicht mit dem C-Quellcode verteilt. Die proprietäre Spiel-CSV-Datei liegt nicht im Source-Zip. Das Projekt ist eine eigene strukturelle Nachimplementierung des beobachteten Dateiformats und garantiert nur für die überprüften Formate Kompatibilität. Die rohe, in der EXE ebenfalls vorhandene zlib-Hilfsroutine war für `csvp.bin` nicht erforderlich und ist nicht als separater CLI-Modus eingebaut.
