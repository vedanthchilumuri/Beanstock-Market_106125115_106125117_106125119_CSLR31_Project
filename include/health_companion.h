#ifndef HEALTH_COMPANION_H
#define HEALTH_COMPANION_H

#include "food_database.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace health {

// One food item inside a meal draft or saved meal.
struct MealFoodEntry {
    std::string name;
    double grams = 0.0;
    double calories = 0.0;
    double protein = 0.0;
    double carbohydrates = 0.0;
    double fat = 0.0;
};

// A meal logged for a specific user on a specific date.
struct MealEntry {
    std::string date;
    std::string name;
    std::vector<MealFoodEntry> foods;
};

// A reusable meal template that can be re-added later.
struct SavedMeal {
    std::string name;
    std::vector<MealFoodEntry> foods;
};

// Daily nutrition goals derived from the active profile.
struct NutritionTargets {
    double calories = 0.0;
    double protein = 0.0;
    double carbohydrates = 0.0;
    double fat = 0.0;
};

// Average nutrition totals across a date window.
struct NutritionAverages {
    std::size_t days = 0;
    double calories = 0.0;
    double protein = 0.0;
    double carbohydrates = 0.0;
    double fat = 0.0;
};

// One user profile with meals, goals, and reminder settings.
struct HealthUser {
    std::string id;
    std::string name;
    double weightKg = 0.0;
    double heightCm = 0.0;
    int age = 0;
    char sex = 'F';
    double activityFactor = 1.2;
    double goalWeightKg = 0.0;
    int daysToTargetWeight = 0;
    bool hasProfile = false;
    std::vector<MealEntry> meals;
    std::vector<SavedMeal> savedMeals;
    std::vector<std::string> finishedDays;
    std::string medicineName;
    std::string medicineTime;
    std::string sleepTime;
    int waterIntervalMinutes = 0;
};

// Main app logic for profiles, meals, reminders, and daily progress.
class HealthCompanion {
public:
    HealthCompanion();
    ~HealthCompanion();

    // Start the app menu loop for the current user.
    void run();

    // Return the full list of saved users.
    const std::vector<HealthUser>& users() const;

    // Return the active user, or nullptr if no one is selected.
    const HealthUser* activeUser() const;

    // Switch to the user with the matching ID.
    bool selectUser(const std::string& id);

    // Create a new profile from the supplied health details.
    std::string createProfile(const std::string& name, double weightKg,
                              double heightCm, int age, char sex,
                              double activityFactor, double goalWeightKg,
                              int daysToTargetWeight);

    // Update an existing profile with fresh details.
    std::string updateProfile(const std::string& id, const std::string& name,
                              double weightKg, double heightCm, int age,
                              char sex, double activityFactor, double goalWeightKg,
                              int daysToTargetWeight);

    // Add a meal for the current day.
    void logMeal(const std::string& name, const std::vector<MealFoodEntry>& foods);

    // Add a meal on a specific date.
    void logMealOnDate(const std::string& name,
                       const std::vector<MealFoodEntry>& foods,
                       const std::string& date);

    // Save the meal as a reusable template.
    void saveMealTemplate(const std::string& name,
                          const std::vector<MealFoodEntry>& foods);

    // Log a saved meal for today.
    void logSavedMeal(std::size_t index);

    // Log a saved meal on a chosen date.
    void logSavedMeal(std::size_t index, const std::string& date);

    // Mark a day as complete for the active user.
    void finishDay(const std::string& date);

    // Check whether a day has already been completed.
    bool isDayFinished(const std::string& date) const;

    // Update the reminder settings for water, medicine, and sleep.
    void setReminders(int waterIntervalMinutes, const std::string& medicineName,
                      const std::string& medicineTime, const std::string& sleepTime);

    // Calculate the target nutrition for the active user.
    NutritionTargets dailyNutritionTargets() const;

    // Total calories logged for today.
    double dailyMealCalories() const;

    // Return the meals saved for the current date.
    std::vector<MealEntry> todayMeals() const;

    // Return all meals for a selected date.
    std::vector<MealEntry> mealsForDate(const std::string& date) const;

    // Estimate average nutrition over a date range.
    NutritionAverages averageNutrition(const std::string& startDate,
                                       const std::string& endDate) const;

private:
    // Loads saved users from disk.
    void load();

    // Saves the current state back to disk.
    void save();

    // Prompts the user to select an active profile.
    void chooseUser();

    // Creates a new empty user entry.
    void createUser();

    // Stops the reminder thread if it is running.
    void stopReminders();

    // Generates the next available user identifier.
    std::string nextUserId() const;

    // Prints the main menu.
    void showMenu() const;

    // Handles the create/update profile flow.
    void createOrUpdateProfile();

    // Shows the currently selected profile summary.
    void showProfile() const;

    // Displays meals logged for the current day.
    void showTodayMeals() const;

    // Collects a new meal from the user and stores it in the profile.
    void createMeal();

    /**
     * Lets the user reuse a previously saved meal template.
     */
    void useSavedMeal();

    /**
     * Manages reminder configuration input from the user.
     */
    void configureReminders();

    /**
     * Background process that periodically triggers reminder notifications.
     */
    void reminderLoop();

    /**
     * Prints a summary of daily nutrition and progress for the active user.
     */
    void printDailySummary() const;

    /**
     * Computes nutrition targets from the user's current profile.
     */
    NutritionTargets nutritionTargets() const;

    FoodDatabase foodDatabase_;
    std::vector<HealthUser> users_;
    HealthUser user_;
    std::string usersDirectory_ = "users";
    std::string legacyDataFile_ = "health_data.txt";

    /**
     * Indicates whether the reminder thread is currently running.
     */
    std::atomic<bool> remindersRunning_{false};

    /**
     * Thread used for monitoring reminder schedules and notifications.
     */
    std::thread reminderThread_;

    /**
     * Guards access to reminder state while the background thread runs.
     */
    mutable std::mutex reminderMutex_;

    /**
     * Tracks the next scheduled water reminder time.
     */
    std::chrono::steady_clock::time_point nextWaterReminder_;
};

}  // namespace health

#endif
