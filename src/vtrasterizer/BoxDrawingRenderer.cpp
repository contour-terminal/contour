// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/BoxDrawingRenderer.h>
#include <vtrasterizer/Pixmap.h>
#include <vtrasterizer/utils.h>

#include <crispy/logstore.h>

#include <fmt/format.h>

#include <range/v3/view/filter.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/zip.hpp>

#include <array>

using namespace std::string_view_literals;

using std::clamp;
using std::get;
using std::max;
using std::min;
using std::nullopt;
using std::optional;
using std::pair;
using std::sort;
using std::string_view;
using std::tuple;

using crispy::point;
using ranges::views::filter;
using ranges::views::iota;
using ranges::views::zip;

namespace vtrasterizer
{

namespace
{
    auto const inline boxDrawingLog = logstore::category("renderer.boxdrawing",
                                                         "Logs box drawing debugging.",
                                                         logstore::category::state::Disabled,
                                                         logstore::category::visibility::Hidden);

    // TODO: Do not depend on this function but rather construct the pixmaps using the correct Y-coordinates.
    atlas::Buffer invertY(atlas::Buffer const& image, ImageSize cellSize)
    {
        atlas::Buffer dest;
        dest.resize(cellSize.area());
        auto const pitch = cellSize.width.as<size_t>();
        auto const height = cellSize.height.as<size_t>();

        for (size_t i = 0; i < cellSize.height.as<size_t>(); ++i)
        {
            for (size_t j = 0; j < cellSize.width.as<size_t>(); ++j)
            {
                dest[i * pitch + j] = image[(height - i - 1u) * pitch + j];
            }
        }
        return dest;
    }
} // namespace

namespace detail
{

    namespace
    {

        enum class Thickness
        {
            Light,
            Heavy
        };

        enum Line : uint8_t
        {
            NoLine,
            Light,  // solid light line
            Light2, // 2-dashed line
            Light3, // 3-dashed line
            Light4, // 4-dashed line
            Double, // solid light double line
            Heavy,  // solid heavy line
            Heavy2, // 2-dashed heavy line
            Heavy3, // 3-dashed heavy line
            Heavy4, // 4-dashed heavy line
        };

        [[maybe_unused]] string_view to_stringview(Line lm)
        {
            switch (lm)
            {
                case NoLine: return "NoLine"sv;
                case Light: return "Light"sv;
                case Light2: return "Light2"sv;
                case Light3: return "Light3"sv;
                case Light4: return "Light4"sv;
                case Double: return "Double"sv;
                case Heavy: return "Heavy"sv;
                case Heavy2: return "Heavy2"sv;
                case Heavy3: return "Heavy3"sv;
                case Heavy4: return "Heavy4"sv;
            }
            return "?"sv;
        }

        enum Diagonal : uint8_t
        {
            NoDiagonal = 0x00,
            Forward = 0x01,
            Backward = 0x02,
            Crossing = 0x03
        };

        void drawArc(atlas::Buffer& buffer, ImageSize imageSize, unsigned thickness, Arc arc)
        {
            // Used to record all the pixel coordinates that have been written to per scanline.
            //
            // The vector index represents the y-axis.
            //
            // The element-array for each y-coordinate represent the x-coordinates that
            // have been written to at that line.
            //
            // This is needed in order to fill the gaps.
            using GapFills = std::vector<std::vector<unsigned>>;
            GapFills gaps;
            gaps.resize(unbox<size_t>(imageSize.height));

            auto const w = unbox(imageSize.width);
            auto const h = unbox(imageSize.height);

            // fmt::print("{}.drawArc: size={}\n", arc, imageSize);
            auto const putpixel = [&](int x, int y, uint8_t alpha = 0xFFu) {
                auto const fy = clamp((unsigned) y, 0u, h - 1);
                auto const fx = clamp((unsigned) x, 0u, w - 1);
                buffer[fy * w + fx] = alpha;
                gaps[fy].push_back(fx);
            };

            // inner circle
            drawEllipseArc(putpixel,
                           imageSize,
                           crispy::point { // radius
                                           unbox<int>(imageSize.width) / 2 - int(thickness) / 2,
                                           unbox<int>(imageSize.height) / 2 - int(thickness) / 2 },
                           arc);

            // outer circle
            drawEllipseArc(putpixel,
                           imageSize,
                           crispy::point { // radius
                                           unbox<int>(imageSize.width) / 2 + int(thickness) / 2 - 1,
                                           unbox<int>(imageSize.height) / 2 + int(thickness) / 2 - 1 },
                           arc);

            // Close arc at open ends to filling works.
            // bool const isLeft = arc == Arc::TopLeft || arc == Arc::BottomLeft;
            // auto const xCorner = isLeft ? unbox<unsigned>(imageSize.width) : 0u;
            // for (auto const i: iota(0, thickness))
            //     gaps.at(unbox<size_t>(imageSize.height) / 2 - thickness/2 + i).push_back(xCorner);

            // fill gap
            for (size_t y = 0; y < gaps.size(); ++y)
            {
                if (auto& gap = gaps[y]; !gap.empty())
                {
                    sort(begin(gap), end(gap));
                    for (auto const xi: iota(gap.front(), gap.back()))
                        buffer.at(y * unbox<size_t>(imageSize.width) + xi) = 0xFF;
                }
            }
        }

        struct ProgressBar
        {
            enum class Part
            {
                Left,
                Middle,
                Right
            };

            ImageSize size {};
            int underlinePosition = 1;

            Part part = Part::Middle;
            bool filledval = false;

            // clang-format off
            constexpr ProgressBar& left() noexcept { part = Part::Left; return *this; }
            constexpr ProgressBar& middle() noexcept { part = Part::Middle; return *this; }
            constexpr ProgressBar& right() noexcept { part = Part::Right; return *this; }
            constexpr ProgressBar& filled() noexcept { filledval = true; return *this; }
            // clang-format on

            operator atlas::Buffer() const
            {
                /*
                    .___________. <- cell top
                    |           |
                    | XXXXXXXXX |
                    | X       X |
                    | X XXXXX X |
                    | X XXXXX X |
                    | X XXXXX X |
                    | X XXXXX X |
                    | X XXXXX X |
                    | X XXXXX X |
                    | X       X |
                    | XXXXXXXXX |
                    |           |
                    `-----------` <- cell bottom
                */

                // clang-format off
                auto constexpr Gap = 1 / 12_th;
                auto constexpr BlockLeft = 3 / 12_th;
                auto constexpr BlockRight = 9 / 12_th;
                auto constexpr BlockTop = 3 / 12_th;
                auto const blockBottom = 1.0 - (double(underlinePosition) / unbox<double>(size.height)) - 2 * Gap;

                auto b = blockElement<1>(size);

                switch (part)
                {
                case Part::Left:
                    b.rect({ BlockLeft-2*Gap, BlockTop-2*Gap },  { 1,             BlockTop-Gap });      // top line
                    b.rect({ BlockLeft-2*Gap, blockBottom+Gap }, { 1,             blockBottom+2*Gap }); // bottom line
                    b.rect({ BlockLeft-2*Gap, BlockTop-2*Gap },  { BlockLeft-Gap, blockBottom+Gap });   // left bar
                    if (filledval)
                        b.rect({ BlockLeft,   BlockTop },        { 1,             blockBottom });
                    break;
                case Part::Middle:
                    b.rect({ 0, BlockTop-2*Gap },  { 1, BlockTop-Gap });      // top line
                    b.rect({ 0, blockBottom+Gap }, { 1, blockBottom+2*Gap }); // bottom line
                    if (filledval)
                        b.rect({ 0,   BlockTop },  { 1, blockBottom });
                    break;
                case Part::Right:
                    b.rect({ 0,              BlockTop-2*Gap },  { BlockRight+2*Gap, BlockTop-Gap });      // top line
                    b.rect({ 0,              blockBottom+Gap }, { BlockRight+2*Gap, blockBottom+2*Gap }); // bottom line
                    b.rect({ BlockRight+Gap, BlockTop-2*Gap },  { BlockRight+2*Gap, blockBottom+Gap });   // left bar
                    if (filledval)
                        b.rect({ 0,          BlockTop },        { BlockRight,       blockBottom });
                    break;
                }
                // clang-format on

                return b;
            }
        };

        struct Box
        {
            Line upval = NoLine;
            Line rightval = NoLine;
            Line downval = NoLine;
            Line leftval = NoLine;
            Diagonal diagonalval = NoDiagonal;
            Arc arcval = NoArc;

            [[nodiscard]] constexpr Box up(Line value = Light)
            {
                Box b(*this);
                b.upval = value;
                return b;
            }
            [[nodiscard]] constexpr Box right(Line value = Light)
            {
                Box b(*this);
                b.rightval = value;
                return b;
            }
            [[nodiscard]] constexpr Box down(Line value = Light)
            {
                Box b(*this);
                b.downval = value;
                return b;
            }
            [[nodiscard]] constexpr Box left(Line value = Light)
            {
                Box b(*this);
                b.leftval = value;
                return b;
            }
            [[nodiscard]] constexpr Box diagonal(Diagonal value)
            {
                Box b(*this);
                b.diagonalval = value;
                return b;
            }

            [[nodiscard]] constexpr Box arc(Arc value)
            {
                Box b(*this);
                b.arcval = value;
                return b;
            }

            [[nodiscard]] constexpr optional<pair<uint8_t, Thickness>> get_dashed_horizontal() const noexcept
            {
                return getDashed(leftval, rightval);
            }

            [[nodiscard]] constexpr optional<pair<uint8_t, Thickness>> get_dashed_vertical() const noexcept
            {
                return getDashed(upval, downval);
            }

            [[nodiscard]] constexpr Box vertical(Line value = Light)
            {
                Box b(*this);
                b.upval = value;
                b.downval = value;
                return b;
            }

            [[nodiscard]] constexpr Box horizontal(Line value = Light)
            {
                Box b(*this);
                b.leftval = value;
                b.rightval = value;
                return b;
            }

          private:
            [[nodiscard]] static constexpr optional<pair<uint8_t, Thickness>> getDashed(Line a,
                                                                                        Line b) noexcept
            {
                if (a != b)
                    return nullopt;

                switch (a)
                {
                    case detail::Light2: return pair { 2, Thickness::Light };
                    case detail::Light3: return pair { 3, Thickness::Light };
                    case detail::Light4: return pair { 4, Thickness::Light };
                    case detail::Heavy2: return pair { 2, Thickness::Heavy };
                    case detail::Heavy3: return pair { 3, Thickness::Heavy };
                    case detail::Heavy4: return pair { 4, Thickness::Heavy };
                    default: return nullopt;
                }
            }
        };

        // U+2500 .. U+257F (128 box drawing characters)

        constexpr auto BoxDrawingDefinitions = std::array<Box, 0x80> // {{{
            {
                Box {}.horizontal(Light),        // U+2500
                Box {}.horizontal(Heavy),        // U+2501
                Box {}.vertical(Light),          // U+2502
                Box {}.vertical(Heavy),          // U+2503
                Box {}.horizontal(Light3),       // U+2504
                Box {}.horizontal(Heavy3),       // U+2505
                Box {}.vertical(Light3),         // U+2506
                Box {}.vertical(Heavy3),         // U+2507
                Box {}.horizontal(Light4),       // U+2508
                Box {}.horizontal(Heavy4),       // U+2509
                Box {}.vertical(Light4),         // U+250A
                Box {}.vertical(Heavy4),         // U+250B
                Box {}.right().down(),           // U+250C
                Box {}.right(Heavy).down(Light), // U+250D
                Box {}.right(Light).down(Heavy), // U+250E
                Box {}.right(Heavy).down(Heavy), // U+250F

                Box {}.down().left(),                      // U+2510
                Box {}.down(Light).left(Heavy),            // U+2511
                Box {}.down(Heavy).left(Light),            // U+2512
                Box {}.down(Heavy).left(Heavy),            // U+2513
                Box {}.up().right(),                       // U+2514
                Box {}.up(Light).right(Heavy),             // U+2515
                Box {}.up(Heavy).right(Light),             // U+2516
                Box {}.up(Heavy).right(Heavy),             // U+2517
                Box {}.up().left(),                        // U+2518
                Box {}.up(Light).left(Heavy),              // U+2519
                Box {}.up(Heavy).left(Light),              // U+251A
                Box {}.up(Heavy).left(Heavy),              // U+251B
                Box {}.vertical().right(),                 // U+251C
                Box {}.vertical(Light).right(Heavy),       // U+251D
                Box {}.up(Heavy).right(Light).down(Light), // U+251E
                Box {}.up(Light).right(Light).down(Heavy), // U+251F

                Box {}.vertical(Heavy).right(Light),         // U+2520
                Box {}.up(Heavy).right(Heavy).down(Light),   // U+2521
                Box {}.up(Light).right(Heavy).down(Heavy),   // U+2522
                Box {}.up(Heavy).right(Heavy).down(Heavy),   // U+2523
                Box {}.vertical(Light).left(Light),          // U+2524
                Box {}.vertical(Light).left(Heavy),          // U+2525
                Box {}.up(Heavy).down(Light).left(Light),    // U+2526
                Box {}.up(Light).down(Heavy).left(Light),    // U+2527
                Box {}.up(Heavy).down(Heavy).left(Light),    // U+2528
                Box {}.up(Heavy).down(Light).left(Heavy),    // U+2529
                Box {}.up(Light).down(Heavy).left(Heavy),    // U+252A
                Box {}.up(Heavy).down(Heavy).left(Heavy),    // U+252B
                Box {}.right(Light).down(Light).left(Light), // U+252C
                Box {}.right(Light).down(Light).left(Heavy), // U+252D
                Box {}.right(Heavy).down(Light).left(Light), // U+252E
                Box {}.right(Heavy).down(Light).left(Heavy), // U+252F

                Box {}.right(Light).down(Heavy).left(Light),           // U+2530
                Box {}.right(Light).down(Heavy).left(Heavy),           // U+2531
                Box {}.right(Heavy).down(Heavy).left(Light),           // U+2532
                Box {}.right(Heavy).down(Heavy).left(Heavy),           // U+2533
                Box {}.up(Light).right(Light).left(Light),             // U+2534
                Box {}.up(Light).right(Light).left(Heavy),             // U+2535
                Box {}.up(Light).right(Heavy).left(Light),             // U+2536
                Box {}.up(Light).right(Heavy).left(Heavy),             // U+2537
                Box {}.up(Heavy).right(Light).left(Light),             // U+2538
                Box {}.up(Heavy).right(Light).left(Heavy),             // U+2539
                Box {}.up(Heavy).right(Heavy).left(Light),             // U+253A
                Box {}.up(Heavy).right(Heavy).left(Heavy),             // U+253B
                Box {}.up(Light).right(Light).down(Light).left(Light), // U+253C
                Box {}.up(Light).right(Light).down(Light).left(Heavy), // U+253D
                Box {}.up(Light).right(Heavy).down(Light).left(Light), // U+253E
                Box {}.up(Light).right(Heavy).down(Light).left(Heavy), // U+253F

                Box {}.up(Heavy).right(Light).down(Light).left(Heavy), // U+2540
                Box {}.up(Light).right(Light).down(Heavy).left(Light), // U+2541
                Box {}.up(Heavy).right(Light).down(Heavy).left(Light), // U+2542
                Box {}.up(Heavy).right(Light).down(Light).left(Heavy), // U+2543
                Box {}.up(Heavy).right(Heavy).down(Light).left(Light), // U+2544
                Box {}.up(Light).right(Light).down(Heavy).left(Heavy), // U+2545
                Box {}.up(Light).right(Heavy).down(Heavy).left(Light), // U+2546
                Box {}.up(Heavy).right(Heavy).down(Light).left(Heavy), // U+2547
                Box {}.up(Light).right(Heavy).down(Heavy).left(Heavy), // U+2548
                Box {}.up(Heavy).right(Light).down(Heavy).left(Heavy), // U+2549
                Box {}.up(Heavy).right(Heavy).down(Heavy).left(Light), // U+254A
                Box {}.up(Heavy).right(Heavy).down(Heavy).left(Heavy), // U+254B
                Box {}.horizontal(Light2),                             // U+254C
                Box {}.horizontal(Heavy2),                             // U+254D
                Box {}.vertical(Light2),                               // U+254E
                Box {}.vertical(Heavy2),                               // U+254F

                Box {}.horizontal(Double),                   // U+2550
                Box {}.vertical(Double),                     // U+2551
                Box {}.right(Double).down(Light),            // U+2552
                Box {}.right(Light).down(Double),            // U+2553
                Box {}.right(Double).down(Double),           // U+2554
                Box {}.down(Light).left(Double),             // U+2555
                Box {}.down(Double).left(Light),             // U+2556
                Box {}.down(Double).left(Double),            // U+2557
                Box {}.up(Light).right(Double),              // U+2558
                Box {}.up(Double).right(Light),              // U+2559
                Box {}.up(Double).right(Double),             // U+255A
                Box {}.up(Light).left(Double),               // U+255B
                Box {}.up(Double).left(Light),               // U+255C
                Box {}.up(Double).left(Double),              // U+255D
                Box {}.up(Light).right(Double).down(Light),  // U+255E
                Box {}.up(Double).right(Light).down(Double), // U+255F

                Box {}.vertical(Double).right(Double),      // U+2560
                Box {}.vertical(Light).left(Double),        // U+2561
                Box {}.vertical(Double).left(Light),        // U+2562
                Box {}.vertical(Double).left(Double),       // U+2563
                Box {}.horizontal(Double).down(Light),      // U+2564
                Box {}.horizontal(Light).down(Double),      // U+2565
                Box {}.horizontal(Double).down(Double),     // U+2566
                Box {}.horizontal(Double).up(Light),        // U+2567
                Box {}.horizontal(Light).up(Double),        // U+2568
                Box {}.horizontal(Double).up(Double),       // U+2569
                Box {}.horizontal(Double).vertical(Light),  // U+256A
                Box {}.horizontal(Light).vertical(Double),  // U+256B
                Box {}.horizontal(Double).vertical(Double), // U+256C
                Box {}.arc(TopLeft),                        // U+256D
                Box {}.arc(TopRight),                       // U+256E
                Box {}.arc(BottomRight),                    // U+256F

                Box {}.arc(BottomLeft),          // U+2570
                Box {}.diagonal(Forward),        // U+2571
                Box {}.diagonal(Backward),       // U+2572
                Box {}.diagonal(Crossing),       // U+2573
                Box {}.left(),                   // U+2574
                Box {}.up(),                     // U+2575
                Box {}.right(),                  // U+2576
                Box {}.down(),                   // U+2577
                Box {}.left(Heavy),              // U+2578
                Box {}.up(Heavy),                // U+2579
                Box {}.right(Heavy),             // U+257A
                Box {}.down(Heavy),              // U+257B
                Box {}.right(Heavy).left(Light), // U+257C
                Box {}.up(Light).down(Heavy),    // U+257D
                Box {}.right(Light).left(Heavy), // U+257E
                Box {}.up(Heavy).down(Light),    // U+257F
            };
        // }}}

        static_assert(BoxDrawingDefinitions.size() == 0x80);

        // {{{ block element construction

        // Arguments from and to are passed as percentage.
        template <typename Container, typename F>
        constexpr void fillBlock(Container& image, ImageSize size, Ratio from, Ratio to, F const& filler)
        {
            auto const h = unbox<int>(size.height) - 1;

            for (auto y = int(unbox<double>(size.height) * from.y);
                 y < int(unbox<double>(size.height) * to.y);
                 ++y)
            {
                for (auto x = int(unbox<double>(size.width) * from.x);
                     x < int(unbox<double>(size.width) * to.x);
                     ++x)
                {
                    image[size_t(h - y) * *size.width + size_t(x)] = (uint8_t) filler(x, y);
                }
            }
        }

        template <size_t N, Inverted Inv>
        auto checker(ImageSize size)
        {
            auto const s = unbox<int>(size.width) / int(N);
            auto const t = unbox<int>(size.height) / int(N);
            return [s, t](int x, int y) {
                auto constexpr Set = Inv == Inverted::No ? 255 : 0;
                auto constexpr Unset = 255 - Set;
                if ((y / t) % 2)
                    return (x / s) % 2 != 0 ? Set : Unset;
                else
                    return (x / s) % 2 == 0 ? Set : Unset;
            };
        }

        template <size_t N>
        auto hbar(ImageSize size)
        {
            auto const s = unbox<int>(size.height) / int(N);
            return [s](int /*x*/, int y) {
                return (y / s) % 2 ? 255 : 0;
            };
        }

        template <size_t N>
        auto right_circ(ImageSize size)
        {
            auto const s = unbox<int>(size.height) / int(N);
            return [s](int /*x*/, int y) {
                return (y / s) % 2 ? 255 : 0;
            };
        }

        template <size_t N>
        auto dotted(ImageSize size)
        {
            auto const s = *size.width / N;
            auto const f = linearEq({ 0, 0 }, { 10, 10 });
            return [s, f](int x, int y) {
                return ((y) / s) % 2 && ((x) / s) % 2 ? 255 : 0;
            };
        }

        template <size_t N>
        auto gatter(ImageSize size)
        {
            auto const s = *size.width / N;
            auto const f = linearEq({ 0, 0 }, { 10, 10 });
            return [s, f](int x, int y) {
                return ((y) / s) % 2 || ((x) / s) % 2 ? 255 : 0;
            };
        }

        template <size_t N, int P>
        auto dbar(ImageSize size)
        {
            auto const s = *size.height / N;
            auto const f = linearEq({ 0, 0 }, { unbox<int>(size.width), unbox<int>(size.height) });
            return [s, f](int x, int y) {
                return (unsigned(y - P * f(x)) / s) % 2 ? 0 : 255;
            };
        }

        struct Lower
        {
            double value;
        };
        [[maybe_unused]] constexpr RatioBlock operator*(RatioBlock a, Lower b) noexcept
        {
            a.from.y = 0;
            a.to.y = b.value;
            return a;
        }

        struct Upper
        {
            double value;
        };
        [[maybe_unused]] constexpr RatioBlock operator*(RatioBlock a, Upper b) noexcept
        {
            a.from.y = b.value;
            a.to.y = 1.0;
            return a;
        }

        struct DiagonalMosaic
        {
            enum class Body
            {
                Lower,
                Upper
            };
            Body body = Body::Lower;
            double a {};
            double b {};
        };

        template <Dir Direction, int DivisorX>
        auto getTriangleProps(ImageSize size)
        {
            auto const c =
                point { Direction == Dir::Left ? unbox<int>(size.width) / DivisorX
                                               : unbox<int>(size.width) - unbox<int>(size.width) / DivisorX,
                        unbox<int>(size.height) / 2 };
            auto const w = unbox<int>(size.width) - 1;
            auto const h = unbox<int>(size.height) - 1;

            if constexpr (Direction == Dir::Left)
            {
                auto const a = linearEq({ 0, 0 }, c);
                auto const b = linearEq({ 0, h }, c);
                return [a, b](int x) {
                    return pair { a(x), b(x) };
                };
            }
            else if constexpr (Direction == Dir::Right)
            {
                auto const a = linearEq(c, { w, 0 });
                auto const b = linearEq(c, { w, h });
                return [a, b](int x) {
                    return pair { a(x), b(x) };
                };
            }
            else if constexpr (Direction == Dir::Top)
            {
                auto const a = linearEq({ 0, 0 }, c);
                auto const b = linearEq(c, { w, 0 });
                return [a, b, c](int x) {
                    if (x < c.x)
                        return pair { 0, a(x) };
                    else
                        return pair { 0, b(x) };
                };
            }
            else if constexpr (Direction == Dir::Bottom)
            {
                auto const a = linearEq({ 0, h }, c);
                auto const b = linearEq(c, { w, h });
                return [a, b, c, h](int x) {
                    if (x < c.x)
                        return pair { a(x), h };
                    else
                        return pair { b(x), h };
                    ;
                };
            }
        }

        template <int P>
        auto triChecker(ImageSize size)
        {
            auto const c = point { unbox<int>(size.width) / 2, unbox<int>(size.height) / 2 };
            auto const w = unbox<int>(size.width) - 1;

            auto const f = linearEq({ 0, 0 }, c);
            auto const g = linearEq(c, { w, 0 });
            auto const k = checker<4, Inverted::No>(size);

            return [=](int x, int y) {
                if constexpr (P == 1)
                    return g(x) >= y ? k(x, y) : 0; // OK
                if constexpr (P == 2)
                    return f(x) >= y ? k(x, y) : 0;
                if constexpr (P == 3)
                    return g(x) <= y ? k(x, y) : 0;
                if constexpr (P == 4)
                    return f(x) <= y ? k(x, y) : 0; // OK
                return 0;
            };
        }

        template <Inverted Inv>
        auto dchecker(ImageSize size)
        {
            auto constexpr Set = Inv == Inverted::No ? 255 : 0;
            auto constexpr Unset = 255 - Set;

            auto const c = point { unbox<int>(size.width) / 2, unbox<int>(size.height) / 2 };
            auto const w = unbox<int>(size.width) - 1;

            auto const f = linearEq({ 0, 0 }, c);
            auto const g = linearEq(c, { w, 0 });

            return [=](int x, int y) {
                auto const [a, b] = pair { f(x), g(x) };
                if (x <= c.x)
                    return a <= y && y <= b ? Set : Unset;
                else
                    return b <= y && y <= a ? Set : Unset;
            };
        }

        template <Dir Direction, Inverted inverted, int DivisorX>
        void fillTriangle(Pixmap& pixmap)
        {
            auto const p = getTriangleProps<Direction, DivisorX>(pixmap.size);
            auto const [set, unset] = []() -> pair<uint8_t, uint8_t> {
                return inverted == Inverted::No ? pair { 0xFF, 0 } : pair { 0, 0xFF };
            }();

            auto const w = unbox(pixmap.size.width);
            auto const h = unbox(pixmap.size.height) - 1;

            for (auto const y: ranges::views::iota(0u, unbox(pixmap.size.height)))
            {
                for (auto const x: ranges::views::iota(0u, unbox(pixmap.size.width)))
                {
                    auto const [a, b] = p(int(x));
                    pixmap.buffer[unsigned(h - y) * w + x] = a <= int(y) && int(y) <= b ? set : unset;
                }
            }
        }

        template <Dir Direction, Inverted Inv = Inverted::No, int DivisorX = 2>
        atlas::Buffer triangle(ImageSize size)
        {
            auto pixmap = blockElement<2>(size);
            fillTriangle<Direction, Inv, DivisorX>(pixmap);
            return pixmap.take();
        }

        enum class UpperOrLower
        {
            Upper,
            Lower
        };
        void diagonalMosaic(Pixmap& pixmap, Ratio ra, Ratio rb, UpperOrLower location) noexcept
        {
            auto const innerSize = pixmap.size - ImageSize { vtbackend::Width(1), vtbackend::Height(1) };

            auto const condition =
                [location, line = linearEq(innerSize * ra, innerSize * rb)](int x, int y) noexcept -> bool {
                return location == UpperOrLower::Upper ? y <= line(x) : y >= line(x);
            };

            auto const h = pixmap.size.height.as<unsigned>() - 1;
            auto const w = pixmap.size.width.as<unsigned>();
            for (auto const y: ranges::views::iota(0u, pixmap.size.height.as<unsigned>()))
                for (auto const x: ranges::views::iota(0u, pixmap.size.width.as<unsigned>()))
                    if (condition(int(x), int(y)))
                        pixmap.buffer.at(w * (h - y) + x) = 0xFF;
        }

        inline atlas::Buffer upperDiagonalMosaic(ImageSize size, Ratio ra, Ratio rb)
        {
            Pixmap pixmap = blockElement<2>(size);
            diagonalMosaic(pixmap, ra, rb, UpperOrLower::Upper);
            return pixmap.take();
        }

        inline atlas::Buffer lowerDiagonalMosaic(ImageSize size, Ratio ra, Ratio rb)
        {
            Pixmap pixmap = blockElement<2>(size);
            diagonalMosaic(pixmap, ra, rb, UpperOrLower::Lower);
            return pixmap.take();
        }

        struct MosaicBlock
        {
            std::vector<RatioBlock> blocks;
        };

        atlas::Buffer operator|(Pixmap a, RatioBlock block)
        {
            fillBlock(a.buffer, a.size, block.from, block.to, a.filler);
            return std::move(a.buffer);
        }

        atlas::Buffer operator|(Pixmap a, MosaicBlock const& b)
        {
            for (RatioBlock const block: b.blocks)
                fillBlock(a.buffer, a.size, block.from, block.to, a.filler);
            return std::move(a.buffer);
        }

        inline MosaicBlock operator+(RatioBlock a, RatioBlock b)
        {
            MosaicBlock m;
            m.blocks.push_back(a);
            m.blocks.push_back(b);
            return m;
        }

        inline MosaicBlock operator+(MosaicBlock a, RatioBlock b)
        {
            a.blocks.push_back(b);
            return a;
        }

        inline RatioBlock operator*(RatioBlock a, RatioBlock b) noexcept
        {
            auto const merge = [](double x, double y) {
                if (x == 0)
                    return y;
                if (y == 0)
                    return x;
                return min(x, y);
            };
            a.from.x = merge(a.from.x, b.from.x);
            a.from.y = merge(a.from.y, b.from.y);
            a.to.x = merge(a.to.x, b.to.x);
            a.to.y = merge(a.to.y, b.to.y);
            return a;
        }

        // 1 <= n <= r*n
        constexpr inline RatioBlock horiz_nth(double r, int n) noexcept
        {
            return RatioBlock { { 0, r * double(n - 1) }, { 1, r * double(n) } };
        }

        constexpr inline RatioBlock vert_nth(double r, int n) noexcept
        {
            return RatioBlock { { r * double(n - 1), 0 }, { r * double(n), 1 } };
        }

        [[maybe_unused]] inline Pixmap operator*(Pixmap&& image, RatioBlock block)
        {
            fillBlock(image.buffer, image.size, block.from, block.to, image.filler);
            return std::move(image);
        }

        // }}}
        // {{{ block sextant construction
        template <typename Container, typename T>
        constexpr inline void blockSextant(Container& image, ImageSize size, T position)
        {
            auto const x0 = (position - 1) % 2;
            auto const y0 = [position]() {
                switch (position / 32)
                {
                    case 0:
                        switch (position % 6)
                        {
                            case 1:
                            case 2: return 0;
                            case 3:
                            case 4: return 1;
                            case 5:
                            case 0: return 2;
                        }
                        break;
                    case 1:
                        switch (position % 6)
                        {
                            case 1:
                            case 2: return 2;
                            case 3:
                            case 4: return 1;
                            case 5:
                            case 0: return 0;
                        }
                        break;
                }
                Guarantee(false);
                crispy::unreachable();
                return 0;
            }();

            auto const x1 = x0 + 1;
            auto const y1 = y0 + 1;

            // fmt::print("- block sextant pos {}: x={} y={} x0={} y0={} x1={} y1={}\n",
            //            position, x, y, x0, y0, x1, y1);

            fillBlock(image, size, { x0 / 2_th, y0 / 3_th }, { x1 / 2_th, y1 / 3_th }, [](int, int) {
                return 0xFF;
            });
        }

        template <typename Container, typename A, typename... B>
        constexpr inline void blockSextant(Container& image, ImageSize size, A first, B... others)
        {
            blockSextant(image, size, first);
            blockSextant(image, size, others...);
        }

        template <typename... T>
        inline atlas::Buffer blockSextant(ImageSize size, T... positions)
        {
            auto image = atlas::Buffer(size.area(), 0x00);
            blockSextant(image, size, positions...);
            return image;
        }
        // }}}

    } // namespace
} // namespace detail

void BoxDrawingRenderer::setRenderTarget(RenderTarget& renderTarget,
                                         DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    clearCache();
}

void BoxDrawingRenderer::clearCache()
{
    // As we're reusing the upper layer's texture atlas, we do not need
    // to clear here anything. It's done for us already.
}

bool BoxDrawingRenderer::render(vtbackend::LineOffset line,
                                vtbackend::ColumnOffset column,
                                char32_t codepoint,
                                vtbackend::RGBColor color)
{
    Renderable::AtlasTileAttributes const* data = getOrCreateCachedTileAttributes(codepoint);
    if (!data)
        return false;

    auto const pos = _gridMetrics.map(line, column);
    auto const x = pos.x;
    auto const y = pos.y;

    auto renderTile = atlas::RenderTile {};
    renderTile.x = atlas::RenderTile::X { x };
    renderTile.y = atlas::RenderTile::Y { y };
    renderTile.bitmapSize = data->bitmapSize;
    renderTile.color = atlas::normalize(color);
    renderTile.normalizedLocation = data->metadata.normalizedLocation;
    renderTile.tileLocation = data->location;

    textureScheduler().renderTile(renderTile);
    return true;
}

constexpr inline bool containsNonCanonicalLines(char32_t codepoint)
{
    if (codepoint < 0x2500 || codepoint > 0x257F)
        return false;
    auto const& box = detail::BoxDrawingDefinitions[codepoint - 0x2500];
    return box.diagonalval != detail::NoDiagonal || box.arcval != NoArc;
}

auto BoxDrawingRenderer::createTileData(char32_t codepoint, atlas::TileLocation tileLocation)
    -> optional<TextureAtlas::TileCreateData>
{
    if (optional<atlas::Buffer> image = buildElements(codepoint))
    {
        *image = invertY(*image, _gridMetrics.cellSize);
        return { createTileData(tileLocation,
                                std::move(*image),
                                atlas::Format::Red,
                                _gridMetrics.cellSize,
                                RenderTileAttributes::X { 0 },
                                RenderTileAttributes::Y { 0 },
                                FRAGMENT_SELECTOR_GLYPH_ALPHA) };
    }

    auto const antialiasing = containsNonCanonicalLines(codepoint);
    atlas::Buffer pixels;
    if (antialiasing)
    {
        auto const supersamplingFactor = []() {
            auto constexpr EnvName = "SSA_FACTOR";
            if (!getenv(EnvName))
                return 2;
            auto const val = atoi(getenv(EnvName));
            if (!(val >= 1 && val <= 8))
                return 1;
            return val;
        }();
        auto const supersamplingSize = _gridMetrics.cellSize * supersamplingFactor;
        auto const supersamplingLineThickness = _gridMetrics.underline.thickness * 2;
        auto tmp = buildBoxElements(codepoint, supersamplingSize, supersamplingLineThickness);
        if (!tmp)
            return nullopt;

        // pixels = downsample(*tmp, _gridMetrics.cellSize, supersamplingFactor);
        pixels = downsample(*tmp, 1, supersamplingSize, _gridMetrics.cellSize);
    }
    else
    {
        auto tmp = buildBoxElements(codepoint, _gridMetrics.cellSize, _gridMetrics.underline.thickness);
        if (!tmp)
            return nullopt;
        pixels = std::move(*tmp);
    }

    pixels = invertY(pixels, _gridMetrics.cellSize);

    return { createTileData(tileLocation,
                            std::move(pixels),
                            atlas::Format::Red,
                            _gridMetrics.cellSize,
                            RenderTileAttributes::X { 0 },
                            RenderTileAttributes::Y { 0 },
                            FRAGMENT_SELECTOR_GLYPH_ALPHA) };
}

Renderable::AtlasTileAttributes const* BoxDrawingRenderer::getOrCreateCachedTileAttributes(char32_t codepoint)
{
    return textureAtlas().get_or_try_emplace(
        crispy::strong_hash { 31, 13, 8, static_cast<uint32_t>(codepoint) },
        [this, codepoint](atlas::TileLocation tileLocation) -> optional<TextureAtlas::TileCreateData> {
            return createTileData(codepoint, tileLocation);
        });
}

bool BoxDrawingRenderer::renderable(char32_t codepoint) noexcept
{
    auto const ascending = [codepoint](char32_t a, char32_t b) noexcept -> bool {
        return a <= codepoint && codepoint <= b;
    };

    return ascending(0x23A1, 0x23A6)      // mathematical square brackets
           || ascending(0x2500, 0x2590)   // box drawing, block elements
           || ascending(0x2594, 0x259F)   // Terminal graphic characters
           || ascending(0x1FB00, 0x1FBAF) // more block sextants
           || ascending(0x1FBF0, 0x1FBF9) // digits
           || ascending(0xEE00, 0xEE05)   // progress bar (Fira Code)
           || codepoint == 0xE0B0         // 
           || codepoint == 0xE0B2         // 
           || codepoint == 0xE0B4         // 
           || codepoint == 0xE0B6         // 
           || codepoint == 0xE0BA         // 
           || codepoint == 0xE0BC         // 
           || codepoint == 0xE0BE         // 
        ;
}

optional<atlas::Buffer> BoxDrawingRenderer::buildElements(char32_t codepoint)
{
    using namespace detail;

    auto const size = _gridMetrics.cellSize;

    auto const ud = [=](Ratio a, Ratio b) {
        return upperDiagonalMosaic(size, a, b);
    };
    auto const ld = [=](Ratio a, Ratio b) {
        return lowerDiagonalMosaic(size, a, b);
    };
    auto const lineArt =
#if __cplusplus > 201703L
        [=, this]
#else
        [=]
#endif
        () {
            auto b = blockElement<2>(size);
            b.getlineThickness(_gridMetrics.underline.thickness);
            return b;
        };
    auto const progressBar =
#if __cplusplus > 201703L
        [=, this]
#else
        [=]
#endif
        () {
            return ProgressBar { size, _gridMetrics.underline.position };
        };
    auto const segmentArt =
#if __cplusplus > 201703L
        [=, this]
#else
        [=]
#endif
        () {
            auto constexpr AntiAliasingSamplingFactor = 1;
            return blockElement<AntiAliasingSamplingFactor>(size)
                .getlineThickness(_gridMetrics.underline.thickness)
                .baseline(_gridMetrics.baseline * AntiAliasingSamplingFactor);
        };

    // TODO: just check notcurses-info to get an idea what may be missing
    // clang-format off
    switch (codepoint)
    {
        // TODO: case 0x239B: // ⎛ LEFT PARENTHESIS UPPER HOOK
        // TODO: case 0x239C: // ⎜ LEFT PARENTHESIS EXTENSION
        // TODO: case 0x239D: // ⎝ LEFT PARENTHESIS LOWER HOOK
        // TODO: case 0x239E: // ⎞ RIGHT PARENTHESIS UPPER HOOK
        // TODO: case 0x239F: // ⎟ RIGHT PARENTHESIS EXTENSION
        // TODO: case 0x23A0: // ⎠ RIGHT PARENTHESIS LOWER HOOK

        case 0x23A1: // ⎡ LEFT SQUARE BRACKET UPPER CORNER
            return blockElement(size) | (left(1 / 8_th) + upper(1 / 8_th) * left(1 / 2_th));
        case 0x23A2: // ⎢ LEFT SQUARE BRACKET EXTENSION
            return blockElement(size) | left(1 / 8_th);
        case 0x23A3: // ⎣ LEFT SQUARE BRACKET LOWER CORNER
            return blockElement(size) | (left(1 / 8_th) + lower(1 / 8_th) * left(1 / 2_th));
        case 0x23A4: // ⎤ RIGHT SQUARE BRACKET UPPER CORNER
            return blockElement(size) | (right(1 / 8_th) + upper(1 / 8_th) * right(1 / 2_th));
        case 0x23A5: // ⎥ RIGHT SQUARE BRACKET EXTENSION
            return blockElement(size) | right(1 / 8_th);
        case 0x23A6: // ⎦ RIGHT SQUARE BRACKET LOWER CORNER
            return blockElement(size) | (right(1 / 8_th) + lower(1 / 8_th) * right(1 / 2_th));

        // TODO: case 0x23A7: // ⎧ LEFT CURLY BRACKET UPPER HOOK
        // TODO: case 0x23A8: // ⎨ LEFT CURLY BRACKET MIDDLE PIECE
        // TODO: case 0x23A9: // ⎩ LEFT CURLY BRACKET LOWER HOOK
        // TODO: case 0x23AA: // ⎪ CURLY BRACKET EXTENSION
        // TODO: case 0x23AB: // ⎫ RIGHT CURLY BRACKET UPPER HOOK
        // TODO: case 0x23AC: // ⎬ RIGHT CURLY BRACKET MIDDLE PIECE
        // TODO: case 0x23AD: // ⎭ RIGHT CURLY BRACKET LOWER HOOK
        // TODO: case 0x23AE: // ⎮ INTEGRAL EXTENSION
        // TODO: case 0x23AF: // ⎯ HORIZONTAL LINE EXTENSION
        // TODO: case 0x23B0: // ⎰ UPPER LEFT OR LOWER RIGHT CURLY BRACKET SECTION
        // TODO: case 0x23B1: // ⎱ UPPER RIGHT OR LOWER LEFT CURLY BRACKET SECTION
        // TODO: case 0x23B2: // ⎲ SUMMATION TOP
        // TODO: case 0x23B3: // ⎳ SUMMATION BOTTOM

        // {{{ 2580..259F block elements
        case 0x2580: return blockElement(size) | upper(1 / 2_th); // ▀ UPPER HALF BLOCK
        case 0x2581: return blockElement(size) | lower(1 / 8_th); // ▁ LOWER ONE EIGHTH BLOCK
        case 0x2582: return blockElement(size) | lower(1 / 4_th); // ▂ LOWER ONE QUARTER BLOCK
        case 0x2583: return blockElement(size) | lower(3 / 8_th); // ▃ LOWER THREE EIGHTHS BLOCK
        case 0x2584: return blockElement(size) | lower(1 / 2_th); // ▄ LOWER HALF BLOCK
        case 0x2585: return blockElement(size) | lower(5 / 8_th); // ▅ LOWER FIVE EIGHTHS BLOCK
        case 0x2586: return blockElement(size) | lower(3 / 4_th); // ▆ LOWER THREE QUARTERS BLOCK
        case 0x2587: return blockElement(size) | lower(7 / 8_th); // ▇ LOWER SEVEN EIGHTHS BLOCK
        case 0x2588: return blockElement(size) | lower(1 / 1_th); // █ FULL BLOCK
        case 0x2589: return blockElement(size) | left(7 / 8_th);  // ▉ LEFT SEVEN EIGHTHS BLOCK
        case 0x258A: return blockElement(size) | left(3 / 4_th);  // ▊ LEFT THREE QUARTERS BLOCK
        case 0x258B: return blockElement(size) | left(5 / 8_th);  // ▋ LEFT FIVE EIGHTHS BLOCK
        case 0x258C: return blockElement(size) | left(1 / 2_th);  // ▌ LEFT HALF BLOCK
        case 0x258D: return blockElement(size) | left(3 / 8_th);  // ▍ LEFT THREE EIGHTHS BLOCK
        case 0x258E: return blockElement(size) | left(1 / 4_th);  // ▎ LEFT ONE QUARTER BLOCK
        case 0x258F: return blockElement(size) | left(1 / 8_th);  // ▏ LEFT ONE EIGHTH BLOCK
        case 0x2590:
            return blockElement(size) | right(1 / 2_th); // ▐ RIGHT HALF BLOCK
        // ░ TODO case 0x2591:
        // ▒ TODO case 0x2592:
        // ▓ TODO case 0x2593:
        case 0x2594: return blockElement(size) | upper(1 / 8_th); // ▔  UPPER ONE EIGHTH BLOCK
        case 0x2595: return blockElement(size) | right(1 / 8_th); // ▕  RIGHT ONE EIGHTH BLOCK
        case 0x2596:                                              // ▖  QUADRANT LOWER LEFT
            return blockElement(size) | (lower(1 / 2_th) * left(1 / 2_th));
        case 0x2597: // ▗  QUADRANT LOWER RIGHT
            return blockElement(size) | (lower(1 / 2_th) * right(1 / 2_th));
        case 0x2598: // ▘  QUADRANT UPPER LEFT
            return blockElement(size) | left(1 / 2_th) * upper(1 / 2_th);
        case 0x2599: // ▙  QUADRANT UPPER LEFT AND LOWER LEFT AND LOWER RIGHT
            return blockElement(size) | (left(1 / 2_th) * upper(1 / 2_th) + lower(1 / 2_th));
        case 0x259A: // ▚  QUADRANT UPPER LEFT AND LOWER RIGHT
            return blockElement(size)
                   | (upper(1 / 2_th) * left(1 / 2_th) + lower(1 / 2_th) * right(1 / 2_th));
        case 0x259B: // ▛  QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER LEFT
            return blockElement(size) | (upper(1 / 2_th) + lower(1 / 2_th) * left(1 / 2_th));
        case 0x259C: // ▜  QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER RIGHT
            return blockElement(size) | (upper(1 / 2_th) + lower(1 / 2_th) * right(1 / 2_th));
        case 0x259D: // ▝  QUADRANT UPPER RIGHT
            return blockElement(size) | (upper(1 / 2_th) * right(1 / 2_th));
        case 0x259E: // ▞  QUADRANT UPPER RIGHT AND LOWER LEFT
            return blockElement(size)
                   | (upper(1 / 2_th) * right(1 / 2_th) + lower(1 / 2_th) * left(1 / 2_th));
        case 0x259F: // ▟  QUADRANT UPPER RIGHT AND LOWER LEFT AND LOWER RIGHT
            return blockElement(size) | (upper(1 / 2_th) * right(1 / 2_th) + lower(1 / 2_th));
        // TODO: ■  U+25A0  BLACK SQUARE
        // TODO: □  U+25A1  WHITE SQUARE
        // TODO: ▢  U+25A2  WHITE SQUARE WITH ROUNDED CORNERS
        // TODO: ▣  U+25A3  WHITE SQUARE CONTAINING BLACK SMALL SQUARE
        // TODO: ▤  U+25A4  SQUARE WITH HORIZONTAL FILL
        // TODO: ▥  U+25A5  SQUARE WITH VERTICAL FILL
        // TODO: ▦  U+25A6  SQUARE WITH ORTHOGONAL CROSSHATCH FILL
        // TODO: ▧  U+25A7  SQUARE WITH UPPER LEFT TO LOWER RIGHT FILL
        // TODO: ▨  U+25A8  SQUARE WITH UPPER RIGHT TO LOWER LEFT FILL
        // TODO: ▩  U+25A9  SQUARE WITH DIAGONAL CROSSHATCH FILL
        // TODO: ▪  U+25AA  BLACK SMALL SQUARE
        // TODO: ▫  U+25AB  WHITE SMALL SQUARE
        // TODO: ▬  U+25AC  BLACK RECTANGLE
        // TODO: ▭  U+25AD  WHITE RECTANGLE
        // TODO: ▮  U+25AE  BLACK VERTICAL RECTANGLE
        // TODO: ▯  U+25AF  WHITE VERTICAL RECTANGLE
        // TODO: ▰  U+25B0  BLACK PARALLELOGRAM
        // TODO: ▱  U+25B1  WHITE PARALLELOGRAM
        // TODO: ▲  U+25B2  BLACK UP-POINTING TRIANGLE
        // TODO: △  U+25B3  WHITE UP-POINTING TRIANGLE
        // TODO: ▴  U+25B4  BLACK UP-POINTING SMALL TRIANGLE
        // TODO: ▵  U+25B5  WHITE UP-POINTING SMALL TRIANGLE
        // TODO: ▶  U+25B6  BLACK RIGHT-POINTING TRIANGLE
        // TODO: ▷  U+25B7  WHITE RIGHT-POINTING TRIANGLE
        // TODO: ▸  U+25B8  BLACK RIGHT-POINTING SMALL TRIANGLE
        // TODO: ▹  U+25B9  WHITE RIGHT-POINTING SMALL TRIANGLE
        // TODO: ►  U+25BA  BLACK RIGHT-POINTING POINTER
        // TODO: ▻  U+25BB  WHITE RIGHT-POINTING POINTER
        // TODO: ▼  U+25BC  BLACK DOWN-POINTING TRIANGLE
        // TODO: ▽  U+25BD  WHITE DOWN-POINTING TRIANGLE
        // TODO: ▾  U+25BE  BLACK DOWN-POINTING SMALL TRIANGLE
        // TODO: ▿  U+25BF  WHITE DOWN-POINTING SMALL TRIANGLE
        // TODO: ◀  U+25C0  BLACK LEFT-POINTING TRIANGLE
        // TODO: ◁  U+25C1  WHITE LEFT-POINTING TRIANGLE
        // TODO: ◂  U+25C2  BLACK LEFT-POINTING SMALL TRIANGLE
        // TODO: ◃  U+25C3  WHITE LEFT-POINTING SMALL TRIANGLE
        // TODO: ◄  U+25C4  BLACK LEFT-POINTING POINTER
        // TODO: ◅  U+25C5  WHITE LEFT-POINTING POINTER
        // TODO: ◆  U+25C6  BLACK DIAMOND
        // TODO: ◇  U+25C7  WHITE DIAMOND
        // TODO: ◈  U+25C8  WHITE DIAMOND CONTAINING BLACK SMALL DIAMOND
        // TODO: ◉  U+25C9  FISHEYE
        // TODO: ◊  U+25CA  LOZENGE
        // TODO: ○  U+25CB  WHITE CIRCLE
        // TODO: ◌  U+25CC  DOTTED CIRCLE
        // TODO: ◍  U+25CD  CIRCLE WITH VERTICAL FILL
        // TODO: ◎  U+25CE  BULLSEYE
        // TODO: ●  U+25CF  BLACK CIRCLE
        // TODO: ◐  U+25D0  CIRCLE WITH LEFT HALF BLACK
        // TODO: ◑  U+25D1  CIRCLE WITH RIGHT HALF BLACK
        // TODO: ◒  U+25D2  CIRCLE WITH LOWER HALF BLACK
        // TODO: ◓  U+25D3  CIRCLE WITH UPPER HALF BLACK
        // TODO: ◔  U+25D4  CIRCLE WITH UPPER RIGHT QUADRANT BLACK
        // TODO: ◕  U+25D5  CIRCLE WITH ALL BUT UPPER LEFT QUADRANT BLACK
        // TODO: ◖  U+25D6  LEFT HALF BLACK CIRCLE
        // TODO: ◗  U+25D7  RIGHT HALF BLACK CIRCLE
        // TODO: ◘  U+25D8  INVERSE BULLET
        // TODO: ◙  U+25D9  INVERSE WHITE CIRCLE
        // TODO: ◚  U+25DA  UPPER HALF INVERSE WHITE CIRCLE
        // TODO: ◛  U+25DB  LOWER HALF INVERSE WHITE CIRCLE
        // TODO: ◜  U+25DC  UPPER LEFT QUADRANT CIRCULAR ARC
        // TODO: ◝  U+25DD  UPPER RIGHT QUADRANT CIRCULAR ARC
        // TODO: ◞  U+25DE  LOWER RIGHT QUADRANT CIRCULAR ARC
        // TODO: ◟  U+25DF  LOWER LEFT QUADRANT CIRCULAR ARC
        // TODO: ◠  U+25E0  UPPER HALF CIRCLE
        // TODO: ◡  U+25E1  LOWER HALF CIRCLE
        // TODO: ◢  U+25E2  BLACK LOWER RIGHT TRIANGLE
        // TODO: ◣  U+25E3  BLACK LOWER LEFT TRIANGLE
        // TODO: ◤  U+25E4  BLACK UPPER LEFT TRIANGLE
        // TODO: ◥  U+25E5  BLACK UPPER RIGHT TRIANGLE
        // TODO: ◦  U+25E6  WHITE BULLET
        // TODO: ◧  U+25E7  SQUARE WITH LEFT HALF BLACK
        // TODO: ◨  U+25E8  SQUARE WITH RIGHT HALF BLACK
        // TODO: ◩  U+25E9  SQUARE WITH UPPER LEFT DIAGONAL HALF BLACK
        // TODO: ◪  U+25EA  SQUARE WITH LOWER RIGHT DIAGONAL HALF BLACK
        // TODO: ◫  U+25EB  WHITE SQUARE WITH VERTICAL BISECTING LINE
        // TODO: ◬  U+25EC  WHITE UP-POINTING TRIANGLE WITH DOT
        // TODO: ◭  U+25ED  UP-POINTING TRIANGLE WITH LEFT HALF BLACK
        // TODO: ◮  U+25EE  UP-POINTING TRIANGLE WITH RIGHT HALF BLACK
        // TODO: ◯  U+25EF  LARGE CIRCLE
        // TODO: ◰  U+25F0  WHITE SQUARE WITH UPPER LEFT QUADRANT
        // TODO: ◱  U+25F1  WHITE SQUARE WITH LOWER LEFT QUADRANT
        // TODO: ◲  U+25F2  WHITE SQUARE WITH LOWER RIGHT QUADRANT
        // TODO: ◳  U+25F3  WHITE SQUARE WITH UPPER RIGHT QUADRANT
        // TODO: ◴  U+25F4  WHITE CIRCLE WITH UPPER LEFT QUADRANT
        // TODO: ◵  U+25F5  WHITE CIRCLE WITH LOWER LEFT QUADRANT
        // TODO: ◶  U+25F6  WHITE CIRCLE WITH LOWER RIGHT QUADRANT
        // TODO: ◷  U+25F7  WHITE CIRCLE WITH UPPER RIGHT QUADRANT
        // TODO: ◸  U+25F8  UPPER LEFT TRIANGLE
        // TODO: ◹  U+25F9  UPPER RIGHT TRIANGLE
        // TODO: ◺  U+25FA  LOWER LEFT TRIANGLE
        // TODO: ◻  U+25FB  WHITE MEDIUM SQUARE
        // TODO: ◼  U+25FC  BLACK MEDIUM SQUARE
        // TODO: ◽ U+25FD  WHITE MEDIUM SMALL SQUARE
        // TODO: ◾ U+25FE  BLACK MEDIUM SMALL SQUARE
        // TODO: ◿  U+25FF  LOWER RIGHT TRIANGLE
        // }}}
        // {{{ 1FB00..1FB3B sextant blocks
        case 0x1FB00: return blockSextant(size, 1);             // 🬀  BLOCK SEXTANT-1
        case 0x1FB01: return blockSextant(size, 2);             // 🬁  BLOCK SEXTANT-2
        case 0x1FB02: return blockSextant(size, 1, 2);          // 🬂  BLOCK SEXTANT-12
        case 0x1FB03: return blockSextant(size, 3);             // 🬃  BLOCK SEXTANT-3
        case 0x1FB04: return blockSextant(size, 1, 3);          // 🬄  BLOCK SEXTANT-13
        case 0x1FB05: return blockSextant(size, 2, 3);          // 🬅  BLOCK SEXTANT-23
        case 0x1FB06: return blockSextant(size, 1, 2, 3);       // 🬆  BLOCK SEXTANT-123
        case 0x1FB07: return blockSextant(size, 4);             // 🬇  BLOCK SEXTANT-4
        case 0x1FB08: return blockSextant(size, 1, 4);          // 🬈  BLOCK SEXTANT-14
        case 0x1FB09: return blockSextant(size, 2, 4);          // 🬉  BLOCK SEXTANT-24
        case 0x1FB0A: return blockSextant(size, 1, 2, 4);       // 🬊  BLOCK SEXTANT-124
        case 0x1FB0B: return blockSextant(size, 3, 4);          // 🬋  BLOCK SEXTANT-34
        case 0x1FB0C: return blockSextant(size, 1, 3, 4);       // 🬌  BLOCK SEXTANT-134
        case 0x1FB0D: return blockSextant(size, 2, 3, 4);       // 🬍  BLOCK SEXTANT-234
        case 0x1FB0E: return blockSextant(size, 1, 2, 3, 4);    // 🬎  BLOCK SEXTANT-1234
        case 0x1FB0F: return blockSextant(size, 5);             // 🬏  BLOCK SEXTANT-5
        case 0x1FB10: return blockSextant(size, 1, 5);          // 🬐  BLOCK SEXTANT-15
        case 0x1FB11: return blockSextant(size, 2, 5);          // 🬑  BLOCK SEXTANT-25
        case 0x1FB12: return blockSextant(size, 1, 2, 5);       // 🬒  BLOCK SEXTANT-125
        case 0x1FB13: return blockSextant(size, 3, 5);          // 🬓  BLOCK SEXTANT-35
        case 0x1FB14: return blockSextant(size, 2, 3, 5);       // 🬔  BLOCK SEXTANT-235
        case 0x1FB15: return blockSextant(size, 1, 2, 3, 5);    // 🬕  BLOCK SEXTANT-1235
        case 0x1FB16: return blockSextant(size, 4, 5);          // 🬖  BLOCK SEXTANT-45
        case 0x1FB17: return blockSextant(size, 1, 4, 5);       // 🬗  BLOCK SEXTANT-145
        case 0x1FB18: return blockSextant(size, 2, 4, 5);       // 🬘  BLOCK SEXTANT-245
        case 0x1FB19: return blockSextant(size, 1, 2, 4, 5);    // 🬙  BLOCK SEXTANT-1245
        case 0x1FB1A: return blockSextant(size, 3, 4, 5);       // 🬚  BLOCK SEXTANT-345
        case 0x1FB1B: return blockSextant(size, 1, 3, 4, 5);    // 🬛  BLOCK SEXTANT-1345
        case 0x1FB1C: return blockSextant(size, 2, 3, 4, 5);    // 🬜  BLOCK SEXTANT-2345
        case 0x1FB1D: return blockSextant(size, 1, 2, 3, 4, 5); // 🬝  BLOCK SEXTANT-12345
        case 0x1FB1E: return blockSextant(size, 6);             // 🬞  BLOCK SEXTANT-6
        case 0x1FB1F: return blockSextant(size, 1, 6);          // 🬟  BLOCK SEXTANT-16
        case 0x1FB20: return blockSextant(size, 2, 6);          // 🬠  BLOCK SEXTANT-26
        case 0x1FB21: return blockSextant(size, 1, 2, 6);       // 🬡  BLOCK SEXTANT-126
        case 0x1FB22: return blockSextant(size, 3, 6);          // 🬢  BLOCK SEXTANT-36
        case 0x1FB23: return blockSextant(size, 1, 3, 6);       // 🬣  BLOCK SEXTANT-136
        case 0x1FB24: return blockSextant(size, 2, 3, 6);       // 🬤  BLOCK SEXTANT-236
        case 0x1FB25: return blockSextant(size, 1, 2, 3, 6);    // 🬥  BLOCK SEXTANT-1236
        case 0x1FB26: return blockSextant(size, 4, 6);          // 🬦  BLOCK SEXTANT-46
        case 0x1FB27: return blockSextant(size, 1, 4, 6);       // 🬧  BLOCK SEXTANT-146
        case 0x1FB28: return blockSextant(size, 1, 2, 4, 6);    // 🬨  BLOCK SEXTANT-1246
        case 0x1FB29: return blockSextant(size, 3, 4, 6);       // 🬩  BLOCK SEXTANT-346
        case 0x1FB2A: return blockSextant(size, 1, 3, 4, 6);    // 🬪  BLOCK SEXTANT-1346
        case 0x1FB2B: return blockSextant(size, 2, 3, 4, 6);    // 🬫  BLOCK SEXTANT-2346
        case 0x1FB2C: return blockSextant(size, 1, 2, 3, 4, 6); // 🬬  BLOCK SEXTANT-12346
        case 0x1FB2D: return blockSextant(size, 5, 6);          // 🬭  BLOCK SEXTANT-56
        case 0x1FB2E: return blockSextant(size, 1, 5, 6);       // 🬮  BLOCK SEXTANT-156
        case 0x1FB2F: return blockSextant(size, 2, 5, 6);       // 🬯  BLOCK SEXTANT-256
        case 0x1FB30: return blockSextant(size, 1, 2, 5, 6);    // 🬰  BLOCK SEXTANT-1256
        case 0x1FB31: return blockSextant(size, 3, 5, 6);       // 🬱  BLOCK SEXTANT-356
        case 0x1FB32: return blockSextant(size, 1, 3, 5, 6);    // 🬲  BLOCK SEXTANT-1356
        case 0x1FB33: return blockSextant(size, 2, 3, 5, 6);    // 🬳  BLOCK SEXTANT-2356
        case 0x1FB34: return blockSextant(size, 1, 2, 3, 5, 6); // 🬴  BLOCK SEXTANT-12356
        case 0x1FB35: return blockSextant(size, 4, 5, 6);       // 🬵  BLOCK SEXTANT-456
        case 0x1FB36: return blockSextant(size, 1, 4, 5, 6);    // 🬶  BLOCK SEXTANT-1456
        case 0x1FB37: return blockSextant(size, 2, 4, 5, 6);    // 🬷  BLOCK SEXTANT-2456
        case 0x1FB38: return blockSextant(size, 1, 2, 4, 5, 6); // 🬸  BLOCK SEXTANT-12456
        case 0x1FB39: return blockSextant(size, 3, 4, 5, 6);    // 🬹  BLOCK SEXTANT-3456
        case 0x1FB3A: return blockSextant(size, 1, 3, 4, 5, 6); // 🬺  BLOCK SEXTANT-13456
        case 0x1FB3B: return blockSextant(size, 2, 3, 4, 5, 6); // 🬻  BLOCK SEXTANT-23456
        // }}}
        // {{{ 1FB3C..1FBAF diagonals, nth, block elements
        case 0x1FB3C: return /* 🬼  */ ld({ 0, 3 / 4_th }, { 1 / 4_th, 1 });
        case 0x1FB3D: return /* 🬽  */ ld({ 0, 3 / 4_th }, { 1, 1 });
        case 0x1FB3E: return /* 🬾  */ ld({ 0, 1 / 4_th }, { 1 / 2_th, 1 });
        case 0x1FB3F: return /* 🬿  */ ld({ 0, 1 / 4_th }, { 1, 1 });
        case 0x1FB40: return /* 🭀  */ ld({ 0, 0 }, { 1 / 2_th, 1 });
        case 0x1FB41: return /* 🭁  */ ld({ 0, 1 / 4_th }, { 1 / 2_th, 0 });
        case 0x1FB42: return /* 🭂  */ ld({ 0, 1 / 4_th }, { 1, 0 });
        case 0x1FB43: return /* 🭃  */ ld({ 0, 3 / 4_th }, { 1 / 2_th, 0 });
        case 0x1FB44: return /* 🭄  */ ld({ 0, 3 / 4_th }, { 1, 0 });
        case 0x1FB45: return /* 🭅  */ ld({ 0, 1 }, { 1 / 2_th, 0 });
        case 0x1FB46: return /* 🭆  */ ld({ 0, 3 / 4_th }, { 1, 1 / 4_th });
        case 0x1FB47: return /* 🭇  */ ld({ 3 / 4_th, 1 }, { 1, 3 / 4_th });
        case 0x1FB48: return /* 🭈  */ ld({ 0, 1 }, { 1, 3 / 4_th });
        case 0x1FB49: return /* 🭉  */ ld({ 1 / 2_th, 1 }, { 1, 1 / 4_th });
        case 0x1FB4A: return /* 🭊  */ ld({ 0, 1 }, { 1, 1 / 4_th });
        case 0x1FB4B: return /* 🭋  */ ld({ 1 / 2_th, 1 }, { 1, 0 });
        case 0x1FB4C: return /* 🭌  */ ld({ 1 / 2_th, 0 }, { 1, 1 / 4_th });
        case 0x1FB4D: return /* 🭍  */ ld({ 0, 0 }, { 1, 1 / 4_th });
        case 0x1FB4E: return /* 🭎  */ ld({ 1 / 2_th, 0 }, { 1, 3 / 4_th });
        case 0x1FB4F: return /* 🭏  */ ld({ 0, 0 }, { 1, 3 / 4_th });
        case 0x1FB50: return /* 🭐  */ ld({ 1 / 2_th, 0 }, { 1, 1 });
        case 0x1FB51: return /* 🭑  */ ld({ 0, 1 / 4_th }, { 1, 3 / 4_th });
        case 0x1FB52: return /* 🭒  */ ud({ 0, 3 / 4_th }, { 1 / 2_th, 1 });
        case 0x1FB53: return /* 🭓  */ ud({ 0, 3 / 4_th }, { 1, 1 });
        case 0x1FB54: return /* 🭔  */ ud({ 0, 1 / 4_th }, { 1 / 2_th, 1 });
        case 0x1FB55: return /* 🭕  */ ud({ 0, 1 / 4_th }, { 1, 1 }); // XXX
        case 0x1FB56: return /* 🭖  */ ud({ 0, 0 }, { 1 / 2_th, 1 });
        case 0x1FB57: return /* 🭗  */ ud({ 0, 1 / 4_th }, { 1 / 4_th, 0 });
        case 0x1FB58: return /* 🭘  */ ud({ 0, 1 / 4_th }, { 1, 0 });
        case 0x1FB59: return /* 🭙  */ ud({ 0, 3 / 4_th }, { 1 / 2_th, 0 });
        case 0x1FB5A: return /* 🭚  */ ud({ 0, 3 / 4_th }, { 1, 0 });
        case 0x1FB5B: return /* 🭛  */ ud({ 0, 1 }, { 1 / 2_th, 0 });
        case 0x1FB5C: return /* 🭜  */ ud({ 0, 3 / 4_th }, { 1, 1 / 4_th });
        case 0x1FB5D: return /* 🭝  */ ud({ 1 / 2_th, 1 }, { 1, 3 / 4_th });
        case 0x1FB5E: return /* 🭞  */ ud({ 0, 1 }, { 1, 3 / 4_th });
        case 0x1FB5F: return /* 🭟  */ ud({ 1 / 2_th, 1 }, { 1, 1 / 4_th });
        case 0x1FB60: return /* 🭠  */ ud({ 0, 1 }, { 1, .25 });
        case 0x1FB61: return /* 🭡  */ ud({ 1 / 2_th, 1 }, { 1, 0 });
        case 0x1FB62: return /* 🭢  */ ud({ 3 / 4_th, 0 }, { 1, 1 / 4_th });
        case 0x1FB63: return /* 🭣  */ ud({ 0, 0 }, { 1, 1 / 4_th });
        case 0x1FB64: return /* 🭤  */ ud({ 1 / 2_th, 0 }, { 1, 3 / 4_th });
        case 0x1FB65: return /* 🭥  */ ud({ 0, 0 }, { 1, 3 / 4_th });
        case 0x1FB66: return /* 🭦  */ ud({ 1 / 2_th, 0 }, { 1, 1 });
        case 0x1FB67: return /* 🭧  */ ud({ 0, 1 / 4_th }, { 1, 3 / 4_th });
        case 0x1FB68: return /* 🭨  */ triangle<Dir::Left, Inverted::Yes>(size);
        case 0x1FB69: return /* 🭩  */ triangle<Dir::Top, Inverted::Yes>(size);
        case 0x1FB6A: return /* 🭪  */ triangle<Dir::Right, Inverted::Yes>(size);
        case 0x1FB6B: return /* 🭫  */ triangle<Dir::Bottom, Inverted::Yes>(size);
        case 0x1FB6C: return /* 🭬  */ triangle<Dir::Left, Inverted::No>(size);
        case 0x1FB6D: return /* 🭭  */ triangle<Dir::Top, Inverted::No>(size);
        case 0x1FB6E: return /* 🭮  */ triangle<Dir::Right, Inverted::No>(size);
        case 0x1FB6F: return /* 🭯  */ triangle<Dir::Bottom, Inverted::No>(size);
        case 0x1FB70: return blockElement(size) | vert_nth(1 / 8_th, 2); // 🭰  VERTICAL ONE EIGHTH BLOCK-2
        case 0x1FB71: return blockElement(size) | vert_nth(1 / 8_th, 3); // 🭱  VERTICAL ONE EIGHTH BLOCK-3
        case 0x1FB72: return blockElement(size) | vert_nth(1 / 8_th, 4); // 🭲  VERTICAL ONE EIGHTH BLOCK-4
        case 0x1FB73: return blockElement(size) | vert_nth(1 / 8_th, 5); // 🭳  VERTICAL ONE EIGHTH BLOCK-5
        case 0x1FB74: return blockElement(size) | vert_nth(1 / 8_th, 6); // 🭴  VERTICAL ONE EIGHTH BLOCK-6
        case 0x1FB75: return blockElement(size) | vert_nth(1 / 8_th, 7); // 🭵  VERTICAL ONE EIGHTH BLOCK-7
        case 0x1FB76: return blockElement(size) | horiz_nth(1 / 8_th, 2); // 🭶  HORIZONTAL ONE EIGHTH BLOCK-2
        case 0x1FB77: return blockElement(size) | horiz_nth(1 / 8_th, 3); // 🭷  HORIZONTAL ONE EIGHTH BLOCK-3
        case 0x1FB78: return blockElement(size) | horiz_nth(1 / 8_th, 4); // 🭸  HORIZONTAL ONE EIGHTH BLOCK-4
        case 0x1FB79: return blockElement(size) | horiz_nth(1 / 8_th, 5); // 🭹  HORIZONTAL ONE EIGHTH BLOCK-5
        case 0x1FB7A: return blockElement(size) | horiz_nth(1 / 8_th, 6); // 🭺  HORIZONTAL ONE EIGHTH BLOCK-6
        case 0x1FB7B: return blockElement(size) | horiz_nth(1 / 8_th, 7); // 🭻  HORIZONTAL ONE EIGHTH BLOCK-7
        case 0x1FB7C:
            return blockElement(size)
                   | (left(1 / 8_th) + lower(1 / 8_th)); // 🭼  LEFT AND LOWER ONE EIGHTH BLOCK
        case 0x1FB7D:
            return blockElement(size)
                   | (left(1 / 8_th) + upper(1 / 8_th)); // 🭽  LEFT AND UPPER ONE EIGHTH BLOCK
        case 0x1FB7E:
            return blockElement(size)
                   | (right(1 / 8_th) + upper(1 / 8_th)); // 🭾  RIGHT AND UPPER ONE EIGHTH BLOCK
        case 0x1FB7F:
            return blockElement(size)
                   | (right(1 / 8_th) + lower(1 / 8_th)); // 🭿  RIGHT AND LOWER ONE EIGHTH BLOCK
        case 0x1FB80:
            return blockElement(size)
                   | (upper(1 / 8_th) + lower(1 / 8_th)); // 🮀  UPPER AND LOWER ONE EIGHTH BLOCK
        case 0x1FB81:
            return blockElement(size)
                   | (horiz_nth(1 / 8_th, 1) // 🮁  HORIZONTAL ONE EIGHTH BLOCK-1358
                      + horiz_nth(1 / 8_th, 3) + horiz_nth(1 / 8_th, 5) + horiz_nth(1 / 8_th, 7));
        case 0x1FB82: return blockElement(size) | upper(1 / 4_th); // 🮂  UPPER ONE QUARTER BLOCK
        case 0x1FB83: return blockElement(size) | upper(3 / 8_th); // 🮃  UPPER THREE EIGHTHS BLOCK
        case 0x1FB84: return blockElement(size) | upper(5 / 8_th); // 🮄  UPPER FIVE EIGHTHS BLOCK
        case 0x1FB85: return blockElement(size) | upper(3 / 4_th); // 🮅  UPPER THREE QUARTERS BLOCK
        case 0x1FB86: return blockElement(size) | upper(7 / 8_th); // 🮆  UPPER SEVEN EIGHTHS BLOCK
        case 0x1FB87: return blockElement(size) | right(1 / 4_th); // 🮇  RIGHT ONE QUARTER BLOCK
        case 0x1FB88: return blockElement(size) | right(3 / 8_th); // 🮈  RIGHT THREE EIGHTHS BLOCK
        case 0x1FB89: return blockElement(size) | right(5 / 8_th); // 🮉  RIGHT FIVE EIGHTHS BLOCK
        case 0x1FB8A: return blockElement(size) | right(3 / 4_th); // 🮊  RIGHT THREE QUARTERS BLOCK
        case 0x1FB8B: return blockElement(size) | right(7 / 8_th); // 🮋  RIGHT SEVEN EIGHTHS BLOCK
        case 0x1FB8C: return blockElement<1>(size, checker<4, Inverted::No>(size)) | left(1 / 2_th);
        case 0x1FB8D: return blockElement<1>(size, checker<4, Inverted::No>(size)) | right(1 / 2_th);
        case 0x1FB8E: return blockElement<1>(size, checker<4, Inverted::No>(size)) | upper(1 / 2_th);
        case 0x1FB8F: return blockElement<1>(size, checker<4, Inverted::No>(size)) | lower(1 / 2_th);
        case 0x1FB90: return blockElement<1>(size, checker<4, Inverted::No>(size)).fill();
        case 0x1FB91:
            return blockElement<1>(size).fill([size](int x, int y) {
                return y <= unbox<int>(size.height) / 2 ? 0xFF : checker<4, Inverted::No>(size)(x, y);
            });
        case 0x1FB92:
            return blockElement<1>(size).fill([size](int x, int y) {
                return y >= unbox<int>(size.height) / 2 ? 0xFF : checker<4, Inverted::No>(size)(x, y);
            });
        case 0x1FB93: break; // not assigned
        case 0x1FB94:
            return blockElement<1>(size).fill([size](int x, int y) {
                return x >= unbox<int>(size.width) / 2 ? 0xFF : checker<4, Inverted::No>(size)(x, y);
            });
        case 0x1FB95: return blockElement<1>(size).fill(checker<8, Inverted::No>(size));
        case 0x1FB96: return blockElement<1>(size).fill(checker<8, Inverted::Yes>(size));
        case 0x1FB97: return blockElement<1>(size).fill(hbar<4>(size));
        case 0x1FB98: return blockElement<2>(size).fill(dbar<8, +1>(size * 4));
        case 0x1FB99: return blockElement<2>(size).fill(dbar<8, -1>(size * 4));
        case 0x1FB9A: return blockElement<1>(size).fill(dchecker<Inverted::Yes>(size));
        case 0x1FB9B: return blockElement<1>(size).fill(dchecker<Inverted::No>(size));
        case 0x1FB9C: return blockElement<1>(size).fill(triChecker<1>(size));
        case 0x1FB9D: return blockElement<1>(size).fill(triChecker<2>(size));
        case 0x1FB9E: return blockElement<1>(size).fill(triChecker<3>(size));
        case 0x1FB9F: return blockElement<1>(size).fill(triChecker<4>(size));
        case 0x1FBA0: return lineArt().line({ 0, 1 / 2_th }, { 1 / 2_th, 0 });
        case 0x1FBA1: return lineArt().line({ 1 / 2_th, 0 }, { 1, 1 / 2_th });
        case 0x1FBA2: return lineArt().line({ 0, 1 / 2_th }, { 1 / 2_th, 1 });
        case 0x1FBA3: return lineArt().line({ 1 / 2_th, 1 }, { 1, 1 / 2_th });
        case 0x1FBA4:
            return lineArt().line({ 0, 1 / 2_th }, { 1 / 2_th, 0 }).line({ 0, 1 / 2_th }, { 1 / 2_th, 1 });
        case 0x1FBA5:
            return lineArt().line({ 1 / 2_th, 0 }, { 1, 1 / 2_th }).line({ 1 / 2_th, 1 }, { 1, 1 / 2_th });
        case 0x1FBA6:
            return lineArt().line({ 0, 1 / 2_th }, { 1 / 2_th, 1 }).line({ 1 / 2_th, 1 }, { 1, 1 / 2_th });
        case 0x1FBA7:
            return lineArt().line({ 0, 1 / 2_th }, { 1 / 2_th, 0 }).line({ 1 / 2_th, 0 }, { 1, 1 / 2_th });
        case 0x1FBA8:
            return lineArt().line({ 0, 1 / 2_th }, { 1 / 2_th, 0 }).line({ 1 / 2_th, 1 }, { 1, 1 / 2_th });
        case 0x1FBA9:
            return lineArt().line({ 1 / 2_th, 0 }, { 1, 1 / 2_th }).line({ 0, 1 / 2_th }, { 1 / 2_th, 1 });
        case 0x1FBAA:
            return lineArt()
                . // line({0, 1/2_th}, {1/2_th, 0}).
                line({ 1 / 2_th, 0 }, { 1, 1 / 2_th })
                .line({ 0, 1 / 2_th }, { 1 / 2_th, 1 })
                .line({ 1 / 2_th, 1 }, { 1, 1 / 2_th })
                .take();
        case 0x1FBAB:
            return lineArt()
                .line({ 0, 1 / 2_th }, { 1 / 2_th, 0 })
                .
                // line({1/2_th, 0}, {1, 1/2_th}).
                line({ 0, 1 / 2_th }, { 1 / 2_th, 1 })
                .line({ 1 / 2_th, 1 }, { 1, 1 / 2_th })
                .take();
        case 0x1FBAC:
            return lineArt()
                .line({ 0, 1 / 2_th }, { 1 / 2_th, 0 })
                .line({ 1 / 2_th, 0 }, { 1, 1 / 2_th })
                .
                // line({0, 1/2_th}, {1/2_th, 1}).
                line({ 1 / 2_th, 1 }, { 1, 1 / 2_th })
                .take();
        case 0x1FBAD:
            return lineArt()
                .line({ 0, 1 / 2_th }, { 1 / 2_th, 0 })
                .line({ 1 / 2_th, 0 }, { 1, 1 / 2_th })
                .line({ 0, 1 / 2_th }, { 1 / 2_th, 1 })
                .
                // line({1/2_th, 1}, {1, 1/2_th}).
                take();
        case 0x1FBAE:
            return lineArt()
                .line({ 0, 1 / 2_th }, { 1 / 2_th, 0 })
                .line({ 1 / 2_th, 0 }, { 1, 1 / 2_th })
                .line({ 0, 1 / 2_th }, { 1 / 2_th, 1 })
                .line({ 1 / 2_th, 1 }, { 1, 1 / 2_th })
                .take();
        case 0x1FBAF:
            return lineArt()
                .line({ 0, 1 / 2_th }, { 1, 1 / 2_th })
                .line({ 1 / 2_th, 3 / 8_th }, { 1 / 2_th, 5 / 8_th })
                .take();
        case 0x1FBF0: return segmentArt().segment_bar(1, 2, 4, 5, 6, 7);
        case 0x1FBF1: return segmentArt().segment_bar(2, 5);
        case 0x1FBF2: return segmentArt().segment_bar(1, 2, 3, 6, 7);
        case 0x1FBF3: return segmentArt().segment_bar(1, 2, 3, 5, 6);
        case 0x1FBF4: return segmentArt().segment_bar(2, 3, 4, 5);
        case 0x1FBF5: return segmentArt().segment_bar(1, 3, 4, 5, 6);
        case 0x1FBF6: return segmentArt().segment_bar(1, 3, 4, 5, 6, 7);
        case 0x1FBF7: return segmentArt().segment_bar(1, 2, 5);
        case 0x1FBF8: return segmentArt().segment_bar(1, 2, 3, 4, 5, 6, 7);
        case 0x1FBF9: return segmentArt().segment_bar(1, 2, 3, 4, 5, 6);
        // }}}

        case 0xE0B0: return /*  */ triangle<Dir::Left, Inverted::No, 1>(size);
        case 0xE0B2: return /*  */ triangle<Dir::Right, Inverted::No, 1>(size);
        case 0xE0B4: return /*  */ blockElement<2>(size).halfFilledCircleRight();
        case 0xE0B6: return /*  */ blockElement<2>(size).halfFilledCircleLeft();
        case 0xE0BA: return /*  */ ld({ 0, 1 }, { 1, 0 });
        case 0xE0BC: return /*  */ ud({ 0, 1 }, { 1, 0 });
        case 0xE0BE: return /*  */ ud({ 0, 0 }, { 1, 1 });

        // PUA defines as introduced by FiraCode: https://github.com/tonsky/FiraCode/issues/1324
        case 0xEE00: return progressBar().left();
        case 0xEE01: return progressBar().middle();
        case 0xEE02: return progressBar().right();
        case 0xEE03: return progressBar().left().filled();
        case 0xEE04: return progressBar().middle().filled();
        case 0xEE05: return progressBar().right().filled();
    }
    // clang-format off

    return nullopt;
}

optional<atlas::Buffer> BoxDrawingRenderer::buildBoxElements(char32_t codepoint,
                                                             ImageSize size,
                                                             int lineThickness)
{
    if (!(codepoint >= 0x2500 && codepoint <= 0x257F))
        return nullopt;

    auto box = detail::BoxDrawingDefinitions[codepoint - 0x2500];

    auto const height = size.height;
    auto const width = size.width;
    auto const horizontalOffset = *height / 2;
    auto const verticalOffset = *width / 2;
    auto const lightThickness = (unsigned) lineThickness;
    auto const heavyThickness = (unsigned) lineThickness * 2;

    auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0x00);

    // catch all non-solid single-lines before the quad-render below

    if (auto const dashed = box.get_dashed_horizontal())
    {
        auto const [dashCount, thicknessMode] = *dashed;
        auto const thickness = thicknessMode == detail::Thickness::Heavy ? heavyThickness : lightThickness;

        auto const y0 = (*height / 2) - (unsigned) thickness / 2;
        auto const w = (unsigned) thickness;
        auto const p = unbox<double>(width) / static_cast<double>(dashCount * 2.0);

        auto x0 = round(p / 2.0);
        for ([[maybe_unused]] auto const _: iota(0u, dashCount))
        {
            auto const x0l = static_cast<int>(round(x0));
            for (auto const y: iota(y0, y0 + w))
                for (auto const x: iota(x0l, x0l + static_cast<int>(p)))
                    image[y * *width + unsigned(x)] = 0xFF;
            x0 += unbox<double>(width) / static_cast<double>(dashCount);
        }

        return image;
    }

    if (auto const dashed = box.get_dashed_vertical())
    {
        auto const [dashCount, thicknessMode] = *dashed;
        auto const thickness = thicknessMode == detail::Thickness::Heavy ? heavyThickness : lightThickness;

        auto const x0 = (*width / 2) - (unsigned) thickness / 2;
        auto const w = (unsigned) thickness;
        auto const p = unbox<double>(height) / static_cast<double>(dashCount * 2.0);

        auto y0 = round(p / 2.0);
        for ([[maybe_unused]] auto const i: iota(0u, dashCount))
        {
            auto const y0l = static_cast<unsigned>(round(y0));
            for (auto const y: iota(y0l, y0l + static_cast<unsigned>(p)))
                for (auto const x: iota(x0, x0 + w))
                    image[y * *width + unsigned(x)] = 0xFF;
            y0 += unbox<double>(height) / static_cast<double>(dashCount);
        }

        return image;
    }

    // left & right
    {
        auto const left = tuple { box.leftval, 0u, *width / 2, true };
        auto const right = tuple { box.rightval, *width / 2, *width, false };
        auto const offset = horizontalOffset;
        for (auto const& pq: { left, right })
        {
            auto const lm = get<0>(pq);
            auto const x0 = get<1>(pq);
            auto const x1 = get<2>(pq);
            switch (lm)
            {
                case detail::NoLine: break;
                case detail::Light: {
                    auto const y0 = offset - lightThickness / 2;
                    // BoxDrawingLog()("{}: line:{}, x:{}..{}, y:{}..{}",
                    //                 isFirst ? "left" : "right",
                    //                 to_stringview(lm),
                    //                 x0,
                    //                 x1 - 1,
                    //                 y0,
                    //                 y0 + lightThickness - 1,
                    //                 offset);
                    for (auto const yi: iota(0u, lightThickness))
                        for (auto const xi: iota(0u, x1 - x0))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;
                    break;
                }
                case detail::Double: {
                    auto y0 = offset - lightThickness / 2 - lightThickness;
                    for (auto const yi: iota(0u, lightThickness))
                        for (auto const xi: iota(0u, x1 - x0))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;

                    y0 = offset + lightThickness / 2;
                    for (auto const yi: iota(0u, lightThickness))
                        for (auto const xi: iota(0u, x1 - x0))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;
                    break;
                }
                case detail::Heavy: {
                    auto const y0 = offset - heavyThickness / 2;
                    for (auto const yi: iota(0u, heavyThickness))
                        for (auto const xi: iota(0u, x1 - x0))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;
                    break;
                }
                case detail::Light2:
                case detail::Light3:
                case detail::Light4:
                case detail::Heavy2:
                case detail::Heavy3:
                case detail::Heavy4:
                    // handled above
                    assert(false);
                    return nullopt;
            }
        }
    }

    // up & down
    // XXX same as (left & right) but with x/y and w/h coords swapped. can we reuse that?
    {
        auto const up = tuple { box.downval, 0u, *height / 2, true };
        auto const down = tuple { box.upval, *height / 2, *height, false };
        auto const offset = verticalOffset;
        for (auto const& pq: { up, down })
        {
            auto const lm = get<0>(pq);
            auto const y0 = get<1>(pq);
            auto const y1 = get<2>(pq);
            // auto const isFirst = get<3>(pq);
            switch (lm)
            {
                case detail::NoLine: break;
                case detail::Light: {
                    auto const x0 = offset - lightThickness / 2;
                    for (auto const yi: iota(0u, y1 - y0))
                        for (auto const xi: iota(0u, lightThickness))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;
                    break;
                }
                case detail::Double: {
                    auto x0 = offset - lightThickness / 2 - lightThickness;
                    for (auto const yi: iota(0u, y1 - y0))
                        for (auto const xi: iota(0u, lightThickness))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;

                    x0 = offset - lightThickness / 2 + lightThickness;
                    for (auto const yi: iota(0u, y1 - y0))
                        for (auto const xi: iota(0u, lightThickness))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;
                    break;
                }
                case detail::Heavy: {
                    auto const x0 = offset - (lightThickness * 3) / 2;
                    for (auto const yi: iota(0u, y1 - y0))
                        for (auto const xi: iota(0u, lightThickness * 3))
                            image[(y0 + yi) * *width + x0 + xi] = 0xFF;
                    break;
                }
                case detail::Light2:
                case detail::Light3:
                case detail::Light4:
                case detail::Heavy2:
                case detail::Heavy3:
                case detail::Heavy4: assert(false && "Cases handled above already."); return nullopt;
            }
        }
    }

    if (box.diagonalval != detail::NoDiagonal)
    {
        auto const a = height.as<double>() / width.as<double>();
        auto const aInv = 1.0 / a;
        using Diagonal = detail::Diagonal;
        if (unsigned(box.diagonalval) & unsigned(Diagonal::Forward))
        {
            for (auto const y: iota(0u, height.as<unsigned>()))
            {
                auto const x = int(double(y) * aInv);
                for (auto const xi: iota(-int(lineThickness) / 2, int(lineThickness) / 2))
                    image[y * *width + (unsigned) max(0, min(x + xi, unbox<int>(width) - 1))] = 0xFF;
            }
        }
        if (unsigned(box.diagonalval) & unsigned(Diagonal::Backward))
        {
            for (auto const y: iota(0u, height.as<unsigned>()))
            {
                auto const x = int(double(*height - y - 1) * aInv);
                for (auto const xi: iota(-int(lineThickness) / 2, int(lineThickness) / 2))
                    image[y * *width + (unsigned) max(0, min(x + xi, unbox<int>(width) - 1))] = 0xFF;
            }
        }
    }

    if (box.arcval != NoArc)
        detail::drawArc(image, size, lightThickness, box.arcval);

    boxDrawingLog()("BoxDrawing: build U+{:04X} ({})", static_cast<uint32_t>(codepoint), size);

    return image;
}

void BoxDrawingRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
