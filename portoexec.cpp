#include <iostream>
#include <map>
#include <string>

using namespace std;

static map<string, string> properties;

static int Run(string container_name,  map<string, string> &properties)
{
    cout << container_name << endl;
    for (auto iter: properties)
    {
      cout << iter.first << "=" << iter.second << endl;
    }
    return 0;
}

static int Destroy(string container_name)
{
    cout << container_name << endl;
    return 0;
}

static int Help()
{
    cout << "portoexec - run command throw portod" << endl;
    cout << endl;
    cout << "SYNOPSYS" << endl;
    cout << "\tportoexec run <container_name> <properties>" << endl;
    cout << "\tportoexec destroy <container_name>" << endl;
    return 1;
}

static void Parcer (string property, string &property_key, string &property_value)
{
    string::size_type n;
    n = property.find('=');
    if (n == string::npos)
        throw property;
    property_key = property.substr(0, n);
    property_value = property.substr(n+1, property.size());
    if (property_key == "" || property_value == "")
        throw property;
}

int main(int argc, char * const argv[])
{
    int result;
    string property_key, property_value;

    if (argc <= 1) {
            result = Help();
            return result;
    }

    string method = argv[1];

    if (argc >= 4 && method == "run") {
        for (int i = 3; i<argc; i++) {
            try {
                Parcer(argv[i], property_key, property_value);
                properties[property_key] = property_value;
            } catch (string s) {
                cerr << "Wrong argument: " << s << '\n';
                return EXIT_FAILURE;
            }
        }
        result = Run(argv[2], properties);
    } else if (argc > 2 && string(argv[1]) == "destroy") {
        result = Destroy(argv[2]);
    } else {
        result = Help();
    }

    return result;
}
