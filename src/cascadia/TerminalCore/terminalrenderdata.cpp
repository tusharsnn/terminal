// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Terminal.hpp"
#include <DefaultSettings.h>

using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::Render;

Viewport Terminal::GetViewport() noexcept
{
    return _GetVisibleViewport();
}

til::point Terminal::GetTextBufferEndPosition() const noexcept
{
    // We use the end line of mutableViewport as the end
    // of the text buffer, it always moves with the written
    // text
    return { _GetMutableViewport().Width() - 1, ViewEndIndex() };
}

const TextBuffer& Terminal::GetTextBuffer() const noexcept
{
    return _activeBuffer();
}

const FontInfo& Terminal::GetFontInfo() const noexcept
{
    _assertLocked();
    return _fontInfo;
}

void Terminal::SetFontInfo(const FontInfo& fontInfo)
{
    _assertLocked();
    _fontInfo = fontInfo;
}

til::point Terminal::GetCursorPosition() const noexcept
{
    const auto& cursor = _activeBuffer().GetCursor();
    return cursor.GetPosition();
}

bool Terminal::IsCursorVisible() const noexcept
{
    const auto& cursor = _activeBuffer().GetCursor();
    return cursor.IsVisible() && !cursor.IsPopupShown();
}

bool Terminal::IsCursorOn() const noexcept
{
    const auto& cursor = _activeBuffer().GetCursor();
    return cursor.IsOn();
}

ULONG Terminal::GetCursorPixelWidth() const noexcept
{
    return 1;
}

ULONG Terminal::GetCursorHeight() const noexcept
{
    return _activeBuffer().GetCursor().GetSize();
}

CursorType Terminal::GetCursorStyle() const noexcept
{
    return _activeBuffer().GetCursor().GetType();
}

bool Terminal::IsCursorDoubleWidth() const
{
    const auto& buffer = _activeBuffer();
    const auto position = buffer.GetCursor().GetPosition();
    return buffer.GetRowByOffset(position.y).DbcsAttrAt(position.x) != DbcsAttribute::Single;
}

const std::vector<RenderOverlay> Terminal::GetOverlays() const noexcept
{
    return {};
}

const bool Terminal::IsGridLineDrawingAllowed() noexcept
{
    return true;
}

const std::wstring Microsoft::Terminal::Core::Terminal::GetHyperlinkUri(uint16_t id) const
{
    return _activeBuffer().GetHyperlinkUriFromId(id);
}

const std::wstring Microsoft::Terminal::Core::Terminal::GetHyperlinkCustomId(uint16_t id) const
{
    return _activeBuffer().GetCustomIdFromId(id);
}

// Method Description:
// - Gets the regex pattern ids of a location
// Arguments:
// - The location
// Return value:
// - The pattern IDs of the location
const std::vector<size_t> Terminal::GetPatternId(const til::point location) const
{
    _assertLocked();

    // Look through our interval tree for this location
    const auto intervals = _patternIntervalTree.findOverlapping({ location.x + 1, location.y }, location);
    if (intervals.size() == 0)
    {
        return {};
    }
    else
    {
        std::vector<size_t> result{};
        for (const auto& interval : intervals)
        {
            result.emplace_back(interval.value);
        }
        return result;
    }
    return {};
}

std::pair<COLORREF, COLORREF> Terminal::GetAttributeColors(const TextAttribute& attr) const noexcept
{
    return GetRenderSettings().GetAttributeColors(attr);
}

std::vector<Microsoft::Console::Types::Viewport> Terminal::GetSelectionRects() noexcept
try
{
    std::vector<Viewport> result;

    for (const auto& lineRect : _GetSelectionRects())
    {
        result.emplace_back(Viewport::FromInclusive(lineRect));
    }

    return result;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

// Method Description:
// - Helper to determine the search highlights in the buffer. Used for rendering.
// Return Value:
// - A vector of rectangles representing the regions to select, line by line. They are absolute coordinates relative to the buffer origin.
std::vector<til::inclusive_rect> Terminal::GetSearchHighlights() const noexcept
try
{
    _assertLocked();

    std::vector<til::inclusive_rect> result;

    // find the search highlights that are within the viewport
    const auto& viewport = _GetVisibleViewport();
    auto lowerIt = std::lower_bound(_searchHighlights.begin(), _searchHighlights.end(), viewport.Top(), [](const til::inclusive_rect& rect, til::CoordType value) {
        return rect.top < value;
    });
    const auto upperIt = std::upper_bound(_searchHighlights.begin(), _searchHighlights.end(), viewport.BottomExclusive(), [](til::CoordType value, const til::inclusive_rect& rect) {
        return value < rect.top;
    });

    return { lowerIt, upperIt };
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

// Method Description:
// - If necessary, scrolls the viewport such that the start point is in the
//   viewport, and if that's already the case, also brings the end point inside
//   the viewport
// Arguments:
// - coordStart - The start point
// - coordEnd - The end point
// Return Value:
// - The updated scroll offset
til::CoordType Terminal::_ScrollToPoints(const til::point coordStart, const til::point coordEnd)
{
    auto notifyScrollChange = false;
    if (coordStart.y < _VisibleStartIndex())
    {
        // recalculate the scrollOffset
        _scrollOffset = ViewStartIndex() - coordStart.y;
        notifyScrollChange = true;
    }
    else if (coordEnd.y > _VisibleEndIndex())
    {
        // recalculate the scrollOffset, note that if the found text is
        // beneath the current visible viewport, it may be within the
        // current mutableViewport and the scrollOffset will be smaller
        // than 0
        _scrollOffset = std::max(0, ViewStartIndex() - coordStart.y);
        notifyScrollChange = true;
    }

    if (notifyScrollChange)
    {
        _activeBuffer().TriggerScroll();
        _NotifyScrollEvent();
    }

    return _scrollOffset;
}

void Terminal::SelectNewRegion(const til::point coordStart, const til::point coordEnd)
{
    const auto newScrollOffset = _ScrollToPoints(coordStart, coordEnd);

    // update the selection coordinates so they're relative to the new scroll-offset
    const auto newCoordStart = til::point{ coordStart.x, coordStart.y - newScrollOffset };
    const auto newCoordEnd = til::point{ coordEnd.x, coordEnd.y - newScrollOffset };
    SetSelectionAnchor(newCoordStart);
    SetSelectionEnd(newCoordEnd, SelectionExpansion::Char);
}

void Terminal::SetSearchHighlights(std::vector<til::inclusive_rect> highlights) noexcept
{
    _assertLocked();
    _searchHighlights = std::move(highlights);
}

// Method Description:
// - Stores the focused search highlighted region of the terminal
// - If the region isn't empty, it will be brought into view
void Terminal::SetSearchHighlightFocused(std::vector<til::inclusive_rect> highlight)
{
    _assertLocked();

    if (!highlight.empty())
    {
        // bring focused region into the view. We expect rects order to be top to bottom
        const auto highlightStart = til::point{ highlight.front().left, highlight.front().top };
        const auto highlightEnd = til::point{ highlight.back().right, highlight.back().bottom };
        _ScrollToPoints(highlightStart, highlightEnd);
    }

    _searchHighlightFocused = std::move(highlight);
}

std::vector<til::inclusive_rect> Terminal::GetSearchHighlightFocused() const noexcept
{
    _assertLocked();
    return _searchHighlightFocused;
}

const std::wstring_view Terminal::GetConsoleTitle() const noexcept
{
    _assertLocked();
    if (_title.has_value())
    {
        return *_title;
    }
    return _startingTitle;
}

// Method Description:
// - Lock the terminal for reading the contents of the buffer. Ensures that the
//      contents of the terminal won't be changed in the middle of a paint
//      operation.
//   Callers should make sure to also call Terminal::UnlockConsole once
//      they're done with any querying they need to do.
void Terminal::LockConsole() noexcept
{
    _readWriteLock.lock();
}

// Method Description:
// - Unlocks the terminal after a call to Terminal::LockConsole.
void Terminal::UnlockConsole() noexcept
{
    _readWriteLock.unlock();
}

const bool Terminal::IsUiaDataInitialized() const noexcept
{
    // GH#11135: Windows Terminal needs to create and return an automation peer
    // when a screen reader requests it. However, the terminal might not be fully
    // initialized yet. So we use this to check if any crucial components of
    // UiaData are not yet initialized.
    _assertLocked();
    return !!_mainBuffer;
}
