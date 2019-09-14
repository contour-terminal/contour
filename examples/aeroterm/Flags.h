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
#pragma once

#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace util {

class Flags {
  public:
    enum class FlagType {
        String,
        Number,
        Float,
        Bool,
    };

    // FlagPassingStyle
    enum FlagStyle { ShortSwitch, LongSwitch, ShortWithValue, LongWithValue, UnnamedParameter };

    enum class ErrorCode {
        TypeMismatch,
        UnknownOption,
        MissingOption,
        MissingOptionValue,
        NotFound,
    };

    class Error : public std::runtime_error {
      public:
        Error(ErrorCode code, std::string arg);

        ErrorCode code() const noexcept { return code_; }
        const std::string& arg() const noexcept { return arg_; }

      private:
        ErrorCode code_;
        std::string arg_;
    };

    struct FlagDef;
    class Flag;

    Flags();

    std::string getString(const std::string& flag) const;
    std::string asString(const std::string& flag) const;
    long int getNumber(const std::string& flag) const;
    float getFloat(const std::string& flag) const;
    bool getBool(const std::string& flag) const;

    const std::vector<std::string>& parameters() const;
    void setParameters(const std::vector<std::string>& v);

    size_t size() const { return set_.size(); }

    std::string to_s() const;

    void set(const Flag& flag);
    void set(const std::string& opt, const std::string& val, FlagStyle fs, FlagType ft);
    bool isSet(const std::string& flag) const;

    Flags& defineString(const std::string& longOpt, char shortOpt, const std::string& valuePlaceholder,
                        const std::string& helpText, std::optional<std::string> defaultValue = std::nullopt,
                        std::function<void(const std::string&)> callback = nullptr);

    Flags& defineNumber(const std::string& longOpt, char shortOpt, const std::string& valuePlaceholder,
                        const std::string& helpText, std::optional<long int> defaultValue = std::nullopt,
                        std::function<void(long int)> callback = nullptr);

    Flags& defineFloat(const std::string& longOpt, char shortOpt, const std::string& valuePlaceholder,
                       const std::string& helpText, std::optional<float> defaultValue = std::nullopt,
                       std::function<void(float)> callback = nullptr);

    Flags& defineBool(const std::string& longOpt, char shortOpt, const std::string& helpText,
                      std::function<void(bool)> callback = nullptr);

    Flags& enableParameters(const std::string& valuePlaceholder, const std::string& helpText);

    std::string helpText(std::string_view const& header = "") const { return helpText(header, 78, 30); }
    std::string helpText(std::string_view const& header, size_t width, size_t helpTextOffset) const;

    const FlagDef* findDef(const std::string& longOption) const;
    const FlagDef* findDef(char shortOption) const;

    void parse(int argc, const char* argv[]);
    void parse(const std::vector<std::string>& args);

    // Attempts to parse given arguments and returns an error code in case of parsing errors instead
    // of throwing.
    std::error_code tryParse(const std::vector<std::string>& args);

  private:
    Flags& define(const std::string& longOpt, char shortOpt, bool required, FlagType type,
                  const std::string& helpText, const std::string& valuePlaceholder,
                  const std::optional<std::string>& defaultValue,
                  std::function<void(const std::string&)> callback);

  private:
    std::list<FlagDef> flagDefs_;
    bool parametersEnabled_;  // non-option parameters enabled?
    std::string parametersPlaceholder_;
    std::string parametersHelpText_;

    typedef std::pair<FlagType, std::string> FlagValue;
    std::unordered_map<std::string, FlagValue> set_;
    std::vector<std::string> raw_;
};

struct Flags::FlagDef {
    FlagType type;
    std::string longOption;
    char shortOption;
    bool required;
    std::string valuePlaceholder;
    std::string helpText;
    std::optional<std::string> defaultValue;
    std::function<void(const std::string&)> callback;

    std::string makeHelpText(size_t width, size_t helpTextOffset) const;
};

class Flags::Flag {
  public:
    Flag(const std::string& opt, const std::string& val, FlagStyle fs, FlagType ft);

    explicit Flag(char shortOpt);
    Flag(char shortOpt, const std::string& val);
    Flag(const std::string& longOpt);
    Flag(const std::string& longOpt, const std::string& val);

    FlagType type() const { return type_; }
    const std::string& name() const { return name_; }
    const std::string& value() const { return value_; }

  private:
    FlagType type_;
    FlagStyle style_;
    std::string name_;
    std::string value_;
};

class FlagsErrorCategory : public std::error_category {
  public:
    static FlagsErrorCategory& get();

    const char* name() const noexcept override;
    std::string message(int ec) const override;
};

std::error_code make_error_code(Flags::ErrorCode errc);

}  // namespace util

namespace std {
template <>
struct is_error_code_enum<util::Flags::ErrorCode> : public std::true_type {
};
}  // namespace std

