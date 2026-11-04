#include "health_companion.h"

#include <exception>
#include <iostream>

// Start the health tracker and keep the app running until the user exits.
int main() {
    try {
        // Load saved data and start the main menu flow.
        health::HealthCompanion app;

        // Run the app until the user chooses to quit.
        app.run();
    } catch (const std::exception& error) {
        // Show the startup problem and exit with a failure code.
        std::cerr << "The app could not start: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
