// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/InFlightTail.hpp>
#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/graphics/KittyGraphics.hpp>
#include <vtbackend/graphics/regis/ReGISTables.hpp>
#include <vtbackend/grid/LineSoA.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/vi/HintTypes.hpp>
#include <vtbackend/input/vi/Selector.hpp>
#include <vtbackend/input/vi/ViInputHandler.hpp>
#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/LivePromptMarks.hpp>
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/PromptRegion.hpp>
#include <vtbackend/vt/Charset.hpp>
#include <vtbackend/vt/DesktopNotification.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>
#include <vtbackend/vt/ProgressState.hpp>
#include <vtbackend/vt/SemanticPrompt.hpp>
#include <vtbackend/vt/VTType.hpp>

#include <core/Flags.hpp>

#include <array>
#include <type_traits>

/// The stable wire names of every enumeration and flag set a terminal state carries.
///
/// An enumerator crosses the wire as its index in its table here, a flag as its bit in its table here --
/// never as the in-memory ordinal or bit position, which a reordering, an insertion or a renumbering of the
/// enumeration would change under a stream already written. A table is appended to, never reordered: a new
/// enumerator is one more row at its end. An enumerator a table lacks cannot be encoded (it crosses as a
/// value every reader refuses), so a row forgotten is a refused restore, never a silently remapped one.
namespace vtbackend::detail
{

/// The stable wire table of an enumeration: `static constexpr std::array Values`, in wire order.
template <typename Enum>
struct StateEnumWire;

/// An enumeration with a stable wire table.
template <typename Enum>
concept WireEnum = std::is_enum_v<Enum> && requires { StateEnumWire<Enum>::Values; };

/// The stable wire table of the flags of a flag set: `static constexpr std::array Values`, flag N crossing
/// as wire bit N.
template <typename Flag>
struct StateFlagWire;

template <typename T>
struct FlagSetTraits: std::false_type
{
};
template <typename Flag>
struct FlagSetTraits<core::Flags<Flag>>: std::true_type
{
    using FlagType = Flag;
};

/// A `core::Flags` set.
template <typename T>
concept FlagSetType = FlagSetTraits<T>::value;

/// The flag enumeration of the flag set @p Flags.
template <FlagSetType Flags>
using FlagOf = FlagSetTraits<Flags>::FlagType;

// {{{ tables -- appended to, never reordered

template <>
struct StateEnumWire<LineShape>
{
    static constexpr auto Values = std::array {
        LineShape::Uniform,
        LineShape::Mixed,
        LineShape::Spanning,
    };
};

template <>
struct StateEnumWire<vtbackend::ActiveStatusDisplay>
{
    static constexpr auto Values = std::array {
        vtbackend::ActiveStatusDisplay::Main,
        vtbackend::ActiveStatusDisplay::StatusLine,
        vtbackend::ActiveStatusDisplay::IndicatorStatusLine,
    };
};

template <>
struct StateEnumWire<vtbackend::BellVolume>
{
    static constexpr auto Values = std::array {
        vtbackend::BellVolume::Off,
        vtbackend::BellVolume::Low,
        vtbackend::BellVolume::High,
    };
};

template <>
struct StateEnumWire<vtbackend::BlockedKind>
{
    static constexpr auto Values = std::array {
        vtbackend::BlockedKind::None,
        vtbackend::BlockedKind::Permission,
        vtbackend::BlockedKind::Question,
        vtbackend::BlockedKind::Auth,
    };
};

template <>
struct StateEnumWire<vtbackend::CharsetId>
{
    static constexpr auto Values = std::array {
        vtbackend::CharsetId::Special,   vtbackend::CharsetId::British,
        vtbackend::CharsetId::Dutch,     vtbackend::CharsetId::Finnish,
        vtbackend::CharsetId::French,    vtbackend::CharsetId::FrenchCanadian,
        vtbackend::CharsetId::German,    vtbackend::CharsetId::NorwegianDanish,
        vtbackend::CharsetId::Spanish,   vtbackend::CharsetId::Swedish,
        vtbackend::CharsetId::Swiss,     vtbackend::CharsetId::USASCII,
        vtbackend::CharsetId::Technical, vtbackend::CharsetId::ISOLatin1Supplemental,
    };
};

template <>
struct StateEnumWire<vtbackend::CharsetTable>
{
    static constexpr auto Values = std::array {
        vtbackend::CharsetTable::G0,
        vtbackend::CharsetTable::G1,
        vtbackend::CharsetTable::G2,
        vtbackend::CharsetTable::G3,
    };
};

template <>
struct StateEnumWire<vtbackend::ColorType>
{
    static constexpr auto Values = std::array {
        vtbackend::ColorType::Undefined, vtbackend::ColorType::Default, vtbackend::ColorType::Bright,
        vtbackend::ColorType::Indexed,   vtbackend::ColorType::RGB,
    };
};

template <>
struct StateEnumWire<vtbackend::CommandBlockEnd>
{
    static constexpr auto Values = std::array {
        vtbackend::CommandBlockEnd::Reported,
        vtbackend::CommandBlockEnd::Implicit,
        vtbackend::CommandBlockEnd::Abandoned,
    };
};

template <>
struct StateEnumWire<vtbackend::CommandBlockState>
{
    static constexpr auto Values = std::array {
        vtbackend::CommandBlockState::Prompting,
        vtbackend::CommandBlockState::Running,
        vtbackend::CommandBlockState::Finished,
    };
};

template <>
struct StateEnumWire<vtbackend::CommandLineSource>
{
    static constexpr auto Values = std::array {
        vtbackend::CommandLineSource::None,
        vtbackend::CommandLineSource::Reported,
        vtbackend::CommandLineSource::Recovered,
    };
};

template <>
struct StateEnumWire<vtbackend::ContextExit>
{
    static constexpr auto Values = std::array {
        vtbackend::ContextExit::Unknown, vtbackend::ContextExit::Success,   vtbackend::ContextExit::Failure,
        vtbackend::ContextExit::Crash,   vtbackend::ContextExit::Interrupt,
    };
};

template <>
struct StateEnumWire<vtbackend::ContextLocality>
{
    static constexpr auto Values = std::array {
        vtbackend::ContextLocality::Unknown,
        vtbackend::ContextLocality::Local,
        vtbackend::ContextLocality::Foreign,
    };
};

template <>
struct StateEnumWire<vtbackend::ContextSignal>
{
    static constexpr auto Values = std::array {
        vtbackend::ContextSignal::None, vtbackend::ContextSignal::Hup,  vtbackend::ContextSignal::Int,
        vtbackend::ContextSignal::Quit, vtbackend::ContextSignal::Ill,  vtbackend::ContextSignal::Trap,
        vtbackend::ContextSignal::Abrt, vtbackend::ContextSignal::Bus,  vtbackend::ContextSignal::Fpe,
        vtbackend::ContextSignal::Kill, vtbackend::ContextSignal::Usr1, vtbackend::ContextSignal::Segv,
        vtbackend::ContextSignal::Usr2, vtbackend::ContextSignal::Pipe, vtbackend::ContextSignal::Alrm,
        vtbackend::ContextSignal::Term, vtbackend::ContextSignal::Xcpu, vtbackend::ContextSignal::Xfsz,
        vtbackend::ContextSignal::Sys,
    };
};

template <>
struct StateEnumWire<vtbackend::ContextType>
{
    static constexpr auto Values = std::array {
        vtbackend::ContextType::None,
        vtbackend::ContextType::Service,
        vtbackend::ContextType::Session,
        vtbackend::ContextType::Shell,
        vtbackend::ContextType::Command,
        vtbackend::ContextType::Vm,
        vtbackend::ContextType::Container,
        vtbackend::ContextType::Elevate,
        vtbackend::ContextType::ChangePrivileges,
        vtbackend::ContextType::Subcontext,
        vtbackend::ContextType::Remote,
        vtbackend::ContextType::Boot,
        vtbackend::ContextType::App,
    };
};

template <>
struct StateEnumWire<vtbackend::ControlTransmissionMode>
{
    static constexpr auto Values = std::array {
        vtbackend::ControlTransmissionMode::S7C1T,
        vtbackend::ControlTransmissionMode::S8C1T,
    };
};

template <>
struct StateEnumWire<vtbackend::CursorDisplay>
{
    static constexpr auto Values = std::array {
        vtbackend::CursorDisplay::Steady,
        vtbackend::CursorDisplay::Blink,
    };
};

template <>
struct StateEnumWire<vtbackend::CursorShape>
{
    static constexpr auto Values = std::array {
        vtbackend::CursorShape::Block,
        vtbackend::CursorShape::Rectangle,
        vtbackend::CursorShape::Underscore,
        vtbackend::CursorShape::Bar,
    };
};

template <>
struct StateEnumWire<vtbackend::DisplayOccasion>
{
    static constexpr auto Values = std::array {
        vtbackend::DisplayOccasion::Always,
        vtbackend::DisplayOccasion::Unfocused,
        vtbackend::DisplayOccasion::Invisible,
    };
};

template <>
struct StateEnumWire<vtbackend::ForeignDirectory>
{
    static constexpr auto Values = std::array {
        vtbackend::ForeignDirectory::Unreported,
        vtbackend::ForeignDirectory::Reported,
    };
};

template <>
struct StateEnumWire<vtbackend::HintAction>
{
    static constexpr auto Values = std::array {
        vtbackend::HintAction::Copy,         vtbackend::HintAction::Open,   vtbackend::HintAction::Paste,
        vtbackend::HintAction::CopyAndPaste, vtbackend::HintAction::Select,
    };
};

template <>
struct StateEnumWire<vtbackend::HintScope>
{
    static constexpr auto Values = std::array {
        vtbackend::HintScope::Visible,
        vtbackend::HintScope::Scrollback,
    };
};

template <>
struct StateEnumWire<vtbackend::HookOrigin>
{
    static constexpr auto Values = std::array {
        vtbackend::HookOrigin::Stream,
        vtbackend::HookOrigin::MacroBody,
    };
};

template <>
struct StateEnumWire<vtbackend::ImageAlignment>
{
    static constexpr auto Values = std::array {
        vtbackend::ImageAlignment::TopStart,     vtbackend::ImageAlignment::TopCenter,
        vtbackend::ImageAlignment::TopEnd,       vtbackend::ImageAlignment::MiddleStart,
        vtbackend::ImageAlignment::MiddleCenter, vtbackend::ImageAlignment::MiddleEnd,
        vtbackend::ImageAlignment::BottomStart,  vtbackend::ImageAlignment::BottomCenter,
        vtbackend::ImageAlignment::BottomEnd,
    };
};

template <>
struct StateEnumWire<vtbackend::ImageFormat>
{
    static constexpr auto Values = std::array {
        vtbackend::ImageFormat::Auto,
        vtbackend::ImageFormat::RGB,
        vtbackend::ImageFormat::RGBA,
        vtbackend::ImageFormat::PNG,
    };
};

template <>
struct StateEnumWire<vtbackend::ImageLayer>
{
    static constexpr auto Values = std::array {
        vtbackend::ImageLayer::Below,
        vtbackend::ImageLayer::Replace,
        vtbackend::ImageLayer::Above,
    };
};

template <>
struct StateEnumWire<vtbackend::ImageResize>
{
    static constexpr auto Values = std::array {
        vtbackend::ImageResize::NoResize,
        vtbackend::ImageResize::ResizeToFit,
        vtbackend::ImageResize::ResizeToFill,
        vtbackend::ImageResize::StretchToFill,
    };
};

template <>
struct StateEnumWire<vtbackend::InputEnd>
{
    static constexpr auto Values = std::array {
        vtbackend::InputEnd::AtOutputStart,
        vtbackend::InputEnd::AtLineEnd,
    };
};

template <>
struct StateEnumWire<vtbackend::InputGenerator::MouseWheelMode>
{
    static constexpr auto Values = std::array {
        vtbackend::InputGenerator::MouseWheelMode::Default,
        vtbackend::InputGenerator::MouseWheelMode::NormalCursorKeys,
        vtbackend::InputGenerator::MouseWheelMode::ApplicationCursorKeys,
    };
};

template <>
struct StateEnumWire<vtbackend::KeyMode>
{
    static constexpr auto Values = std::array {
        vtbackend::KeyMode::Normal,
        vtbackend::KeyMode::Application,
    };
};

template <>
struct StateEnumWire<vtbackend::LinkKind>
{
    static constexpr auto Values = std::array {
        vtbackend::LinkKind::Url,
        vtbackend::LinkKind::Path,
        vtbackend::LinkKind::Custom,
    };
};

template <>
struct StateEnumWire<vtbackend::LinkOpening>
{
    static constexpr auto Values = std::array {
        vtbackend::LinkOpening::Refused,
        vtbackend::LinkOpening::Allowed,
    };
};

template <>
struct StateEnumWire<vtbackend::MarkOwner>
{
    static constexpr auto Values = std::array {
        vtbackend::MarkOwner::Undecided,
        vtbackend::MarkOwner::ShellIntegration,
        vtbackend::MarkOwner::ContextSignalling,
    };
};

template <>
struct StateEnumWire<vtbackend::MouseButton>
{
    static constexpr auto Values = std::array {
        vtbackend::MouseButton::Left,      vtbackend::MouseButton::Right,
        vtbackend::MouseButton::Middle,    vtbackend::MouseButton::Release,
        vtbackend::MouseButton::WheelUp,   vtbackend::MouseButton::WheelDown,
        vtbackend::MouseButton::WheelLeft, vtbackend::MouseButton::WheelRight,
    };
};

template <>
struct StateEnumWire<vtbackend::MouseProtocol>
{
    static constexpr auto Values = std::array {
        vtbackend::MouseProtocol::X10,
        vtbackend::MouseProtocol::NormalTracking,
        vtbackend::MouseProtocol::HighlightTracking,
        vtbackend::MouseProtocol::ButtonTracking,
        vtbackend::MouseProtocol::AnyEventTracking,
    };
};

template <>
struct StateEnumWire<vtbackend::MouseTransport>
{
    static constexpr auto Values = std::array {
        vtbackend::MouseTransport::Default, vtbackend::MouseTransport::Extended,
        vtbackend::MouseTransport::SGR,     vtbackend::MouseTransport::SGRPixels,
        vtbackend::MouseTransport::URXVT,
    };
};

template <>
struct StateEnumWire<vtbackend::NotificationPayloadType>
{
    static constexpr auto Values = std::array {
        vtbackend::NotificationPayloadType::Title, vtbackend::NotificationPayloadType::Body,
        vtbackend::NotificationPayloadType::Close, vtbackend::NotificationPayloadType::Query,
        vtbackend::NotificationPayloadType::Alive,
    };
};

template <>
struct StateEnumWire<vtbackend::NotificationSound>
{
    static constexpr auto Values = std::array {
        vtbackend::NotificationSound::DesktopDefault,
        vtbackend::NotificationSound::Silent,
        vtbackend::NotificationSound::Chime,
    };
};

template <>
struct StateEnumWire<vtbackend::NotificationUrgency>
{
    static constexpr auto Values = std::array {
        vtbackend::NotificationUrgency::Low,
        vtbackend::NotificationUrgency::Normal,
        vtbackend::NotificationUrgency::Critical,
    };
};

template <>
struct StateEnumWire<vtbackend::CellSizeDropLog>
{
    static constexpr auto Values = std::array {
        vtbackend::CellSizeDropLog::NotYetLogged,
        vtbackend::CellSizeDropLog::Logged,
    };
};

template <>
struct StateEnumWire<vtbackend::OutputStartMarking>
{
    static constexpr auto Values = std::array {
        vtbackend::OutputStartMarking::Unseen,
        vtbackend::OutputStartMarking::Seen,
    };
};

template <>
struct StateEnumWire<vtbackend::PathKind>
{
    static constexpr auto Values = std::array {
        vtbackend::PathKind::Missing,
        vtbackend::PathKind::File,
        vtbackend::PathKind::Directory,
    };
};

template <>
struct StateEnumWire<vtbackend::ProgramState>
{
    static constexpr auto Values = std::array {
        vtbackend::ProgramState::Idle,    vtbackend::ProgramState::Working, vtbackend::ProgramState::Done,
        vtbackend::ProgramState::Blocked, vtbackend::ProgramState::Error,
    };
};

template <>
struct StateEnumWire<vtbackend::ProgressState>
{
    static constexpr auto Values = std::array {
        vtbackend::ProgressState::Inactive, vtbackend::ProgressState::Normal,
        vtbackend::ProgressState::Error,    vtbackend::ProgressState::Indeterminate,
        vtbackend::ProgressState::Paused,
    };
};

template <>
struct StateEnumWire<vtbackend::PromptRedraw>
{
    static constexpr auto Values = std::array {
        vtbackend::PromptRedraw::OnResize,
        vtbackend::PromptRedraw::Never,
    };
};

template <>
struct StateEnumWire<vtbackend::SearchCaseSensitivity>
{
    static constexpr auto Values = std::array {
        vtbackend::SearchCaseSensitivity::Smart,
        vtbackend::SearchCaseSensitivity::Insensitive,
        vtbackend::SearchCaseSensitivity::Sensitive,
    };
};

template <>
struct StateEnumWire<vtbackend::SearchOrigin>
{
    static constexpr auto Values = std::array {
        vtbackend::SearchOrigin::Typed,
        vtbackend::SearchOrigin::DoubleClick,
        vtbackend::SearchOrigin::WordUnderCursor,
    };
};

template <>
struct StateEnumWire<vtbackend::SearchSyntax>
{
    static constexpr auto Values = std::array {
        vtbackend::SearchSyntax::Literal,
        vtbackend::SearchSyntax::Regex,
    };
};

template <>
struct StateEnumWire<vtbackend::SearchWordBoundary>
{
    static constexpr auto Values = std::array {
        vtbackend::SearchWordBoundary::Any,
        vtbackend::SearchWordBoundary::WholeWord,
    };
};

template <>
struct StateEnumWire<vtbackend::Selection::State>
{
    static constexpr auto Values = std::array {
        vtbackend::Selection::State::Waiting,
        vtbackend::Selection::State::InProgress,
        vtbackend::Selection::State::Complete,
    };
};

template <>
struct StateEnumWire<vtbackend::SelectionKind>
{
    static constexpr auto Values = std::array {
        vtbackend::SelectionKind::Linear,
        vtbackend::SelectionKind::Rectangular,
        vtbackend::SelectionKind::WordWise,
        vtbackend::SelectionKind::FullLine,
    };
};

template <>
struct StateEnumWire<vtbackend::StatusDisplayType>
{
    static constexpr auto Values = std::array {
        vtbackend::StatusDisplayType::None,
        vtbackend::StatusDisplayType::Indicator,
        vtbackend::StatusDisplayType::HostWritable,
    };
};

template <>
struct StateEnumWire<vtbackend::TailCompleteness>
{
    static constexpr auto Values = std::array {
        vtbackend::TailCompleteness::Complete,
        vtbackend::TailCompleteness::Overflowed,
    };
};

template <>
struct StateEnumWire<vtbackend::Terminal::LocatorCoordUnit>
{
    static constexpr auto Values = std::array {
        vtbackend::Terminal::LocatorCoordUnit::CharacterCells,
        vtbackend::Terminal::LocatorCoordUnit::DevicePixels,
    };
};

template <>
struct StateEnumWire<vtbackend::Terminal::LocatorReporting>
{
    static constexpr auto Values = std::array {
        vtbackend::Terminal::LocatorReporting::Off,
        vtbackend::Terminal::LocatorReporting::Continuous,
        vtbackend::Terminal::LocatorReporting::OneShot,
    };
};

template <>
struct StateEnumWire<vtbackend::UserInputLock>
{
    static constexpr auto Values = std::array {
        vtbackend::UserInputLock::Unlocked,
        vtbackend::UserInputLock::Locked,
    };
};

template <>
struct StateEnumWire<vtbackend::VTType>
{
    static constexpr auto Values = std::array {
        vtbackend::VTType::VT100, vtbackend::VTType::VT220, vtbackend::VTType::VT240,
        vtbackend::VTType::VT330, vtbackend::VTType::VT340, vtbackend::VTType::VT320,
        vtbackend::VTType::VT420, vtbackend::VTType::VT510, vtbackend::VTType::VT520,
        vtbackend::VTType::VT525,
    };
};

template <>
struct StateEnumWire<vtbackend::ViMode>
{
    static constexpr auto Values = std::array {
        vtbackend::ViMode::Normal,     vtbackend::ViMode::Insert,      vtbackend::ViMode::Visual,
        vtbackend::ViMode::VisualLine, vtbackend::ViMode::VisualBlock, vtbackend::ViMode::Hint,
    };
};

template <>
struct StateEnumWire<vtbackend::ViMotion>
{
    static constexpr auto Values = std::array {
        vtbackend::ViMotion::Explicit,
        vtbackend::ViMotion::Selection,
        vtbackend::ViMotion::FullLine,
        vtbackend::ViMotion::CharLeft,
        vtbackend::ViMotion::CharRight,
        vtbackend::ViMotion::ScreenColumn,
        vtbackend::ViMotion::FileBegin,
        vtbackend::ViMotion::FileEnd,
        vtbackend::ViMotion::LineBegin,
        vtbackend::ViMotion::LineTextBegin,
        vtbackend::ViMotion::LineDown,
        vtbackend::ViMotion::LineEnd,
        vtbackend::ViMotion::LineUp,
        vtbackend::ViMotion::LinesCenter,
        vtbackend::ViMotion::PageDown,
        vtbackend::ViMotion::PageUp,
        vtbackend::ViMotion::PageTop,
        vtbackend::ViMotion::PageBottom,
        vtbackend::ViMotion::ParagraphBackward,
        vtbackend::ViMotion::ParagraphForward,
        vtbackend::ViMotion::GlobalCurlyCloseUp,
        vtbackend::ViMotion::GlobalCurlyCloseDown,
        vtbackend::ViMotion::GlobalCurlyOpenUp,
        vtbackend::ViMotion::GlobalCurlyOpenDown,
        vtbackend::ViMotion::LineMarkUp,
        vtbackend::ViMotion::LineMarkDown,
        vtbackend::ViMotion::ParenthesisMatching,
        vtbackend::ViMotion::SearchResultBackward,
        vtbackend::ViMotion::SearchResultForward,
        vtbackend::ViMotion::WordBackward,
        vtbackend::ViMotion::WordEndForward,
        vtbackend::ViMotion::WordForward,
        vtbackend::ViMotion::BigWordBackward,
        vtbackend::ViMotion::BigWordEndForward,
        vtbackend::ViMotion::BigWordForward,
        vtbackend::ViMotion::TillBeforeCharRight,
        vtbackend::ViMotion::TillAfterCharLeft,
        vtbackend::ViMotion::ToCharRight,
        vtbackend::ViMotion::ToCharLeft,
        vtbackend::ViMotion::RepeatCharMove,
        vtbackend::ViMotion::RepeatCharMoveReverse,
        vtbackend::ViMotion::JumpToLastJumpPoint,
        vtbackend::ViMotion::JumpToMarkBackward,
        vtbackend::ViMotion::JumpToMarkForward,
        vtbackend::ViMotion::CenterCursor,
        vtbackend::ViMotion::LinkForward,
        vtbackend::ViMotion::LinkBackward,
    };
};

template <>
struct StateEnumWire<vtbackend::kitty_graphics::Action>
{
    static constexpr auto Values = std::array {
        vtbackend::kitty_graphics::Action::Query,
        vtbackend::kitty_graphics::Action::Transmit,
        vtbackend::kitty_graphics::Action::TransmitAndDisplay,
        vtbackend::kitty_graphics::Action::Put,
        vtbackend::kitty_graphics::Action::Delete,
        vtbackend::kitty_graphics::Action::Frame,
        vtbackend::kitty_graphics::Action::Animate,
        vtbackend::kitty_graphics::Action::Compose,
    };
};

template <>
struct StateEnumWire<vtbackend::kitty_graphics::AnimationState>
{
    static constexpr auto Values = std::array {
        vtbackend::kitty_graphics::AnimationState::Unset,
        vtbackend::kitty_graphics::AnimationState::Stop,
        vtbackend::kitty_graphics::AnimationState::RunAwaitingFrames,
        vtbackend::kitty_graphics::AnimationState::Loop,
    };
};

template <>
struct StateEnumWire<vtbackend::kitty_graphics::CompositionMode>
{
    static constexpr auto Values = std::array {
        vtbackend::kitty_graphics::CompositionMode::AlphaBlend,
        vtbackend::kitty_graphics::CompositionMode::Replace,
    };
};

template <>
struct StateEnumWire<vtbackend::kitty_graphics::Compression>
{
    static constexpr auto Values = std::array {
        vtbackend::kitty_graphics::Compression::None,
        vtbackend::kitty_graphics::Compression::ZlibDeflate,
    };
};

template <>
struct StateEnumWire<vtbackend::kitty_graphics::Format>
{
    static constexpr auto Values = std::array {
        vtbackend::kitty_graphics::Format::Rgb,
        vtbackend::kitty_graphics::Format::Rgba,
        vtbackend::kitty_graphics::Format::Png,
    };
};

template <>
struct StateEnumWire<vtbackend::kitty_graphics::Medium>
{
    static constexpr auto Values = std::array {
        vtbackend::kitty_graphics::Medium::Direct,
        vtbackend::kitty_graphics::Medium::File,
        vtbackend::kitty_graphics::Medium::TemporaryFile,
        vtbackend::kitty_graphics::Medium::SharedMemory,
    };
};

template <>
struct StateEnumWire<vtbackend::regis::WritingMode>
{
    static constexpr auto Values = std::array {
        vtbackend::regis::WritingMode::Overlay,
        vtbackend::regis::WritingMode::Replace,
        vtbackend::regis::WritingMode::Complement,
        vtbackend::regis::WritingMode::Erase,
    };
};

template <>
struct StateFlagWire<vtbackend::CellFlag>
{
    static constexpr auto Values = std::array {
        vtbackend::CellFlag::Bold,
        vtbackend::CellFlag::Faint,
        vtbackend::CellFlag::Italic,
        vtbackend::CellFlag::Underline,
        vtbackend::CellFlag::Blinking,
        vtbackend::CellFlag::Inverse,
        vtbackend::CellFlag::Hidden,
        vtbackend::CellFlag::CrossedOut,
        vtbackend::CellFlag::DoublyUnderlined,
        vtbackend::CellFlag::CurlyUnderlined,
        vtbackend::CellFlag::DottedUnderline,
        vtbackend::CellFlag::DashedUnderline,
        vtbackend::CellFlag::Framed,
        vtbackend::CellFlag::Encircled,
        vtbackend::CellFlag::Overline,
        vtbackend::CellFlag::RapidBlinking,
        vtbackend::CellFlag::CharacterProtected,
        vtbackend::CellFlag::WideCharContinuation,
        vtbackend::CellFlag::CharacterProtectedISO,
        vtbackend::CellFlag::MulticellContinuation,
        vtbackend::CellFlag::WrapPadding,
        vtbackend::CellFlag::InputMethodPreedit,
    };
};

template <>
struct StateFlagWire<vtbackend::ContextField>
{
    static constexpr auto Values = std::array {
        vtbackend::ContextField::Type,
        vtbackend::ContextField::User,
        vtbackend::ContextField::Hostname,
        vtbackend::ContextField::MachineId,
        vtbackend::ContextField::BootId,
        vtbackend::ContextField::Pid,
        vtbackend::ContextField::PidFdId,
        vtbackend::ContextField::Comm,
        vtbackend::ContextField::WorkingDirectory,
        vtbackend::ContextField::CommandLine,
        vtbackend::ContextField::Vm,
        vtbackend::ContextField::Container,
        vtbackend::ContextField::TargetUser,
        vtbackend::ContextField::TargetHost,
        vtbackend::ContextField::SessionId,
    };
};

template <>
struct StateFlagWire<vtbackend::LineFlag>
{
    static constexpr auto Values = std::array {
        vtbackend::LineFlag::Wrappable,
        vtbackend::LineFlag::Wrapped,
        vtbackend::LineFlag::Marked,
        vtbackend::LineFlag::OutputStart,
        vtbackend::LineFlag::DoubleWidth,
        vtbackend::LineFlag::DoubleHeightTop,
        vtbackend::LineFlag::DoubleHeightBottom,
        vtbackend::LineFlag::CommandEnd,
        vtbackend::LineFlag::PromptEnd,
        vtbackend::LineFlag::UserMark,
    };
};

template <>
struct StateFlagWire<vtbackend::StatusNotice>
{
    static constexpr auto Values = std::array {
        vtbackend::StatusNotice::KeyboardLocked,
        vtbackend::StatusNotice::KeyBindingsDisabled,
        vtbackend::StatusNotice::ViMode,
        vtbackend::StatusNotice::UserInputLocked,
    };
};

// }}}

} // namespace vtbackend::detail
