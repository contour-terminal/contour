// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Report.h>

#include <iostream>
#include <sstream>

using namespace std;
using namespace regex_dfa;

// {{{ Message
string Report::Message::to_string() const
{
    switch (type)
    {
        case Type::Warning: return fmt::format("[{}] {}", sourceLocation, text);
        case Type::LinkError: return fmt::format("{}: {}", type, text);
        default: return fmt::format("[{}] {}: {}", sourceLocation, type, text);
    }
}

bool Report::Message::operator==(const Message& other) const noexcept
{
    // XXX ignore SourceLocation's filename & end
    return type == other.type && sourceLocation.offset == other.sourceLocation.offset && text == other.text;
}
// }}}
// {{{ ConsoleReport
void ConsoleReport::onMessage(Message&& message)
{
    switch (message.type)
    {
        case Type::Warning: cerr << fmt::format("Warning: {}\n", message); break;
        default: cerr << fmt::format("Error: {}\n", message); break;
    }
}
// }}}
// {{{ BufferedReport
void BufferedReport::onMessage(Message&& msg)
{
    messages_.emplace_back(std::move(msg));
}

void BufferedReport::clear()
{
    messages_.clear();
}

string BufferedReport::to_string() const
{
    stringstream sstr;
    for (const Message& message: messages_)
    {
        switch (message.type)
        {
            case Type::Warning: sstr << "Warning: " << message.to_string() << "\n"; break;
            default: sstr << "Error: " << message.to_string() << "\n"; break;
        }
    }
    return sstr.str();
}

bool BufferedReport::operator==(const BufferedReport& other) const noexcept
{
    if (size() != other.size())
        return false;

    for (size_t i = 0, e = size(); i != e; ++i)
        if (messages_[i] != other.messages_[i])
            return false;

    return true;
}

bool BufferedReport::contains(const Message& message) const noexcept
{
    for (const Message& m: messages_)
        if (m == message)
            return true;

    return false;
}

DifferenceReport difference(const BufferedReport& first, const BufferedReport& second)
{
    DifferenceReport diff;

    for (const Report::Message& m: first)
        if (!second.contains(m))
            diff.first.push_back(m);

    for (const Report::Message& m: second)
        if (!first.contains(m))
            diff.second.push_back(m);

    return diff;
}

ostream& operator<<(ostream& os, const BufferedReport& report)
{
    os << report.to_string();
    return os;
}
// }}}
