# Digital Health Companion

A C++17 health companion with both a native Windows desktop interface and a
terminal version. Food nutrition is looked up from Open Food Facts; an internet
connection is needed to search foods.

## Features

- Create multiple local user profiles with generated IDs, names, current and
  goal weights, height, age, sex, and activity level.
- Estimate a daily calorie target with the adult Mifflin-St Jeor BMR formula
  multiplied by an activity factor, then estimate a gain or loss adjustment
  from the requested number of days to reach the goal (using 7,700 kcal/kg,
  capped at 500 kcal/day). Weight-loss targets are not set below BMR.
- Search packaged foods, choose a matching result, enter a serving size, and
  store its estimated calories, protein, carbohydrates, and fat.
- Build meals from multiple searched foods, log their combined nutrition, and
  save reusable meal templates for quick logging.
- Browse daily food logs with a calendar, mark days finished, and view average
  daily nutrition for a month or custom date range. Days with no food entries
  count as zero in the averages.
- Configure repeating water reminders and daily medicine and sleep reminders.
- Keep each user's profile, logged meals, saved meals, and reminder data in a
  separate `users/<user-id>.txt` file in the directory from which the app is
  started. Existing `health_data.txt` data is migrated to per-user files on
  launch.

Nutrition values are estimates for general wellness, not medical advice.
Open Food Facts entries are community-contributed and can be incomplete or
inaccurate; check package labels when available. The app does not replace
professional guidance.

## Building

This project includes a Makefile for the common build paths. From the project
root, you can use:

```sh
make
make console
make gui
make reset
```

On MSYS2/MinGW environments, `mingw32-make` may be the executable name instead
of `make` if the build tools are not exposed on `PATH`.

The console application can also be built directly with g++ on Windows or
Linux. On Linux, install the C++ compiler, libcurl development files, and
pkg-config (for example, `sudo apt install g++ libcurl4-openssl-dev pkg-config`).
Food searches use WinHTTP on Windows and libcurl on Linux.

### Windows (MinGW-w64 / MSYS2 UCRT64)

```powershell
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude src\main.cpp src\health_companion.cpp src\food_database.cpp src\food_database_client.cpp -o health_companion.exe -lwinhttp
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude src\windows_gui.cpp src\health_companion.cpp src\food_database.cpp src\food_database_client.cpp -o health_companion_gui.exe -mwindows -lwinhttp -luser32 -lgdi32 -lcomctl32
g++ -std=c++17 -Wall -Wextra -Wpedantic src\reset_data.cpp -o reset_data.exe
```

Run the console version with `.\health_companion.exe`, or the GUI with
`.\health_companion_gui.exe`. The GUI uses the Windows API directly and is
Windows-only. The food response parser and HTTP client are separate modules.

### Linux (g++)

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude \
  src/main.cpp src/health_companion.cpp src/food_database.cpp \
  src/food_database_client.cpp -o health_companion \
  $(pkg-config --cflags --libs libcurl) -pthread
```

Run it from the project directory with `./health_companion`. Linux uses the
console application; the native Windows GUI is not available there.

To erase all saved users and return to the initial state on Windows, close the
app and run `reset_data.exe`. It asks you to type `RESET` before deleting the
`.txt` user files in `users/` and `health_data.txt` next to the executable.
Other files are left untouched.

The Windows window provides user and profile selection, calorie summaries, food
search, meal composition and logging, saved meals, and reminder settings. Food
searches run in the background; an internet connection is required. Scroll the
window to view the daily food history and date-range averages. Reminder alerts
appear as popups while the desktop app is running.

## User Guide

### Start the app and choose a profile

Run the app from the project directory so that it reads and writes the expected
`users/` folder:

```powershell
.\health_companion_gui.exe
.\health_companion.exe
```

Choose an existing user or create a new profile. In the GUI, use the profile
selector or **New user**. In the terminal app, enter a listed user ID or choose
`N` to create one; menu option **8** switches users. Profiles and their logs
are stored locally in separate `users/<user-id>.txt` files.

Complete the profile with a name, current and goal weight, height, age (18-100),
sex (M/F for the estimation formula), activity level, and the number of days to
reach the target weight. The number of days must be from 1 to 36,500. The
estimated calorie target uses 7,700 kcal per kilogram over that timeline, with
the resulting daily adjustment capped at 500 kcal in either direction.
Weight-loss targets are not set below the estimated BMR. If current and goal
weights are equal, the target uses maintenance calories. These are rough
estimates, not medical advice.

### Search foods and log meals

Food search requires an internet connection. Search by food name, select the
matching Open Food Facts entry, and enter the amount eaten in grams. The app
scales the listed per-100-g nutrition values to that serving; check the package
label because database entries can be incomplete or inaccurate.

Add foods to a meal, give the meal a name, then log it. You can save the meal as
a template and log it again later without searching for each food again. The
terminal version finishes building a meal when you submit a blank search; its
menu option **5** logs a saved meal.

### Review history and averages

Use the GUI date picker and **View day** to inspect a day, or menu option **4**
to view today's meals in the terminal app. **Finish day** marks the selected
date as finished for reference; it does not prevent later edits. In the GUI,
choose a date range and calculate average daily nutrition. The average includes
every calendar day in the range, including days with no logged meals.

### Configure reminders

Set a water interval in minutes; use `0` to disable it. Enter medicine and
sleep reminder times in 24-hour `HH:MM` format. Reminders only appear while the
app is running. The GUI displays popups; the terminal app prints reminders in
the console. These are not operating-system notifications.

### Save data and reset

Profile changes, meal logs, templates, finished days, and reminder settings are
saved to the active user's file. The GUI and terminal app share the same data,
so close one before opening the other to avoid simultaneous edits.

To erase saved user data on Windows, close the app and run `reset_data.exe`.
Confirm by typing `RESET`. The utility removes user `.txt` files in `users/` and
the legacy `health_data.txt`; other files are left untouched. Back up any data
you want to keep before resetting.

## Nutrition lookup and references

Searches use the Open Food Facts product search API:
<https://search.openfoodfacts.org/search>
See the [API documentation](https://search.openfoodfacts.org/docs)
and [terms of use](https://world.openfoodfacts.org/terms-of-use). Searches run
only when submitted, not while typing. Nutrition values are interpreted per
100 g and scaled by the serving size you enter. Product data is
community-contributed, so verify it against package labels when possible.

The GUI and terminal versions use the same `users/` folder. Close one version
before using the other to avoid overwriting changes with stale data.
