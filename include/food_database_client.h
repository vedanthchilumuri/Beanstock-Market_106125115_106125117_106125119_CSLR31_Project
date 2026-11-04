#ifndef FOOD_DATABASE_CLIENT_H
#define FOOD_DATABASE_CLIENT_H

#include <string>

namespace health {

// Fetch the raw food search results from Open Food Facts.
// Throws if the request fails or the server sends back an error.
std::string downloadOpenFoodFactsSearchResults(const std::string& query);

}  // namespace health

#endif
