#include <iostream>
#include <vector>
#include <map>

#include "kvalue.hpp"
#include "cgroup.hpp"

extern void dump_reg(void);
int main2() {
    while (true) {
        TCgroupSnapshot cgs;
        cout << cgs << endl;
    }
 
    return EXIT_SUCCESS;
}

int main3() {
    try {
        TCgroupSnapshot cgs;
        cout << cgs << endl;
    } catch (const char *e) {
        cerr << e << endl;
        return EXIT_FAILURE;
    } catch (string e) {
        cerr << e << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int main() {
    try {
        map<string, map<string, string> >data =
            {{"basesearch_1",
              {{"memory_guarantee", "10G"},
               {"policy", "runtime"}}},
             {"basesearch_2",
              {{"memory_guarantee", "20G"},
               {"policy", "runtime"}}},
             {"sky",
              {{"memory_limit", "2G"},
               {"policy", "system"}}},
             {"nirvana_1",
              {{"memory_guarantee", "1G"},
               {"memory_guarantee", "500M"},
               {"policy", "batch"}}},
             {"nirvana_2",
              {{"memory_guarantee", "1G"},
               {"memory_guarantee", "500M"},
               {"policy", "batch"}}}};

        TKeyValueStorage st;
        st.MountTmpfs();

        for (auto node : data) {
            kv::TNode wr;

            for (auto key : node.second) {
                auto pair = wr.add_pairs();
                pair->set_key(key.first);
                pair->set_val(key.second);
            }

            TError error = st.SaveNode(node.first, wr);
            if (error)
                throw error.GetMsg();

            kv::TNode rd;
            error = st.LoadNode(node.first, rd);
            if (error)
                throw error;

            int i = 0;
            for (auto key : node.second) {
                auto pair = rd.pairs(i);

                if (pair.key() != key.first ||
                    pair.val() != key.second)
                    throw string("Invalid serialized data!");

                i++;
            }

            st.RemoveNode(node.first);
        }
    } catch (string e) {
        cerr << "Error: " << e << endl;
        return EXIT_FAILURE;
    } catch (...) {
        cerr << "Error!" << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
