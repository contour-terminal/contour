// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include <dwrite.h>
#include <dwrite_3.h>

#include <wrl/implements.h>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::InhibitFtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace text
{
class dwrite_analysis_wrapper:
    public RuntimeClass<RuntimeClassFlags<ClassicCom | InhibitFtmBase>,
                        IDWriteTextAnalysisSource,
                        IDWriteTextAnalysisSink>
{
  public:
    dwrite_analysis_wrapper(std::wstring const& _text,
                            std::wstring const& _userLocale,
                            DWRITE_READING_DIRECTION _readingDirection =
                                DWRITE_READING_DIRECTION::DWRITE_READING_DIRECTION_LEFT_TO_RIGHT):
        readingDirection(_readingDirection), text(_text), userLocale(_userLocale)
    {
    }

#pragma region IDWriteTextAnalysisSource
    HRESULT GetTextAtPosition(UINT32 textPosition,
                              _Outptr_result_buffer_(*textLength) WCHAR const** textString,
                              _Out_ UINT32* textLength) override
    {
        *textString = nullptr;
        *textLength = 0;

        if (textPosition < text.size())
        {
            *textString = &text.at(textPosition);
            *textLength = text.size() - textPosition;
        }

        return S_OK;
    }

    HRESULT GetTextBeforePosition(UINT32 textPosition,
                                  _Outptr_result_buffer_(*textLength) WCHAR const** textString,
                                  _Out_ UINT32* textLength) override
    {
        *textString = nullptr;
        *textLength = 0;

        if (textPosition > 0 && textPosition <= text.size())
        {
            *textString = text.data();
            *textLength = textPosition;
        }

        return S_OK;
    }

    DWRITE_READING_DIRECTION GetParagraphReadingDirection() override
    {
        // The direction the caller resolved for this run. DirectWrite asks for it before analysing,
        // and returning a hard-coded left-to-right made every right-to-left run analyse as if it
        // were Latin.
        return readingDirection;
    }

    HRESULT GetLocaleName(UINT32 textPosition,
                          _Out_ UINT32* textLength,
                          _Outptr_result_z_ WCHAR const** localeName) override
    {
        *localeName = userLocale.c_str();
        *textLength = text.size() - textPosition;

        return S_OK;
    }

    HRESULT GetNumberSubstitution(UINT32 textPosition,
                                  _Out_ UINT32* textLength,
                                  _COM_Outptr_ IDWriteNumberSubstitution** numberSubstitution) override
    {

        *numberSubstitution = nullptr;
        *textLength = text.size() - textPosition;

        return S_OK;
    }
#pragma endregion

#pragma region IDWriteTextAnalysisSink
    HRESULT SetScriptAnalysis(UINT32 textPosition,
                              UINT32 textLength,
                              _In_ DWRITE_SCRIPT_ANALYSIS const* scriptAnalysis) override
    {
        script = *scriptAnalysis;
        return S_OK;
    }

    HRESULT SetLineBreakpoints(UINT32 textPosition,
                               UINT32 textLength,
                               _In_reads_(textLength) DWRITE_LINE_BREAKPOINT const* lineBreakpoints) override
    {
        return S_OK;
    }

    HRESULT SetBidiLevel(UINT32 textPosition,
                         UINT32 textLength,
                         UINT8 /*explicitLevel*/,
                         UINT8 resolvedLevel) override
    {
        // DirectWrite hands back the level it resolved; record it so the caller can place the run
        // rather than discarding it and assuming zero.
        (void) textPosition;
        (void) textLength;
        this->resolvedBidiLevel = resolvedLevel;
        return S_OK;
    }

    HRESULT SetNumberSubstitution(UINT32 textPosition,
                                  UINT32 textLength,
                                  _In_ IDWriteNumberSubstitution* numberSubstitution) override
    {
        return S_OK;
    }
#pragma endregion

    DWRITE_SCRIPT_ANALYSIS script;

    /// The level DirectWrite resolved for the run, recorded by SetBidiLevel().
    UINT8 resolvedBidiLevel = 0;

  private:
    DWRITE_READING_DIRECTION readingDirection;
    std::wstring const& text;
    std::wstring const& userLocale;
};
} // namespace text
