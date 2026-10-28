/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/helpers/shader_ids.hpp"

#include <fstream>
#include <iostream>

using namespace ls::shader_ids;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: lsfg-vk-shader-ids <output.json>\n";
        return 2;
    }

    std::ofstream out(argv[1]);
    out << "[";
    bool first = true;
    for (const bool fp16 : { false, true }) {
        for (const auto& request : REQUESTS) {
            out << (first ? "\n" : ",\n")
                << "  {\"id\": " << request.id
                << ", \"perf\": " << (request.perf ? "true" : "false")
                << ", \"fp16\": " << (fp16 ? "true" : "false")
                << ", \"resource\": " << resourceId(request.id, fp16, request.perf) << "}";
            first = false;
        }
    }
    out << "\n]\n";

    out.close();
    if (!out) {
        std::cerr << "unable to write " << argv[1] << '\n';
        return 1;
    }
    return 0;
}
