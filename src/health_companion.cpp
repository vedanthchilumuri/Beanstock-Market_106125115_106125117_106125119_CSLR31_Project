// Health companion app logic.
// This file handles profiles, food logs, saved meals, summaries, and reminder
// checks for the command-line version of the app.
#include "health_companion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace health {
namespace {

// Keep only valid user IDs so saved files and menu choices stay consistent.
bool validUserId(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(),
        [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-' || ch == '_';
        });
}

// Turn a timestamp into the local time so dates and reminders use the right clock.
std::tm localTimeFrom(std::time_t now) {
    std::tm result{};
#ifdef _WIN32
    if (localtime_s(&result, &now) != 0) {
#else
    if (localtime_r(&now, &result) == nullptr) {
#endif
        throw std::runtime_error("Could not read the local time.");
    }
    return result;
}

// Get the current date in YYYY-MM-DD form.
std::string todayDate() {
    const std::time_t now = std::time(nullptr);
    const std::tm local = localTimeFrom(now);
    std::ostringstream result;
    result << std::put_time(&local, "%Y-%m-%d");
    return result.str();
}

// Get the current time in HH:MM format for reminders.
std::string currentMinute() {
    const std::time_t now = std::time(nullptr);
    std::tm local = localTimeFrom(now);
    std::ostringstream result;
    result << std::put_time(&local, "%H:%M");
    return result.str();
}

// Parse a date in YYYY-MM-DD format.
bool parseDate(const std::string& value, std::tm& date) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i != 4 && i != 7 &&
            !std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    date = {};
    date.tm_year = std::stoi(value.substr(0, 4)) - 1900;
    date.tm_mon = std::stoi(value.substr(5, 2)) - 1;
    date.tm_mday = std::stoi(value.substr(8, 2));
    date.tm_hour = 12;
    if (date.tm_year < 70 || date.tm_mday < 1 ||
        date.tm_mon < 0 || date.tm_mon > 11) return false;
    const int year = date.tm_year;
    const int month = date.tm_mon;
    const int day = date.tm_mday;
    if (std::mktime(&date) == -1) return false;
    return date.tm_year == year && date.tm_mon == month && date.tm_mday == day;
}

// Return true when the date string is valid.
bool validDate(const std::string& value) {
    std::tm date{};
    return parseDate(value, date);
}

// Turn a date into a number that is easy to compare and sort.
long long dateOrdinal(const std::string& value) {
    int year = std::stoi(value.substr(0, 4));
    const int month = std::stoi(value.substr(5, 2));
    const int day = std::stoi(value.substr(8, 2));
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned int yearOfEra = static_cast<unsigned int>(year - era * 400);
    const unsigned int adjustedMonth = static_cast<unsigned int>(
        month + (month > 2 ? -3 : 9));
    const unsigned int dayOfYear =
        (153 * adjustedMonth + 2) / 5 + static_cast<unsigned int>(day - 1);
    const unsigned int dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<long long>(era) * 146097 + dayOfEra;
}

// Return true when the time string is in HH:MM format and is valid.
bool validTime(const std::string& value) {
    if (value.size() != 5 || value[2] != ':') {
        return false;
    }
    if (!std::isdigit(static_cast<unsigned char>(value[0])) ||
        !std::isdigit(static_cast<unsigned char>(value[1])) ||
        !std::isdigit(static_cast<unsigned char>(value[3])) ||
        !std::isdigit(static_cast<unsigned char>(value[4]))) {
        return false;
    }
    const int hours = std::stoi(value.substr(0, 2));
    const int minutes = std::stoi(value.substr(3, 2));
    return hours >= 0 && hours <= 23 && minutes >= 0 && minutes <= 59;
}

// Ask for a positive number and reject bad input.
bool readPositiveDouble(const std::string& prompt, double& value) {
    std::cout << prompt;
    if (!(std::cin >> value) || !std::isfinite(value) || value <= 0.0) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "Please enter a number greater than zero.\n";
        return false;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    return true;
}

// Remove extra spaces and line breaks from a string.
std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Make sure a meal has a name and at least one valid food entry.
void validateMeal(const std::string& name, const std::vector<MealFoodEntry>& foods) {
    if (trim(name).empty() || foods.empty()) {
        throw std::invalid_argument("Enter a meal name and add at least one food.");
    }
    for (const MealFoodEntry& food : foods) {
        if (trim(food.name).empty() || !std::isfinite(food.grams) ||
            food.grams <= 0.0 || food.grams > 10000.0 ||
            !std::isfinite(food.calories) || food.calories < 0.0 ||
            !std::isfinite(food.protein) || food.protein < 0.0 ||
            !std::isfinite(food.carbohydrates) || food.carbohydrates < 0.0 ||
            !std::isfinite(food.fat) || food.fat < 0.0) {
            throw std::invalid_argument("Meal foods must have valid serving and nutrition values.");
        }
    }
}

}  // namespace

// Build the app state and load any saved data.
HealthCompanion::HealthCompanion() {
    load();
}

// Clean up reminders before the app closes.
HealthCompanion::~HealthCompanion() {
    stopReminders();
}

// Return the full list of saved users.
const std::vector<HealthUser>& HealthCompanion::users() const {
    return users_;
}

// Return the currently selected user, if any.
const HealthUser* HealthCompanion::activeUser() const {
    return user_.id.empty() ? nullptr : &user_;
}

// Switch to the user with the matching ID.
bool HealthCompanion::selectUser(const std::string& id) {
    auto selected = std::find_if(users_.begin(), users_.end(),
        [&id](const HealthUser& user) { return user.id == id; });
    if (selected == users_.end()) return false;
    if (!user_.id.empty()) {
        save();
        selected = std::find_if(users_.begin(), users_.end(),
            [&id](const HealthUser& user) { return user.id == id; });
    }
    user_ = *selected;
    save();
    return true;
}

std::string HealthCompanion::createProfile(const std::string& name, double weightKg,
                                           double heightCm, int age, char sex,
                                           double activityFactor, double goalWeightKg,
                                           int daysToTargetWeight) {
    if (name.empty() || weightKg < 20.0 || weightKg > 300.0 ||
        goalWeightKg < 20.0 || goalWeightKg > 300.0 ||
        daysToTargetWeight < 1 || daysToTargetWeight > 36500 ||
        heightCm < 100.0 || heightCm > 250.0 || age < 18 || age > 100 ||
        (sex != 'M' && sex != 'F') ||
        (activityFactor != 1.2 && activityFactor != 1.375 &&
         activityFactor != 1.55 && activityFactor != 1.725)) {
        throw std::invalid_argument("Check the profile fields and their supported ranges.");
    }
    user_ = HealthUser{};
    user_.id = nextUserId();
    user_.name = name;
    user_.weightKg = weightKg;
    user_.heightCm = heightCm;
    user_.age = age;
    user_.sex = sex;
    user_.activityFactor = activityFactor;
    user_.goalWeightKg = goalWeightKg;
    user_.daysToTargetWeight = daysToTargetWeight;
    user_.hasProfile = true;
    users_.push_back(user_);
    save();
    return user_.id;
}

std::string HealthCompanion::updateProfile(const std::string& id,
                                           const std::string& name, double weightKg,
                                           double heightCm, int age, char sex,
                                           double activityFactor, double goalWeightKg,
                                           int daysToTargetWeight) {
    if (name.empty() || weightKg < 20.0 || weightKg > 300.0 ||
        goalWeightKg < 20.0 || goalWeightKg > 300.0 ||
        daysToTargetWeight < 1 || daysToTargetWeight > 36500 ||
        heightCm < 100.0 || heightCm > 250.0 || age < 18 || age > 100 ||
        (sex != 'M' && sex != 'F') ||
        (activityFactor != 1.2 && activityFactor != 1.375 &&
         activityFactor != 1.55 && activityFactor != 1.725)) {
        throw std::invalid_argument("Check the profile fields and their supported ranges.");
    }
    if (!selectUser(id)) {
        throw std::invalid_argument("That user ID was not found.");
    }
    user_.name = name;
    user_.weightKg = weightKg;
    user_.heightCm = heightCm;
    user_.age = age;
    user_.sex = sex;
    user_.activityFactor = activityFactor;
    user_.goalWeightKg = goalWeightKg;
    user_.daysToTargetWeight = daysToTargetWeight;
    user_.hasProfile = true;
    save();
    return user_.id;
}

void HealthCompanion::logMeal(const std::string& name,
                              const std::vector<MealFoodEntry>& foods) {
    logMealOnDate(name, foods, todayDate());
}

void HealthCompanion::logMealOnDate(const std::string& name,
                                    const std::vector<MealFoodEntry>& foods,
                                    const std::string& date) {
    if (user_.id.empty()) {
        throw std::invalid_argument("Select or create a user before logging a meal.");
    }
    if (!validDate(date) || date > todayDate()) {
        throw std::invalid_argument("Choose a valid date no later than today.");
    }
    validateMeal(name, foods);
    user_.meals.push_back({date, trim(name), foods});
    save();
}

void HealthCompanion::saveMealTemplate(const std::string& name,
                                       const std::vector<MealFoodEntry>& foods) {
    if (user_.id.empty()) {
        throw std::invalid_argument("Select or create a user before saving a meal.");
    }
    validateMeal(name, foods);
    const std::string mealName = trim(name);
    const auto existing = std::find_if(user_.savedMeals.begin(), user_.savedMeals.end(),
        [&mealName](const SavedMeal& meal) { return meal.name == mealName; });
    if (existing == user_.savedMeals.end()) {
        user_.savedMeals.push_back({mealName, foods});
    } else {
        existing->foods = foods;
    }
    save();
}

// Log a saved meal from the template list.
void HealthCompanion::logSavedMeal(std::size_t index) {
    logSavedMeal(index, todayDate());
}

// Log a saved meal from the template list.
void HealthCompanion::logSavedMeal(std::size_t index, const std::string& date) {
    if (index >= user_.savedMeals.size()) {
        throw std::invalid_argument("Choose a saved meal.");
    }
    const SavedMeal& meal = user_.savedMeals[index];
    logMealOnDate(meal.name, meal.foods, date);
}

// Mark a day as finished.
void HealthCompanion::finishDay(const std::string& date) {
    if (user_.id.empty() || !validDate(date) || date > todayDate()) {
        throw std::invalid_argument("Choose a valid date no later than today.");
    }
    if (!isDayFinished(date)) {
        user_.finishedDays.push_back(date);
        std::sort(user_.finishedDays.begin(), user_.finishedDays.end());
        save();
    }
}

// Check whether a day has been finished.
bool HealthCompanion::isDayFinished(const std::string& date) const {
    return std::find(user_.finishedDays.begin(), user_.finishedDays.end(), date) !=
        user_.finishedDays.end();
}

void HealthCompanion::setReminders(int waterIntervalMinutes,
                                   const std::string& medicineName,
                                   const std::string& medicineTime,
                                   const std::string& sleepTime) {
    if (user_.id.empty() || waterIntervalMinutes < 0 || waterIntervalMinutes > 1440 ||
        (!medicineName.empty() && !validTime(medicineTime)) ||
        (!sleepTime.empty() && !validTime(sleepTime))) {
        throw std::invalid_argument("Enter a valid water interval and reminder time.");
    }
    user_.waterIntervalMinutes = waterIntervalMinutes;
    user_.medicineName = medicineName;
    user_.medicineTime = medicineName.empty() ? "" : medicineTime;
    user_.sleepTime = sleepTime;
    save();
}

// Get the active user's nutrition targets.
NutritionTargets HealthCompanion::dailyNutritionTargets() const {
    return nutritionTargets();
}

// Total calories for today's meals.
double HealthCompanion::dailyMealCalories() const {
    double calories = 0.0;
    for (const MealEntry& meal : user_.meals) {
        if (meal.date == todayDate()) {
            for (const MealFoodEntry& food : meal.foods) {
                calories += food.calories;
            }
        }
    }
    return calories;
}

// List today's meals.
std::vector<MealEntry> HealthCompanion::todayMeals() const {
    return mealsForDate(todayDate());
}

// List meals for a specific date.
std::vector<MealEntry> HealthCompanion::mealsForDate(const std::string& date) const {
    std::vector<MealEntry> result;
    for (const MealEntry& meal : user_.meals) {
        if (meal.date == date) result.push_back(meal);
    }
    return result;
}

NutritionAverages HealthCompanion::averageNutrition(
    const std::string& startDate, const std::string& endDate) const {
    if (!validDate(startDate) || !validDate(endDate) || startDate > endDate ||
        // Get the current date in YYYY-MM-DD format.
        endDate > todayDate()) {
        throw std::invalid_argument(
            "Choose a valid date range ending today or earlier, with the start on or before the end.");
    }
    NutritionAverages averages;
    for (const MealEntry& meal : user_.meals) {
        if (meal.date < startDate || meal.date > endDate) continue;
        for (const MealFoodEntry& food : meal.foods) {
            averages.calories += food.calories;
            averages.protein += food.protein;
            averages.carbohydrates += food.carbohydrates;
            averages.fat += food.fat;
        }
    }
    averages.days = static_cast<std::size_t>(
        dateOrdinal(endDate) - dateOrdinal(startDate) + 1);
    if (averages.days > 0) {
        const double count = static_cast<double>(averages.days);
        averages.calories /= count;
        averages.protein /= count;
        averages.carbohydrates /= count;
        averages.fat /= count;
    }
    return averages;
}

// Stop the reminder thread before switching users or exiting.
// Stop the reminder thread.
void HealthCompanion::stopReminders() {
    remindersRunning_ = false;
    if (reminderThread_.joinable()) {
        reminderThread_.join();
    }
}

// =============================================================================
// PART 3: run()  —  the heart of the terminal app (called from main()).
// 1) log in a user, 2) start the reminder thread, 3) loop showing the menu
// and dispatching choices until the user picks 0, 4) clean up and save.
// =============================================================================
// Run the main app loop.
void HealthCompanion::run() {
    // Login screen. chooseUser() fills user_ with the selected/new user.
    chooseUser();
    // If user_.id is still empty, input ended (e.g. Ctrl+Z / closed window)
    // before anyone was chosen, so there is nothing to do.
    if (user_.id.empty()) {
        return;
    }
    // Start the background thread. std::thread(&Class::method, this) runs
    // this->reminderLoop() on a NEW thread, in parallel with this one.
    // The main thread continues below to handle the menu, while the
    // reminder thread wakes every 5 s to print reminders.
    remindersRunning_ = true;
    reminderThread_ = std::thread(&HealthCompanion::reminderLoop, this);
    // Welcome banner. `usersDirectory_ << "\\."` prints  users\.
    std::cout << "=== Digital Health Companion ===\n"
              << "Food nutrition lookup: Open Food Facts (internet connection required)\n"
              << "Nutrition data is community-contributed; check product labels when possible.\n"
              << "Signed in as " << user_.name << " (" << user_.id << ").\n"
              << "User profiles and meal logs are saved in individual files in "
              << usersDirectory_ << "\\.\n"
              << "This app provides general wellness estimates, not medical advice.\n\n";

    // ---- Main menu loop (an "event loop") ----------------------------------
    while (true) {
        showMenu();
        // Read the WHOLE line as a string (not `cin >> int`). This way typing
        // "abc" or "12x" simply doesn't match any option — cin never enters
        // a fail state here, and nothing is left behind in the buffer.
        std::string choice;
        std::getline(std::cin, choice);
        // !std::cin is true on end-of-input (Ctrl+Z then Enter on Windows,
        // or the window closing). Treat that the same as choosing 0.
        if (!std::cin || choice == "0") {
            break;
        }
        // Every menu action runs inside try. Parts 1 and 2 report problems
        // by THROWING exceptions (invalid meal, no internet, file error...).
        // Catching them here means one failed action prints a message and
        // returns to the menu instead of crashing the whole program.
        try {
            // Dispatch table written as an if / else-if chain: each menu
            // number calls one member function.
            if (choice == "1") {
                createOrUpdateProfile();
            } else if (choice == "2") {
                showProfile();
            } else if (choice == "3") {
                createMeal();
            } else if (choice == "4") {
                showTodayMeals();
            } else if (choice == "5") {
                useSavedMeal();
            } else if (choice == "6") {
                configureReminders();
            } else if (choice == "7") {
                printDailySummary();
            } else if (choice == "8") {
                // ---- Switch user ----
                // 1. Stop the reminder thread FIRST, because chooseUser()
                //    is about to replace user_ and the thread reads user_.
                //    Stopping it avoids a race and stops the old user's
                //    reminders appearing for the new user.
                stopReminders();
                // 2. Write the current user's data to disk.
                save();
                // 3. Show the login screen again.
                chooseUser();
                if (!user_.id.empty()) {
                    // 4. Schedule the first water reminder one interval from
                    //    now (at least 1 minute so it never fires instantly).
                    //    The braces { } limit the lock_guard's lifetime: the
                    //    mutex is unlocked automatically at the closing brace
                    //    (RAII). (The thread is stopped here, so the lock is
                    //    just being careful.)
                    {
                        std::lock_guard<std::mutex> lock(reminderMutex_);
                        nextWaterReminder_ = std::chrono::steady_clock::now() +
                            std::chrono::minutes(std::max(user_.waterIntervalMinutes, 1));
                    }
                    // 5. Start a fresh reminder thread for the new user.
                    remindersRunning_ = true;
                    reminderThread_ = std::thread(&HealthCompanion::reminderLoop, this);
                }
            } else {
                // Anything else (blank line, "9", "abc"...) is rejected.
                std::cout << "Choose a number from 0 to 8.\n";
            }
        } catch (const std::exception& error) {
            // error.what() is the message given when the exception was thrown.
            std::cout << "Could not complete that action: " << error.what() << '\n';
        }
        std::cout << '\n';   // blank line before the menu is shown again
    }
    // ---- Exit path (option 0 or end of input) ----
    // Stop and join the thread before saving/returning; a std::thread that
    // is still running when the object is destroyed would crash the program.
    stopReminders();
    save();
    std::cout << "Your data has been saved. Goodbye!\n";
}

// Load saved data from disk.
void HealthCompanion::load() {
    std::stringstream inputData;
    std::vector<std::filesystem::path> userFiles;
    if (std::filesystem::exists(usersDirectory_)) {
        if (!std::filesystem::is_directory(usersDirectory_)) {
            throw std::runtime_error(usersDirectory_ + " exists but is not a directory.");
        }
        for (const auto& entry : std::filesystem::directory_iterator(usersDirectory_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".txt") {
                userFiles.push_back(entry.path());
            }
        }
    }

    std::sort(userFiles.begin(), userFiles.end());
    if (!userFiles.empty()) {
        for (const auto& path : userFiles) {
            std::ifstream input(path);
            if (!input) {
                throw std::runtime_error("Could not read " + path.string() + ".");
            }
            inputData << input.rdbuf();
            if (input.bad()) {
                throw std::runtime_error("Could not read " + path.string() + ".");
            }
            inputData << '\n';
        }
    } else {
        std::ifstream legacyInput(legacyDataFile_);
        if (!legacyInput && std::filesystem::exists(legacyDataFile_)) {
            throw std::runtime_error("Could not read " + legacyDataFile_ + ".");
        }
        if (legacyInput) {
            inputData << legacyInput.rdbuf();
            if (legacyInput.bad()) {
                throw std::runtime_error("Could not read " + legacyDataFile_ + ".");
            }
        }
    }
    std::istream& input = inputData;

    HealthUser legacy;
    bool hasLegacyData = false;
    std::unordered_map<std::string, std::size_t> currentLoggedMeal;
    std::unordered_map<std::string, std::size_t> currentSavedMeal;
    auto findUser = [this](const std::string& id) -> HealthUser* {
        const auto found = std::find_if(users_.begin(), users_.end(),
            [&id](const HealthUser& user) { return user.id == id; });
        return found == users_.end() ? nullptr : &*found;
    };

    std::string record;
    while (std::getline(input, record)) {
        std::istringstream row(record);
        std::string type;
        row >> type;
        if (type == "USER") {
            HealthUser user;
            row >> user.id >> std::quoted(user.name) >> user.weightKg >> user.heightCm
                >> user.age >> user.sex >> user.activityFactor >> user.goalWeightKg;
            if (row && !user.id.empty()) {
                if (!(row >> user.daysToTargetWeight)) {
                    user.daysToTargetWeight = 0;
                }
                user.hasProfile = user.weightKg > 0.0 && user.heightCm > 0.0 &&
                    user.age >= 18 && user.goalWeightKg > 0.0;
                users_.push_back(user);
            }
        } else if (type == "FINISHED_DAY_V2") {
            std::string id;
            std::string date;
            row >> id >> date;
            if (row && validDate(date)) {
                if (HealthUser* user = findUser(id)) {
                    user->finishedDays.push_back(date);
                }
            }
        } else if (type == "MEAL_V2") {
            std::string id;
            MealEntry meal;
            row >> id >> meal.date >> std::quoted(meal.name);
            if (row) {
                if (HealthUser* user = findUser(id)) {
                    user->meals.push_back(std::move(meal));
                    currentLoggedMeal[id] = user->meals.size() - 1;
                }
            }
        } else if (type == "MEAL_FOOD_V2") {
            std::string id;
            MealFoodEntry food;
            row >> id >> std::quoted(food.name) >> food.grams >> food.calories
                >> food.protein >> food.carbohydrates >> food.fat;
            const auto meal = currentLoggedMeal.find(id);
            if (row && meal != currentLoggedMeal.end()) {
                if (HealthUser* user = findUser(id)) {
                    user->meals[meal->second].foods.push_back(std::move(food));
                }
            }
        } else if (type == "SAVED_MEAL_V2") {
            std::string id;
            SavedMeal meal;
            row >> id >> std::quoted(meal.name);
            if (row) {
                if (HealthUser* user = findUser(id)) {
                    user->savedMeals.push_back(std::move(meal));
                    currentSavedMeal[id] = user->savedMeals.size() - 1;
                }
            }
        } else if (type == "SAVED_FOOD_V2") {
            std::string id;
            MealFoodEntry food;
            row >> id >> std::quoted(food.name) >> food.grams >> food.calories
                >> food.protein >> food.carbohydrates >> food.fat;
            const auto meal = currentSavedMeal.find(id);
            if (row && meal != currentSavedMeal.end()) {
                if (HealthUser* user = findUser(id)) {
                    user->savedMeals[meal->second].foods.push_back(std::move(food));
                }
            }
        } else if (type == "MEAL") {
            MealEntry meal;
            std::string first;
            row >> first;
            HealthUser* user = !first.empty() && first[0] == 'U'
                ? findUser(first)
                : nullptr;
            if (user != nullptr) {
                MealFoodEntry food;
                row >> meal.date >> std::quoted(meal.name) >> food.grams >> food.calories
                    >> food.protein >> food.carbohydrates >> food.fat;
                if (row) {
                    food.name = meal.name;
                    meal.foods.push_back(std::move(food));
                    user->meals.push_back(std::move(meal));
                }
            } else {
                MealFoodEntry food;
                meal.date = first;
                row >> std::quoted(meal.name) >> food.grams >> food.calories
                    >> food.protein >> food.carbohydrates >> food.fat;
                if (row) {
                    food.name = meal.name;
                    meal.foods.push_back(std::move(food));
                    legacy.meals.push_back(std::move(meal));
                    hasLegacyData = true;
                }
            }
        } else if (type == "MEDICINE") {
            std::string first;
            row >> first;
            if (!first.empty() && first[0] == 'U') {
                if (HealthUser* user = findUser(first)) {
                    row >> std::quoted(user->medicineName) >> user->medicineTime;
                }
            } else {
                std::istringstream legacyRow(record);
                legacyRow >> type >> std::quoted(legacy.medicineName) >> legacy.medicineTime;
                hasLegacyData = static_cast<bool>(legacyRow);
            }
        } else if (type == "WATER") {
            std::string first;
            row >> first;
            if (!first.empty() && first[0] == 'U') {
                if (HealthUser* user = findUser(first)) {
                    row >> user->waterIntervalMinutes;
                }
            } else {
                try {
                    legacy.waterIntervalMinutes = std::stoi(first);
                    hasLegacyData = true;
                } catch (const std::exception&) {
                    throw std::runtime_error(
                        "Invalid water reminder in " + legacyDataFile_ + ".");
                }
            }
        } else if (type == "SLEEP") {
            std::string first;
            row >> first;
            if (!first.empty() && first[0] == 'U') {
                if (HealthUser* user = findUser(first)) {
                    row >> user->sleepTime;
                }
            } else {
                legacy.sleepTime = first;
                hasLegacyData = true;
            }
        } else if (type == "PROFILE") {
            row >> legacy.weightKg >> legacy.heightCm;
            legacy.hasProfile = static_cast<bool>(row) &&
                legacy.weightKg > 0.0 && legacy.heightCm > 0.0;
            hasLegacyData = legacy.hasProfile;
        }
    }
    if (hasLegacyData) {
        legacy.id = nextUserId();
        legacy.name = "Existing User";
        users_.push_back(legacy);
    }
    if (userFiles.empty() && !users_.empty()) {
        save();
    }
}

// Save the current data.
void HealthCompanion::save() {
    if (!user_.id.empty()) {
        const auto active = std::find_if(users_.begin(), users_.end(),
            [this](const HealthUser& user) { return user.id == user_.id; });
        if (active == users_.end()) {
            users_.push_back(user_);
        } else {
            *active = user_;
        }
    }
    std::filesystem::create_directories(usersDirectory_);
    for (const HealthUser& user : users_) {
        if (!validUserId(user.id)) {
            throw std::runtime_error("Could not save a user with an invalid ID.");
        }
        const std::filesystem::path userFile =
            std::filesystem::path(usersDirectory_) / (user.id + ".txt");
        std::ofstream output(userFile, std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Could not write " + userFile.string() + ".");
        }
        output << std::setprecision(10);
        output << "USER " << user.id << ' ' << std::quoted(user.name) << ' '
               << user.weightKg << ' ' << user.heightCm << ' ' << user.age << ' '
               << user.sex << ' ' << user.activityFactor << ' ' << user.goalWeightKg << ' '
               << user.daysToTargetWeight << '\n';
        for (const std::string& date : user.finishedDays) {
            output << "FINISHED_DAY_V2 " << user.id << ' ' << date << '\n';
        }
        for (const MealEntry& meal : user.meals) {
            output << "MEAL_V2 " << user.id << ' ' << meal.date << ' '
                   << std::quoted(meal.name) << '\n';
            for (const MealFoodEntry& food : meal.foods) {
                output << "MEAL_FOOD_V2 " << user.id << ' '
                       << std::quoted(food.name) << ' ' << food.grams << ' '
                       << food.calories << ' ' << food.protein << ' '
                       << food.carbohydrates << ' ' << food.fat << '\n';
            }
        }
        for (const SavedMeal& meal : user.savedMeals) {
            output << "SAVED_MEAL_V2 " << user.id << ' '
                   << std::quoted(meal.name) << '\n';
            for (const MealFoodEntry& food : meal.foods) {
                output << "SAVED_FOOD_V2 " << user.id << ' '
                       << std::quoted(food.name) << ' ' << food.grams << ' '
                       << food.calories << ' ' << food.protein << ' '
                       << food.carbohydrates << ' ' << food.fat << '\n';
            }
        }
        if (!user.medicineName.empty()) {
            output << "MEDICINE " << user.id << ' ' << std::quoted(user.medicineName)
                   << ' ' << user.medicineTime << '\n';
        }
        if (user.waterIntervalMinutes > 0) {
            output << "WATER " << user.id << ' ' << user.waterIntervalMinutes << '\n';
        }
        if (!user.sleepTime.empty()) {
            output << "SLEEP " << user.id << ' ' << user.sleepTime << '\n';
        }
        if (!output) {
            throw std::runtime_error("Could not save user data to " +
                                     userFile.string() + ".");
        }
    }
}

// Create the next available user ID.
std::string HealthCompanion::nextUserId() const {
    unsigned int nextId = 1;
    for (const HealthUser& user : users_) {
        if (user.id.size() < 2 || user.id[0] != 'U') continue;
        try {
            nextId = std::max(nextId, static_cast<unsigned int>(
                std::stoul(user.id.substr(1)) + 1));
        } catch (const std::exception&) {
            continue;
        }
    }
    std::ostringstream result;
    result << 'U' << std::setw(4) << std::setfill('0') << nextId;
    return result.str();
}

// =============================================================================
// PART 3: chooseUser()  —  the login screen.
// Lists every user loaded from users/*.txt and keeps asking until a valid ID
// is entered or a new user is created. On return, user_ holds the ACTIVE
// user (a working copy); users_ is the vector of ALL users.
// =============================================================================
// Choose the active user.
void HealthCompanion::chooseUser() {
    // Start with an empty user (id == "" means "nobody is signed in").
    user_ = HealthUser{};
    while (true) {   // repeat until a valid choice is made
        std::cout << "\n=== Select user ===\n";
        // Print "U0001 - Name" for every user in the vector.
        for (const HealthUser& user : users_) {
            std::cout << user.id << " - " << user.name << '\n';
        }
        std::cout << "Enter a user ID, or N to create a user: ";
        std::string choice;
        std::getline(std::cin, choice);
        choice = trim(choice);           // remove spaces around the input
        if (!std::cin) return;           // input closed -> give up (user_ stays empty)
        if (choice == "N" || choice == "n") {
            createUser();
            // createUser() leaves user_.id empty if profile entry failed;
            // in that case loop back and show the list again.
            if (!user_.id.empty()) return;
            continue;
        }
        // LINEAR SEARCH through the vector: std::find_if visits each element
        // and returns an iterator to the first one for which the lambda
        // returns true — O(n) in the number of users. The lambda captures
        // `choice` by reference ([&choice]) so it can compare against it.
        // (IDs are case-sensitive: "u0001" will not match "U0001".)
        const auto selected = std::find_if(users_.begin(), users_.end(),
            [&choice](const HealthUser& user) { return user.id == choice; });
        // find_if returns end() when nothing matched.
        if (selected == users_.end()) {
            std::cout << "That user ID was not found.\n";
            continue;
        }
        // Copy the found user into user_ (the active working copy).
        user_ = *selected;
        // First water reminder = now + interval (minimum 1 minute). Locked
        // with the mutex because nextWaterReminder_ is shared with the
        // reminder thread. steady_clock is used (not the wall clock) because
        // it never jumps if the PC's time is changed.
        {
            std::lock_guard<std::mutex> lock(reminderMutex_);
            nextWaterReminder_ = std::chrono::steady_clock::now() +
                std::chrono::minutes(std::max(user_.waterIntervalMinutes, 1));
        }
        save();
        return;
    }
}

// =============================================================================
// PART 3: createUser()  —  "N" on the login screen.
// Gives the new user the next free ID (U0001, U0002, ...) and runs the same
// profile prompts used by menu option 1.
// =============================================================================
// Create a new user record.
void HealthCompanion::createUser() {
    user_ = HealthUser{};
    user_.id = nextUserId();     // Part 2: highest existing number + 1
    createOrUpdateProfile();     // asks name, weight, height, age, ...
    // hasProfile becomes true only if every prompt was answered validly.
    if (user_.hasProfile) {
        // BUG FIX: the original code did `users_.push_back(user_);` here.
        // But createOrUpdateProfile() has ALREADY called save(), and save()
        // adds the active user to users_ if it is missing. Pushing again
        // created TWO copies of the same user in the vector. Every later
        // save() updated only the first copy, then wrote the stale second
        // copy over the same users/U000X.txt file — so all meals and
        // reminders entered in that session were lost on exit. save() alone
        // inserts the user exactly once.
        save();
        std::cout << "Your user ID is " << user_.id
                  << ". Use it to select this profile at the next launch.\n";
    } else {
        // Profile entry failed/cancelled -> go back to "nobody signed in".
        user_ = HealthUser{};
    }
}

// =============================================================================
// PART 3: showMenu()  —  prints the main menu. `const` because it only reads.
// =============================================================================
// Display the main menu.
void HealthCompanion::showMenu() const {
    std::cout << "Signed in: " << user_.name << " (" << user_.id << ")\n"
              << "1. Create/update profile\n"
              << "2. View profile and nutrition targets\n"
              << "3. Build and log a meal\n"
              << "4. View today's meals\n"
              << "5. Log a saved meal\n"
              << "6. Configure water, medicine, and sleep reminders\n"
              << "7. Daily summary\n"
              << "8. Switch user\n"
              << "0. Save and exit\n"
              << "> ";
}

// =============================================================================
// PART 3: createOrUpdateProfile()  —  menu option 1, and also used by
// createUser(). Asks for every profile field, validates each answer, and
// only if ALL are valid copies them into user_ and saves.
// Design: values are read into LOCAL variables first, so a mistake halfway
// through (e.g. invalid age) leaves the existing profile completely unchanged.
// Each invalid input prints a message and `return`s to the menu.
// =============================================================================
// Create or update the profile.
void HealthCompanion::createOrUpdateProfile() {
    // When updating, show the current name in [brackets]; pressing Enter
    // keeps it. The ternary (?:) prints nothing for a brand-new user.
    std::cout << "Name" << (user_.name.empty() ? "" : " [" + user_.name + "]")
              << ": ";
    std::string name;
    std::getline(std::cin, name);       // getline allows spaces in names
    name = trim(name);
    if (name.empty()) name = user_.name;   // blank -> keep old name
    if (name.empty()) {                    // still blank -> new user typed nothing
        std::cout << "Name cannot be empty.\n";
        return;
    }
    double weight = 0.0;
    double height = 0.0;
    double goalWeight = 0.0;
    int daysToTargetWeight = 0;
    // || short-circuits: if weight is invalid, height is never asked.
    if (!readPositiveDouble("Current weight (kg): ", weight) ||
        !readPositiveDouble("Height (cm): ", height)) {
        return;
    }

    // Age: the Mifflin-St Jeor formula is for adults, hence 18-100.
    // Same input pattern as readPositiveDouble: check, clear(), ignore().
    std::cout << "Age (18-100): ";
    int age = 0;
    if (!(std::cin >> age) || age < 18 || age > 100) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "Please enter an age from 18 to 100 for the adult calorie formula.\n";
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    // Sex: the formula adds +5 for male and -161 for female.
    // Read as a string so we can check it is exactly ONE character.
    std::cout << "Sex used by the Mifflin-St Jeor formula (M/F): ";
    std::string sex;
    std::getline(std::cin, sex);
    if (sex.size() != 1) {
        std::cout << "Enter M or F.\n";
        return;
    }
    // toupper so 'm' and 'f' are accepted too. The cast to unsigned char
    // is required: passing a negative char to toupper is undefined behaviour.
    const char formulaSex = static_cast<char>(
        std::toupper(static_cast<unsigned char>(sex[0])));
    if (formulaSex != 'M' && formulaSex != 'F') {
        std::cout << "Enter M or F.\n";
        return;
    }

    // Activity level: user picks 1-4; it is mapped to a multiplier below.
    std::cout << "Activity level: 1 sedentary, 2 light, 3 moderate, 4 very active: ";
    int activityChoice = 0;
    if (!(std::cin >> activityChoice) || activityChoice < 1 || activityChoice > 4) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "Choose activity level 1, 2, 3, or 4.\n";
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    // LOOKUP TABLE (a plain array): index = choice - 1.
    //   1 -> 1.2 (sedentary), 2 -> 1.375 (light),
    //   3 -> 1.55 (moderate), 4 -> 1.725 (very active)
    // `static const` = created once and shared by all calls; never changes.
    // Using an array instead of a switch gives O(1) direct indexing.
    static const double activityFactors[] = {1.2, 1.375, 1.55, 1.725};

    if (!readPositiveDouble("Goal weight (kg): ", goalWeight)) {
        return;
    }
    std::cout << "Number of days to reach target weight (1-36500): ";
    if (!(std::cin >> daysToTargetWeight) || daysToTargetWeight < 1 ||
        daysToTargetWeight > 36500) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "Enter a number of days from 1 to 36500.\n";
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    // Range check done at the END (after all questions) for the 3 numbers
    // read by readPositiveDouble, which only checks "> 0".
    if (weight < 20.0 || weight > 300.0 || goalWeight < 20.0 ||
        goalWeight > 300.0 || height < 100.0 || height > 250.0) {
        std::cout << "Use current and goal weights from 20 to 300 kg, and a height "
                     "from 100 to 250 cm.\n";
        return;
    }

    // ---- All inputs valid: commit them to the active user ----
    user_.name = name;
    user_.weightKg = weight;
    user_.heightCm = height;
    user_.age = age;
    user_.sex = formulaSex;
    user_.activityFactor = activityFactors[activityChoice - 1];
    user_.goalWeightKg = goalWeight;
    user_.daysToTargetWeight = daysToTargetWeight;
    user_.hasProfile = true;   // marks the profile as complete
    save();                    // Part 2: write users/<id>.txt
    // Immediately show the calorie/macro targets computed from the new
    // profile (formula lives in nutritionTargets(), Part 1).
    std::cout << "Profile saved. Your estimated nutrition targets:\n";
    const NutritionTargets targets = nutritionTargets();
    std::cout << "Daily calories: " << targets.calories << " kcal; protein: "
              << targets.protein << " g; carbohydrates: " << targets.carbohydrates
              << " g; fat: " << targets.fat << " g.\n";
}

// =============================================================================
// PART 3: showProfile()  —  menu option 2. Prints the profile, BMI and the
// daily targets. `const` member function: it only reads data.
// =============================================================================
// Display the current profile summary.
void HealthCompanion::showProfile() const {
    if (!user_.hasProfile) {
        std::cout << "Complete your profile to see personalized calorie estimates.\n";
        return;
    }
    // BMI = weight(kg) / height(m)^2, so convert cm -> m first.
    const double heightMeters = user_.heightCm / 100.0;
    // std::fixed + setprecision(1) => numbers print with exactly 1 decimal
    // (70 -> 70.0). These are "sticky": they stay in effect on std::cout for
    // the rest of the program, which is why later screens also show 1
    // decimal after option 2 has been used. The activity factor is
    // temporarily printed with 3 decimals (1.550), then switched back to 1.
    std::cout << std::fixed << std::setprecision(1)
              << "User ID: " << user_.id << "\n"
              << "Name: " << user_.name << "\n"
              << "Current weight: " << user_.weightKg << " kg\n"
              << "Goal weight: " << user_.goalWeightKg << " kg\n"
              << "Days to reach target weight: ";
    if (user_.daysToTargetWeight > 0) {
        std::cout << user_.daysToTargetWeight << '\n';
    } else {
        std::cout << "not set (using the legacy calorie adjustment)\n";
    }
    std::cout
              << "Height: " << user_.heightCm << " cm\n"
              << "Age: " << user_.age << "\n"
              << "Activity multiplier: " << std::setprecision(3)
              << user_.activityFactor << std::setprecision(1) << "\n"
              << "BMI: " << user_.weightKg / (heightMeters * heightMeters)
              << " (informational only)\n";
    // Targets use the requested timeline, with the daily adjustment capped at 500 kcal.
    const NutritionTargets targets = nutritionTargets();
    std::cout << "Estimated daily calorie target: " << targets.calories << " kcal\n"
              << "Estimated nutrition targets: "
              << targets.protein << " g protein, " << targets.carbohydrates
              << " g carbohydrates, " << targets.fat << " g fat.\n";
    std::cout << "Formula: Mifflin-St Jeor BMR x activity level, adjusted using "
                 "7700 kcal per kg over the requested timeline (capped at 500 kcal/day). "
                 "Weight-loss targets are not set below BMR.\n"
              << "This is a general adult estimate, not a medical plan.\n";
}

// Calculate nutrition targets from the user profile.
NutritionTargets HealthCompanion::nutritionTargets() const {
    NutritionTargets targets;
    if (!user_.hasProfile) {
        return targets;
    }
    const double sexAdjustment = user_.sex == 'M' ? 5.0 : -161.0;
    const double bmr = 10.0 * user_.weightKg + 6.25 * user_.heightCm
        - 5.0 * user_.age + sexAdjustment;
    const double maintenanceCalories = bmr * user_.activityFactor;
    if (user_.goalWeightKg < user_.weightKg ||
        user_.goalWeightKg > user_.weightKg) {
        const double adjustment = user_.daysToTargetWeight > 0
            ? std::clamp((user_.goalWeightKg - user_.weightKg) * 7700.0 /
                             user_.daysToTargetWeight,
                         -500.0, 500.0)
            : (user_.goalWeightKg < user_.weightKg ? -500.0 : 500.0);
        targets.calories = user_.goalWeightKg < user_.weightKg
            ? std::max(bmr, maintenanceCalories + adjustment)
            : maintenanceCalories + adjustment;
    } else {
        targets.calories = maintenanceCalories;
    }
    targets.protein = user_.weightKg * 1.2;
    targets.carbohydrates = targets.calories * 0.5 / 4.0;
    targets.fat = targets.calories * 0.3 / 9.0;
    return targets;
}

// =============================================================================
// PART 3: createMeal()  —  menu option 3: "Build and log a meal".
// Loop: search a food online -> pick a result -> enter grams -> add to the
// meal. A blank search ends the loop. Then name the meal, optionally save
// it as a reusable template, and log it for today.
// Example: idli 200 g -> 130 kcal/100g x 2 = 260 kcal stored for that item.
// =============================================================================
// Build and log a new meal.
void HealthCompanion::createMeal() {
    // The meal being built: a dynamic array (vector) of food items.
    // It grows with push_back as foods are added.
    std::vector<MealFoodEntry> foods;
    while (true) {
        std::cout << "Search food to add to the meal (blank to finish): ";
        std::string query;
        std::getline(std::cin, query);
        query = trim(query);
        if (query.empty()) {
            // Blank on the very first search = cancel (nothing to log).
            if (foods.empty()) return;
            break;   // blank after adding foods = "I'm done, log it"
        }

        // Part 2 does the HTTPS request to Open Food Facts and parses the
        // JSON into a vector<FoodProduct> (values are per 100 g).
        // If there is no internet it THROWS; that exception jumps straight
        // out of createMeal() to the catch block in run(), which prints
        // "Could not complete that action: ..." (the half-built meal is lost).
        const std::vector<FoodProduct> results = foodDatabase_.search(query);
        if (results.empty()) {
            std::cout << "No foods with calorie information were found. Try another search.\n";
            continue;   // back to the top of the loop: ask for a new search
        }

        // Show at most 10 results. std::min<std::size_t> forces both
        // arguments to the same type (size_t) so min() compiles.
        const std::size_t limit = std::min<std::size_t>(results.size(), 10);
        std::cout << "\nSearch results (nutrition values per 100 g):\n";
        // Displayed numbers start at 1 (i + 1) because people count from 1;
        // the vector index starts at 0.
        for (std::size_t i = 0; i < limit; ++i) {
            const FoodProduct& food = results[i];
            std::cout << (i + 1) << ". " << food.name << " - "
                      << food.caloriesPer100g << " kcal, protein "
                      << food.proteinPer100g << " g, carbs "
                      << food.carbohydratesPer100g << " g, fat "
                      << food.fatPer100g << " g\n";
        }
        std::cout << "Select a result (0 to search again): ";
        int selection = 0;
        // Valid range is 0..limit. The cast to size_t is so we compare two
        // unsigned values (avoids a signed/unsigned comparison warning);
        // it's safe because selection < 0 was already rejected first.
        if (!(std::cin >> selection) || selection < 0 ||
            // Handle size t>.
            static_cast<std::size_t>(selection) > limit) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "Invalid selection.\n";
            continue;
        }
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (selection == 0) continue;   // 0 = none of these, search again

        double grams = 0.0;
        if (!readPositiveDouble("Serving size (g): ", grams)) continue;
        if (grams > 10000.0) {          // sanity limit: 10 kg
            std::cout << "Serving size must be at most 10,000 g.\n";
            continue;
        }
        // selection is 1-based, vector index is 0-based -> selection - 1.
        // A const reference avoids copying the FoodProduct.
        const FoodProduct& food = results[static_cast<std::size_t>(selection - 1)];
        // Database values are per 100 g, so scale by grams / 100.
        // e.g. 150 g -> multiplier 1.5 -> 60 kcal/100g becomes 90 kcal.
        const double multiplier = grams / 100.0;
        // Brace-initialise a MealFoodEntry {name, grams, kcal, P, C, F}
        // (same order as the struct fields) and append it to the meal.
        foods.push_back({food.name, grams,
                         food.caloriesPer100g * multiplier,
                         food.proteinPer100g * multiplier,
                         food.carbohydratesPer100g * multiplier,
                         food.fatPer100g * multiplier});
        std::cout << "Added " << grams << " g of " << food.name << ".\n";
    }

    // ---- Loop finished: at least one food was added ----
    std::cout << "Meal name: ";
    std::string name;
    std::getline(std::cin, name);
    name = trim(name);
    if (name.empty()) {
        std::cout << "Meal name cannot be empty; the meal was not logged.\n";
        return;
    }
    // Default answer is No (capital N in "y/N" is the usual convention);
    // only "y" or "Y" saves it.
    std::cout << "Save this meal for future use? (y/N): ";
    std::string choice;
    std::getline(std::cin, choice);
    if (choice == "y" || choice == "Y") {
        // Part 1: stores a copy in user_.savedMeals (a template for option 5).
        // If a saved meal with the same name exists, its foods are replaced.
        saveMealTemplate(name, foods);
    }
    // Part 1: validates the meal, appends {today's date, name, foods} to
    // user_.meals, and saves to the file.
    logMeal(name, foods);
    std::cout << "Logged " << name << " with " << foods.size() << " food(s).\n";
}

// =============================================================================
// PART 3: showTodayMeals()  —  menu option 4 (also used by option 7).
// Goes through ALL logged meals, keeps only today's (a filter), prints each
// food, a per-meal total, and a grand total for the day.
// Complexity: O(total number of food items ever logged), since every meal
// is checked — a linear scan of the vector.
// =============================================================================
// Show the meals logged for today.
void HealthCompanion::showTodayMeals() const {
    // Running totals for the whole day (accumulators).
    double calories = 0.0;
    double protein = 0.0;
    double carbohydrates = 0.0;
    double fat = 0.0;
    bool found = false;   // did we print at least one meal?
    std::cout << "Today's meals (" << todayDate() << "):\n";
    // Range-based for loop over the vector of meals. `const MealEntry&`
    // = read-only reference, so no copy of each meal is made.
    for (const MealEntry& meal : user_.meals) {
        // Dates are stored as "YYYY-MM-DD" strings, so comparing strings is
        // the same as comparing dates. Skip meals from other days.
        if (meal.date != todayDate()) {
            continue;
        }
        found = true;
        // Totals for this one meal (reset for each meal).
        double mealCalories = 0.0;
        double mealProtein = 0.0;
        double mealCarbohydrates = 0.0;
        double mealFat = 0.0;
        std::cout << "- " << meal.name << ":\n";
        // Nested loop: each meal is itself a vector of food items.
        for (const MealFoodEntry& food : meal.foods) {
            std::cout << "  " << food.name << ", " << food.grams << " g: "
                      << food.calories << " kcal\n";
            mealCalories += food.calories;
            mealProtein += food.protein;
            mealCarbohydrates += food.carbohydrates;
            mealFat += food.fat;
        }
        std::cout << "  Meal total: " << mealCalories << " kcal; P "
                  << mealProtein << " g, C " << mealCarbohydrates << " g, F "
                  << mealFat << " g\n";
        // Add this meal's totals to the day's totals.
        calories += mealCalories;
        protein += mealProtein;
        carbohydrates += mealCarbohydrates;
        fat += mealFat;
    }
    if (!found) {
        std::cout << "No meals logged yet.\n";
    }
    std::cout << "Total: " << calories << " kcal; protein " << protein
              << " g; carbohydrates " << carbohydrates << " g; fat "
              << fat << " g.\n";
}

// =============================================================================
// PART 3: useSavedMeal()  —  menu option 5: "Log a saved meal".
// Saved meals are templates (e.g. "Usual breakfast"). This lists them,
// lets the user pick one by number, and logs a copy of it for today —
// no internet search needed, since the nutrition values are already stored.
// =============================================================================
// Log a saved meal template.
void HealthCompanion::useSavedMeal() {
    if (user_.savedMeals.empty()) {
        std::cout << "No saved meals yet. Build a meal and choose to save it for future use.\n";
        return;
    }
    std::cout << "Saved meals:\n";
    // Index-based loop (not range-for) because we need the position i to
    // print the menu number i + 1. vector gives O(1) access by index.
    for (std::size_t i = 0; i < user_.savedMeals.size(); ++i) {
        std::cout << (i + 1) << ". " << user_.savedMeals[i].name << " ("
                  << user_.savedMeals[i].foods.size() << " food(s))\n";
    }
    std::cout << "Choose a meal to log (0 to cancel): ";
    // size_t is unsigned, so no "< 0" check is needed: a negative entry
    // like -1 wraps around to a huge number and fails the "> size" check.
    std::size_t selection = 0;
    if (!(std::cin >> selection) || selection > user_.savedMeals.size()) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "Invalid selection.\n";
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (selection == 0) return;            // cancelled
    // Part 1: copies the template's foods into a new MealEntry dated today.
    // Convert the 1-based menu number to a 0-based vector index.
    logSavedMeal(selection - 1);
    std::cout << "Logged saved meal " << user_.savedMeals[selection - 1].name << ".\n";
}

// =============================================================================
// PART 3: configureReminders()  —  menu option 6.
// Asks for: water interval (minutes), medicine name + time, sleep time.
// All three answers are validated FIRST; only if all are valid are they
// stored (under the mutex, since the reminder thread reads them) and saved.
// A blank/0 answer disables that reminder.
// =============================================================================
// Configure reminder settings.
void HealthCompanion::configureReminders() {
    // Water: repeating reminder every N minutes. 0 = off, max 1440 (= 24 h).
    std::cout << "Water reminder interval in minutes (0 disables): ";
    int interval = 0;
    if (!(std::cin >> interval) || interval < 0 || interval > 1440) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "Enter an interval from 0 to 1440 minutes.\n";
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    // Medicine: once a day at a fixed clock time. Time is only asked if a
    // name was given. validTime() checks the exact "HH:MM" format with
    // HH 00-23 and MM 00-59 (so "8:00" is rejected; type "08:00").
    std::string medicineName;
    std::string medicineTime;
    std::cout << "Medicine name (blank disables): ";
    std::getline(std::cin, medicineName);
    medicineName = trim(medicineName);
    if (!medicineName.empty()) {
        std::cout << "Medicine reminder time (HH:MM, 24-hour): ";
        std::getline(std::cin, medicineTime);
        if (!validTime(medicineTime)) {
            // Returning here means NOTHING is changed — the old reminder
            // settings stay as they were (all-or-nothing update).
            std::cout << "Invalid time. Reminders were not changed.\n";
            return;
        }
    }

    // Sleep: once a day at a fixed clock time; blank = off.
    std::string sleepTime;
    std::cout << "Sleep reminder time (HH:MM, blank disables): ";
    std::getline(std::cin, sleepTime);
    sleepTime = trim(sleepTime);
    if (!sleepTime.empty() && !validTime(sleepTime)) {
        std::cout << "Invalid time. Reminders were not changed.\n";
        return;
    }

    // ---- Commit the new settings (CRITICAL SECTION) ----
    // The reminder thread reads these same fields every 5 seconds. Locking
    // reminderMutex_ makes sure it never reads them while they are half
    // updated (e.g. new medicine name but old time). lock_guard locks in
    // its constructor and unlocks automatically when the block's } is
    // reached — even if an exception occurs (RAII).
    {
        std::lock_guard<std::mutex> lock(reminderMutex_);
        user_.waterIntervalMinutes = interval;
        user_.medicineName = medicineName;
        // No medicine name -> store an empty time too.
        user_.medicineTime = medicineName.empty() ? "" : medicineTime;
        user_.sleepTime = sleepTime;
        // Restart the water countdown from now.
        nextWaterReminder_ = std::chrono::steady_clock::now() +
            std::chrono::minutes(std::max(user_.waterIntervalMinutes, 1));
    }
    // Save outside the lock: file writing is slow and doesn't need it.
    save();
    std::cout << "Reminders saved. Daily reminders use your computer's local time.\n";
}

// =============================================================================
// PART 3: reminderLoop()  —  runs on the SEPARATE reminder thread.
// Started by run() with std::thread(&HealthCompanion::reminderLoop, this).
// Every 5 seconds it checks three reminders and prints any that are due,
// while the main thread is busy waiting for keyboard input. This is why a
// "[REMINDER]" line can appear in the middle of a menu.
//
//   Water    : interval-based ("every N minutes"), uses steady_clock.
//   Medicine : clock-time based ("at 08:00"), uses the local wall clock.
//   Sleep    : clock-time based, same as medicine.
//
// Note: reminders only work while the program is open (no OS notifications).
// =============================================================================
// Check reminders on a timer.
void HealthCompanion::reminderLoop() {
    // Remember the last "date minute" we announced, e.g. "2026-11-02 08:00".
    // Because we check every 5 s, the current minute matches the medicine
    // time about 12 times; this prevents printing the reminder 12 times.
    // Including the DATE lets it fire again tomorrow at the same minute.
    std::string lastMedicineNotice;
    std::string lastSleepNotice;
    // Keep going until stopReminders() sets the atomic flag to false.
    while (remindersRunning_) {
        // Local copies of the shared settings.
        std::string medicineName;
        std::string medicineTime;
        std::string sleepTime;
        bool waterDue = false;
        // ---- CRITICAL SECTION: read shared data under the mutex ----
        // We copy what we need and release the lock quickly. Printing and
        // sleeping happen OUTSIDE the lock, so the main thread is never
        // kept waiting (e.g. in configureReminders) for long.
        {
            std::lock_guard<std::mutex> lock(reminderMutex_);
            medicineName = user_.medicineName;
            medicineTime = user_.medicineTime;
            sleepTime = user_.sleepTime;
            // Water: due if enabled and the scheduled time has passed.
            if (user_.waterIntervalMinutes > 0 &&
                // Handle now.
                std::chrono::steady_clock::now() >= nextWaterReminder_) {
                waterDue = true;
                // Schedule the NEXT one, interval minutes from now.
                nextWaterReminder_ = std::chrono::steady_clock::now() +
                    std::chrono::minutes(user_.waterIntervalMinutes);
            }
        }   // <- mutex released here

        const std::string date = todayDate();      // "YYYY-MM-DD"
        const std::string minute = currentMinute(); // "HH:MM"
        // Each message starts with '\n' (so it doesn't join whatever the
        // user is typing) and ends by re-printing the "> " prompt.
        // std::flush forces the text to appear immediately instead of
        // waiting in the output buffer.
        if (waterDue) {
            std::cout << "\n[REMINDER] Time to drink some water.\n> " << std::flush;
        }
        // Medicine: enabled, the clock now shows the chosen minute, and we
        // haven't already announced this exact date+minute.
        if (!medicineName.empty() && minute == medicineTime &&
            lastMedicineNotice != date + " " + minute) {
            lastMedicineNotice = date + " " + minute;
            std::cout << "\n[REMINDER] Medicine: " << medicineName << ".\n> " << std::flush;
        }
        // Sleep: same logic as medicine.
        if (!sleepTime.empty() && minute == sleepTime &&
            lastSleepNotice != date + " " + minute) {
            lastSleepNotice = date + " " + minute;
            std::cout << "\n[REMINDER] Your scheduled sleep time is coming up.\n> " << std::flush;
        }
        // Sleep 5 s instead of spinning in a tight loop, so the thread uses
        // almost no CPU. 5 s < 60 s, so a 1-minute window is never missed.
        // Trade-off: stopReminders() may wait up to 5 s for this to finish.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// =============================================================================
// PART 3: printDailySummary()  —  menu option 7.
// Re-uses showTodayMeals() (code reuse) and then compares the calories
// eaten today with the daily calorie target from the profile.
// =============================================================================
// Show the daily nutrition summary.
void HealthCompanion::printDailySummary() const {
    std::cout << "\n=== Daily summary: " << todayDate() << " ===\n";
    showTodayMeals();
    if (user_.hasProfile) {
        // Target from Part 1 (Mifflin-St Jeor); consumed = sum of today's
        // food calories (Part 1's dailyMealCalories()).
        const NutritionTargets targets = nutritionTargets();
        double consumedCalories = 0.0;
        consumedCalories = dailyMealCalories();
        std::cout << "Estimated calorie target: " << targets.calories
                  << " kcal; logged: " << consumedCalories << " kcal.\n";
    } else {
        std::cout << "Create a profile to see estimated daily nutrition targets.\n";
    }
}

}  // namespace health
