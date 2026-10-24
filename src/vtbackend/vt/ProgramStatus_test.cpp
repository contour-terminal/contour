// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/ProgramStatus.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace vtbackend;

namespace
{

/// The record @p payload decodes to, requiring one.
ProgramStatusRecord recordOf(std::string_view payload)
{
    auto const report = parseProgramStatus(payload);
    REQUIRE(report.has_value());
    REQUIRE(std::holds_alternative<ProgramStatusRecord>(*report));
    return std::get<ProgramStatusRecord>(*report);
}

/// Why @p payload is rejected whole, requiring that it is.
ProgramStatusRejection rejectionOf(std::string_view payload)
{
    auto const report = parseProgramStatus(payload);
    REQUIRE(!report.has_value());
    return report.error();
}

/// `state=idle:msg=<base64 of text>`.
std::string idleWithMessage(std::string_view text)
{
    return "state=idle:msg=" + core::base64::encode(text);
}

} // namespace

TEST_CASE("OSC 7501 decodes the terraform example", "[vtbackend][programstatus]")
{
    auto const report =
        vtbackend::parseProgramStatus("state=blocked:kind=permission:app=terraform:msg="
                                      "QXBwbHkgMyB0byBhZGQsIDEgdG8gY2hhbmdlLCAwIHRvIGRlc3Ryb3k/");
    REQUIRE(report.has_value());
    auto const& record = std::get<vtbackend::ProgramStatusRecord>(*report);
    CHECK(record.state == vtbackend::ProgramState::Blocked);
    CHECK(record.kind == vtbackend::BlockedKind::Permission);
    CHECK(record.app == "terraform");
    CHECK(record.message == "Apply 3 to add, 1 to change, 0 to destroy?");
    CHECK(record.id.empty());
    CHECK(!record.progress.has_value());
}

TEST_CASE("OSC 7501 keys that do not apply are absent", "[vtbackend][programstatus]")
{
    CHECK(!recordOf("state=working:progress=150").progress.has_value());
    CHECK(recordOf("state=working:progress=42").progress == uint8_t { 42 });
    CHECK(recordOf("state=blocked:progress=0").progress == uint8_t { 0 });
    CHECK(!recordOf("state=done:progress=50").progress.has_value());
    CHECK(!recordOf("state=working:progress=-1").progress.has_value());
    CHECK(recordOf("state=working:kind=auth").kind == BlockedKind::None);
    CHECK(recordOf("state=blocked:kind=auth").kind == BlockedKind::Auth);
    CHECK(recordOf("state=blocked:kind=bogus").kind == BlockedKind::None);
    CHECK(recordOf("state=idle:app=a b").app.empty());
    CHECK(recordOf("state=idle:app=a,b").app.empty()); // ',' is a value byte but not an app byte
    CHECK(recordOf(" state = idle ").state == ProgramState::Idle);
    CHECK(recordOf("garbage:state=done").state == ProgramState::Done);
    CHECK(recordOf("state=idle:state=done").state == ProgramState::Done);
    CHECK(recordOf("state=idle:color=red").state == ProgramState::Idle);
    CHECK(recordOf("=x:state=error").state == ProgramState::Error);
}

TEST_CASE("OSC 7501 rejects a report whole", "[vtbackend][programstatus]")
{
    CHECK(rejectionOf("") == ProgramStatusRejection::NoState);
    CHECK(rejectionOf("state=bogus") == ProgramStatusRejection::UnknownState);
    CHECK(rejectionOf("state=idle:id=a//b") == ProgramStatusRejection::BadId);
    CHECK(rejectionOf("state=idle:id=a/b/c/d/e/f/g/h/i") == ProgramStatusRejection::BadId);
    CHECK(recordOf("state=idle:id=a/b/c/d/e/f/g/h").id == "a/b/c/d/e/f/g/h");
    CHECK(rejectionOf("state=idle:id=" + std::string(33, 'x')) == ProgramStatusRejection::BadId);
    CHECK(rejectionOf("state=idle:id=a,b") == ProgramStatusRejection::BadId);
    CHECK(rejectionOf(idleWithMessage("a\nb")) == ProgramStatusRejection::ControlCharacter);
    CHECK(rejectionOf(idleWithMessage("a\x7f"
                                      "b"))
          == ProgramStatusRejection::ControlCharacter);
    CHECK(rejectionOf(idleWithMessage("a\xc2\x9b"
                                      "b"))
          == ProgramStatusRejection::ControlCharacter);
    CHECK(rejectionOf("state=idle:msg=" + std::string(2733, 'A')) == ProgramStatusRejection::TextTooLong);
    CHECK(rejectionOf("state=idle:title=" + std::string(257, 'A')) == ProgramStatusRejection::TextTooLong);
    // 2732 encoded bytes may carry 2049 decoded ones, one past the decoded limit.
    CHECK(rejectionOf("state=idle:msg=" + core::base64::encode(std::string(2049, 'x')))
          == ProgramStatusRejection::TextTooLong);
    CHECK(recordOf("state=idle:msg=" + core::base64::encode(std::string(2048, 'x'))).message.size() == 2048);
    CHECK(recordOf("state=idle:title=" + core::base64::encode(std::string(192, 'x'))).title.size() == 192);
    CHECK(recordOf("state=idle:msg=@@").message.empty());
    CHECK(recordOf("state=idle:msg=Zm9v!").message.empty());
    CHECK(rejectionOf("state=idle:msg=////") == ProgramStatusRejection::InvalidUtf8);
    CHECK(rejectionOf("state=idle:msg=Zm9v_") == ProgramStatusRejection::BadBase64);
    CHECK(rejectionOf("state=idle:msg=Zm9vY") == ProgramStatusRejection::BadBase64);
    CHECK(rejectionOf("state=idle:msg=Zm=9v") == ProgramStatusRejection::BadBase64);
    CHECK(rejectionOf("state=idle:" + std::string(4100, 'x')) == ProgramStatusRejection::TooLong);
    CHECK(rejectionOf("state=idle:abcdefghijklmnopq=1") == ProgramStatusRejection::KeyTooLong);
}

TEST_CASE("OSC 7501 base64 padding is optional", "[vtbackend][programstatus]")
{
    CHECK(recordOf("state=idle:msg=Zm9vYg==").message == "foob");
    CHECK(recordOf("state=idle:msg=Zm9vYg").message == "foob");
    CHECK(recordOf("state=idle:msg=Zm9vYmE").message == "fooba");
    CHECK(recordOf("state=idle:msg=" + core::base64::encode("caf\xc3\xa9")).message == "caf\xc3\xa9");
}

TEST_CASE("OSC 7501 query and clear", "[vtbackend][programstatus]")
{
    auto const query = parseProgramStatus("?");
    REQUIRE(query.has_value());
    CHECK(std::holds_alternative<ProgramStatusQuery>(*query));

    auto const clear = parseProgramStatus("state=clear:id=us-east");
    REQUIRE(clear.has_value());
    CHECK(std::get<ProgramStatusClear>(*clear) == ProgramStatusClear { .id = "us-east" });

    auto const clearAll = parseProgramStatus("state=clear");
    REQUIRE(clearAll.has_value());
    CHECK(std::get<ProgramStatusClear>(*clearAll).id.empty());
}

TEST_CASE("OSC 7501 detection reply lists the states and kinds", "[vtbackend][programstatus]")
{
    CHECK(programStatusDetectionReply()
          == "\033]7501;?:states=idle,working,done,blocked,error:kinds=permission,question,auth\033\\");
}
