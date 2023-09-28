// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>

#include <fmt/format.h>

#include <string_view>

using namespace vtbackend;
using std::pair;
using std::to_string;
using Category = vtbackend::FunctionCategory;
using namespace std::string_view_literals;

int main()
{
    for (auto const& [category, headline]: { pair { Category::C0, "Control Codes"sv },
                                             pair { Category::ESC, "Escape Sequences"sv },
                                             pair { Category::CSI, "Control Sequences"sv },
                                             pair { Category::OSC, "Operating System Commands"sv },
                                             pair { Category::DCS, "Device Control Sequences"sv } })
    {
        fmt::print("## {} - {}\n\n", category, headline);
        for (auto const& fn: vtbackend::allFunctions())
        {
            std::string markdown;

            markdown += fmt::format("## {} - {}\n\n", fn.documentation.mnemonic, fn.documentation.comment);

            markdown += fmt::format("### Conformance Level\n\n");
            if (fn.extension != VTExtension::None)
                markdown += fmt::format("{} extension\n\n", fn.extension);
            else
                markdown += fmt::format("{}\n\n", fn.conformanceLevel);

            markdown += fmt::format("### Syntax\n\n```\n");
            if (!fn.documentation.parameters.empty())
                markdown += fmt::format("{}{} {}{} {}\n",
                                        fn.category,
                                        fn.leader ? fmt::format(" {}", fn.leader) : "",
                                        fn.documentation.parameters,
                                        fn.intermediate ? fmt::format(" {}", fn.intermediate) : "",
                                        fn.finalSymbol);
            else
                markdown += fmt::format("{}\n", fn);
            markdown += fmt::format("```\n\n");

            if (!fn.documentation.description.empty())
                markdown += fmt::format("### Description\n\n{}\n\n", fn.documentation.description);

            if (!fn.documentation.notes.empty())
                markdown += fmt::format("### Notes\n\n{}\n\n", fn.documentation.notes);

            if (!fn.documentation.examples.empty())
                markdown += fmt::format("### Examples\n\n```\n{}\n```\n\n", fn.documentation.examples);

            fmt::print("{}", markdown);
        }
    }
    return EXIT_SUCCESS;
}
