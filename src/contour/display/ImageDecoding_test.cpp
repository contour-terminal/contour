// SPDX-License-Identifier: Apache-2.0
//
// The QImage-backed decoder every terminal of a GUI build is given, a daemon's hosted ones included.

#include <contour/display/ImageDecoding.hpp>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtGui/QColor>
#include <QtGui/QImage>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>

TEST_CASE("decodeImage turns a PNG into RGBA pixels and declines everything else", "[image]")
{
    auto source = QImage { 4, 3, QImage::Format_RGBA8888 };
    source.fill(QColor(255, 0, 0, 255));
    auto png = QByteArray {};
    {
        auto buffer = QBuffer { &png };
        buffer.open(QIODevice::WriteOnly);
        REQUIRE(source.save(&buffer, "PNG"));
    }
    auto const bytes = std::span<uint8_t const> { reinterpret_cast<uint8_t const*>(png.constData()),
                                                  static_cast<size_t>(png.size()) };

    auto size = vtbackend::ImageSize {};
    auto const pixels = contour::display::decodeImage(vtbackend::ImageFormat::PNG, bytes, size);
    REQUIRE(pixels.has_value());
    CHECK(size.width == vtbackend::Width(4));
    CHECK(size.height == vtbackend::Height(3));
    REQUIRE(pixels->size() == std::size_t { 4 } * 3 * 4);
    CHECK((*pixels)[0] == 255);
    CHECK((*pixels)[1] == 0);
    CHECK((*pixels)[3] == 255);

    auto ignored = vtbackend::ImageSize {};
    // The PNG's bytes named as another format are not decoded as one.
    CHECK_FALSE(contour::display::decodeImage(vtbackend::ImageFormat::RGBA, bytes, ignored).has_value());
    auto const garbage = std::array<uint8_t, 4> { 0x00, 0x01, 0x02, 0x03 };
    CHECK_FALSE(contour::display::decodeImage(vtbackend::ImageFormat::PNG, garbage, ignored).has_value());
}

TEST_CASE("decodeImage reads whatever file an application hands over as PNG", "[image]")
{
    // iTerm2 inline images (`imgcat photo.jpg`) and GIP pass any image file as ImageFormat::PNG, so the
    // decoder must recognise the file itself: a BMP, built into Qt like PNG, stands for JPEG and GIF here.
    auto source = QImage { 2, 2, QImage::Format_RGB32 };
    source.fill(QColor(0, 0, 255));
    auto bmp = QByteArray {};
    {
        auto buffer = QBuffer { &bmp };
        buffer.open(QIODevice::WriteOnly);
        REQUIRE(source.save(&buffer, "BMP"));
    }
    auto const bytes = std::span<uint8_t const> { reinterpret_cast<uint8_t const*>(bmp.constData()),
                                                  static_cast<size_t>(bmp.size()) };

    auto size = vtbackend::ImageSize {};
    auto const pixels = contour::display::decodeImage(vtbackend::ImageFormat::PNG, bytes, size);
    REQUIRE(pixels.has_value());
    CHECK(size == vtbackend::ImageSize { vtbackend::Width(2), vtbackend::Height(2) });
    CHECK((*pixels)[2] == 255);
}
