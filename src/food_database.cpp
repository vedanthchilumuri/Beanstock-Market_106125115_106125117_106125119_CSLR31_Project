// Food database helper.
// This file reads Open Food Facts search results and turns them into food items
// the app can use for meal tracking.
#include "food_database.h"
#include "food_database_client.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace health {
namespace {

// Skip over spaces and line breaks when reading a JSON value.
std::size_t skipWhitespace(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return pos;
}

// Find where a quoted JSON string ends so we can read it safely.
std::size_t findStringEnd(const std::string& text, std::size_t start) {
    for (std::size_t i = start + 1; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
        } else if (text[i] == '"') {
            return i;
        }
    }
    return std::string::npos;
}

// Turn JSON escape codes like \n and \" into regular text.
std::string decodeJsonString(const std::string& text, std::size_t start, std::size_t end) {
    std::string result;
    for (std::size_t i = start + 1; i < end; ++i) {
        char ch = text[i];
        if (ch != '\\') {
            result.push_back(ch);
            continue;
        }
        if (++i >= end) {
            break;
        }
        switch (text[i]) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u':
            // Skip the four hex digits in a Unicode escape.
            i = std::min(i + 4, end - 1);
            break;
        default: result.push_back(text[i]); break;
        }
    }
    return result;
}

// Pull the value for one JSON field and move past any spaces before it.
std::size_t findFieldValue(const std::string& object, const std::string& field) {
    const std::string needle = "\"" + field + "\"";
    const std::size_t key = object.find(needle);
    if (key == std::string::npos) {
        return key;
    }
    const std::size_t colon = object.find(':', key + needle.size());
    if (colon == std::string::npos) {
        return colon;
    }
    return skipWhitespace(object, colon + 1);
}

// Read a string value from a JSON object, even if it is nested in a small array.
std::string stringField(const std::string& object, const std::string& field) {
    const std::size_t value = findFieldValue(object, field);
    if (value == std::string::npos) {
        return {};
    }
    const std::size_t stringStart = object[value] == '"'
        ? value
        : object[value] == '[' ? object.find('"', value + 1) : std::string::npos;
    if (stringStart == std::string::npos) {
        return {};
    }
    const std::size_t end = findStringEnd(object, stringStart);
    return end == std::string::npos
        ? std::string{}
        : decodeJsonString(object, stringStart, end);
}

// Read a number from JSON and fall back to zero if the value is missing or invalid.
double numberField(const std::string& object, const std::string& field) {
    const std::size_t value = findFieldValue(object, field);
    if (value == std::string::npos ||
        !(std::isdigit(static_cast<unsigned char>(object[value])) ||
          object[value] == '-' || object[value] == '+')) {
        return 0.0;
    }
    try {
        const double number = std::stod(object.substr(value));
        return std::isfinite(number) && number >= 0.0 ? number : 0.0;
    } catch (const std::exception&) {
        return 0.0;
    }
}

// Split a JSON array into separate product objects so each one can be read.
std::vector<std::string> objectsInArray(const std::string& response, const std::string& field) {
    const std::string keyName = "\"" + field + "\"";
    const std::size_t arrayKey = response.find(keyName);
    const std::size_t arrayStart = arrayKey == std::string::npos
        ? std::string::npos
        : response.find('[', arrayKey + keyName.size());
    if (arrayStart == std::string::npos) {
        throw std::runtime_error("Food database response did not contain " + field + ".");
    }

    std::vector<std::string> objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    std::size_t objectStart = std::string::npos;
    for (std::size_t i = arrayStart + 1; i < response.size(); ++i) {
        const char ch = response[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            if (depth++ == 0) {
                objectStart = i;
            }
        } else if (ch == '}' && depth > 0 && --depth == 0) {
            objects.push_back(response.substr(objectStart, i - objectStart + 1));
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

}  // namespace

// Search for matching food items in the saved Open Food Facts results.
std::vector<FoodProduct> FoodDatabase::search(const std::string& query) const {
    if (query.empty()) {
        throw std::invalid_argument("Enter a food name to search.");
    }

    // -----------------------------------------------------------------------
    // Request the search hits, then build records from products with a name
    // and usable calorie data. Nutrients are retained on a per-100-gram basis.
    // -----------------------------------------------------------------------
    const std::string response = downloadOpenFoodFactsSearchResults(query);
    const std::vector<std::string> objects = objectsInArray(response, "hits");
    std::vector<FoodProduct> products;
    for (const std::string& object : objects) {
        FoodProduct product;
        product.name = stringField(object, "product_name");
        const std::string brand = stringField(object, "brands");
        if (!brand.empty() && product.name.find(brand) == std::string::npos) {
            product.name += " (" + brand + ")";
        }
        const std::size_t nutrientsStart = findFieldValue(object, "nutriments");
        if (product.name.empty() || nutrientsStart == std::string::npos ||
            object[nutrientsStart] != '{') {
            continue;
        }

        const std::size_t nutrientsEnd = object.find('}', nutrientsStart + 1);
        if (nutrientsEnd == std::string::npos) {
            continue;
        }
        const std::string nutrients =
            object.substr(nutrientsStart, nutrientsEnd - nutrientsStart + 1);
        // Prefer the API's kcal value. If only kJ is present, convert it to
        // kcal before storing the product's per-100-gram energy.
        product.caloriesPer100g = numberField(nutrients, "energy-kcal_100g");
        if (product.caloriesPer100g == 0.0) {
            product.caloriesPer100g = numberField(nutrients, "energy_100g") / 4.184;
        }
        product.proteinPer100g = numberField(nutrients, "proteins_100g");
        product.carbohydratesPer100g = numberField(nutrients, "carbohydrates_100g");
        product.fatPer100g = numberField(nutrients, "fat_100g");
        if (product.caloriesPer100g > 0.0 &&
            // Handle isfinite.
            std::isfinite(product.caloriesPer100g)) {
            products.push_back(product);
        }
    }
    return products;
}

}  // namespace health
