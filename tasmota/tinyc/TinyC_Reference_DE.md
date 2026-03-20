# TinyC Sprachreferenz

**TinyC** ist eine Untermenge von C, die zu Bytecode fuer eine stackbasierte virtuelle Maschine kompiliert wird.
Es laeuft sowohl im Browser (JavaScript-VM) als auch auf ESP32/ESP8266 (als Tasmota-Treiber XDRV_124).

---

## Inhaltsverzeichnis

1. [Datentypen](#datentypen)
2. [Literale](#literale)
3. [Variablen & Gueltigkeitsbereich](#variablen--gueltigkeitsbereich)
4. [Operatoren](#operatoren)
5. [Kontrollfluss](#kontrollfluss)
6. [Funktionen](#funktionen)
7. [Callback-Funktionen](#callback-funktionen)
8. [Tasmota-Systemvariablen](#tasmota-systemvariablen)
9. [Arrays](#arrays)
10. [Zeichenketten](#zeichenketten)
11. [Praeprozessor](#praeprozessor)
12. [Kommentare](#kommentare)
13. [Typumwandlung](#typumwandlung)
14. [Eingebaute Funktionen](#eingebaute-funktionen)
15. [Multi-VM Slots (ESP32)](#multi-vm-slots-esp32)
16. [VM-Grenzen](#vm-grenzen)
17. [Geraetedateiverwaltung (IDE)](#geraetedateiverwaltung-ide)
18. [Tastenkuerzel (IDE)](#tastenkuerzel-ide)
19. [Beispiele](#beispiele)

---

## Datentypen

| Typ     | Groesse | Beschreibung                              |
|---------|---------|-------------------------------------------|
| `int`   | 32-Bit  | Vorzeichenbehaftete Ganzzahl              |
| `float` | 32-Bit  | IEEE 754 Gleitkommazahl                   |
| `char`  | 8-Bit   | Vorzeichenloses Zeichen (maskiert auf 0xFF) |
| `bool`  | 32-Bit  | Boolescher Wert (0 = false, ungleich 0 = true) |
| `void`  | —       | Kein Wert (Rueckgabetyp fuer Funktionen)  |

### Typ-Aliase

| Alias          | Entspricht |
|----------------|------------|
| `int32_t`      | `int`      |
| `uint32_t`     | `int`      |
| `unsigned int` | `int`      |
| `uint8_t`      | `char`     |

---

## Literale

### Ganzzahl-Literale
```c
42          // dezimal
0xFF        // hexadezimal (Praefix 0x oder 0X)
0b1010      // binaer (Praefix 0b oder 0B)
```

### Gleitkomma-Literale
```c
3.14        // Dezimalpunkt
2.5f        // mit Float-Suffix
0.001       // fuehrende Null
```

### Zeichen-Literale
```c
'A'         // einzelnes Zeichen
'\n'        // Escape-Sequenz
'\0'        // Null-Terminator
```

**Unterstuetzte Escape-Sequenzen:** `\n` `\t` `\r` `\\` `\'` `\"` `\0`

### Zeichenketten-Literale
```c
"Hello"             // einfache Zeichenkette
"Line 1\nLine 2"    // mit Escape-Sequenzen
```
Zeichenketten-Literale werden fuer die Initialisierung von `char`-Arrays und als Argumente fuer Zeichenketten-Funktionen verwendet.

### Boolesche Literale
```c
true        // ergibt 1
false       // ergibt 0
```

---

## Variablen & Gueltigkeitsbereich

### Globale Variablen
Ausserhalb jeder Funktion deklariert. Von allen Funktionen aus zugaenglich.
```c
int counter = 0;
float pi = 3.14;
char buffer[64];
```

### Persistente Variablen
Globale Variablen mit dem Schluesselwort `persist` werden automatisch im Flash gespeichert und beim Programmstart wiederhergestellt. Dies entspricht den `p:` Variablen im Tasmota Scripter.

```c
persist float totalEnergy = 0.0;   // wird ueber Neustarts gespeichert
persist int bootCount;              // Skalar — 4 Bytes in Datei
persist char deviceName[32];        // Array — 32 Slots in Datei
```

- Nur globale Variablen koennen `persist` sein (keine lokalen Variablen oder Funktionsparameter)
- Persist-Variablen werden **automatisch geladen** aus einer `.pvs`-Datei (abgeleitet vom `.tcb`-Dateinamen, z.B. `/weather.pvs` fuer `/weather.tcb`) beim Programmstart
- Persist-Variablen werden **automatisch gespeichert** beim Stoppen des Programms (`TinyCStop`)
- `saveVars()` aufrufen zum manuellen Speichern (z.B. nach Mitternachts-Zaehleraktualisierung)
- Maximal 32 Persist-Eintraege pro Programm
- Binaerformat — kompakt und schnell (rohe int32 Werte, Floats als bit-cast int32)

```c
persist float dval = 0.0;
persist float mval = 0.0;

void EverySecond() {
    if (tasm_hour == 0 && last_hr != 0) {
        dval = smlGet(2);  // Tageszaehler aktualisieren
        saveVars();         // sofort speichern
    }
}
```

### Watch-Variablen (Aenderungserkennung)
Globale Variablen mit dem Schluesselwort `watch` verfolgen automatisch Aenderungen. Bei jedem Schreibzugriff wird der alte Wert als Schattenwert gespeichert — unverzichtbar fuer IOT-Monitoring.

```c
watch float power;
watch int relay;
```

- Nur skalare Globals koennen `watch` sein (int, float — keine Arrays oder lokale Variablen)
- Jeder Schreibzugriff speichert automatisch den vorherigen Wert und setzt ein Written-Flag
- Benoetigt 2 zusaetzliche Global-Slots pro Watch-Variable (Schatten + Written-Flag)

**Intrinsische Funktionen:**

| Funktion | Rueckgabe | Beschreibung |
|----------|-----------|-------------|
| `changed(var)` | `int` | 1 wenn aktueller Wert vom Schattenwert abweicht |
| `delta(var)` | `int/float` | aktuell - Schatten (vorzeichenbehaftete Differenz) |
| `written(var)` | `int` | 1 wenn Variable seit letztem `snapshot()` zugewiesen wurde |
| `snapshot(var)` | `void` | Schatten = aktuell, Written-Flag loeschen |

```c
watch float power;

void EverySecond() {
    power = sensorGet("ENERGY#Power");
    if (changed(power)) {
        float diff = delta(power);
        // auf Leistungsaenderung reagieren
        snapshot(power);  // Aenderung bestaetigen
    }
}
```

### Lokale Variablen
Innerhalb von Funktionen oder Bloecken deklariert. Blockbasierter Gueltigkeitsbereich (neuer Bereich pro `{ }`).
```c
void myFunc() {
    int x = 10;        // lokal in myFunc
    if (x > 5) {
        int y = 20;    // lokal in diesem Block
    }
    // y ist hier nicht zugaenglich
}
```

### Funktionsparameter
Skalare werden als Wert uebergeben, Arrays als Referenz.
```c
void process(int value, int data[]) {
    // value ist eine Kopie, data ist eine Referenz
}
```

---

## Operatoren

### Arithmetisch
| Op  | Beschreibung   | Typen              |
|-----|----------------|--------------------|
| `+` | Addition       | int, float, char[] |
| `-` | Subtraktion    | int, float         |
| `*` | Multiplikation | int, float         |
| `/` | Division       | int, float         |
| `%` | Modulo         | nur int            |
| `-` | Unaere Negation | int, float        |

**Hinweis:** Fuer `char[]`-Variablen fuehrt `+` eine Zeichenkettenverkettung durch (siehe [Zeichenketten](#zeichenketten)).

### Vergleich
| Op   | Beschreibung         |
|------|----------------------|
| `==` | Gleich               |
| `!=` | Ungleich             |
| `<`  | Kleiner als          |
| `>`  | Groesser als         |
| `<=` | Kleiner oder gleich  |
| `>=` | Groesser oder gleich |

### Logisch
| Op     | Beschreibung                       |
|--------|------------------------------------|
| `&&`   | Logisches UND (Kurzschluss)        |
| `\|\|` | Logisches ODER (Kurzschluss)       |
| `!`    | Logisches NICHT                    |

### Bitweise
| Op  | Beschreibung    |
|-----|-----------------|
| `&` | UND             |
| `\|`| ODER            |
| `^` | XOR             |
| `~` | NICHT           |
| `<<`| Linksverschiebung  |
| `>>`| Rechtsverschiebung |

### Zuweisung
| Op  | Beschreibung                                           |
|-----|--------------------------------------------------------|
| `=` | Zuweisen (fuer `char[]`: Zeichenketten-Kopie)          |
| `+=`| Addieren und zuweisen (fuer `char[]`: Zeichenkette anfuegen) |
| `-=`| Subtrahieren und zuweisen                              |
| `*=`| Multiplizieren und zuweisen                            |
| `/=`| Dividieren und zuweisen                                |

### Inkrement / Dekrement
```c
++x     // Prae-Inkrement
--x     // Prae-Dekrement
x++     // Post-Inkrement
x--     // Post-Dekrement
```

### Operatorvorrang (hoechste bis niedrigste Prioritaet)

1. Postfix: `x++` `x--` `a[i]` `f()` `(type)`
2. Unaer: `++x` `--x` `-x` `!x` `~x`
3. Multiplikativ: `*` `/` `%`
4. Additiv: `+` `-`
5. Verschiebung: `<<` `>>`
6. Relational: `<` `>` `<=` `>=`
7. Gleichheit: `==` `!=`
8. Bitweises UND: `&`
9. Bitweises XOR: `^`
10. Bitweises ODER: `|`
11. Logisches UND: `&&`
12. Logisches ODER: `||`
13. Zuweisung: `=` `+=` `-=` `*=` `/=`

---

## Kontrollfluss

### if / else
```c
if (condition) {
    // ...
}

if (condition) {
    // ...
} else {
    // ...
}

if (a > 0) {
    // ...
} else if (a == 0) {
    // ...
} else {
    // ...
}
```

### while-Schleife
```c
while (condition) {
    // ...
    if (done) break;
    if (skip) continue;
}
```

### for-Schleife
```c
for (int i = 0; i < 10; i++) {
    // ...
}

// alle Teile optional:
for (;;) {
    // Endlosschleife
    break;
}
```

### switch / case
```c
switch (value) {
    case 1:
        // ... Durchfall!
    case 2:
        // ...
        break;
    default:
        // ...
        break;
}
```
**Hinweis:** Faelle fallen durch, sofern nicht `break` verwendet wird (wie in Standard-C).

### break / continue
- `break;` — verlasse die innerste Schleife oder switch-Anweisung
- `continue;` — springe zur naechsten Iteration der innersten Schleife

---

## Funktionen

### Deklaration
```c
int add(int a, int b) {
    return a + b;
}

void doSomething() {
    // kein Rueckgabewert noetig
}
```

### Einstiegspunkt
Jedes Programm muss eine `main()`-Funktion haben:
```c
int main() {
    // Programm startet hier
    return 0;
}
```

### Rekursion
Vollstaendig unterstuetzt:
```c
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}
```

### Array-Parameter
Arrays werden als Referenz uebergeben:
```c
void fill(int arr[], int size, int value) {
    for (int i = 0; i < size; i++) {
        arr[i] = value;
    }
}
```

---

## Callback-Funktionen

TinyC unterstuetzt **Callback-Funktionen**, die Tasmota automatisch bei bestimmten Ereignissen aufruft.
Definieren Sie einfach Funktionen mit diesen bekannten Namen — keine Registrierung erforderlich.

### Verfuegbare Callbacks

| Funktion | Tasmota-Hook | Wann aufgerufen | Anwendungsfall |
|----------|-------------|-----------------|----------------|
| `EveryLoop()` | FUNC_LOOP | Jede Hauptschleifen-Iteration (~1–5 ms) | Ultraschnelles Polling, Bit-Banging, zeitkritische E/A |
| `Every50ms()` | FUNC_EVERY_50_MSECOND | Alle 50 ms (20x/Sek.) | Schnelles Polling, Funkempfang, Sensorabtastung |
| `EverySecond()` | FUNC_EVERY_SECOND | Jede Sekunde | Periodische Aufgaben, Zaehler, langsames Polling |
| `JsonCall()` | FUNC_JSON_APPEND | Telemetriezyklus (~300s) | JSON zu MQTT-Telemetrie hinzufuegen |
| `WebPage()` | FUNC_WEB_ADD_MAIN_BUTTON | Seitenladen (einmalig) | Diagramme, benutzerdefiniertes HTML, Skripte |
| `WebCall()` | FUNC_WEB_SENSOR | Webseitenaktualisierung (~1s) | Sensorzeilen zur Tasmota-Weboberflaeche hinzufuegen |
| `WebUI()` | AJAX /tc_ui Aktualisierung | Alle 2s + bei Widget-Aenderung | Interaktives Widget-Dashboard (Schaltflaechen, Regler usw.) |
| `UdpCall()` | UDP-Paket empfangen | Bei jeder Multicast-Variable | Eingehende UDP-Variablen verarbeiten |
| `WebOn()` | Benutzerdefinierter HTTP-Endpunkt | Bei Anfrage an `webOn()`-URL | REST-APIs, JSON-Endpunkte, Webhooks |
| `TaskLoop()` | FreeRTOS-Task (ESP32) | Kontinuierliche Schleife im eigenen Task | Hintergrundverarbeitung, unabhaengig vom Haupt-Thread |
| `CleanUp()` | FUNC_SAVE_BEFORE_RESTART | Vor Geraete-Neustart | Dateien schliessen, Daten sichern, Ressourcen freigeben |
| `TouchButton(btn, val)` | Touch-Ereignis | Bei GFX-Button/Slider-Beruehrung | Touch-Button-Druecke und Slider-Aenderungen behandeln |
| `HomeKitWrite(dev, var, val)` | HomeKit-Schreibzugriff | Wenn Apple Home einen Wert aendert | Licht, Schalter, Steckdose von Apple Home steuern |
| `Command(char cmd[])` | Benutzerdefinierter Konsolenbefehl | Wenn Benutzer registriertes Praefix in Konsole eingibt | Benutzerdefinierte Tasmota-Befehle verarbeiten (z.B. MP3Play, MP3Stop) |
| `Event(char cmd[])` | Tasmota-Event-Regel-Trigger | Bei `Event`-Befehl aus Regeln oder Konsole | Auf Tasmota-Regel-Events reagieren |
| `OnExit()` | Skript-Stopp | Wenn VM gestoppt oder Skript ersetzt wird | Serielle Ports schliessen, Ressourcen freigeben |
| `OnMqttConnect()` | FUNC_MQTT_INIT | MQTT-Broker verbunden | Topics abonnieren, Status publizieren |
| `OnMqttDisconnect()` | mqtt_disconnected Flag | MQTT-Broker getrennt | Offline-Status setzen, Publizierung stoppen |
| `OnInit()` | Erstes FUNC_NETWORK_UP | Einmal nach erster WiFi-Verbindung | Einmalige Init: Dienste starten, MQTT abonnieren |
| `OnWifiConnect()` | FUNC_NETWORK_UP | WiFi/Netzwerk verbunden (jedes Mal) | Reconnect-Behandlung |
| `OnWifiDisconnect()` | FUNC_NETWORK_DOWN | WiFi/Netzwerk getrennt | Netzwerkabhaengige Aufgaben pausieren |
| `OnTimeSet()` | FUNC_TIME_SYNCED | NTP-Zeit synchronisiert | Zeitbasierte Aktionen planen |

### Ausfuehrungsmodell

1. **`main()`** laeuft zuerst in einem FreeRTOS-Task (ESP32) — `delay()` funktioniert als echte blockierende Verzoegerung
2. Nach dem Anhalten von main bleiben **Globale und Heap erhalten** — sie werden NICHT freigegeben
3. Tasmota ruft periodisch Ihre Callbacks auf, die Globale lesen/aendern koennen
4. Callbacks laufen synchron mit einer Instruktionsbegrenzung — kein `delay()` erlaubt
5. Wenn `TaskLoop()` definiert ist, laeuft es im selben FreeRTOS-Task nach dem Anhalten von main() — `delay()` funktioniert, laeuft unabhaengig von Tasmota's Haupt-Thread

### Tasmota-Ausgabefunktionen

Verwenden Sie diese Funktionen in Callbacks, um Daten an Tasmota zu senden:

| Funktion | Beschreibung | Verwenden in |
|----------|-------------|--------------|
| `responseAppend(buf)` | Char-Array an JSON-Telemetrie anfuegen (-> `ResponseAppend_P`) | `JsonCall()` |
| `responseAppend("literal")` | Zeichenketten-Literal an JSON-Telemetrie anfuegen | `JsonCall()` |
| `webSend(buf)` | Char-Array an Webseite senden (-> `WSContentSend`) | `WebPage()` / `WebCall()` / `WebOn()` |
| `webSend("literal")` | Zeichenketten-Literal an Webseite senden | `WebPage()` / `WebCall()` / `WebOn()` |
| `webFlush()` | Web-Inhaltspuffer zum Client leeren (-> `WSContentFlush`) | `WebPage()` / `WebCall()` / `WebOn()` |
| `webSendFile("filename")` | Dateiinhalt vom Dateisystem an Webseite senden | `WebPage()` / `WebCall()` / `WebUI()` / `WebOn()` |
| `addCommand("prefix")` | Benutzerdefiniertes Konsolen-Befehlspraefix registrieren (z.B. `"MP3"` -> MP3Play, MP3Stop) | `main()` |
| `responseCmnd(buf)` | Char-Array als Konsolenbefehls-Antwort senden | `Command()` |
| `responseCmnd("literal")` | Zeichenketten-Literal als Konsolenbefehls-Antwort senden | `Command()` |

### Webseitenformat

Verwenden Sie Tasmota's `{s}` `{m}` `{e}` Makros in `webSend()`, um Tabellenzeilen zu erstellen:
- `{s}` — Zeile beginnen (Beschriftungsspalte)
- `{m}` — Mitte (Wertspalte)
- `{e}` — Zeile beenden

Beispiel: `"{s}Temperature{m}25.3 °C{e}"` wird als beschriftete Zeile auf der Webseite dargestellt.

### JSON-Telemetrieformat

Verwenden Sie `responseAppend()`, um JSON-Fragmente hinzuzufuegen. Beginnen Sie mit einem Komma:
- `",\"Sensor\":{\"Temp\":25}"` wird an das Telemetrie-JSON angefuegt

### Beispiel

```c
int counter = 0;

void EverySecond() {
    counter++;
}

void JsonCall() {
    // Fuegt an Tasmota MQTT-Telemetrie-JSON an
    char buf[64];
    sprintfInt(buf, ",\"TinyC\":{\"Count\":%d}", counter);
    responseAppend(buf);
}

void WebCall() {
    // Fuegt eine Zeile zur Tasmota-Webseite hinzu
    char buf[64];
    sprintfInt(buf, "{s}TinyC Counter{m}%d{e}", counter);
    webSend(buf);
}

int main() {
    counter = 0;
    return 0;
}
```

**Ergebnis:** Nach dem Hochladen und Ausfuehren zeigt die Tasmota-Webseite eine "TinyC Counter"-Zeile, die jede Sekunde hochzaehlt, und die MQTT-Telemetrie enthaelt `,"TinyC":{"Count":N}`.

### Benutzerdefinierte Konsolenbefehle

Skripte koennen benutzerdefinierte Tasmota-Konsolenbefehle mit `addCommand("prefix")` registrieren. Wenn ein Benutzer z.B. `MP3Play Sound.mp3` in der Konsole eingibt, erkennt Tasmota das Praefix `"MP3"`, extrahiert den Unterbefehl `"PLAY SOUND.MP3"` und ruft `Command("PLAY SOUND.MP3")` im Skript auf.

**Hinweis:** Tasmota konvertiert das Befehls-Topic in Grossbuchstaben, daher kommen Unterbefehle als `"PLAY"`, `"STOP"` usw. an. Daten nach einem Leerzeichen (Dateinamen, Zahlen) behalten ihre urspruengliche Gross-/Kleinschreibung.

```c
int volume = 15;

void Command(char cmd[]) {
    char buf[64];
    if (strFind(cmd, "PLAY") == 0) {
        // Play verarbeiten
        responseCmnd("Playing");
    } else if (strFind(cmd, "STOP") == 0) {
        responseCmnd("Stopped");
    } else if (strFind(cmd, "VOL") == 0) {
        char arg[16];
        strSub(arg, cmd, 4, 0);  // alles nach "VOL " extrahieren
        volume = atoi(arg);
        sprintfInt(buf, "Volume: %d", volume);
        responseCmnd(buf);
    } else {
        responseCmnd("Unknown: Play|Stop|Vol");
    }
}

int main() {
    addCommand("MP3");   // Praefix "MP3" registrieren
    return 0;
}
```

**Ergebnis:** Die Eingabe von `MP3Play` in der Tasmota-Konsole ruft `Command("PLAY")` auf, `MP3Vol 20` ruft `Command("VOL 20")` auf.

### TaskLoop-Beispiel (ESP32)

```c
int counter = 0;

void TaskLoop() {
    counter++;
    char buf[64];
    sprintfInt(buf, "TaskLoop count=%d", counter);
    addLog(buf);       // erscheint im Tasmota-Konsolenlog
    delay(1000);       // echte 1-Sekunden-Verzoegerung, blockiert Tasmota nicht
}

void JsonCall() {
    char buf[64];
    sprintfInt(buf, ",\"TinyC\":{\"Count\":%d}", counter);
    responseAppend(buf);
}

int main() {
    addLog("TaskLoop demo starting");
    return 0;
}
```

**Ergebnis:** `TaskLoop()` laeuft unabhaengig in einem FreeRTOS-Task und erhoeht den Zaehler jede Sekunde. `JsonCall()` meldet den Zaehler in der MQTT-Telemetrie. Beide laufen gleichzeitig — der Mutex stellt sicheren VM-Zugriff sicher.

### Wichtige Hinweise

- Callbacks muessen **schnell** sein — maximal 200.000 Instruktionen (ESP32) / 20.000 (ESP8266) pro Aufruf
- Kein `delay()` in Callbacks (begrenzt auf 100ms falls aufgerufen) — ausser `TaskLoop()`, das echte Verzoegerungen unterstuetzt
- `main()` muss zurueckkehren (nicht endlos schleifen), damit Callbacks aktiviert werden
- Nur die acht oben genannten bekannten Namen werden erkannt
- Der Compiler erkennt diese Funktionsnamen automatisch und bettet sie in die Binaerdatei ein
- `EveryLoop()` laeuft bei jeder Hauptschleifen-Iteration (~1–5 ms) — halten Sie es **sehr kurz**, um Tasmota nicht zu blockieren
- `Every50ms()` ist ideal fuer schnelles, nicht-blockierendes E/A-Polling (SPI-Funk, GPIO usw.)
- Verwenden Sie `WebPage()` fuer einmaligen Seiteninhalt (Diagramme, Skripte) — wird einmal beim Laden der Seite aufgerufen
- Verwenden Sie `WebCall()` fuer Sensor-aehnliche Zeilen, die periodisch aktualisiert werden
- Verwenden Sie `UdpCall()` zur Verarbeitung eingehender UDP-Multicast-Variablen
- `TaskLoop()` laeuft in einem eigenen FreeRTOS-Task (nur ESP32) — kann `delay()` frei verwenden, VM-Zugriff ist Mutex-serialisiert mit Haupt-Thread-Callbacks

---

## Tasmota-Systemvariablen

TinyC stellt virtuelle `tasm_*`-Variablen bereit, die den Tasmota-Systemzustand direkt lesen/schreiben. Sie werden wie normale Variablen verwendet — keine Funktionsaufrufe noetig. Der Compiler uebersetzt sie automatisch in Syscalls.

### Verfuegbare Variablen

| Variable | Typ | L/S | Beschreibung |
|----------|-----|-----|--------------|
| `tasm_wifi` | int | lesen | WiFi-Status (1 = verbunden, 0 = getrennt) |
| `tasm_mqttcon` | int | lesen | MQTT-Verbindungsstatus (1 = verbunden) |
| `tasm_teleperiod` | int | lesen/schreiben | Telemetrieperiode in Sekunden (10–3600, begrenzt) |
| `tasm_uptime` | int | lesen | Geraete-Betriebszeit in Sekunden |
| `tasm_heap` | int | lesen | Freier Heap-Speicher in Bytes |
| `tasm_power` | int | lesen/schreiben | Relais-Schaltzustand (Bitmaske, Schreiben schaltet Relais) |
| `tasm_dimmer` | int | lesen/schreiben | Dimmer-Pegel 0–100 (Schreiben sendet Dimmer-Befehl) |
| `tasm_temp` | float | lesen | Temperatur vom Tasmota-Sensor (globales `TempRead()`) |
| `tasm_hum` | float | lesen | Luftfeuchtigkeit vom Tasmota-Sensor (globales `HumRead()`) |
| `tasm_hour` | int | lesen | Aktuelle Stunde (0–23, von RTC) |
| `tasm_minute` | int | lesen | Aktuelle Minute (0–59, von RTC) |
| `tasm_second` | int | lesen | Aktuelle Sekunde (0–59, von RTC) |
| `tasm_year` | int | lesen | Aktuelles Jahr (z.B. 2026, von RTC) |
| `tasm_month` | int | lesen | Aktueller Monat (1–12, von RTC) |
| `tasm_day` | int | lesen | Tag des Monats (1–31, von RTC) |
| `tasm_wday` | int | lesen | Wochentag (1=So, 2=Mo, ... 7=Sa) |
| `tasm_cw` | int | lesen | ISO-Kalenderwoche (1–53) |
| `tasm_sunrise` | int | lesen | Sonnenaufgang, Minuten seit Mitternacht (erfordert USE_SUNRISE) |
| `tasm_sunset` | int | lesen | Sonnenuntergang, Minuten seit Mitternacht (erfordert USE_SUNRISE) |
| `tasm_time` | int | lesen | Aktuelle Uhrzeit, Minuten seit Mitternacht |
| `tasm_pheap` | int | lesen | Freier PSRAM-Speicher in Bytes (nur ESP32) |
| `tasm_smlj` | int | lesen/schreiben | SML JSON-Ausgabe aktivieren/deaktivieren (erfordert USE_SML_M) |
| `tasm_npwr` | int | lesen | Anzahl der Power-Geraete (Relais) |

### Indizierte Tasmota-Zustandsfunktionen

| Funktion | Beschreibung |
|----------|-------------|
| `int tasmPower(int index)` | Power-Zustand des Relais `index` (0-basiert). Gibt 0 oder 1 zurueck |
| `int tasmSwitch(int index)` | Schalter-Zustand (0-basiert, Switch1 = Index 0). Gibt -1 bei ungueltigem Index zurueck |
| `int tasmCounter(int index)` | Impulszaehler-Wert (0-basiert, Counter1 = Index 0). Erfordert USE_COUNTER |

### Tasmota String-Info

`int tasmInfo(int sel, char buf[])` — fuellt `buf` mit einem Tasmota-Info-String. Gibt Stringlaenge zurueck.

| sel | Inhalt |
|-----|--------|
| 0 | MQTT-Topic |
| 1 | MAC-Adresse |
| 2 | Lokale IP-Adresse |
| 3 | Friendly Name |
| 4 | Device Name |
| 5 | MQTT Group-Topic |
| 6 | Reset-Grund (String) |

**Beispiel:**
```c
char topic[64];
tasmInfo(0, topic);    // MQTT-Topic holen
char ip[20];
tasmInfo(2, ip);       // lokale IP holen
```

### Verwendung

```c
// Systemzustand lesen
if (tasm_wifi) {
    printStr("WiFi connected\n");
}

// Sensorwerte lesen (float)
float t = tasm_temp;
float h = tasm_hum;

// Echtzeituhr lesen
int h = tasm_hour;       // 0–23
int m = tasm_minute;     // 0–59
int s = tasm_second;     // 0–59
int y = tasm_year;       // z.B. 2026
int mo = tasm_month;     // 1–12
int d = tasm_day;        // 1–31
int wd = tasm_wday;      // 1=So..7=Sa
int cw = tasm_cw;        // ISO-Kalenderwoche 1–53

// Sonnenauf-/untergangsautomatisierung
int now = tasm_time;     // Minuten seit Mitternacht
if (now > tasm_sunset || now < tasm_sunrise) {
    tasm_power = 1;      // Nacht — Licht einschalten
}

// Systemzustand schreiben
tasm_teleperiod = 60;    // Telemetrie auf 60 Sekunden setzen
tasm_power = 1;          // Relais EIN schalten
tasm_dimmer = 50;        // Dimmer auf 50% setzen
```

### Hinweise

- **Keine Deklaration noetig** — `tasm_*`-Namen werden vom Compiler automatisch erkannt
- **Kein globaler Slot belegt** — sie verbrauchen keinen globalen Variablenspeicher
- **Nur-Lesen-Erzwingung** — Schreiben auf Nur-Lesen-Variablen (z.B. `tasm_wifi = 1`) erzeugt einen Kompilierfehler
- **Float-Typinferenz** — `tasm_temp` und `tasm_hum` sind in Ausdruecken korrekt als `float` typisiert
- **Schreib-Seiteneffekte** — `tasm_power` fuehrt den `Power`-Befehl aus, `tasm_dimmer` fuehrt den `Dimmer`-Befehl aus, `tasm_teleperiod` aktualisiert Tasmota's Einstellungen direkt
- In der Browser-IDE geben alle Variablen simulierte Werte zurueck

### Beispiel — Automatische Leistungssteuerung

```c
void EverySecond() {
    // Relais ausschalten wenn Temperatur zu hoch
    if (tasm_temp > 30.0) {
        tasm_power = 0;
    }

    // Per Web melden
    char buf[64];
    sprintfFloat(buf, "{s}Temp{m}%.1f C{e}", tasm_temp);
    webSend(buf);
}

int main() {
    tasm_teleperiod = 30;  // schnelle Telemetrie zum Testen
    return 0;
}
```

---

## Arrays

### Deklaration & Initialisierung
```c
int data[10];                       // nicht initialisiert
int primes[5] = {2, 3, 5, 7, 11};  // mit Initialisierer
float values[3] = {1.5, 2.5};      // teilweise initialisiert
char name[32] = "TinyC";           // Zeichenketten-Initialisierung (null-terminiert)
char greeting[] = "Hello World";    // Groesse aus String abgeleitet (12)
int flags[] = {1, 0, 1, 1};        // Groesse aus Initialisierer abgeleitet (4)
```

Wenn die Groesse weggelassen wird (`[]`), leitet der Compiler sie automatisch ab:
- **String-Initialisierer:** Groesse = Stringlaenge + 1 (fuer Null-Terminator)
- **Array-Initialisierer:** Groesse = Anzahl der Elemente

### Zugriff
```c
int x = data[0];       // lesen
data[3] = 42;          // schreiben
data[i + 1] = data[i]; // berechneter Index
```

### Gueltigkeitsbereich
- **Kleine Arrays (≤16 Elemente)** — inline im globalen Datenspeicher oder lokalen Rahmen (schneller Direktzugriff)
- **Grosse Arrays (>16 Elemente)** — automatisch auf dem VM-Heap allokiert

### Array-Speicher

Arrays mit bis zu 16 Elementen werden inline im globalen oder lokalen Rahmen gespeichert fuer schnellen Direktzugriff. Arrays mit mehr als 16 Elementen werden vom Compiler automatisch auf dem VM-Heap allokiert — keine spezielle Syntax noetig:

```c
int rgb[3];            // inline (3 ≤ 16) — schneller Direktzugriff
char buf[128];         // Heap (128 > 16) — automatische Allokation
float data[2000];      // Heap (2000 > 16)

int main() {
    rgb[0] = 255;       // direkter Rahmenzugriff
    buf[0] = 'H';       // Heap-Zugriff — gleiche Syntax
    data[1999] = 3.14;  // Heap-Zugriff
    return 0;
}
```

Sowohl Inline- als auch Heap-Arrays unterstuetzen alle gleichen Operationen: Elementzugriff, Zeichenkettenoperationen auf `char[]`, Uebergabe an Funktionen usw.

**Heap-Grenzen:**

| Plattform | Max. Heap-Slots | Max. Handles |
|-----------|-----------------|--------------|
| ESP8266   | 2.048 (8 KB)    | 8            |
| ESP32     | 8.192 (32 KB)   | 16           |
| Browser   | 16.384 (64 KB)  | 32           |

---

## Zeichenketten

Zeichenketten in TinyC sind `char`-Arrays mit Null-Terminierung.

### Deklaration
```c
char greeting[32] = "Hello";
char buffer[64];    // nicht initialisierter Puffer
```

### Zeichenkettenzuweisung & Verkettung mit `+`

Die Operatoren `=` und `+=` funktionieren mit `char[]`-Variablen fuer intuitive Zeichenkettenverarbeitung:

```c
char buf[64];
char name[16] = "World";

// Zeichenketten-Literal oder char-Array zuweisen
buf = "Hello";          // entspricht strcpy(buf, "Hello")
buf = name;             // entspricht strcpy(buf, name)

// Mit += anfuegen
buf += " ";             // entspricht strcat(buf, " ")
buf += name;            // entspricht strcat(buf, name)

// Mit + verketten
buf = buf + "!";        // entspricht strcat(buf, "!")
buf = buf + name;       // entspricht strcat(buf, name)
```

**Hinweis:** Der `+`-Operator funktioniert nur, wenn die linke Seite von `=` dieselbe Variable wie die linke Seite von `+` ist (d.h. `buf = buf + ...`). Variablenueberschreitende Verkettung wie `a = b + c` wird nicht unterstuetzt — verwenden Sie dafuer `strcpy` + `strcat`.

### Eingebaute Zeichenketten-Funktionen
```c
int len = strlen(greeting);             // Laenge (ohne \0)
strcpy(buffer, greeting);               // Array in Array kopieren
strcpy(buffer, "World");                // Literal in Array kopieren
strcat(buffer, greeting);               // Array anfuegen
strcat(buffer, "!");                    // Literal anfuegen
int cmp = strcmp(greeting, buffer);     // Vergleich: -1, 0 oder 1
printString(greeting);                  // Zeichenkette ausgeben
```

### Formatierte Zeichenkettenausgabe (sprintf)

Einen einzelnen Wert in ein char-Array formatieren. Der Compiler erkennt den Werttyp automatisch:

```c
char line[64];
char name[16];
float pi = 3.14;

sprintf(line, "x = %d", 42);              // "x = 42"
sprintf(line, "pi = %.2f", pi);           // "pi = 3.14"
sprintf(line, "name: %s", name);          // "name: World"
```

### Mehrteilige Zeichenketten erstellen (sprintfAppend)

Verwenden Sie `sprintfAppend`, um mehrere Werte in einen Puffer zu verketten. Es fuegt am aktuellen Ende der Zeichenkette an:

```c
char report[128];
sprintf(report, "Sensor %d", 1);               // "Sensor 1"
sprintfAppend(report, " name=%s", name);        // "Sensor 1 name=World"
sprintfAppend(report, " val=%d", 42);           // "Sensor 1 name=World val=42"
sprintfAppend(report, " temp=%.1f", 3.14);      // "Sensor 1 name=World val=42 temp=3.1"
printString(report);
```

| Funktion | Beschreibung |
|----------|-------------|
| `sprintf(char dst[], "fmt", val)` | Wert in dst formatieren (ueberschreibt). Typ wird automatisch erkannt. |
| `sprintfAppend(char dst[], "fmt", val)` | Wert formatieren und an dst anfuegen. Typ wird automatisch erkannt. |

> **Alte Varianten:** `sprintfInt`, `sprintfFloat`, `sprintfStr`, `sprintfAppendInt`, `sprintfAppendFloat`, `sprintfAppendStr` funktionieren weiterhin.

**Format-Spezifikatoren:** `%d` (int), `%f` `%.2f` `%e` `%g` (float), `%s` (Zeichenkette). Jeder Aufruf verarbeitet genau einen `%`-Spezifikator.

### Zeichenkettenmanipulation

```c
char src[64] = "hello,world,test";
char dst[32];

// N-tes Token (1-basiert) nach Trennzeichen extrahieren
int len = strToken(dst, src, ',', 2);  // dst = "world", len = 5

// Teilzeichenkette (0-basierte Position, Laenge)
strSub(dst, src, 6, 5);               // dst = "world"
strSub(dst, src, -4, 4);              // dst = "test" (negativ = vom Ende)

// Teilzeichenketten-Position finden (-1 wenn nicht gefunden)
int pos = strFind(src, "world");       // pos = 6
int no = strFind(src, "xyz");          // no = -1
```

| Funktion | Beschreibung |
|----------|-------------|
| `strToken(char dst[], char src[], int delim, int n)` | N-tes Token (1-basiert) durch Trennzeichen `delim` in dst kopieren. Gibt die Token-Laenge zurueck. |
| `strSub(char dst[], char src[], int pos, int len)` | `len` Zeichen ab `pos` (0-basiert, negativ=vom Ende) in dst kopieren. `len=0` kopiert bis zum Ende der Zeichenkette. Gibt die tatsaechliche Laenge zurueck. |
| `strFind(char haystack[], char needle[])` | Erstes Vorkommen von needle in haystack finden. Gibt die Position (0-basiert) oder -1 zurueck, wenn nicht gefunden. |
| `int strToInt(char str[])` | String in Integer umwandeln (wie `atoi`) |
| `float strToFloat(char str[])` | String in Float umwandeln (wie `atof`) |

### Array-Sortierung

| Funktion | Beschreibung |
|----------|-------------|
| `sortArray(int arr[], int count, int flags)` | Array sortieren. `flags`: 0=int aufsteigend, 1=float aufsteigend, 2=int absteigend, 3=float absteigend |
| `arrayFill(int arr[], int value, int count)` | Erste `count` Elemente mit `value` fuellen |
| `arrayCopy(int dst[], int src[], int count)` | `count` Elemente von `src` nach `dst` kopieren |
| `int smlCopy(int arr[], int count)` | SML-Decoderwerte in Float-Array kopieren. Gibt Anzahl zurueck (erfordert USE_SML_M) |

### Zeichenzugriff
```c
char ch = greeting[0];     // lesen: 'H'
greeting[0] = 'h';         // schreiben: jetzt "hello"
```

### Escape-Sequenzen in Zeichenketten
| Escape | Zeichen          |
|--------|------------------|
| `\n`   | Zeilenumbruch    |
| `\t`   | Tabulator        |
| `\r`   | Wagenruecklauf   |
| `\\`   | Backslash        |
| `\"`   | Anfuehrungszeichen |
| `\'`   | Einfaches Anfuehrungszeichen |
| `\0`   | Null-Terminator  |

---

## Praeprozessor

### #define — Kompilierzeit-Konstanten

Einfache Kompilierzeit-Konstanten (keine Makro-Expansion):
```c
#define LED_PIN 5
#define MAX_SIZE 100
#define PI 3.14
#define DOUBLE_PI (PI * 2)
```

**Eigenschaften:**
- Der Wert muss ein konstanter Ausdruck sein
- Unterstuetzt Arithmetik mit anderen `#define`-Werten: `+`, `-`, `*`, `/`
- Verwendet fuer Array-Groessen, Funktionsargumente usw.
- Gueltigkeitsbereich: gesamtes Programm
- Wertlose Definitionen fuer Bedingungen erlaubt: `#define ESP32`

**Einschraenkungen:**
- Kein `#include`

### Funktionsaehnliche Makros

Parametrisierte Makros fuehren Textersetzung vor der Kompilierung durch:

```c
#define LOG(A) addLog(A)
#define CLAMP(V, MX) min(max(V, 0), MX)
#define SQUARE(X) (X * X)
```

**Verwendung:**
```c
LOG("sensor init");          // -> addLog("sensor init")
int v = CLAMP(reading, 100); // -> int v = min(max(reading, 0), 100)
int s = SQUARE(5);           // -> int s = (5 * 5)
```

**Eigenschaften:**
- Parameter werden durch Ganzwort-Abgleich ersetzt (ersetzt keine Teilbezeichner)
- Verschachtelte Klammern in Argumenten werden korrekt behandelt: `LOG(foo(1,2))` funktioniert
- Zeichenketten-Literal-Argumente bleiben erhalten: `LOG("hello, world")` — das Komma innerhalb der Anfuehrungszeichen wird nicht als Argumenttrenner behandelt
- Verschachtelte Makro-Expansion: Makros im expandierten Rumpf werden expandiert (bis zu 10 Iterationen)
- Mehrere Parameter unterstuetzt: `#define ADD(A, B) (A + B)`

**Makros mit leerem Rumpf — Debug-Entfernung:**
```c
#define DBG(M)              // leerer Rumpf — kein Ersetzungstext

DBG("checkpoint 1");        // -> vollstaendig entfernt (einschliesslich Semikolon)
int x = 42;                 // diese Zeile bleibt unberuehrt
```

Makros mit leerem Rumpf entfernen den gesamten Aufruf einschliesslich des abschliessenden Semikolons. Dies ist nuetzlich zum Entfernen von Debug-Aufrufen in Produktionsversionen:

```c
#ifdef DEBUG
  #define DBG(M) addLog(M)
#else
  #define DBG(M)
#endif

DBG("init done");  // loggt im Debug, entfernt im Release
```

### Bedingte Kompilierung

```c
#define ESP32
#define USE_SENSOR

#ifdef ESP32
  int pin = 8;       // eingeschlossen — ESP32 ist definiert
#else
  int pin = 2;       // ausgeschlossen
#endif

#ifndef USE_DISPLAY
  // eingeschlossen — USE_DISPLAY ist nicht definiert
#endif
```

| Direktive               | Beschreibung                                          |
|-------------------------|-------------------------------------------------------|
| `#define NAME`          | Einen Namen definieren (ohne Wert, fuer Bedingungen)  |
| `#define NAME value`    | Einen Namen mit einem konstanten Wert definieren      |
| `#define NAME(A) body`  | Funktionsaehnliches Makro mit Textersetzung           |
| `#undef NAME`          | Einen zuvor definierten Namen aufheben                |
| `#ifdef NAME`          | Block einschliessen, wenn NAME definiert ist          |
| `#ifndef NAME`         | Block einschliessen, wenn NAME NICHT definiert ist    |
| `#if EXPR`             | Block einschliessen, wenn Ausdruck ungleich Null      |
| `#else`                | Alternativer Block                                    |
| `#endif`               | Bedingten Block beenden                               |

**`#if`-Ausdruecke** unterstuetzen:
- Ganzzahl-Literale: `#if 1`, `#if 0`
- Definierte Namen (1 wenn definiert, 0 wenn nicht): `#if ESP32`
- `defined(NAME)`-Operator: `#if defined(ESP32)`
- Logische Operatoren: `&&`, `||`, `!`
- Vergleich: `==`, `!=`, `>`, `<`, `>=`, `<=`
- Klammern zur Gruppierung

```c
#if defined(ESP32) && !defined(USE_LEGACY)
  // ESP32-spezifischer moderner Code
#endif
```

**Hinweise:**
- Bedingungen koennen verschachtelt werden
- Uebersprungener Code wird nicht kompiliert (muss keine gueltige Syntax sein)
- Zeilennummern in Fehlermeldungen bleiben erhalten

---

## Kommentare

```c
// Einzeiliger Kommentar

/* Mehrzeiliger
   Kommentar */
```

---

## Typumwandlung

### Explizite Umwandlungen
```c
float f = 3.14;
int i = (int)f;         // schneidet auf 3 ab

int x = 42;
float y = (float)x;     // konvertiert zu 42.0

int ch = 321;
char c = (char)ch;      // maskiert auf 0xFF -> 65 ('A')

int b = (bool)42;       // ungleich Null -> 1
```

### Implizite Konvertierungen
Wenn `int` und `float` in einem Ausdruck gemischt werden, wird der `int`-Operand automatisch zu `float` heraufgestuft:
```c
int a = 5;
float b = 2.5;
float c = a + b;    // a wird zu float heraufgestuft, Ergebnis = 7.5
```

---

## Eingebaute Funktionen

### Ausgabe

| Funktion                | Beschreibung                       |
|-------------------------|-------------------------------------|
| `print(int value)`      | Ganzzahl + Zeilenumbruch ausgeben   |
| `print("literal")`     | String-Literal ausgeben (automatisch erkannt) |
| `print(char buf[])`    | Char-Array als String ausgeben (automatisch erkannt) |
| `printStr("literal")`   | Zeichenketten-Literal ausgeben (explizit) |
| `printString(char arr[])` | Null-terminiertes Char-Array ausgeben (explizit) |

> **Hinweis:** `print()` erkennt den Argumenttyp automatisch. Bei einem String-Literal wird der String ausgegeben. Bei einem `char[]`-Array wird der Inhalt als String ausgegeben. Bei einem `int` wird der numerische Wert ausgegeben. Die expliziten Funktionen `printStr`/`printString` sind weiterhin verfügbar, aber selten nötig.

### GPIO

| Funktion                             | Beschreibung                          |
|--------------------------------------|---------------------------------------|
| `pinMode(int pin, int mode)`         | Pin-Modus setzen (1=INPUT, 3=OUTPUT, 5=INPUT_PULLUP, 9=INPUT_PULLDOWN) |
| `digitalWrite(int pin, int value)`   | HIGH(1) oder LOW(0) schreiben         |
| `int digitalRead(int pin)`           | Pin-Zustand lesen                     |
| `int analogRead(int pin)`            | Analogwert lesen (0–4095)             |
| `analogWrite(int pin, int value)`    | PWM-Wert schreiben                    |
| `gpioInit(int pin, int mode)`        | Pin von Tasmota freigeben + pinMode   |

### Zeitsteuerung

| Funktion                         | Beschreibung                        |
|----------------------------------|-------------------------------------|
| `delay(int ms)`                  | Millisekunden warten                |
| `delayMicroseconds(int us)`      | Mikrosekunden warten                |
| `int millis()`                   | Millisekunden seit Programmstart    |
| `int micros()`                   | Mikrosekunden seit Programmstart    |

### Software-Timer

4 unabhaengige Countdown-Timer (IDs 0-3) basierend auf `millis()`. Timer laufen unabhaengig von Callbacks — setzen Sie einen Timer in `main()` oder einem beliebigen Callback, pruefen Sie ihn in `EveryLoop()`.

| Funktion                              | Beschreibung                                              |
|---------------------------------------|-----------------------------------------------------------|
| `timerStart(int id, int ms)`          | Timer `id` (0-3) mit `ms` Millisekunden Timeout starten  |
| `int timerDone(int id)`               | Gibt 1 zurueck wenn Timer abgelaufen (oder nie gestartet), 0 wenn laufend |
| `timerStop(int id)`                   | Timer abbrechen                                           |
| `int timerRemaining(int id)`          | Verbleibende Millisekunden (0 wenn abgelaufen/gestoppt)   |

**Beispiel — Wiederholender Timer mit Timeout:**
```c
int counter;

void main() {
    counter = 0;
    timerStart(0, 5000);    // Timer 0: alle 5 Sekunden
    timerStart(1, 60000);   // Timer 1: nach 1 Minute stoppen
}

void EveryLoop() {
    if (timerDone(0)) {
        counter++;
        print(counter);
        timerStart(0, 5000);  // fuer naechstes Intervall neu starten
    }
    if (timerDone(1)) {
        timerStop(0);         // wiederholenden Timer stoppen
    }
}
```

### Seriell

| Funktion                          | Beschreibung                            |
|-----------------------------------|-----------------------------------------|
| `serialBegin(int baud)`           | Seriell mit Baudrate initialisieren     |
| `serialPrint("literal")`          | Zeichenkette auf seriell ausgeben       |
| `serialPrintInt(int value)`       | Ganzzahl auf seriell ausgeben           |
| `serialPrintFloat(float value)`   | Gleitkommazahl auf seriell ausgeben     |
| `serialPrintln("literal")`        | Zeichenkette + Zeilenumbruch auf seriell |
| `int serialRead()`                | Byte lesen (-1 wenn keines verfuegbar)  |
| `int serialAvailable()`           | Verfuegbare Bytes zum Lesen             |
| `serialClose()`                   | Seriellen Port schließen                |
| `serialWriteByte(int b)`          | Einzelnes Byte an serielle Schnittstelle senden |
| `serialWriteStr(char str[])`      | Char-Array an serielle Schnittstelle senden |
| `serialWriteBuf(char buf[], int len)` | `len` Bytes aus Buffer an serielle Schnittstelle senden |

### 1-Wire

| Funktion                          | Beschreibung                                               |
|-----------------------------------|------------------------------------------------------------|
| `owSetPin(int pin)`               | GPIO-Pin fuer nativen 1-Wire-Bus setzen                    |
| `int owReset()`                   | Reset-Puls senden, 1 bei Praesenz-Erkennung                |
| `owWrite(int byte)`               | Ein Byte auf den Bus schreiben                             |
| `int owRead()`                    | Ein Byte vom Bus lesen                                     |
| `owWriteBit(int bit)`             | Ein einzelnes Bit schreiben (0 oder 1)                     |
| `int owReadBit()`                 | Ein einzelnes Bit lesen                                    |
| `owSearchReset()`                 | ROM-Suchstatus zuruecksetzen                               |
| `int owSearch(char rom[])`        | Naechstes Geraet finden, 8-Byte-ROM in `rom[]` speichern, 1 bei Erfolg |

> Die nativen 1-Wire-Funktionen verwenden hardware-getimtes Bit-Banging in C — keine externe Bibliothek noetig. Ein 4,7 kΩ Pull-up-Widerstand auf der Datenleitung ist erforderlich. Fuer lange Busse oder stoeranfaellige Umgebungen kann eine DS2480B Seriell-zu-1-Wire-Bruecke verwendet werden (siehe `examples/onewire.tc`).

### Mathematik

| Funktion                                            | Beschreibung                     |
|-----------------------------------------------------|----------------------------------|
| `int abs(int value)`                                | Absolutwert                      |
| `int min(int a, int b)`                             | Minimum zweier Werte             |
| `int max(int a, int b)`                             | Maximum zweier Werte             |
| `int map(int val, int fLo, int fHi, int tLo, int tHi)` | Wert von einem Bereich auf einen anderen abbilden |
| `int random(int min, int max)`                      | Zufaellige Ganzzahl im Bereich   |
| `float sqrt(float x)`                               | Quadratwurzel                    |
| `float sin(float x)`                                | Sinus (Bogenmass)                |
| `float cos(float x)`                                | Kosinus (Bogenmass)              |
| `float exp(float x)`                                | Exponentialfunktion (e^x)        |
| `float log(float x)`                                | Natürlicher Logarithmus (ln x)  |
| `float pow(float basis, float exp)`                  | Potenz (basis^exp)               |
| `float acos(float x)`                               | Arkuskosinus (Bogenmass)         |
| `float intBitsToFloat(int bits)`                     | Int als IEEE 754 Float interpretieren |
| `int floor(float x)`                                | Ganzzahlanteil (Richtung −∞)     |
| `int ceil(float x)`                                 | Ganzzahlanteil + 1 (Richtung +∞) |
| `int round(float x)`                                | Auf naechste Ganzzahl runden     |

### Zeichenketten

| Funktion                             | Beschreibung                         |
|--------------------------------------|--------------------------------------|
| `int strlen(char arr[])`             | Zeichenkettenlaenge (ohne Null)      |
| `strcpy(char dst[], char src[])`     | Zeichenkette kopieren                |
| `strcpy(char dst[], "literal")`      | Literal in Array kopieren            |
| `strcat(char dst[], char src[])`     | Zeichenkette verketten               |
| `strcat(char dst[], "literal")`      | Literal verketten                    |
| `int strcmp(char a[], char b[])`     | Vergleich: gibt -1, 0 oder 1 zurueck |
| `printString(char arr[])`            | Zeichenkette ausgeben                |

**Zeichenketten-Operatoren:** `char[]`-Variablen unterstuetzen auch `=`, `+=` und `+` fuer Zeichenkettenzuweisung und -verkettung — siehe Abschnitt [Zeichenketten](#zeichenketten).

### sprintf — Formatierte Zeichenketten

Einen einzelnen Wert in ein char-Array formatieren. Der Compiler erkennt den Werttyp automatisch. Jeder Aufruf verarbeitet einen `%`-Spezifikator.

| Funktion | Beschreibung |
|----------|-------------|
| `int sprintf(char dst[], "fmt", val)` | Wert in dst formatieren (ueberschreibt). Typ automatisch erkannt. |
| `int sprintfAppend(char dst[], "fmt", val)` | Wert formatieren, an Ende von dst anfuegen. Typ automatisch erkannt. |

> **Alte Varianten:** `sprintfInt`, `sprintfFloat`, `sprintfStr`, `sprintfAppendInt`, `sprintfAppendFloat`, `sprintfAppendStr` funktionieren weiterhin.

**Format-Spezifikatoren:** `%d` (int), `%f` `%.Nf` `%e` `%g` (float), `%s` (Zeichenkette).
Alle Funktionen geben die Gesamtlaenge der Zeichenkette zurueck.

```c
// Mehrteilige Zeichenkette durch Verkettung von Append-Aufrufen erstellen:
char buf[128];
sprintf(buf, "ID=%d", 1);
sprintfAppend(buf, " name=%s", name);
sprintfAppend(buf, " val=%.1f", 3.14);
// buf = "ID=1 name=World val=3.1"
```

### Datei-E/A

Dateien auf dem ESP32-Dateisystem (LittleFS) lesen und schreiben. In der Browser-IDE werden Dateien in einem virtuellen Dateisystem simuliert.

| Funktion                                   | Beschreibung                                          |
|--------------------------------------------|-------------------------------------------------------|
| `int fileOpen("path", mode)`               | Datei oeffnen, gibt Handle (0–3) oder -1 bei Fehler zurueck |
| `int fileClose(handle)`                    | Datei-Handle schliessen, gibt 0 oder -1 zurueck      |
| `int fileRead(handle, char buf[], max)`    | Bis zu max Bytes in buf lesen, gibt Anzahl zurueck    |
| `int fileWrite(handle, char buf[], len)`   | len Bytes aus buf schreiben, gibt Anzahl zurueck      |
| `int fileExists("path")`                   | Pruefen ob Datei existiert: 1=ja, 0=nein             |
| `int fileDelete("path")`                   | Datei loeschen, gibt 0=ok, -1=Fehler zurueck         |
| `int fileSize("path")`                     | Dateigroesse in Bytes, -1 bei Fehler                  |
| `int fileSeek(handle, offset, whence)`     | Zur Position springen. Gibt 1=ok, 0=Fehler zurueck   |
| `int fileTell(handle)`                     | Aktuelle Position in Datei, -1 bei Fehler             |
| `int fsInfo(int sel)`                      | Dateisystem-Info: sel=0 → Gesamtgroesse KB, sel=1 → frei KB |
| `int fileOpenDir("path")`                  | Verzeichnis zum Auflisten oeffnen, gibt Handle oder -1 zurueck |
| `int fileReadDir(handle, char name[])`     | Naechsten Dateinamen in name lesen. Gibt 1=Eintrag, 0=Ende zurueck |

**Dateimodi:** `0` = Lesen, `1` = Schreiben (Erstellen/Abschneiden), `2` = Anfuegen

**Seek-Modus (whence):** `0` = SEEK_SET (vom Anfang), `1` = SEEK_CUR (von aktueller Position), `2` = SEEK_END (vom Ende)

**Hinweise:**
- Dateipfade können Zeichenketten-Literale oder char[]-Variablen sein (z.B. `"/data.txt"`)
- **Dateisystem-Auswahl** (Scripter-kompatibel): Standard ist SD-Karte (`ufsp`). Praefix `/ffs/` fuer Flash, `/sdfs/` fuer SD-Karte explizit: `fileOpen("/ffs/config.txt", 0)` oeffnet von Flash, `fileOpen("/data.txt", 0)` oeffnet von SD-Karte
- Maximal 4 gleichzeitig geoeffnete Dateien (ESP32), 8 im Browser
- Puffer-Argumente (`buf`) muessen `char`-Arrays sein, keine Zeichenketten-Literale
- `fileRead` gibt die tatsaechlich gelesene Byteanzahl zurueck (kann weniger als `max` sein)
- Schliessen Sie Dateien immer, wenn Sie fertig sind, um Handles freizugeben

```c
// Beispiel: Schreiben und zuruecklesen
char data[32];
char buf[32];
strcpy(data, "Hello!\n");

int f = fileOpen("/test.txt", 1);   // Schreibmodus
fileWrite(f, data, strlen(data));
fileClose(f);

f = fileOpen("/test.txt", 0);       // Lesemodus
int n = fileRead(f, buf, 31);
buf[n] = 0;
fileClose(f);
printString(buf);                    // gibt "Hello!" aus

fileDelete("/test.txt");             // aufraeumen

// Beispiel: Dateien in einem Verzeichnis auflisten
char fname[64];
int dir = fileOpenDir("/images");
if (dir >= 0) {
    while (fileReadDir(dir, fname)) {
        printString(fname);
        print("\n");
    }
    fileClose(dir);
}
```

**Hinweise zur Verzeichnisauflistung:**
- `fileOpenDir` belegt einen Datei-Handle-Slot (gleicher Pool wie `fileOpen`), mit `fileClose` schliessen
- `fileReadDir` gibt nur Dateinamen zurueck (kein Pfad-Praefix), ueberspringt Unterverzeichnisse
- Pfad-Argument kann ein String-Literal oder eine char-Array-Variable sein

### Erweiterte Dateioperationen

Dateisystemverwaltung, strukturierte Array-Ein-/Ausgabe und Logdatei-Rotation.

| Funktion | Beschreibung |
|----------|--------------|
| `int fileFormat()` | LittleFS-Dateisystem formatieren (loescht alle Daten). Gibt 0=ok zurueck |
| `int fileMkdir("pfad")` | Verzeichnis erstellen. Gibt 1=ok, 0=Fehler zurueck |
| `int fileRmdir("pfad")` | Verzeichnis entfernen. Gibt 1=ok, 0=Fehler zurueck |
| `int fileReadArray(int arr[], handle)` | Eine Tab-getrennte Zeile in Int-Array lesen. Gibt Elementanzahl zurueck |
| `fileWriteArray(int arr[], handle)` | Array als Tab-getrennte Zeile mit Zeilenumbruch schreiben |
| `fileWriteArray(int arr[], handle, append)` | Mit Append-Flag: 1=Zeilenumbruch weglassen (zum Anhaengen mehrerer Arrays in einer Zeile) |
| `int fileLog("datei", char str[], limit)` | String + Zeilenumbruch an Datei anhaengen. Erste Zeile entfernen wenn Datei `limit` Bytes ueberschreitet. Gibt Dateigroesse zurueck |
| `int fileDownload("datei", char url[])` | URL-Inhalt in Datei herunterladen. Gibt HTTP-Statuscode zurueck (200=ok). Kompatibel mit Scripters `frw()` |
| `int fileGetStr(char dst[], handle, "delim", index, endChar)` | Datei von Anfang nach N-tem Vorkommen des Trennzeichens durchsuchen, String bis endChar extrahieren. Gibt Stringlaenge zurueck. Kompatibel mit Scripters `fcs()` |

**fileReadArray / fileWriteArray Format:** Werte werden als Dezimaltext durch TAB-Zeichen getrennt gespeichert, ein Array pro Zeile. Kompatibel mit Scripters `fra()`/`fwa()` Format.

```c
// Beispiel: Array-Daten speichern und laden
int values[5];
values[0] = 100; values[1] = 200; values[2] = 300;
values[3] = 400; values[4] = 500;

int f = fileOpen("/data.tab", 1);    // Schreibmodus
fileWriteArray(values, f);           // schreibt "100\t200\t300\t400\t500\n"
fileClose(f);

int loaded[5];
f = fileOpen("/data.tab", 0);       // Lesemodus
int n = fileReadArray(loaded, f);   // n = 5
fileClose(f);
```

```c
// Beispiel: Rotierende Logdatei (max 4096 Bytes)
char msg[64];
strcpy(msg, "Sensorwert: 23.5C");
fileLog("/log.txt", msg, 4096);
// Haengt Zeile an, entfernt aelteste Zeile wenn Datei > 4096 Bytes
```

```c
// Beispiel: Datei aus dem Web herunterladen
char url[128];
strcpy(url, "http://192.168.1.100/data.csv");
int status = fileDownload("/data.csv", url);
// status = 200 bei Erfolg, negativ bei Fehler
```

```c
// Beispiel: 2. komma-getrenntes Feld aus CSV-Datei extrahieren
// Dateiinhalt: "name,temperature,humidity\nSensor1,23.5,65\n"
int f = fileOpen("/data.csv", 0);       // zum Lesen oeffnen
char value[32];
int len = fileGetStr(value, f, ",", 2, '\n');
// value = "23.5", len = 4 (Inhalt zwischen 2. Komma und Zeilenumbruch)
fileClose(f);
```

### Datei-Datenextraktion (IoT-Zeitreihen)

Einen Zeitbereich aus Tab-getrennten CSV-Datendateien in Float-Arrays extrahieren fuer die Analyse. Entwickelt fuer IoT-Datensammler die Sensorwerte in regelmaessigen Intervallen protokollieren.

**Dateiformat:** Erste Spalte ist ein Zeitstempel (ISO oder deutsches Format), gefolgt von Tab-getrennten Float-Werten. Erste Zeile kann eine Kopfzeile sein (wird automatisch uebersprungen).

| Funktion | Beschreibung |
|----------|--------------|
| `int fileExtract(handle, char from[], char to[], col_offs, accum, int arr1[], ...)` | Zeilen extrahieren wo `from <= Zeitstempel <= to`. Sucht immer vom Dateianfang. Gibt Zeilenanzahl zurueck |
| `int fileExtractFast(handle, char from[], char to[], col_offs, accum, int arr1[], ...)` | Wie oben, merkt sich Dateiposition fuer effiziente sequenzielle Zeitbereichsabfragen |

**Parameter:**
- `handle` — offener Datei-Handle (von `fileOpen`)
- `from`, `to` — Zeitbereich als char[] (ISO `2024-01-15T12:00:00` oder Deutsch `15.1.24 12:00`)
- `col_offs` — so viele Datenspalten ueberspringen bevor Arrays gefuellt werden (0 = ab erster Datenspalte)
- `accum` — 0: Werte speichern, 1: zu bestehenden Array-Werten addieren (zum Kombinieren mehrerer Extraktionen)
- `arr1, arr2, ...` — variable Anzahl Int-Arrays, eines pro zu extrahierender Spalte (max. 16). Werte werden als IEEE 754 Float-Bitmuster gespeichert — Float-Variablen oder Casts zum Lesen verwenden

```c
// Beispiel: Temperatur und Luftfeuchtigkeit fuer einen Tag extrahieren
int temp[96], hum[96];  // 96 = 24h * 4 (15-Min-Intervalle)
char from[24], to[24];
strcpy(from, "15.12.21 00:00");
strcpy(to, "16.12.21 00:00");

int f = fileOpen("/daily.csv", 0);
// col_offs=4 ueberspringt WB,WR1,WR2,WR3 → startet bei ATMP_a (5. Datenspalte)
int rows = fileExtract(f, from, to, 4, 0, temp, hum);
fileClose(f);
// rows = Anzahl 15-Min-Abtastungen, temp[] und hum[] mit Floats gefuellt
```

```c
// Beispiel: Sequenzielle Tagesabfragen mit fileExtractFast
int energy[96];
char from[24], to[24];
int f = fileOpen("/yearly.csv", 0);

strcpy(from, "1.1.24 00:00");
strcpy(to, "2.1.24 00:00");
int r1 = fileExtractFast(f, from, to, 0, 0, energy);
// Naechster Tag — fileExtractFast ueberspringt bereits gescannte Daten
strcpy(from, "2.1.24 00:00");
strcpy(to, "3.1.24 00:00");
int r2 = fileExtractFast(f, from, to, 0, 0, energy);
fileClose(f);
```

### Zeit- / Zeitstempel-Funktionen

Zeitstempel-Konvertierung und -Arithmetik. Unterstuetzt ISO-Webformat (`2024-01-15T12:30:45`) und deutsches Gebietsformat (`15.1.24 12:30`). Kompatibel mit Scripters `tstamp`, `cts`, `tso`, `tsn`, `s2t`.

| Funktion | Beschreibung |
|----------|--------------|
| `int timeStamp(char buf[])` | Aktuellen Tasmota-Zeitstempel in buf schreiben. Gibt 0 zurueck |
| `int timeConvert(char buf[], flg)` | Zeitstempel-Format in-place konvertieren. 0=Deutsch→Web, 1=Web→Deutsch. Gibt 0 zurueck |
| `int timeOffset(char buf[], days)` | `days` Tage zum Zeitstempel in buf addieren (in-place). Gibt 0 zurueck |
| `int timeOffset(char buf[], days, zeroFlag)` | Mit `zeroFlag`=1: zusaetzlich Uhrzeit auf Null setzen (HH:MM:SS→00:00:00) |
| `int timeToSecs(char buf[])` | Zeitstempel-String in Epochensekunden umwandeln. Gibt Sekunden zurueck |
| `int secsToTime(char buf[], secs)` | Epochensekunden in ISO-Zeitstempel-String in buf umwandeln. Gibt 0 zurueck |

**Format-Erkennung:** `timeConvert` und `timeOffset` erkennen das Eingabeformat automatisch (ISO wenn `T` enthalten, sonst Deutsch) und konvertieren entsprechend.

```c
// Beispiel: Aktuelle Zeit holen und Formate konvertieren
char ts[24];
timeStamp(ts);               // ts = "2024-06-15T14:30:00"

char de[24];
strcpy(de, ts);
timeConvert(de, 1);          // de = "15.6.24 14:30"

timeConvert(de, 0);          // de = "2024-06-15T14:30:00" (zurueck zu Web)
```

```c
// Beispiel: Datumsarithmetik
char ts[24];
timeStamp(ts);               // "2024-06-15T14:30:00"
timeOffset(ts, 7);           // "2024-06-22T14:30:00" (+ 7 Tage)
timeOffset(ts, -3, 1);       // "2024-06-19T00:00:00" (- 3 Tage, Zeit nullen)
```

```c
// Beispiel: In Sekunden umwandeln und zurueck
char ts[24];
timeStamp(ts);
int secs = timeToSecs(ts);   // Epochensekunden

secs = secs + 3600;          // 1 Stunde addieren
secsToTime(ts, secs);        // zurueck zu Zeitstempel-String
```

### Tasmota-Befehl

Einen beliebigen Tasmota-Konsolenbefehl ausfuehren und die JSON-Antwort erfassen.

| Funktion                                     | Beschreibung                                          |
|----------------------------------------------|-------------------------------------------------------|
| `int tasmCmd("command", char response[])`    | Befehl ausfuehren, Antwort speichern, Laenge zurueckgeben |
| `int tasmCmd(char cmd[], char response[])`   | Befehl ausfuehren (char-Array), Antwort speichern |

**Hinweise:**
- Befehl kann ein String-Literal oder ein char[]-Array sein
- Der Antwortpuffer sollte ein `char`-Array sein (empfohlene Groesse: 256)
- Gibt die Laenge der Antwortzeichenkette zurueck, oder -1 bei Fehler
- In der Browser-IDE wird eine simulierte Scheinantwort zurueckgegeben
- Auf dem ESP32 werden echte Tasmota-Befehle ausgefuehrt und die JSON-Antwort erfasst

```c
char resp[256];
int len = tasmCmd("Status 0", resp);
if (len > 0) {
    printString(resp);   // gibt JSON-Antwort aus
}
```

### Sensor-JSON-Parsing

Einen beliebigen Tasmota-Sensorwert ueber seinen JSON-Pfad auslesen. Pfadsegmente werden durch `#` getrennt (gleiche Konvention wie Tasmota Scripter).

| Funktion | Beschreibung |
|----------|-------------|
| `float sensorGet("Sensor#Key")` | Sensorwert lesen, gibt float zurueck |

Die Funktion loest intern eine Sensor-Statusabfrage aus und navigiert durch den JSON-Baum. Unterstuetzt bis zu 3 Verschachtelungsebenen.

```c
// BME280-Sensor lesen
float temp = sensorGet("BME280#Temperature");
float hum = sensorGet("BME280#Humidity");
float press = sensorGet("BME280#Pressure");

// SHT3X an Adresse 0x44 lesen
float t = sensorGet("SHT3X_0x44#Temperature");

// Energiezaehler lesen (wenn USE_ENERGY_SENSOR definiert)
float power = sensorGet("ENERGY#Power");
float voltage = sensorGet("ENERGY#Voltage");
float today = sensorGet("ENERGY#Today");

// Verschachtelt: Zigbee-Geraet
float zt = sensorGet("ZbReceived#0x2342#Temperature");
```

**Hinweise:**
- Der Pfad muss ein Zeichenketten-Literal sein (wird zur Kompilierzeit aufgeloest)
- Gibt 0.0 zurueck, wenn der Sensor oder Schluessel nicht gefunden wird
- Gibt einen Float zurueck — weisen Sie ihn einer `float`-Variable zu
- In der Browser-IDE werden Temperature=22.5, Humidity=55.0, Pressure=1013.25 simuliert

### Lokalisierte Zeichenketten

Lokalisierte Anzeigetexte von Tasmota zur Laufzeit abrufen. Die Texte entsprechen der Spracheinstellung der Firmware (z.B. `en_GB.h`, `de_DE.h`). Fuer Web-UI-Beschriftungen verwenden; JSON-Schluessel bleiben auf Englisch.

| Funktion | Beschreibung |
|----------|-------------|
| `int LGetString(int index, char dst[])` | Lokalisierten Text nach `dst` kopieren, gibt Laenge zurueck (0 bei ungueltigem Index) |

**Index-Tabelle:**

| Index | Tasmota-Define | Englisch | Deutsch |
|-------|---------------|----------|---------|
| 0 | D_TEMPERATURE | Temperature | Temperatur |
| 1 | D_HUMIDITY | Humidity | Feuchtigkeit |
| 2 | D_PRESSURE | Pressure | Luftdruck |
| 3 | D_DEWPOINT | Dew point | Taupunkt |
| 4 | D_CO2 | Carbon dioxide | Kohlendioxid |
| 5 | D_ECO2 | eCO2 | eCO2 |
| 6 | D_TVOC | TVOC | TVOC |
| 7 | D_VOLTAGE | Voltage | Spannung |
| 8 | D_CURRENT | Current | Strom |
| 9 | D_POWERUSAGE | Power | Leistung |
| 10 | D_POWER_FACTOR | Power Factor | Leistungsfaktor |
| 11 | D_ENERGY_TODAY | Energy Today | Energie Heute |
| 12 | D_ENERGY_YESTERDAY | Energy Yesterday | Energie Gestern |
| 13 | D_ENERGY_TOTAL | Energy Total | Energie Gesamt |
| 14 | D_FREQUENCY | Frequency | Frequenz |
| 15 | D_ILLUMINANCE | Illuminance | Beleuchtungsstaerke |
| 16 | D_DISTANCE | Distance | Entfernung |
| 17 | D_MOISTURE | Moisture | Feuchtigkeit |
| 18 | D_LIGHT | Light | Licht |
| 19 | D_SPEED | Speed | Geschwindigkeit |
| 20 | D_ABSOLUTE_HUMIDITY | Abs Humidity | Abs Feuchtigkeit |

**Beispiel:**
```c
char lbl[32];
char buf[80];

void web_row(int idx, float val, char unit[]) {
    LGetString(idx, lbl);
    strcpy(buf, "{s}");
    strcat(buf, lbl);
    strcat(buf, "{m}");
    webSend(buf);
    sprintfFloat(buf, "%.1f ", val);
    strcat(buf, unit);
    strcat(buf, "{e}");
    webSend(buf);
}

void WebCall() {
    web_row(0, temperature, "&deg;C");  // "Temperatur" (bei de_DE)
    web_row(1, humidity, "%");           // "Feuchtigkeit"
    web_row(2, pressure, "hPa");         // "Luftdruck"
}
```

### Tasmota-Ausgabe (Callbacks)

Daten direkt an Tasmota's Telemetrie- und Websysteme aus Callback-Funktionen senden.

| Funktion | Beschreibung |
|----------|-------------|
| `void responseAppend(char buf[])` | Zeichenkette an MQTT-JSON-Telemetrie anfuegen (`ResponseAppend_P`) |
| `void responseAppend("literal")` | Zeichenketten-Literal an JSON anfuegen (kein Puffer noetig) |
| `void webSend(char buf[])` | Zeichenkette an Webseiten-HTML senden (`WSContentSend`) |
| `void webSend("literal")` | Zeichenketten-Literal an Webseite senden (kein Puffer noetig) |
| `void webFlush()` | Web-Inhaltspuffer zum Client leeren (`WSContentFlush`) |
| `void addLog(char buf[])` | Nachricht ins Tasmota-Log schreiben (`AddLog` auf INFO-Ebene) |
| `void addLog("literal")` | Zeichenketten-Literal ins Tasmota-Log schreiben |
| `webSendJsonArray(float arr[], int count)` | Float-Array als JSON-Integer-Array ausgeben |

**Hinweise:**
- `addLog`, `webSend` und `responseAppend` akzeptieren sowohl ein char-Array als auch ein Zeichenketten-Literal
- Zeichenketten-Literal-Varianten sind effizienter — keine Kopie durch einen Puffer, direkt aus dem Konstantenpool gesendet
- Verwenden Sie `responseAppend()` innerhalb von `JsonCall()` — fuegt an das MQTT-Telemetrie-JSON an
- Verwenden Sie `webSend()` innerhalb von `WebPage()` fuer einmaligen Seiteninhalt (Diagramme, Skripte, benutzerdefiniertes HTML)
- Verwenden Sie `webSend()` innerhalb von `WebCall()` fuer Sensor-aehnliche Zeilen, die periodisch aktualisiert werden
- Verwenden Sie das Format `{s}Beschriftung{m}Wert{e}` in `webSend()` fuer Sensor-aehnliche Tabellenzeilen
- Rufen Sie `webFlush()` periodisch auf, wenn Sie grosse HTML-Seiten erstellen, um den Chunked-Transfer-Puffer zu leeren (500 Bytes)
- Beginnen Sie JSON mit Komma: `",\"Key\":value"` um korrekt an die Telemetrie anzufuegen
- In der Browser-IDE werden beide zur Ausgabekonsole geleitet; `webFlush()` ist eine Leeroperation
- Callback-Instruktionslimit: 200.000 (ESP32), 20.000 (ESP8266)
- Siehe [Callback-Funktionen](#callback-funktionen) fuer vollstaendige Beispiele

### HTTP-Anfragen

HTTP GET/POST-Anfragen an externe APIs stellen. URLs koennen Zeichenketten-Literale oder dynamisch in char-Arrays erstellt sein. Anfragen sind blockierend mit einem 5-Sekunden-Timeout.

| Funktion | Beschreibung |
|----------|-------------|
| `int httpGet(char url[], char response[])` | HTTP GET, gibt Antwortlaenge oder negativen Fehler zurueck |
| `int httpPost(char url[], char data[], char response[])` | HTTP POST, gibt Antwortlaenge oder negativen Fehler zurueck |
| `void httpHeader(char name[], char value[])` | Benutzerdefinierten Header fuer die naechste Anfrage setzen |
| `int webParse(char source[], "delim", int index, char result[])` | Nicht-JSON Antworttext parsen (siehe unten) |

**Rueckgabewerte:** `> 0` = Laenge des Antwortkoerpers, `0` = leere Antwort, negativ = HTTP-Fehlercode (z.B. -404).

**Beispiel — Daikin-Klimaanlage Sensorabfrage:**
```c
char url[64];
char response[256];
char token[32];
int len;
int pos;

void main() {
    strcpy(url, "http://192.168.188.43/aircon/get_sensor_info");
    len = httpGet(url, response);
    // response = "ret=OK,htemp=19.0,hhum=-,otemp=7.0,err=0,cmpfreq=0"

    if (len > 0) {
        // Innentemperatur extrahieren (htemp)
        pos = strFind(response, token);  // "htemp=" finden
        strToken(token, response, ',', 3);  // 3. Token = "htemp=19.0"
        printString(token);
    }
}
```

**Beispiel — Tasmota-Befehl an ein anderes Geraet:**
```c
char url[128];
char response[512];
int len;

void EverySecond() {
    strcpy(url, "http://192.168.1.100/cm?cmnd=Status%200");
    len = httpGet(url, response);
    if (len > 0) {
        print(len);
        // Antwort mit strFind/strToken parsen...
    }
}
```

**Beispiel — POST mit benutzerdefiniertem Header:**
```c
char url[128];
char data[128];
char hname[32];
char hval[64];
char response[512];

void main() {
    strcpy(url, "http://192.168.1.100/api/data");
    strcpy(data, "{\"value\":42}");
    strcpy(hname, "Content-Type");
    strcpy(hval, "application/json");
    httpHeader(hname, hval);  // Header vor Anfrage setzen
    int len = httpPost(url, data, response);
}
```

**webParse() — Nicht-JSON Web-Antworten parsen**

Entspricht Scripters `gwr()`. Extrahiert Daten aus Klartext-HTTP-Antworten (Key=Value, CSV, zeilenbasierte Formate).

**Zwei Modi:**
- **index > 0** — `source` an `delim` aufteilen, das N-te Segment zurueckgeben (1-basiert). Gibt Laenge zurueck.
- **index < 0** — Muster `delim=wert` finden, Wert extrahieren (stoppt bei `,`, `:` oder NUL). Gibt Laenge zurueck.
- **index == 0** — Keine Aktion, gibt 0 zurueck.

**Beispiel — Daikin-Klimaanlage mit webParse:**
```c
char url[64];
char response[256];
char value[32];

void main() {
    strcpy(url, "http://192.168.188.43/aircon/get_sensor_info");
    int len = httpGet(url, response);
    // response = "ret=OK,htemp=19.0,hhum=-,otemp=7.0,err=0,cmpfreq=0"

    if (len > 0) {
        // Name=Wert Modus: Wert nach "htemp=" extrahieren
        webParse(response, "htemp", -1, value);  // value = "19.0"
        float temp = atof(value);
        print(temp);  // 19.0

        // Split Modus: 4. komma-getrenntes Feld holen
        webParse(response, ",", 4, value);  // value = "otemp=7.0"
        printString(value);
    }
}
```

### TCP-Server

Einen TCP-Stream-Server starten, um eingehende Verbindungen anzunehmen. Es wird nur ein Client gleichzeitig bedient.

| Funktion | Beschreibung |
|----------|--------------|
| `int tcpServer(int port)` | TCP-Server auf `port` starten. Gibt 0=ok, -1=Fehler, -2=kein Netzwerk zurueck |
| `tcpClose()` | TCP-Server schliessen und Client trennen |
| `int tcpAvailable()` | Wartenden Client annehmen und verfuegbare Bytes zurueckgeben |
| `int tcpRead(char buf[])` | String vom TCP-Client in `buf` lesen. Gibt gelesene Bytes zurueck |
| `tcpWrite(char str[])` | String an TCP-Client senden |
| `int tcpReadArray(int arr[])` | Verfuegbare Bytes in Int-Array lesen (ein Byte pro Element). Gibt Anzahl zurueck |
| `tcpWriteArray(int arr[], int num)` | `num` Array-Elemente als uint8-Bytes an TCP-Client senden |
| `tcpWriteArray(int arr[], int num, int type)` | Mit Typ senden: 0=uint8, 1=uint16 BE, 2=sint16 BE, 3=float BE |

**Beispiel — Einfacher TCP-Echo-Server:**
```c
char buf[128];

void main() {
    tcpServer(8888);   // auf Port 8888 horchen
}

void Every50ms() {
    int n = tcpAvailable();  // Client annehmen + pruefen
    if (n > 0) {
        tcpRead(buf);        // eingehenden String lesen
        tcpWrite(buf);       // zuruecksenden
    }
}
```

**Beispiel — Binaere Datenuebertragung:**
```c
int data[100];

void main() {
    tcpServer(9000);
}

void EverySecond() {
    int n = tcpAvailable();
    if (n > 0) {
        // Rohdaten in Array lesen
        int count = tcpReadArray(data);
        print(count);
        // als uint16 Big-Endian zuruecksenden
        tcpWriteArray(data, count, 1);
    }
}
```

### mDNS-Dienstankuendigung

Das Geraet als mDNS-Dienst im lokalen Netzwerk registrieren, um Geraeteemulation zu ermoeglichen (Everhome ecotracker, Shelly oder benutzerdefinierte Dienste).

| Funktion | Beschreibung |
|----------|-------------|
| `int mdnsRegister("name", "mac", "type")` | mDNS-Responder starten und Dienst ankuendigen. Gibt 0 bei Erfolg zurueck |

**Parameter (alle Zeichenketten-Literale):**
- **name** — Hostname-Praefix. Verwenden Sie `"-"` fuer Tasmota's Standard-Hostname, oder ein benutzerdefiniertes Praefix (MAC wird automatisch angefuegt)
- **mac** — MAC-Adresse. Verwenden Sie `"-"` fuer die eigene MAC des Geraets (Kleinbuchstaben, ohne Doppelpunkte), oder geben Sie eine benutzerdefinierte Zeichenkette an
- **type** — Diensttyp: `"everhome"` (ecotracker), `"shelly"` oder ein beliebiger benutzerdefinierter Dienstname

**Eingebaute Emulationstypen:**
- `"everhome"` — registriert `_everhome._tcp` mit IP-, Serial-, Productid-TXT-Eintraegen
- `"shelly"` — registriert `_http._tcp` und `_shelly._tcp` mit Firmware-Metadaten-TXT-Eintraegen
- Jede andere Zeichenkette — registriert `_<type>._tcp` mit IP- und Serial-TXT-Eintraegen

**Beispiel — Everhome-Ecotracker-Emulation:**
```c
int main() {
    mdnsRegister("ecotracker-", "-", "everhome");
    return 0;
}
```

Dies entspricht Scripter's `mdnsRegister("ecotracker-", "-", "everhome")`.

### WebUI-Widgets

Interaktive Dashboards mit Widget-Funktionen erstellen. Widgets koennen an zwei Stellen erscheinen:

1. **Dedizierte `/tc_ui`-Seite** — verwenden Sie den `WebUI()`-Callback
2. **Tasmota-Hauptseite** (Sensorbereich) — verwenden Sie den `WebCall()`-Callback

Beide Callbacks verwenden die gleichen Widget-Funktionen.

| Funktion | Beschreibung |
|----------|-------------|
| `webButton(var, "label")` | Umschalt-Schaltflaeche (0/1) — zeigt EIN/AUS, Klick schaltet um |
| `webSlider(var, min, max, "label")` | Bereichsregler — ziehen zum Einstellen des Werts |
| `webCheckbox(var, "label")` | Kontrollkaestchen (0/1) — Aktivieren/Deaktivieren schaltet um |
| `webText(chararray, maxlen, "label")` | Texteingabe — Zeichenkettenvariable bearbeiten |
| `webNumber(var, min, max, "label")` | Zahleneingabe mit Min/Max-Grenzen |
| `webPulldown(var, "label", "opt0\|opt1\|opt2")` | Dropdown-Auswahl mit Beschriftung — Pipe-getrennte Optionen, 0-basierter Index. `"@getfreepins"` als Optionen zeigt verfuegbare GPIO-Pins |
| `webRadio(var, "opt0\|opt1\|opt2")` | Optionsschaltflaechengruppe — Pipe-getrennte Optionen, 0-basierter Index |
| `webTime(var, "label")` | Zeitauswahl (HH:MM) — gespeichert als HHMM-Ganzzahl (z.B. 1430 = 14:30) |
| `webPageLabel(page, "label")` | Seite 0–5 mit einer Schaltflaechenbeschriftung auf der Hauptseite registrieren |
| `int webPage()` | Gibt die aktuelle Seitennummer zurueck, die gerendert wird (in `WebUI()` zur Verzweigung verwenden) |
| `webConsoleButton("/url", "label")` | Schaltflaeche im Tasmota-Utilities-Menue registrieren (max 4). Navigiert zu URL bei Klick |

Das erste Argument der Widget-Funktionen ist immer eine **globale Variable**, die das Widget liest und in die es schreibt. Der Compiler uebergibt automatisch die Adresse der Variable an den Syscall.

**Beispiel — Widgets auf der Hauptseite:**
```c
int relay;
int brightness;

void WebCall() {
    webButton(relay, "Power");
    webSlider(brightness, 0, 100, "Brightness");
}
```

**Beispiel — Mehrere Seiten mit benutzerdefinierten Schaltflaechen:**

Bis zu 6 Seiten koennen mit `webPageLabel()` registriert werden. Jede erstellt eine Schaltflaeche auf der Tasmota-Hauptseite. Verwenden Sie `webPage()` innerhalb von `WebUI()`, um verschiedene Widgets pro Seite zu rendern.

```c
int power;
int brightness;
int mode;
int alarm_time;
char devname[32];

void WebUI() {
    int page = webPage();
    if (page == 0) {
        webButton(power, "Power");
        webSlider(brightness, 0, 100, "Brightness");
        webPulldown(mode, "Mode", "Off|Auto|Manual");
    }
    if (page == 1) {
        webTime(alarm_time, "Wake-up Time");
        webText(devname, 32, "Device Name");
    }
}

int main() {
    webPageLabel(0, "Controls");   // erste Schaltflaeche auf der Hauptseite
    webPageLabel(1, "Settings");   // zweite Schaltflaeche auf der Hauptseite
    return 0;
}
```

Wenn kein `webPageLabel()` aufgerufen wird, aber `WebUI()` existiert, erscheint eine einzelne "TinyC UI"-Schaltflaeche.

**Funktionsweise:**
1. `WebCall()` rendert Widgets im Sensorbereich der Tasmota-Hauptseite
2. `WebUI()` rendert Widgets auf dedizierten Seiten unter `http://<device>/tc_ui?p=N`
3. `webPageLabel(N, "text")` registriert Seite N (0–5) mit einer Schaltflaeche auf der Hauptseite
4. `webPage()` gibt die aktuelle Seitennummer zurueck, damit `WebUI()` verschiedene Widgets anzeigen kann
5. Wenn Sie einen Regler bewegen / eine Schaltflaeche klicken, sendet JavaScript den neuen Wert per AJAX
6. Der Server schreibt den Wert direkt in die TinyC-globale Variable
7. Die Seite aktualisiert sich automatisch, um den aktualisierten Zustand anzuzeigen
8. Text- und Zahleneingaben pausieren die automatische Aktualisierung waehrend der Bearbeitung (wird bei Fokusverlust fortgesetzt)

### WebChart — Automatische Google Charts

`WebChart()` rendert Google Charts auf der Tasmota-Hauptseite mit einem einzigen Funktionsaufruf pro Datenserie. Die Google Charts-Bibliothek und das gesamte JavaScript werden automatisch generiert.

```c
void WebChart(int type, "title", "unit", int color, int pos, int count,
              float array[], int decimals, int interval, float ymin, float ymax)
```

| Parameter | Beschreibung |
|-----------|-------------|
| `type` | Diagrammtyp: `0` = Liniendiagramm, `1` = Saeulendiagramm |
| `"title"` | Diagrammtitel (String-Literal). Leer `""` = Serie zum vorherigen Diagramm hinzufuegen |
| `"unit"` | Y-Achsen-Einheit (String-Literal, z.B. `"°C"`, `"%"`, `"m/s"`) |
| `color` | Linien-/Balkenfarbe als Hex-RGB (z.B. `0xe74c3c` fuer Rot) |
| `pos` | Aktuelle Schreibposition im Ringpuffer |
| `count` | Anzahl gueltiger Datenpunkte (≤ Array-Groesse) |
| `array` | Float-Array mit den Daten (Ringpuffer) |
| `decimals` | Anzahl Dezimalstellen fuer Datenwerte (0–6) |
| `interval` | Minuten zwischen Datenpunkten (fuer X-Achsen-Zeitbeschriftung) |
| `ymin` | Y-Achsen-Minimum. Wenn `ymin >= ymax`, automatische Skalierung |
| `ymax` | Y-Achsen-Maximum. Wenn `ymin >= ymax`, automatische Skalierung |

**Beispiel — 24h Wetterdaten:**
```c
#define NPTS 288       // 24h bei 5-Minuten-Intervallen
persist float h_temp[NPTS];
persist float h_hum[NPTS];
persist int h_pos = 0;
persist int h_count = 0;

void WebPage() {
    if (h_count < 1) return;
    WebChart(0, "Temperatur", "\u00b0C", 0xe74c3c, h_pos, h_count, h_temp, 1, 5, -20, 50);
    WebChart(0, "Luftfeuchte", "%",      0x3498db, h_pos, h_count, h_hum,  1, 5, 0, 100);
}
```

- **Fester Bereich** fuer Daten mit bekannten Grenzen (Luftfeuchte 0–100, UV-Index 0–12)
- **Auto-Skalierung** (`0, 0`) fuer Daten mit variablem Bereich (Helligkeit, Wind, Regen)
- Aufruf aus `WebPage()`-Callback — jeder Aufruf erzeugt eine Datenserie
- Mehrere Serien in einem Diagramm: erster Aufruf hat Titel, weitere verwenden `""` als Titel
- **Benutzerdefinierte Diagrammgroesse:** `webChartSize(width, height)` vor dem ersten `WebChart()`-Aufruf aufrufen, um benutzerdefinierte Diagrammabmessungen in Pixeln festzulegen

**HTML aus Dateien einbinden:**

Verwenden Sie `webSendFile("filename")`, um den Inhalt einer Datei vom Geraetedateisystem direkt an die Webseite zu senden. Dies ist nuetzlich fuer grosses HTML, CSS oder JavaScript, das zu gross waere, um in Bytecode-Konstanten kompiliert zu werden.

```c
void WebPage() {
    webSendFile("chart.html");  // Diagrammbibliothek von /chart.html einbinden
}
```

Die Datei wird in 256-Byte-Stuecken gelesen und per `WSContentSend` gesendet. Der Dateiname kann mit oder ohne fuehrendes `/` angegeben werden.

### Benutzerdefinierte Web-Handler

Benutzerdefinierte HTTP-Endpunkte auf dem Tasmota-Webserver registrieren. Wenn eine Anfrage eintrifft, wird der `WebOn()`-Callback aufgerufen, wobei die Handler-Nummer ueber `webHandler()` zugaenglich ist.

| Funktion | Beschreibung |
|----------|-------------|
| `webOn(int num, "url")` | Handler 1–4 fuer den angegebenen URL-Pfad registrieren |
| `int webHandler()` | Gibt die Handler-Nummer (1–4) innerhalb des `WebOn()`-Callbacks zurueck |
| `int webArg("name", buf)` | HTTP-Anfrageparameter in char-Puffer lesen, gibt Laenge zurueck (0 wenn fehlend) |

Verwenden Sie `webSend(buf)`, um den Antwortkoerper auszugeben. Der Standard-Inhaltstyp der Antwort ist `text/plain`.

**Beispiel — JSON-API-Endpunkt:**
```c
char buf[128];

void WebOn() {
    int h = webHandler();
    if (h == 1) {
        // GET /v1/json?id=xxx
        char id[32];
        int len = webArg("id", id);
        sprintfFloat(buf, "{\"handler\":1,\"id\":\"%s\",\"value\":42}", id);
        webSend(buf);
    }
}

int main() {
    webOn(1, "/v1/json");
    return 0;
}
```

**Beispiel — Mehrere Endpunkte:**
```c
void WebOn() {
    int h = webHandler();
    char buf[64];
    if (h == 1) {
        sprintf(buf, "{\"temp\":%.1f}", smlGet(1));
        webSend(buf);
    }
    if (h == 2) {
        webSend("OK");
    }
}

int main() {
    webOn(1, "/api/sensor");
    webOn(2, "/api/ping");
    return 0;
}
```

**Hinweise:**
- Bis zu 4 Handler koennen registriert werden (1–4)
- URLs muessen mit `/` beginnen (z.B. `/v1/json`, `/api/data`)
- `webOn()` wird in `main()` aufgerufen — Handler werden beim Programmstart registriert
- Der `WebOn()`-Callback laeuft nachdem `main()` zurueckgekehrt ist (wie andere Callbacks)
- `webArg()` liest sowohl GET-Abfrageparameter als auch POST-Formularfelder
- Aequivalent zu Scripter's `won(N, "/url")` + `>onN`-Abschnitt
- CORS ist aktiviert, sodass Endpunkte von externen Anwendungen zugaenglich sind

### UDP-Multicast (Scripter-kompatibel)

Float-Variablen zwischen Tasmota-Geraeten ueber UDP-Multicast auf 239.255.255.250:1999 teilen.
Kompatibel mit Tasmota Scripter's globalem Variablenprotokoll.

| Funktion | Beschreibung |
|----------|-------------|
| `float udpRecv("name")` | Letzten empfangenen Wert fuer benannte Variable abrufen (0 wenn keiner) |
| `int udpReady("name")` | Gibt 1 zurueck wenn neuer Wert seit letzter Pruefung empfangen |
| `void udpSendArray("name", float_arr, count)` | Float-Array per binaeren Multicast senden |
| `int udpRecvArray("name", float_arr, maxcount)` | Float-Array empfangen, gibt tatsaechliche Anzahl zurueck |
| `udpSendStr("name", char str[])` | String über UDP-Multicast senden |

**Protokoll:**
- Einzelner Float: sende `=>name:[4 Bytes IEEE-754 Float]`
- Float-Array: sende `=>name:[2-Byte LE Anzahl][N x 4-Byte Float]`
- Empfang: sowohl ASCII (`=>name=value`) als auch binaer (einzeln oder Array)
- Multicast-Gruppe: `239.255.255.250`, Port `1999`
- Maximal 8 ueberwachte Variablennamen, je 16 Zeichen
- Maximal 64 Floats pro Array

**Callback:** Definieren Sie `void UdpCall()`, um bei jeder empfangenen Variable benachrichtigt zu werden.
Der UDP-Socket wird beim ersten Schreibzugriff auf eine globale Variable, `udpRecv()`- oder `udpReady()`-Aufruf automatisch initialisiert.
Skalare `global` Float-Variablen werden bei Zuweisung automatisch per UDP gesendet (kein expliziter Aufruf noetig).

**Socket-Watchdog:** Der Multicast-Socket hat einen eingebauten Inaktivitaets-Watchdog (Standard: 60 Sekunden). Wenn innerhalb der Timeout-Periode kein Paket empfangen wird, wird der Socket automatisch geschlossen und neu geoeffnet. Dies behebt das bekannte ESP32-Problem, bei dem der UDP-Empfangspfad nach variabler Zeit stillschweigend aufhoert zu funktionieren. Mit `udp(8, 0, sekunden)` kann der Timeout geaendert werden (0 = deaktiviert).

**Beispiel (Skalar — automatischer Broadcast):**
```c
global float temperature = 0.0;  // als 'global' deklariert → sendet automatisch bei Zuweisung

void EverySecond() {
    temperature = 20.0 + sin(counter) * 5.0;
    // Kein udpSend() noetig — Zuweisung an 'global' Variable sendet automatisch
}

void UdpCall() {
    float remote = udpRecv("temperature");
    // Fernwert verarbeiten...
}
```

**Beispiel (Array):**
```c
float sensors[8];

void EverySecond() {
    // 8 Sensorwerte als Array senden
    udpSendArray("sensors", sensors, 8);
}

void UdpCall() {
    float remote[8];
    int n = udpRecvArray("sensors", remote, 8);
    // n = Anzahl der tatsaechlich empfangenen Floats
}
```

### Allgemeine UDP-Funktion

Scripter-kompatible `udp()`-Funktion fuer beliebige UDP-Kommunikation. Verwendet einen separaten Socket von der Multicast-Variablenfreigabe oben.

| Funktion | Beschreibung |
|----------|-------------|
| `int udp(0, int port)` | UDP-Port oeffnen. Gibt 1 bei Erfolg zurueck |
| `int udp(1, char buf[])` | Empfangenen String in buf lesen. Gibt Byteanzahl zurueck (0 = nichts) |
| `void udp(2, char str[])` | Antwort an Absender-IP und -Port senden |
| `void udp(3, char url[], char str[])` | String an url mit dem Port von `udp(0)` senden |
| `int udp(4, char buf[])` | Remote-Absender-IP als String. Gibt Laenge zurueck |
| `int udp(5)` | Remote-Absender-Port zurueckgeben |
| `int udp(6, char url[], int port, char str[])` | String an beliebige url:port senden |
| `int udp(7, char url[], int port, int arr[], int count)` | Array als Rohbytes an url:port senden |
| `int udp(8, int welcher, int sekunden)` | Socket-Inaktivitaets-Timeout setzen (welcher: 0=Multicast, 1=Allgemeiner Port; 0=deaktiviert) |

**Hinweise:**
- Das erste Argument (Modus) muss ein ganzzahliges Literal (0-8) sein
- Modi 6 und 7 erstellen einen temporaeren Socket (kein vorheriges `udp(0)` noetig)
- Modus 1 ist nicht-blockierend: gibt sofort 0 zurueck wenn kein Paket verfuegbar
- Modus 7 sendet das untere Byte jedes Array-Elements
- Modus 8 konfiguriert den Socket-Watchdog: wenn innerhalb von `sekunden` kein Paket empfangen wird, wird der Socket automatisch zurueckgesetzt. Standard ist 60 Sekunden. 0 zum Deaktivieren.

### I2C-Bus

Direkter I2C-Bus-Zugriff fuer Sensortreiber (erfordert `USE_I2C`). Alle Funktionen nehmen `bus` als letzten Parameter (0 oder 1).

| Funktion | Beschreibung |
|----------|-------------|
| `int i2cExists(int addr, int bus)` | Pruefen ob Geraet an Adresse antwortet. Gibt 1 zurueck wenn gefunden |
| `int i2cRead8(int addr, int reg, int bus)` | Einzelnes Byte aus Register lesen. Gibt Bytewert (0–255) zurueck |
| `int i2cWrite8(int addr, int reg, int val, int bus)` | Einzelnes Byte in Register schreiben. Gibt 1=ok, 0=Fehler zurueck |
| `int i2cRead(int addr, int reg, char buf[], int len, int bus)` | `len` Bytes in char-Array lesen. Gibt 1=ok zurueck |
| `int i2cWrite(int addr, int reg, char buf[], int len, int bus)` | `len` Bytes aus char-Array schreiben. Gibt 1=ok zurueck |
| `int i2cRead0(int addr, char buf[], int len, int bus)` | `len` Bytes ohne Register lesen. Gibt 1=ok zurueck |
| `int i2cWrite0(int addr, int reg, int bus)` | Nur Register-Byte schreiben (keine Daten). Gibt 1=ok zurueck |
| `int i2cSetDevice(int addr, int bus)` | Pruefen ob Adresse **nicht belegt** und ansprechbar. Gibt 1=verfuegbar zurueck |
| `i2cSetActiveFound(int addr, "type", int bus)` | Adresse als belegt registrieren. Loggt Erkennung |
| `int i2cReadRS(int addr, int reg, char buf[], int len, int bus)` | I2C-Lesen mit Repeated-Start (SMBus) |
| `I2cResetActive(int addr, int bus)` | Beanspruchte I2C-Adresse freigeben |

**Hinweise:**
- `bus` = 0 oder 1 — waehlt welcher I2C-Bus verwendet wird
- Adresse ist 7-Bit (0x00–0x7F), z.B. `0x48` fuer TMP102
- Register ist 8-Bit (0x00–0xFF)
- Pufferfunktionen verwenden `char[]`-Arrays — jedes Element enthaelt ein Byte (0–255)
- Maximale Pufferlaenge ist 255 Bytes
- Gibt 0 zurueck wenn I2C nicht einkompiliert ist oder die Operation fehlschlaegt
- `i2cSetDevice` + `i2cSetActiveFound` verwenden, um I2C-Adressen korrekt zu beanspruchen und Konflikte mit Tasmota-Treibern zu vermeiden

**Beispiel — TMP102-Temperatursensor auf Bus 0 lesen:**
```c
#define TMP102_ADDR  0x48
#define TMP102_TEMP  0x00
#define I2C_BUS      0

void EverySecond() {
    if (!i2cExists(TMP102_ADDR, I2C_BUS)) return;

    char buf[2];
    if (i2cRead(TMP102_ADDR, TMP102_TEMP, buf, 2, I2C_BUS)) {
        // TMP102: 12-Bit Temperatur in oberen Bits von 2 Bytes
        int raw = (buf[0] << 4) | (buf[1] >> 4);
        if (raw > 2047) raw = raw - 4096;  // Vorzeichenerweiterung
        float temp = (float)raw * 0.0625;

        char out[64];
        sprintfFloat(out, "TMP102: %.2f °C\n", temp);
        printString(out);
    }
}
```

### Smart Meter (SML)

Zaehlerstaende auslesen und Zaehler ueber Tasmota's SML-Treiber steuern (erfordert `USE_SML` oder `USE_SML_M`).

SML kann **ohne Scripter** laufen — nur `USE_UFILESYS` wird fuer dateibasierte Zaehlerbeschreibungen benoetigt.
Der SML-Deskriptor-Tab der IDE verwaltet die Zaehlerdefinitionsdatei (`/sml_meter.def`) auf dem Geraet.

#### Zaehlerstaende lesen

| Funktion | Beschreibung |
|----------|-------------|
| `float smlGet(int index)` | Zaehlerwert abrufen. Index 0 gibt Anzahl zurueck, 1..N gibt Werte zurueck |
| `int smlGetStr(int index, char buf[])` | Zaehler-ID-Zeichenkette in Puffer abrufen, gibt Laenge zurueck |

**Hinweise:**
- Index ist 1-basiert: `smlGet(1)` gibt den ersten Zaehlerwert zurueck
- `smlGet(0)` gibt die Gesamtzahl der Zaehlervariablen zurueck
- Gibt 0 zurueck wenn SML nicht einkompiliert ist oder der Index ausserhalb des Bereichs liegt
- Die Werte sind dieselben wie Scripter's `sml[x]`-Syntax

**Beispiel:**
```c
void WebCall() {
    char buf[64];
    int n = smlGet(0);  // Gesamtzaehler
    int i = 1;
    while (i <= n) {
        float val = smlGet(i);
        sprintfFloat(buf, "{s}Meter %d{m}%.2f{e}", val);
        webSend(buf);
        i++;
    }
}
```

#### Erweiterte Zaehlersteuerung

Diese Funktionen erfordern, dass `USE_SML_SCRIPT_CMD` in der Firmware aktiviert ist.

| Funktion | Beschreibung |
|----------|-------------|
| `int smlWrite(int meter, char buf[])` | Hex-Sequenz an Zaehler senden (z.B. Aufweck- oder Anfragebefehle) |
| `int smlWrite(int meter, "hex")` | Dasselbe, mit Zeichenketten-Literal (kein temporaerer Puffer noetig) |
| `int smlRead(int meter, char buf[])` | Rohen Zaehlerpuffer in char-Array lesen, gibt gelesene Bytes zurueck |
| `int smlSetBaud(int meter, int baud)` | Baudrate des seriellen Ports eines Zaehlers aendern |
| `int smlSetWStr(int meter, char buf[])` | Asynchrone Schreibzeichenkette fuer naechsten geplanten Sendevorgang setzen |
| `int smlSetWStr(int meter, "hex")` | Dasselbe, mit Zeichenketten-Literal |
| `int smlSetOptions(int options)` | Globale SML-Optionen-Bitmaske setzen |
| `int smlGetV(int sel)` | Daten-Gueltigkeitsflags abrufen/zuruecksetzen (0=abrufen, 1=zuruecksetzen) |

**Hinweise:**
- `meter` ist der 1-basierte Zaehlerindex aus dem SML-Deskriptor
- `smlWrite` und `smlSetWStr` akzeptieren entweder ein `char[]`-Array oder ein Zeichenketten-Literal — der Compiler erkennt automatisch welche Variante verwendet wird
- `smlWrite` sendet eine hex-kodierte Bytesequenz (z.B. `"AA0100"`) an den seriellen Port des Zaehlers
- `smlRead` kopiert den rohen Empfangspuffer in ein char-Array fuer benutzerdefiniertes Parsen
- `smlSetBaud` aendert dynamisch die Baudrate des Zaehlers (nuetzlich fuer Zaehler, die Geschwindigkeitsverhandlung erfordern)
- `smlSetWStr` setzt eine Hex-Zeichenkette, die beim naechsten geplanten Zaehlerabfragezyklus gesendet wird
- Diese Funktionen ersetzen Scripter's `>F`/`>S`-Abschnitt-Zaehlersteuerungsbefehle

**Beispiel — OBIS-Zaehler-Aufwecksequenz:**
```c
void EverySecond() {
    // Zeichenketten-Literal — kein temporaerer Puffer noetig
    smlWrite(1, "2F3F210D0A");  // "/?!\r\n" in Hex
}
```

**Beispiel — Dynamische Baudratenverhandlung:**
```c
void EverySecond() {
    // Zaehlerantwort lesen
    char buf[64];
    int n = smlRead(1, buf);
    if (n > 0 && buf[0] == 0x06) {
        // ACK empfangen, auf hohe Geschwindigkeit umschalten
        smlSetBaud(1, 9600);
    }
}
```

#### SML-Deskriptor-Editor (IDE)

Die IDE enthaelt einen **SML-Deskriptor**-Tab im linken Bereich zur Verwaltung von Zaehlerdefinitionen:

- **Zaehler-Datenbank**: Ein Dropdown laedt `.tas`-Zaehlerdefinitionen aus der [Community-Datenbank](https://github.com/ottelo9/tasmota-sml-script)
- **Benutzerdefinierte Zaehler-URL**: Die Datenbank-URL wird aus `/sml_meter_url.txt` auf dem Geraetedateisystem gelesen. Um ein anderes Zaehler-Repository zu verwenden, bearbeiten Sie diese Datei mit einer URL, die auf ein Verzeichnis mit einer `smartmeter.json`-Indexdatei zeigt. Die Standard-URL verweist auf das Community-GitHub-Repository.
- **RX/TX-Pin-Auswahl**: Dropdowns werden aus den freien GPIOs des Geraets befuellt (ueber `freegpio`-API)
- **Pin-Platzhalter**: `%0rxpin%` und `%0txpin%` in Deskriptoren werden beim Speichern durch die ausgewaehlten Pins ersetzt
- **Auf Geraet speichern**: Extrahiert nur den `>M`-Abschnitt und speichert ihn als `/sml_meter.def`
- **Von Geraet laden**: Liest die aktuelle `/sml_meter.def` vom Geraet

#### Callback-Zusammenfuehrung

Viele `.tas`-Zaehlerdateien erfordern periodischen Code (Scripter's `>S`- und `>F`-Abschnitte) fuer Zaehlerkommunikation, Aufwecksequenzen oder Baudratenverhandlung. In TinyC schreiben Sie diese direkt als Callback-Funktionen im SML-Editor:

```
void EverySecond() {
    smlWrite(1, "2F3F210D0A");
}

>M 1
+1,3,s,16,9600,SML,1
1,1-0:1.8.0*255(@1,Energy In,kWh,E_in,3
#
```

**Funktionsweise:**
1. Schreiben Sie TinyC-Callback-Funktionen (`EverySecond()`, `Every100ms()` usw.) an beliebiger Stelle im SML-Editor — vor oder nach dem `>M`-Abschnitt
2. Beim **Speichern** geht nur der `>M`-Abschnitt als `/sml_meter.def` auf das Geraet
3. Beim **Kompilieren** fuehrt die IDE automatisch SML-Callbacks in das Hauptprogramm zusammen:
   - Wenn der Haupteditor bereits denselben Callback hat — wird der SML-Code an den bestehenden Funktionsrumpf angefuegt
   - Wenn der Haupteditor ihn nicht hat — wird eine neue Callback-Funktion erstellt
4. Der zusammengefuehrte Quellcode wird als ein Programm kompiliert — SML-Code und Hauptcode teilen sich dieselben Globalen und Funktionen

### SPI-Bus

Direkter SPI-Bus-Zugriff fuer Sensoren und Displays. Unterstuetzt sowohl Hardware-SPI (unter Verwendung der von Tasmota konfigurierten Pins) als auch Software-Bitbang auf beliebigen GPIO-Pins.

| Funktion | Beschreibung |
|----------|-------------|
| `int spiInit(int sclk, int mosi, int miso, int speed_mhz)` | SPI-Bus initialisieren. Gibt 1=ok zurueck |
| `spiSetCS(int index, int pin)` | Chip-Select-Pin fuer Slot-Index (1–4) setzen |
| `int spiTransfer(int cs, char buf[], int len, int mode)` | Bytes uebertragen. Gibt uebertragene Bytes zurueck |

**`spiInit`-Pin-Modi:**
- `sclk = -1` — Tasmota's primaeren Hardware-SPI-Bus verwenden (GPIO in Tasmota konfiguriert)
- `sclk = -2` — HSPI sekundaeren Hardware-SPI-Bus verwenden (nur ESP32)
- `sclk >= 0` — Bitbang-Modus mit GPIO-Pins (`sclk`, `mosi`, `miso`)
- Setzen Sie `mosi` oder `miso` auf -1, wenn nicht benoetigt (z.B. Nur-Lesen- oder Nur-Schreiben-Geraet)
- `speed_mhz` setzt die Taktfrequenz fuer Hardware-SPI (wird fuer Bitbang ignoriert)

**`spiTransfer`-Modi:**
| Modus | Beschreibung |
|-------|-------------|
| 1 | 8-Bit pro Element — jedes `buf[]`-Element = 1 uebertragenes Byte |
| 2 | 16-Bit pro Element — jedes `buf[]`-Element = 2 Bytes (MSB zuerst) |
| 3 | 24-Bit pro Element — jedes `buf[]`-Element = 3 Bytes (MSB zuerst) |
| 4 | 8-Bit mit CS-Umschaltung pro Byte — CS geht fuer jedes Byte Low/High |

**Hinweise:**
- Der `cs`-Parameter ist ein 1-basierter CS-Slot-Index (entsprechend `spiSetCS`). Verwenden Sie 0 fuer keine automatische CS-Verwaltung
- Die Uebertragung ist Vollduplex: `buf[]` wird geschrieben (MOSI) und gelesene Werte (MISO) ersetzen jedes Element
- Die maximale praktische Uebertragungslaenge ist durch Ihre char-Array-Groesse begrenzt
- SPI-Ressourcen werden automatisch bereinigt, wenn die VM stoppt
- Hardware-SPI erfordert in Tasmota konfigurierte SPI-Pins (Template- oder Modul-Einstellungen)

**Beispiel — MAX31855-Thermoelement lesen (SPI, 32-Bit-Lesung):**
```c
#define CS_PIN  5

int main() {
    spiInit(-1, -1, -1, 4);   // HW-SPI bei 4 MHz
    spiSetCS(1, CS_PIN);       // CS-Slot 1 = Pin 5

    char buf[4];
    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
    spiTransfer(1, buf, 4, 1); // 4 Bytes lesen

    // MAX31855: Bits 31..18 = 14-Bit Thermoelement-Temperatur
    int raw = ((buf[0] << 8) | buf[1]) >> 2;
    if (raw & 0x2000) raw = raw - 16384;  // Vorzeichenerweiterung
    float temp = (float)raw * 0.25;

    char out[64];
    sprintfFloat(out, "Thermocouple: %.2f °C\n", temp);
    printString(out);
    return 0;
}
```

### Display-Zeichnung

Erfordert einen Tasmota-Build mit aktiviertem `USE_DISPLAY` und einem konfigurierten Display-Treiber. Alle Zeichenfunktionen arbeiten direkt auf dem Tasmota-Display-Renderer — viel effizienter als das Erstellen von DisplayText-Befehlszeichenketten.

#### Einrichtung & Steuerung

| Funktion | Beschreibung |
|----------|-------------|
| `dspClear()` | Display loeschen, Position auf (0,0) zuruecksetzen |
| `dspPos(x, y)` | Aktuelle Zeichenposition setzen (Pixel) |
| `dspFont(f)` | Schriftart setzen (0-7), setzt Textgroesse auf 1 fuer Nicht-GFX-Schriften zurueck |
| `dspSize(s)` | Textgroessen-Multiplikator setzen |
| `dspColor(fg, bg)` | Vordergrund- und Hintergrundfarbe setzen (16-Bit RGB565) |
| `dspPad(n)` | Text-Auffuellung fuer `dspDraw()` setzen: positiv = linksbuendig aufgefuellt auf n Zeichen, negativ = rechtsbuendig aufgefuellt auf n Zeichen, 0 = aus |
| `dspDim(val)` | Display-Helligkeit setzen (0-15) |
| `dspOnOff(on)` | Display ein- (1) oder ausschalten (0) |
| `dspUpdate()` | Display-Aktualisierung erzwingen (erforderlich fuer E-Paper-Displays) |
| `dspWidth()` | Gibt Display-Breite in Pixeln zurueck |
| `dspHeight()` | Gibt Display-Hoehe in Pixeln zurueck |

#### Zeichenprimitiven

Alle Primitiven verwenden die aktuelle Position, die durch `dspPos()` gesetzt wurde, und die aktuelle Vordergrundfarbe, die durch `dspColor()` gesetzt wurde.

| Funktion | Beschreibung |
|----------|-------------|
| `dspDraw(buf)` | Textzeichenkette an aktueller Position zeichnen |
| `dspPixel(x, y)` | Einzelnen Pixel an (x,y) zeichnen |
| `dspLine(x1, y1)` | Linie von aktueller Position zu (x1,y1) zeichnen, aktualisiert Position |
| `dspHLine(w)` | Horizontale Linie von aktueller Position, Breite w, aktualisiert Position |
| `dspVLine(h)` | Vertikale Linie von aktueller Position, Hoehe h, aktualisiert Position |
| `dspRect(w, h)` | Rechteckumriss an aktueller Position zeichnen |
| `dspFillRect(w, h)` | Gefuelltes Rechteck an aktueller Position zeichnen |
| `dspCircle(r)` | Kreisumriss an aktueller Position mit Radius r zeichnen |
| `dspFillCircle(r)` | Gefuellten Kreis an aktueller Position zeichnen |
| `dspRoundRect(w, h, r)` | Abgerundetes Rechteck an aktueller Position mit Eckenradius r |
| `dspFillRoundRect(w, h, r)` | Gefuelltes abgerundetes Rechteck |
| `dspTriangle(x1, y1, x2, y2)` | Dreieck von aktueller Position zu (x1,y1) und (x2,y2) |
| `dspFillTriangle(x1, y1, x2, y2)` | Gefuelltes Dreieck |

#### Bild & Rohbefehle

| Funktion | Beschreibung |
|----------|-------------|
| `dspPicture("file.jpg", scale)` | Bilddatei vom Dateisystem an aktueller Position zeichnen (scale: 0=Original) |
| `int dspLoadImage("file.jpg")` | JPG in PSRAM als RGB565-Pixelspeicher laden, gibt Slot 0-3 zurueck (-1 bei Fehler). Bleibt im Speicher bis VM stoppt. Nur ESP32+JPEG_PICTS |
| `dspPushImageRect(slot, sx, sy, dx, dy, w, h)` | Teilrechteck aus geladenem Bild auf Bildschirm zeichnen. Liest aus Bild bei (sx,sy), schreibt auf Bildschirm bei (dx,dy), Groesse w×h. Fuer Hintergrund-Wiederherstellung (z.B. Uhrzeiger ueber Zifferblatt) |
| `int dspImageWidth(slot)` | Breite des geladenen Bildes im Slot abfragen (0 bei ungueltigem Slot) |
| `int dspImageHeight(slot)` | Hoehe des geladenen Bildes im Slot abfragen (0 bei ungueltigem Slot) |
| `dspText(buf)` | Rohen DisplayText-Befehl ausfuehren (z.B. `"[z][x50][y20]Hello"`) |

#### Vordefinierte Farbkonstanten (RGB565)

Die folgenden Farbkonstanten sind **vordefiniert** — kein `#define` noetig:

| Konstante | Wert | Konstante | Wert |
|-----------|------|-----------|------|
| `BLACK` | 0 | `WHITE` | 65535 |
| `RED` | 63488 | `GREEN` | 2016 |
| `BLUE` | 31 | `YELLOW` | 65504 |
| `CYAN` | 2047 | `MAGENTA` | 63519 |
| `ORANGE` | 64800 | `PURPLE` | 30735 |
| `GREY` | 33808 | `DARKGREY` | 21130 |
| `LIGHTGREY` | 50712 | `DARKGREEN` | 992 |
| `NAVY` | 16 | `MAROON` | 32768 |
| `OLIVE` | 33792 | | |

Benutzerdefinierte `#define`-Ueberschreibungen haben Vorrang vor vordefinierten Farben.

#### Beispiel

```c
int counter;
char buf[32];

void EverySecond() {
    counter++;

    dspClear();
    dspColor(WHITE, BLACK);    // Weiss auf Schwarz

    // Titel
    dspFont(2);
    dspSize(2);
    dspPos(10, 10);
    dspDraw("TinyC Display");

    // Zaehler
    dspFont(1);
    dspSize(1);
    sprintfInt(buf, "Count: %d", counter);
    dspPos(10, 60);
    dspDraw(buf);

    // Roten Rahmen um den Zaehler zeichnen
    dspColor(RED, BLACK);
    dspPos(5, 55);
    dspRect(150, 25);

    // Blauen gefuellten Kreis zeichnen
    dspColor(BLUE, BLACK);
    dspPos(200, 80);
    dspFillCircle(20);

    dspUpdate();  // noetig fuer E-Paper
}

int main() {
    counter = 0;
    dspClear();
    return 0;
}
```

### Touch-Buttons & Slider

GFX-Touch-Buttons und Slider auf dem Display erstellen. Diese Komfortfunktionen formatieren intern `[b...]` und `[bs...]` DisplayText-Befehle.

#### Button-Erstellung

| Funktion | Beschreibung |
|----------|-------------|
| `dspButton(num, x, y, w, h, oc, fc, tc, ts, "text")` | Power-Button erstellen (steuert Relais `num`) |
| `dspTButton(num, x, y, w, h, oc, fc, tc, ts, "text")` | Virtuellen Toggle-Button erstellen (MQTT TBT) |
| `dspPButton(num, x, y, w, h, oc, fc, tc, ts, "text")` | Virtuellen Push-Button erstellen (MQTT PBT) |
| `dspSlider(num, x, y, w, h, nelem, bg, fc, bc)` | Slider erstellen |

Parameter: `num` = Button-Index (0-15), `x,y` = Position, `w,h` = Groesse, `oc` = Umrissfarbe, `fc` = Fuellfarbe, `tc` = Textfarbe, `ts` = Textgroesse, `nelem` = Slider-Segmente, `bg` = Hintergrundfarbe, `bc` = Balkenfarbe.

#### Zustand setzen & lesen

| Funktion | Beschreibung |
|----------|-------------|
| `dspButtonState(num, val)` | Button-Zustand setzen (0/1) oder Slider-Wert (0-100) |
| `int touchButton(num)` | Button-Zustand lesen: 0/1 fuer Buttons, -1 wenn undefiniert |
| `dspButtonDel(num)` | Button/Slider `num` loeschen, oder alle wenn `num` = -1 |

#### Touch-Callback

Der `TouchButton`-Callback wird bei Touch-Ereignissen mit Button-Index und Wert aufgerufen:

```c
void TouchButton(int btn, int val) {
    if (btn == 0) {
        // Toggle-Button gedrueckt, val = 0 oder 1
        char buf[16];
        sprintfInt(buf, "%d", val);
        tasmCmd("Power1", buf);
    }
    if (btn == 1) {
        // Slider bewegt, val = 0-100
        char buf[16];
        sprintfInt(buf, "%d", val);
        tasmCmd("Dimmer", buf);
    }
}

int main() {
    dspTButton(0, 10, 10, 100, 50, WHITE, BLUE, WHITE, 2, "Light");
    dspSlider(1, 10, 80, 200, 40, 10, DARKGREY, WHITE, CYAN);
    return 0;
}
```

### Audio

| Funktion | Beschreibung |
|---|---|
| `audioVol(int vol)` | Audiolautstaerke setzen (0-100) |
| `audioPlay("file.mp3")` | MP3-Datei vom Dateisystem abspielen |
| `audioSay("hello")` | Text-zu-Sprache-Ausgabe |

Erfordert einen auf dem Geraet konfigurierten I2S-Audiotreiber.

```c
audioVol(50);              // Lautstaerke auf 50% setzen
audioPlay("/alarm.mp3");   // MP3-Datei abspielen
audioSay("sensor alert");  // Text sprechen
```

### Persistente Variablen

| Funktion | Beschreibung |
|---|---|
| `saveVars()` | Alle `persist` Globals in die `.pvs`-Datei des Programms speichern |

Persist-Variablen werden automatisch beim Programmstart geladen und beim `TinyCStop` gespeichert. Verwenden Sie `saveVars()` um an kritischen Stellen zu speichern (z.B. nach Mitternachts-Zaehleraktualisierung).

### Watch-Variablen (Aenderungserkennung)

| Funktion | Beschreibung |
|---|---|
| `changed(var)` | Gibt 1 zurueck wenn Watch-Variable vom Schattenwert abweicht |
| `delta(var)` | Gibt aktuell - Schatten zurueck (int oder float je nach Variablentyp) |
| `written(var)` | Gibt 1 zurueck wenn Variable seit letztem `snapshot()` zugewiesen wurde |
| `snapshot(var)` | Schattenwert aktualisieren und Written-Flag loeschen |

Watch-Variablen sind Compiler-Intrinsics — sie erzeugen Inline-Vergleichscode ohne Laufzeit-Overhead (kein Syscall).

### Deep Sleep (ESP32)

| Funktion | Beschreibung |
|---|---|
| `deepSleep(int sekunden)` | Tiefschlaf mit Timer-Aufwachen nach `sekunden` |
| `deepSleepGpio(int sekunden, int pin, int level)` | Tiefschlaf mit Timer + GPIO-Aufwachen (0=low, 1=high) |
| `int wakeupCause()` | Gibt ESP32-Aufwachgrund zurueck (0=Reset, 2=EXT0, 3=EXT1, 4=Timer, ...) |

Persistente Variablen und Einstellungen werden vor dem Tiefschlaf automatisch gespeichert.

```c
// Alle 5 Minuten aufwachen zum Sensor-Ablesen
int cause = wakeupCause();
if (cause == 4) {
    // Timer-Aufwachen — Sensor lesen, Daten senden
}
deepSleep(300);  // 300 Sekunden schlafen

// Schlafen bis GPIO12 HIGH wird (oder max 1 Stunde)
deepSleepGpio(3600, 12, 1);
```

### Hardware-Register

Direkt auf Peripherie-Register des ESP32 zugreifen (fuer Low-Level-Treiber und Debugging).

| Funktion | Beschreibung |
|----------|-------------|
| `int peekReg(int addr)` | 32-Bit-Wert aus Peripherie-Register lesen |
| `pokeReg(int addr, int val)` | 32-Bit-Wert in Peripherie-Register schreiben |

### Email (ESP32 — benoetigt USE_SENDMAIL)

| Funktion | Beschreibung |
|---|---|
| `mailBody(body)` | E-Mail-Text setzen (HTML). `body` ist ein `char[]`-Array |
| `mailAttach("/pfad")` | Dateianhang vom Dateisystem hinzufuegen (String-Literal, bis zu 8) |
| `int mailSend(params)` | E-Mail senden. `params` ist `char[]` mit `[server:port:user:passwd:von:an:betreff]`. Gibt 0=ok zurueck |

Fuer einfache E-Mails ohne Anhaenge den Text nach `]` in params setzen:
```c
char cmd[200];
strcpy(cmd, "[smtp.gmail.com:465:user:pass:von@x.com:an@y.com:Alarm] Sensor ausgeloest!");
int result = mailSend(cmd);
```

Fuer E-Mails mit Dateianhaengen `mailBody()` und `mailAttach()` vor `mailSend()` verwenden:
```c
// Text aufbauen
char body[200];
sprintfStr(body, "<h1>Tagesbericht</h1><p>Temperatur: %d C</p>", "%.1f");

// Text und Anhaenge registrieren
mailBody(body);
mailAttach("/daten.csv");
mailAttach("/log.txt");

// Senden — params nur [server:port:user:passwd:von:an:betreff]
char params[200];
strcpy(params, "[*:*:*:*:*:an@example.com:Tagesbericht]");
int result = mailSend(params);
// result: 0=ok, 1=Parse-Fehler, 4=Speicherfehler
```

`*` fuer Server/Port/User/Passwort/Von-Felder verwenden um `#define`-Standardwerte aus `user_config_override.h` zu nutzen.

### Tesla Powerwall (ESP32 — benoetigt TESLA_POWERWALL)

Zugriff auf die lokale Tesla Powerwall API ueber HTTPS. Verwendet die SSL-Implementierung der E-Mail-Bibliothek (Standard-Arduino-SSL funktioniert nicht mit Powerwall).

**Benoetigt:** `#define TESLA_POWERWALL` in `user_config_override.h` und die ESP-Mail-Client-Bibliothek.

| Funktion | Beschreibung |
|----------|-------------|
| `int pwlRequest(url)` | Konfigurationsbefehl oder API-Anfrage. Gibt 0=ok, -1=Fehler zurueck |
| `pwlBind(&var, pfad)` | Globale Float-Variable fuer Auto-Fill registrieren. Pfad mit `#`-Trenner (max 24 Bindings) |
| `float pwlGet(pfad)` | Float-Wert aus letzter Antwort. Unterstuetzt `[N]`-Suffix fuer N-tes Vorkommen |
| `int pwlStr(pfad, buf)` | String aus letzter Antwort in `char[]`-Puffer extrahieren. Gibt Laenge zurueck |

**Empfohlener Ansatz — `pwlBind` (einmal parsen, alle fuellen):**

Globale Variablen mit JSON-Pfaden in `Setup()` registrieren. Wenn `pwlRequest()` eine Antwort erhaelt, wird JSON **einmal** geparst und alle passenden gebundenen Variablen direkt gefuellt. Keine String-Ersetzungen, kein wiederholtes Parsen.

```c
float sip, sop, bip, hip, pwl, rper;

void Setup() {
    pwlRequest("@D192.168.188.60,email@example.com,meinpasswort");
    pwlRequest("@C0x000004714B006CCD,0x000004714B007969");

    // Bindings registrieren — originale JSON-Schluesselnamen verwenden
    pwlBind(&sip, "site#instant_power");
    pwlBind(&sop, "solar#instant_power");
    pwlBind(&bip, "battery#instant_power");
    pwlBind(&hip, "load#instant_power");
    pwlBind(&pwl, "percentage");
    pwlBind(&rper, "backup_reserve_percent");
}

void Loop() {
    // Alle passenden Bindings werden automatisch gefuellt:
    pwlRequest("/api/meters/aggregates");
    // sip, sop, bip, hip sind jetzt gesetzt

    pwlRequest("/api/system_status/soe");
    // pwl ist jetzt gesetzt

    pwlRequest("/api/operation");
    // rper ist jetzt gesetzt
}
```

**Konfigurations-Praefixe:**
| Praefix | Beschreibung |
|---------|-------------|
| `@Dip,email,passwort` | IP und Zugangsdaten konfigurieren |
| `@Ccts1,cts2` | CTS-Seriennummern konfigurieren (werden in Antworten maskiert) |
| `@N` | Auth-Cookie loeschen (erneute Authentifizierung erzwingen) |

**Haeufige API-Endpunkte:**
| Endpunkt | Daten |
|----------|-------|
| `/api/meters/aggregates` | Netz-, Batterie-, Haus-, Solarleistung (W) |
| `/api/system_status/soe` | Ladestand / Batterieprozent |
| `/api/system_status` | Systemstatus-Informationen |
| `/api/operation` | Betriebsmodus, Reserveprozent |
| `/api/meters/readings` | Detaillierte Zaehlerablesung pro CTS |

**N-tes Vorkommen:** `pwlGet("key[N]")` extrahiert das N-te Vorkommen eines wiederholten Schluessels aus der JSON-Antwort. Nuetzlich fuer `/api/meters/readings` mit mehreren CTS-Objekten mit gleichen Schluesselnamen:

```c
// Netzphasen — CTS2 Grid-Phasen sind Vorkommen 6,7,8 von "p_W"
phs1 = pwlGet("p_W[6]");
phs2 = pwlGet("p_W[7]");
phs3 = pwlGet("p_W[8]");
```

**Ad-hoc-Zugriff:** `pwlGet()` und `pwlStr()` stehen fuer einmalige Wertextraktion zur Verfuegung, aber `pwlBind()` wird fuer wiederholtes Polling bevorzugt, da es erneutes Parsen vermeidet.

### Adressierbare LED-Streifen (WS2812 — benoetigt USE_WS2812)

WS2812 / NeoPixel adressierbare LED-Streifen direkt aus TinyC steuern.

**Benoetigt:** `#define USE_WS2812` in `user_config_override.h`.

| Funktion | Beschreibung |
|----------|-------------|
| `setPixels(array, len, offset)` | Setzt `len` Pixel aus `array`, ab Strip-Position `offset & 0x7FF`. Aktualisiert den Strip sofort. |

**Farbformat:** Jedes Array-Element ist `0xRRGGBB` (24-Bit RGB als Integer gepackt).

**RGBW-Modus:** Bit 12 von offset setzen (`offset | 0x1000`) fuer RGBW-Modus. Im RGBW-Modus kodieren zwei aufeinanderfolgende Array-Elemente ein Pixel (High-Word = `0x00RG`, Low-Word = `0xBW00`).

**Beispiel — Regenbogen-Effekt:**
```c
int leds[60];

void setup() {
    for (int i = 0; i < 60; i++) {
        int hue = (i * 256) / 60;
        leds[i] = hueToRGB(hue);
    }
    setPixels(leds, 60, 0);
}

int hueToRGB(int h) {
    int r, g, b;
    int region = h / 43;
    int remainder = (h - region * 43) * 6;
    switch (region) {
        case 0:  r = 255; g = remainder; b = 0; break;
        case 1:  r = 255 - remainder; g = 255; b = 0; break;
        case 2:  r = 0; g = 255; b = remainder; break;
        case 3:  r = 0; g = 255 - remainder; b = 255; break;
        case 4:  r = remainder; g = 0; b = 255; break;
        default: r = 255; g = 0; b = 255 - remainder; break;
    }
    return (r << 16) | (g << 8) | b;
}
```

---

### ESP Kamera (ESP32)

Kamera-Unterstuetzung fuer ESP32-Boards mit OV2640/OV3660/OV5640-Sensoren. Zwei Modi verfuegbar:

- **Tasmota Webcam-Treiber** (sel 0-7): Verwendet den Standard `USE_WEBCAM` Treiber. `USE_WEBCAM` in `user_config_override.h` definieren.
- **TinyC integrierte Kamera** (sel 8-18): Direkter esp_camera-Treiber mit boardspezifischen Pins, MJPEG-Streaming auf Port 81 und PSRAM-Slot-Verwaltung. `USE_TINYC_CAMERA` definieren (via `-DTINYC_CAMERA` Build-Flag). Keine `USE_WEBCAM` Abhaengigkeit.

Beide Modi unterstuetzen `mailAttachPic()` fuer E-Mail-Bildanhaenge (bis zu 4 Bilder pro E-Mail).

#### Kamera-Init mit eigenen Pins (TinyC integrierter Modus)

```c
// Pin-Reihenfolge: pwdn, reset, xclk, sda, scl, d7..d0, vsync, href, pclk
int campins[] = {-1, -1, 15, 4, 5, 16, 17, 18, 12, 10, 8, 9, 11, 6, 7, 13};
int ok = cameraInit(campins, PIXFORMAT_JPEG, FRAMESIZE_VGA, 12, 0, 0, -1);
```

| Funktion | Beschreibung |
|----------|-------------|
| `cameraInit(pins[], format, framesize, quality, fb_count, grab_mode, xclk_freq)` | Kamera mit Pin-Array initialisieren. Gibt 0=ok, sonst Fehler zurueck. `fb_count`=0 auto, `grab_mode`=0 auto, `xclk_freq`=-1 Standard 20MHz. |

#### Kamerasteuerung (camControl)

Alle Kamera-Operationen nutzen `camControl(sel, p1, p2)`:

**Tasmota Webcam-Treiber (sel 0-7, benoetigt USE_WEBCAM):**

| sel | Funktion | Beschreibung |
|-----|----------|-------------|
| 0 | `camControl(0, resolution, 0)` | Init ueber Tasmota-Treiber (WcSetup) |
| 1 | `camControl(1, bufnum, 0)` | Bild in Tasmota Pic-Buffer aufnehmen (1-4) |
| 2 | `camControl(2, option, wert)` | Optionen setzen (WcSetOptions) |
| 3 | `camControl(3, 0, 0)` | Breite abfragen |
| 4 | `camControl(4, 0, 0)` | Hoehe abfragen |
| 5 | `camControl(5, on_off, 0)` | Tasmota Stream-Server starten/stoppen |
| 6 | `camControl(6, param, 0)` | Bewegungserkennung (-1=Bewegung lesen, -2=Helligkeit lesen, ms=Intervall) |

**TinyC integrierte Kamera (sel 7-18, benoetigt USE_WEBCAM oder USE_TINYC_CAMERA):**

| sel | Funktion | Beschreibung |
|-----|----------|-------------|
| 7 | `camControl(7, bufnum, dateiHandle)` | Bild-Buffer in Datei speichern, gibt Bytes zurueck |
| 8 | `camControl(8, 0, 0)` | Sensor-PID abfragen (z.B. 0x2642 = OV2640, 0x3660 = OV3660) |
| 9 | `camControl(9, param, wert)` | Sensor-Parameter setzen (siehe Tabelle) |
| 10 | `camControl(10, slot, 0)` | Bild in PSRAM-Slot aufnehmen (1-4), gibt JPEG-Groesse zurueck |
| 11 | `camControl(11, slot, dateiHandle)` | PSRAM-Slot in Datei speichern, gibt Bytes zurueck |
| 12 | `camControl(12, slot, 0)` | PSRAM-Slot freigeben (0 = alle Slots) |
| 13 | `camControl(13, 0, 0)` | Kamera deinitialisieren + alle Slots + Stream stoppen |
| 14 | `camControl(14, slot, 0)` | Slot-Groesse in Bytes abfragen (0 wenn leer) |
| 15 | `camControl(15, on_off, 0)` | MJPEG Stream-Server auf Port 81 starten/stoppen |
| 16 | `camControl(16, intervall_ms, schwelle)` | Bewegungserkennung aktivieren (0=deaktivieren) |
| 17 | `camControl(17, sel, 0)` | Bewegungswert: 0=Trigger, 1=Helligkeit, 2=ausgeloest, 3=Intervall |
| 18 | `camControl(18, 0, 0)` | Bewegungs-Referenzbuffer freigeben |
| 19 | `camControl(19, addr, mask)` | Rohes Sensorregister lesen |
| 20 | `camControl(20, addr, val)` | Rohes Sensorregister schreiben |

Aufnahme (sel 10) kopiert das JPEG vom Kamera-Framebuffer in einen PSRAM-Slot und gibt den Framebuffer sofort zurueck, was schnelle aufeinanderfolgende Aufnahmen ermoeglicht. Bis zu 4 Slots koennen gleichzeitig Bilder halten.

**Wichtig:** Kamera-Aufnahme (`camControl(10, ...)`) muss in `TaskLoop()` (VM-Task-Thread) laufen. Aufruf aus `EverySecond()` (Haupt-Thread) friert das Geraet ein.

**Stream-Server (sel 15):** Startet einen MJPEG-Server auf Port 81 mit `/stream`, `/cam.mjpeg` und `/cam.jpg` Endpunkten. Wird automatisch verzoegert, wenn WiFi noch nicht bereit ist (sicher fuer Autoexec). Der Stream wird auf der Tasmota-Hauptseite eingebettet.

#### Sensor-Parameter (sel=9)

| param | Einstellung | Bereich |
|-------|------------|---------|
| 0 | vflip | 0/1 |
| 1 | Helligkeit | -2..2 |
| 2 | Saettigung | -2..2 |
| 3 | hmirror | 0/1 |
| 4 | Kontrast | -2..2 |
| 5 | Bildgroesse | FRAMESIZE_* |
| 6 | Qualitaet | 10..63 |
| 7 | Schaerfe | -2..2 |

#### E-Mail Bildanhaenge

In PSRAM-Slots aufgenommene Bilder koennen per `mailAttachPic()` an E-Mails angehaengt werden. Bis zu 4 Bilder pro E-Mail:

```c
// 2 Bilder in Slot 1 und 2 aufnehmen
camControl(10, 1, 0);
camControl(10, 2, 0);

// E-Mail mit beiden Bildern senden
mailBody("Bewegungsalarm");
mailAttachPic(1);
mailAttachPic(2);
mailSend("[*:*:*:*:*:user@example.com:Alarm]");
```

#### Aufnahme und Speichern Beispiel

```c
// Bild in PSRAM-Slot 1 aufnehmen
int size = camControl(10, 1, 0);

// Slot 1 in Datei speichern
int fh = fileOpen(path, 1);    // zum Schreiben oeffnen
int written = camControl(11, 1, fh);
fileClose(fh);

// MJPEG Stream auf Port 81 starten
camControl(15, 1, 0);
```

#### Komplettes Kamera-Skript

Siehe `webcam_tinyc.tc` fuer ein vollstaendiges Sicherheitskamera-Beispiel mit MJPEG-Streaming, Bewegungserkennung, PIR-Alarm, E-Mail-Benachrichtigung, Zeitraffer und automatischem Aufraeumen. Siehe `webcam.tc` fuer die Variante mit dem Tasmota Webcam-Treiber.

---

### HomeKit (ESP32 — benoetigt USE_HOMEKIT)

Apple HomeKit-Integration — Geraete direkt aus TinyC als HomeKit-Zubehoer bereitstellen. Sensoren, Lichter, Schalter und Steckdosen werden ueber Apple Home steuerbar. Alle HomeKit-gebundenen Variablen verwenden **native Float-Werte** — keine x10-Skalierung noetig.

**Benoetigt:** `#define USE_HOMEKIT` in `user_config_override.h`.

#### Vordefinierte HomeKit-Konstanten

| Konstante | Wert | HAP-Kategorie | Variablen |
|-----------|------|---------------|-----------|
| `HK_TEMPERATURE` | 1 | Sensor (Temperatur) | 1: Temperatur in °C |
| `HK_HUMIDITY` | 2 | Sensor (Feuchte) | 1: Feuchte in % |
| `HK_LIGHT_SENSOR` | 3 | Sensor (Helligkeit) | 1: Lux-Wert |
| `HK_BATTERY` | 4 | Sensor (Batterie) | 3: Ladezustand, Schwach-Flag, Ladestatus |
| `HK_CONTACT` | 5 | Sensor (Kontakt) | 1: Offen/Geschlossen |
| `HK_SWITCH` | 6 | Schalter | 1: Ein/Aus |
| `HK_OUTLET` | 7 | Steckdose | 1: Ein/Aus |
| `HK_LIGHT` | 8 | Licht (Farbe) | 4: Power, Hue, Saturation, Brightness |

#### HomeKit-Funktionen

| Funktion | Beschreibung |
|----------|-------------|
| `hkSetCode(code)` | Kopplungscode festlegen (Format: `"XXX-XX-XXX"`) |
| `hkAdd(name, typ)` | Geraet hinzufuegen — Name und Typ (z.B. `HK_TEMPERATURE`) |
| `hkVar(variable)` | Float-Variable an das aktuelle Geraet binden |
| `int hkReady(variable)` | Gibt 1 zurueck wenn HomeKit diese Variable geaendert hat (loescht Flag automatisch) |
| `int hkStart()` | Deskriptor fertigstellen und HomeKit starten. Gibt 0=ok zurueck |
| `int hkInit(char descriptor[])` | HomeKit mit Raw-Deskriptor starten |
| `hkReset()` | Alle Kopplungsdaten loeschen (Werksreset). Nach Neustart erneut koppeln |
| `hkStop()` | HomeKit-Server beenden |

#### hkReady() — Aenderungsabfrage

`hkReady(var)` funktioniert wie `udpReady()` — gibt 1 zurueck wenn Apple Home diese Variable seit dem letzten Aufruf geaendert hat, und loescht das Flag automatisch. Die Firmware schreibt den Wert direkt in die globale Variable, daher ist keine manuelle Zuweisung noetig. Da `global` Variablen automatisch per UDP senden, ist kein expliziter Aufruf mehr noetig:

```c
void EverySecond() {
    // global Variablen senden automatisch bei Zuweisung — kein expliziter udpSend noetig
}
```

#### HomeKitWrite-Callback (Optional)

Wird aufgerufen wenn Apple Home einen Wert aendert. Der Wert ist bereits in der globalen Variable gespeichert bevor dieser Callback laeuft — nur fuer lokale Seiteneffekte wie Relais-Weiterleitung verwenden:

```c
void HomeKitWrite(int dev, int var, float val) {
    // dev = Geraeteindex (Reihenfolge der hkAdd-Aufrufe, ab 0)
    // var = Variablenindex (Reihenfolge der hkVar-Aufrufe pro Geraet, ab 0)
    // val = neuer Float-Wert von Apple Home (bereits in der Variablen gespeichert)
    // Nur fuer Seiteneffekte wie tasm_power = 1 noetig
}
```

#### Builder-Pattern (hkAdd + hkVar)

Geraete werden schrittweise definiert. `hkAdd()` beginnt ein Geraet, `hkVar()` bindet Float-Variablen daran. Mehrere `hkVar()`-Aufrufe fuer Geraete mit mehreren Eigenschaften (z.B. Licht mit Farbe):

```c
// Farbiges Licht — 4 Variablen: Power, Hue, Saturation, Brightness
float pwr, hue, sat, bri;

hkSetCode("111-22-333");
hkAdd("Lampe", HK_LIGHT);
hkVar(pwr); hkVar(hue); hkVar(sat); hkVar(bri);

// Einfacher Sensor — 1 Variable
float temp;
hkAdd("Temperatur", HK_TEMPERATURE);
hkVar(temp);

hkStart();
```

#### Vollstaendiges Beispiel — Buero mit Licht + Sensoren

```c
// HomeKit-gebundene Variablen (native Float-Werte)
float mh_pwr, mh_hue, mh_sat, mh_bri;  // Farblicht
float elamp;     // Ecklicht ein/aus
float btemp;     // Temperatur (z.B. 22.5)
float bhumi;     // Feuchte (z.B. 55.0)
int last_pwr;

// Nur fuer Relais-Weiterleitung noetig — Wert ist bereits in der Variable
void HomeKitWrite(int dev, int var, float val) {
    if (dev == 0 && var == 0) {
        int pwr;
        pwr = 0;
        if (val > 0.0) { pwr = 1; }
        if (pwr != last_pwr) { tasm_power = pwr; last_pwr = pwr; }
    }
}

void EverySecond() {
    // Sensorwerte via UDP empfangen
    if (udpReady("btemp")) { btemp = udpRecv("btemp"); }
    if (udpReady("bhumi")) { bhumi = udpRecv("bhumi"); }

    // global Variablen senden automatisch bei Zuweisung — kein expliziter udpSend noetig
}

int main() {
    mh_pwr = 0.0; mh_hue = 0.0; mh_sat = 0.0; mh_bri = 50.0;
    elamp = 0.0; btemp = 22.0; bhumi = 50.0;
    last_pwr = -1;

    hkSetCode("111-11-111");
    hkAdd("Licht", HK_LIGHT);
    hkVar(mh_pwr); hkVar(mh_hue); hkVar(mh_sat); hkVar(mh_bri);
    hkAdd("Ecklicht", HK_OUTLET);          hkVar(elamp);
    hkAdd("Temperatur", HK_TEMPERATURE);    hkVar(btemp);
    hkAdd("Feuchte", HK_HUMIDITY);          hkVar(bhumi);
    hkStart();
    return 0;
}
```

#### Kopplung

1. Firmware mit `USE_HOMEKIT` kompilieren und flashen
2. TinyC-Programm mit `hkSetCode()` / `hkAdd()` / `hkStart()` kompilieren und hochladen
3. QR-Code unter `http://<Geraet>/hk` mit iPhone scannen
4. Bei Konfigurationsaenderungen `hkReset()` einmalig ausfuehren, dann erneut koppeln

#### Vordefinierte Datei-Konstanten

Fuer `fileOpen()` stehen folgende Kurzformen zur Verfuegung:

| Konstante | Wert | Beschreibung |
|-----------|------|-------------|
| `r` | 0 | Lesen |
| `w` | 1 | Schreiben |
| `a` | 2 | Anhaengen |

```c
int f = fileOpen("/daten.csv", r);   // statt fileOpen("/daten.csv", 0)
f = fileOpen("/log.txt", a);          // statt fileOpen("/log.txt", 2)
```

### Plugin-Abfrage

| Funktion | Beschreibung |
|----------|-------------|
| `int pluginQuery(char dst[], int index, int p1, int p2)` | Binäres Plugin abfragen. Gibt Ergebnis zurueck und schreibt optionale String-Antwort in `dst` |

### Debug

| Funktion      | Beschreibung                    |
|---------------|---------------------------------|
| `dumpVM()`    | VM-Zustand auf Konsole ausgeben |

---

## Multi-VM Slots (ESP32)

Auf dem ESP32 koennen bis zu **6 unabhaengige TinyC-Programme** gleichzeitig in separaten VM-Slots laufen. Jeder Slot hat eigenen Bytecode, Globals, Stack, Heap und Ausgabepuffer. Speicher wird dynamisch allokiert — leere Slots kosten null Bytes, nicht-autoexec Slots verwenden Lazy Loading (nur ~33 Bytes bis zum ersten Start). Der ESP8266 unterstuetzt nur 1 Slot.

### Slot-Konfiguration

Slot-Zuweisungen und Autoexec-Flags werden in `/tinyc.cfg` auf dem Dateisystem gespeichert. Diese Datei wird automatisch erstellt und aktualisiert, wenn ein Programm geladen, hochgeladen oder das Autoexec-Flag umgeschaltet wird. Eine manuelle Bearbeitung ist nicht notwendig.

Beispiel `/tinyc.cfg`:
```
/weather.tcb,1
/display.tcb,1
/logger.tcb,0
,0
_info,0
```

Jede Zeile entspricht einem Slot (0–3): `Dateiname,Autoexec-Flag`. Die letzte Zeile `_info,<0|1>` steuert, ob Debug-Statuszeilen auf der Tasmota-Hauptseite angezeigt werden.

### Tasmota-Befehle

Alle Befehle verwenden standardmaessig Slot 0, wenn keine Slot-Nummer angegeben wird (abwaertskompatibel).

| Befehl                        | Beschreibung                                     |
|-------------------------------|--------------------------------------------------|
| `TinyC`                       | Status aller Slots anzeigen (JSON)               |
| `TinyCRun [slot] [/datei.tcb]`| Slot starten (optional Datei vorher laden)       |
| `TinyCStop [slot]`            | Slot stoppen                                     |
| `TinyCReset [slot]`           | Slot stoppen und zuruecksetzen                   |
| `TinyCExec <n>`               | Instruktionen pro Tick setzen (Standard 1000)    |
| `TinyCInfo 0\|1`              | VM-Debug-Zeilen auf Hauptseite ein-/ausblenden   |
| `TinyC ?<abfrage>`            | Globale Variablen per Index abfragen (siehe unten)|

**Beispiele:**
```
TinyCRun                    → Slot 0 starten
TinyCRun /weather.tcb       → Datei in Slot 0 laden und starten
TinyCRun 2 /logger.tcb      → Datei in Slot 2 laden und starten
TinyCStop 1                 → Slot 1 stoppen
TinyCReset 3                → Slot 3 zuruecksetzen
TinyCInfo 1                 → Debug-Info auf Hauptseite anzeigen
```

### Web-Konsole (`/tc`)

Die TinyC-Konsolenseite unter `/tc` zeigt eine kompakte Uebersicht aller Slots:

- **Statusanzeige**: gruener Punkt = aktiv (laeuft oder Callback-bereit), orange = geladen aber nicht gestartet, grau = leer
- **Run / Stop Buttons**: kontextabhaengig — Run ausgegraut wenn aktiv, Stop ausgegraut wenn inaktiv
- **A-Button**: schaltet Auto-Ausfuehrung beim Booten um (gruen = aktiviert). Wird sofort in `/tinyc.cfg` gespeichert
- **Programm laden**: Dateiauswahl mit Slot-Dropdown um beliebige `.tcb`-Datei in beliebigen Slot zu laden
- **Programm hochladen**: Datei-Upload mit Slot-Dropdown um eine `.tcb`-Datei direkt hochzuladen

### API-Endpunkte

Die JSON-API unter `/tc_api` unterstuetzt einen `slot`-Parameter:

```
GET /tc_api?cmd=run&slot=2     → Slot 2 starten
GET /tc_api?cmd=stop&slot=1    → Slot 1 stoppen
GET /tc_api?cmd=status         → Status aller Slots
POST /tc_upload?slot=3&api=1   → .tcb in Slot 3 hochladen (JSON-Antwort)
```

### Variablen-Abfrage — `_Q()` Makro (Google Charts)

Globale TinyC-Variablen koennen per HTTP als JSON abgefragt werden, um Live-Dashboards mit Google Charts oder anderen JavaScript-Charting-Bibliotheken zu erstellen.

Das `_Q()` Makro wird zur **Kompilierzeit** in String-Literalen expandiert. Der Compiler ersetzt Variablennamen durch ihre Indizes und Typen — das Binary enthaelt keine Variablennamen, nur kompakte Index-basierte Abfragen.

**Syntax:** `_Q(var1, var2, ...)`

Der Compiler ersetzt `_Q(...)` durch einen index-kodierten Abfrage-String:
- `<index>i` — int-Skalar
- `<index>f` — float-Skalar
- `<index>s<n>` — char[n] String
- `<index>I<n>` — int-Array mit n Elementen
- `<index>F<n>` — float-Array mit n Elementen

**Beispiel:** Bei den Globalen `float temperature; int counter;` wird der String:
```c
"TinyC+%3F_Q(temperature,counter)"
```
zur Kompilierzeit expandiert zu:
```
"TinyC+%3F0f;1i"
```

**Antwortformat:** JSON-Array in der Reihenfolge der Abfrage:
```json
{"TinyC":[23.5,42]}
```

**Verwendung im WebPage-Callback:**
```c
float temperature = 23.5;
int counter = 0;

void WebPage() {
    webSend("<script>fetch('/cm?cmnd=TinyC+%3F_Q(temperature,counter)')");
    webSend(".then(r=>r.json()).then(d=>{var v=d.TinyC;");
    webSend("// v[0]=temperature, v[1]=counter");
    webSend("});</script>");
}
```

Fuer einen bestimmten Slot wird die Slot-Nummer vorangestellt:
```
TinyC ?2 0f;1i      → Abfrage aus Slot 2
```

### Boot-Ablauf

Beim Booten liest TinyC `/tinyc.cfg` und:
1. Laedt jede konfigurierte `.tcb`-Datei in ihren Slot
2. Startet automatisch Slots mit gesetztem Autoexec-Flag (`1`)

Wenn keine `/tinyc.cfg` existiert (erster Start), werden keine Programme geladen.

### Ressourcenverbrauch

Jeder VM-Slot verbraucht ca. **3,2 KB RAM** (nur Struct, ohne Programm-Bytecode). Slots werden dynamisch allokiert — nur aktive Slots verbrauchen Speicher. Das Slot-Pointer-Array selbst benoetigt nur 24 Bytes. Nicht-autoexec Slots verwenden Lazy Loading: nur der Dateiname (~33 Bytes) wird gespeichert bis zum ersten Start.

| Ressource             | Kosten                       |
|-----------------------|------------------------------|
| Pointer-Array         | 16 Bytes (4 Zeiger)          |
| Pro-Slot Struct       | ~3,2 KB                      |
| Programm-Bytecode     | variabel (malloc)            |
| Heap (alle Arrays)    | max 32 KB, bei Bedarf allokiert |

### Callbacks mit mehreren Slots

Jeder Slot erhaelt seine eigenen Callbacks unabhaengig:

- `EverySecond()`, `Every50ms()` — werden an alle aktiven Slots verteilt
- `WebCall()` — jeder Slot kann eigene Sensorzeilen zur Hauptseite hinzufuegen
- `JsonCall()` — jeder Slot fuegt eigene Telemetriedaten hinzu
- `TaskLoop()` — laeuft im eigenen FreeRTOS-Task des Slots (ESP32)
- `CleanUp()` — wird auf allen Slots vor Geraete-Neustart aufgerufen

Geteilte Ressourcen (UDP, SPI, Datei-Handles) sind global — nur ein Slot sollte diese gleichzeitig nutzen.

### Beispiel: Zwei Programme nebeneinander

Slot 0 — Temperaturueberwachung:
```c
int temp = 0;
void EverySecond() { temp = tasm_analog0; }
void WebCall() {
    char buf[64];
    sprintfInt(buf, "{s}Temperatur{m}%d{e}", temp);
    webSend(buf);
}
int main() { return 0; }
```

Slot 1 — Betriebszeitzaehler:
```c
int uptime = 0;
void EverySecond() { uptime++; }
void WebCall() {
    char buf[64];
    sprintfInt(buf, "{s}Betriebszeit{m}%d s{e}", uptime);
    webSend(buf);
}
int main() { return 0; }
```

Beide zeigen ihre Sensorzeilen gleichzeitig auf der Tasmota-Hauptseite an.

---

## VM-Grenzen

| Ressource         | ESP8266  | ESP32    | Browser  | Anmerkungen                        |
|--------------------|----------|----------|----------|------------------------------------|
| Stack-Tiefe        | 64       | 256      | 256      | Operandenstack-Eintraege           |
| Aufrufrahmen       | 8        | 32       | 32       | Maximale Rekursions-/Aufruftiefe   |
| Lokale pro Rahmen  | 256      | 256      | 256      | Skalare + kleine Arrays ≤16 inline  |
| Globale Variablen  | 64       | 256      | 256      | Skalare + kleine Arrays ≤16 inline  |
| Codegroesse        | 4 KB     | 16 KB    | 64 KB    | Bytecode (16-Bit-Adressierung)     |
| Heap-Speicher      | 8 KB     | 32 KB    | 64 KB    | Fuer Arrays >16 Elemente (autom. Allokation) |
| Heap-Handles       | 8        | 16       | 32       | Max. gleichzeitige Heap-Allokationen |
| Konstantenpool     | 32       | 64       | 65536    | Zeichenketten- & Float-Konstanten  |
| Instruktionslimit  | 1M       | 1M       | 1M       | Sicherheitslimit pro Ausfuehrung   |
| GPIO-Pins          | 40       | 40       | 40       | Pins 0–39 (im Browser simuliert)   |
| Datei-Handles      | 4        | 4        | 8        | Gleichzeitig geoeffnete Dateien    |
| VM-Slots           | 1        | 6        | 1        | Gleichzeitige Programme            |

---

## Geraetedateiverwaltung (IDE)

### IDE-Installation

Die IDE-Datei (`tinyc_ide.html.gz`) kann entweder auf dem **Flash-Dateisystem** oder der **SD-Karte** liegen — je nachdem, welches als Benutzer-Dateisystem (`ufsp`) gemountet ist. Laden Sie `tinyc_ide.html.gz` ueber die Tasmota-Seite **Dateisystem verwalten** hoch.

> **Hinweis:** TinyC-Skripte und Datendateien (`.tc`, `.tcb` usw.) werden ebenfalls auf dem Benutzer-Dateisystem (`ufsp`) gespeichert.

### Dateioperationen

Die IDE-Werkzeugleiste enthaelt Steuerelemente zur Verwaltung von Dateien auf dem Tasmota-Geraetedateisystem:

- **Geraetedateien-Dropdown** — Listet alle Dateien auf dem Geraet auf. Waehlen Sie eine Datei, um sie in den Editor zu laden. Die Liste zeigt Dateiname und Groesse (z.B. `config.tc (1.2KB)`).
- **Datei-Speichern-Schaltflaeche** — Speichert den aktuellen Editorinhalt als Datei auf dem Geraet. Fragt nach einem Dateinamen (Standard ist der aktuelle Dateiname).
- **Automatische Aktualisierung** — Die Dateiliste wird automatisch aktualisiert, wenn die Geraete-IP eingegeben oder geaendert wird, und nach jedem Speichervorgang.

Alle Dateioperationen verwenden den `/tc_api`-Endpunkt mit CORS-Unterstuetzung, sodass die IDE von jedem Browser aus verwendet werden kann — sie muss nicht vom Geraet bereitgestellt werden.

### API-Endpunkte

| Endpunkt | Methode | Beschreibung |
|----------|---------|-------------|
| `/tc_api?cmd=listfiles` | GET | Gibt JSON-Liste der Dateien zurueck: `{"ok":true,"files":[{"name":"x","size":123},...]}` |
| `/tc_api?cmd=readfile&path=/name` | GET | Gibt Dateiinhalt als Klartext zurueck |
| `/tc_api?cmd=readfile&path=/name@von_bis` | GET | Gibt zeitgefilterte CSV-Daten zurueck (siehe unten) |
| `/tc_api?cmd=writefile&path=/name` | POST | Schreibt POST-Body in Datei, gibt `{"ok":true,"size":N}` zurueck |
| `/tc_api?cmd=deletefile&path=/name` | GET | Loescht eine Datei vom Dateisystem |

### Zeitbereichs-gefilterter Dateizugriff

Haengen Sie `@von_bis` an den Dateipfad an, um nur Zeilen innerhalb eines Zeitbereichs aus einer CSV-Datendatei zu extrahieren. Dies ist nuetzlich fuer die Bereitstellung von IoT-Zeitreihendaten an Chart-Bibliotheken.

**URL-Format:**
```
/tc_api?cmd=readfile&path=/data.csv@TT.MM.JJ-HH:MM_TT.MM.JJ-HH:MM
```

**Beispiel:**
```
/tc_api?cmd=readfile&path=/sml.csv@1.1.24-00:00_31.1.24-23:59
```

Sowohl das deutsche (`TT.MM.JJ HH:MM`) als auch das ISO-Format (`JJJJ-MM-TTTHH:MM:SS`) werden unterstuetzt. Der `_` (Unterstrich) trennt die Von- und Bis-Zeitstempel.

**Antwort:** Die Kopfzeile (erste Zeile) wird immer eingeschlossen, gefolgt von nur den Datenzeilen, deren Zeitstempel in der ersten Spalte innerhalb `[von..bis]` liegt. Zeilen nach dem Endzeitstempel werden effizient uebersprungen (fruehzeitiger Abbruch).

**Leistungsoptimierung:** Wenn eine Indexdatei existiert (gleicher Name mit `.ind`-Erweiterung, enthaltend `Zeitstempel\tByte-Offset`-Zeilen), werden Byte-Offsets verwendet, um direkt zur Startposition zu springen. Andernfalls wird eine geschaetzte Positionssuche basierend auf dem ersten und letzten Zeitstempel der Datei durchgefuehrt (aehnlich wie Scripters `opt_fext`).

### Port 82 Download-Server (ESP32)

Fuer grosse Datenbankdateien kann der zeitgefilterte Dateizugriff auf Port 80 die Haupt-Webserver-Schleife blockieren. TinyC enthaelt einen dedizierten **Port 82 Download-Server**, der Dateien in einem FreeRTOS-Hintergrund-Task bereitstellt und das Geraet waehrend grosser Uebertragungen reaktionsfaehig haelt.

**URL-Format:**
```
http://<ip>:82/ufs/<dateiname>
http://<ip>:82/ufs/<dateiname>@von_bis
```

**Beispiele:**
```
http://192.168.1.100:82/ufs/sml.csv
http://192.168.1.100:82/ufs/sml.csv@1.1.24-00:00_31.1.24-23:59
```

**Eigenschaften:**
- Laeuft in einem dedizierten FreeRTOS-Task (angeheftet an Core 1, Prioritaet 3)
- Blockiert nicht die Tasmota-Hauptschleife oder Weboberflaeche
- Unterstuetzt die gleiche `@von_bis` Zeitbereichsfilterung wie `/tc_api` readfile
- Verwendet Chunked-Transfer-Encoding fuer gefilterte Antworten
- Content-Disposition-Header fuer Browser-Download
- Ein Download gleichzeitig (gibt HTTP 503 zurueck wenn beschaeftigt)
- Automatische MIME-Typ-Erkennung (`.csv`/`.txt` = text/plain, `.html`, `.json`)
- Der Port kann durch Definition von `TC_DLPORT` vor der Kompilierung geaendert werden (Standard: 82)

### Typischer Arbeitsablauf

1. Geraete-IP in die Werkzeugleiste eingeben
2. Das **Geraetedateien**-Dropdown fuellt sich automatisch mit allen Dateien auf dem Geraet
3. Datei auswaehlen, um sie in den Editor zu laden — oder neuen Code schreiben
4. **Datei speichern** klicken, um den Quellcode auf dem Geraet zu speichern (z.B. als `myapp.tc`)
5. **Auf Geraet ausfuehren** klicken, um zu kompilieren, die `.tcb`-Binaerdatei hochzuladen und die Ausfuehrung zu starten

So koennen Sie TinyC-Quelldateien zusammen mit ihrem kompilierten Bytecode auf dem Geraet aufbewahren, was das Bearbeiten von Programmen direkt ohne lokale Dateispeicherung erleichtert.

## Tastenkuerzel (IDE)

| Tastenkuerzel      | Aktion                |
|--------------------|-----------------------|
| Ctrl + Enter       | Kompilieren           |
| Ctrl + Shift + Enter | Kompilieren & Ausfuehren |
| Ctrl + S           | Datei speichern       |
| Ctrl + O           | Datei oeffnen         |
| Ctrl + F           | Suchen                |
| Enter (in Suche)   | Naechstes finden      |
| Shift + Enter (in Suche) | Vorheriges finden |
| Escape             | Suchleiste schliessen |
| Tab (im Editor)    | 4 Leerzeichen einfuegen |

---

## Beispiele

Die IDE enthaelt 19 sofort einsatzbereite Beispiele im Dropdown "Beispiel laden..." — von einfachem Blinken bis zu Wetterstationsempfaengern und interaktiven WebUI-Dashboards.

### Hello World
```c
int main() {
    printStr("Hello, TinyC!\n");
    return 0;
}
```

### LED-Blinken
```c
#define LED 2
#define INPUT         0x01
#define OUTPUT        0x03
#define INPUT_PULLUP  0x05
#define INPUT_PULLDOWN 0x09

int main() {
    gpioInit(LED, OUTPUT);
    while (true) {
        digitalWrite(LED, 1);
        delay(500);
        digitalWrite(LED, 0);
        delay(500);
    }
    return 0;
}
```

### Fibonacci
```c
int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    for (int i = 0; i < 10; i++) {
        print(fib(i));
    }
    return 0;
}
```

### Zeichenkettenoperationen
```c
int main() {
    char greeting[32] = "Hello";
    char name[16] = "World";
    char buf[64];

    // Klassischer Funktionsstil
    strcpy(buf, greeting);
    strcat(buf, ", ");
    strcat(buf, name);
    strcat(buf, "!\n");
    printString(buf);       // Hello, World!

    // Dasselbe mit +-Operator
    buf = greeting;
    buf += ", ";
    buf += name;
    buf = buf + "!\n";
    printString(buf);       // Hello, World!

    // Formatierte Zeichenketten
    char line[64];
    sprintf(line, "count = %d", 42);
    printString(line);      // count = 42

    // Mehrere Werte mit sprintfAppend
    char report[128];
    sprintf(report, "Sensor %d", 1);
    sprintfAppend(report, " name=%s", name);
    sprintfAppend(report, " temp=%.1f", 23.5);
    printString(report);    // Sensor 1 name=World temp=23.5

    return 0;
}
```

### Bubble Sort
```c
void bubbleSort(int arr[], int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}

int main() {
    int data[8] = {64, 34, 25, 12, 22, 11, 90, 1};
    bubbleSort(data, 8);
    for (int i = 0; i < 8; i++) {
        print(data[i]);
    }
    return 0;
}
```

### WebUI-Dashboard
```c
int power;
int brightness;
int mode;

void WebUI() {
    int page = webPage();
    if (page == 0) {
        webButton(power, "Power");
        webSlider(brightness, 0, 100, "Brightness");
    }
    if (page == 1) {
        webPulldown(mode, "Mode", "Off|Auto|Manual");
    }
}

int main() {
    webPageLabel(0, "Controls");
    webPageLabel(1, "Settings");
    brightness = 50;
    return 0;
}
```

---

## Unterschiede zu Standard-C

| Merkmal                  | Standard-C     | TinyC                        |
|--------------------------|----------------|------------------------------|
| Zeiger                   | Volle Unterstuetzung | **Nicht unterstuetzt**  |
| Structs / Unions         | Volle Unterstuetzung | **Nicht unterstuetzt**  |
| Enums                    | Volle Unterstuetzung | **Nicht unterstuetzt**  |
| Dynamischer Speicher     | malloc/free    | Auto-Heap fuer Arrays >16 Elemente (kein explizites malloc) |
| Mehrdimensionale Arrays  | `int a[3][4]`  | **Nicht unterstuetzt**       |
| Zeichenkettentyp         | `char*`        | Nur `char arr[N]`            |
| Praeprozessor            | Volles CPP     | `#define`, `#ifdef`, `#if`, `#else`, `#endif` (kein `#include`, keine Makros) |
| Header-Dateien           | `#include`     | **Nicht unterstuetzt**       |
| Typedef                  | Volle Unterstuetzung | **Nicht unterstuetzt**  |
| sizeof                   | Volle Unterstuetzung | **Nicht unterstuetzt**  |
| Ternaerer Operator       | `a ? b : c`   | **Nicht unterstuetzt**       |
| do-while                 | `do {} while`  | **Nicht unterstuetzt**       |
| goto                     | Volle Unterstuetzung | **Nicht unterstuetzt**  |
| Funktionszeiger          | Volle Unterstuetzung | **Nicht unterstuetzt**  |
| Variadische Funktionen   | `printf(...)`  | **Nicht unterstuetzt**       |
| Standardbibliothek       | stdio, stdlib  | Nur eingebaute Funktionen    |

---

*Generiert aus TinyC-Quellcode — lexer.js, parser.js, codegen.js, opcodes.js, vm.js*
