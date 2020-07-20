/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <terminal/Screen.h>
#include <terminal/Selector.h>
#include <catch2/catch.hpp>

using namespace std;
using namespace std::placeholders;
using namespace terminal;

TEST_CASE("Selector.Linear")
{
    auto screenEvents = ScreenEvents{};
    auto screen = Screen{{3, 11}, screenEvents, [&](auto const& msg) { INFO(fmt::format("{}", msg)); }};
    screen.write(
		"12345,67890"s +
		"ab,cdefg,hi"s +
		"12345,67890"s
	);

	SECTION("forward single-line") {
		auto selector = Selector{Selector::Mode::Linear, U",", screen.currentBuffer(), Coordinate{2, 2}};
		selector.extend(Coordinate{2, 4});
		// selected area "b,cdefg,hi\n1234"
		// TODO
	}
}

TEST_CASE("Selector.LinearWordWise")
{
	// TODO
}

TEST_CASE("Selector.FullLine")
{
	// TODO
}

TEST_CASE("Selector.Rectangular")
{
	// TODO
}
