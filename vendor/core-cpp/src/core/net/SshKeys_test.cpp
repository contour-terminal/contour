// SPDX-License-Identifier: Apache-2.0
//
// OpenSSH public keys, authorized_keys lines and SSHSIG verification.
//
// The Ed25519 and ECDSA cases are held against keys and signatures `ssh-keygen` made (the commands
// are in testdata/ssh/README.md), so the verifier agrees with OpenSSH rather than with itself. The
// `sk-` cases cannot be: ssh-keygen makes those only with a FIDO authenticator attached. They are
// held against `testing::SoftwareSecurityKey`, which signs as PROTOCOL.u2f says an authenticator
// does, with OpenSSL driven by hand.
#include <core/Base64.hpp>
#include <core/net/SshKeys.hpp>
#include <core/net/detail/SshWire.hpp>
#include <core/net/testing/SoftwareSecurityKey.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using core::net::SshKeyError;
using core::net::SshKeyType;
using core::net::SshPublicKey;
using core::net::SshSigError;
using core::net::detail::appendSshString;
using core::net::detail::appendSshUint32;
using core::net::detail::SshWireReader;
using core::net::testing::SoftwareSecurityKey;
using core::net::testing::UserPresence;

namespace
{

constexpr auto Namespace = std::string_view { "contour-auth" };

/// The whole of a fixture file under testdata/ssh/.
std::string fixture(std::string_view name)
{
    auto const path = std::string { CORE_CPP_SSH_TESTDATA_DIR } + "/" + std::string { name };
    auto file = std::ifstream { path, std::ios::binary };
    REQUIRE(file.good());
    auto text = std::ostringstream {};
    text << file.rdbuf();
    return text.str();
}

std::span<std::byte const> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span { text.data(), text.size() });
}

std::string textOf(std::span<std::byte const> bytes)
{
    return { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
}

/// The raw blob inside an armored signature: the base64 between the BEGIN and END lines.
std::string unarmor(std::string_view armored)
{
    auto body = std::string {};
    for (auto const line: armored | std::views::split('\n'))
    {
        auto const text = std::string_view { line.begin(), line.end() };
        if (!text.empty() && !text.starts_with("-----"))
            body += text;
    }
    return core::base64::decode(body);
}

SshPublicKey keyFixture(std::string_view name)
{
    auto key = core::net::parseSshPublicKey(fixture(name));
    REQUIRE(key.has_value());
    return *key;
}

/// The fields of a raw SSHSIG blob, as views into it.
struct SshsigFields
{
    std::span<std::byte const> publicKey;
    std::span<std::byte const> nameSpace;
    std::span<std::byte const> reserved;
    std::span<std::byte const> hashAlgorithm;
    std::span<std::byte const> signature;
};

/// The fields of @p raw, as views into it: a temporary would leave them dangling, hence the deleted
/// overload.
SshsigFields fieldsOf(std::string const& raw)
{
    auto reader = SshWireReader { bytesOf(raw) };
    REQUIRE(reader.raw(6).has_value());
    REQUIRE(reader.uint32() == 1U);
    auto const publicKey = reader.string();
    auto const nameSpace = reader.string();
    auto const reserved = reader.string();
    auto const hashAlgorithm = reader.string();
    auto const signature = reader.string();
    REQUIRE((publicKey && nameSpace && reserved && hashAlgorithm && signature));
    REQUIRE(reader.atEnd());
    return { *publicKey, *nameSpace, *reserved, *hashAlgorithm, *signature };
}

SshsigFields fieldsOf(std::string&& raw) = delete;

/// A raw SSHSIG blob assembled from parts, for the cases that must forge one.
std::string sshsigOf(SshsigFields const& fields)
{
    auto out = std::vector<std::byte> {};
    for (auto const c: std::string_view { "SSHSIG" })
        out.push_back(static_cast<std::byte>(c));
    appendSshUint32(out, 1);
    appendSshString(out, fields.publicKey);
    appendSshString(out, fields.nameSpace);
    appendSshString(out, fields.reserved);
    appendSshString(out, fields.hashAlgorithm);
    appendSshString(out, fields.signature);
    return textOf(out);
}

std::string base64Of(std::span<std::byte const> bytes)
{
    return core::base64::encode(textOf(bytes));
}

} // namespace

// {{{ public keys

TEST_CASE("parseSshPublicKey reads ssh-keygen's .pub files", "[net][ssh]")
{
    auto const [file, type, comment] = GENERATE(table<std::string_view, SshKeyType, std::string_view>({
        { "ed25519.pub", SshKeyType::Ed25519, "ed25519-fixture" },
        { "ecdsa-p256.pub", SshKeyType::EcdsaP256, "ecdsa-p256-fixture" },
    }));
    CAPTURE(file);
    auto const text = fixture(file);
    auto const key = core::net::parseSshPublicKey(text);
    REQUIRE(key.has_value());
    CHECK(key->type == type);
    CHECK(key->comment == comment);
    auto const middle = std::string_view { text }.substr(text.find(' ') + 1);
    CHECK(base64Of(key->blob) == middle.substr(0, middle.find(' ')));
}

TEST_CASE("parseSshPublicKey reads security keys", "[net][ssh]")
{
    auto const type = GENERATE(SshKeyType::SkEd25519, SshKeyType::SkEcdsaP256);
    auto const securityKey = SoftwareSecurityKey::generate(type);
    REQUIRE(securityKey.has_value());
    auto const key = core::net::parseSshPublicKey((*securityKey)->publicKeyLine("my yubikey") + "\n");
    REQUIRE(key.has_value());
    CHECK(key->type == type);
    CHECK(key->comment == "my yubikey");
}

TEST_CASE("parseSshPublicKey keeps a comment with spaces and no comment at all", "[net][ssh]")
{
    auto const text = fixture("ed25519.pub");
    auto const keyAndType = text.substr(0, text.rfind(' '));
    auto const bare = core::net::parseSshPublicKey(keyAndType);
    REQUIRE(bare.has_value());
    CHECK(bare->comment.empty());
    auto const spaced = core::net::parseSshPublicKey("  " + keyAndType + "\tme at  home \r\n");
    REQUIRE(spaced.has_value());
    CHECK(spaced->comment == "me at  home");
    CHECK(spaced->blob == bare->blob);
}

TEST_CASE("parseSshPublicKey refuses RSA and unknown types", "[net][ssh]")
{
    auto const line = GENERATE(std::string_view { "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC7 rsa" },
                               std::string_view { "ssh-dss AAAAB3NzaC1kc3M= dsa" },
                               std::string_view { "no-such-type AAAA" });
    CAPTURE(line);
    auto const key = core::net::parseSshPublicKey(line);
    REQUIRE_FALSE(key.has_value());
    CHECK(key.error() == SshKeyError::UnsupportedType);
}

TEST_CASE("parseSshPublicKey refuses malformed keys", "[net][ssh]")
{
    auto const ed25519 = fixture("ed25519.pub");
    auto const ecdsa = fixture("ecdsa-p256.pub");
    auto const ecdsaBase64 = ecdsa.substr(ecdsa.find(' ') + 1, ecdsa.rfind(' ') - ecdsa.find(' ') - 1);
    auto const ed25519Base64 =
        ed25519.substr(ed25519.find(' ') + 1, ed25519.rfind(' ') - ed25519.find(' ') - 1);
    // The 104-byte ECDSA blob ends in one '=': its last digit carries two padding bits.
    REQUIRE((ecdsaBase64.ends_with('=') && !ecdsaBase64.ends_with("==")));
    auto const withPaddingBitSet = [](std::string text) {
        constexpr auto Alphabet =
            std::string_view { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };
        auto& digit = text[text.size() - 2];
        digit = Alphabet[Alphabet.find(digit) ^ 1U];
        return text;
    };
    auto const line = GENERATE_COPY(
        std::string {},
        std::string { "ssh-ed25519" },
        std::string { "ssh-ed25519 !!!notbase64 comment" },
        std::string { "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAPPsKR22SKf8kiPM0HVGK+R933e5unsB8NfOolV7LcR=" },
        // Padding bits that are not zero: base64 that decodes, but not as ssh-keygen would write it.
        "ecdsa-sha2-nistp256 " + withPaddingBitSet(ecdsaBase64),
        // An ECDSA blob under the Ed25519 name: the type inside the blob must be the one outside.
        "ssh-ed25519 " + ecdsaBase64,
        "ecdsa-sha2-nistp256 " + ed25519Base64);
    CAPTURE(line);
    auto const key = core::net::parseSshPublicKey(line);
    REQUIRE_FALSE(key.has_value());
    CHECK(key.error() == SshKeyError::Malformed);
}

TEST_CASE("parseSshPublicKey refuses truncated, oversized and trailing fields", "[net][ssh]")
{
    for (auto const* const file: { "ed25519.pub", "ecdsa-p256.pub" })
    {
        auto const blob = keyFixture(file).blob;
        auto const typeName = std::string { file } == "ed25519.pub" ? "ssh-ed25519 " : "ecdsa-sha2-nistp256 ";
        auto const parse = [&](std::span<std::byte const> bytes) {
            return core::net::parseSshPublicKey(typeName + base64Of(bytes));
        };
        REQUIRE(parse(blob).has_value());

        // Every proper prefix is short of some field.
        for (auto const length: std::views::iota(std::size_t { 0 }, blob.size()))
        {
            CAPTURE(file, length);
            auto const key = parse(std::span { blob }.first(length));
            REQUIRE_FALSE(key.has_value());
            CHECK(key.error() == SshKeyError::Malformed);
        }

        // One byte too many.
        auto longer = blob;
        longer.push_back(std::byte { 0 });
        CHECK(parse(longer).error() == SshKeyError::Malformed);

        // The last field's length claims far more than is there, up to the largest uint32.
        for (auto const claimed: { std::uint32_t { 0x7FFF'FFFF }, std::uint32_t { 0xFFFF'FFFF } })
        {
            auto oversized = blob;
            auto const lastLength = oversized.size() - (oversized.size() == 51 ? 32 : 65) - 4;
            for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 4 }))
                oversized[lastLength + index] =
                    static_cast<std::byte>((claimed >> (24 - (8 * index))) & 0xFFU);
            CAPTURE(file, claimed);
            CHECK(parse(oversized).error() == SshKeyError::Malformed);
        }
    }
}

TEST_CASE("parseSshPublicKey refuses an ECDSA point off the curve", "[net][ssh]")
{
    auto blob = keyFixture("ecdsa-p256.pub").blob;
    blob.back() ^= std::byte { 0x01 };
    auto const key = core::net::parseSshPublicKey("ecdsa-sha2-nistp256 " + base64Of(blob));
    REQUIRE_FALSE(key.has_value());
    CHECK(key.error() == SshKeyError::Malformed);
}

TEST_CASE("sshFingerprintOf matches ssh-keygen -lf", "[net][ssh]")
{
    auto const name = GENERATE(std::string_view { "ed25519" }, std::string_view { "ecdsa-p256" });
    CAPTURE(name);
    auto const listed = fixture(std::string { name } + ".fingerprint"); // "256 SHA256:... comment (TYPE)"
    auto const fingerprint =
        listed.substr(listed.find(' ') + 1, listed.find(' ', listed.find(' ') + 1) - listed.find(' ') - 1);
    CHECK(fingerprint.starts_with("SHA256:"));
    CHECK(core::net::sshFingerprintOf(keyFixture(std::string { name } + ".pub")) == fingerprint);
}

// }}}
// {{{ authorized_keys

TEST_CASE("parseAuthorizedKeysLine splits options outside quotes", "[net][ssh]")
{
    auto const keyLine = fixture("ed25519.pub");
    auto const line = core::net::parseAuthorizedKeysLine(R"(grants="read,screen",no-pty )" + keyLine);
    REQUIRE(line.has_value());
    REQUIRE(line->has_value());
    auto const& entry = **line;
    CHECK(entry.options == std::vector<std::string> { R"(grants="read,screen")", "no-pty" });
    CHECK(entry.key == keyFixture("ed25519.pub"));
}

TEST_CASE("parseAuthorizedKeysLine keeps a quoted space and an escaped quote in an option", "[net][ssh]")
{
    auto const line =
        core::net::parseAuthorizedKeysLine(R"(command="echo \"a, b\" c",restrict )" + fixture("ed25519.pub"));
    REQUIRE(line.has_value());
    REQUIRE(line->has_value());
    CHECK((*line)->options == std::vector<std::string> { R"(command="echo \"a, b\" c")", "restrict" });
}

TEST_CASE("parseAuthorizedKeysLine reads a key with no options", "[net][ssh]")
{
    auto const line = core::net::parseAuthorizedKeysLine(fixture("ecdsa-p256.pub"));
    REQUIRE(line.has_value());
    REQUIRE(line->has_value());
    CHECK((*line)->options.empty());
    CHECK((*line)->key.type == SshKeyType::EcdsaP256);
}

TEST_CASE("parseAuthorizedKeysLine skips blank and comment lines", "[net][ssh]")
{
    auto const text = GENERATE(std::string_view {},
                               std::string_view { "\n" },
                               std::string_view { "   \t\r\n" },
                               std::string_view { "# a comment" },
                               std::string_view { "   # an indented comment\n" });
    auto const line = core::net::parseAuthorizedKeysLine(text);
    REQUIRE(line.has_value());
    CHECK_FALSE(line->has_value());
}

TEST_CASE("parseAuthorizedKeysLine refuses what is not a key line", "[net][ssh]")
{
    auto const keyLine = fixture("ed25519.pub");
    SECTION("an unterminated quote")
    {
        CHECK(core::net::parseAuthorizedKeysLine(R"(command="echo )" + keyLine).error()
              == SshKeyError::Malformed);
    }
    SECTION("options and no key")
    {
        CHECK(core::net::parseAuthorizedKeysLine("no-pty,restrict").error() == SshKeyError::Malformed);
        CHECK(core::net::parseAuthorizedKeysLine("no-pty  ").error() == SshKeyError::Malformed);
    }
    SECTION("an RSA key, with and without options")
    {
        CHECK(core::net::parseAuthorizedKeysLine("ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC7 me").error()
              == SshKeyError::UnsupportedType);
        CHECK(core::net::parseAuthorizedKeysLine("no-pty ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC7 me").error()
              == SshKeyError::UnsupportedType);
    }
    SECTION("a broken key after options")
    {
        CHECK(core::net::parseAuthorizedKeysLine("no-pty ssh-ed25519 AAAA!").error()
              == SshKeyError::Malformed);
    }
}

// }}}
// {{{ SSHSIG

TEST_CASE("verifySshSignature accepts ssh-keygen -Y sign", "[net][ssh][sshsig]")
{
    auto const [keyFile, sigFile] = GENERATE(table<std::string_view, std::string_view>({
        { "ed25519.pub", "ed25519.sig" },
        { "ecdsa-p256.pub", "ecdsa-p256.sig" },
    }));
    CAPTURE(sigFile);
    auto const key = keyFixture(keyFile);
    auto const message = fixture("message.txt");
    auto const armored = fixture(sigFile);

    auto const verified = core::net::verifySshSignature(key, Namespace, bytesOf(message), armored);
    CHECK(verified.has_value());
    auto const raw = core::net::verifySshSignature(key, Namespace, bytesOf(message), unarmor(armored));
    CHECK(raw.has_value());
}

TEST_CASE("verifySshSignature refuses a changed message", "[net][ssh][sshsig]")
{
    auto const name = GENERATE(std::string_view { "ed25519" }, std::string_view { "ecdsa-p256" });
    CAPTURE(name);
    auto message = fixture("message.txt");
    message[3] = static_cast<char>(message[3] ^ 0x01);
    auto const verified = core::net::verifySshSignature(keyFixture(std::string { name } + ".pub"),
                                                        Namespace,
                                                        bytesOf(message),
                                                        fixture(std::string { name } + ".sig"));
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::BadSignature);
}

TEST_CASE("verifySshSignature refuses another namespace", "[net][ssh][sshsig]")
{
    auto const verified = core::net::verifySshSignature(
        keyFixture("ed25519.pub"), "other", bytesOf(fixture("message.txt")), fixture("ed25519.sig"));
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::WrongNamespace);
}

TEST_CASE("verifySshSignature refuses a signature by another key", "[net][ssh][sshsig]")
{
    auto const keyFile = GENERATE(std::string_view { "ed25519.pub" }, std::string_view { "ecdsa-p256.pub" });
    auto const verified = core::net::verifySshSignature(
        keyFixture(keyFile), Namespace, bytesOf(fixture("message.txt")), fixture("ed25519-other.sig"));
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::KeyMismatch);
}

TEST_CASE("verifySshSignature refuses ssh-keygen's sha256 signature", "[net][ssh][sshsig]")
{
    // A genuine signature, made with -O hashalg=sha256: refused, because only sha512 is accepted.
    auto const verified = core::net::verifySshSignature(
        keyFixture("ed25519.pub"), Namespace, bytesOf(fixture("message.txt")), fixture("ed25519-sha256.sig"));
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::UnsupportedHash);
}

TEST_CASE("verifySshSignature refuses a reserved field that is not empty", "[net][ssh][sshsig]")
{
    // Malformed, not BadSignature: the envelope is refused before any signature is checked.
    auto const raw = unarmor(fixture("ed25519.sig"));
    auto fields = fieldsOf(raw);
    fields.reserved = bytesOf("x");
    auto const verified = core::net::verifySshSignature(
        keyFixture("ed25519.pub"), Namespace, bytesOf(fixture("message.txt")), sshsigOf(fields));
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::Malformed);
}

TEST_CASE("verifySshSignature refuses a hash other than sha512", "[net][ssh][sshsig]")
{
    auto const raw = unarmor(fixture("ed25519.sig"));
    auto fields = fieldsOf(raw);
    auto const algorithm = GENERATE(std::string_view { "sha256" },
                                    std::string_view { "sha384" },
                                    std::string_view { "sha1" },
                                    std::string_view {});
    fields.hashAlgorithm = bytesOf(algorithm);
    auto const verified = core::net::verifySshSignature(
        keyFixture("ed25519.pub"), Namespace, bytesOf(fixture("message.txt")), sshsigOf(fields));
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::UnsupportedHash);
}

TEST_CASE("verifySshSignature refuses truncated, oversized and trailing fields", "[net][ssh][sshsig]")
{
    auto const key = keyFixture("ecdsa-p256.pub");
    auto const message = fixture("message.txt");
    auto const raw = unarmor(fixture("ecdsa-p256.sig"));
    REQUIRE(core::net::verifySshSignature(key, Namespace, bytesOf(message), raw).has_value());

    for (auto const length: std::views::iota(std::size_t { 0 }, raw.size()))
    {
        CAPTURE(length);
        auto const verified =
            core::net::verifySshSignature(key, Namespace, bytesOf(message), raw.substr(0, length));
        REQUIRE_FALSE(verified.has_value());
        CHECK(verified.error() == SshSigError::Malformed);
    }
    CHECK(core::net::verifySshSignature(key, Namespace, bytesOf(message), raw + '\0').error()
          == SshSigError::Malformed);

    // Every length field in turn claims the largest uint32, and then one more byte than is there.
    auto offsets = std::vector<std::size_t> {};
    auto at = std::size_t { 10 };
    while (at < raw.size())
    {
        offsets.push_back(at);
        auto fieldReader = SshWireReader { bytesOf(raw).subspan(at) };
        auto const length = fieldReader.uint32();
        REQUIRE(length.has_value());
        at += 4 + *length;
    }
    REQUIRE(offsets.size() == 5);
    for (auto const offset: offsets)
    {
        auto const present = raw.size() - offset - 4;
        for (auto const claimed: { std::uint32_t { 0xFFFF'FFFF }, static_cast<std::uint32_t>(present + 1) })
        {
            CAPTURE(offset, claimed);
            auto forged = raw;
            for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 4 }))
                forged[offset + index] = static_cast<char>((claimed >> (24 - (8 * index))) & 0xFFU);
            auto const verified = core::net::verifySshSignature(key, Namespace, bytesOf(message), forged);
            REQUIRE_FALSE(verified.has_value());
            CHECK(verified.error() == SshSigError::Malformed);
        }
    }
}

TEST_CASE("verifySshSignature refuses a malformed envelope", "[net][ssh][sshsig]")
{
    auto const key = keyFixture("ed25519.pub");
    auto const message = fixture("message.txt");
    auto const raw = unarmor(fixture("ed25519.sig"));
    auto const refuse = [&](std::string const& sshsig) {
        auto const verified = core::net::verifySshSignature(key, Namespace, bytesOf(message), sshsig);
        REQUIRE_FALSE(verified.has_value());
        CHECK(verified.error() == SshSigError::Malformed);
    };
    SECTION("the wrong magic")
    {
        refuse("SSHSIX" + raw.substr(6));
    }
    SECTION("another version")
    {
        auto forged = raw;
        forged[9] = '\2';
        refuse(forged);
    }
    SECTION("armor around broken base64")
    {
        refuse("-----BEGIN SSH SIGNATURE-----\nU1NIU0lH!!!\n-----END SSH SIGNATURE-----\n");
    }
    SECTION("armor with no end line")
    {
        refuse("-----BEGIN SSH SIGNATURE-----\n" + core::base64::encode(raw) + "\n");
    }
    SECTION("empty")
    {
        refuse({});
    }
    SECTION("armor lines and nothing between")
    {
        refuse("-----BEGIN SSH SIGNATURE----------END SSH SIGNATURE-----");
        refuse("-----BEGIN SSH SIGNATURE-----\n-----END SSH SIGNATURE-----\n");
    }
    SECTION("armor shorter than its two lines")
    {
        // Starts with BEGIN and ends with "-----": the size guard must refuse it before the body
        // is cut, whatever the END line's spelling shares with BEGIN's.
        refuse("-----BEGIN SSH SIGNATURE-----");
        refuse("-----BEGIN SSH SIGNATURE-----END SSH SIGNATURE-----");
    }
}

TEST_CASE("verifySshSignature refuses an ECDSA mpint that is not minimal or is negative",
          "[net][ssh][sshsig]")
{
    auto const key = keyFixture("ecdsa-p256.pub");
    auto const message = fixture("message.txt");
    auto const raw = unarmor(fixture("ecdsa-p256.sig"));
    auto fields = fieldsOf(raw);

    // signature = string "ecdsa-sha2-nistp256" ‖ string (mpint r ‖ mpint s)
    auto outer = SshWireReader { fields.signature };
    auto const sigType = outer.string();
    auto const inner = outer.string();
    REQUIRE((sigType && inner));
    auto values = SshWireReader { *inner };
    auto const r = values.string();
    auto const s = values.string();
    REQUIRE((r && s));

    auto const rebuild = [&](std::vector<std::byte> const& newR) {
        auto mpints = std::vector<std::byte> {};
        appendSshString(mpints, newR);
        appendSshString(mpints, *s);
        auto signature = std::vector<std::byte> {};
        appendSshString(signature, *sigType);
        appendSshString(signature, mpints);
        auto forged = fields;
        forged.signature = signature;
        return core::net::verifySshSignature(key, Namespace, bytesOf(message), sshsigOf(forged));
    };

    REQUIRE(rebuild({ r->begin(), r->end() }).has_value());

    // A sign byte of zero in front of a value whose top bit is already clear (or of another zero).
    auto padded = std::vector<std::byte> { std::byte { 0 } };
    padded.insert(padded.end(), r->begin(), r->end());
    CHECK(rebuild(padded).error() == SshSigError::Malformed);

    auto negative = std::vector<std::byte> { std::byte { 0x80 } };
    negative.insert(negative.end(), r->begin(), r->end());
    CHECK(rebuild(negative).error() == SshSigError::Malformed);
}

TEST_CASE("assembleSshsig round-trips an agent's signature", "[net][ssh][sshsig]")
{
    auto const name = GENERATE(std::string_view { "ed25519" }, std::string_view { "ecdsa-p256" });
    CAPTURE(name);
    auto const key = keyFixture(std::string { name } + ".pub");
    auto const message = fixture("message.txt");
    auto const raw = unarmor(fixture(std::string { name } + ".sig"));
    auto const fields = fieldsOf(raw);

    auto const assembled = core::net::assembleSshsig(key, Namespace, fields.signature);
    CHECK(assembled == raw);
    CHECK(core::net::verifySshSignature(key, Namespace, bytesOf(message), assembled).has_value());
}

TEST_CASE("sshsigSignedData is PROTOCOL.sshsig's signed data with sha512", "[net][ssh][sshsig]")
{
    auto const signedData = core::net::sshsigSignedData(Namespace, bytesOf("abc"));
    auto reader = SshWireReader { signedData };
    auto const magic = reader.raw(6);
    REQUIRE(magic.has_value());
    CHECK(textOf(*magic) == "SSHSIG");
    CHECK(textOf(reader.string().value()) == Namespace);
    CHECK(reader.string().value().empty());
    CHECK(textOf(reader.string().value()) == "sha512");
    auto const digest = reader.string().value();
    // SHA-512("abc"), FIPS 180-2 appendix C.1.
    CHECK(base64Of(digest)
          == "3a81oZNherrMQXNJriBBMRLm+k6JqX6iCp7u5ktV05ohkpkqJ0/BqDa6PCOj/uu9RU1EI2Q86A4qmslPpUyknw==");
    CHECK(reader.atEnd());
}

TEST_CASE("verifySshSignature verifies security-key signatures", "[net][ssh][sshsig][sk]")
{
    auto const type = GENERATE(SshKeyType::SkEd25519, SshKeyType::SkEcdsaP256);
    CAPTURE(type);
    auto const securityKey = SoftwareSecurityKey::generate(type);
    REQUIRE(securityKey.has_value());
    auto const key = core::net::parseSshPublicKey((*securityKey)->publicKeyLine("sk"));
    REQUIRE(key.has_value());
    auto const message = std::string { "contour-auth-v1 challenge" };
    auto const signedData = core::net::sshsigSignedData(Namespace, bytesOf(message));

    SECTION("with the user present")
    {
        auto const signature = (*securityKey)->sign(signedData, UserPresence::Present, 7);
        REQUIRE(signature.has_value());
        auto const sshsig = core::net::assembleSshsig(*key, Namespace, *signature);
        CHECK(core::net::verifySshSignature(*key, Namespace, bytesOf(message), sshsig).has_value());

        auto changed = message;
        changed.back() = '!';
        CHECK(core::net::verifySshSignature(*key, Namespace, bytesOf(changed), sshsig).error()
              == SshSigError::BadSignature);
    }
    SECTION("without the user present")
    {
        auto const signature = (*securityKey)->sign(signedData, UserPresence::Absent, 7);
        REQUIRE(signature.has_value());
        auto const sshsig = core::net::assembleSshsig(*key, Namespace, *signature);
        CHECK(core::net::verifySshSignature(*key, Namespace, bytesOf(message), sshsig).error()
              == SshSigError::UserNotPresent);
    }
    SECTION("without the user present and a bad signature")
    {
        // BadSignature, not UserNotPresent: the flag is read only from a signature that verified.
        auto const signature = (*securityKey)->sign(signedData, UserPresence::Absent, 7);
        REQUIRE(signature.has_value());
        auto const sshsig = core::net::assembleSshsig(*key, Namespace, *signature);
        auto changed = message;
        changed.back() = '!';
        CHECK(core::net::verifySshSignature(*key, Namespace, bytesOf(changed), sshsig).error()
              == SshSigError::BadSignature);
    }
    SECTION("with the flags edited after signing")
    {
        // Setting the flag on a signature made without it must not pass: the flags are signed.
        auto signature = (*securityKey)->sign(signedData, UserPresence::Absent, 7);
        REQUIRE(signature.has_value());
        (*signature)[signature->size() - 5] |= std::byte { 0x01 };
        auto const sshsig = core::net::assembleSshsig(*key, Namespace, *signature);
        CHECK(core::net::verifySshSignature(*key, Namespace, bytesOf(message), sshsig).error()
              == SshSigError::BadSignature);
    }
    SECTION("bound to another application")
    {
        // The application is part of the public key, so another one is another key.
        auto const other = SoftwareSecurityKey::generate(type, "ssh:other");
        REQUIRE(other.has_value());
        auto const otherKey = core::net::parseSshPublicKey((*other)->publicKeyLine("other"));
        REQUIRE(otherKey.has_value());
        auto const signature = (*other)->sign(signedData, UserPresence::Present, 1);
        REQUIRE(signature.has_value());
        auto const sshsig = core::net::assembleSshsig(*otherKey, Namespace, *signature);
        CHECK(core::net::verifySshSignature(*otherKey, Namespace, bytesOf(message), sshsig).has_value());
        CHECK(core::net::verifySshSignature(*key, Namespace, bytesOf(message), sshsig).error()
              == SshSigError::KeyMismatch);
    }
}

TEST_CASE("verifySshSignature refuses a signature of another algorithm than the key's", "[net][ssh][sshsig]")
{
    // An Ed25519 signature presented for a security key: the blob embeds the right key, but the
    // signature's type is not the key's.
    auto const securityKey = SoftwareSecurityKey::generate(SshKeyType::SkEd25519);
    REQUIRE(securityKey.has_value());
    auto const key = core::net::parseSshPublicKey((*securityKey)->publicKeyLine("sk"));
    REQUIRE(key.has_value());
    auto const raw = unarmor(fixture("ed25519.sig"));
    auto const fields = fieldsOf(raw);
    auto const sshsig = core::net::assembleSshsig(*key, Namespace, fields.signature);
    auto const verified =
        core::net::verifySshSignature(*key, Namespace, bytesOf(fixture("message.txt")), sshsig);
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::Malformed);
}

TEST_CASE("verifySshSignature refuses a valid signature under another algorithm's name", "[net][ssh][sshsig]")
{
    // ssh-keygen's Ed25519 signature, whose bytes verify, relabelled as an sk-ssh-ed25519 one and
    // otherwise left as it is (no flags or counter, so it still parses as an Ed25519 signature).
    // Only the check that the signature's name is the key's own can refuse it.
    auto const key = keyFixture("ed25519.pub");
    auto const message = fixture("message.txt");
    auto const raw = unarmor(fixture("ed25519.sig"));
    auto const fields = fieldsOf(raw);
    auto inner = SshWireReader { fields.signature };
    auto const name = inner.string();
    auto const value = inner.string();
    REQUIRE((name && value && inner.atEnd()));

    auto const relabel = [&](std::string_view newName) {
        auto signature = std::vector<std::byte> {};
        appendSshString(signature, newName);
        appendSshString(signature, *value);
        return core::net::verifySshSignature(
            key, Namespace, bytesOf(message), core::net::assembleSshsig(key, Namespace, signature));
    };
    REQUIRE(relabel("ssh-ed25519").has_value()); // the control: the same bytes under their own name
    auto const verified = relabel("sk-ssh-ed25519@openssh.com");
    REQUIRE_FALSE(verified.has_value());
    CHECK(verified.error() == SshSigError::Malformed);
}

TEST_CASE("SshWireReader never reads past its end", "[net][ssh]")
{
    auto const bytes = std::vector<std::byte> { std::byte { 0 }, std::byte { 0 },   std::byte { 0 },
                                                std::byte { 3 }, std::byte { 'a' }, std::byte { 'b' } };
    auto reader = SshWireReader { bytes };
    CHECK_FALSE(reader.string().has_value());
    // A failed string consumes nothing: its length is still there to read.
    CHECK(reader.uint32() == 3U);
    CHECK_FALSE(reader.raw(3).has_value());
    CHECK(reader.byte() == std::uint8_t { 'a' });
    CHECK(reader.byte() == std::uint8_t { 'b' });
    CHECK_FALSE(reader.byte().has_value());
    CHECK_FALSE(reader.uint32().has_value());
    CHECK(reader.atEnd());
}

// }}}

TEST_CASE("parseSshPublicKeyBlob takes the blob of a .pub line and nothing else")
{
    auto const line = core::net::parseSshPublicKey(fixture("ed25519.pub"));
    REQUIRE(line.has_value());

    auto const blob = core::net::parseSshPublicKeyBlob(line->blob);
    REQUIRE(blob.has_value());
    CHECK(blob->type == SshKeyType::Ed25519);
    CHECK(blob->blob == line->blob);
    CHECK(blob->comment.empty());

    auto trailing = line->blob;
    trailing.push_back(std::byte { 0 });
    CHECK(core::net::parseSshPublicKeyBlob(trailing).error() == SshKeyError::Malformed);

    auto rsa = std::vector<std::byte> {};
    appendSshString(rsa, std::string_view { "ssh-rsa" });
    CHECK(core::net::parseSshPublicKeyBlob(rsa).error() == SshKeyError::UnsupportedType);
}
