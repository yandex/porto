#include "property.hpp"

#include <map>

using namespace std;

string TContainerSpec::Get(string property) {
    return data[property].value;
}

void TContainerSpec::Set(string property, string value) {
    if (data[property].checker(value))
        data[property].value = value;
}
