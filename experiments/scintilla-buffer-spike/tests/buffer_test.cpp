#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ScintillaTypes.h"
#include "Debugging.h"
#include "Position.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "SparseVector.h"
#include "ChangeHistory.h"
#include "CellBuffer.h"
#include "UndoHistory.h"

// The standalone core normally gets this diagnostic sink from the GUI port.
namespace Scintilla::Internal::Platform {
void DebugPrintf(const char *format, ...) noexcept
{
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
}
}

namespace {
using Scintilla::Internal::CellBuffer;

std::string snapshot(CellBuffer &buffer)
{
    std::string result(static_cast<size_t>(buffer.Length()), '\0');
    if (!result.empty())
        buffer.GetCharRange(result.data(), 0, buffer.Length());
    return result;
}

void insert(CellBuffer &buffer, const std::string &text)
{
    bool sequence = false;
    buffer.InsertString(buffer.Length(), text.data(), text.size(), sequence);
}

bool undo(CellBuffer &buffer)
{
    if (!buffer.CanUndo())
        return false;
    const int steps = buffer.StartUndo();
    for (int i = 0; i < steps; ++i)
        buffer.PerformUndoStep();
    return steps > 0;
}

bool redo(CellBuffer &buffer)
{
    if (!buffer.CanRedo())
        return false;
    const int steps = buffer.StartRedo();
    for (int i = 0; i < steps; ++i)
        buffer.PerformRedoStep();
    return steps > 0;
}

bool run()
{
    // Include Chinese, emoji, U+2028/U+2029, NBSP, NUL, LF and CRLF.
    const char bytes[] = u8"中文\n段\u2029末\u2028不换行\u00a0🙂\0尾\r\n";
    const std::string original(bytes, sizeof(bytes) - 1);
    const std::string inserted = u8"新\u2029\n";

    CellBuffer buffer(true, false);
    buffer.SetUTF8Substance(true);
    insert(buffer, original);
    if (snapshot(buffer) != original) {
        std::cerr << "initial byte round trip failed\n";
        return false;
    }
    buffer.DeleteUndoHistory();
    insert(buffer, inserted);
    if (snapshot(buffer) != original + inserted || !undo(buffer)
        || snapshot(buffer) != original || !redo(buffer)
        || snapshot(buffer) != original + inserted) {
        std::cerr << "insertion undo/redo did not preserve bytes\n";
        return false;
    }

    buffer.DeleteUndoHistory();
    const auto separator = original.find(u8"\u2029");
    if (separator == std::string::npos)
        return false;
    bool sequence = false;
    buffer.DeleteChars(static_cast<Sci::Position>(separator), 3, sequence);
    std::string removed = original + inserted;
    removed.erase(separator, 3);
    if (snapshot(buffer) != removed || !undo(buffer)
        || snapshot(buffer) != original + inserted || !redo(buffer)
        || snapshot(buffer) != removed) {
        std::cerr << "paragraph separator deletion undo/redo failed\n";
        return false;
    }

    CellBuffer newline(true, false);
    CellBuffer paragraph(true, false);
    newline.SetUTF8Substance(true);
    paragraph.SetUTF8Substance(true);
    insert(newline, "a\nb");
    insert(paragraph, u8"a\u2029b");
    if (snapshot(newline) != "a\nb" || snapshot(paragraph) != u8"a\u2029b"
        || snapshot(newline) == snapshot(paragraph)) {
        std::cerr << "newline and literal paragraph separator were not distinct\n";
        return false;
    }
    return true;
}
} // namespace

int main()
{
    if (!run())
        return 1;
    std::cout << "Scintilla core byte fidelity and undo/redo passed\n";
    return 0;
}
