/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

int main(int argc, char const* argv[])
{
    string_view constexpr alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ "
        "abcdefghijklmnopqrstuvwxyz "
        "0123456789 []{}();+-*/=";

    vector<char> chunk;
    chunk.reserve(1 * 1024 * 1024);
    for (size_t i = 0; i < chunk.capacity(); ++i)
        chunk.push_back(alphabet[i % alphabet.size()]);

    size_t const repeat = 2;
    auto const start = chrono::steady_clock::now();
    for (size_t i = 0; i < repeat; ++i)
        cout.write(chunk.data(), chunk.size());
    auto const end = chrono::steady_clock::now();
    auto const ms = chrono::duration_cast<chrono::milliseconds>(end - start).count();

    cout << "\nDuration: " << (ms / 1000) << "." << (ms % 1000) << " secs" << endl;

    return EXIT_SUCCESS;
}
