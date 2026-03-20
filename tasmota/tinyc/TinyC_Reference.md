# TinyC Language Reference

**TinyC** is a subset of C that compiles to bytecode for a stack-based virtual machine.
It runs both in the browser (JavaScript VM) and on ESP32/ESP8266 (as Tasmota driver XDRV_124).

---

## Table of Contents

1. [Data Types](#data-types)
2. [Literals](#literals)
3. [Variables & Scope](#variables--scope)
4. [Operators](#operators)
5. [Control Flow](#control-flow)
6. [Functions](#functions)
7. [Callback Functions](#callback-functions)
8. [Tasmota System Variables](#tasmota-system-variables)
9. [Arrays](#arrays)
10. [Strings](#strings)
11. [Preprocessor](#preprocessor)
12. [Comments](#comments)
13. [Type Casting](#type-casting)
14. [Built-in Functions](#built-in-functions)
15. [Multi-VM Slots (ESP32)](#multi-vm-slots-esp32)
16. [VM Limits](#vm-limits)
17. [Device File Management (IDE)](#device-file-management-ide)
18. [Keyboard Shortcuts (IDE)](#keyboard-shortcuts-ide)
19. [Examples](#examples)

---

## Data Types

| Type    | Size   | Description                         |
|---------|--------|-------------------------------------|
| `int`   | 32-bit | Signed integer                      |
| `float` | 32-bit | IEEE 754 floating-point             |
| `char`  | 8-bit  | Unsigned character (masked to 0xFF) |
| `bool`  | 32-bit | Boolean (0 = false, non-zero = true)|
| `void`  | —      | No value (function return type)     |

### Type Aliases

| Alias          | Maps to |
|----------------|---------|
| `int32_t`      | `int`   |
| `uint32_t`     | `int`   |
| `unsigned int` | `int`   |
| `uint8_t`      | `char`  |

---

## Literals

### Integer Literals
```c
42          // decimal
0xFF        // hexadecimal (prefix 0x or 0X)
0b1010      // binary (prefix 0b or 0B)
```

### Float Literals
```c
3.14        // decimal point
2.5f        // with float suffix
0.001       // leading zero
```

### Character Literals
```c
'A'         // single character
'\n'        // escape sequence
'\0'        // null terminator
```

**Supported escape sequences:** `\n` `\t` `\r` `\\` `\'` `\"` `\0`

### String Literals
```c
"Hello"             // simple string
"Line 1\nLine 2"    // with escape sequences
```
String literals are used for `char` array initialization and as arguments to string functions.

### Boolean Literals
```c
true        // evaluates to 1
false       // evaluates to 0
```

---

## Variables & Scope

### Global Variables
Declared outside any function. Accessible from all functions.
```c
int counter = 0;
float pi = 3.14;
char buffer[64];
```

### Persistent Variables
Global variables declared with the `persist` keyword are automatically saved to flash and restored on program restart. This is equivalent to `p:` variables in Tasmota Scripter.

```c
persist float totalEnergy = 0.0;   // saved/restored across reboots
persist int bootCount;              // scalar — 4 bytes in file
persist char deviceName[32];        // array — 32 slots in file
```

- Only global variables can be `persist` (not local variables or function parameters)
- Persist variables are **automatically loaded** from a `.pvs` file (derived from the `.tcb` filename, e.g. `/weather.pvs` for `/weather.tcb`) when the program starts
- Persist variables are **automatically saved** when the program is stopped (`TinyCStop`)
- Call `saveVars()` to manually save at any time (e.g., after midnight counter updates)
- Maximum 32 persist entries per program
- Binary format — compact and fast (raw int32 values, floats stored as bit-cast int32)
- File stored on user filesystem (same as `.tcb` files)

```c
persist float dval = 0.0;
persist float mval = 0.0;

void EverySecond() {
    if (tasm_hour == 0 && last_hr != 0) {
        dval = smlGet(2);  // update daily counter
        saveVars();         // save immediately
    }
}
```

### Watch Variables (Change Detection)
Global variables declared with the `watch` keyword automatically track changes. Every write saves the old value as a shadow, enabling change detection — essential for IOT monitoring scenarios.

```c
watch float power;
watch int relay;
```

- Only scalar globals can be `watch` (int, float — not arrays or locals)
- Every write automatically saves the previous value and sets a written flag
- Uses 2 extra global slots per watch variable (shadow + written flag)

**Intrinsic functions:**

| Function | Returns | Description |
|----------|---------|-------------|
| `changed(var)` | `int` | 1 if current value differs from shadow |
| `delta(var)` | `int/float` | current - shadow (signed difference) |
| `written(var)` | `int` | 1 if variable was assigned since last `snapshot()` |
| `snapshot(var)` | `void` | set shadow = current, clear written flag |

```c
watch float power;

void EverySecond() {
    power = sensorGet("ENERGY#Power");
    if (changed(power)) {
        float diff = delta(power);
        // react to power change
        snapshot(power);  // acknowledge change
    }
}
```

### Local Variables
Declared inside functions or blocks. Block-scoped (new scope per `{ }`).
```c
void myFunc() {
    int x = 10;        // local to myFunc
    if (x > 5) {
        int y = 20;    // local to this block
    }
    // y is not accessible here
}
```

### Function Parameters
Passed by value for scalars, by reference for arrays.
```c
void process(int value, int data[]) {
    // value is a copy, data is a reference
}
```

---

## Operators

### Arithmetic
| Op  | Description    | Types              |
|-----|----------------|--------------------|
| `+` | Addition       | int, float, char[] |
| `-` | Subtraction    | int, float         |
| `*` | Multiplication | int, float         |
| `/` | Division       | int, float         |
| `%` | Modulo         | int only           |
| `-` | Unary negation | int, float         |

**Note:** For `char[]` variables, `+` performs string concatenation (see [Strings](#strings)).

### Comparison
| Op   | Description        |
|------|--------------------|
| `==` | Equal              |
| `!=` | Not equal          |
| `<`  | Less than          |
| `>`  | Greater than       |
| `<=` | Less than or equal |
| `>=` | Greater or equal   |

### Logical
| Op     | Description                    |
|--------|--------------------------------|
| `&&`   | Logical AND (short-circuit)    |
| `\|\|` | Logical OR (short-circuit)     |
| `!`    | Logical NOT                    |

### Bitwise
| Op  | Description |
|-----|-------------|
| `&` | AND         |
| `\|`| OR          |
| `^` | XOR         |
| `~` | NOT         |
| `<<`| Left shift  |
| `>>`| Right shift |

### Assignment
| Op  | Description                                       |
|-----|---------------------------------------------------|
| `=` | Assign (for `char[]`: string copy)                |
| `+=`| Add and assign (for `char[]`: string append)      |
| `-=`| Subtract and assign                               |
| `*=`| Multiply and assign                               |
| `/=`| Divide and assign                                 |

### Increment / Decrement
```c
++x     // pre-increment
--x     // pre-decrement
x++     // post-increment
x--     // post-decrement
```

### Operator Precedence (highest to lowest)

1. Postfix: `x++` `x--` `a[i]` `f()` `(type)`
2. Unary: `++x` `--x` `-x` `!x` `~x`
3. Multiplicative: `*` `/` `%`
4. Additive: `+` `-`
5. Shift: `<<` `>>`
6. Relational: `<` `>` `<=` `>=`
7. Equality: `==` `!=`
8. Bitwise AND: `&`
9. Bitwise XOR: `^`
10. Bitwise OR: `|`
11. Logical AND: `&&`
12. Logical OR: `||`
13. Assignment: `=` `+=` `-=` `*=` `/=`

---

## Control Flow

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

### while Loop
```c
while (condition) {
    // ...
    if (done) break;
    if (skip) continue;
}
```

### for Loop
```c
for (int i = 0; i < 10; i++) {
    // ...
}

// all parts optional:
for (;;) {
    // infinite loop
    break;
}
```

### switch / case
```c
switch (value) {
    case 1:
        // ... fall-through!
    case 2:
        // ...
        break;
    default:
        // ...
        break;
}
```
**Note:** Cases fall through unless `break` is used (like standard C).

### break / continue
- `break;` — exit the innermost loop or switch
- `continue;` — skip to the next iteration of the innermost loop

---

## Functions

### Declaration
```c
int add(int a, int b) {
    return a + b;
}

void doSomething() {
    // no return value needed
}
```

### Entry Point
Every program must have a `main()` function:
```c
int main() {
    // program starts here
    return 0;
}
```

### Recursion
Fully supported:
```c
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}
```

### Array Parameters
Arrays are passed by reference:
```c
void fill(int arr[], int size, int value) {
    for (int i = 0; i < size; i++) {
        arr[i] = value;
    }
}
```

---

## Callback Functions

TinyC supports **callback functions** that Tasmota calls automatically at specific events.
Simply define functions with these well-known names — no registration needed.

### Available Callbacks

| Function | Tasmota Hook | When Called | Use Case |
|----------|-------------|-------------|----------|
| `EveryLoop()` | FUNC_LOOP | Every main loop iteration (~1–5 ms) | Ultra-fast polling, bit-banging, time-critical I/O |
| `Every50ms()` | FUNC_EVERY_50_MSECOND | Every 50 ms (20x/sec) | Fast polling, radio receive, sensor sampling |
| `EverySecond()` | FUNC_EVERY_SECOND | Every 1 second | Periodic tasks, counters, slow polling |
| `JsonCall()` | FUNC_JSON_APPEND | Telemetry cycle (~300s) | Add JSON to MQTT telemetry |
| `WebPage()` | FUNC_WEB_ADD_MAIN_BUTTON | Page load (once) | Charts, custom HTML, scripts |
| `WebCall()` | FUNC_WEB_SENSOR | Web page refresh (~1s) | Add sensor rows to Tasmota web UI |
| `WebUI()` | AJAX /tc_ui refresh | Every 2s + on widget change | Interactive widget dashboard (buttons, sliders, etc.) |
| `UdpCall()` | UDP packet received | On each multicast variable | Process incoming UDP variables |
| `WebOn()` | Custom HTTP endpoint | On request to `webOn()` URL | REST APIs, JSON endpoints, webhooks |
| `TaskLoop()` | FreeRTOS task (ESP32) | Continuous loop in own task | Background processing, independent of main thread |
| `CleanUp()` | FUNC_SAVE_BEFORE_RESTART | Before device restart | Close files, flush data, release resources |
| `TouchButton(btn, val)` | Touch event | On GFX button/slider touch | Handle touch button presses and slider changes |
| `HomeKitWrite(dev, var, val)` | HomeKit write | When Apple Home changes a value | Control lights, switches, outlets from Apple Home |
| `Command(char cmd[])` | Custom console command | When user types registered prefix in console | Handle custom Tasmota commands (e.g., MP3Play, MP3Stop) |
| `Event(char cmd[])` | Tasmota event rule trigger | On `Event` command from rules or console | React to Tasmota rule events |
| `OnExit()` | Script stop | When VM is stopped or script replaced | Close serial ports, release resources |
| `OnMqttConnect()` | FUNC_MQTT_INIT | MQTT broker connected | Subscribe topics, publish status |
| `OnMqttDisconnect()` | mqtt_disconnected flag | MQTT broker disconnected | Set offline state, stop publishing |
| `OnInit()` | First FUNC_NETWORK_UP | Once after first WiFi connect | One-time init: start services, subscribe MQTT |
| `OnWifiConnect()` | FUNC_NETWORK_UP | WiFi/network connected (every time) | Reconnect handling |
| `OnWifiDisconnect()` | FUNC_NETWORK_DOWN | WiFi/network lost | Pause network-dependent tasks |
| `OnTimeSet()` | FUNC_TIME_SYNCED | NTP time synchronized | Schedule time-based actions |

### Execution Model

1. **`main()`** runs first in a FreeRTOS task (ESP32) — `delay()` works as real blocking delay
2. After main halts, **globals and heap persist** — they are NOT freed
3. Tasmota periodically calls your callbacks, which can read/modify globals
4. Callbacks run synchronously with an instruction limit — no `delay()` allowed
5. If `TaskLoop()` is defined, it runs in the same FreeRTOS task after main() halts — `delay()` works, runs independently of Tasmota's main thread

### Tasmota Output Functions

Use these functions in callbacks to send data to Tasmota:

| Function | Description | Use In |
|----------|-------------|--------|
| `responseAppend(buf)` | Append char array to JSON telemetry (→ `ResponseAppend_P`) | `JsonCall()` |
| `responseAppend("literal")` | Append string literal to JSON telemetry | `JsonCall()` |
| `webSend(buf)` | Send char array to web page (→ `WSContentSend`) | `WebPage()` / `WebCall()` / `WebOn()` |
| `webSend("literal")` | Send string literal to web page | `WebPage()` / `WebCall()` / `WebOn()` |
| `webFlush()` | Flush web content buffer to client (→ `WSContentFlush`) | `WebPage()` / `WebCall()` / `WebOn()` |
| `webSendFile("filename")` | Send file contents from filesystem to web page | `WebPage()` / `WebCall()` / `WebUI()` / `WebOn()` |
| `addCommand("prefix")` | Register custom console command prefix (e.g., `"MP3"` → MP3Play, MP3Stop) | `main()` |
| `responseCmnd(buf)` | Send char array as console command response | `Command()` |
| `responseCmnd("literal")` | Send string literal as console command response | `Command()` |

### Web Page Format

Use Tasmota's `{s}` `{m}` `{e}` macros in `webSend()` to create table rows:
- `{s}` — start row (label column)
- `{m}` — middle (value column)
- `{e}` — end row

Example: `"{s}Temperature{m}25.3 °C{e}"` renders as a labeled row on the web page.

### JSON Telemetry Format

Use `responseAppend()` to add JSON fragments. Start with a comma:
- `",\"Sensor\":{\"Temp\":25}"` appends to the telemetry JSON

### Example

```c
int counter = 0;

void EverySecond() {
    counter++;
}

void JsonCall() {
    // Appends to Tasmota MQTT telemetry JSON
    char buf[64];
    sprintfInt(buf, ",\"TinyC\":{\"Count\":%d}", counter);
    responseAppend(buf);
}

void WebCall() {
    // Adds a row to the Tasmota web page
    char buf[64];
    sprintfInt(buf, "{s}TinyC Counter{m}%d{e}", counter);
    webSend(buf);
}

int main() {
    counter = 0;
    return 0;
}
```

**Result:** After uploading and running, the Tasmota web page shows a "TinyC Counter" row that increments every second, and MQTT telemetry includes `,"TinyC":{"Count":N}`.

### Custom Console Commands

Scripts can register custom Tasmota console commands using `addCommand("prefix")`. When a user types e.g. `MP3Play Sound.mp3` in the console, Tasmota matches the prefix `"MP3"`, extracts the subcommand `"PLAY SOUND.MP3"`, and calls `Command("PLAY SOUND.MP3")` on the script.

**Note:** Tasmota uppercases the command topic, so subcommands arrive as `"PLAY"`, `"STOP"`, etc. Data after a space (filenames, numbers) keeps its original case.

```c
int volume = 15;

void Command(char cmd[]) {
    char buf[64];
    if (strFind(cmd, "PLAY") == 0) {
        // handle play
        responseCmnd("Playing");
    } else if (strFind(cmd, "STOP") == 0) {
        responseCmnd("Stopped");
    } else if (strFind(cmd, "VOL") == 0) {
        char arg[16];
        strSub(arg, cmd, 4, 0);  // extract everything after "VOL "
        volume = atoi(arg);
        sprintfInt(buf, "Volume: %d", volume);
        responseCmnd(buf);
    } else {
        responseCmnd("Unknown: Play|Stop|Vol");
    }
}

int main() {
    addCommand("MP3");   // register "MP3" prefix
    return 0;
}
```

**Result:** Typing `MP3Play` in the Tasmota console calls `Command("PLAY")`, typing `MP3Vol 20` calls `Command("VOL 20")`.

### TaskLoop Example (ESP32)

```c
int counter = 0;

void TaskLoop() {
    counter++;
    char buf[64];
    sprintfInt(buf, "TaskLoop count=%d", counter);
    addLog(buf);       // appears in Tasmota console log
    delay(1000);       // real 1-second delay, doesn't block Tasmota
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

**Result:** `TaskLoop()` runs independently in a FreeRTOS task, incrementing the counter every second. `JsonCall()` reports the counter in MQTT telemetry. Both run concurrently — the mutex ensures safe VM access.

### Important Notes

- Callbacks must be **fast** — max 200,000 instructions (ESP32) / 20,000 (ESP8266) per invocation
- No `delay()` in callbacks (capped at 100ms if called) — except `TaskLoop()` which supports real delays
- `main()` must return (not loop forever) for callbacks to activate
- Only the eight well-known names above are recognized
- The compiler auto-detects these function names and embeds them in the binary
- `EveryLoop()` runs every main loop iteration (~1–5 ms) — keep it **very short** to avoid blocking Tasmota
- `Every50ms()` is ideal for fast, non-blocking I/O polling (SPI radio, GPIO, etc.)
- Use `WebPage()` for one-time page content (charts, scripts) — called once when page loads
- Use `WebCall()` for sensor-style rows that refresh periodically
- Use `UdpCall()` to process incoming UDP multicast variables
- `TaskLoop()` runs in a dedicated FreeRTOS task (ESP32 only) — can use `delay()` freely, VM access is mutex-serialized with main-thread callbacks

---

## Tasmota System Variables

TinyC provides virtual `tasm_*` variables that read/write Tasmota system state directly. They are used like normal variables — no function calls needed. The compiler translates them to syscalls automatically.

### Available Variables

| Variable | Type | R/W | Description |
|----------|------|-----|-------------|
| `tasm_wifi` | int | read | WiFi status (1 = connected, 0 = disconnected) |
| `tasm_mqttcon` | int | read | MQTT connection status (1 = connected) |
| `tasm_teleperiod` | int | read/write | Telemetry period in seconds (10–3600, clamped) |
| `tasm_uptime` | int | read | Device uptime in seconds |
| `tasm_heap` | int | read | Free heap memory in bytes |
| `tasm_power` | int | read/write | Relay power state (bitmask, write toggles relay) |
| `tasm_dimmer` | int | read/write | Dimmer level 0–100 (write sends Dimmer command) |
| `tasm_temp` | float | read | Temperature from Tasmota sensor (global `TempRead()`) |
| `tasm_hum` | float | read | Humidity from Tasmota sensor (global `HumRead()`) |
| `tasm_hour` | int | read | Current hour (0–23, from RTC) |
| `tasm_minute` | int | read | Current minute (0–59, from RTC) |
| `tasm_second` | int | read | Current second (0–59, from RTC) |
| `tasm_year` | int | read | Current year (e.g. 2026, from RTC) |
| `tasm_month` | int | read | Current month (1–12, from RTC) |
| `tasm_day` | int | read | Day of month (1–31, from RTC) |
| `tasm_wday` | int | read | Day of week (1=Sun, 2=Mon, … 7=Sat) |
| `tasm_cw` | int | read | ISO calendar week (1–53) |
| `tasm_sunrise` | int | read | Sunrise, minutes since midnight (requires USE_SUNRISE) |
| `tasm_sunset` | int | read | Sunset, minutes since midnight (requires USE_SUNRISE) |
| `tasm_time` | int | read | Current time, minutes since midnight |
| `tasm_pheap` | int | read | Free PSRAM in bytes (ESP32 only, 0 on ESP8266) |
| `tasm_smlj` | int | read/write | SML JSON output enable/disable (requires USE_SML_M) |
| `tasm_npwr` | int | read | Number of power (relay) devices |

### Indexed Tasmota State Functions

| Function | Description |
|----------|-------------|
| `int tasmPower(int index)` | Power state of relay `index` (0-based). Returns 0 or 1 |
| `int tasmSwitch(int index)` | Switch state (0-based, Switch1 = index 0). Returns -1 if invalid |
| `int tasmCounter(int index)` | Pulse counter value (0-based, Counter1 = index 0). Requires USE_COUNTER |

### Tasmota String Info

`int tasmInfo(int sel, char buf[])` — fills `buf` with a Tasmota info string. Returns string length.

| sel | Content |
|-----|---------|
| 0 | MQTT topic |
| 1 | MAC address |
| 2 | Local IP address |
| 3 | Friendly name |
| 4 | Device name |
| 5 | MQTT group topic |
| 6 | Reset reason (string) |

**Example:**
```c
char topic[64];
tasmInfo(0, topic);    // get MQTT topic
char ip[20];
tasmInfo(2, ip);       // get local IP
```

### Usage

```c
// Read system state
if (tasm_wifi) {
    printStr("WiFi connected\n");
}

// Read sensor values (float)
float t = tasm_temp;
float h = tasm_hum;

// Read real-time clock
int h = tasm_hour;       // 0–23
int m = tasm_minute;     // 0–59
int s = tasm_second;     // 0–59
int y = tasm_year;       // e.g. 2026
int mo = tasm_month;     // 1–12
int d = tasm_day;        // 1–31
int wd = tasm_wday;      // 1=Sun..7=Sat
int cw = tasm_cw;        // ISO calendar week 1–53

// Sunrise/sunset automation
int now = tasm_time;     // minutes since midnight
if (now > tasm_sunset || now < tasm_sunrise) {
    tasm_power = 1;      // night — turn on light
}

// Write system state
tasm_teleperiod = 60;    // set telemetry to 60 seconds
tasm_power = 1;          // turn relay ON
tasm_dimmer = 50;        // set dimmer to 50%
```

### Notes

- **No declaration needed** — `tasm_*` names are recognized by the compiler automatically
- **No global slot used** — they don't consume global variable space
- **Read-only enforcement** — writing to read-only variables (e.g., `tasm_wifi = 1`) gives a compile-time error
- **Float type inference** — `tasm_temp` and `tasm_hum` are correctly typed as `float` in expressions
- **Write side-effects** — `tasm_power` executes `Power` command, `tasm_dimmer` executes `Dimmer` command, `tasm_teleperiod` updates Tasmota's Settings directly
- In the browser IDE, all variables return simulated values

### Example — Auto Power Control

```c
void EverySecond() {
    // Turn off relay if temperature too high
    if (tasm_temp > 30.0) {
        tasm_power = 0;
    }

    // Report via web
    char buf[64];
    sprintfFloat(buf, "{s}Temp{m}%.1f C{e}", tasm_temp);
    webSend(buf);
}

int main() {
    tasm_teleperiod = 30;  // fast telemetry for testing
    return 0;
}
```

---

## Arrays

### Declaration & Initialization
```c
int data[10];                       // uninitialized
int primes[5] = {2, 3, 5, 7, 11};  // with initializer
float values[3] = {1.5, 2.5};      // partial init
char name[32] = "TinyC";           // string init (null-terminated)
char greeting[] = "Hello World";    // size inferred from string (12)
int flags[] = {1, 0, 1, 1};        // size inferred from initializer (4)
```

When the size is omitted (`[]`), the compiler infers it automatically:
- **String initializer:** size = string length + 1 (for null terminator)
- **Array initializer:** size = number of elements

### Access
```c
int x = data[0];       // read
data[3] = 42;          // write
data[i + 1] = data[i]; // computed index
```

### Scope
- **Small arrays (≤16 elements)** — stored inline in global data or local frame (fast direct access)
- **Large arrays (>16 elements)** — automatically allocated on the VM heap

### Array Memory

Arrays with up to 16 elements are stored inline in the global or local frame for fast direct access. Arrays with more than 16 elements are automatically routed to the VM heap by the compiler — no special syntax needed:

```c
int rgb[3];            // inline (3 ≤ 16) — fast direct access
char buf[128];         // heap (128 > 16) — automatic allocation
float data[2000];      // heap (2000 > 16)

int main() {
    rgb[0] = 255;       // direct frame access
    buf[0] = 'H';       // heap access — same syntax
    data[1999] = 3.14;  // heap access
    return 0;
}
```

Both inline and heap arrays support all the same operations: element access, string operations on `char[]`, passing to functions, etc.

**Heap limits:**

**Heap limits:**

| Platform | Max Heap Slots | Max Handles |
|----------|---------------|-------------|
| ESP8266  | 2,048 (8 KB)  | 8           |
| ESP32    | 8,192 (32 KB) | 16          |
| Browser  | 16,384 (64 KB)| 32          |

---

## Strings

Strings in TinyC are `char` arrays with null termination.

### Declaration
```c
char greeting[32] = "Hello";
char buffer[64];    // uninitialized buffer
```

### String Assignment & Concatenation with `+`

The `=` and `+=` operators work on `char[]` variables for intuitive string handling:

```c
char buf[64];
char name[16] = "World";

// Assign string literal or char array
buf = "Hello";          // same as strcpy(buf, "Hello")
buf = name;             // same as strcpy(buf, name)

// Append with +=
buf += " ";             // same as strcat(buf, " ")
buf += name;            // same as strcat(buf, name)

// Concatenate with +
buf = buf + "!";        // same as strcat(buf, "!")
buf = buf + name;       // same as strcat(buf, name)
```

**Note:** The `+` operator only works when the left side of `=` is the same variable as the left side of `+` (i.e., `buf = buf + ...`). Cross-variable concatenation like `a = b + c` is not supported — use `strcpy` + `strcat` for that.

### Built-in String Functions
```c
int len = strlen(greeting);             // length (excluding \0)
strcpy(buffer, greeting);               // copy array to array
strcpy(buffer, "World");                // copy literal to array
strcat(buffer, greeting);               // append array
strcat(buffer, "!");                    // append literal
int cmp = strcmp(greeting, buffer);     // compare: -1, 0, or 1
printString(greeting);                  // print string to output
```

### Formatted String Output (sprintf)

Format a single value into a char array. The compiler auto-detects the value type:

```c
char line[64];
char name[16];
float pi = 3.14;

sprintf(line, "x = %d", 42);              // "x = 42"
sprintf(line, "pi = %.2f", pi);           // "pi = 3.14"
sprintf(line, "name: %s", name);          // "name: World"
```

### Building Multi-Value Strings (sprintfAppend)

Use `sprintfAppend` to chain multiple values into one buffer. It appends at the current end of the string:

```c
char report[128];
sprintf(report, "Sensor %d", 1);               // "Sensor 1"
sprintfAppend(report, " name=%s", name);        // "Sensor 1 name=World"
sprintfAppend(report, " val=%d", 42);           // "Sensor 1 name=World val=42"
sprintfAppend(report, " temp=%.1f", 3.14);      // "Sensor 1 name=World val=42 temp=3.1"
printString(report);
```

| Function | Description |
|----------|-------------|
| `sprintf(char dst[], "fmt", val)` | Format value into dst (overwrites). Type auto-detected. |
| `sprintfAppend(char dst[], "fmt", val)` | Format value and append to dst. Type auto-detected. |

> **Legacy aliases:** The explicit-type variants `sprintfInt`, `sprintfFloat`, `sprintfStr`, `sprintfAppendInt`, `sprintfAppendFloat`, `sprintfAppendStr` are still supported for backward compatibility.

**Format specifiers:** `%d` (int), `%f` `%.2f` `%e` `%g` (float), `%s` (string). Each call handles exactly one `%` specifier.

### String Manipulation

```c
char src[64] = "hello,world,test";
char dst[32];

// Extract nth token (1-based) by delimiter
int len = strToken(dst, src, ',', 2);  // dst = "world", len = 5

// Substring (0-based position, length)
strSub(dst, src, 6, 5);               // dst = "world"
strSub(dst, src, -4, 4);              // dst = "test" (negative = from end)

// Find substring position (-1 if not found)
int pos = strFind(src, "world");       // pos = 6
int no = strFind(src, "xyz");          // no = -1
```

| Function | Description |
|----------|-------------|
| `strToken(char dst[], char src[], int delim, int n)` | Copy nth token (1-based) delimited by char `delim` into dst. Returns token length. |
| `strSub(char dst[], char src[], int pos, int len)` | Copy `len` chars starting at `pos` (0-based, negative=from end) into dst. `len=0` copies to end of string. Returns actual length. |
| `strFind(char haystack[], char needle[])` | Find first occurrence of needle in haystack. Returns position (0-based) or -1 if not found. |
| `int strToInt(char str[])` | Convert string to integer (like `atoi`). Returns 0 if not a valid number. |
| `float strToFloat(char str[])` | Convert string to float (like `atof`). Returns 0.0 if not a valid number. |

### Array Sort

Sort an array in-place:

| Function | Description |
|----------|-------------|
| `sortArray(int arr[], int count, int flags)` | Sort array in-place. `flags`: 0=int ascending, 1=float ascending, 2=int descending, 3=float descending |
| `arrayFill(int arr[], int value, int count)` | Fill first `count` elements with `value` |
| `arrayCopy(int dst[], int src[], int count)` | Copy `count` elements from `src` to `dst` |
| `int smlCopy(int arr[], int count)` | Copy SML decoder values into float array. Returns number copied (requires USE_SML_M) |

```c
int data[5] = {42, 7, 99, 3, 55};
sortArray(data, 5, 0);    // ascending: {3, 7, 42, 55, 99}
sortArray(data, 5, 2);    // descending: {99, 55, 42, 7, 3}
arrayFill(data, 0, 5);    // zero all elements
```

### Character Access
```c
char ch = greeting[0];     // read: 'H'
greeting[0] = 'h';         // write: now "hello"
```

### Escape Sequences in Strings
| Escape | Character      |
|--------|----------------|
| `\n`   | Newline        |
| `\t`   | Tab            |
| `\r`   | Carriage return|
| `\\`   | Backslash      |
| `\"`   | Double quote   |
| `\'`   | Single quote   |
| `\0`   | Null terminator|

---

## Preprocessor

### #define — Compile-Time Constants

Simple compile-time constants (no macro expansion):
```c
#define LED_PIN 5
#define MAX_SIZE 100
#define PI 3.14
#define DOUBLE_PI (PI * 2)
```

**Features:**
- Value must be a constant expression
- Supports arithmetic on other `#define` values: `+`, `-`, `*`, `/`
- Used for array sizes, function arguments, etc.
- Scope: entire program
- Valueless defines allowed for conditionals: `#define ESP32`

**Limitations:**
- No `#include`

### Function-Like Macros

Parameterized macros perform text substitution before compilation:

```c
#define LOG(A) addLog(A)
#define CLAMP(V, MX) min(max(V, 0), MX)
#define SQUARE(X) (X * X)
```

**Usage:**
```c
LOG("sensor init");          // → addLog("sensor init")
int v = CLAMP(reading, 100); // → int v = min(max(reading, 0), 100)
int s = SQUARE(5);           // → int s = (5 * 5)
```

**Features:**
- Parameters are replaced by whole-word matching (won't replace partial identifiers)
- Nested parentheses in arguments are handled correctly: `LOG(foo(1,2))` works
- String literal arguments are preserved: `LOG("hello, world")` — the comma inside quotes is not treated as an argument separator
- Nested macro expansion: macros in the expanded body are expanded (up to 10 iterations)
- Multiple parameters supported: `#define ADD(A, B) (A + B)`

**Empty body macros — debug stripping:**
```c
#define DBG(M)              // empty body — no replacement text

DBG("checkpoint 1");        // → stripped entirely (including semicolon)
int x = 42;                 // this line is unaffected
```

Empty-body macros remove the entire invocation including the trailing semicolon. This is useful for stripping debug calls in production builds:

```c
#ifdef DEBUG
  #define DBG(M) addLog(M)
#else
  #define DBG(M)
#endif

DBG("init done");  // logs in debug, stripped in release
```

### Conditional Compilation

```c
#define ESP32
#define USE_SENSOR

#ifdef ESP32
  int pin = 8;       // included — ESP32 is defined
#else
  int pin = 2;       // excluded
#endif

#ifndef USE_DISPLAY
  // included — USE_DISPLAY is not defined
#endif
```

| Directive               | Description                                      |
|-------------------------|--------------------------------------------------|
| `#define NAME`          | Define a name (no value, for conditionals)       |
| `#define NAME value`    | Define a name with a constant value              |
| `#define NAME(A) body`  | Function-like macro with text substitution       |
| `#undef NAME`          | Undefine a previously defined name               |
| `#ifdef NAME`          | Include block if NAME is defined                 |
| `#ifndef NAME`         | Include block if NAME is NOT defined             |
| `#if EXPR`             | Include block if expression is non-zero          |
| `#else`                | Alternative block                                |
| `#endif`               | End conditional block                            |

**`#if` expressions** support:
- Integer literals: `#if 1`, `#if 0`
- Defined names (1 if defined, 0 if not): `#if ESP32`
- `defined(NAME)` operator: `#if defined(ESP32)`
- Logical operators: `&&`, `||`, `!`
- Comparison: `==`, `!=`, `>`, `<`, `>=`, `<=`
- Parentheses for grouping

```c
#if defined(ESP32) && !defined(USE_LEGACY)
  // ESP32-specific modern code
#endif
```

**Notes:**
- Conditionals can be nested
- Skipped code is not compiled (does not need to be valid syntax)
- Line numbers in error messages are preserved

---

## Comments

```c
// Single-line comment

/* Multi-line
   comment */
```

---

## Type Casting

### Explicit Casts
```c
float f = 3.14;
int i = (int)f;         // truncates to 3

int x = 42;
float y = (float)x;     // converts to 42.0

int ch = 321;
char c = (char)ch;      // masks to 0xFF → 65 ('A')

int b = (bool)42;       // non-zero → 1
```

### Implicit Conversions
When mixing `int` and `float` in an expression, the `int` operand is automatically promoted to `float`:
```c
int a = 5;
float b = 2.5;
float c = a + b;    // a promoted to float, result = 7.5
```

---

## Built-in Functions

### Output

| Function                | Description                      |
|-------------------------|----------------------------------|
| `print(int value)`      | Print integer + newline          |
| `print("literal")`     | Print string literal (auto-detected) |
| `print(char buf[])`    | Print char array as string (auto-detected) |
| `printStr("literal")`   | Print string literal (explicit)  |
| `printString(char arr[])` | Print null-terminated char array (explicit) |

> **Note:** `print()` auto-detects the argument type. When passed a string literal, it prints the string. When passed a `char[]` array, it prints the array contents as a string. When passed an `int`, it prints the numeric value. The explicit `printStr`/`printString` functions are still available but rarely needed.

### GPIO

| Function                             | Description                          |
|--------------------------------------|--------------------------------------|
| `pinMode(int pin, int mode)`         | Set pin mode (1=INPUT, 3=OUTPUT, 5=INPUT_PULLUP, 9=INPUT_PULLDOWN) |
| `digitalWrite(int pin, int value)`   | Write HIGH(1) or LOW(0)             |
| `int digitalRead(int pin)`           | Read pin state                       |
| `int analogRead(int pin)`            | Read analog value (0–4095)           |
| `analogWrite(int pin, int value)`    | Write PWM value                      |
| `gpioInit(int pin, int mode)`        | Release pin from Tasmota + pinMode   |

### Timing

| Function                         | Description                     |
|----------------------------------|---------------------------------|
| `delay(int ms)`                  | Wait milliseconds               |
| `delayMicroseconds(int us)`      | Wait microseconds               |
| `int millis()`                   | Milliseconds since program start|
| `int micros()`                   | Microseconds since program start|

### Software Timers

4 independent countdown timers (IDs 0-3) based on `millis()`. Timers run independently of callbacks — set a timer in `main()` or any callback, check it in `EveryLoop()`.

| Function                              | Description                                          |
|---------------------------------------|------------------------------------------------------|
| `timerStart(int id, int ms)`          | Start timer `id` (0-3) with `ms` millisecond timeout |
| `int timerDone(int id)`               | Returns 1 if timer expired (or never started), 0 if running |
| `timerStop(int id)`                   | Cancel timer                                         |
| `int timerRemaining(int id)`          | Milliseconds remaining (0 if expired/stopped)        |

**Example — repeating timer with timeout:**
```c
int counter;

void main() {
    counter = 0;
    timerStart(0, 5000);    // timer 0: every 5 seconds
    timerStart(1, 60000);   // timer 1: stop after 1 minute
}

void EveryLoop() {
    if (timerDone(0)) {
        counter++;
        print(counter);
        timerStart(0, 5000);  // restart for next interval
    }
    if (timerDone(1)) {
        timerStop(0);         // stop repeating timer
    }
}
```

### Serial

| Function                          | Description                        |
|-----------------------------------|------------------------------------|
| `serialBegin(int baud)`           | Initialize serial at baud rate     |
| `serialPrint("literal")`          | Print string to serial             |
| `serialPrintInt(int value)`       | Print integer to serial            |
| `serialPrintFloat(float value)`   | Print float to serial              |
| `serialPrintln("literal")`        | Print string + newline to serial   |
| `int serialRead()`                | Read byte (-1 if none available)   |
| `int serialAvailable()`           | Bytes available to read            |
| `serialClose()`                   | Close serial port                  |
| `serialWriteByte(int b)`          | Write single byte to serial        |
| `serialWriteStr(char str[])`      | Write char array to serial (binary-safe) |
| `serialWriteBuf(char buf[], int len)` | Write `len` bytes from buffer to serial |

### 1-Wire

| Function                          | Description                                                |
|-----------------------------------|------------------------------------------------------------|
| `owSetPin(int pin)`               | Set GPIO pin for native 1-Wire bus                         |
| `int owReset()`                   | Send reset pulse, return 1 if presence detected            |
| `owWrite(int byte)`               | Write one byte to the bus                                  |
| `int owRead()`                    | Read one byte from the bus                                 |
| `owWriteBit(int bit)`             | Write a single bit (0 or 1)                                |
| `int owReadBit()`                 | Read a single bit                                          |
| `owSearchReset()`                 | Reset the ROM search state                                 |
| `int owSearch(char rom[])`        | Find next device, store 8-byte ROM in `rom[]`, return 1 if found |

> The native 1-Wire functions use hardware-timed bit-banging in C — no external library needed. Requires a 4.7 kΩ pull-up resistor on the data line. For long buses or noisy environments, use a DS2480B serial-to-1-Wire bridge (see `examples/onewire.tc`).

### Math

| Function                                            | Description                     |
|-----------------------------------------------------|---------------------------------|
| `int abs(int value)`                                | Absolute value                  |
| `int min(int a, int b)`                             | Minimum of two values           |
| `int max(int a, int b)`                             | Maximum of two values           |
| `int map(int val, int fLo, int fHi, int tLo, int tHi)` | Map value from one range to another |
| `int random(int min, int max)`                      | Random integer in range         |
| `float sqrt(float x)`                               | Square root                     |
| `float sin(float x)`                                | Sine (radians)                  |
| `float cos(float x)`                                | Cosine (radians)                |
| `float exp(float x)`                                | Exponential (e^x)               |
| `float log(float x)`                                | Natural logarithm (ln x)       |
| `float pow(float base, float exp)`                   | Power (base^exp)                |
| `float acos(float x)`                               | Inverse cosine (radians)        |
| `float intBitsToFloat(int bits)`                     | Reinterpret int as IEEE 754 float |
| `int floor(float x)`                                | Integer part (round toward −∞)  |
| `int ceil(float x)`                                 | Integer part + 1 (round toward +∞) |
| `int round(float x)`                                | Round to nearest integer        |

### String

| Function                             | Description                         |
|--------------------------------------|-------------------------------------|
| `int strlen(char arr[])`             | String length (excluding null)      |
| `strcpy(char dst[], char src[])`     | Copy string                         |
| `strcpy(char dst[], "literal")`      | Copy literal into array             |
| `strcat(char dst[], char src[])`     | Concatenate string                  |
| `strcat(char dst[], "literal")`      | Concatenate literal                 |
| `int strcmp(char a[], char b[])`     | Compare: returns -1, 0, or 1       |
| `printString(char arr[])`            | Print string to output              |

**String operators:** `char[]` variables also support `=`, `+=`, and `+` for string assignment and concatenation — see [Strings](#strings) section.

### sprintf — Formatted Strings

Format a single value into a char array. The compiler auto-detects the value type from the 3rd argument. Each call handles one `%` specifier.

| Function | Description |
|----------|-------------|
| `int sprintf(char dst[], "fmt", val)` | Format value into dst (overwrites). Type auto-detected. |
| `int sprintfAppend(char dst[], "fmt", val)` | Format value, append to end of dst. Type auto-detected. |

> **Legacy aliases:** `sprintfInt`, `sprintfFloat`, `sprintfStr`, `sprintfAppendInt`, `sprintfAppendFloat`, `sprintfAppendStr` still work.

**Format specifiers:** `%d` (int), `%f` `%.Nf` `%e` `%g` (float), `%s` (string).
All functions return the total string length.

```c
// Build a multi-value string by chaining Append calls:
char buf[128];
sprintf(buf, "ID=%d", 1);
sprintfAppend(buf, " name=%s", name);
sprintfAppend(buf, " val=%.1f", 3.14);
// buf = "ID=1 name=World val=3.1"
```

### File I/O

Read and write files on the ESP32 filesystem (LittleFS). In the browser IDE, files are simulated in a virtual filesystem.

| Function                                   | Description                                      |
|--------------------------------------------|--------------------------------------------------|
| `int fileOpen("path", mode)`               | Open file, returns handle (0–3) or -1 on error   |
| `int fileClose(handle)`                    | Close file handle, returns 0 or -1               |
| `int fileRead(handle, char buf[], max)`    | Read up to max bytes into buf, returns count     |
| `int fileWrite(handle, char buf[], len)`   | Write len bytes from buf, returns count          |
| `int fileExists("path")`                   | Check if file exists: 1=yes, 0=no                |
| `int fileDelete("path")`                   | Delete file, returns 0=ok, -1=error              |
| `int fileSize("path")`                     | Get file size in bytes, -1 on error              |
| `int fileSeek(handle, offset, whence)`     | Seek to position. Returns 1=ok, 0=fail           |
| `int fileTell(handle)`                     | Get current position in file, -1 on error        |
| `int fsInfo(int sel)`                      | Filesystem info: sel=0 → total KB, sel=1 → free KB |
| `int fileOpenDir("path")`                  | Open directory for listing, returns handle or -1 |
| `int fileReadDir(handle, char name[])`     | Read next filename into name. Returns 1=entry, 0=end |

**File modes:** `0` = read, `1` = write (create/truncate), `2` = append

**Seek whence:** `0` = SEEK_SET (from start), `1` = SEEK_CUR (from current), `2` = SEEK_END (from end)

**Notes:**
- File paths can be string literals (e.g., `"/data.txt"`) or `char[]` variables
- **Filesystem selection** (Scripter-compatible): default is SD card (`ufsp`). Use `/ffs/` prefix for flash, `/sdfs/` prefix for SD card explicitly: `fileOpen("/ffs/config.txt", 0)` opens from flash, `fileOpen("/data.txt", 0)` opens from SD card
- Maximum 4 files open simultaneously (ESP32), 8 in browser
- Buffer arguments (`buf`) must be `char` arrays, not string literals
- `fileRead` returns the number of bytes actually read (may be less than `max`)
- Always close files when done to free handles

```c
// Example: Write and read back
char data[32];
char buf[32];
strcpy(data, "Hello!\n");

int f = fileOpen("/test.txt", 1);   // write mode
fileWrite(f, data, strlen(data));
fileClose(f);

f = fileOpen("/test.txt", 0);       // read mode
int n = fileRead(f, buf, 31);
buf[n] = 0;
fileClose(f);
printString(buf);                    // prints "Hello!"

fileDelete("/test.txt");             // clean up

// Example: List files in a directory
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

**Directory listing notes:**
- `fileOpenDir` uses a file handle slot (same pool as `fileOpen`), close with `fileClose` when done
- `fileReadDir` returns filenames only (no path prefix), skips subdirectories
- Path argument can be a string literal or a char array variable

### Extended File Operations

Filesystem management, structured array I/O, and log file rotation.

| Function | Description |
|----------|-------------|
| `int fileFormat()` | Format LittleFS filesystem (erases all data). Returns 0=ok |
| `int fileMkdir("path")` | Create directory. Returns 1=ok, 0=fail |
| `int fileRmdir("path")` | Remove directory. Returns 1=ok, 0=fail |
| `int fileReadArray(int arr[], handle)` | Read one tab-delimited line into int array. Returns element count |
| `fileWriteArray(int arr[], handle)` | Write array as tab-delimited line with trailing newline |
| `fileWriteArray(int arr[], handle, append)` | Write with append flag: 1=omit newline (for appending multiple arrays on one line) |
| `int fileLog("fname", char str[], limit)` | Append string + newline to file. Remove first line if file exceeds `limit` bytes. Returns file size |
| `int fileDownload("fname", char url[])` | Download URL content to file. Returns HTTP status code (200=ok). Compatible with Scripter's `frw()` |
| `int fileGetStr(char dst[], handle, "delim", index, endChar)` | Search file from start for Nth occurrence of delimiter, extract string until endChar. Returns string length. Compatible with Scripter's `fcs()` |

**fileReadArray / fileWriteArray format:** Values are stored as decimal text separated by TAB characters, one array per line. This is compatible with Scripter's `fra()`/`fwa()` format.

```c
// Example: Save and load array data
int values[5];
values[0] = 100; values[1] = 200; values[2] = 300;
values[3] = 400; values[4] = 500;

int f = fileOpen("/data.tab", 1);    // write mode
fileWriteArray(values, f);           // writes "100\t200\t300\t400\t500\n"
fileClose(f);

int loaded[5];
f = fileOpen("/data.tab", 0);       // read mode
int n = fileReadArray(loaded, f);   // n = 5
fileClose(f);
```

```c
// Example: Rolling log file (max 4096 bytes)
char msg[64];
strcpy(msg, "Sensor reading: 23.5C");
fileLog("/log.txt", msg, 4096);
// Appends line, removes oldest line if file > 4096 bytes
```

```c
// Example: Download file from web
char url[128];
strcpy(url, "http://192.168.1.100/data.csv");
int status = fileDownload("/data.csv", url);
// status = 200 on success, negative on error
```

```c
// Example: Extract 2nd comma-delimited field from CSV file
// File content: "name,temperature,humidity\nSensor1,23.5,65\n"
int f = fileOpen("/data.csv", 0);       // open for reading
char value[32];
int len = fileGetStr(value, f, ",", 2, '\n');
// value = "23.5", len = 4 (content between 2nd comma and newline)
fileClose(f);
```

### File Data Extract (IoT Time-Series)

Extract a time range from tab-delimited CSV data files into float arrays for analysis. Designed for IoT data collectors that log sensor readings at regular intervals.

**Data file format:** First column is a timestamp (ISO or German locale), followed by tab-separated float values. First line may be a header (auto-skipped).

| Function | Description |
|----------|-------------|
| `int fileExtract(handle, char from[], char to[], col_offs, accum, int arr1[], ...)` | Extract rows where `from <= timestamp <= to`. Always seeks from file start. Returns row count |
| `int fileExtractFast(handle, char from[], char to[], col_offs, accum, int arr1[], ...)` | Same but caches file position for efficient sequential time-range queries |

**Parameters:**
- `handle` — open file handle (from `fileOpen`)
- `from`, `to` — timestamp range as char[] (ISO `2024-01-15T12:00:00` or German `15.1.24 12:00`)
- `col_offs` — skip this many data columns before distributing to arrays (0 = start at first data column)
- `accum` — 0: store values, 1: add to existing array values (for combining multiple extracts)
- `arr1, arr2, ...` — variable number of int arrays, one per column to extract (up to 16). Values are stored as IEEE 754 float bit patterns — use float variables or casts to read them

```c
// Example: Extract temperature and humidity for one day
int temp[96], hum[96];  // 96 = 24h * 4 (15-min intervals)
char from[24], to[24];
strcpy(from, "15.12.21 00:00");
strcpy(to, "16.12.21 00:00");

int f = fileOpen("/daily.csv", 0);
// col_offs=4 skips WB,WR1,WR2,WR3 → starts at ATMP_a (5th data col)
int rows = fileExtract(f, from, to, 4, 0, temp, hum);
fileClose(f);
// rows = number of 15-min samples, temp[] and hum[] filled with floats
```

```c
// Example: Sequential daily queries with fileExtractFast
int energy[96];
char from[24], to[24];
int f = fileOpen("/yearly.csv", 0);

strcpy(from, "1.1.24 00:00");
strcpy(to, "2.1.24 00:00");
int r1 = fileExtractFast(f, from, to, 0, 0, energy);
// Next day — fileExtractFast skips already-scanned data
strcpy(from, "2.1.24 00:00");
strcpy(to, "3.1.24 00:00");
int r2 = fileExtractFast(f, from, to, 0, 0, energy);
fileClose(f);
```

### Time / Timestamp Functions

Timestamp conversion and arithmetic. Supports ISO web format (`2024-01-15T12:30:45`) and German locale format (`15.1.24 12:30`). Compatible with Scripter's `tstamp`, `cts`, `tso`, `tsn`, `s2t`.

| Function | Description |
|----------|-------------|
| `int timeStamp(char buf[])` | Get current Tasmota local timestamp into buf. Returns 0 |
| `int timeConvert(char buf[], flg)` | Convert timestamp format in-place. 0=German→Web, 1=Web→German. Returns 0 |
| `int timeOffset(char buf[], days)` | Add `days` offset to timestamp in buf (in-place). Returns 0 |
| `int timeOffset(char buf[], days, zeroFlag)` | With `zeroFlag`=1: also zero the time portion (HH:MM:SS→00:00:00) |
| `int timeToSecs(char buf[])` | Convert timestamp string to epoch seconds. Returns seconds |
| `int secsToTime(char buf[], secs)` | Convert epoch seconds to ISO timestamp string in buf. Returns 0 |

**Format auto-detection:** `timeConvert` and `timeOffset` auto-detect the input format (ISO if contains `T`, German otherwise) and preserve or convert accordingly.

```c
// Example: Get current time and convert formats
char ts[24];
timeStamp(ts);               // ts = "2024-06-15T14:30:00"

char de[24];
strcpy(de, ts);
timeConvert(de, 1);          // de = "15.6.24 14:30"

timeConvert(de, 0);          // de = "2024-06-15T14:30:00" (back to web)
```

```c
// Example: Date arithmetic
char ts[24];
timeStamp(ts);               // "2024-06-15T14:30:00"
timeOffset(ts, 7);           // "2024-06-22T14:30:00" (+ 7 days)
timeOffset(ts, -3, 1);       // "2024-06-19T00:00:00" (- 3 days, zero time)
```

```c
// Example: Convert to seconds and back
char ts[24];
timeStamp(ts);
int secs = timeToSecs(ts);   // epoch seconds

secs = secs + 3600;          // add 1 hour
secsToTime(ts, secs);        // back to timestamp string
```

### Tasmota Command

Execute any Tasmota console command and capture the JSON response.

| Function                                     | Description                                    |
|----------------------------------------------|------------------------------------------------|
| `int tasmCmd("command", char response[])`    | Execute command (string literal), store response, return length |
| `int tasmCmd(char cmd[], char response[])`   | Execute command (char array), store response, return length |

**Notes:**
- Command can be a string literal (e.g., `"Status 0"`) or a `char[]` variable for dynamic commands
- Response buffer should be a `char` array (recommended size: 256)
- Returns length of response string, or -1 on error
- In the browser IDE, returns a simulated mock response
- On ESP32, executes real Tasmota commands and captures the JSON response

```c
char resp[256];
int len = tasmCmd("Status 0", resp);
if (len > 0) {
    printString(resp);   // prints JSON response
}
```

### Sensor JSON Parsing

Read any Tasmota sensor value by its JSON path. Path segments are separated by `#` (same convention as Tasmota Scripter).

| Function | Description |
|----------|-------------|
| `float sensorGet("Sensor#Key")` | Read sensor value, returns float |

The function internally triggers a sensor status read and navigates the JSON tree. Supports up to 3 levels of nesting.

```c
// Read BME280 sensor
float temp = sensorGet("BME280#Temperature");
float hum = sensorGet("BME280#Humidity");
float press = sensorGet("BME280#Pressure");

// Read SHT3X on address 0x44
float t = sensorGet("SHT3X_0x44#Temperature");

// Read energy meter (if USE_ENERGY_SENSOR defined)
float power = sensorGet("ENERGY#Power");
float voltage = sensorGet("ENERGY#Voltage");
float today = sensorGet("ENERGY#Today");

// Nested: Zigbee device
float zt = sensorGet("ZbReceived#0x2342#Temperature");
```

**Notes:**
- Path must be a string literal (resolved at compile time)
- Returns 0.0 if the sensor or key is not found
- Returns a float — assign to a `float` variable
- In the browser IDE, simulates Temperature=22.5, Humidity=55.0, Pressure=1013.25

### Localized Strings

Retrieve Tasmota's localized display strings at runtime. The strings match the firmware's compile-time language setting (e.g. `en_GB.h`, `de_DE.h`). Use these for web UI labels; JSON keys stay in English.

| Function | Description |
|----------|-------------|
| `int LGetString(int index, char dst[])` | Copy localized string to `dst`, returns length (0 if invalid index) |

**String Index Table:**

| Index | Tasmota Define | English |
|-------|---------------|---------|
| 0 | D_TEMPERATURE | Temperature |
| 1 | D_HUMIDITY | Humidity |
| 2 | D_PRESSURE | Pressure |
| 3 | D_DEWPOINT | Dew point |
| 4 | D_CO2 | Carbon dioxide |
| 5 | D_ECO2 | eCO2 |
| 6 | D_TVOC | TVOC |
| 7 | D_VOLTAGE | Voltage |
| 8 | D_CURRENT | Current |
| 9 | D_POWERUSAGE | Power |
| 10 | D_POWER_FACTOR | Power Factor |
| 11 | D_ENERGY_TODAY | Energy Today |
| 12 | D_ENERGY_YESTERDAY | Energy Yesterday |
| 13 | D_ENERGY_TOTAL | Energy Total |
| 14 | D_FREQUENCY | Frequency |
| 15 | D_ILLUMINANCE | Illuminance |
| 16 | D_DISTANCE | Distance |
| 17 | D_MOISTURE | Moisture |
| 18 | D_LIGHT | Light |
| 19 | D_SPEED | Speed |
| 20 | D_ABSOLUTE_HUMIDITY | Abs Humidity |

**Example:**
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
    web_row(0, temperature, "&deg;C");  // "Temperature" or localized
    web_row(1, humidity, "%");           // "Humidity" or localized
    web_row(2, pressure, "hPa");         // "Pressure" or localized
}
```

### Tasmota Output (Callbacks)

Send data directly to Tasmota's telemetry and web systems from callback functions.

| Function | Description |
|----------|-------------|
| `void responseAppend(char buf[])` | Append string to MQTT JSON telemetry (`ResponseAppend_P`) |
| `void responseAppend("literal")` | Append string literal to JSON (no buffer needed) |
| `void webSend(char buf[])` | Send string to web page HTML (`WSContentSend`) |
| `void webSend("literal")` | Send string literal to web page (no buffer needed) |
| `void webFlush()` | Flush web content buffer to client (`WSContentFlush`) |
| `void addLog(char buf[])` | Write message to Tasmota log (`AddLog` at INFO level) |
| `void addLog("literal")` | Write string literal to Tasmota log |
| `webSendJsonArray(float arr[], int count)` | Emit float array as JSON integer array in web response |

**Notes:**
- `addLog`, `webSend` and `responseAppend` accept either a char array or a string literal
- String literal variants are more efficient — no copy through a buffer, sent directly from constant pool
- Use `responseAppend()` inside `JsonCall()` — appends to the MQTT telemetry JSON
- Use `webSend()` inside `WebPage()` for one-time page content (charts, scripts, custom HTML)
- Use `webSend()` inside `WebCall()` for sensor-style rows that refresh periodically
- Use `{s}Label{m}Value{e}` format in `webSend()` for sensor-style table rows
- Call `webFlush()` periodically when building large HTML pages to flush the chunked transfer buffer (500 bytes)
- Start JSON with comma: `",\"Key\":value"` to append correctly to telemetry
- In the browser IDE, both route to the output console; `webFlush()` is a no-op
- Callback instruction limit: 200,000 (ESP32), 20,000 (ESP8266)
- See [Callback Functions](#callback-functions) for full examples

### HTTP Requests

Make HTTP GET/POST requests to external APIs. URLs can be string literals or dynamically built in char arrays. Requests are blocking with a 5-second timeout.

| Function | Description |
|----------|-------------|
| `int httpGet(char url[], char response[])` | HTTP GET, returns response length or negative error |
| `int httpPost(char url[], char data[], char response[])` | HTTP POST, returns response length or negative error |
| `void httpHeader(char name[], char value[])` | Set custom header for the next request |
| `int webParse(char source[], "delim", int index, char result[])` | Parse non-JSON response text (see below) |

**Return values:** `> 0` = response body length, `0` = empty response, negative = HTTP error code (e.g., -404).

**Example — Daikin aircon sensor query:**
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
        // Extract indoor temperature (htemp)
        pos = strFind(response, token);  // find "htemp="
        strToken(token, response, ',', 3);  // 3rd token = "htemp=19.0"
        printString(token);
    }
}
```

**Example — Tasmota command to another device:**
```c
char url[128];
char response[512];
int len;

void EverySecond() {
    strcpy(url, "http://192.168.1.100/cm?cmnd=Status%200");
    len = httpGet(url, response);
    if (len > 0) {
        print(len);
        // parse response with strFind/strToken...
    }
}
```

**Example — POST with custom header:**
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
    httpHeader(hname, hval);  // set header before request
    int len = httpPost(url, data, response);
}
```

**webParse() — Parse non-JSON web responses**

Equivalent to Scripter's `gwr()`. Extracts data from plain-text HTTP responses (key=value, CSV, line-based formats).

**Two modes:**
- **index > 0** — Split `source` by `delim`, return the Nth segment (1-based). Returns length.
- **index < 0** — Find `delim=value` pattern, extract value (stops at `,`, `:`, or NUL). Returns length.
- **index == 0** — No-op, returns 0.

**Example — Daikin aircon with webParse:**
```c
char url[64];
char response[256];
char value[32];

void main() {
    strcpy(url, "http://192.168.188.43/aircon/get_sensor_info");
    int len = httpGet(url, response);
    // response = "ret=OK,htemp=19.0,hhum=-,otemp=7.0,err=0,cmpfreq=0"

    if (len > 0) {
        // name=value mode: extract value after "htemp="
        webParse(response, "htemp", -1, value);  // value = "19.0"
        float temp = atof(value);
        print(temp);  // 19.0

        // split mode: get 4th comma-separated field
        webParse(response, ",", 4, value);  // value = "otemp=7.0"
        printString(value);
    }
}
```

### TCP Server

Start a TCP stream server to accept incoming connections. Only one client is served at a time.

| Function | Description |
|----------|-------------|
| `int tcpServer(int port)` | Start TCP server on `port`. Returns 0=ok, -1=fail, -2=no network |
| `tcpClose()` | Close TCP server and disconnect client |
| `int tcpAvailable()` | Accept pending client and return bytes available to read |
| `int tcpRead(char buf[])` | Read string from TCP client into `buf`. Returns bytes read |
| `tcpWrite(char str[])` | Write string to TCP client |
| `int tcpReadArray(int arr[])` | Read available bytes into int array (one byte per element). Returns count |
| `tcpWriteArray(int arr[], int num)` | Write `num` array elements as uint8 bytes to TCP client |
| `tcpWriteArray(int arr[], int num, int type)` | Write with type: 0=uint8, 1=uint16 BE, 2=sint16 BE, 3=float BE |

**Example — Simple TCP echo server:**
```c
char buf[128];

void main() {
    tcpServer(8888);   // listen on port 8888
}

void Every50ms() {
    int n = tcpAvailable();  // accept client + check available
    if (n > 0) {
        tcpRead(buf);        // read incoming string
        tcpWrite(buf);       // echo it back
    }
}
```

**Example — Binary data streaming:**
```c
int data[100];

void main() {
    tcpServer(9000);
}

void EverySecond() {
    int n = tcpAvailable();
    if (n > 0) {
        // read raw bytes into array
        int count = tcpReadArray(data);
        print(count);
        // send back as uint16 big-endian
        tcpWriteArray(data, count, 1);
    }
}
```

### mDNS Service Advertisement

Register the device as an mDNS service on the local network, enabling device emulation (Everhome ecotracker, Shelly, or custom services).

| Function | Description |
|----------|-------------|
| `int mdnsRegister("name", "mac", "type")` | Start mDNS responder and advertise service. Returns 0 on success |

**Parameters (all string literals):**
- **name** — hostname prefix. Use `"-"` for Tasmota's default hostname, or a custom prefix (MAC is appended automatically)
- **mac** — MAC address. Use `"-"` for device's own MAC (lowercase, no colons), or provide a custom string
- **type** — service type: `"everhome"` (ecotracker), `"shelly"`, or any custom service name

**Built-in emulation types:**
- `"everhome"` — registers `_everhome._tcp` with IP, serial, productid TXT records
- `"shelly"` — registers `_http._tcp` and `_shelly._tcp` with firmware metadata TXT records
- Any other string — registers `_<type>._tcp` with IP and serial TXT records

**Example — Everhome ecotracker emulation:**
```c
int main() {
    mdnsRegister("ecotracker-", "-", "everhome");
    return 0;
}
```

This is equivalent to Scripter's `mdnsRegister("ecotracker-", "-", "everhome")`.

### WebUI Widgets

Create interactive dashboards using widget functions. Widgets can appear in two places:

1. **Dedicated `/tc_ui` page** — use the `WebUI()` callback
2. **Tasmota main page** (sensor section) — use the `WebCall()` callback

Both callbacks use the same widget functions.

| Function | Description |
|----------|-------------|
| `webButton(var, "label")` | Toggle button (0/1) — displays ON/OFF, click toggles |
| `webSlider(var, min, max, "label")` | Range slider — drag to set value |
| `webCheckbox(var, "label")` | Checkbox (0/1) — check/uncheck toggles |
| `webText(chararray, maxlen, "label")` | Text input — edit string variable |
| `webNumber(var, min, max, "label")` | Number input with min/max bounds |
| `webPulldown(var, "label", "opt0\|opt1\|opt2")` | Dropdown select with label — pipe-separated options, 0-based index. Use `"@getfreepins"` as options to show available GPIO pins |
| `webRadio(var, "opt0\|opt1\|opt2")` | Radio button group — pipe-separated options, 0-based index |
| `webTime(var, "label")` | Time picker (HH:MM) — stored as HHMM integer (e.g., 1430 = 14:30) |
| `webPageLabel(page, "label")` | Register page 0–5 with a button label on the main page |
| `int webPage()` | Returns current page number being rendered (use in `WebUI()` to branch) |
| `webConsoleButton("/url", "label")` | Register button in Tasmota Utilities menu (max 4). Navigates to URL on click |

The first argument of widget functions is always a **global variable** that the widget reads from and writes to. The compiler automatically passes the variable's address to the syscall.

**Example — Widgets on the main page:**
```c
int relay;
int brightness;

void WebCall() {
    webButton(relay, "Power");
    webSlider(brightness, 0, 100, "Brightness");
}
```

**Example — Multiple pages with custom buttons:**

Up to 6 pages can be registered with `webPageLabel()`. Each creates a button on the Tasmota main page. Use `webPage()` inside `WebUI()` to render different widgets per page.

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
    webPageLabel(0, "Controls");   // first button on main page
    webPageLabel(1, "Settings");   // second button on main page
    return 0;
}
```

If no `webPageLabel()` is called but `WebUI()` exists, a single "TinyC UI" button appears.

**How it works:**
1. `WebCall()` renders widgets in the sensor section of the Tasmota main page
2. `WebUI()` renders widgets on dedicated pages at `http://<device>/tc_ui?p=N`
3. `webPageLabel(N, "text")` registers page N (0–5) with a button on the main page
4. `webPage()` returns the current page number so `WebUI()` can show different widgets
5. When you move a slider / click a button, JavaScript sends the new value via AJAX
6. The server writes the value directly into the TinyC global variable
7. The page auto-refreshes to show updated state
8. Text and number inputs pause auto-refresh while you're editing (resumes on blur)

### WebChart — Automatic Google Charts

`WebChart()` renders Google Charts on the Tasmota main page with a single function call per data series. It automatically loads the Google Charts library and generates all required JavaScript.

```c
void WebChart(int type, "title", "unit", int color, int pos, int count,
              float array[], int decimals, int interval, float ymin, float ymax)
```

| Parameter | Description |
|-----------|-------------|
| `type` | Chart type: `0` = line chart, `1` = column chart |
| `"title"` | Chart title (string literal). Empty `""` = add series to previous chart |
| `"unit"` | Y-axis unit label (string literal, e.g. `"°C"`, `"%"`, `"m/s"`) |
| `color` | Line/bar color as hex RGB (e.g. `0xe74c3c` for red) |
| `pos` | Current write position in the ring buffer |
| `count` | Number of valid data points (≤ array size) |
| `array` | Float array containing the data (ring buffer) |
| `decimals` | Number of decimal places for data values (0–6) |
| `interval` | Minutes between data points (for X-axis time labels) |
| `ymin` | Y-axis minimum. If `ymin >= ymax`, chart auto-scales |
| `ymax` | Y-axis maximum. If `ymin >= ymax`, chart auto-scales |

**Example — 24h weather charts:**
```c
#define NPTS 288       // 24h at 5-min intervals
persist float h_temp[NPTS];
persist float h_hum[NPTS];
persist int h_pos = 0;
persist int h_count = 0;

void WebPage() {
    if (h_count < 1) return;
    WebChart(0, "Temperature", "\u00b0C", 0xe74c3c, h_pos, h_count, h_temp, 1, 5, -20, 50);
    WebChart(0, "Humidity",    "%",        0x3498db, h_pos, h_count, h_hum,  1, 5, 0, 100);
}
```

**Chart size:** Use `webChartSize(width, height)` to set custom chart dimensions in pixels before a `WebChart()` call. Pass 0 for either parameter to use the default size.

- Use **fixed range** for data with known bounds (humidity 0–100, UV index 0–12)
- Use **auto-scale** (`0, 0`) for data with variable range (brightness, wind, rain)
- Call from `WebPage()` callback — each call emits one data series
- Multiple series on one chart: first call has a title, subsequent calls use `""` as title

**Including HTML from files:**

Use `webSendFile("filename")` to send the contents of a file from the device filesystem directly to the web page. This is useful for large HTML, CSS, or JavaScript that would be too big to compile into bytecode constants.

```c
void WebPage() {
    webSendFile("chart.html");  // include chart library from /chart.html
}
```

The file is read in 256-byte chunks and sent via `WSContentSend`. The filename can be with or without leading `/`.

### Custom Web Handlers

Register custom HTTP endpoints on the Tasmota web server. When a request arrives, the `WebOn()` callback is invoked with the handler number accessible via `webHandler()`.

| Function | Description |
|----------|-------------|
| `webOn(int num, "url")` | Register handler 1–4 for the given URL path |
| `int webHandler()` | Returns the handler number (1–4) inside `WebOn()` callback |
| `int webArg("name", buf)` | Read HTTP request argument into char buffer, returns length (0 if missing) |

Use `webSend(buf)` to emit the response body. The response content type is `text/plain` by default.

**Example — JSON API endpoint:**
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

**Example — Multiple endpoints:**
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

**Notes:**
- Up to 4 handlers can be registered (1–4)
- URLs must start with `/` (e.g., `/v1/json`, `/api/data`)
- `webOn()` is called in `main()` — handlers are registered at program start
- `WebOn()` callback runs after `main()` has returned (same as other callbacks)
- `webArg()` reads both GET query parameters and POST form fields
- Equivalent to Scripter's `won(N, "/url")` + `>onN` section
- CORS is enabled so endpoints are accessible from external apps

### UDP Multicast (Scripter-compatible)

Share float variables between Tasmota devices via UDP multicast on 239.255.255.250:1999.
Compatible with Tasmota Scripter's global variable protocol.

| Function | Description |
|----------|-------------|
| `float udpRecv("name")` | Get last received value for named variable (0 if none) |
| `int udpReady("name")` | Returns 1 if new value received since last check |
| `void udpSendArray("name", float_arr, count)` | Broadcast a float array via binary multicast |
| `int udpRecvArray("name", float_arr, maxcount)` | Receive float array, returns actual count |
| `udpSendStr("name", char str[])` | Send string via multicast (ASCII mode `=>name=...`) |

**Protocol:**
- Single float: send `=>name:[4 bytes IEEE-754 float]`
- Float array: send `=>name:[2-byte LE count][N × 4-byte float]`
- Receive: both ASCII (`=>name=value`) and binary (single or array)
- Multicast group: `239.255.255.250`, port `1999`
- Max 8 tracked variable names, 16 chars each
- Max 64 floats per array

**Callback:** Define `void UdpCall()` to be notified on each received variable.
UDP socket is auto-initialized on first global variable write, `udpRecv()`, or `udpReady()` call.
Scalar `global` float variables automatically broadcast via UDP when assigned (no explicit send needed).

**Socket Watchdog:** The multicast socket has a built-in inactivity watchdog (default: 60 seconds). If no packet is received within the timeout period, the socket is automatically closed and re-opened. This recovers from the known ESP32 issue where the UDP receive path silently stops working after a variable amount of time. Use `udp(8, 0, seconds)` to change the timeout (0 = disable).

**Example (scalar — auto-broadcast):**
```c
global float temperature = 0.0;  // declared as 'global' → auto-broadcasts on write

void EverySecond() {
    temperature = 20.0 + sin(counter) * 5.0;
    // No udpSend() needed — assigning a 'global' variable auto-broadcasts it
}

void UdpCall() {
    float remote = udpRecv("temperature");
    // process remote value...
}
```

**Example (array):**
```c
float sensors[8];

void EverySecond() {
    // Send 8 sensor values as array
    udpSendArray("sensors", sensors, 8);
}

void UdpCall() {
    float remote[8];
    int n = udpRecvArray("sensors", remote, 8);
    // n = number of floats actually received
}
```

### General-Purpose UDP

Scripter-compatible `udp()` function for arbitrary UDP communication. Uses a separate socket from the multicast variable sharing above.

| Function | Description |
|----------|-------------|
| `int udp(0, int port)` | Open a listening UDP port. Returns 1 on success |
| `int udp(1, char buf[])` | Read received string into buf. Returns byte count (0 = nothing) |
| `void udp(2, char str[])` | Reply to sender's IP and port |
| `void udp(3, char url[], char str[])` | Send string to url using the port from `udp(0)` |
| `int udp(4, char buf[])` | Get remote sender IP as string. Returns length |
| `int udp(5)` | Get remote sender port number |
| `int udp(6, char url[], int port, char str[])` | Send string to arbitrary url:port |
| `int udp(7, char url[], int port, int arr[], int count)` | Send array as raw bytes to url:port |
| `int udp(8, int which, int seconds)` | Set socket inactivity timeout (which: 0=multicast, 1=general port; 0=disable) |

**Notes:**
- The first argument (mode) must be a literal integer (0-8)
- Modes 6 and 7 create a temporary socket for each send (no prior `udp(0)` needed)
- Mode 1 is non-blocking: returns 0 immediately if no packet is available
- Mode 7 sends the lower byte of each array element
- Mode 8 configures the socket watchdog: if no packet is received within `seconds`, the socket is automatically reset. Default is 60 seconds. Set to 0 to disable.

```c
char buf[128];
char ip[20];

void main() {
    udp(0, 5000);  // listen on port 5000
}

void Every50ms() {
    int n = udp(1, buf);  // check for incoming
    if (n > 0) {
        udp(4, ip);               // get sender IP
        int port = udp(5);        // get sender port
        udp(2, "ACK");            // reply to sender
    }
}

void sendData() {
    char msg[64];
    strcpy(msg, "hello");
    udp(6, "192.168.1.100", 5000, msg);  // send to specific IP:port
}
```

### I2C Bus

Direct I2C bus access for sensor drivers (requires `USE_I2C`). All functions take `bus` as the last parameter (0 or 1).

| Function | Description |
|----------|-------------|
| `int i2cExists(int addr, int bus)` | Check if device responds at address. Returns 1 if found |
| `int i2cRead8(int addr, int reg, int bus)` | Read single byte from register. Returns byte value (0–255) |
| `int i2cWrite8(int addr, int reg, int val, int bus)` | Write single byte to register. Returns 1=ok, 0=fail |
| `int i2cRead(int addr, int reg, char buf[], int len, int bus)` | Read `len` bytes into char array. Returns 1=ok |
| `int i2cWrite(int addr, int reg, char buf[], int len, int bus)` | Write `len` bytes from char array. Returns 1=ok |
| `int i2cRead0(int addr, char buf[], int len, int bus)` | Read `len` bytes without register. Returns 1=ok |
| `int i2cWrite0(int addr, int reg, int bus)` | Write register byte only (no data). Returns 1=ok |
| `int i2cSetDevice(int addr, int bus)` | Check if address is **unclaimed** and responsive. Returns 1=available |
| `i2cSetActiveFound(int addr, "type", int bus)` | Register address as claimed by your driver. Logs discovery |
| `int i2cReadRS(int addr, int reg, char buf[], int len, int bus)` | Read with repeated-start (SMBus). Keeps bus held between write and read phase |
| `I2cResetActive(int addr, int bus)` | Release a previously claimed I2C address (undo `i2cSetActiveFound`) |

**Notes:**
- `bus` = 0 or 1 — selects which I2C bus to use
- Address is 7-bit (0x00–0x7F), e.g. `0x48` for TMP102
- Register is 8-bit (0x00–0xFF)
- Buffer functions use `char[]` arrays — each element holds one byte (0–255)
- Maximum buffer length is 255 bytes
- Returns 0 if I2C is not compiled in or the operation fails
- Use `i2cSetDevice` + `i2cSetActiveFound` to properly claim I2C addresses and prevent conflicts with Tasmota's built-in drivers

**Example — Read TMP102 temperature sensor on bus 0:**
```c
#define TMP102_ADDR  0x48
#define TMP102_TEMP  0x00
#define I2C_BUS      0

void EverySecond() {
    if (!i2cExists(TMP102_ADDR, I2C_BUS)) return;

    char buf[2];
    if (i2cRead(TMP102_ADDR, TMP102_TEMP, buf, 2, I2C_BUS)) {
        // TMP102: 12-bit temp in upper bits of 2 bytes
        int raw = (buf[0] << 4) | (buf[1] >> 4);
        if (raw > 2047) raw = raw - 4096;  // sign extend
        float temp = (float)raw * 0.0625;

        char out[64];
        sprintfFloat(out, "TMP102: %.2f °C\n", temp);
        printString(out);
    }
}
```

### Smart Meter (SML)

Read meter values and control meters via Tasmota's SML driver (requires `USE_SML` or `USE_SML_M`).

SML can run **without Scripter** — only `USE_UFILESYS` is needed for file-based meter descriptors.
The IDE's SML Descriptor tab manages the meter definition file (`/sml_meter.def`) on the device.

#### Reading Meter Values

| Function | Description |
|----------|-------------|
| `float smlGet(int index)` | Get meter value. Index 0 returns count, 1..N returns values |
| `int smlGetStr(int index, char buf[])` | Get meter ID string into buffer, returns length |

**Notes:**
- Index is 1-based: `smlGet(1)` returns the first meter value
- `smlGet(0)` returns the total number of meter variables
- Returns 0 if SML is not compiled in or index is out of range
- Values are the same as Scripter's `sml[x]` syntax

**Example:**
```c
void WebCall() {
    char buf[64];
    int n = smlGet(0);  // total meters
    int i = 1;
    while (i <= n) {
        float val = smlGet(i);
        sprintfFloat(buf, "{s}Meter %d{m}%.2f{e}", val);
        webSend(buf);
        i++;
    }
}
```

#### Advanced Meter Control

These functions require `USE_SML_SCRIPT_CMD` to be enabled in the firmware.

| Function | Description |
|----------|-------------|
| `int smlWrite(int meter, char buf[])` | Send hex sequence to meter (e.g. wake-up or request commands) |
| `int smlWrite(int meter, "hex")` | Same, with string literal (no temp buffer needed) |
| `int smlRead(int meter, char buf[])` | Read raw meter buffer into char array, returns bytes read |
| `int smlSetBaud(int meter, int baud)` | Change baud rate of a meter's serial port |
| `int smlSetWStr(int meter, char buf[])` | Set async write string for next scheduled send |
| `int smlSetWStr(int meter, "hex")` | Same, with string literal |
| `int smlSetOptions(int options)` | Set SML global options bitmask |
| `int smlGetV(int sel)` | Get/reset data valid flags (0=get, 1=reset) |

**Notes:**
- `meter` is the 1-based meter index from the SML descriptor
- `smlWrite` and `smlSetWStr` accept either a `char[]` array or a string literal — the compiler auto-detects which variant to use
- `smlWrite` sends a hex-encoded byte sequence (e.g. `"AA0100"`) to the meter's serial port
- `smlRead` copies the raw receive buffer into a char array for custom parsing
- `smlSetBaud` dynamically changes the meter's baud rate (useful for meters that require speed negotiation)
- `smlSetWStr` sets a hex string to be sent on the next scheduled meter poll cycle
- These functions replace Scripter's `>F`/`>S` section meter control commands

**Example — OBIS meter wake-up sequence:**
```c
void EverySecond() {
    // String literal — no temp buffer needed
    smlWrite(1, "2F3F210D0A");  // "/?!\r\n" in hex
}
```

**Example — Dynamic baud rate negotiation:**
```c
void EverySecond() {
    // Read meter response
    char buf[64];
    int n = smlRead(1, buf);
    if (n > 0 && buf[0] == 0x06) {
        // ACK received, switch to high speed
        smlSetBaud(1, 9600);
    }
}
```

#### SML Descriptor Editor (IDE)

The IDE includes an **SML Descriptor** tab in the left pane for managing meter definitions:

- **Meter database**: A dropdown loads `.tas` meter definitions from the [community database](https://github.com/ottelo9/tasmota-sml-script)
- **Custom meter URL**: The database URL is read from `/sml_meter_url.txt` on the device filesystem. To use a different meter repository, edit this file with a URL pointing to a directory containing a `smartmeter.json` index file. The default URL points to the community GitHub repository.
- **RX/TX pin selection**: Dropdowns populated from the device's free GPIOs (via `freegpio` API)
- **Pin placeholders**: `%0rxpin%` and `%0txpin%` in descriptors are replaced with selected pins on save
- **Save to Device**: Extracts only the `>M` section and saves it as `/sml_meter.def`
- **Load from Device**: Reads the current `/sml_meter.def` from the device

#### Callback Merge

Many `.tas` meter files require periodic code (Scripter's `>S` and `>F` sections) for meter communication, wake-up sequences, or baud rate negotiation. In TinyC, you write these as callback functions directly in the SML editor:

```
void EverySecond() {
    smlWrite(1, "2F3F210D0A");
}

>M 1
+1,3,s,16,9600,SML,1
1,1-0:1.8.0*255(@1,Energy In,kWh,E_in,3
#
```

**How it works:**
1. Write TinyC callback functions (`EverySecond()`, `Every100ms()`, etc.) anywhere in the SML editor — before or after the `>M` section
2. On **Save**, only the `>M` section goes to `/sml_meter.def` on the device
3. On **Compile**, the IDE automatically merges SML callbacks into the main program:
   - If the main editor already has the same callback — the SML code is appended to the existing function body
   - If the main editor doesn't have it — a new callback function is created
4. The merged source is compiled as one program — SML code and main code share the same globals and functions

### SPI Bus

Direct SPI bus access for sensors and displays. Supports both hardware SPI (using Tasmota-configured pins) and software bitbang on arbitrary GPIO pins.

| Function | Description |
|----------|-------------|
| `int spiInit(int sclk, int mosi, int miso, int speed_mhz)` | Initialize SPI bus. Returns 1=ok |
| `spiSetCS(int index, int pin)` | Set chip select pin for slot index (1–4) |
| `int spiTransfer(int cs, char buf[], int len, int mode)` | Transfer bytes. Returns bytes transferred |

**`spiInit` pin modes:**
- `sclk = -1` — Use Tasmota's primary hardware SPI bus (GPIO configured in Tasmota)
- `sclk = -2` — Use HSPI secondary hardware SPI bus (ESP32 only)
- `sclk >= 0` — Bitbang mode using GPIO pins (`sclk`, `mosi`, `miso`)
- Set `mosi` or `miso` to -1 if not needed (e.g. read-only or write-only device)
- `speed_mhz` sets clock frequency for hardware SPI (ignored for bitbang)

**`spiTransfer` modes:**
| Mode | Description |
|------|-------------|
| 1 | 8-bit per element — each `buf[]` element = 1 byte transferred |
| 2 | 16-bit per element — each `buf[]` element = 2 bytes (MSB first) |
| 3 | 24-bit per element — each `buf[]` element = 3 bytes (MSB first) |
| 4 | 8-bit with per-byte CS toggle — CS goes low/high for each byte |

**Notes:**
- `cs` parameter is 1-based CS slot index (matching `spiSetCS`). Use 0 for no automatic CS management
- Transfer is full-duplex: `buf[]` is written (MOSI) and read values (MISO) replace each element
- Maximum practical transfer length is limited by your char array size
- SPI resources are automatically cleaned up when the VM stops
- Hardware SPI requires SPI pins configured in Tasmota (Template or Module settings)

**Example — Read MAX31855 thermocouple (SPI, 32-bit read):**
```c
#define CS_PIN  5

int main() {
    spiInit(-1, -1, -1, 4);   // HW SPI at 4 MHz
    spiSetCS(1, CS_PIN);       // CS slot 1 = pin 5

    char buf[4];
    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
    spiTransfer(1, buf, 4, 1); // read 4 bytes

    // MAX31855: bits 31..18 = 14-bit thermocouple temp
    int raw = ((buf[0] << 8) | buf[1]) >> 2;
    if (raw & 0x2000) raw = raw - 16384;  // sign extend
    float temp = (float)raw * 0.25;

    char out[64];
    sprintfFloat(out, "Thermocouple: %.2f °C\n", temp);
    printString(out);
    return 0;
}
```

### Display Drawing

Requires a Tasmota build with `USE_DISPLAY` enabled and a configured display driver. All drawing functions operate on the Tasmota display renderer directly — much more efficient than building DisplayText command strings.

#### Setup & Control

| Function | Description |
|----------|-------------|
| `dspClear()` | Clear display, reset position to (0,0) |
| `dspPos(x, y)` | Set current draw position (pixels) |
| `dspFont(f)` | Set font (0-7), resets text size to 1 for non-GFX fonts |
| `dspSize(s)` | Set text size multiplier |
| `dspColor(fg, bg)` | Set foreground and background color (16-bit RGB565) |
| `dspPad(n)` | Set text padding for `dspDraw()`: positive = left-aligned padded to n chars, negative = right-aligned padded to n chars, 0 = off |
| `dspDim(val)` | Set display brightness (0-15) |
| `dspOnOff(on)` | Turn display on (1) or off (0) |
| `dspUpdate()` | Force display update (required for e-paper displays) |
| `dspWidth()` | Returns display width in pixels |
| `dspHeight()` | Returns display height in pixels |

#### Drawing Primitives

All primitives use the current position set by `dspPos()` and the current foreground color set by `dspColor()`.

| Function | Description |
|----------|-------------|
| `dspDraw(buf)` | Draw text string at current position |
| `dspPixel(x, y)` | Draw single pixel at (x,y) |
| `dspLine(x1, y1)` | Draw line from current pos to (x1,y1), updates pos |
| `dspHLine(w)` | Horizontal line from current pos, width w, updates pos |
| `dspVLine(h)` | Vertical line from current pos, height h, updates pos |
| `dspRect(w, h)` | Draw rectangle outline at current pos |
| `dspFillRect(w, h)` | Draw filled rectangle at current pos |
| `dspCircle(r)` | Draw circle outline at current pos with radius r |
| `dspFillCircle(r)` | Draw filled circle at current pos |
| `dspRoundRect(w, h, r)` | Rounded rectangle at current pos with corner radius r |
| `dspFillRoundRect(w, h, r)` | Filled rounded rectangle |
| `dspTriangle(x1, y1, x2, y2)` | Triangle from current pos to (x1,y1) and (x2,y2) |
| `dspFillTriangle(x1, y1, x2, y2)` | Filled triangle |

#### Image & Raw Commands

| Function | Description |
|----------|-------------|
| `dspPicture("file.jpg", scale)` | Draw image file from filesystem at current pos (scale: 0=original) |
| `int dspLoadImage("file.jpg")` | Load JPG into PSRAM as RGB565 pixel store, returns slot 0-3 (-1 on error). Stays in memory until VM stops. ESP32+JPEG_PICTS only |
| `dspPushImageRect(slot, sx, sy, dx, dy, w, h)` | Push a sub-rectangle from a loaded image to screen. Reads from image at (sx,sy), writes to screen at (dx,dy), size w×h. Use for dirty-rect background restore (e.g., analog clock hands over a watchface) |
| `int dspImageWidth(slot)` | Get width of loaded image in slot (0 if invalid) |
| `int dspImageHeight(slot)` | Get height of loaded image in slot (0 if invalid) |
| `dspText(buf)` | Execute raw DisplayText command string (e.g., `"[z][x50][y20]Hello"`) |

#### Predefined Color Constants (RGB565)

The following color constants are **predefined** — no `#define` needed:

| Constant | Value | Constant | Value |
|----------|-------|----------|-------|
| `BLACK` | 0 | `WHITE` | 65535 |
| `RED` | 63488 | `GREEN` | 2016 |
| `BLUE` | 31 | `YELLOW` | 65504 |
| `CYAN` | 2047 | `MAGENTA` | 63519 |
| `ORANGE` | 64800 | `PURPLE` | 30735 |
| `GREY` | 33808 | `DARKGREY` | 21130 |
| `LIGHTGREY` | 50712 | `DARKGREEN` | 992 |
| `NAVY` | 16 | `MAROON` | 32768 |
| `OLIVE` | 33792 | | |

User `#define` overrides take precedence over predefined colors.

#### Example

```c
int counter;
char buf[32];

void EverySecond() {
    counter++;

    dspClear();
    dspColor(WHITE, BLACK);    // white on black

    // Title
    dspFont(2);
    dspSize(2);
    dspPos(10, 10);
    dspDraw("TinyC Display");

    // Counter
    dspFont(1);
    dspSize(1);
    sprintfInt(buf, "Count: %d", counter);
    dspPos(10, 60);
    dspDraw(buf);

    // Draw a red box around the counter
    dspColor(RED, BLACK);
    dspPos(5, 55);
    dspRect(150, 25);

    // Draw a blue filled circle
    dspColor(BLUE, BLACK);
    dspPos(200, 80);
    dspFillCircle(20);

    dspUpdate();  // needed for e-paper
}

int main() {
    counter = 0;
    dspClear();
    return 0;
}
```

### Touch Buttons & Sliders

Create GFX touch buttons and sliders on the display. Colors are RGB565 values (use predefined constants like WHITE, BLUE, etc.).

#### Button Creation

| Function | Description |
|----------|-------------|
| `dspButton(num, x, y, w, h, oc, fc, tc, ts, "text")` | Create power button (controls relay `num`) |
| `dspTButton(num, x, y, w, h, oc, fc, tc, ts, "text")` | Create virtual toggle button (MQTT TBT) |
| `dspPButton(num, x, y, w, h, oc, fc, tc, ts, "text")` | Create virtual push button (MQTT PBT) |
| `dspSlider(num, x, y, w, h, nelem, bg, fc, bc)` | Create slider |

Parameters: `num` = button index (0-15), `x,y` = position, `w,h` = size, `oc` = outline color, `fc` = fill color, `tc` = text color, `ts` = text size, `nelem` = slider segments, `bg` = background color, `bc` = bar color.

#### State Control & Reading

| Function | Description |
|----------|-------------|
| `dspButtonState(num, val)` | Set button state (0/1) or slider value (0-100) |
| `int touchButton(num)` | Read button state: 0/1 for buttons, -1 if undefined |
| `dspButtonDel(num)` | Delete button/slider `num`, or all if `num` is -1 |

#### Touch Callback

The `TouchButton` callback is called on touch events with the button index and value:

```c
void TouchButton(int btn, int val) {
    if (btn == 0) {
        // Toggle button pressed, val = 0 or 1
        char buf[16];
        sprintfInt(buf, "%d", val);
        tasmCmd("Power1", buf);
    }
    if (btn == 1) {
        // Slider moved, val = 0-100
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

| Function | Description |
|---|---|
| `audioVol(int vol)` | Set audio volume (0-100) |
| `audioPlay("file.mp3")` | Play MP3 file from filesystem |
| `audioSay("hello")` | Text-to-speech output |

Requires I2S audio driver configured on the device.

```c
audioVol(50);              // set volume to 50%
audioPlay("/alarm.mp3");   // play MP3 file
audioSay("sensor alert");  // speak text
```

### Persistent Variables

| Function | Description |
|---|---|
| `saveVars()` | Save all `persist` globals to the program's `.pvs` file |

Persist variables are automatically loaded on program start and saved on `TinyCStop`. Use `saveVars()` to save at critical points (e.g., after midnight counter updates).

### Watch Variables (Change Detection)

| Function | Description |
|---|---|
| `changed(var)` | Returns 1 if watch variable differs from its shadow value |
| `delta(var)` | Returns current - shadow (int or float depending on variable type) |
| `written(var)` | Returns 1 if variable was assigned since last `snapshot()` |
| `snapshot(var)` | Update shadow to current value and clear written flag |

Watch variables are compiler intrinsics — they generate inline comparison code with zero runtime overhead (no syscall).

### Deep Sleep (ESP32)

| Function | Description |
|---|---|
| `deepSleep(int seconds)` | Enter deep sleep with timer wakeup after `seconds` |
| `deepSleepGpio(int seconds, int pin, int level)` | Deep sleep with timer + GPIO wakeup (0=low, 1=high) |
| `int wakeupCause()` | Returns ESP32 wakeup cause (0=reset, 2=EXT0, 3=EXT1, 4=timer, 5=touchpad, ...) |

Persist variables and settings are saved automatically before entering deep sleep.

```c
// Wake every 5 minutes to read sensor
int cause = wakeupCause();
if (cause == 4) {
    // woke from timer — read sensor, send data
}
deepSleep(300);  // sleep 300 seconds

// Sleep until GPIO12 goes HIGH (or 1 hour max)
deepSleepGpio(3600, 12, 1);
```

### Hardware Registers (ESP32)

Direct read/write access to ESP32 memory-mapped peripheral registers. Only addresses in the peripheral range are allowed (0x3FF00000–0x3FFFFFFF or 0x60000000–0x600FFFFF).

| Function | Description |
|---|---|
| `int peekReg(int addr)` | Read 32-bit value from peripheral register |
| `pokeReg(int addr, int val)` | Write 32-bit value to peripheral register |

**Warning:** Incorrect register writes can crash or damage the device. Only use if you know what you're doing.

### Email (ESP32 — requires USE_SENDMAIL)

| Function | Description |
|---|---|
| `mailBody(body)` | Set email body text (HTML). `body` is a `char[]` array |
| `mailAttach("/path")` | Add file attachment from filesystem (string literal, up to 8) |
| `int mailSend(params)` | Send email. `params` is `char[]` with `[server:port:user:passwd:from:to:subject]`. Returns 0=ok |

For simple emails without attachments, put body text after the `]` in params:
```c
char cmd[200];
strcpy(cmd, "[smtp.gmail.com:465:user:pass:from@x.com:to@y.com:Alert] Sensor triggered!");
int result = mailSend(cmd);
```

For emails with file attachments, use `mailBody()` and `mailAttach()` before `mailSend()`:
```c
// Build body
char body[200];
sprintfStr(body, "<h1>Daily Report</h1><p>Temperature: %d C</p>", "%.1f");

// Register body and attachments
mailBody(body);
mailAttach("/data.csv");
mailAttach("/log.txt");

// Send — params only need [server:port:user:passwd:from:to:subject]
char params[200];
strcpy(params, "[*:*:*:*:*:to@example.com:Daily Report]");
int result = mailSend(params);
// result: 0=ok, 1=parse error, 4=memory error
```

Use `*` for server/port/user/password/from fields to use `#define` defaults from `user_config_override.h`.

### Tesla Powerwall (ESP32 — requires TESLA_POWERWALL)

Access Tesla Powerwall local API via HTTPS. Uses the email library's SSL implementation (standard Arduino SSL does not work with Powerwall).

**Requires:** `#define TESLA_POWERWALL` in `user_config_override.h` and the ESP-Mail-Client library.

| Function | Description |
|----------|-------------|
| `int pwlRequest(url)` | Config command or API request. Returns 0=ok, -1=fail |
| `pwlBind(&var, path)` | Register a global float variable for auto-fill. Path uses `#` separator (max 24 bindings) |
| `float pwlGet(path)` | Extract float from last response. Supports `[N]` suffix for nth occurrence |
| `int pwlStr(path, buf)` | Extract string from last response into `char[]` buffer. Returns length |

**Recommended approach — `pwlBind` (parse once, fill all):**

Register global variables with JSON paths in `Setup()`. When `pwlRequest()` receives a response, the JSON is parsed **once** and all matching bound variables are filled directly. No string replacements, no repeated parsing.

```c
float sip, sop, bip, hip, pwl, rper;

void Setup() {
    pwlRequest("@D192.168.188.60,email@example.com,mypassword");
    pwlRequest("@C0x000004714B006CCD,0x000004714B007969");

    // Register bindings — use original JSON key names
    pwlBind(&sip, "site#instant_power");
    pwlBind(&sop, "solar#instant_power");
    pwlBind(&bip, "battery#instant_power");
    pwlBind(&hip, "load#instant_power");
    pwlBind(&pwl, "percentage");
    pwlBind(&rper, "backup_reserve_percent");
}

void Loop() {
    // All matching bindings filled automatically:
    pwlRequest("/api/meters/aggregates");
    // sip, sop, bip, hip are now set

    pwlRequest("/api/system_status/soe");
    // pwl is now set

    pwlRequest("/api/operation");
    // rper is now set
}
```

**Configuration prefixes:**
| Prefix | Description |
|--------|-------------|
| `@Dip,email,password` | Configure IP and credentials |
| `@Ccts1,cts2` | Configure CTS serial numbers (masked in responses) |
| `@N` | Clear auth cookie (force re-authentication) |

**Common API endpoints:**
| Endpoint | Data |
|----------|------|
| `/api/meters/aggregates` | Site, battery, load, solar power (W) |
| `/api/system_status/soe` | State of energy / battery percentage |
| `/api/system_status` | System status info |
| `/api/operation` | Operation mode, reserve percentage |
| `/api/meters/readings` | Detailed meter readings per CTS |

**Nth-occurrence extraction:** `pwlGet("key[N]")` extracts the Nth occurrence of a repeated key from the JSON response. Useful for `/api/meters/readings` which has multiple CTS objects with the same key names:

```c
// Per-phase grid readings — CTS2 grid phases are occurrences 6,7,8 of "p_W"
phs1 = pwlGet("p_W[6]");
phs2 = pwlGet("p_W[7]");
phs3 = pwlGet("p_W[8]");
```

**Ad-hoc access:** `pwlGet()` and `pwlStr()` are available for one-off value extraction from the last response, but `pwlBind()` is preferred for repeated polling since it avoids re-parsing.

### Addressable LED Strip (WS2812 — requires USE_WS2812)

Control WS2812 / NeoPixel addressable LED strips directly from TinyC.

**Requires:** `#define USE_WS2812` in `user_config_override.h`.

| Function | Description |
|----------|-------------|
| `setPixels(array, len, offset)` | Set `len` pixels from `array`, starting at strip position `offset & 0x7FF`. Updates strip immediately. |

**Color format:** Each array element is `0xRRGGBB` (24-bit RGB packed into an int).

**RGBW mode:** Set bit 12 of offset (`offset | 0x1000`) for RGBW mode. In RGBW mode, two consecutive array elements encode one pixel (high word = `0x00RG`, low word = `0xBW00`).

**Example — Rainbow effect:**
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

### ESP Camera (ESP32)

Camera support for ESP32 boards with OV2640/OV3660/OV5640 sensors. Two modes available:

- **Tasmota webcam driver** (sel 0-7): Uses the standard `USE_WEBCAM` driver. Define `USE_WEBCAM` in `user_config_override.h`.
- **TinyC integrated camera** (sel 8-18): Direct esp_camera driver with board-specific pins, MJPEG streaming on port 81, and PSRAM slot management. Define `USE_TINYC_CAMERA` (via `-DTINYC_CAMERA` build flag). No `USE_WEBCAM` dependency.

Both modes support `mailAttachPic()` for email picture attachments (up to 4 pictures per email).

#### Camera Init with Custom Pins (TinyC integrated mode)

```c
// Pin array order: pwdn, reset, xclk, sda, scl, d7..d0, vsync, href, pclk
int campins[] = {-1, -1, 15, 4, 5, 16, 17, 18, 12, 10, 8, 9, 11, 6, 7, 13};
int ok = cameraInit(campins, PIXFORMAT_JPEG, FRAMESIZE_VGA, 12, 0, 0, -1);
```

| Function | Description |
|----------|-------------|
| `cameraInit(pins[], format, framesize, quality, fb_count, grab_mode, xclk_freq)` | Init camera with pin array. Returns 0=ok, non-zero=error. `fb_count`=0 auto, `grab_mode`=0 auto, `xclk_freq`=-1 default 20MHz. |

#### Camera Control (camControl)

All camera operations use `camControl(sel, p1, p2)`:

**Tasmota webcam driver (sel 0-7, requires USE_WEBCAM):**

| sel | Function | Description |
|-----|----------|-------------|
| 0 | `camControl(0, resolution, 0)` | Init via Tasmota driver (WcSetup) |
| 1 | `camControl(1, bufnum, 0)` | Capture to Tasmota pic buffer (1-4) |
| 2 | `camControl(2, option, value)` | Set options (WcSetOptions) |
| 3 | `camControl(3, 0, 0)` | Get width |
| 4 | `camControl(4, 0, 0)` | Get height |
| 5 | `camControl(5, on_off, 0)` | Start/stop Tasmota stream server |
| 6 | `camControl(6, param, 0)` | Motion detection (-1=read motion, -2=read brightness, ms=interval) |

**TinyC integrated camera (sel 7-18, requires USE_WEBCAM or USE_TINYC_CAMERA):**

| sel | Function | Description |
|-----|----------|-------------|
| 7 | `camControl(7, bufnum, fileHandle)` | Save picture buffer to file, returns bytes written |
| 8 | `camControl(8, 0, 0)` | Get sensor PID (e.g. 0x2642 = OV2640, 0x3660 = OV3660) |
| 9 | `camControl(9, param, value)` | Set sensor parameter (see table below) |
| 10 | `camControl(10, slot, 0)` | Capture to PSRAM slot (1-4), returns JPEG size in bytes |
| 11 | `camControl(11, slot, fileHandle)` | Save PSRAM slot to file, returns bytes written |
| 12 | `camControl(12, slot, 0)` | Free PSRAM slot (0 = free all slots) |
| 13 | `camControl(13, 0, 0)` | Deinit camera + free all slots + stop stream |
| 14 | `camControl(14, slot, 0)` | Get slot size in bytes (0 if empty) |
| 15 | `camControl(15, on_off, 0)` | Start/stop MJPEG stream server on port 81 |
| 16 | `camControl(16, interval_ms, threshold)` | Enable motion detection (0=disable) |
| 17 | `camControl(17, sel, 0)` | Get motion value: 0=trigger, 1=brightness, 2=triggered, 3=interval |
| 18 | `camControl(18, 0, 0)` | Free motion reference buffer |
| 19 | `camControl(19, addr, mask)` | Read raw sensor register at `addr`, masked by `mask` |
| 20 | `camControl(20, addr, val)` | Write raw value `val` to sensor register at `addr` |

Capture (sel 10) copies the JPEG from the camera framebuffer to a PSRAM slot and immediately returns the camera framebuffer, allowing fast consecutive captures. Up to 4 slots can hold pictures simultaneously.

**Important:** Camera capture (`camControl(10, ...)`) must run in `TaskLoop()` (VM task thread). Calling from `EverySecond()` (main thread) will freeze the device.

**Stream server (sel 15):** Starts an MJPEG server on port 81 with `/stream`, `/cam.mjpeg`, and `/cam.jpg` endpoints. Automatically deferred if WiFi is not ready yet (safe for autoexec). The stream is embedded on the Tasmota main page via `FUNC_WEB_ADD_MAIN_BUTTON`.

#### Sensor Parameters (sel=9)

| param | Setting | Range |
|-------|---------|-------|
| 0 | vflip | 0/1 |
| 1 | brightness | -2..2 |
| 2 | saturation | -2..2 |
| 3 | hmirror | 0/1 |
| 4 | contrast | -2..2 |
| 5 | framesize | FRAMESIZE_* |
| 6 | quality | 10..63 |
| 7 | sharpness | -2..2 |

#### Email Picture Attachments

Pictures captured to PSRAM slots are available for email via `mailAttachPic()`. Up to 4 pictures can be attached per email:

```c
// Capture 2 pictures to slots 1 and 2
camControl(10, 1, 0);
camControl(10, 2, 0);

// Send email with both pictures attached
mailBody("Motion alarm");
mailAttachPic(1);
mailAttachPic(2);
mailSend("[*:*:*:*:*:user@example.com:Alarm]");
```

#### Capture and Save Example

```c
// Capture to PSRAM slot 1
int size = camControl(10, 1, 0);

// Save slot 1 to file
int fh = fileOpen(path, 1);    // open for write
int written = camControl(11, 1, fh);
fileClose(fh);

// Start MJPEG stream on port 81
camControl(15, 1, 0);
```

#### Complete Camera Script

See `webcam_tinyc.tc` for a full security camera example with MJPEG streaming, motion detection, PIR alarm, email alerts, timelapse, and auto-cleanup. See `webcam.tc` for the equivalent using the Tasmota webcam driver.

---

### HomeKit (ESP32 — requires USE_HOMEKIT)

Apple HomeKit integration — expose devices directly from TinyC as HomeKit accessories. Sensors, lights, switches, and outlets become controllable via Apple Home. All HomeKit-bound variables use **native float values** — no x10 scaling needed.

**Requires:** `#define USE_HOMEKIT` in `user_config_override.h`.

#### Predefined HomeKit Constants

| Constant | Value | HAP Category | Variables |
|----------|-------|--------------|-----------|
| `HK_TEMPERATURE` | 1 | Sensor (Temperature) | 1: temperature in °C |
| `HK_HUMIDITY` | 2 | Sensor (Humidity) | 1: humidity in % |
| `HK_LIGHT_SENSOR` | 3 | Sensor (Ambient Light) | 1: lux value |
| `HK_BATTERY` | 4 | Sensor (Battery) | 3: level, low-battery flag, charging state |
| `HK_CONTACT` | 5 | Sensor (Contact) | 1: open/closed |
| `HK_SWITCH` | 6 | Switch | 1: on/off |
| `HK_OUTLET` | 7 | Outlet | 1: on/off |
| `HK_LIGHT` | 8 | Light (Color) | 4: power, hue, saturation, brightness |

#### HomeKit Functions

| Function | Description |
|----------|-------------|
| `hkSetCode(code)` | Set pairing code (format: `"XXX-XX-XXX"`) |
| `hkAdd(name, type)` | Add device — name and type (e.g. `HK_TEMPERATURE`) |
| `hkVar(variable)` | Bind a float variable to the current device |
| `int hkReady(variable)` | Returns 1 if HomeKit changed this variable since last check (auto-clears) |
| `int hkStart()` | Finalize descriptor and start HomeKit. Returns 0=ok |
| `int hkInit(char descriptor[])` | Start HomeKit with a raw descriptor char array (advanced — bypasses builder pattern) |
| `hkReset()` | Erase all pairing data (factory reset). Re-pair after reboot |
| `hkStop()` | Stop HomeKit server |

#### hkReady() — Change Polling

`hkReady(var)` works like `udpReady()` — it returns 1 if Apple Home has changed this variable since the last call, and automatically clears the flag. The firmware writes the value directly into the global variable, so no manual assignment is needed. Use `hkReady()` to forward changed values via UDP:

```c
void EverySecond() {
    // global variables auto-broadcast on assignment — no explicit udpSend needed
}
```

#### HomeKitWrite Callback (Optional)

Called when Apple Home changes a value. The value is already written to the global variable before this callback runs — use it only for local side effects like relay forwarding:

```c
void HomeKitWrite(int dev, int var, float val) {
    // dev = device index (order of hkAdd calls, starting at 0)
    // var = variable index (order of hkVar calls per device, starting at 0)
    // val = new float value from Apple Home (already stored in global)
    // Only needed for side effects like tasm_power = 1
}
```

#### Builder Pattern (hkAdd + hkVar)

Devices are defined step by step. `hkAdd()` starts a device, `hkVar()` binds float variables to it. Use multiple `hkVar()` calls for devices with multiple characteristics (e.g. color light):

```c
// Color light — 4 variables: power, hue, saturation, brightness
float pwr, hue, sat, bri;

hkSetCode("111-22-333");
hkAdd("Lamp", HK_LIGHT);
hkVar(pwr); hkVar(hue); hkVar(sat); hkVar(bri);

// Simple sensor — 1 variable
float temp;
hkAdd("Temperature", HK_TEMPERATURE);
hkVar(temp);

hkStart();
```

#### Full Example — Office with Light + Sensors

```c
// HomeKit-bound variables (native float values)
float mh_pwr, mh_hue, mh_sat, mh_bri;  // color light
float elamp;     // corner light on/off
float btemp;     // temperature (e.g. 22.5)
float bhumi;     // humidity (e.g. 55.0)
int last_pwr;

// Only needed for relay forwarding — value is already in the global
void HomeKitWrite(int dev, int var, float val) {
    if (dev == 0 && var == 0) {
        int pwr;
        pwr = 0;
        if (val > 0.0) { pwr = 1; }
        if (pwr != last_pwr) { tasm_power = pwr; last_pwr = pwr; }
    }
}

void EverySecond() {
    // Receive sensor values via UDP
    if (udpReady("btemp")) { btemp = udpRecv("btemp"); }
    if (udpReady("bhumi")) { bhumi = udpRecv("bhumi"); }

    // global variables auto-broadcast on assignment — no explicit udpSend needed
}

int main() {
    mh_pwr = 0.0; mh_hue = 0.0; mh_sat = 0.0; mh_bri = 50.0;
    elamp = 0.0; btemp = 22.0; bhumi = 50.0;
    last_pwr = -1;

    hkSetCode("111-11-111");
    hkAdd("Light", HK_LIGHT);
    hkVar(mh_pwr); hkVar(mh_hue); hkVar(mh_sat); hkVar(mh_bri);
    hkAdd("Corner Light", HK_OUTLET);       hkVar(elamp);
    hkAdd("Temperature", HK_TEMPERATURE);    hkVar(btemp);
    hkAdd("Humidity", HK_HUMIDITY);           hkVar(bhumi);
    hkStart();
    return 0;
}
```

#### Pairing

1. Compile and flash firmware with `USE_HOMEKIT`
2. Compile and upload TinyC program using `hkSetCode()` / `hkAdd()` / `hkStart()`
3. Scan QR code at `http://<device>/hk` with iPhone
4. After configuration changes, run `hkReset()` once, then re-pair

#### Predefined File Constants

Shorthand constants for `fileOpen()`:

| Constant | Value | Description |
|----------|-------|-------------|
| `r` | 0 | Read |
| `w` | 1 | Write |
| `a` | 2 | Append |

```c
int f = fileOpen("/data.csv", r);   // instead of fileOpen("/data.csv", 0)
f = fileOpen("/log.txt", a);         // instead of fileOpen("/log.txt", 2)
```

### Plugin Query (Binary Plugins)

Query loaded binary plugins (PIC modules) for data.

| Function | Description |
|---|---|
| `int pluginQuery(char dst[], int index, int p1, int p2)` | Call plugin at `index` with parameters `p1`, `p2`. Result string copied to `dst`. Returns string length |

### Debug

| Function      | Description                |
|---------------|----------------------------|
| `dumpVM()`    | Dump VM state to console   |

---

## Multi-VM Slots (ESP32)

On ESP32, up to **6 independent TinyC programs** can run simultaneously in separate VM slots. Each slot has its own bytecode, globals, stack, heap, and output buffer. Memory is allocated dynamically — empty slots cost zero bytes, and non-autoexec slots use lazy loading (only ~33 bytes until first run). ESP8266 supports only 1 slot.

### Slot Configuration

Slot assignments and autoexec flags are stored in `/tinyc.cfg` on the filesystem. This file is created and updated automatically whenever a program is loaded, uploaded, or the autoexec flag is toggled. There is no need to edit it manually.

Example `/tinyc.cfg`:
```
/weather.tcb,1
/display.tcb,1
/logger.tcb,0
,0
,0
/mp3player.tcb,1
_info,0
```

Each line corresponds to a slot (0–5): `filename,autoexec_flag`. The last line `_info,<0|1>` controls whether debug status rows are shown on the Tasmota main web page.

### Tasmota Commands

All commands default to slot 0 if no slot number is given (backward-compatible).

| Command                       | Description                                      |
|-------------------------------|--------------------------------------------------|
| `TinyC`                       | Show status for all slots (JSON)                 |
| `TinyCRun [slot] [/file.tcb]` | Run slot (optionally load file first)            |
| `TinyCStop [slot]`            | Stop slot                                        |
| `TinyCReset [slot]`           | Stop and reset slot                              |
| `TinyCExec <n>`               | Set instructions per tick (default 1000)         |
| `TinyCInfo 0\|1`              | Show/hide VM debug rows on main web page         |
| `TinyC ?<query>`              | Query global variables by index (see below)      |

**Examples:**
```
TinyCRun                    → run slot 0
TinyCRun /weather.tcb       → load file into slot 0 and run
TinyCRun 2 /logger.tcb      → load file into slot 2 and run
TinyCStop 1                 → stop slot 1
TinyCReset 3                → reset slot 3
TinyCInfo 1                 → show debug info on main page
```

### Web Console (`/tc`)

The TinyC console page at `/tc` shows a compact overview of all slots:

- **Status indicator**: green dot = active (running or callback-ready), orange = loaded but not running, grey = empty
- **Run / Stop buttons**: context-aware — Run is greyed out when active, Stop is greyed out when idle
- **A button**: toggles auto-execute on boot (green = enabled). Saved to `/tinyc.cfg` immediately
- **Load Program**: file selector with slot dropdown to load any `.tcb` file into any slot
- **Upload Program**: file upload with slot dropdown to upload and load a `.tcb` file directly

### API Endpoints

The JSON API at `/tc_api` supports a `slot` parameter:

```
GET /tc_api?cmd=run&slot=2     → run slot 2
GET /tc_api?cmd=stop&slot=1    → stop slot 1
GET /tc_api?cmd=status         → status of all slots
POST /tc_upload?slot=3&api=1   → upload .tcb to slot 3 (JSON response)
```

### Variable Query — `_Q()` Macro (Google Charts)

TinyC global variables can be queried via HTTP as JSON, enabling live dashboards with Google Charts or any JavaScript charting library.

The `_Q()` macro is expanded at **compile time** inside string literals. The compiler resolves variable names to their index and type, so the binary contains no variable names — only compact index-based queries.

**Syntax:** `_Q(var1, var2, ...)`

The compiler replaces `_Q(...)` with an index-encoded query string:
- `<index>i` — int scalar
- `<index>f` — float scalar
- `<index>s<n>` — char[n] string
- `<index>I<n>` — int array of n elements
- `<index>F<n>` — float array of n elements

**Example:** Given globals `float temperature; int counter;`, the string:
```c
"TinyC+%3F_Q(temperature,counter)"
```
expands at compile time to:
```
"TinyC+%3F0f;1i"
```

**Response format:** JSON array in the order requested:
```json
{"TinyC":[23.5,42]}
```

**Usage in WebPage callback:**
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

For a specific slot, prefix with the slot number:
```
TinyC ?2 0f;1i      → query slot 2
```

### Boot Sequence

On boot, TinyC reads `/tinyc.cfg` and:
1. Loads each configured `.tcb` file into its slot
2. Auto-runs slots that have the autoexec flag set (`1`)

If no `/tinyc.cfg` exists (first boot), no programs are loaded.

### Resource Usage

Each VM slot uses approximately **3.2 KB RAM** (struct only, without program bytecode). Slots are allocated dynamically — only active slots consume memory. The slot pointer array itself is just 24 bytes. Non-autoexec slots use lazy loading: only the filename (~33 bytes) is stored until the slot is first run.

| Resource              | Cost                         |
|-----------------------|------------------------------|
| Pointer array         | 24 bytes (6 pointers)        |
| Per-slot struct       | ~3.2 KB                      |
| Program bytecode      | variable (malloc'd)          |
| Heap (large arrays)   | 32 KB max, allocated on demand |
| Autoexec stagger      | 100 ms delay between starts  |

### Callbacks with Multiple Slots

Each slot receives its own callbacks independently:

- `EverySecond()`, `Every50ms()` — dispatched to all active slots
- `WebCall()` — each slot can add its own sensor rows to the main page
- `JsonCall()` — each slot appends its own telemetry data
- `TaskLoop()` — runs in slot's own FreeRTOS task (ESP32)
- `CleanUp()` — called on all slots before device restart

Shared resources (UDP, SPI, file handles) are global — only one slot should use each at a time.

### Example: Two Programs Side by Side

Slot 0 — Temperature monitor:
```c
int temp = 0;
void EverySecond() { temp = tasm_analog0; }
void WebCall() {
    char buf[64];
    sprintfInt(buf, "{s}Temperature{m}%d{e}", temp);
    webSend(buf);
}
int main() { return 0; }
```

Slot 1 — Uptime counter:
```c
int uptime = 0;
void EverySecond() { uptime++; }
void WebCall() {
    char buf[64];
    sprintfInt(buf, "{s}Uptime{m}%d s{e}", uptime);
    webSend(buf);
}
int main() { return 0; }
```

Both display their sensor rows on the Tasmota main page simultaneously.

---

## VM Limits

| Resource          | ESP8266  | ESP32    | Browser  | Notes                              |
|-------------------|----------|----------|----------|------------------------------------|
| Stack depth       | 64       | 256      | 256      | Operand stack entries              |
| Call frames       | 8        | 32       | 32       | Maximum recursion / call depth     |
| Locals per frame  | 256      | 256      | 256      | Scalars + small arrays ≤16 inline  |
| Global variables  | 64       | 256      | 256      | Scalars + small arrays ≤16 inline  |
| Code size         | 4 KB     | 16 KB    | 64 KB    | Bytecode (16-bit addressing)       |
| Heap memory       | 8 KB     | 32 KB    | 64 KB    | For arrays >16 elements (auto alloc)   |
| Heap handles      | 8        | 16       | 32       | Max simultaneous heap allocations  |
| Constant pool     | 32       | 64       | 65536    | String & float constants           |
| Instruction limit | 1M       | 1M       | 1M       | Safety limit per execution         |
| GPIO pins         | 40       | 40       | 40       | Pins 0–39 (simulated in browser)   |
| File handles      | 4        | 4        | 8        | Simultaneously open files          |
| VM slots          | 1        | 6        | 1        | Simultaneous programs              |

---

## Device File Management (IDE)

### IDE Installation

The IDE file (`tinyc_ide.html.gz`) can reside on either the **flash filesystem** or the **SD card** — whichever is mounted as the user filesystem (`ufsp`). Upload `tinyc_ide.html.gz` via the Tasmota **Manage File System** page.

> **Note:** TinyC scripts and data files (`.tc`, `.tcb`, etc.) are also stored on the user filesystem (`ufsp`).

### File Operations

The IDE toolbar includes controls for managing files on the Tasmota device filesystem:

- **Device Files dropdown** — Lists all files on the device. Select a file to load it into the editor. The list shows filename and size (e.g. `config.tc (1.2KB)`).
- **Save File button** — Saves the current editor content as a file on the device. Prompts for a filename (defaults to the current filename).
- **Auto-refresh** — The file list refreshes automatically when the device IP is entered or changed, and after each save.

All file operations use the `/tc_api` endpoint with CORS support, so the IDE can be used from any browser — it doesn't need to be served from the device.

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/tc_api?cmd=listfiles` | GET | Returns JSON list of files: `{"ok":true,"files":[{"name":"x","size":123},...]}` |
| `/tc_api?cmd=readfile&path=/name` | GET | Returns file content as plain text |
| `/tc_api?cmd=readfile&path=/name@from_to` | GET | Returns time-filtered CSV data (see below) |
| `/tc_api?cmd=writefile&path=/name` | POST | Writes POST body to file, returns `{"ok":true,"size":N}` |
| `/tc_api?cmd=deletefile&path=/name` | GET | Deletes a file from the filesystem |

### Time-Range Filtered File Access

Append `@from_to` to the file path to extract only rows within a timestamp range from a CSV data file. This is useful for serving IoT time-series data to chart libraries.

**URL format:**
```
/tc_api?cmd=readfile&path=/data.csv@DD.MM.YY-HH:MM_DD.MM.YY-HH:MM
```

**Example:**
```
/tc_api?cmd=readfile&path=/sml.csv@1.1.24-00:00_31.1.24-23:59
```

Both German (`DD.MM.YY HH:MM`) and ISO (`YYYY-MM-DDTHH:MM:SS`) timestamp formats are supported. The `_` (underscore) separates the from and to timestamps.

**Response:** The header line (first line) is always included, followed by only those data lines whose first-column timestamp falls within `[from..to]`. Lines past the end timestamp are skipped efficiently (early break).

**Performance optimization:** If an index file exists (same name with `.ind` extension, containing `timestamp\tbyte_offset` lines), byte offsets are used to seek directly to the start position. Otherwise, an estimated seek is performed based on the file's first and last timestamps (similar to Scripter's `opt_fext`).

### Port 82 Download Server (ESP32)

For large database files, the time-filtered readfile on port 80 can block the main web server loop. TinyC includes a dedicated **port 82 download server** that serves files in a FreeRTOS background task, keeping the device responsive during large transfers.

**URL format:**
```
http://<ip>:82/ufs/<filename>
http://<ip>:82/ufs/<filename>@from_to
```

**Examples:**
```
http://192.168.1.100:82/ufs/sml.csv
http://192.168.1.100:82/ufs/sml.csv@1.1.24-00:00_31.1.24-23:59
```

**Features:**
- Runs in a dedicated FreeRTOS task (pinned to core 1, priority 3)
- Does not block the main Tasmota loop or web interface
- Supports the same `@from_to` time-range filtering as the `/tc_api` readfile
- Uses chunked transfer encoding for filtered responses
- Content-Disposition header for browser download
- One download at a time (returns HTTP 503 if busy)
- Automatic MIME type detection (`.csv`/`.txt` = text/plain, `.html`, `.json`)
- The port can be changed by defining `TC_DLPORT` before compilation (default: 82)

### Typical Workflow

1. Enter device IP in the toolbar
2. The **Device Files** dropdown auto-populates with all files on the device
3. Select a file to load it into the editor — or write new code
4. Click **Save File** to store the source on the device (e.g. as `myapp.tc`)
5. Click **Run on Device** to compile, upload the `.tcb` binary, and start execution

This lets you keep TinyC source files on the device alongside their compiled bytecode, making it easy to edit programs directly without needing local file storage.

## Keyboard Shortcuts (IDE)

| Shortcut           | Action              |
|--------------------|---------------------|
| Ctrl + Enter       | Compile             |
| Ctrl + Shift + Enter | Compile & Run     |
| Ctrl + S           | Save file           |
| Ctrl + O           | Open file           |
| Ctrl + F           | Find                |
| Enter (in Find)    | Find next           |
| Shift + Enter (in Find) | Find previous  |
| Escape             | Close Find bar      |
| Tab (in editor)    | Insert 4 spaces     |

---

## Examples

The IDE includes 19 ready-to-use examples in the "Load Example..." dropdown — from basic blink to weather station receivers and interactive WebUI dashboards.

### Hello World
```c
int main() {
    printStr("Hello, TinyC!\n");
    return 0;
}
```

### LED Blink
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

### String Operations
```c
int main() {
    char greeting[32] = "Hello";
    char name[16] = "World";
    char buf[64];

    // Classic function style
    strcpy(buf, greeting);
    strcat(buf, ", ");
    strcat(buf, name);
    strcat(buf, "!\n");
    printString(buf);       // Hello, World!

    // Same thing with + operator
    buf = greeting;
    buf += ", ";
    buf += name;
    buf = buf + "!\n";
    printString(buf);       // Hello, World!

    // Formatted strings
    char line[64];
    sprintf(line, "count = %d", 42);
    printString(line);      // count = 42

    // Multi-value with sprintfAppend
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

### WebUI Dashboard
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

## Differences from Standard C

| Feature                  | Standard C     | TinyC                        |
|--------------------------|----------------|------------------------------|
| Pointers                 | Full support   | **Not supported**            |
| Structs / Unions         | Full support   | **Not supported**            |
| Enums                    | Full support   | **Not supported**            |
| Dynamic memory           | malloc/free    | Auto heap for arrays >16 elements (no explicit malloc) |
| Multi-dimensional arrays | `int a[3][4]`  | **Not supported**            |
| String type              | `char*`        | `char arr[N]` only           |
| Preprocessor             | Full CPP       | `#define`, `#ifdef`, `#if`, `#else`, `#endif` (no `#include`, no macros) |
| Header files             | `#include`     | **Not supported**            |
| Typedef                  | Full support   | **Not supported**            |
| sizeof                   | Full support   | **Not supported**            |
| Ternary operator         | `a ? b : c`   | **Not supported**            |
| do-while                 | `do {} while`  | **Not supported**            |
| goto                     | Full support   | **Not supported**            |
| Function pointers        | Full support   | **Not supported**            |
| Variadic functions       | `printf(...)`  | **Not supported**            |
| Standard library         | stdio, stdlib  | Built-in functions only      |

---

*Generated from TinyC source — lexer.js, parser.js, codegen.js, opcodes.js, vm.js*
