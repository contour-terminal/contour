// SPDX-License-Identifier: Apache-2.0
#include <core/fs/FsError.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace core::fs
{

namespace
{

    FsError classify(std::span<SystemErrorRow const> table, std::int64_t systemCode, std::string context)
    {
        auto const row = std::ranges::find(table, systemCode, &SystemErrorRow::systemCode);
        return FsError { .code = row == table.end() ? FsErrorCode::Io : row->code,
                         .systemCode = systemCode,
                         .context = std::move(context) };
    }

} // namespace

std::string FsError::toString() const
{
    auto result = std::string { fs::toString(code) };
    if (!context.empty())
        result += " (" + context + ")";
    if (systemCode != 0)
        result += " [system code " + std::to_string(systemCode) + "]";
    return result;
}

FsError fsErrorFromSystemCode(std::int64_t systemCode, std::string context)
{
    return classify(systemErrorTable(), systemCode, std::move(context));
}

FsError fsErrorFromNtStatus(std::int64_t status, std::string context)
{
    return classify(ntStatusTable(), status, std::move(context));
}

} // namespace core::fs
