// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>

#include <format>
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
        std::cout << std::format("## {} - {}\n\n", category, headline);
        for (auto const& fn: vtbackend::allFunctions())
        {
            std::string markdown;

            markdown += std::format("## {} - {}\n\n", fn.documentation.mnemonic, fn.documentation.comment);

            markdown += std::format("### Conformance Level\n\n");
            if (fn.extension != VTExtension::None)
                markdown += std::format("{} extension\n\n", fn.extension);
            else
                markdown += std::format("{}\n\n", fn.conformanceLevel);

            markdown += std::format("### Syntax\n\n```\n");
            if (!fn.documentation.parameters.empty())
                markdown += std::format("{}{} {}{} {}\n",
                                        fn.category,
                                        fn.leader ? std::format(" {}", fn.leader) : "",
                                        fn.documentation.parameters,
                                        fn.intermediate ? std::format(" {}", fn.intermediate) : "",
                                        fn.finalSymbol);
            else
                markdown += std::format("{}\n", fn);
            markdown += std::format("```\n\n");

            if (!fn.documentation.description.empty())
                markdown += std::format("### Description\n\n{}\n\n", fn.documentation.description);

            if (!fn.documentation.notes.empty())
                markdown += std::format("### Notes\n\n{}\n\n", fn.documentation.notes);

            if (!fn.documentation.examples.empty())
                markdown += std::format("### Examples\n\n```\n{}\n```\n\n", fn.documentation.examples);

            std::cout << std::format("{}", markdown);
        }
    }
    return EXIT_SUCCESS;
}
