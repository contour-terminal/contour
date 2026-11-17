// SPDX-License-Identifier: Apache-2.0
//
// Prints the orchestrator's manifest, as its declared tables make it (manifestText()), to standard output and
// nothing else. The build embeds this output in the WebAssembly module's manifest section
// (cmake/EmbedManifest.cmake), and a test compares it with contract/manifest-v2/valid/orchestrator.manifest.
// A host tool, outside guest/: it writes to a stream.
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
#endif

#include <orchestrator/core/Commands.hpp>

int main()
{
#ifdef _WIN32
    // Byte for byte: no line end becomes a CRLF on the way out.
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        return EXIT_FAILURE;
#endif
    auto const text = orchestrator::manifestText();
    if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size() || std::fflush(stdout) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
