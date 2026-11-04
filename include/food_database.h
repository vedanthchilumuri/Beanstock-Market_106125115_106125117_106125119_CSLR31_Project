#ifndef FOOD_DATABASE_H
#define FOOD_DATABASE_H

#include <string>
#include <vector>

namespace health {

// One food item with its nutrition values per 100 grams.
struct FoodProduct {
    std::string name;
    double caloriesPer100g = 0.0;
    double proteinPer100g = 0.0;
    double carbohydratesPer100g = 0.0;
    double fatPer100g = 0.0;
};

class FoodDatabase {
public:
    // Look up foods that match a search term and return useful nutrition data.
    std::vector<FoodProduct> search(const std::string& query) const;
};

}  // namespace health

#endif
