// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonExtensionConsentModel.hpp>

#include <algorithm>
#include <utility>

namespace contour::window
{

namespace
{
    /// @return Whether @p request is the question named by @p name and @p grantDigest.
    [[nodiscard]] bool isQuestion(vthost::proto::ExtensionPermissionRequest const& request,
                                  std::string_view name,
                                  std::string_view grantDigest) noexcept
    {
        return request.name == name && request.grantDigest == grantDigest;
    }
} // namespace

DaemonExtensionConsentModel::DaemonExtensionConsentModel(Reply reply,
                                                         core::platform::IClock const& clock,
                                                         Timer timer,
                                                         QObject* parent):
    QObject { parent }, _reply { std::move(reply) }, _clock { clock }, _timer { std::move(timer) }
{
}

void DaemonExtensionConsentModel::show(vthost::proto::ExtensionPermissionRequest const& request)
{
    // A re-sent question this window shows, or holds back, changes nothing here and takes no focus.
    if (std::ranges::any_of(_questions, [&](auto const& held) {
            return isQuestion(held, request.name, request.grantDigest);
        }))
        return;
    _questions.insert(_questions.begin(), request);
    presentFront();
}

void DaemonExtensionConsentModel::settle(std::string_view name, std::string_view grantDigest)
{
    auto const found = std::ranges::find_if(
        _questions, [&](auto const& held) { return isQuestion(held, name, grantDigest); });
    if (found == _questions.end())
        return;
    if (found == _questions.begin())
    {
        popShown();
        return;
    }
    _questions.erase(found);
}

void DaemonExtensionConsentModel::clear()
{
    if (_questions.empty())
        return;
    _questions.clear();
    notifyChanged();
}

int DaemonExtensionConsentModel::attachViewer()
{
    auto const viewer = _nextViewer++;
    _viewers.emplace(viewer, Viewer {});
    return viewer;
}

void DaemonExtensionConsentModel::detachViewer(int viewer)
{
    _viewers.erase(viewer);
}

void DaemonExtensionConsentModel::setViewerActive(int viewer, bool active)
{
    auto const found = _viewers.find(viewer);
    if (found == _viewers.end())
        return;
    auto const focus = active ? ViewerFocus::Active : ViewerFocus::Inactive;
    if (found->second.focus == focus)
        return;
    found->second.focus = focus;
    if (focus == ViewerFocus::Active)
    {
        // The click that activates the window must not land on an armed Allow.
        found->second.activeSince = _clock.now();
        holdBegan();
    }
    else
        notifyChanged();
}

bool DaemonExtensionConsentModel::isAllowHeldFor(int viewer) const
{
    auto const found = _viewers.find(viewer);
    if (_questions.empty() || found == _viewers.end() || found->second.focus != ViewerFocus::Active)
        return true;
    auto const armedFrom = std::max(_shownAt, found->second.activeSince);
    return _clock.now() - armedFrom < DaemonConsentAllowHold;
}

QString DaemonExtensionConsentModel::title() const
{
    return _questions.empty() ? QString {} : QString::fromStdString(_questions.front().title);
}

QStringList DaemonExtensionConsentModel::lines() const
{
    auto shown = QStringList {};
    if (!_questions.empty())
        for (auto const& line: _questions.front().lines)
            shown.append(QString::fromStdString(line.text));
    return shown;
}

void DaemonExtensionConsentModel::allowFrom(int viewer)
{
    if (_questions.empty() || isAllowHeldFor(viewer))
        return;
    auto const& shown = _questions.front();
    _reply(shown.name, shown.grantDigest, vthost::GrantState::Granted);
    popShown();
}

void DaemonExtensionConsentModel::deny()
{
    if (_questions.empty())
        return;
    auto const& shown = _questions.front();
    _reply(shown.name, shown.grantDigest, vthost::GrantState::Denied);
    popShown();
}

void DaemonExtensionConsentModel::dismiss()
{
    if (!_questions.empty())
        popShown();
}

void DaemonExtensionConsentModel::popShown()
{
    _questions.erase(_questions.begin());
    if (_questions.empty())
        notifyChanged();
    else
        presentFront();
}

void DaemonExtensionConsentModel::presentFront()
{
    _shownAt = _clock.now();
    holdBegan();
    emit focusRequested();
}

void DaemonExtensionConsentModel::holdBegan()
{
    // Ends this hold; a later one keeps its own.
    _timer(this, DaemonConsentAllowHold, [this] { notifyChanged(); });
    notifyChanged();
}

void DaemonExtensionConsentModel::notifyChanged()
{
    ++_revision;
    emit changed();
}

} // namespace contour::window
