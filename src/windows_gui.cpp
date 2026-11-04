// windows_gui.cpp
// The Windows version of the health app. It builds the main window, keeps the UI
// responsive, and passes user actions through to the health logic.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include "health_companion.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kFoodSearchComplete = WM_APP + 1;
constexpr UINT_PTR kReminderTimer = 1;

enum ControlId {
    UserSelect = 100,
    NewUser,
    Name,
    Weight,
    GoalWeight,
    DaysToTargetWeight,
    Height,
    Age,
    Sex,
    Activity,
    SaveProfile,
    ProfileSummary,
    WaterInterval,
    MedicineName,
    MedicineTime,
    SleepTime,
    SaveReminders,
    FoodQuery,
    SearchFood,
    FoodResults,
    ServingSize,
    MealName,
    AddFood,
    LogMeal,
    SaveMealTemplate,
    MealDraft,
    SavedMeals,
    LogSavedMeal,
    DayPicker,
    ViewDay,
    FinishDay,
    TodayMeals,
    DayStatus,
    RangeStart,
    RangeEnd,
    ThisMonth,
    CalculateAverage,
    AverageSummary,
    Status
};

struct SearchPayload {
    std::vector<health::FoodProduct> products;
    std::string error;
};

// ---------------------------------------------------------------------------
// Window state
// These handles and collections are shared by the window procedures and the
// helper functions that create, refresh, and respond to the UI controls.
// ---------------------------------------------------------------------------
HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_title = nullptr;
HWND g_profileTitle = nullptr;
HWND g_userSelect = nullptr;
HWND g_nameLabel = nullptr;
HWND g_name = nullptr;
HWND g_weightLabel = nullptr;
HWND g_weight = nullptr;
HWND g_goalWeightLabel = nullptr;
HWND g_goalWeight = nullptr;
HWND g_daysToTargetWeightLabel = nullptr;
HWND g_daysToTargetWeight = nullptr;
HWND g_heightLabel = nullptr;
HWND g_height = nullptr;
HWND g_ageLabel = nullptr;
HWND g_age = nullptr;
HWND g_sexLabel = nullptr;
HWND g_sex = nullptr;
HWND g_activityLabel = nullptr;
HWND g_activity = nullptr;
HWND g_profileSummary = nullptr;
HWND g_waterInterval = nullptr;
HWND g_waterLabel = nullptr;
HWND g_medicineName = nullptr;
HWND g_medicineLabel = nullptr;
HWND g_medicineTime = nullptr;
HWND g_medicineTimeLabel = nullptr;
HWND g_sleepTime = nullptr;
HWND g_sleepLabel = nullptr;
HWND g_foodQuery = nullptr;
HWND g_foodLabel = nullptr;
HWND g_searchButton = nullptr;
HWND g_foodResults = nullptr;
HWND g_servingSize = nullptr;
HWND g_servingLabel = nullptr;
HWND g_mealName = nullptr;
HWND g_mealNameLabel = nullptr;
HWND g_mealDraftList = nullptr;
HWND g_mealDraftLabel = nullptr;
HWND g_savedMeals = nullptr;
HWND g_savedMealsLabel = nullptr;
HWND g_todayMeals = nullptr;
HWND g_mealsLabel = nullptr;
HWND g_dayPicker = nullptr;
HWND g_dayStatus = nullptr;
HWND g_rangeStart = nullptr;
HWND g_rangeEnd = nullptr;
HWND g_averageRangeLabel = nullptr;
HWND g_rangeStartLabel = nullptr;
HWND g_rangeEndLabel = nullptr;
HWND g_averageSummary = nullptr;
HWND g_status = nullptr;
std::unique_ptr<health::HealthCompanion> g_companion;
std::vector<health::FoodProduct> g_products;
std::vector<health::MealFoodEntry> g_mealFoods;
std::thread g_searchThread;
std::chrono::steady_clock::time_point g_nextWaterReminder;
std::string g_lastMedicineReminder;
std::string g_lastSleepReminder;
bool g_creatingUser = false;
int g_scrollOffset = 0;
int g_clientHeight = 0;
constexpr int kContentHeight = 1250;

void layoutControls(int width, int height);
void scrollPage(int amount);

// Pull the current text out of a control and drop the final null character.
std::string text(HWND control) {
    const int size = GetWindowTextLengthA(control);
    std::string value(static_cast<std::size_t>(size) + 1, '\0');
    GetWindowTextA(control, value.data(), size + 1);
    value.resize(static_cast<std::size_t>(size));
    return value;
}

// Put a new value into a control.
void setText(HWND control, const std::string& value) {
    SetWindowTextA(control, value.c_str());
}

// Format a number for the UI so it reads clearly on screen.
std::string number(double value, int precision = 1) {
    std::ostringstream result;
    result << std::fixed << std::setprecision(precision) << value;
    return result.str();
}

// Show a quick update in the app status area.
void setStatus(const std::string& value) {
    if (g_status != nullptr) setText(g_status, value);
}

// ---------------------------------------------------------------------------
// Helper: showError()
// Show an error dialog and also leave the message in the window's status area
// so the failure remains visible after the dialog is dismissed.
// ---------------------------------------------------------------------------
// Show a friendly error box and leave the message visible in the app.
void showError(const std::string& value) {
    MessageBoxA(g_window, value.c_str(), "Health Companion",
                MB_OK | MB_ICONERROR);
    setStatus(value);
}

// ---------------------------------------------------------------------------
// Callback: scrollableControlProcedure()
// Forward mouse-wheel movement to the containing page. When a child control
// receives focus, scroll the page if that control is outside the visible area.
// ---------------------------------------------------------------------------
LRESULT CALLBACK scrollableControlProcedure(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam,
                                             UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEWHEEL) {
        const int steps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        if (steps != 0) scrollPage(-steps * 90);
        return 0;
    }
    if (message == WM_SETFOCUS) {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        MapWindowPoints(nullptr, g_window,
                        reinterpret_cast<POINT*>(&bounds), 2);
        RECT client{};
        GetClientRect(g_window, &client);
        if (bounds.top < 0) {
            scrollPage(bounds.top);
        } else if (bounds.bottom > client.bottom) {
            scrollPage(bounds.bottom - client.bottom);
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Helper: control()
// Create a child window with the shared font and scroll handling. If subclass
// installation fails, destroy the new control and report the initialization
// error rather than leaving a partially configured UI.
// ---------------------------------------------------------------------------
// Create a standard child control.
HWND control(const char* type, const char* label, DWORD style, int id) {
    HWND result = CreateWindowExA(
        0, type, label, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, g_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_instance, nullptr);
    if (result != nullptr) {
        SendMessageA(result, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        if (!SetWindowSubclass(result, scrollableControlProcedure, 1, 0)) {
            DestroyWindow(result);
            throw std::runtime_error("Could not enable scrolling for a window control.");
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Helpers: label(), edit(), button()
// Apply the standard styles used for the window's text, input, and action
// controls. Numeric edits use ES_NUMBER to restrict keyboard input.
// ---------------------------------------------------------------------------
// Create a text label control.
HWND label(const char* textValue) {
    return control("STATIC", textValue, SS_LEFT, 0);
}

// Create an input field control.
HWND edit(int id, bool numeric = false) {
    DWORD style = WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP;
    if (numeric) style |= ES_NUMBER;
    return control("EDIT", "", style, id);
}

// Create a button control.
HWND button(const char* title, int id) {
    return control("BUTTON", title, BS_PUSHBUTTON | WS_TABSTOP, id);
}

// Place a control on the page.
void put(HWND controlWindow, int x, int y, int width, int height) {
    MoveWindow(controlWindow, x, y - g_scrollOffset, width, height, TRUE);
}

// ---------------------------------------------------------------------------
// Helpers: setDatePicker(), dateText()
// Transfer dates between the native calendar controls and the application's
// YYYY-MM-DD string format. dateText() rejects an invalid calendar selection.
// ---------------------------------------------------------------------------
// Set a date picker to a value.
void setDatePicker(HWND picker, const SYSTEMTIME& date) {
    DateTime_SetSystemtime(picker, GDT_VALID, &date);
}

// Read the selected date from a date picker.
std::string dateText(HWND picker) {
    SYSTEMTIME date{};
    if (DateTime_GetSystemtime(picker, &date) != GDT_VALID) {
        throw std::runtime_error("Choose a valid calendar date.");
    }
    std::ostringstream result;
    result << date.wYear << '-' << std::setfill('0') << std::setw(2)
           << date.wMonth << '-' << std::setw(2) << date.wDay;
    return result.str();
}

// Create the app's window controls.
void createControls() {
    // -----------------------------------------------------------------------
    // Create the controls for profiles, reminders, food search, meal logging,
    // the selected day's history, and date-range nutrition averages.
    // -----------------------------------------------------------------------
    g_title = label("Digital Health Companion");
    g_profileTitle = label("User profile");
    g_userSelect = control("COMBOBOX", "",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, UserSelect);
    button("New user", NewUser);
    g_nameLabel = label("Name");
    g_name = edit(Name);
    g_weightLabel = label("Current weight (kg)");
    g_weight = edit(Weight);
    g_goalWeightLabel = label("Goal weight (kg)");
    g_goalWeight = edit(GoalWeight);
    g_daysToTargetWeightLabel = label("Days to reach target weight");
    g_daysToTargetWeight = edit(DaysToTargetWeight, true);
    g_heightLabel = label("Height (cm)");
    g_height = edit(Height);
    g_ageLabel = label("Age");
    g_age = edit(Age, true);
    g_sexLabel = label("Sex");
    g_sex = control("COMBOBOX", "",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, Sex);
    SendMessageA(g_sex, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Female"));
    SendMessageA(g_sex, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Male"));
    g_activityLabel = label("Activity");
    g_activity = control("COMBOBOX", "",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, Activity);
    for (const char* item : {"Sedentary", "Light", "Moderate", "Very active"}) {
        SendMessageA(g_activity, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    button("Save profile", SaveProfile);
    g_profileSummary = label("Save a profile to get a personalized calorie estimate.");

    g_waterLabel = label("Water interval (minutes; 0 disables)");
    g_waterInterval = edit(WaterInterval, true);
    g_medicineLabel = label("Medicine");
    g_medicineName = edit(MedicineName);
    g_medicineTimeLabel = label("Medicine time (HH:MM)");
    g_medicineTime = edit(MedicineTime);
    g_sleepLabel = label("Sleep time (HH:MM)");
    g_sleepTime = edit(SleepTime);
    button("Save reminders", SaveReminders);

    g_foodLabel = label("Food search");
    g_foodQuery = edit(FoodQuery);
    g_searchButton = button("Search", SearchFood);
    g_foodResults = control("LISTBOX", "",
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_HSCROLL,
        FoodResults);
    g_servingLabel = label("Serving (g)");
    g_servingSize = edit(ServingSize, true);
    setText(g_servingSize, "100");
    button("Add selected food to meal", AddFood);
    g_mealNameLabel = label("Meal name");
    g_mealName = edit(MealName);
    g_mealDraftLabel = label("Foods in this meal");
    g_mealDraftList = control("LISTBOX", "",
        LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_HSCROLL,
        MealDraft);
    button("Log meal", LogMeal);
    button("Save for later", SaveMealTemplate);
    g_savedMealsLabel = label("Saved meals");
    g_savedMeals = control("COMBOBOX", "",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, SavedMeals);
    button("Log saved meal", LogSavedMeal);
    g_mealsLabel = label("Foods eaten on selected day");
    g_dayPicker = control(DATETIMEPICK_CLASSA, "",
        DTS_SHORTDATEFORMAT | WS_TABSTOP, DayPicker);
    button("View day", ViewDay);
    button("Finish day", FinishDay);
    g_dayStatus = label("");
    g_todayMeals = control("LISTBOX", "",
        LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_HSCROLL,
        TodayMeals);
    g_rangeStart = control(DATETIMEPICK_CLASSA, "",
        DTS_SHORTDATEFORMAT | WS_TABSTOP, RangeStart);
    g_rangeEnd = control(DATETIMEPICK_CLASSA, "",
        DTS_SHORTDATEFORMAT | WS_TABSTOP, RangeEnd);
    g_averageRangeLabel = label("Average date range");
    g_rangeStartLabel = label("Start");
    g_rangeEndLabel = label("End");
    button("This month", ThisMonth);
    button("Calculate average", CalculateAverage);
    g_averageSummary = label("Choose a date range to see average daily nutrition.");
    g_status = label("Ready. Food search requires an internet connection.");

    SYSTEMTIME now{};
    GetLocalTime(&now);
    setDatePicker(g_dayPicker, now);
    setDatePicker(g_rangeEnd, now);
    SYSTEMTIME monthStart = now;
    monthStart.wDay = 1;
    setDatePicker(g_rangeStart, monthStart);
    SendMessageA(g_userSelect, CB_SETDROPPEDWIDTH, 260, 0);
    SendMessageA(g_savedMeals, CB_SETDROPPEDWIDTH, 400, 0);
    SendMessageA(g_sex, CB_SETCURSEL, 0, 0);
    SendMessageA(g_activity, CB_SETCURSEL, 0, 0);
}

// Lay out controls for the window.
void layoutControls(int width, int height) {
    // -----------------------------------------------------------------------
    // Lay out controls in columns based on the client width and synchronize
    // the vertical scrollbar with the fixed height of the page content.
    // -----------------------------------------------------------------------
    g_clientHeight = height;
    g_scrollOffset = std::min(g_scrollOffset, std::max(kContentHeight - height, 0));
    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = kContentHeight - 1;
    scroll.nPage = static_cast<UINT>(std::max(height, 0));
    scroll.nPos = g_scrollOffset;
    SetScrollInfo(g_window, SB_VERT, &scroll, TRUE);

    const int margin = 18;
    const int gap = 10;
    const int full = std::max(width - margin * 2, 300);
    const int quarter = (full - gap * 3) / 4;
    const int half = (full - gap) / 2;
    const int third = (full - gap * 2) / 3;
    const int x2 = margin + quarter + gap;
    const int x3 = x2 + quarter + gap;
    const int x4 = x3 + quarter + gap;

    put(g_title, margin, 12, full, 30);
    put(g_profileTitle, margin, 48, full, 20);
    put(g_userSelect, margin, 72, half, 180);
    put(GetDlgItem(g_window, NewUser), margin + half + gap, 72, half, 26);

    put(g_nameLabel, margin, 110, quarter, 20);
    put(g_name, margin, 132, quarter, 25);
    put(g_weightLabel, x2, 110, quarter, 20);
    put(g_weight, x2, 132, quarter, 25);
    put(g_goalWeightLabel, x3, 110, quarter, 20);
    put(g_goalWeight, x3, 132, quarter, 25);
    put(g_heightLabel, x4, 110, quarter, 20);
    put(g_height, x4, 132, quarter, 25);

    put(g_ageLabel, margin, 165, quarter, 20);
    put(g_age, margin, 187, quarter, 25);
    put(g_sexLabel, x2, 165, quarter, 20);
    put(g_sex, x2, 187, quarter, 180);
    put(g_activityLabel, x3, 165, quarter, 20);
    put(g_activity, x3, 187, quarter, 180);
    put(g_daysToTargetWeightLabel, x4, 165, quarter, 20);
    put(g_daysToTargetWeight, x4, 187, quarter, 25);
    put(GetDlgItem(g_window, SaveProfile), x4, 220, quarter, 28);
    put(g_profileSummary, margin, 255, full, 25);

    put(g_waterLabel, margin, 290, third, 20);
    put(g_waterInterval, margin, 312, third, 25);

    put(g_medicineLabel, margin + third + gap, 290, third, 20);
    put(g_medicineName, margin + third + gap, 312, third, 25);
    put(g_medicineTimeLabel, margin, 350, third, 20);
    put(g_medicineTime, margin, 372, third, 25);
    put(g_sleepLabel, margin + third + gap, 350, third, 20);
    put(g_sleepTime, margin + third + gap, 372, third, 25);
    put(GetDlgItem(g_window, SaveReminders), margin + 2 * (third + gap),
        372, third, 28);

    put(g_foodLabel, margin, 417, full, 20);
    put(g_foodQuery, margin, 439, full - 100, 26);
    put(g_searchButton, margin + full - 90, 439, 90, 28);
    put(g_foodResults, margin, 473, full, 100);
    put(g_servingLabel, margin, 581, 110, 20);
    put(g_servingSize, margin, 603, 150, 25);
    put(GetDlgItem(g_window, AddFood), margin + 160, 601, 230, 28);
    put(g_mealNameLabel, margin, 641, 110, 20);
    put(g_mealName, margin, 663, half, 25);
    put(g_mealDraftLabel, margin, 697, full, 20);
    put(g_mealDraftList, margin, 719, full, 58);
    put(GetDlgItem(g_window, LogMeal), margin, 785, 150, 28);
    put(GetDlgItem(g_window, SaveMealTemplate), margin + 160, 785, 170, 28);
    put(g_savedMealsLabel, margin, 823, 140, 20);
    put(g_savedMeals, margin, 845, half, 180);
    put(GetDlgItem(g_window, LogSavedMeal), margin + half + gap, 845, half, 28);
    put(g_mealsLabel, margin, 885, full, 20);
    put(g_dayPicker, margin, 907, 190, 25);
    put(GetDlgItem(g_window, ViewDay), margin + 200, 905, 110, 28);
    put(GetDlgItem(g_window, FinishDay), margin + 320, 905, 120, 28);
    put(g_dayStatus, margin + 450, 907, full - 450, 25);
    put(g_todayMeals, margin, 945, full, 165);
    put(g_averageSummary, margin, 1120, full, 44);
    put(g_averageRangeLabel, margin, 1175, full, 20);
    put(g_rangeStartLabel, margin, 1199, 45, 20);
    put(g_rangeStart, margin + 50, 1195, 170, 25);
    put(g_rangeEndLabel, margin + 230, 1199, 35, 20);
    put(g_rangeEnd, margin + 270, 1195, 170, 25);
    put(GetDlgItem(g_window, ThisMonth), margin + 450, 1193, 120, 28);
    put(GetDlgItem(g_window, CalculateAverage), margin + 580, 1193, 170, 28);
    put(g_status, margin, 1210, full, 32);
}

// Scroll the page content.
void scrollPage(int amount) {
    g_scrollOffset = std::clamp(g_scrollOffset + amount, 0,
                                std::max(kContentHeight - g_clientHeight, 0));
    RECT client{};
    GetClientRect(g_window, &client);
    layoutControls(client.right, client.bottom);
}

void refreshDashboard();

// ---------------------------------------------------------------------------
// Helper: refreshMealDraft()
// Rebuild the pending meal list from g_mealFoods, which holds the foods added
// so far but not yet logged or saved as a template.
// ---------------------------------------------------------------------------
// Refresh the meal draft list.
void refreshMealDraft() {
    SendMessageA(g_mealDraftList, LB_RESETCONTENT, 0, 0);
    for (const health::MealFoodEntry& food : g_mealFoods) {
        std::ostringstream line;
        line << food.name << " | " << number(food.grams) << " g | "
             << number(food.calories) << " kcal";
        const std::string value = line.str();
        SendMessageA(g_mealDraftList, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(value.c_str()));
    }
}

// ---------------------------------------------------------------------------
// Helper: refreshUsers()
// Rebuild the user selector, then load the active user's profile and reminder
// settings into the controls. If no user is active, select the first available
// profile or prompt the user to create one.
// ---------------------------------------------------------------------------
// Refresh the user list.
void refreshUsers() {
    const LRESULT previousCount = SendMessageA(g_userSelect, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < previousCount; ++i) {
        delete reinterpret_cast<std::string*>(
            SendMessageA(g_userSelect, CB_GETITEMDATA, i, 0));
    }
    SendMessageA(g_userSelect, CB_RESETCONTENT, 0, 0);
    const auto& users = g_companion->users();
    for (const health::HealthUser& user : users) {
        const std::string entry = user.id + " - " + user.name;
        const LRESULT index = SendMessageA(g_userSelect, CB_ADDSTRING, 0,
                                            reinterpret_cast<LPARAM>(entry.c_str()));
        SendMessageA(g_userSelect, CB_SETITEMDATA, index,
                     reinterpret_cast<LPARAM>(new std::string(user.id)));
    }
    const health::HealthUser* active = g_companion->activeUser();
    if (active == nullptr) {
        if (!users.empty()) {
            const std::string firstUserId = users.front().id;
            if (g_companion->selectUser(firstUserId)) refreshUsers();
        } else {
            MessageBoxA(g_window, "Create a user profile to start tracking meals and nutrition.",
                        "Health Companion", MB_OK | MB_ICONINFORMATION);
            g_creatingUser = true;
        }
        return;
    }
    g_creatingUser = false;
    for (int i = 0; i < static_cast<int>(users.size()); ++i) {
        if (users[static_cast<std::size_t>(i)].id == active->id) {
            SendMessageA(g_userSelect, CB_SETCURSEL, i, 0);
            break;
        }
    }
    setText(g_name, active->name);
    setText(g_weight, number(active->weightKg));
    setText(g_goalWeight, number(active->goalWeightKg));
    setText(g_daysToTargetWeight, active->daysToTargetWeight > 0
        ? std::to_string(active->daysToTargetWeight) : "");
    setText(g_height, number(active->heightCm));
    setText(g_age, std::to_string(active->age));
    SendMessageA(g_sex, CB_SETCURSEL, active->sex == 'M' ? 1 : 0, 0);
    const int activityIndex = active->activityFactor < 1.3 ? 0
        : active->activityFactor < 1.46 ? 1
        : active->activityFactor < 1.64 ? 2 : 3;
    SendMessageA(g_activity, CB_SETCURSEL, activityIndex, 0);
    setText(g_waterInterval, std::to_string(active->waterIntervalMinutes));
    setText(g_medicineName, active->medicineName);
    setText(g_medicineTime, active->medicineTime);
    setText(g_sleepTime, active->sleepTime);
    g_nextWaterReminder = std::chrono::steady_clock::now() +
        std::chrono::minutes(std::max(active->waterIntervalMinutes, 1));
    refreshDashboard();
}

// ---------------------------------------------------------------------------
// Helper: refreshDashboard()
// Rebuild the saved-meal selector and selected-day list, calculate meal and
// daily nutrient totals, and update the profile and day-status summaries.
// ---------------------------------------------------------------------------
// Refresh the dashboard values.
void refreshDashboard() {
    const health::HealthUser* active = g_companion->activeUser();
    SendMessageA(g_todayMeals, LB_RESETCONTENT, 0, 0);
    SendMessageA(g_savedMeals, CB_RESETCONTENT, 0, 0);
    refreshMealDraft();
    if (active == nullptr) {
        setText(g_profileSummary, "Create or select a user profile to begin.");
        return;
    }
    const std::string selectedDate = dateText(g_dayPicker);
    const health::NutritionTargets target = g_companion->dailyNutritionTargets();
    const std::vector<health::MealEntry> meals =
        g_companion->mealsForDate(selectedDate);
    std::ostringstream summary;
    double dayCalories = 0.0;
    if (active->hasProfile) {
        summary << active->id << "  |  Daily target: " << number(target.calories, 0)
                << " kcal";
    } else {
        summary << "Complete the profile to see a daily calorie estimate.";
    }
    for (const health::MealEntry& meal : meals) {
        for (const health::MealFoodEntry& food : meal.foods) {
            dayCalories += food.calories;
        }
    }
    summary << "  |  " << selectedDate << ": " << number(dayCalories, 0) << " kcal";
    setText(g_profileSummary, summary.str());
    for (const health::SavedMeal& meal : active->savedMeals) {
        SendMessageA(g_savedMeals, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(meal.name.c_str()));
    }
    if (!active->savedMeals.empty()) {
        SendMessageA(g_savedMeals, CB_SETCURSEL, 0, 0);
    }
    double dayProtein = 0.0;
    double dayCarbohydrates = 0.0;
    double dayFat = 0.0;
    for (const health::MealEntry& meal : meals) {
        double calories = 0.0;
        double protein = 0.0;
        double carbohydrates = 0.0;
        double fat = 0.0;
        for (const health::MealFoodEntry& food : meal.foods) {
            calories += food.calories;
            protein += food.protein;
            carbohydrates += food.carbohydrates;
            fat += food.fat;
            dayProtein += food.protein;
            dayCarbohydrates += food.carbohydrates;
            dayFat += food.fat;

            std::ostringstream foodLine;
            foodLine << meal.name << " - " << food.name << " | "
                     << number(food.grams) << " g | " << number(food.calories)
                     << " kcal | P " << number(food.protein) << " g | C "
                     << number(food.carbohydrates) << " g | F "
                     << number(food.fat) << " g";
            const std::string foodValue = foodLine.str();
            SendMessageA(g_todayMeals, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(foodValue.c_str()));
        }
        std::ostringstream line;
        line << meal.name << " total | " << number(calories) << " kcal | P "
             << number(protein) << " g | C " << number(carbohydrates)
             << " g | F " << number(fat) << " g";
        const std::string value = line.str();
        SendMessageA(g_todayMeals, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(value.c_str()));
    }
    if (meals.empty()) {
        const std::string empty = "No food logged for " + selectedDate + ".";
        SendMessageA(g_todayMeals, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(empty.c_str()));
    }
    std::ostringstream daySummary;
    daySummary << "Day total: " << number(dayCalories) << " kcal | Protein "
               << number(dayProtein) << " g | Carbohydrates "
               << number(dayCarbohydrates) << " g | Fat "
               << number(dayFat) << " g";
    setText(g_dayStatus, daySummary.str() + (g_companion->isDayFinished(selectedDate)
        ? " | Finished (still editable)" : " | Not finished"));
}

// ---------------------------------------------------------------------------
// Helper: calculateAverage()
// Ask HealthCompanion for the selected date range's daily averages and render
// the result, including the number of calendar days in the calculation.
// ---------------------------------------------------------------------------
// Calculate average nutrition for a date range.
void calculateAverage() {
    const health::NutritionAverages average = g_companion->averageNutrition(
        dateText(g_rangeStart), dateText(g_rangeEnd));
    std::ostringstream result;
    result << "Daily average over " << average.days << " calendar day(s): "
           << number(average.calories, 0) << " kcal | Protein "
           << number(average.protein) << " g | Carbohydrates "
           << number(average.carbohydrates) << " g | Fat "
           << number(average.fat) << " g. Days without entries count as zero.";
    setText(g_averageSummary, result.str());
}

// ---------------------------------------------------------------------------
// Helper: setRangeToThisMonth()
// Set the date-range controls to the first day of this month through today,
// select today in the day picker, and calculate the month's average.
// ---------------------------------------------------------------------------
// Set the date range to the current month.
void setRangeToThisMonth() {
    SYSTEMTIME end{};
    GetLocalTime(&end);
    SYSTEMTIME start = end;
    start.wDay = 1;
    setDatePicker(g_rangeStart, start);
    setDatePicker(g_rangeEnd, end);
    setDatePicker(g_dayPicker, end);
    calculateAverage();
}

// ---------------------------------------------------------------------------
// Action: finishSelectedDay()
// Mark the selected date as finished and refresh its display. Finishing a day
// is informational; meals can still be added or edited afterward.
// ---------------------------------------------------------------------------
// Finish the selected day.
void finishSelectedDay() {
    const std::string date = dateText(g_dayPicker);
    g_companion->finishDay(date);
    refreshDashboard();
    setStatus("Finished " + date + ". You can still add meals or make changes.");
}

// ---------------------------------------------------------------------------
// Action: clearProfileFields()
// Clear the profile form and set the flag used by saveProfile() to create a
// new user instead of updating the currently selected profile.
// ---------------------------------------------------------------------------
// Clear the profile form fields.
void clearProfileFields() {
    g_creatingUser = true;
    for (HWND field : {g_name, g_weight, g_goalWeight, g_daysToTargetWeight,
                       g_height, g_age}) {
        setText(field, "");
    }
    setText(g_mealName, "");
    SendMessageA(g_sex, CB_SETCURSEL, 0, 0);
    SendMessageA(g_activity, CB_SETCURSEL, 0, 0);
    setText(g_profileSummary, "Enter profile details, then select Save profile.");
    setStatus("Enter profile details, including the number of days to reach the target weight.");
}

// ---------------------------------------------------------------------------
// Action: saveProfile()
// Read the profile fields, map the selected activity level to its multiplier,
// and create a profile or update the active one through HealthCompanion.
// ---------------------------------------------------------------------------
// Save the current profile.
void saveProfile() {
    const std::string name = text(g_name);
    const double weight = std::stod(text(g_weight));
    const double goalWeight = std::stod(text(g_goalWeight));
    const int daysToTargetWeight = std::stoi(text(g_daysToTargetWeight));
    const double height = std::stod(text(g_height));
    const int age = std::stoi(text(g_age));
    const char sex = SendMessageA(g_sex, CB_GETCURSEL, 0, 0) == 1 ? 'M' : 'F';
    const int activityIndex = static_cast<int>(
        SendMessageA(g_activity, CB_GETCURSEL, 0, 0));
    static const double factors[] = {1.2, 1.375, 1.55, 1.725};
    if (activityIndex < 0 || activityIndex >= 4) {
        throw std::invalid_argument("Choose an activity level.");
    }

    const health::HealthUser* active = g_companion->activeUser();
    const std::string id = active == nullptr || g_creatingUser
        ? g_companion->createProfile(name, weight, height, age, sex,
                                     factors[activityIndex], goalWeight,
                                     daysToTargetWeight)
        : g_companion->updateProfile(active->id, name, weight, height, age, sex,
                                     factors[activityIndex], goalWeight,
                                     daysToTargetWeight);
    g_creatingUser = false;
    refreshUsers();
    setStatus("Saved profile " + id + " and calculated your daily target.");
}

// ---------------------------------------------------------------------------
// Action: beginFoodSearch()
// Start the Open Food Facts lookup on a worker thread. The worker posts a
// completion message to the window so the UI thread can safely display results.
// ---------------------------------------------------------------------------
// Start a food search request.
void beginFoodSearch() {
    const std::string query = text(g_foodQuery);
    if (query.empty()) throw std::invalid_argument("Enter a food to search.");
    if (g_searchThread.joinable()) g_searchThread.join();
    EnableWindow(g_searchButton, FALSE);
    setStatus("Searching Open Food Facts...");
    const HWND target = g_window;
    g_searchThread = std::thread([query, target] {
        auto* result = new SearchPayload;
        try {
            health::FoodDatabase database;
            result->products = database.search(query);
        } catch (const std::exception& error) {
            result->error = error.what();
        }
        if (!PostMessageA(target, kFoodSearchComplete, 0,
                          // Handle reinterpret cast< l p a r a m>.
                          reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    });
}

// ---------------------------------------------------------------------------
// Action: finishFoodSearch()
// Take ownership of the worker result, re-enable the search button, and show
// either the products or the error returned by the background request.
// ---------------------------------------------------------------------------
// Handle the result of a food search.
void finishFoodSearch(SearchPayload* result) {
    std::unique_ptr<SearchPayload> payload(result);
    if (g_searchThread.joinable()) g_searchThread.join();
    EnableWindow(g_searchButton, TRUE);
    SendMessageA(g_foodResults, LB_RESETCONTENT, 0, 0);
    g_products = std::move(payload->products);
    if (!payload->error.empty()) {
        showError(payload->error);
        return;
    }
    for (const health::FoodProduct& food : g_products) {
        std::ostringstream line;
        line << food.name << " | " << number(food.caloriesPer100g, 0)
             << " kcal | P " << number(food.proteinPer100g)
             << " g | C " << number(food.carbohydratesPer100g)
             << " g | F " << number(food.fatPer100g) << " g per 100 g";
        const std::string value = line.str();
        SendMessageA(g_foodResults, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(value.c_str()));
    }
    setStatus(g_products.empty() ? "No foods with calorie information were found."
                                 : "Select a food result and enter the serving size.");
}

// ---------------------------------------------------------------------------
// Action: addSelectedFood()
// Validate the selected result and serving size, scale each per-100-gram
// nutrient value to that serving, and append the food to the meal draft.
// ---------------------------------------------------------------------------
// Add the selected food to the current meal.
void addSelectedFood() {
    const LRESULT selected = SendMessageA(g_foodResults, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || static_cast<std::size_t>(selected) >= g_products.size()) {
        throw std::invalid_argument("Search for food, then select a result to add.");
    }
    const double grams = std::stod(text(g_servingSize));
    if (!std::isfinite(grams) || grams <= 0.0 || grams > 10000.0) {
        throw std::invalid_argument("Enter a serving from 0 to 10,000 g.");
    }
    const health::FoodProduct& food = g_products[static_cast<std::size_t>(selected)];
    const double multiplier = grams / 100.0;
    g_mealFoods.push_back({food.name, grams,
                           food.caloriesPer100g * multiplier,
                           food.proteinPer100g * multiplier,
                           food.carbohydratesPer100g * multiplier,
                           food.fatPer100g * multiplier});
    refreshMealDraft();
    setStatus("Added " + food.name + " to the meal. Add more foods or log it.");
}

// ---------------------------------------------------------------------------
// Action: logDraftMeal()
// Save the current food draft as a meal on the selected date, then clear the
// draft fields and refresh the dashboard from the stored data.
// ---------------------------------------------------------------------------
// Log the current meal draft.
void logDraftMeal() {
    g_companion->logMealOnDate(text(g_mealName), g_mealFoods,
                               dateText(g_dayPicker));
    g_mealFoods.clear();
    setText(g_mealName, "");
    refreshDashboard();
    setStatus("Meal logged for " + dateText(g_dayPicker) + ".");
}

// ---------------------------------------------------------------------------
// Action: saveMealTemplate()
// Store the current meal draft for reuse. Saving a template does not add it
// to the selected day's food log.
// ---------------------------------------------------------------------------
// Save the current meal draft as a template.
void saveMealTemplate() {
    g_companion->saveMealTemplate(text(g_mealName), g_mealFoods);
    refreshDashboard();
    setStatus("Meal saved for future use. Log it separately to add it to today's meals.");
}

// ---------------------------------------------------------------------------
// Action: logSavedMeal()
// Validate the selected saved-meal entry and add it to the currently selected
// date's log.
// ---------------------------------------------------------------------------
// Log a saved meal from the template list.
void logSavedMeal() {
    const LRESULT selected = SendMessageA(g_savedMeals, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        throw std::invalid_argument("Save a meal for later, then select it here.");
    }
    g_companion->logSavedMeal(static_cast<std::size_t>(selected),
                              dateText(g_dayPicker));
    refreshDashboard();
    setStatus("Saved meal logged for " + dateText(g_dayPicker) + ".");
}

// ---------------------------------------------------------------------------
// Action: saveReminders()
// Read reminder settings from the controls, persist them for the active user,
// and reset the next water-reminder time.
// ---------------------------------------------------------------------------
// Save reminder settings.
void saveReminders() {
    const std::string waterText = text(g_waterInterval);
    const int water = waterText.empty() ? 0 : std::stoi(waterText);
    const std::string medicine = text(g_medicineName);
    const std::string medicineTime = text(g_medicineTime);
    const std::string sleepTime = text(g_sleepTime);
    g_companion->setReminders(water, medicine, medicineTime, sleepTime);
    g_nextWaterReminder = std::chrono::steady_clock::now() +
        std::chrono::minutes(std::max(water, 1));
    setStatus("Reminders saved. Leave the app running for notifications.");
}

// ---------------------------------------------------------------------------
// Helper: checkReminders()
// Check the water interval and the active user's medicine and sleep times.
// Show a single notification containing all reminders due on this timer tick.
// ---------------------------------------------------------------------------
// Check whether reminder alerts should fire.
void checkReminders() {
    const health::HealthUser* user = g_companion->activeUser();
    if (user == nullptr) return;
    std::ostringstream notices;
    if (user->waterIntervalMinutes > 0 &&
        // Handle now.
        std::chrono::steady_clock::now() >= g_nextWaterReminder) {
        g_nextWaterReminder = std::chrono::steady_clock::now() +
            std::chrono::minutes(user->waterIntervalMinutes);
        notices << "Time to drink some water.\n";
    }
    std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);
    std::ostringstream current;
    current << std::put_time(&localTime, "%Y-%m-%d %H:%M");
    const std::string minute = current.str();
    if (!user->medicineName.empty() && localTime.tm_sec < 30 &&
        minute.substr(11) == user->medicineTime &&
        g_lastMedicineReminder != minute) {
        g_lastMedicineReminder = minute;
        notices << "Medicine: " << user->medicineName << ".\n";
    }
    if (!user->sleepTime.empty() && localTime.tm_sec < 30 &&
        minute.substr(11) == user->sleepTime &&
        g_lastSleepReminder != minute) {
        g_lastSleepReminder = minute;
        notices << "Your scheduled sleep time is coming up.\n";
    }
    const std::string message = notices.str();
    if (!message.empty()) {
        setStatus("Reminder: " + message.substr(0, message.find('\n')));
        MessageBoxA(g_window, message.c_str(), "Health Companion reminder",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    }
}

// ---------------------------------------------------------------------------
// Callback: windowProcedure()
// Handle window lifecycle, layout and scroll events, control notifications,
// asynchronous food-search completion, and the reminder timer.
// ---------------------------------------------------------------------------
LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                 WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_window = window;
        createControls();
        SetTimer(window, kReminderTimer, 15000, nullptr);
        refreshUsers();
        return 0;
    case WM_SIZE:
        layoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &scroll);
        const int previous = g_scrollOffset;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: g_scrollOffset -= 40; break;
        case SB_LINEDOWN: g_scrollOffset += 40; break;
        case SB_PAGEUP: g_scrollOffset -= static_cast<int>(scroll.nPage); break;
        case SB_PAGEDOWN: g_scrollOffset += static_cast<int>(scroll.nPage); break;
        case SB_THUMBTRACK: g_scrollOffset = scroll.nTrackPos; break;
        case SB_THUMBPOSITION: g_scrollOffset = scroll.nTrackPos; break;
        case SB_TOP: g_scrollOffset = 0; break;
        case SB_BOTTOM: g_scrollOffset = kContentHeight; break;
        default: break;
        }
        g_scrollOffset = std::clamp(g_scrollOffset, 0,
            std::max(kContentHeight - static_cast<int>(scroll.nPage), 0));
        if (previous != g_scrollOffset) {
            RECT client{};
            GetClientRect(window, &client);
            layoutControls(client.right, client.bottom);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int steps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        if (steps != 0) scrollPage(-steps * 90);
        return 0;
    }
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
        if (notification->code == DTN_DATETIMECHANGE &&
            notification->hwndFrom == g_dayPicker) {
            refreshDashboard();
        }
        return 0;
    }
    case WM_COMMAND:
        try {
            switch (LOWORD(wParam)) {
            case NewUser:
                SendMessageA(g_userSelect, CB_SETCURSEL, -1, 0);
                g_mealFoods.clear();
                refreshMealDraft();
                clearProfileFields();
                break;
            case UserSelect:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    const LRESULT index = SendMessageA(g_userSelect, CB_GETCURSEL, 0, 0);
                    if (index != CB_ERR) {
                        const auto* id = reinterpret_cast<std::string*>(
                            SendMessageA(g_userSelect, CB_GETITEMDATA, index, 0));
                        if (id != nullptr && g_companion->selectUser(*id)) {
                            g_creatingUser = false;
                            g_mealFoods.clear();
                            setText(g_mealName, "");
                            refreshUsers();
                        }
                    }
                }
                break;
            case SaveProfile:
                if (HIWORD(wParam) == BN_CLICKED) saveProfile();
                break;
            case SearchFood:
                if (HIWORD(wParam) == BN_CLICKED) beginFoodSearch();
                break;
            case LogMeal:
                if (HIWORD(wParam) == BN_CLICKED) logDraftMeal();
                break;
            case AddFood:
                if (HIWORD(wParam) == BN_CLICKED) addSelectedFood();
                break;
            case SaveMealTemplate:
                if (HIWORD(wParam) == BN_CLICKED) saveMealTemplate();
                break;
            case LogSavedMeal:
                if (HIWORD(wParam) == BN_CLICKED) logSavedMeal();
                break;
            case ViewDay:
                if (HIWORD(wParam) == BN_CLICKED) refreshDashboard();
                break;
            case FinishDay:
                if (HIWORD(wParam) == BN_CLICKED) finishSelectedDay();
                break;
            case ThisMonth:
                if (HIWORD(wParam) == BN_CLICKED) setRangeToThisMonth();
                break;
            case CalculateAverage:
                if (HIWORD(wParam) == BN_CLICKED) calculateAverage();
                break;
            case SaveReminders:
                if (HIWORD(wParam) == BN_CLICKED) saveReminders();
                break;
            }
        } catch (const std::exception& error) {
            showError(error.what());
        }
        return 0;
    case kFoodSearchComplete:
        finishFoodSearch(reinterpret_cast<SearchPayload*>(lParam));
        return 0;
    case WM_TIMER:
        checkReminders();
        return 0;
    case WM_CLOSE:
        if (g_searchThread.joinable()) g_searchThread.join();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kReminderTimer);
        for (LRESULT i = 0; i < SendMessageA(g_userSelect, CB_GETCOUNT, 0, 0); ++i) {
            delete reinterpret_cast<std::string*>(
                SendMessageA(g_userSelect, CB_GETITEMDATA, i, 0));
        }
        PostQuitMessage(0);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 850;
        limits->ptMinTrackSize.y = 650;
        return 0;
    }
    default:
        return DefWindowProcA(window, message, wParam, lParam);
    }
}

}  // namespace

// Start the Windows app.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    // -----------------------------------------------------------------------
    // Initialize the common controls and health data, register the window
    // class, create the main window, and dispatch messages until it closes.
    // -----------------------------------------------------------------------
    g_instance = instance;
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_DATE_CLASSES;
    if (!InitCommonControlsEx(&commonControls)) {
        MessageBoxA(nullptr, "Could not initialize the calendar controls.",
                    "Health Companion", MB_OK | MB_ICONERROR);
        return 1;
    }
    try {
        g_companion = std::make_unique<health::HealthCompanion>();
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Health Companion",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = "DigitalHealthCompanionWindow";
    if (!RegisterClassA(&windowClass)) {
        MessageBoxA(nullptr, "Could not register the application window.",
                    "Health Companion", MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND window = CreateWindowExA(
        0, windowClass.lpszClassName, "Digital Health Companion",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 900,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        MessageBoxA(nullptr, "Could not create the application window.",
                    "Health Companion", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return static_cast<int>(message.wParam);
}
